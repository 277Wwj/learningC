#include <bits/stdc++.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include "Buffer.hpp"
using namespace std;
void set_nonblocking(int fd){
    int flags=fcntl(fd,F_GETFL,0);
    fcntl(fd,F_SETFL,flags|O_NONBLOCK);

}
int main(){
    int fds[2];
    socketpair(AF_UNIX,SOCK_STREAM,0,fds);
    set_nonblocking(fds[0]);
    send(fds[1],"hello",5,0);
    send(fds[1],"world",5,0);
    Buffer buf;
    int n=buf.readFd(fds[0]);
    ptintf("读入：%d字节，Buffer里可读 %zu字节\n",n,buf.readableBytes());
    printf("peek: %s\n",buf.peek());
    buf.retrieve(5);
    printf("消费 5 字节后，剩余 %zu 字节: %s\n", buf.readableBytes(), buf.peek());

    close(fds[0]);
    close(fds[1]);
    return 0;
    
}