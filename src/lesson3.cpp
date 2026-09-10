#include<bits/stdc++.h>
#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<unistd.h>
#include<errno.h>
using namespace std;
int main(){
    int listen_fd=socket(AF_INET,SOCK_STREAM,0);
    int opt=1;
    setsockopt(listen_fd ,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family=AF_INET;
    addr.sin_addr.s_addr=INADDR_ANY;
    addr.sin_port=htons(9090);
    bind(listen_fd,(sockaddr*)&addr,sizeof(addr));
    listen(listen_fd,10);
   cout << "select服务器启动，监听端口9090...(单线程！ 线程id: "
     << this_thread::get_id() << ")" << endl;
    vector<int >clients;
    while(true){
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(listen_fd,&readfds);
        int maxfd=listen_fd;
        for(int c:clients){
            FD_SET(c,&readfds);
            if(c>maxfd)maxfd=c;
        }
    
    int n =select(maxfd+1,&readfds,nullptr,nullptr,nullptr);
    if(n<0){
        perror("select错误");
        break;
    }
    if(FD_ISSET(listen_fd,&readfds)){
        int c=accept(listen_fd,nullptr,nullptr);
        clients.push_back(c);
        printf("新客户端连接 fd=%d\n,当前客户端数量: %lu\n",c,clients.size());

    }
    for(auto it=clients.begin();it!=clients.end();){
        int c=*it;
        if(!FD_ISSET(c,&readfds)){
            ++it;
            continue;
        }
        char buf[1024];
        int r=recv(c,buf,sizeof(buf)-1,0);
        if(r<=0){
            close(c);
            it=clients.erase(it);
            printf("客户端断开连接 fd=%d,当前客户端数量: %lu\n",c,clients.size());
            continue;
        }
        else
        {
            buf[r]='\0';
            printf("收到客户端消息 fd=%d,消息:%s\n",c,buf);
            send(c,buf,r,0);
            ++it;
        }
        
    }

    }
    close(listen_fd);
    return 0;
}