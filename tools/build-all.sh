#!/usr/bin/env bash
# build-all.sh — 一键构建 PDScope-NG：C++ 核心 + Flutter 界面 → dist/ 里可直接双击运行的目录。
#
# 三端通用（Linux / macOS / Windows 上的 Git Bash）。Windows 上要进 Git Bash，
# 因为核心那一步需要 MSVC 的 INCLUDE/LIB，而那套环境是 tools/msvc-env.sh 搭的。
# 不想开 Git Bash 的 Windows 用户用同一目录下的 build-all.ps1，两者做的是一回事。
#
# 默认依次做五步，任何一步失败就停下并保留现场：
#     1. 配置并构建 C++ 核心（CMake + Ninja）
#     2. 跑核心单测（pdscope-tests）——「跳过」不计为通过，有跳过会单独报出来
#     3. Flutter：pub get / analyze / test
#     4. 构建 Flutter 发行版
#     5. 归置到 dist/PDScope-<平台>-<架构>/
#
# 用法：
#     tools/build-all.sh                  # 全流程
#     tools/build-all.sh --no-tests       # 只编译，不跑测试
#     tools/build-all.sh --core-only      # 只做 C++ 核心
#     tools/build-all.sh --app-only       # 只做 Flutter（核心需已构建）
#     tools/build-all.sh --clean          # 先删构建目录再重来
#     tools/build-all.sh --flutter ~/flutter --jobs 4 --out dist

set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP="$REPO/app"
ORIG_PWD="$PWD"          # 解析 --out 的相对路径要用它，见后面归置那一段

# ── 参数默认值 ───────────────────────────────────────────────────────
NO_TESTS=0
CORE_ONLY=0
APP_ONLY=0
DEBUG=0
JOBS=4
CLEAN=0
FLUTTER_ARG=""
OUT_ARG=""

usage() {
    # 只打印开头那段注释：跳过 shebang，遇到第一行非注释就停。
    # 不用固定行号 —— 注释增删一行就会漏出代码（比如 set -uo pipefail）。
    awk 'NR==1{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "${BASH_SOURCE[0]}"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --no-tests)  NO_TESTS=1 ;;
        --core-only) CORE_ONLY=1 ;;
        --app-only)  APP_ONLY=1 ;;
        --debug)     DEBUG=1 ;;
        --clean)     CLEAN=1 ;;
        --jobs)      JOBS="${2:-}"; shift ;;
        --flutter)   FLUTTER_ARG="${2:-}"; shift ;;
        --out)       OUT_ARG="${2:-}"; shift ;;
        -h|--help)   usage; exit 0 ;;
        *) echo "未知参数：$1（--help 看用法）" >&2; exit 2 ;;
    esac
    shift
done

if [ "$CORE_ONLY" = 1 ] && [ "$APP_ONLY" = 1 ]; then
    echo "--core-only 与 --app-only 不能同时用" >&2
    exit 2
fi

# ── 输出小工具 ───────────────────────────────────────────────────────
die() {
    echo "" >&2
    echo "!! 失败：$1" >&2
    shift
    for line in "$@"; do echo "   $line" >&2; done
    exit 1
}

STEP_N=0
STEP_TOTAL=0

step() {
    STEP_N=$((STEP_N + 1))
    echo ""
    echo "========================================================================"
    echo "  [$STEP_N/$STEP_TOTAL] $1"
    echo "========================================================================"
}

run() {
    echo "\$ $*"
    "$@"
    local rc=$?
    if [ $rc -ne 0 ]; then die "命令返回 $rc" "  $*"; fi
    return 0
}

# ── 认平台 ───────────────────────────────────────────────────────────
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) OS=windows ;;
    Darwin)               OS=macos ;;
    Linux)                OS=linux ;;
    *)                    OS=unknown ;;
esac
[ "$OS" = unknown ] && die "认不出这个平台：$(uname -s)"

case "$(uname -m)" in
    x86_64|amd64)  ARCH=x64 ;;
    arm64|aarch64) ARCH=arm64 ;;
    *)             ARCH="$(uname -m)" ;;
esac

# 传给原生程序（cmake / cl.exe）的路径：Windows 上必须是盘符形式。
# ⚠ msvc-env.sh 设了 MSYS2_ARG_CONV_EXCL='*'（防止 MSYS 转坏 INCLUDE/LIB），
#   于是 Git Bash **不会**再自动把 /d/Code 转成 D:\Code —— 得自己给对。
if [ "$OS" = windows ]; then
    REPO_ARG="$(cd "$REPO" && pwd -W 2>/dev/null || echo "$REPO")"
else
    REPO_ARG="$REPO"
fi

