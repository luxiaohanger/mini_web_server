class Epoll;
class Channel;
class EventLoop {
   private:
    Epoll* ep;

   public:
    EventLoop();
    ~EventLoop();
    void loop();
    void updateChannel(Channel* channel);
};
