"""capture-window.py — 按标题抓某个窗口的截图（Windows，纯 ctypes + 标准库）

为什么自己写：`PrintWindow` 抓不到 Flutter 的 GPU 合成内容时会得到一张全黑图，
所以要「先试 PrintWindow，太黑就退回抓屏幕」。PowerShell 那条路在本机工具链里
起不了 GUI 进程，所以用 Python。

用法：
    python tools/capture-window.py 输出.png [标题前缀] [--wait 秒]

标题前缀默认 `PDScope`。截图前会先把目标窗口置前。
找不到窗口时退出码 2；抓到全黑图会明确报出来而不是闷声存一张黑图。
"""

import ctypes
import ctypes.wintypes as wt
import struct
import sys
import time
import zlib

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)

user32.SetProcessDPIAware()          # 不去虚拟化的话坐标会被缩放，抓出来是裁过的

# ⚠ 这些 argtypes/restype 必须显式声明：默认按 C int 处理，而 HDC/HBITMAP 是 64 位句柄，
#   一旦地址高位非零就会 `OverflowError: int too long to convert`（表现为「偶尔抓不了」）。
gdi32.CreateCompatibleDC.restype = wt.HDC
gdi32.CreateCompatibleDC.argtypes = [wt.HDC]
gdi32.CreateCompatibleBitmap.restype = wt.HBITMAP
gdi32.CreateCompatibleBitmap.argtypes = [wt.HDC, ctypes.c_int, ctypes.c_int]
gdi32.SelectObject.restype = wt.HGDIOBJ
gdi32.SelectObject.argtypes = [wt.HDC, wt.HGDIOBJ]
gdi32.BitBlt.restype = wt.BOOL
gdi32.BitBlt.argtypes = [wt.HDC, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                         ctypes.c_int, wt.HDC, ctypes.c_int, ctypes.c_int,
                         wt.DWORD]
gdi32.GetDIBits.restype = ctypes.c_int
gdi32.GetDIBits.argtypes = [wt.HDC, wt.HBITMAP, wt.UINT, wt.UINT,
                            ctypes.c_void_p, ctypes.c_void_p, wt.UINT]
gdi32.DeleteObject.argtypes = [wt.HGDIOBJ]
gdi32.DeleteDC.argtypes = [wt.HDC]

_EnumWindowsProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
user32.GetWindowTextLengthW.argtypes = [wt.HWND]
user32.GetWindowTextLengthW.restype = ctypes.c_int
user32.GetDC.restype = wt.HDC
user32.GetWindowDC.restype = wt.HDC
user32.GetWindowDC.argtypes = [wt.HWND]
user32.ReleaseDC.argtypes = [wt.HWND, wt.HDC]
user32.PrintWindow.argtypes = [wt.HWND, wt.HDC, wt.UINT]
user32.GetWindowRect.argtypes = [wt.HWND, ctypes.c_void_p]
user32.IsWindowVisible.argtypes = [wt.HWND]
user32.SetForegroundWindow.argtypes = [wt.HWND]

PW_RENDERFULLCONTENT = 0x00000002
BI_RGB = 0
DIB_RGB_COLORS = 0
SRCCOPY = 0x00CC0020


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG),
                ("biPlanes", wt.WORD), ("biBitCount", wt.WORD),
                ("biCompression", wt.DWORD), ("biSizeImage", wt.DWORD),
                ("biXPelsPerMeter", wt.LONG), ("biYPelsPerMeter", wt.LONG),
                ("biClrUsed", wt.DWORD), ("biClrImportant", wt.DWORD)]


