#include "Server.h"

int main() {
    Server s;
    s.listenPort(8888);
    s.startLoop();
    return 0;
}