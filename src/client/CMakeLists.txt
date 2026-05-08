# 1. 搜集所有源文件（虽然手动列出更好，但 src 目录下全扫描比较省事）
file(GLOB SRCS CONFIGURE_DEPENDS "*.cpp")

# 2. 定义可执行文件目标
# 基于目标的配置（Target-based Management）
# library 和 executable 都是 target
add_executable(client ${SRCS})

target_link_libraries(client PUBLIC share_lib)
target_include_directories(client PUBLIC ${CMAKE_SOURCE_DIR}/include)
