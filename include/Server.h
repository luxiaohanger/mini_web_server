#pragma once
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
    ThreadPool* threadpool;
    MainReactor* mainReactor;
    std::vector<SubReactor*> SubReactors;
    int subIdx;
    void handleNewConnection(Socket* sck);

   public:
    Server();
    ~Server();

    void listenPort(int port);
    void start();
};