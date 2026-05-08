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

#include "error_solve.h"

int main() {
    std::cout << "here is client\n";

    int cli_fd = socket(PF_INET, SOCK_STREAM, 0);
    errif(cli_fd == -1, "socket create error");

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    serv_addr.sin_port = htons(8888);

    errif(connect(cli_fd, (sockaddr*)&serv_addr, sizeof(serv_addr)) == -1,
          "socket connect error");

    while (true) {
        char buf[1024]{};
        std::cin >> buf;

        ssize_t write_byte = write(cli_fd, buf, sizeof(buf));
        errif(write_byte == -1, "socket write error");

        memset(buf, 0, sizeof(buf));  // 清空缓冲区
        ssize_t read_bytes =
            read(cli_fd, buf,
                 sizeof(buf));  // 从服务器socket读到缓冲区，返回已读数据大小
        if (read_bytes > 0) {
            printf("message from server: %s\n", buf);
        } else if (
            read_bytes ==
            0) {  // read返回0，表示EOF，通常是服务器断开链接，等会儿进行测试
            printf("server socket disconnected!\n");
            break;
        } else if (read_bytes ==
                   -1) {  // read返回-1，表示发生错误，按照上文方法进行错误处理
            close(cli_fd);
            errif(true, "socket read error");
        }
    }

    return 0;
}