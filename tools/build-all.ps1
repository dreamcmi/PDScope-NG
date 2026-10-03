<#
build-all.ps1 — 一键构建 PDScope-NG：C++ 核心 + Flutter 界面 → dist/ 里可直接双击运行的目录。

⚠ 这个文件必须存成 **带 BOM 的 UTF-8**，别改成无 BOM。
  Windows PowerShell 5.1 读 .ps1 时若没有 BOM，会按系统代码页（中文机器上是 936）
  解码，于是下面所有中文注释都变成乱码 —— 而且乱码字节里只要有一个落成引号或
  括号，整个脚本就**语法错误**，报错行号还指向注释中间，看着像别的地方写坏了。
  （和 runner/CMakeLists.txt 必须加 /utf-8 是同一类坑：源码编码没人管就会出事。）

这是给 Windows 用的那份（PowerShell 是 Windows 自带的）。Linux / macOS 用同一目录下的
build-all.sh，两者做的是一回事：同一套五步、同一套开关、同一个归置布局。

兼容 Windows PowerShell 5.1（不依赖 PowerShell 7）：所以刻意不用 `??`、三元运算符、
`-Depth` 这些 5.1 没有的写法。

默认依次做五步，任何一步失败就停下并保留现场：
    1. 配置并构建 C++ 核心（CMake + Ninja）
    2. 跑核心单测（pdscope-tests）——「跳过」不计为通过，有跳过会单独报出来
    3. Flutter：pub get / analyze / test
    4. 构建 Flutter 发行版
    5. 归置到 dist/PDScope-<平台>-<架构>/

用法：
    powershell -ExecutionPolicy Bypass -File tools\build-all.ps1
    powershell -ExecutionPolicy Bypass -File tools\build-all.ps1 -NoTests -Jobs 8
    powershell -ExecutionPolicy Bypass -File tools\build-all.ps1 -CoreOnly
#>
[CmdletBinding()]
param(
    [switch]$NoTests,       # 不跑测试
    [switch]$CoreOnly,      # 只构建 C++ 核心与单测
    [switch]$AppOnly,       # 只构建 Flutter 与归置（核心需已构建）
    # ⚠ 不能叫 -Debug：[CmdletBinding()] 已经带上了 PowerShell 自己的通用参数 -Debug，
    #   重名会让整个脚本连解析都过不去（"parameter ... defined multiple times"）。
    [switch]$DebugBuild,    # 构建 Debug 而不是 Release
    [int]$Jobs = 4,         # 并行编译任务数
    [switch]$Clean,         # 先删掉构建目录再重来
    [string]$Flutter,       # Flutter SDK 目录（含 bin\flutter.bat 的那一层）
    [string]$OutDir         # 归置到哪里（默认仓库下的 dist\）
)

$ErrorActionPreference = 'Stop'

# 输出统一按 UTF-8：仓库里其它程序都是写 UTF-8 的，这里跟上，
# 免得重定向到文件或管道时按系统代码页编出一份别的工具读不对的东西。
try { [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false) } catch { }

$Repo = Split-Path -Parent $PSScriptRoot
$App  = Join-Path $Repo 'app'
$OrigPwd = (Get-Location).Path

if ($CoreOnly -and $AppOnly) { Write-Host '--CoreOnly 与 --AppOnly 不能同时用'; exit 2 }

$Script:StepN = 0
$Script:StepTotal = 0
$Script:VsRoot = $null

function Die {
    param([string]$Message, [string[]]$Hints = @())
    Write-Host ''
    Write-Host "!! 失败：$Message"
    foreach ($h in $Hints) { Write-Host "   $h" }
    exit 1
}

function Write-Step {
    param([string]$Title)
    $Script:StepN++
    Write-Host ''
    Write-Host ('=' * 72)
    Write-Host "  [$($Script:StepN)/$($Script:StepTotal)] $Title"
    Write-Host ('=' * 72)
}

