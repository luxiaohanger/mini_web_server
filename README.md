# 基于 epoll 的高性能 web 服务器

## 技术栈
- C++ : socket 网络编程、移动语义、函数回调（function、bind）、RAII 资源管理 、string_view
- CMake : 基于目标的配置
- Linux : 服务器的使用；epoll; readv 散射读取
- shell 脚本
- Reactor 架构：事件驱动、分发与业务解耦



## 学习日志
- stage1 : 实现简单的socket连接
- stage2 : 增加错误判断 `errif` ; 实现 echo 服务器 ; 重构CMAKE , 规范基于目标的配置
- stage3 : 使用 非阻塞 ET 式 epoll 监听 fd ，实现单文件 [代码雏形](/src/server/prev_main.cpp); 使用面向对象重构文件 ; 学习复杂依赖关系的类如何编写 .h 和 .cpp 文件
- stage4 : 将裸露文件描述符封装为 `Channel`；实现简单的 Reactor 架构和事件驱动、任务分发；尝试自己重构高度耦合的类；新建docs文档，记录架构变化，实现 [架构设计v1](/docs/DESIGN_v1.md)
- stage5 : 彻底重构设计架构，实现工业级 C++ 网络库的核心雏形，详见 [架构设计v2](/docs/DESIGN_v2.md)
- stage6 : 重新设计 `Socket` 类，封装相关底层调用；调整持有裸露 fd 的类持有 `Socket*` ,形成 RAII 资源管理闭环；引入 `Buffer` 类，实现高性能的缓冲区数据传输，详见 [架构设计v3](/docs/DESIGN_v3.md)

## quick start
- 1.cmake build
- 2.参见 scripts/SCRIPTS.md


## 致谢
本项目学习了 `https://github.com/yuesong-feng/30dayMakeCppServer` 


