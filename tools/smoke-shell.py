"""smoke-shell.py — 桌面外壳的端到端探针（Windows）

拖放本身没法自动触发（`WM_DROPFILES` 的 `HDROP` 由系统外壳分配，跨进程造不出来），
但外壳的另外三条入口**都跟拖放共用同一个 `SendPaths` / 同一套状态回报**，
而且都能从外部观测 —— 不需要人眼看窗口：

  ① 无参数启动 → 菜单栏是中文三项，且「导出」两项置灰
     （置灰说明界面把「当前没有文档」报给了外壳）
  ② 启动参数 → 打开文件 → 标题里出现这份抓包的名字，导出项转为可点
  ③ 第二个实例 → 路径转交给已运行的窗口 → 标题换成第二份、进程只剩一个

标题是界面主动报给外壳的（`shellState.title`），外壳拿它 `SetWindowTextW`；
菜单项的灰/亮同理。所以「标题对了、菜单灰对了」= 整条链路都通：
命令行 → 外壳 → 通道 → Dart → 核心解码 → 回报状态 → Win32 生效。

用法：
    python tools/smoke-shell.py [样本A] [样本B]

不给参数时自动在仓库根目录里挑样本（一份 .atkcc、一份 .sqlite）。
全部检查通过退出码 0，否则非 0。
"""

import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)                       # PDScope-NG/
REPO = os.path.dirname(ROOT)                       # ATKDecom/
EXE = os.path.join(ROOT, "app", "build", "windows", "x64", "runner", "Release",
                   "PDScope.exe")

TITLE_PREFIX = "PDScope"

# Win32 里的几个常量（本地重写，免得依赖 pywin32）。
MF_BYPOSITION = 0x00000400
MF_BYCOMMAND = 0x00000000
MF_GRAYED = 0x00000001
MF_CHECKED = 0x00000008

user32 = ctypes.WinDLL("user32", use_last_error=True)
_EnumWindowsProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

user32.GetMenu.restype = wt.HMENU
user32.GetMenu.argtypes = [wt.HWND]
user32.GetSubMenu.restype = wt.HMENU
user32.GetSubMenu.argtypes = [wt.HMENU, ctypes.c_int]
user32.GetMenuItemCount.restype = ctypes.c_int
user32.GetMenuItemCount.argtypes = [wt.HMENU]
user32.GetMenuStringW.restype = ctypes.c_int
user32.GetMenuStringW.argtypes = [wt.HMENU, wt.UINT, wt.LPWSTR, ctypes.c_int,
                                  wt.UINT]
user32.GetMenuState.restype = wt.UINT
user32.GetMenuState.argtypes = [wt.HMENU, wt.UINT, wt.UINT]
user32.GetWindowTextLengthW.restype = ctypes.c_int
user32.GetWindowTextLengthW.argtypes = [wt.HWND]


def visible_windows(prefix):
    """所有可见的、标题以 prefix 开头的顶层窗口 → [(hwnd, title)]"""
    found = []

    def callback(hwnd, _lparam):
        if not user32.IsWindowVisible(hwnd):
            return True
        length = user32.GetWindowTextLengthW(hwnd)
        if length == 0:
            return True
        buf = ctypes.create_unicode_buffer(length + 1)
        user32.GetWindowTextW(hwnd, buf, length + 1)
        if buf.value == prefix or buf.value.startswith(prefix + " — "):
            found.append((hwnd, buf.value))
        return True

    user32.EnumWindows(_EnumWindowsProc(callback), 0)
    return found