echo "PDScope-NG 一键构建"
echo "  仓库     : $REPO"
echo "  平台     : $OS / $ARCH"
echo "  配置     : $([ "$DEBUG" = 1 ] && echo Debug || echo Release) · 并行 $JOBS"
echo "  测试     : $([ "$NO_TESTS" = 1 ] && echo 跳过 || echo 跑)"

# ── 工具链 ───────────────────────────────────────────────────────────
if [ "$OS" = windows ]; then
    # 这个脚本开着 `set -u`，而 msvc-env.sh 里还有些未赋值的变量引用；
    # 先关掉 nounset 再 source，免得它的内部细节把整个构建打断。
    # shellcheck source=/dev/null
    set +u
    source "$REPO/tools/msvc-env.sh"
    _msvc_rc=$?
    set -u
    [ $_msvc_rc -eq 0 ] || die "搭 MSVC 环境失败" \
        "tools/msvc-env.sh 需要 Visual Studio 的「使用 C++ 的桌面开发」工作负载"
fi

# Visual Studio 根目录（只有 Windows 有）。
# msvc-env.sh 内部已经找过一次，但它没导出；而 VS 不一定装在 C 盘
# （这台机器就在 D:\IDE\VisualStudio），所以这里再问一次 vswhere，不写死路径。
vs_root() {
    local vswhere="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
    [ -x "$vswhere" ] || return 1
    local p
    p="$("$vswhere" -latest -products '*' \
         -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 \
         -property installationPath 2>/dev/null | tr -d '\r')"
    [ -n "$p" ] || return 1
    if command -v cygpath >/dev/null 2>&1; then cygpath -u "$p"; else echo "$p"; fi
}

find_cmake() {
    if command -v cmake >/dev/null 2>&1; then command -v cmake; return 0; fi
    local c vr
    if [ "$OS" = windows ]; then
        vr="$(vs_root || true)"
        if [ -n "$vr" ]; then
            c="$vr/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
            [ -x "$c" ] && { echo "$c"; return 0; }
        fi
        for c in "/c/Program Files/CMake/bin/cmake.exe" \
                 "/c/Program Files (x86)/CMake/bin/cmake.exe"; do
            [ -x "$c" ] && { echo "$c"; return 0; }
        done
    else
        for c in /opt/homebrew/bin/cmake /usr/local/bin/cmake /usr/bin/cmake; do
            [ -x "$c" ] && { echo "$c"; return 0; }
        done
    fi
    return 1
}

CMAKE="$(find_cmake)" || die "找不到 cmake" \
    "装 CMake 并加进 PATH，或让 Visual Studio 自带的 CMake 可用"
echo "  cmake    : $CMAKE"

# ninja：msvc-env.sh 会把 VS 自带的那份挂上 PATH；这里兜一层，并用它给 cmake 传
# -DCMAKE_MAKE_PROGRAM，免得 cmake 自己找不到时给一句含糊的报错。
NINJA="$(command -v ninja 2>/dev/null || true)"
if [ -z "$NINJA" ] && [ "$OS" = windows ]; then
    _vr="$(vs_root || true)"
    if [ -n "$_vr" ] && [ -x "$_vr/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe" ]; then
        export PATH="$PATH:$_vr/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja"
        NINJA="$(command -v ninja 2>/dev/null || true)"
    fi
fi
[ -n "$NINJA" ] || die "找不到 ninja" "装 Ninja（Windows 上 Visual Studio 自带一份）"
echo "  ninja    : $NINJA"

# ── 构建目录：与仓库既有约定一致 ─────────────────────────────────────
# Windows 用 build（app/windows/runner 的 CMakeLists 写死了从 build/out 拷核心库），
# Unix 用 build/native（README 与 CI 都这么写）。
if [ "$OS" = windows ]; then
    BUILD_DIR="$REPO/build"
    BUILD_ARG="$REPO_ARG/build"
else
    BUILD_DIR="$REPO/build/native"
    BUILD_ARG="$REPO_ARG/build/native"
fi

# ── 数一下这轮要跑几步（别用「大概」，进度行写成 [4/3] 会让人以为漏了一步）──
if [ "$APP_ONLY" = 0 ]; then
    STEP_TOTAL=$((STEP_TOTAL + 1))
    [ "$NO_TESTS" = 0 ] && STEP_TOTAL=$((STEP_TOTAL + 1))
fi
[ "$CORE_ONLY" = 0 ] && STEP_TOTAL=$((STEP_TOTAL + 3))

if [ "$CLEAN" = 1 ]; then
    for d in "$BUILD_DIR" "$APP/build"; do
        [ -d "$d" ] && { echo "  清理 ${d#$REPO/}"; rm -rf "$d"; }
    done
fi

