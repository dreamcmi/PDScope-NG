#!/usr/bin/env bash
# msvc-env.sh — 在 **Git Bash** 里手工搭出 MSVC 的编译环境。
#
# 为什么不用 `VsDevCmd.bat` / `vcvars64.bat`：它们只能在 cmd.exe 里跑，
# 而这个开发环境把 cmd.exe 与 PowerShell 的 Start-Job 都拦了（安全策略）。
# 好在这几个环境变量本来就是那几个固定目录，直接摆出来即可。
#
# 用法（必须先 `source`，子 shell 里改了环境没用）：
#     source tools/msvc-env.sh
#     cmake -S . -B build -G Ninja
#     cmake --build build
#
# 版本目录是**探测**出来的而不是写死的 —— MSVC 与 Windows SDK 的补丁号一直在变，
# 写死的话下次更新工具链就报「找不到编译器」，而报错现场看着像代码有问题。

_pdscope_win() {  # POSIX 路径 → Windows 路径（cl.exe 只认 Windows 路径）
    local p="$1"
    if command -v cygpath >/dev/null 2>&1; then cygpath -w "$p"; else printf '%s' "$p"; fi
}

_pdscope_need() {  # 目录必须存在，否则给出人说人话的报错
    if [ ! -d "$1" ]; then
        echo "[msvc-env] 找不到 $2：$1" >&2
        echo "[msvc-env] 请确认已安装 Visual Studio 2022 的「使用 C++ 的桌面开发」工作负载" >&2
        return 1
    fi
}

# ── 探测 VS 安装位置 ─────────────────────────────────────────────────
if [ -n "$PDSCOPE_VS_ROOT" ]; then
    _vs="$PDSCOPE_VS_ROOT"
else
    _vswhere="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
    if [ -x "$_vswhere" ]; then
        _vs="$("$_vswhere" -latest -products '*' \
               -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 \
               -property installationPath 2>/dev/null | tr -d '\r')"
    fi
    [ -z "$_vs" ] && _vs="C:/Program Files/Microsoft Visual Studio/2022/Community"
fi
_pdscope_need "$_vs" "Visual Studio 安装目录" || return 1

# ── MSVC 工具集（取版本号最大的那份）────────────────────────────────
_msvc_parent="$_vs/VC/Tools/MSVC"
_pdscope_need "$_msvc_parent" "MSVC 工具集目录" || return 1
_msvc_ver="$(ls "$_msvc_parent" 2>/dev/null | sort -V | tail -1)"
_pdscope_need "$_msvc_parent/$_msvc_ver" "MSVC 工具集（没找到任何版本）" || return 1
_msvc="$_msvc_parent/$_msvc_ver"

# ── Windows SDK（同样取最大的那份）───────────────────────────────────
_sdk_root="/c/Program Files (x86)/Windows Kits/10"
_pdscope_need "$_sdk_root/Include" "Windows SDK（Include 目录）" || return 1
_sdk_ver="$(ls "$_sdk_root/Include" 2>/dev/null | sort -V | tail -1)"
_pdscope_need "$_sdk_root/Include/$_sdk_ver" "Windows SDK（没找到任何版本）" || return 1
_sdk="$_sdk_root/Include/$_sdk_ver"
_sdk_lib="$_sdk_root/Lib/$_sdk_ver"

# ── 拼环境变量 ───────────────────────────────────────────────────────
# MSYS 会把「看起来像 POSIX 路径」的环境变量自动转换掉，把 INCLUDE/LIB 转坏。
# 这里明确排除，让 cl.exe 拿到原样的盘符路径。
export MSYS2_ENV_CONV_EXCL="INCLUDE;LIB;LIBPATH;PDSCOPE_*"
export MSYS2_ARG_CONV_EXCL="*"

_w() { _pdscope_win "$1"; }

export INCLUDE="$(_w "$_msvc/include");$(_w "$_sdk/ucrt");$(_w "$_sdk/um");$(_w "$_sdk/shared");$(_w "$_sdk/winrt");$(_w "$_sdk/cppwinrt")"
export LIB="$(_w "$_msvc/lib/x64");$(_w "$_sdk_lib/ucrt/x64");$(_w "$_sdk_lib/um/x64")"
# cl.exe / link.exe 必须在 PATH 上：CMake 用 `cl` 这个名字去找编译器（只给 INCLUDE/LIB 不够）。
export PATH="$_msvc/bin/Hostx64/x64:$PATH"
# SDK 的 bin 里是 rc.exe（资源编译器）与 mt.exe（清单工具）。少了它们 CMake 的
# 编译器自检会在**链接那一步**失败，报出来的错看着像「找不到编译器」，其实编译器早就过了。
export PATH="$_sdk_root/bin/$_sdk_ver/x64:$PATH"
# Ninja：优先用 VS 自带的那份（Python 的 pip 版不一定装了）
export PATH="$PATH:/c/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja"

echo "[msvc-env] MSVC $_msvc_ver  ·  Windows SDK $_sdk_ver"
"$_msvc/bin/Hostx64/x64/cl.exe" 2>&1 | head -1
