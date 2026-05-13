#include "error_solve.h"

#include <iostream>

void errif(bool condition, const char* errmsg) {
    if (condition) {
        // 打印自定义错误信息 + 系统错误码解释
        perror(errmsg);
        exit(EXIT_FAILURE);
    }
}