# ── 1. 构建核心 ──────────────────────────────────────────────────────
if [ "$APP_ONLY" = 0 ]; then
    step "配置并构建 C++ 核心 → ${BUILD_DIR#$REPO/}"
    CFG_TYPE=Release
    [ "$DEBUG" = 1 ] && CFG_TYPE=Debug
    run "$CMAKE" -S "$REPO_ARG" -B "$BUILD_ARG" -G Ninja -DCMAKE_BUILD_TYPE="$CFG_TYPE"
    run "$CMAKE" --build "$BUILD_ARG" --parallel "$JOBS"
    echo "   核心产物：$BUILD_DIR/out"

    if [ "$NO_TESTS" = 0 ]; then
        step "跑 C++ 核心单测"
        TESTS="$BUILD_DIR/out/pdscope-tests"
        [ "$OS" = windows ] && TESTS="$TESTS.exe"
        [ -x "$TESTS" ] || die "找不到测试程序：$TESTS"
        echo "\$ $TESTS"
        # 运行器的退出码 = 失败用例数，所以非零就是真失败
        "$TESTS"
        rc=$?
        [ $rc -ne 0 ] && die "核心单测有 $rc 个用例失败"
        echo "   核心单测通过（「跳过」不计为通过，若有用例跳过上面会单独报出来）"
    fi
fi

# ── Flutter ──────────────────────────────────────────────────────────
FLUTTER=""
if [ "$CORE_ONLY" = 0 ]; then
    # ⚠ 判据一律用 -f（存在），**不能**用 -x（可执行）：
    #   Windows 上装的是 flutter.bat，它在 Git Bash 眼里是 -rw-r--r--，
    #   -x 恒为假 —— 但那是 Windows 权限位的映射问题，执行起来完全正常
    #   （Git Bash 会把 .bat 转给 cmd）。用 -x 判会得出「明明装了却说找不到」。
    _flutter_ok() { [ -f "$1" ]; }

    find_flutter() {
        local name=flutter
        [ "$OS" = windows ] && name=flutter.bat

        if [ -n "$FLUTTER_ARG" ]; then
            if _flutter_ok "$FLUTTER_ARG"; then echo "$FLUTTER_ARG"; return 0; fi
            if _flutter_ok "$FLUTTER_ARG/bin/$name"; then echo "$FLUTTER_ARG/bin/$name"; return 0; fi
            return 1
        fi
        local var root
        for var in PDSCOPE_FLUTTER FLUTTER_ROOT; do
            root="${!var:-}"
            if [ -n "$root" ] && _flutter_ok "$root/bin/$name"; then
                echo "$root/bin/$name"; return 0
            fi
        done
        local found
        found="$(command -v flutter 2>/dev/null)"
        [ -n "$found" ] && { echo "$found"; return 0; }

        # 几个常见位置。刻意不做全盘递归 —— 那既慢又容易撞进别人的目录。
        local c
        for c in \
            /c/flutter/bin/$name /d/flutter/bin/$name /e/flutter/bin/$name \
            /c/src/flutter/bin/$name /d/src/flutter/bin/$name \
            /d/Language/flutter/bin/$name /d/Tools/flutter/bin/$name \
            /d/IDE/flutter/bin/$name /d/dev/flutter/bin/$name \
            "$HOME/flutter/bin/$name" "$HOME/development/flutter/bin/$name" \
            /opt/flutter/bin/$name /usr/local/flutter/bin/$name /usr/lib/flutter/bin/$name \
            /snap/flutter/common/flutter/bin/$name
        do
            _flutter_ok "$c" && { echo "$c"; return 0; }
        done
        return 1
    }

    FLUTTER="$(find_flutter)" || die "找不到 Flutter SDK" \
        "三种给法，任选一种：" \
        "  1) tools/build-all.sh --flutter <SDK 目录>" \
        "  2) 设环境变量 PDSCOPE_FLUTTER=<SDK 目录>（或 FLUTTER_ROOT）" \
        "  3) 把 <SDK>/bin 加进 PATH" \
        "SDK 目录指含 bin/flutter 的那一层，不是 bin 里面。"
    echo "  flutter  : $FLUTTER"

    # 界面通过 FFI 找核心库；显式给一下，别让它靠猜。
    export PDSCOPE_LIB_DIR="$BUILD_DIR/out"

    # ⚠ flutter 的每条子命令都必须在 app/ 里跑：它按当前目录找 pubspec.yaml，
    #   在仓库根跑会直接报 "Expected to find project root in current working directory"。
    cd "$APP" || die "进不去 app 目录：$APP"

    step "Flutter：pub get → analyze → test"
    run "$FLUTTER" pub get
    run "$FLUTTER" analyze
    [ "$NO_TESTS" = 0 ] && run "$FLUTTER" test --concurrency=1

    step "构建 Flutter 发行版（$OS）"
    if [ "$DEBUG" = 1 ]; then
        run "$FLUTTER" build "$OS" --debug
    else
        run "$FLUTTER" build "$OS" --release
    fi