# 跑一条原生命令；非零就停。PowerShell 的 $ErrorActionPreference 管不到原生程序的
# 退出码，所以必须自己看 $LASTEXITCODE —— 少看一次就会「构建失败了还报成功」。
function Invoke-Native {
    param(
        [string]$Exe,
        [string[]]$Arguments = @(),
        [string]$WorkDir
    )
    Write-Host ("$ {0} {1}" -f $Exe, ($Arguments -join ' '))
    $old = (Get-Location).Path
    if ($WorkDir) { Set-Location -LiteralPath $WorkDir }
    $rc = 0
    try {
        & $Exe @Arguments
        $rc = $LASTEXITCODE
    } finally {
        Set-Location -LiteralPath $old
    }
    if ($rc -ne 0) { Die "命令返回 $rc" @("$Exe $($Arguments -join ' ')"); }
}

# ── 认平台 ───────────────────────────────────────────────────────────
$IsWin = $true
$IsMac = $false
$IsLinux = $false
if ($PSVersionTable.PSVersion.Major -ge 6) {
    if ($IsWindows) { $IsWin = $true } else { $IsWin = $false }
    if ($IsMacOS) { $IsMac = $true }
    if ($IsLinux) { $IsLinux = $true }
}
if ($IsMac) { $OsName = 'macos' } elseif ($IsLinux) { $OsName = 'linux' } else { $OsName = 'windows' }

$Arch = $env:PROCESSOR_ARCHITECTURE
switch ($Arch) {
    'AMD64' { $Arch = 'x64' }
    'ARM64' { $Arch = 'arm64' }
    'x86'   { $Arch = 'x86' }
    default { if (-not $Arch) { $Arch = 'unknown' } }
}

Write-Host 'PDScope-NG 一键构建'
Write-Host "  仓库     : $Repo"
Write-Host "  平台     : $OsName / $Arch"
$cfgName = 'Release'; if ($DebugBuild) { $cfgName = 'Debug' }
Write-Host "  配置     : $cfgName · 并行 $Jobs"
$testName = '跑'; if ($NoTests) { $testName = '跳过' }
Write-Host "  测试     : $testName"

