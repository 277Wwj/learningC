// 主从 Reactor（简化版）：主线程 accept + 分配，N 个子线程各管一个 epoll
#include <bits/stdc++.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
using namespace std;
const int PORT=8080;
const int SUB_NUM=4;//子Reactor数量（=工作线程数量）
string make_response(const string &body){//构造回应
    string resp;
    resp += "HTTP/1.1 200 OK\r\n";
    resp += "Content-Length: " + to_string(body.size()) + "\r\n";
    resp += "Connection: close\r\n";
    resp += "\r\n";
    resp += body;
    return resp;
}
void set_nonblocking (int fd){//给这个事件设置非阻塞模式
    int flags=fcntl(fd,F_GETFL,0);
    fcntl(fd,F_SETFL,flags|O_NONBLOCK);

}

void sub_reactor_loop(int epfd,int id){
    epoll_event events[1024];//最多返回就绪事件1024
    while (true){
        int n=epoll_wait(epfd,events,1024,-1);//一直等待事件集合里的1024个事件的通知；
        //等待有事情发生
        for(int i=0; i<n;i++){
            int fd=events[i].data.fd;//获取就绪事件的文件描述符（为什么还要建立一个int呢直接获取不就行了）

            char buf[4096];//接受数据的数组，
            int r=recv(fd,buf,sizeof(buf),0);//将接受的数据给buf并且返回数据的长度；
            if(r>0)//有数据
            {
                string request(buf,r);//将buf转为string并且长度为n；
                size_t pos=request.find("\r\n\r\n");//分割请求报文的请求头和数据；
                if(pos==string ::npos){//如果这个fd里没有数据就查下一个；
                    close(fd);continue;

                }
                string head =request.substr(0,pos);
                size_t lineEnd=head.find("\r\n");
                stringstream ss(head.substr(0,lineEnd));//将第一行存进ss“水管”;
                string method,path,version;
                ss>>method>>path>>version;//根据“水管”里的空格进行分割然后分别赋值；
                printf("[子Reactor %d] 处理 %s\n", id, path.c_str());
                string resp=make_response("<h1>Hello</h1>");//这什么意思，作出回应报文吗；
                send(fd,resp.data(),resp.size(),0);//发送数据，在发送这个fd的数据，，，但是发送给谁了我不知道
                epoll_ctl(epfd,EPOLL_CTL_DEL,fd,nullptr);//注册进epoll,意思就是将这个有数据的fd加入监控列表呗；

                close(fd);

                


            }else{
                epoll_ctl(epfd,EPOLL_CTL_DEL,fd,nullptr);
                close(fd);
            }
            

        }
    }

}
int main(){
    int listen_fd=socket(AF_INET,SOCK_STREAM,0);//创建连接通道；
    int opt=1;
    setsockopt(listen_fd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
    //设置通信连接断开后可直接重连接，不会在等60s；
    sockaddr_in addr{};
    addr.sin_family=AF_INET;
    addr.sin_addr.s_addr=INADDR_ANY;
    addr.sin_port=htons(PORT);
    bind(listen_fd,(sockaddr*)&addr,sizeof(addr));
    listen(listen_fd,10);
    //创建通信监听，设置端口可复用，填IP/端口，开始监听，最多等待10个连接
    set_nonblocking(listen_fd);
    int main_epfd=epoll_create1(0);//创建一个epoll监视器；

    epoll_event ev{};
    ev.events=EPOLLIN|EPOLLET;
    ev.data.fd=listen_fd;
    //设置这个监视器的属性或者方法；
    epoll_ctl(main_epfd,EPOLL_CTL_ADD,listen_fd,&ev);//将listen_fd添加进main_epfd（epoll）进行监控

    //创建监视器后设置监视器信息，1监视listen.fd;
    vector<int>sub_epfds(SUB_NUM);
    vector<thread> sub_threads;
    for(int i=0;i<SUB_NUM;i++){
        sub_epfds[i]=epoll_create1(0);
        sub_threads.emplace_back(sub_reactor_loop,sub_epfds[i],i);
    }
    epoll_event events[1024];
    int next=0;
    printf("主从 Reactor 服务器启动！端口 %d，子 Reactor %d 个\n", PORT, SUB_NUM);
    while(true){
        int n=epoll_wait(main_epfd,events,1024,-1);//监听main_epfd 一直等待
        for(int i=0;i<n;i++){
            int fd=events[i].data.fd;
            if(fd!=listen_fd)continue;
            while(true){
                int c=accept(listen_fd,nullptr,nullptr);
                if(c<0){
                    if(errno==EAGAIN||errno==EWOULDBLOCK)
                    break;
                    break;
                }
                set_nonblocking(c);
                int idx=next%SUB_NUM;
                next++;
                epoll_event cev{};
                cev.events=EPOLLIN;
                cev.data.fd=c;
                epoll_ctl(sub_epfds[idx],EPOLL_CTL_ADD,c,&cev);
                //将这个新连接的fd添加进第idx下的epfds监控下
                printf("连接 fd=%d 分给子 Reactor %d\n", c, idx);



            }
        }

    }
return 0;
}
