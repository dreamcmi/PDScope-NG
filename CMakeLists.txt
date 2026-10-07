# PDScope-NG — CMake 工程（C++17 核心 + C ABI + 独立 CLI）
#
# 目标：
#   pdscope           共享库（供 Dart FFI 调用，只导出 extern "C" 接口）
#   pdscope-cli       独立命令行导出/查看（静态链接核心，不依赖 Flutter / 显示服务器）
#   pdscope-tests     核心测试（容器 / 协议 / 导出）
#
# 依赖全部来自仓库内的 third_party，离线可构建；不在构建期下载任何东西。

cmake_minimum_required(VERSION 3.20)

project(PDScope-NG
    VERSION 0.3.0
    DESCRIPTION "USB PD / UFCS capture analyzer core"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# 版本号只有一个来源（本文件的 project(VERSION …)），编译期注入。
# ⚠ 别在源码里再写一份字面量：那会变成第二个真相源，发行时必然对不上。
add_compile_definitions(PDSCOPE_NG_VERSION="${PROJECT_VERSION}")

if(MSVC)
    add_compile_options(/utf-8 /W3 /MP)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# 所有产物集中到一处，方便 Flutter 侧按平台拷贝
set(PDSCOPE_OUTPUT_DIR ${CMAKE_BINARY_DIR}/out)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PDSCOPE_OUTPUT_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PDSCOPE_OUTPUT_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ── third_party: zlib ────────────────────────────────────────────────
# 只编 raw DEFLATE 需要的那些源文件，静态链进核心。保留源文件里的 zlib 许可声明。
set(ZLIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/zlib)
add_library(pdscope_zlib STATIC
    ${ZLIB_DIR}/adler32.c
    ${ZLIB_DIR}/compress.c
    ${ZLIB_DIR}/crc32.c
    ${ZLIB_DIR}/deflate.c
    ${ZLIB_DIR}/infback.c
    ${ZLIB_DIR}/inffast.c
    ${ZLIB_DIR}/inflate.c
    ${ZLIB_DIR}/inftrees.c
    ${ZLIB_DIR}/trees.c
    ${ZLIB_DIR}/uncompr.c
    ${ZLIB_DIR}/zutil.c)
target_include_directories(pdscope_zlib PUBLIC ${ZLIB_DIR})
set_target_properties(pdscope_zlib PROPERTIES FOLDER third_party)

# ── third_party: SQLite amalgamation ─────────────────────────────────
# 只读连接 POWER-Z 导出的库。关掉用不到的特性以缩小体积并去掉多余行为。
set(SQLITE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite)
add_library(pdscope_sqlite STATIC ${SQLITE_DIR}/sqlite3.c)
target_include_directories(pdscope_sqlite PUBLIC ${SQLITE_DIR})
target_compile_definitions(pdscope_sqlite PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_OMIT_LOAD_EXTENSION=1
    SQLITE_OMIT_DEPRECATED=1
    SQLITE_DQS=0
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_OMIT_PROGRESS_CALLBACK=1
    SQLITE_OMIT_SHARED_CACHE=1
    # 只读反序列化（sqlite3_deserialize）：把整份文件当内存库读，不落盘、不做副本。
    # 显式打开，免得将来某个版本的默认值变了导致核心编不过。
    SQLITE_ENABLE_DESERIALIZE=1
    SQLITE_MAX_EXPR_DEPTH=0)
set_target_properties(pdscope_sqlite PROPERTIES FOLDER third_party)

# ── third_party: nlohmann/json（仅头文件，不编进库）─────────────────
add_library(pdscope_json INTERFACE)
target_include_directories(pdscope_json INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json)
set_target_properties(pdscope_json PROPERTIES FOLDER third_party)

# ── 核心 ─────────────────────────────────────────────────────────────
add_library(pdscope_core STATIC
    core/src/util.cpp
    core/src/container/zip.cpp
    core/src/container/inflate.cpp
    core/src/container/atkcc.cpp
    core/src/container/sqlite_reader.cpp
    core/src/container/powerz.cpp
    core/src/container/pdstream.cpp
    core/src/pd/symbols.cpp
    core/src/pd/crc.cpp
    core/src/pd/format.cpp
    core/src/pd/tables.cpp
    core/src/pd/svid.cpp
    core/src/pd/pdo.cpp
    core/src/pd/data.cpp
    core/src/pd/vdm.cpp
    core/src/pd/extended.cpp
    core/src/pd/decoder.cpp
    core/src/ufcs/crc.cpp
    core/src/ufcs/tables.cpp
    core/src/ufcs/format.cpp
    core/src/ufcs/frame.cpp
    core/src/ufcs/payload.cpp
    core/src/ufcs/decoder.cpp
    core/src/bmc.cpp
    core/src/pipeline.cpp
    core/src/filters.cpp
    core/src/csv.cpp
    core/src/session.cpp)
target_include_directories(pdscope_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/core/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/core/src)
target_link_libraries(pdscope_core PRIVATE pdscope_zlib pdscope_sqlite pdscope_json)
set_target_properties(pdscope_core PROPERTIES FOLDER core)

# ── 共享库（C ABI）─────────────────────────────────────────────────
# .def 当**源文件**交给 CMake，而不是自己拼 `/DEF:"…"` 链接选项：
# 后者在 Ninja + MSVC 下会把引号再包一层，变成 LNK1104 打不开 `"路径"`。
set(PDSCOPE_DEF_SRC "")
if(MSVC)
    set(PDSCOPE_DEF_SRC core/pdscope.def)
endif()

add_library(pdscope SHARED core/src/abi.cpp core/src/live_pd.cpp ${PDSCOPE_DEF_SRC})
target_include_directories(pdscope PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/core/include)
# abi.cpp 要经 session.h 拿到核心内部类型（util.h 里有 nlohmann/json），
# 所以这几条 include 与其它消费核心内部头的目标保持一致。
target_include_directories(pdscope PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/core/src
    ${ZLIB_DIR}
    ${SQLITE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json)
# 定义在编导出符号；Windows 下没它 __declspec(dllexport) 会退化成 dllimport
target_compile_definitions(pdscope PRIVATE PDSCOPE_BUILD_SHARED=1)
target_link_libraries(pdscope PRIVATE pdscope_core)
# 核心与三方库全部静态并入，最终只留一个可搬运的动态库
set_target_properties(pdscope PROPERTIES
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    FOLDER core)

# ── CLI ──────────────────────────────────────────────────────────────
# CLI 直接用核心的 C++ 接口（不经 C ABI）：它要打表格、要报通道清单与挑通道结果，
# 走 C ABI 得多绕一层 JSON。C ABI 本身由 pdscope-tests 覆盖。
add_executable(pdscope-cli cli/main.cpp)
target_link_libraries(pdscope-cli PRIVATE pdscope_core)
# CLI 直接用核心的 C++ 头（session.h → util.h → nlohmann/json），
# 所以第三方的 include 目录要自己带上：那些目录挂在 pdscope_core 的 PRIVATE 链接上，不外传。
target_include_directories(pdscope-cli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/core/src
    ${ZLIB_DIR}
    ${SQLITE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json)
set_target_properties(pdscope-cli PROPERTIES FOLDER cli)

# ── 测试 ─────────────────────────────────────────────────────────────
enable_testing()
add_executable(pdscope-tests
    tests/test_main.cpp
    tests/test_zip.cpp
    tests/test_atkcc.cpp
    tests/test_pdstream.cpp
    tests/test_sqlite.cpp
    tests/test_pd.cpp
    tests/test_ufcs.cpp
    tests/test_csv.cpp
    tests/test_abi.cpp
    tests/test_live_pd.cpp
    # ⚠ 把 C ABI 的实现也编进测试目标，测试才能真的调 pdscope_open_file 这些函数。
    #   之前只测 C++ 内部接口，"边界上这一层"完全没覆盖 —— 结果就是一个
    #   「Windows 打不开非 ASCII 路径」的 bug 一路躲过了全部测试，
    #   最后是 Dart 侧的集成测试才把它揪出来（见 tests/test_path.cpp）。
    core/src/abi.cpp
    core/src/live_pd.cpp)
target_compile_definitions(pdscope-tests PRIVATE PDSCOPE_BUILD_SHARED=1)
target_link_libraries(pdscope-tests PRIVATE pdscope_core)
target_include_directories(pdscope-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/core/src
    # 测试要现场造 SQLite 样本（sqlite3_serialize），所以自己也需要 amalgamation 的头
    ${SQLITE_DIR}
    ${ZLIB_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json)
add_test(NAME pdscope-tests COMMAND pdscope-tests)

# ── 桌面外壳（Windows 独有）─────────────────────────────────────────
# 外壳里**纯逻辑**的那几段并进同一个测试目标：参数过滤、WM_COPYDATA 编解码、
# 中文菜单结构与标签、窗口类名。它们全是「错了不报错」的类型，值得自动盯着。
# （真在窗口消息里跑的那几段 —— 拖放、菜单点击、跨实例转交 —— 只能靠 GUI 冒烟，
#   见 doc/desktop.md 的「怎么验证」。这也是 shell.cpp 不依赖 runner 其它文件的
#   原因：带上 utils.h 就得链 Flutter 引擎，这个目标编不动。）
if(WIN32)
    target_sources(pdscope-tests PRIVATE
        tests/test_shell_win.cpp
        app/windows/runner/shell.cpp)
    target_include_directories(pdscope-tests PRIVATE app/windows/runner)
    target_link_libraries(pdscope-tests PRIVATE shell32 user32)
endif()

# ── 安装布局（发行包按平台各自打包，这里只保证 install 目标可用）─────
include(GNUInstallDirs)
install(TARGETS pdscope pdscope-cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES core/include/pdscope/pdscope.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/pdscope)
install(FILES LICENSE THIRD_PARTY_NOTICES.md DESTINATION ${CMAKE_INSTALL_DATADIR}/pdscope)
