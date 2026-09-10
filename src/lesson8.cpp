#include <bits/stdc++.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/epoll.h>
#include "Channel.hpp"
#include "Poller.hpp"
using namespace std;
int main(){
    int fds[2];
    socketpair(AF_UNIX,SOCK_STREAM,0,fds);
    Channel ch(fds[0]);
    ch.setReadCallback([&]{
        char buf[64]={0};
        int r=recv(fds[0],buf,sizeof(buf)-1,0);
        printf("epoll 自动触发 可读回调读到：%s\n",buf);

    });
    ch.enableReading();
    Poller poller ;
    poller.updateChannel(&ch);
    const char*msg="hello poller";
    send(fds[1],msg,strlen(msg),0);
    auto ready =poller.poll();
    for(auto &[pch,recvents]:ready){
        pch->handleEvent(recvents);
    }
    close(fds[0]);
    close(fds[1]);
    return 0;
}