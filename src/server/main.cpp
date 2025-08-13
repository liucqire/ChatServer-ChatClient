#include "chatserver.hpp"
#include "chatservice.hpp"
#include <iostream>
#include <signal.h>

void resetHandler(int) {
    ChatService::instance()->reset();
    exit(0);
}

int main(int argc, char **argv) {
    if (argc != 3)
        cerr << "Usage: ./ChatServer 127.0.0.1 6000" << endl;

    signal(SIGINT, resetHandler);

    EventLoop loop;
    InetAddress addr(argv[1], atoi(argv[2]));
    ChatServer server(&loop, addr, "ChatServer");
    server.start();
    loop.loop();
    return 0;
}