add_library(share_lib

    error_solve.cpp

    # 后续cpp文件
)

target_include_directories(share_lib PUBLIC ${CMAKE_SOURCE_DIR}/include)