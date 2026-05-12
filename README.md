# 基于 epoll 的高性能 web 服务器

## 技术栈
- C++ socket 编程
- CMake 项目管理
- Linux 服务器的使用
- shell 脚本
- epoll 
- Reactor 架构、事件驱动、任务分发



## 学习日志
- stage1 : 实现简单的socket连接
- stage2 : 增加错误判断 `errif` ; 实现 echo 服务器 ; 重构CMAKE,规范基于目标的配置
- stage3 : 使用 非阻塞 ET 式 epoll 监听 fd ; 使用面向对象重构文件 ;学习复杂依赖关系的类如何编写 .h 和 .cpp 文件
- stage4 : 将裸露文件描述符封装为 `Channel`；实现简单的 Reactor 架构和事件驱动、任务分发；尝试自己重构高度耦合的类；新建docs文档，记录架构变化，当前实现为 /docs/DESIGN_1.md

## quick start
- 1.cmake build
- 2.参见 scripts/SCRIPTS.md


## 致谢
本项目学习了 `https://github.com/yuesong-feng/30dayMakeCppServer` 


