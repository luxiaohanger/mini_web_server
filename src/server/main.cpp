#include "Server.h"

int main() {
    Server s;
    s.listenPort(8888);
    s.start();
    return 0;
}