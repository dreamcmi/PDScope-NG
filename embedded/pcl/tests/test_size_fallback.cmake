# @file test_size_fallback.cmake
# @brief 验证未知工具链及显式关闭优化均不添加平台专用参数。
cmake_minimum_required(VERSION 3.20)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/PclSizeOptions.cmake)

# @brief 使用没有实际目标的脚本；任何误用目标编译/链接命令都会令测试失败。
set(PCL_OPTIMIZE_SIZE ON)
set(MSVC FALSE)
set(CMAKE_C_COMPILER_ID PCLUnsupportedCompiler)
pcl_apply_size_options(pcl_target_must_not_be_accessed)

# @brief 禁用优化时即使已知MSVC也不能进入工具链探测或修改目标。
set(PCL_OPTIMIZE_SIZE OFF)
set(MSVC TRUE)
pcl_apply_size_options(pcl_target_must_not_be_accessed)
