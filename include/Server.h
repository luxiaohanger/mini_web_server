#pragma once
#include <memory>
#include <vector>
class Socket;
class EventLoop;
class Acceptor;
class Connection;
class ThreadPool;
class SubReactor;
class MainReactor;

class Server {
   private:
    std::unique_ptr<ThreadPool> threadpool;
    std::unique_ptr<MainReactor> mainReactor;
    std::vector<std::unique_ptr<SubReactor>> SubReactors;
    int subIdx;
    void handleNewConnection(Socket* sck);

   public:
    Server();
    ~Server();

    void listenPort(int port);
    void start();
};