def find_window(prefix):
    """标题以 prefix 开头的、可见的顶层窗口 → (hwnd, title) 或 None"""
    hit = []

    def cb(hwnd, _l):
        if not user32.IsWindowVisible(hwnd):
            return True
        n = user32.GetWindowTextLengthW(hwnd)
        if n == 0:
            return True
        buf = ctypes.create_unicode_buffer(n + 1)
        user32.GetWindowTextW(hwnd, buf, n + 1)
        if buf.value.startswith(prefix):
            hit.append((hwnd, buf.value))
        return True

    user32.EnumWindows(_EnumWindowsProc(cb), 0)
    return hit[0] if hit else None


def grab(hwnd, use_screen):
    """抓窗口像素 → (w, h, bytes(BGRA, 自上而下))"""
    rect = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rect))
    w, h = rect.right - rect.left, rect.bottom - rect.top
    if w <= 0 or h <= 0:
        return None

    src = user32.GetDC(None) if use_screen else user32.GetWindowDC(hwnd)
    if not src:
        return None
    dc = gdi32.CreateCompatibleDC(src)
    bmp = gdi32.CreateCompatibleBitmap(src, w, h)
    gdi32.SelectObject(dc, bmp)

    if use_screen:
        gdi32.BitBlt(dc, 0, 0, w, h, src, rect.left, rect.top, SRCCOPY)
    else:
        user32.PrintWindow(hwnd, dc, PW_RENDERFULLCONTENT)

    info = BITMAPINFOHEADER()
    info.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    info.biWidth = w
    info.biHeight = -h            # 负高度 = 自上而下，省得再翻转
    info.biPlanes = 1
    info.biBitCount = 32
    info.biCompression = BI_RGB

    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(dc, bmp, 0, h, ctypes.cast(buf, ctypes.c_void_p),
                    ctypes.cast(ctypes.byref(info), ctypes.c_void_p),
                    DIB_RGB_COLORS)

    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(dc)
    user32.ReleaseDC(None if use_screen else hwnd, src)
    return w, h, buf.raw


def looks_blank(w, h, bgra):
    """抽样看是不是一整片同色（PrintWindow 抓不到 GPU 内容时就是全黑）。"""
    step = max(1, (w * h) // 4000)
    first = bgra[:3]
    same = 0
    sampled = 0
    for i in range(0, w * h, step):
        off = i * 4
        sampled += 1
        if bgra[off:off + 3] == first:
            same += 1
    return sampled > 0 and same / sampled > 0.995


def write_png(path, w, h, bgrs):
    rows = []
    for y in range(h):
        line = bytearray(w * 3)
        base = y * w * 4
        for x in range(w):
            off = base + x * 4
            line[x * 3] = bgrs[off + 2]
            line[x * 3 + 1] = bgrs[off + 1]
            line[x * 3 + 2] = bgrs[off]
        rows.append(b"\x00" + bytes(line))
    raw = b"".join(rows)

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    wait = 10.0
    for a in sys.argv[1:]:
        if a.startswith("--wait"):
            wait = float(a.split("=", 1)[1]) if "=" in a else wait

    if not args:
        print(__doc__)
        return 2
    out = args[0]
    prefix = args[1] if len(args) > 1 else "PDScope"

    deadline = time.time() + wait
    target = None
    while time.time() < deadline:
        target = find_window(prefix)
        if target:
            break
        time.sleep(0.3)
    if not target:
        print(f"找不到标题以「{prefix}」开头的窗口")
        return 2

    hwnd, title = target
    user32.SetForegroundWindow(hwnd)
    time.sleep(1.0)

    shot = grab(hwnd, use_screen=False)
    if shot and looks_blank(*shot):
        print("  · PrintWindow 拿到的是同色图（GPU 合成抓不到），改用抓屏幕")
        shot = grab(hwnd, use_screen=True)
    if not shot:
        print("抓取失败")
        return 1

    w, h, bgra = shot
    if looks_blank(w, h, bgra):
        print("  · ⚠ 结果仍是一整片同色 —— 桌面可能没有真实显示输出，图不可用")
    write_png(out, w, h, bgra)
    print(f"  ✓ {out}  {w}×{h}  标题={title!r}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
