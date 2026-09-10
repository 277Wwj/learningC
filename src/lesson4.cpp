#include<bits/stdc++.h>
#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<unistd.h>
#include<errno.h>
#include<sys/epoll.h>
#include <fcntl.h>
using namespace std;
void set_nonblocking(int fd){
    int flags=fcntl(fd,F_GETFL,0);
    fcntl(fd,F_SETFL,flags|O_NONBLOCK);

}
int main(){
    int listen_fd=socket(AF_INET,SOCK_STREAM,0);
    int opt=1;
    setsockopt(listen_fd, SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
    sockaddr_in addr{};
    addr.sin_addr.s_addr=INADDR_ANY;
    addr.sin_port=htons(9091);
    addr.sin_family=AF_INET;
    bind(listen_fd,(sockaddr*)&addr,sizeof(addr));
    listen(listen_fd,10);
    int epfd=epoll_create1(0);
    struct epoll_event ev{};
    ev.events=EPOLLIN;
    ev.data.fd=listen_fd;
    epoll_ctl(epfd,EPOLL_CTL_ADD,listen_fd,&ev);
    struct epoll_event events[1024];
    cout<<"epoll服务器启动，监听端口9091...(单线程！ 线程id: "<<this_thread::get_id()<<")"<<endl;
    while(true){
        int n=epoll_wait(epfd,events,1024,-1);
        if(n<0){
            perror("epoll_wait错误");
            break;
        }
        for(int i=0;i<n;i++){
            int fd=events[i].data.fd;
            if(fd==listen_fd){
                int c=accept(listen_fd,nullptr,nullptr);
                set_nonblocking(c);
                struct epoll_event cev{};
                cev.events=EPOLLIN|EPOLLET;
                cev.data.fd=c;
                epoll_ctl(epfd,EPOLL_CTL_ADD,c,&cev);
                printf("新客户端连接 fd=%d\n,当前客户端数量: %d\n",c,n);

            }
            else
            {
                while(1){

                    char buf[1024];

                int r=recv(fd,buf,sizeof(buf)-1,0);
                if(r==0){
                    epoll_ctl(epfd,EPOLL_CTL_DEL,fd,nullptr);
                    close(fd);
                    printf("客户端断开连接 fd=%d,当前客户端数量: %d\n",fd,n-1);
                    break;
                }
                if(r>0)
                {
                    buf[r]='\0';
                    printf("收到客户端消息 fd=%d,消息:%s\n",fd,buf);
                    send(fd,buf,r,0);
                    continue;
                }
                if(errno==EAGAIN||errno==EWOULDBLOCK){
                    break;

                }
                epoll_ctl(epfd,EPOLL_CTL_DEL,fd,nullptr);
                close(fd);
                printf("recv错误 fd=%d,当前客户端数量: %d\n",fd,n-1);
                break;
                }
                
            }
        }
    }
    close(listen_fd);
    close(epfd);
    return 0;
}