def wait_for(predicate, timeout, description):
    """轮询到 predicate 为真；超时返回 None。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.25)
    return None


def menu_labels(hmenu):
    """某一层菜单的各项文字（去掉 `&` 助记符）→ [str]"""
    if not hmenu:
        return []
    out = []
    for pos in range(user32.GetMenuItemCount(hmenu)):
        buf = ctypes.create_unicode_buffer(256)
        n = user32.GetMenuStringW(hmenu, pos, buf, 256, MF_BYPOSITION)
        out.append(buf.value.replace("&", "") if n > 0 else "")
    return out


def menu_bar(hwnd):
    return user32.GetMenu(hwnd)


def exported_items(hmenu):
    """文件菜单里那两个「导出」项 → [(position, label)]"""
    file_menu = user32.GetSubMenu(hmenu, 0)
    return [(i, label) for i, label in enumerate(menu_labels(file_menu))
            if "导出" in label]


def any_grayed(hwnd):
    """文件菜单里的导出项是不是全灰着（= 外壳认为当前没有文档）。"""
    hmenu = menu_bar(hwnd)
    if not hmenu:
        return None
    file_menu = user32.GetSubMenu(hmenu, 0)
    items = exported_items(hmenu)
    if not items:
        return None
    states = [user32.GetMenuState(file_menu, pos, MF_BYPOSITION) & MF_GRAYED
              for pos, _label in items]
    return all(states)


def is_checked(hwnd, menu_index, label_part):
    """视图菜单里某一项有没有打勾（= 界面上那一栏现在是展开的）。"""
    hmenu = menu_bar(hwnd)
    if not hmenu:
        return None
    sub = user32.GetSubMenu(hmenu, menu_index)
    if not sub:
        return None
    for pos, label in enumerate(menu_labels(sub)):
        if label_part in label:
            return bool(user32.GetMenuState(sub, pos, MF_BYPOSITION)
                        & MF_CHECKED)
    return None


def pick_sample(patterns):
    for pattern in patterns:
        for name in sorted(os.listdir(REPO)):
            if all(p in name for p in pattern):
                return os.path.join(REPO, name)
    return None


def spawn(*args):
    return subprocess.Popen([EXE, *args], cwd=os.path.dirname(EXE),
                            close_fds=True)


class Report:
    def __init__(self):
        self.ok = True

    def check(self, condition, good, bad):
        if condition:
            print(f"  ✓ {good}")
        else:
            print(f"  ✗ {bad}")
            self.ok = False
        return bool(condition)


def main():
    if not os.path.isfile(EXE):
        print(f"找不到 {EXE}\n先跑 flutter build windows --release。")
        return 2

    argv = [a for a in sys.argv[1:] if not a.startswith("-")]
    if len(argv) >= 2:
        sample_a, sample_b = argv[0], argv[1]
    else:
        sample_a = pick_sample([(".atkcc",)])
        sample_b = pick_sample([(".sqlite", "ufcs")]) or pick_sample([(".sqlite",)])
    for path in (sample_a, sample_b):
        if not path or not os.path.isfile(path):
            print(f"样本缺失：{path}")
            return 2
    # The app is launched with its Release directory as cwd. Forward absolute
    # paths so an invocation from the repository root still opens the sample.
    sample_a, sample_b = os.path.abspath(sample_a), os.path.abspath(sample_b)

    name_a = os.path.basename(sample_a)
    name_b = os.path.basename(sample_b)
    print(f"程序   {EXE}")
    print(f"样本 A {name_a}")
    print(f"样本 B {name_b}\n")

    report = Report()
    procs = []
    try:
        # ── ① 无参数启动：中文菜单 + 没有文档时导出项置灰 ──────────────
        print("① 无参数启动 → 菜单栏与「无文档」状态")
        procs.append(spawn())
        found = wait_for(lambda: visible_windows(TITLE_PREFIX), 60, "窗口出现")
        if not report.check(bool(found), "窗口已出现", "60 秒内没有出现 PDScope 窗口"):
            raise SystemExit(1)
        hwnd = found[0][0]

        labels = menu_labels(menu_bar(hwnd))
        report.check(
            labels == ["文件(F)", "视图(V)", "帮助(H)"],
            f"菜单栏 = {labels}",
            f"菜单栏不对：{labels}（期望 文件/视图/帮助 三项）")

        # 界面把 hasDoc=false 报过来之后，导出项才是灰的 —— 所以要等一会儿。
        grayed = wait_for(lambda: any_grayed(hwnd) is True, 30, "导出项置灰")
        report.check(
            grayed is not None,
            "没有文档时导出项已置灰（说明界面把状态报给了外壳）",
            "30 秒内导出项没有置灰 —— 界面可能没把「没有文档」报给外壳")

        # 勾选状态同理：默认两栏都展开，视图菜单里两项都该打勾。
        report.check(
            is_checked(hwnd, 1, "筛选栏") is True,
            "视图 → 显示筛选栏 打勾（默认展开）",
            f"「显示筛选栏」的勾不对：{is_checked(hwnd, 1, '筛选栏')}")
        report.check(
            is_checked(hwnd, 1, "详情面板") is True,
            "视图 → 显示详情面板 打勾（默认展开）",
            f"「显示详情面板」的勾不对：{is_checked(hwnd, 1, '详情面板')}")

        # ── ② 启动参数 ────────────────────────────────────────────────
        print("\n② 启动参数 → 打开这份抓包")
        procs.append(spawn(sample_a))
        want_a = f"{TITLE_PREFIX} — {name_a}"
        title = wait_for(
            lambda: next((t for h, t in visible_windows(TITLE_PREFIX)
                          if t == want_a and h == hwnd), None),
            120, "标题出现样本名")
        report.check(bool(title), f"标题 = {title}",
                     f"标题没有变成「{want_a}」，实际是 "
                     f"{[t for _h, t in visible_windows(TITLE_PREFIX)]}")

        # 上一条真的验过「灰」之后，再验「由灰变亮」。上一段没通过时不重复报一条
        # 会自我印证的 ✓（导出项本来就亮着的话，"不是灰的" 恒真，什么都说明不了）。
        if grayed is not None:
            enabled = wait_for(lambda: any_grayed(hwnd) is False, 30, "导出项可用")
            report.check(enabled is not None, "有文档后导出项恢复可点",
                         "有文档了导出项还是灰的")

        # ── ③ 第二个实例转交 ──────────────────────────────────────────
        print("\n③ 第二个实例 → 路径转交，自己退出")
        second = spawn(sample_b)
        procs.append(second)
        code = second.wait(timeout=30)
        report.check(second.poll() is not None,
                     f"第二个实例已退出（退出码 {code}）",
                     "第二个实例没有自己退出（转交没成功？）")

        want_b = f"{TITLE_PREFIX} — {name_b}"
        title = wait_for(
            lambda: next((t for h, t in visible_windows(TITLE_PREFIX)
                          if t == want_b and h == hwnd), None),
            120, "标题换成第二份")
        report.check(bool(title), f"标题 = {title}",
                     f"标题没有换成「{want_b}」，实际是 "
                     f"{[t for _h, t in visible_windows(TITLE_PREFIX)]}")

        windows = visible_windows(TITLE_PREFIX)
        report.check(len(windows) == 1, "全程只有一个窗口（第二份是在同一窗口里开的）",
                     f"窗口数应当是 1，实际 {len(windows)}："
                     f"{[t for _h, t in windows]}")

        # 转交回来的路径也该真的打开了 —— 用「选项卡数」看不出来，
        # 但文件名变了对上一条就够了。
        print("\n  · 全表：")
        for item in menu_labels(menu_bar(hwnd)):
            print(f"      {item}")

    finally:
        for proc in reversed(procs):
            if proc.poll() is None:
                proc.terminate()
        time.sleep(1.0)

    print("\n" + ("全部通过" if report.ok else "有失败项"))
    return 0 if report.ok else 1


if __name__ == "__main__":
    sys.exit(main())
