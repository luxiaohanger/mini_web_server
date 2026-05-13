#pragma once
#include <unordered_map>
#include <vector>
class Socket;
class EventLoop;
class Acceptor;
class Connection;

class Server {
   private:
    EventLoop* eloop;
    std::vector<Acceptor*> Acceptors;
    std::unordered_map<int, Connection*> Connections;
    void handleNewConnection(Socket* sck);
    void handleDeleteConnection(int fd);

   public:
    Server();
    ~Server();

    void listenPort(int port);
    void startLoop();
};