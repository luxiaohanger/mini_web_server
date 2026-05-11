#include "error_solve.h"

#include <fcntl.h>

#include <iostream>

void errif(bool condition, const char* errmsg) {
    if (condition) {
        // 打印自定义错误信息 + 系统错误码解释
        perror(errmsg);
        exit(EXIT_FAILURE);
    }
}

void setnonblocking(int fd) {
    // file control
    // 先 file get flag ,位掩码修改后 file set flag
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

bool is_valid_fd(int fd) { return fcntl(fd, F_GETFL) != -1 || errno != EBADF; }