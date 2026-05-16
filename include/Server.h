#pragma once
#include <unordered_map>
#include <vector>
class Socket;
class EventLoop;
class Acceptor;
class Connection;
class ThreadPool;

class Server {
   private:
    ThreadPool* threadpool;
    EventLoop* eloop;
    std::vector<Acceptor*> Acceptors;
    std::unordered_map<Socket*, Connection*> Connections;
    void handleNewConnection(Socket* sck);
    void handleDeleteConnection(Socket* sck);

   public:
    Server();
    ~Server();

    void listenPort(int port);
    void startLoop();
};