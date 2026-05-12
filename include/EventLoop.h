class Epoll;

class EventLoop {
   private:
    Epoll* ep;

   public:
    EventLoop(Epoll* ep);
    ~EventLoop();
    void loop();
};
