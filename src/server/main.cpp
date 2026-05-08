#include <iostream>
// Linux 系统编程基础
#include <fcntl.h>       // 提供 fcntl() (用于设置非阻塞 IO)
#include <sys/socket.h>  // 提供 socket(), bind(), listen(), accept(), setsockopt()
#include <sys/types.h>
#include <unistd.h>  // 提供 close(), read(), write()

// 网络地址转换
#include <arpa/inet.h>   // 提供 inet_ntop(), inet_pton(), htons()
#include <netinet/in.h>  // 提供 struct sockaddr_in

// 错误处理与日志
#include <errno.h>   // 提供 errno 变量
#include <string.h>  // 提供 strerror()

// include/头文件
#include "error_solve.h"

int main() {
    std::cout << "here is server\n";

    int listen_fd = socket(PF_INET, SOCK_STREAM, 0);
    errif(listen_fd == -1, "socket create error");

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;          // 地址族
    addr.sin_addr.s_addr = INADDR_ANY;  // 监听所有网卡（0.0.0.0）
    addr.sin_port = htons(8888);        // htons 是为了处理大端/小端字节序转换

    errif(bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) == -1,
          "socket bind error");

    errif(listen(listen_fd, 128) == -1, "socket listen error");

    struct sockaddr_in clnt_addr;
    socklen_t clnt_addr_len = sizeof(clnt_addr);
    bzero(&clnt_addr, sizeof(clnt_addr));

    // 计时
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    int clnt_sockfd = accept(listen_fd, (sockaddr*)&clnt_addr, &clnt_addr_len);
    errif(clnt_sockfd == -1, "socket accept error");

    clock_gettime(CLOCK_MONOTONIC, &end);

    // 计算耗时（秒）
    double time_spent = (end.tv_sec - start.tv_sec) +
                        (end.tv_nsec - start.tv_nsec) / 1000000000.0;

    std::cout << "等待连接耗时 :" << time_spent << '\n';

    // IO
    while (true) {
        char buf[1024]{};

        ssize_t read_byte = read(clnt_sockfd, buf, sizeof(buf));

        if (read_byte > 0) {
            std::cout << "msg from client " << clnt_sockfd << " : " << buf
                      << '\n';
            write(clnt_sockfd, buf, read_byte);
        } else if (read_byte == 0) {
            std::cout << clnt_sockfd << " disconnect" << '\n';
            close(clnt_sockfd);
            break;
        } else {
            close(clnt_sockfd);
            errif(true, "socket read error");
        }
    }

    return 0;
}