cmake_minimum_required(VERSION 3.15)
project(MyApp VERSION 1.0 LANGUAGES CXX)

# 全局选项（可以在命令行覆盖）
option(BUILD_SHARED_LIBS "Build libraries as shared" OFF)
option(BUILD_TESTING "Enable building tests" ON)

# 使用现代 CMake：在 target 层面设置标准
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 推荐开启位置无关代码以便生成 shared/static 都友好（可选）
if(NOT CMAKE_POSITION_INDEPENDENT_CODE)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# 输出目录（可自定义）
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# 子目录
add_subdirectory(src)

# 安装规则（可选）
include(GNUInstallDirs)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
# 导出 targets（如果你做 install + package 再完善）