fi

# ── 5. 归置 ──────────────────────────────────────────────────────────
if [ "$CORE_ONLY" = 0 ]; then
    step "归置到可直接双击运行的目录"
    CFG=Release
    [ "$DEBUG" = 1 ] && CFG=Debug

    case "$OS" in
        windows) SRC="$APP/build/windows/x64/runner/$CFG" ;;
        macos)   SRC="$APP/build/macos/Build/Products/$CFG" ;;
        linux)   SRC="" ;;
    esac
    if [ "$OS" = linux ]; then
        # 架构段不一定是 x64，所以这里扫一遍
        for d in "$APP"/build/linux/*/"$(echo "$CFG" | tr 'A-Z' 'a-z')"/bundle; do
            [ -d "$d" ] && { SRC="$d"; break; }
        done
    fi
    [ -n "$SRC" ] && [ -d "$SRC" ] || die "找不到 Flutter 产物目录：$SRC"

    DEST_BASE="${OUT_ARG:-$REPO/dist}"
    # --out 给相对路径时按「调用脚本时所在的目录」算，而不是 app/ ——
    # 脚本中途会 cd 到 app/ 去跑 flutter，那时相对路径的含义已经变了。
    case "$DEST_BASE" in
        /*|[A-Za-z]:/*|[A-Za-z]:\\*) ;;
        *) DEST_BASE="$ORIG_PWD/$DEST_BASE" ;;
    esac
    DEST="$DEST_BASE/PDScope-$OS-$ARCH"
    rm -rf "$DEST"
    mkdir -p "$DEST"

    if [ "$OS" = macos ]; then
        # .app 是自包含的一整包，双击它就行
        cp -R "$SRC/PDScope.app" "$DEST/PDScope.app"
    else
        # Windows / Linux：目录里的内容整个摊到顶层，可执行文件就在根上
        cp -R "$SRC"/. "$DEST"/
    fi

    # 命令行程序 + 许可与第三方声明。对照 CI：Unix 侧拷了这两份声明，
    # Windows 侧的打包步骤漏了 —— 这里统一都带上（见 THIRD_PARTY_NOTICES.md 开头）。
    CLI="$BUILD_DIR/out/pdscope-cli"
    [ "$OS" = windows ] && CLI="$CLI.exe"
    [ -f "$CLI" ] && cp "$CLI" "$DEST/"
    for f in LICENSE THIRD_PARTY_NOTICES.md; do
        [ -f "$REPO/$f" ] && cp "$REPO/$f" "$DEST/"
    done

    # 运行说明
    if [ "$OS" = windows ]; then
        HOW="双击 PDScope.exe 运行。"
        TIP="整个目录都要保留：pdscope.dll 与 data/ 缺一不可。"
    elif [ "$OS" = macos ]; then
        HOW="双击 PDScope.app 运行。"
        TIP="整个目录都要保留。首次打开若被 Gatekeeper 拦下，右键 →「打开」。"
    else
        HOW="双击 PDScope（或在终端里 ./PDScope）运行。"
        TIP="整个目录都要保留：lib/ 与 data/ 缺一不可。"
    fi
    cat > "$DEST/运行说明.txt" <<EOF
PDScope-NG
==========

$HOW
$TIP

pdscope-cli 是命令行版本，用法见仓库 README：
    pdscope-cli <抓包文件> --csv
EOF

    # ⚠ 这条检查很值：runner 的 CMakeLists 在核心库缺失时**只 WARNING 不报错**，
    #   界面能编译成功，运行时才弹「核心动态库没有加载成功」。这里提前拦下来。
    case "$OS" in
        windows)
            [ -f "$DEST/PDScope.exe" ] || die "归置后找不到 PDScope.exe：$DEST"
            [ -f "$DEST/pdscope.dll" ] || die "PDScope.exe 旁边没有 pdscope.dll —— 双击会报「核心动态库没有加载成功」"
            ;;
        linux)
            [ -f "$DEST/PDScope" ] || die "归置后找不到 PDScope 可执行文件：$DEST"
            chmod +x "$DEST/PDScope"
            ;;
        macos)
            [ -d "$DEST/PDScope.app" ] || die "归置后找不到 PDScope.app：$DEST"
            ;;
    esac

    echo ""
    echo "========================================================================"
    echo "  可直接双击运行：$DEST"
    case "$OS" in
        windows) echo "    入口：PDScope.exe" ;;
        macos)   echo "    入口：PDScope.app" ;;
        linux)   echo "    入口：PDScope" ;;
    esac
    echo "========================================================================"
elif [ "$CORE_ONLY" = 1 ]; then
    echo ""
    echo "  核心构建完成。产物：$BUILD_DIR/out"
fi
