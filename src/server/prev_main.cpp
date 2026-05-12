#include <iostream>
// Linux 系统编程基础
#include <fcntl.h>  // 提供 fcntl() (用于设置非阻塞 IO)
#include <sys/epoll.h>
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

// define
#define MAX_EVENTS 100
#define READ_BUFFER 1024

int prev_main() {
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

    int epfd = epoll_create1(0);
    errif(epfd == -1, "epoll create error");

    // 创建两个 epoll_event
    // 类型变量，一个是缓冲数组，一个是用于填充、复用的临时体
    epoll_event events[MAX_EVENTS], ev;
    memset(events, 0, sizeof(events));
    memset(&ev, 0, sizeof(ev));

    // 填充ev，并加入epoll进行监视
    ev.data.fd = listen_fd;
    ev.events = EPOLLIN | EPOLLET;

    // 设置非阻塞
    setnonblocking(listen_fd);
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    // IO
    while (true) {
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);
        errif(nfds == -1, "epoll wait error");

        for (int i = 0; i < nfds; ++i) {
            if (events[i].data.fd == listen_fd) {
                // 新连接
                // 加载 clnt_fd
                struct sockaddr_in clnt_addr;
                socklen_t clnt_addr_len = sizeof(clnt_addr);
                bzero(&clnt_addr, sizeof(clnt_addr));
                int clnt_sockfd =
                    accept(listen_fd, (sockaddr*)&clnt_addr, &clnt_addr_len);
                errif(clnt_sockfd == -1, "socket accept error");
                std::cout << "new client accept!\n";

                // 将新产生的 clnt_fd 加入epoll监听
                memset(&ev, 0, sizeof(ev));
                ev.data.fd = clnt_sockfd;
                ev.events = EPOLLIN | EPOLLET;
                setnonblocking(clnt_sockfd);
                epoll_ctl(epfd, EPOLL_CTL_ADD, clnt_sockfd, &ev);
            } else if (events[i].events & EPOLLIN) {
                // 可读事件
                // 在 epoll 的 ET
                // 模式下，内核只会在缓冲区状态发生“变化”时通知你一次。
                // 通过“死循环”确保将内核接收缓冲区中的数据彻底抽干 (Drain)
                char buf[READ_BUFFER];
                int clnt_sockfd = events[i].data.fd;
                while (true) {
                    memset(buf, 0, sizeof(buf));
                    ssize_t read_byte = read(clnt_sockfd, buf, sizeof(buf));
                    if (read_byte > 0) {
                        std::cout << "msg from client " << clnt_sockfd << " : "
                                  << buf << '\n';
                        write(clnt_sockfd, buf, read_byte);
                    } else if (read_byte == -1 && errno == EINTR) {
                        // 被信号中断，继续读取
                        std::cout << "interrupted! continue.\n";
                        continue;
                    } else if (read_byte == -1 &&
                               ((errno == EAGAIN) || (errno == EWOULDBLOCK))) {
                        // 非阻塞IO中，这个条件表示数据全部读取完毕
                        std::cout << "read OK!\n";
                        break;
                    } else if (read_byte == 0) {  // EOF，客户端断开连接
                        std::cout << "EOF\n";
                        // 关闭socket会自动将文件描述符从epoll树上移除
                        close(clnt_sockfd);
                        break;
                    }
                }
            } else {
                //
            }
        }
    }
    close(listen_fd);
    return 0;
}