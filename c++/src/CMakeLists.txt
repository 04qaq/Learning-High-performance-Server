# src/CMakeLists.txt
# 把 src 里实现做成一个库，然后链接到可执行文件

# 列出源文件（显式列举比 glob 更可控）
set(CORE_SOURCES
    log.cpp
    Config.cpp
    #thread.cpp
    fiber.cpp
)

add_library(core ${CORE_SOURCES})
# 公开头文件所在路径（使用 target_include_directories）
target_include_directories(core
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:include>  # 安装时头文件放的位置
)
target_compile_features(core PUBLIC cxx_std_17)

# 可执行文件
add_executable(app main.cpp)
find_package(yaml-cpp REQUIRED)
target_link_libraries(app PRIVATE yaml-cpp)
target_link_libraries(app PRIVATE core)

# 如果你想添加安装规则：
install(TARGETS core app
        EXPORT MyAppTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
