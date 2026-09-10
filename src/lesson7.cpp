#include <bits/stdc++.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/epoll.h>
#include "Channel.hpp"
using namespace std;
int main(){
    int fds[2];
    socketpair(AF_UNIX,SOCK_STREAM,0,fds);
    Channel ch(fds[0]);
    ch.setReadCallback([&]{
        char buf[64]={0};
        int r=recv(fds[0],buf,sizeof(buf)-1,0);
        printf("可读回调触发！读到: %s\n", buf);
    });
    const char *msg ="hello channel";
    send(fds[1],msg,strlen(msg),0);
    ch.handleEvent(EPOLLIN);
    close(fds[0]);
    close(fds[1]);
    return 0;

}