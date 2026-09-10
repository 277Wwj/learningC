#include<bits/stdc++.h>
#include<fcntl.h>
#include<unistd.h>
#include<errno.h>
#include<sys/socket.h>
using namespace std;
void set_nonblocking(int fd){
    int flags=fcntl(fd,F_GETFL,0);
    fcntl(fd,F_SETFL,flags|O_NONBLOCK);

}
int main(){
    int fds[2];
    socketpair(AF_UNIX,SOCK_STREAM,0,fds);
    set_nonblocking(fds[0]);
    char buf[64];
    ssize_t n=recv(fds[0],buf,sizeof(buf)-1,0);
    if(n==-1&&(errno==EAGAIN ||errno==EWOULDBLOCK)){
        cout<<"->没有数据可读,立刻返回EAGAIN(没卡住！)"<<endl;
    }
    const char*msg="Hello,World!";
    send(fds[1],msg,strlen(msg),0);
    memset(buf,0,sizeof(buf));
    n=recv(fds[0],buf,sizeof(buf),0);
    printf("第二次 recv()返回:%zd,数据:%s\n",n,buf);
    close(fds[0]);
    close(fds[1]);
    return 0;

}