# ── Windows：找一套**真的能用**的 MSVC + Windows SDK ─────────────────
# 与 tools/msvc-env.sh 的关键区别：判据是「必须真的有 cl.exe」。
# Visual Studio 更新有时会留下一个只有 lib/crt、没有 bin 的残缺目录，
# 按版本号取最大就会指向它，报错现场看着像「编译器没装」。
function Initialize-Msvc {
    if (-not $IsWin) { return }

    $vsRoot = $env:PDSCOPE_VS_ROOT
    if (-not $vsRoot) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (Test-Path -LiteralPath $vswhere) {
            $vsRoot = (& $vswhere -latest -products '*' `
                       -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
                       -property installationPath 2>$null | Select-Object -First 1)
            if ($vsRoot) { $vsRoot = $vsRoot.Trim() }
        }
    }
    if (-not $vsRoot) {
        foreach ($c in @("$env:ProgramFiles\Microsoft Visual Studio\2022\Community",
                         "$env:ProgramFiles\Microsoft Visual Studio\2022\Professional",
                         "$env:ProgramFiles\Microsoft Visual Studio\2022\Enterprise")) {
            if (Test-Path -LiteralPath $c) { $vsRoot = $c; break }
        }
    }
    if (-not $vsRoot) { Die '找不到 Visual Studio' @('装 VS 2022 的「使用 C++ 的桌面开发」工作负载，或用 PDSCOPE_VS_ROOT 指定根目录') }

    $msvcParent = Join-Path $vsRoot 'VC\Tools\MSVC'
    $toolset = $null
    if (Test-Path -LiteralPath $msvcParent) {
        $dirs = Get-ChildItem -LiteralPath $msvcParent -Directory -ErrorAction SilentlyContinue |
                Sort-Object -Property @{ Expression = { Convert-VersionKey $_.Name } } -Descending
        foreach ($d in $dirs) {
            if (Test-Path -LiteralPath (Join-Path $d.FullName 'bin\Hostx64\x64\cl.exe')) { $toolset = $d; break }
        }
    }
    if (-not $toolset) { Die "MSVC 工具集不可用：$msvcParent" @('没有任何一份含 bin\Hostx64\x64\cl.exe；用 VS Installer 修一下') }

    $kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
    $sdkInc = $null; $sdkLib = $null
    $incRoot = Join-Path $kits 'Include'
    if (Test-Path -LiteralPath $incRoot) {
        $dirs = Get-ChildItem -LiteralPath $incRoot -Directory -ErrorAction SilentlyContinue |
                Sort-Object -Property @{ Expression = { Convert-VersionKey $_.Name } } -Descending
        foreach ($d in $dirs) {
            if (Test-Path -LiteralPath (Join-Path $d.FullName 'ucrt\stdio.h')) { $sdkInc = $d; break }
        }
    }
    $libRoot = Join-Path $kits 'Lib'
    if (Test-Path -LiteralPath $libRoot) {
        $dirs = Get-ChildItem -LiteralPath $libRoot -Directory -ErrorAction SilentlyContinue |
                Sort-Object -Property @{ Expression = { Convert-VersionKey $_.Name } } -Descending
        foreach ($d in $dirs) {
            if (Test-Path -LiteralPath (Join-Path $d.FullName 'um\x64\kernel32.lib')) { $sdkLib = $d; break }
        }
    }
    if (-not $sdkInc -or -not $sdkLib) { Die "Windows SDK 不可用：$kits" @('缺 Include\*\ucrt\stdio.h 或 Lib\*\um\x64\kernel32.lib') }

    $inc = @((Join-Path $toolset.FullName 'include'))
    foreach ($x in @('ucrt', 'um', 'shared', 'winrt')) { $inc += (Join-Path $sdkInc.FullName $x) }
    $env:INCLUDE = ($inc -join ';')

    $lib = @((Join-Path $toolset.FullName 'lib\x64'))
    $lib += (Join-Path $sdkLib.FullName 'ucrt\x64')
    $lib += (Join-Path $sdkLib.FullName 'um\x64')
    $env:LIB = ($lib -join ';')

    $pathExtra = @((Join-Path $toolset.FullName 'bin\Hostx64\x64'))
    $sdkBin = Join-Path $kits ("bin\{0}\x64" -f $sdkInc.Name)
    if (Test-Path -LiteralPath $sdkBin) { $pathExtra += $sdkBin }
    $env:PATH = (($pathExtra + $env:PATH) -join ';')

    Write-Host "   MSVC 工具集: $($toolset.Name)"
    Write-Host "   Windows SDK: $($sdkInc.Name)"
    $Script:VsRoot = $vsRoot
}

# 把 '14.52.36725' / '10.0.26100.0' 变成**零填充的字符串**再排序。
# 为什么不返回数组让 Sort-Object 直接比：PowerShell 对数组值的比较不按逐段数值来，
# 排出来的顺序不保证是版本序。零填充成定宽后按字典序比就等价于按数值比。
function Convert-VersionKey {
    param([string]$Name)
    $parts = @()
    foreach ($p in ($Name -split '\.')) {
        if ($p -match '^\d+$') { $parts += ([int]$p).ToString('D6') } else { $parts += 'Z' }
    }
    return ($parts -join '.')
}

Initialize-Msvc

# ── 找 cmake / ninja ─────────────────────────────────────────────────
function Find-Tool {
    param([string]$Name, [string]$VsRelative)
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    if ($Script:VsRoot -and $VsRelative) {
        $p = Join-Path $Script:VsRoot $VsRelative
        if (Test-Path -LiteralPath $p) { return $p }
    }
    $pf = $env:ProgramFiles
    foreach ($c in @((Join-Path $pf "CMake\bin\$Name.exe"), (Join-Path ${env:ProgramFiles(x86)} "CMake\bin\$Name.exe"))) {
        if ($c -and (Test-Path -LiteralPath $c)) { return $c }
    }
    return $null
}

$CMake = Find-Tool -Name 'cmake' -VsRelative 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not $CMake) { Die '找不到 cmake' @('装 CMake 并加进 PATH，或让 Visual Studio 自带的 CMake 可用') }
Write-Host "  cmake    : $CMake"

$Ninja = Find-Tool -Name 'ninja' -VsRelative 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
if (-not $Ninja) { Die '找不到 ninja' @('装 Ninja（Windows 上 Visual Studio 自带一份）') }
Write-Host "  ninja    : $Ninja"

# ── 构建目录：与仓库既有约定一致 ─────────────────────────────────────
# Windows 用 build（app/windows/runner 的 CMakeLists 写死了从 build/out 拷核心库），
# Unix 用 build/native（README 与 CI 都这么写）。
if ($IsWin) { $BuildDir = Join-Path $Repo 'build' } else { $BuildDir = Join-Path $Repo 'build\native' }

# ── 数一下这轮要跑几步 ───────────────────────────────────────────────
if (-not $AppOnly) {
    $Script:StepTotal++
    if (-not $NoTests) { $Script:StepTotal++ }
}
if (-not $CoreOnly) { $Script:StepTotal += 3 }

if ($Clean) {
    foreach ($d in @($BuildDir, (Join-Path $App 'build'))) {
        if (Test-Path -LiteralPath $d) {
            Write-Host "  清理 $d"
            Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

# ── 1. 构建核心 ──────────────────────────────────────────────────────
if (-not $AppOnly) {
    Write-Step "配置并构建 C++ 核心 → $BuildDir"
    $cfgType = 'Release'; if ($DebugBuild) { $cfgType = 'Debug' }
    Invoke-Native -Exe $CMake -Arguments @('-S', $Repo, '-B', $BuildDir, '-G', 'Ninja',
        "-DCMAKE_BUILD_TYPE=$cfgType", "-DCMAKE_MAKE_PROGRAM=$Ninja")
    Invoke-Native -Exe $CMake -Arguments @('--build', $BuildDir, '--parallel', "$Jobs")
    Write-Host "   核心产物：$BuildDir\out"

    if (-not $NoTests) {
        Write-Step '跑 C++ 核心单测'
        $tests = Join-Path $BuildDir 'out\pdscope-tests.exe'
        if (-not (Test-Path -LiteralPath $tests)) { Die "找不到测试程序：$tests" }
        Write-Host "$ $tests"
        & $tests
        $rc = $LASTEXITCODE
        # 运行器的退出码 = 失败用例数，所以非零就是真失败
        if ($rc -ne 0) { Die "核心单测有 $rc 个用例失败" }
        Write-Host '   核心单测通过（「跳过」不计为通过，若有用例跳过上面会单独报出来）'
    }
}

# ── Flutter ──────────────────────────────────────────────────────────
if (-not $CoreOnly) {
    function Find-Flutter {
        # ⚠ 判据用「文件存在」即可：flutter.bat 在 Windows 上没有可执行位可言。
        $name = 'flutter.bat'
        if (-not $IsWin) { $name = 'flutter' }

        if ($Flutter) {
            if (Test-Path -LiteralPath $Flutter -PathType Leaf) { return $Flutter }
            $p = Join-Path $Flutter "bin\$name"
            if (Test-Path -LiteralPath $p) { return $p }
            return $null
        }
        foreach ($v in @('PDSCOPE_FLUTTER', 'FLUTTER_ROOT')) {
            $root = [Environment]::GetEnvironmentVariable($v)
            if ($root) {
                $p = Join-Path $root "bin\$name"
                if (Test-Path -LiteralPath $p) { return $p }
            }
        }
        $cmd = Get-Command 'flutter' -ErrorAction SilentlyContinue
        if ($cmd) { return $cmd.Source }

        # 几个常见位置。刻意不做全盘递归 —— 那既慢又容易撞进别人的目录。
        $roots = @()
        if ($IsWin) {
            $roots = @("$env:SystemDrive\flutter", 'C:\flutter', 'D:\flutter', 'E:\flutter',
                       'C:\src\flutter', 'D:\src\flutter', 'D:\Language\flutter',
                       'D:\Tools\flutter', 'D:\IDE\flutter', 'D:\dev\flutter')
            foreach ($d in @('C:', 'D:', 'E:')) { if (-not $roots.Contains("$d\flutter")) { $roots += "$d\flutter" } }
        } else {
            $roots = @('/opt/flutter', '/usr/local/flutter', '/usr/lib/flutter',
                       "$HOME/flutter", "$HOME/development/flutter", '/snap/flutter/common/flutter')
        }
        foreach ($r in $roots) {
            $p = Join-Path $r "bin\$name"
            if (Test-Path -LiteralPath $p) { return $p }
        }
        return $null
    }

    $FlutterExe = Find-Flutter
    if (-not $FlutterExe) {
        Die '找不到 Flutter SDK' @(
            '三种给法，任选一种：',
            '  1) -Flutter <SDK 目录>',
            '  2) 设环境变量 PDSCOPE_FLUTTER=<SDK 目录>（或 FLUTTER_ROOT）',
            '  3) 把 <SDK>\bin 加进 PATH',
            'SDK 目录指含 bin\flutter.bat 的那一层，不是 bin 里面。')
    }
    Write-Host "  flutter  : $FlutterExe"

    # 界面通过 FFI 找核心库；显式给一下，别让它靠猜。
    $env:PDSCOPE_LIB_DIR = Join-Path $BuildDir 'out'

    # ⚠ flutter 的每条子命令都必须在 app\ 里跑：它按当前目录找 pubspec.yaml，
    #   在仓库根跑会直接报 "Expected to find project root in current working directory"。
    Write-Step 'Flutter：pub get → analyze → test'
    Invoke-Native -Exe $FlutterExe -Arguments @('pub', 'get') -WorkDir $App
    Invoke-Native -Exe $FlutterExe -Arguments @('analyze') -WorkDir $App
    if (-not $NoTests) {
        Invoke-Native -Exe $FlutterExe -Arguments @('test', '--concurrency=1') -WorkDir $App
    }

    Write-Step "构建 Flutter 发行版（$OsName）"
    $buildArgs = @('build', $OsName)
    if ($DebugBuild) { $buildArgs += '--debug' } else { $buildArgs += '--release' }
    Invoke-Native -Exe $FlutterExe -Arguments $buildArgs -WorkDir $App

    # ── 5. 归置 ──────────────────────────────────────────────────────
    Write-Step '归置到可直接双击运行的目录'
    $cfg = 'Release'; if ($DebugBuild) { $cfg = 'Debug' }
    if ($IsWin) {
        $src = Join-Path $App "build\windows\x64\runner\$cfg"
    } elseif ($IsMac) {
        $src = Join-Path $App "build\macos\Build\Products\$cfg"
    } else {
        $src = $null
        $base = Join-Path $App 'build/linux'
        if (Test-Path -LiteralPath $base) {
            $cfgLower = $cfg.ToLower()
            foreach ($a in (Get-ChildItem -LiteralPath $base -Directory -ErrorAction SilentlyContinue)) {
                $b = Join-Path $a.FullName "$cfgLower\bundle"
                if (Test-Path -LiteralPath $b) { $src = $b; break }
            }
        }
    }
    if (-not $src -or -not (Test-Path -LiteralPath $src)) { Die "找不到 Flutter 产物目录：$src" }

    # -Out 给相对路径时按「调用脚本时所在的目录」算，而不是 app\ ——
    # 归置这一段虽然不切目录，但保持一致更不容易出意外。
    if ($OutDir) {
        if ([System.IO.Path]::IsPathRooted($OutDir)) { $destBase = $OutDir }
        else { $destBase = Join-Path $OrigPwd $OutDir }
    } else {
        $destBase = Join-Path $Repo 'dist'
    }
    $dest = Join-Path $destBase "PDScope-$OsName-$Arch"
    if (Test-Path -LiteralPath $dest) { Remove-Item -LiteralPath $dest -Recurse -Force }
    New-Item -ItemType Directory -Path $dest -Force | Out-Null

    if ($IsMac) {
        # .app 是自包含的一整包，双击它就行
        Copy-Item -LiteralPath (Join-Path $src 'PDScope.app') -Destination (Join-Path $dest 'PDScope.app') -Recurse -Force
    } else {
        # Windows / Linux：目录里的内容整个摊到顶层，可执行文件就在根上
        Copy-Item -Path (Join-Path $src '*') -Destination $dest -Recurse -Force
    }

    # 命令行程序 + 许可与第三方声明。对照 CI：Unix 侧拷了这两份声明，
    # Windows 侧的打包步骤漏了 —— 这里统一都带上（见 THIRD_PARTY_NOTICES.md 开头）。
    $cliName = 'pdscope-cli'; if ($IsWin) { $cliName = 'pdscope-cli.exe' }
    $cli = Join-Path $BuildDir "out\$cliName"
    if (Test-Path -LiteralPath $cli) { Copy-Item -LiteralPath $cli -Destination $dest -Force }
    foreach ($f in @('LICENSE', 'THIRD_PARTY_NOTICES.md')) {
        $p = Join-Path $Repo $f
        if (Test-Path -LiteralPath $p) { Copy-Item -LiteralPath $p -Destination $dest -Force }
    }

    # 运行说明。⚠ 5.1 的 Set-Content -Encoding UTF8 会写 BOM，这里要的是无 BOM UTF-8。
    if ($IsWin) {
        $how = '双击 PDScope.exe 运行。'
        $tip = '整个目录都要保留：pdscope.dll 与 data\ 缺一不可。'
    } elseif ($IsMac) {
        $how = '双击 PDScope.app 运行。'
        $tip = '整个目录都要保留。首次打开若被 Gatekeeper 拦下，右键 →「打开」。'
    } else {
        $how = '双击 PDScope（或在终端里 ./PDScope）运行。'
        $tip = '整个目录都要保留：lib\ 与 data\ 缺一不可。'
    }
    $note = @"
PDScope-NG
==========

$how
$tip

pdscope-cli 是命令行版本，用法见仓库 README：
    pdscope-cli <抓包文件> --csv
"@
    $notePath = Join-Path $dest '运行说明.txt'
    [System.IO.File]::WriteAllText($notePath, $note, (New-Object System.Text.UTF8Encoding($false)))

    # ⚠ 这条检查很值：runner 的 CMakeLists 在核心库缺失时**只 WARNING 不报错**，
    #   界面能编译成功，运行时才弹「核心动态库没有加载成功」。这里提前拦下来。
    if ($IsWin) {
        if (-not (Test-Path -LiteralPath (Join-Path $dest 'PDScope.exe'))) { Die "归置后找不到 PDScope.exe：$dest" }
        if (-not (Test-Path -LiteralPath (Join-Path $dest 'pdscope.dll'))) {
            Die 'PDScope.exe 旁边没有 pdscope.dll —— 双击会报「核心动态库没有加载成功」'
        }
    } elseif ($IsLinux) {
        if (-not (Test-Path -LiteralPath (Join-Path $dest 'PDScope'))) { Die "归置后找不到 PDScope 可执行文件：$dest" }
    } elseif ($IsMac) {
        if (-not (Test-Path -LiteralPath (Join-Path $dest 'PDScope.app'))) { Die "归置后找不到 PDScope.app：$dest" }
    }

    Write-Host ''
    Write-Host ('=' * 72)
    Write-Host "  可直接双击运行：$dest"
    if ($IsWin) { Write-Host '    入口：PDScope.exe' }
    elseif ($IsMac) { Write-Host '    入口：PDScope.app' }
    else { Write-Host '    入口：PDScope' }
    Write-Host ('=' * 72)
} else {
    Write-Host ''
    Write-Host "  核心构建完成。产物：$BuildDir\out"
}
