cmake_minimum_required(VERSION 3.14)
project(runner LANGUAGES CXX)

# Define the application target. To change its name, change BINARY_NAME in the
# top-level CMakeLists.txt, not the value here, or `flutter run` will no longer
# work.
#
# Any new source files that you add to the application should be added here.
add_executable(${BINARY_NAME} WIN32
  "flutter_window.cpp"
  "main.cpp"
  "shell.cpp"
  "utils.cpp"
  "win32_window.cpp"
  "${FLUTTER_MANAGED_DIR}/generated_plugin_registrant.cc"
  "Runner.rc"
  "runner.exe.manifest"
)

# Apply the standard set of build settings. This can be removed for applications
# that need different build settings.
apply_standard_settings(${BINARY_NAME})

# Add preprocessor definitions for the build version.
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION=\"${FLUTTER_VERSION}\"")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_MAJOR=${FLUTTER_VERSION_MAJOR}")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_MINOR=${FLUTTER_VERSION_MINOR}")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_PATCH=${FLUTTER_VERSION_PATCH}")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_BUILD=${FLUTTER_VERSION_BUILD}")

# Disable Windows macros that collide with C++ standard library functions.
target_compile_definitions(${BINARY_NAME} PRIVATE "NOMINMAX")

# 源码里有中文（菜单项与注释）。不加 /utf-8 的话 MSVC 按系统 ANSI 代码页读源码，
# 实测（CP936 环境）：先一条 C4819，然后中文注释的字节能把后面的代码吃掉，
# 直接报语法错 —— 也就是说这个 target 根本编不出东西来。
target_compile_options(${BINARY_NAME} PRIVATE "/utf-8")

# Add dependency libraries and include directories. Add any application-specific
# dependencies here.
target_link_libraries(${BINARY_NAME} PRIVATE flutter flutter_wrapper_app)
target_link_libraries(${BINARY_NAME} PRIVATE "dwmapi.lib")
# 外壳用到的：CommandLineToArgvW / DragQueryFile / DragFinish / DragAcceptFiles。
target_link_libraries(${BINARY_NAME} PRIVATE "shell32.lib")
target_include_directories(${BINARY_NAME} PRIVATE "${CMAKE_SOURCE_DIR}")

# Run the Flutter tool portions of the build. This must not be removed.
add_dependencies(${BINARY_NAME} flutter_assemble)

# ── 把核心动态库放到可执行文件旁边 ────────────────────────────────────
# Dart 侧的 FFI 先找可执行文件同目录，所以构建完必须把 pdscope.dll 拷过来。
# 开发期的库在本仓库的 build/out/（CMake 工程产出）；发行包由打包脚本统一放置。
#
# ⚠ 先构建核心再构建界面：cmake -S .. -B ../build && cmake --build ../build
#   库还不存在时**不报错**，只是这一步被跳过 —— 否则第一次 clone 下来连
#   界面都编不了（那时还没人跑过核心的构建）。
set(PDSCOPE_CORE_DLL
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../build/out/pdscope.dll")

if(EXISTS "${PDSCOPE_CORE_DLL}")
  add_custom_command(TARGET ${BINARY_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PDSCOPE_CORE_DLL}"
            "$<TARGET_FILE_DIR:${BINARY_NAME}>"
    COMMENT "拷贝核心动态库 pdscope.dll 到运行目录")
else()
  message(WARNING
    "没找到 ${PDSCOPE_CORE_DLL} —— 界面能编译，但运行时会报「核心动态库没有加载成功」。"
    "先构建核心：cmake -S .. -B ../build && cmake --build ../build")
endif()
