// 单 Reactor + 线程池版：主线程做 IO，线程池做业务
#include <bits/stdc++.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include "Threadpool.hpp"
using namespace std;
void set_nonblocking (int fd){
    int flags=fcntl(fd,F_GETFL,0);
    fcntl(fd,F_SETFL,flags|O_NONBLOCK);
}
string make_response(int code ,const string & reason ,const string &body){
    string resp;
    resp += "HTTP/1.1 " + to_string(code) + " " + reason + "\r\n";
    resp += "Content-Length: " + to_string(body.size()) + "\r\n";
    resp += "Connection: close\r\n";   // 简化：短连接，响应完就关
    resp += "\r\n";
    resp += body;
    return resp;
}
void process_request(int fd,string request){
    size_t pos=request .find("\r\n\r\n");
    if(pos==string::npos)return;
    string head = request.substr(0, pos);
    size_t lineEnd = head.find("\r\n");
    stringstream ss(head.substr(0, lineEnd));
    string method, path, version;
    ss >> method >> path >> version;
    cout << "[线程 " << this_thread::get_id() << "] 处理请求: " << path << endl;
    string body;
    string reason="OK";
    int code=200;
    if(path=="/slow"){
    this_thread::sleep_for(chrono::seconds(2));
    body="<h1>慢任务处理完成</h1>";
    }else if(path=="/"){
        body ="<h1>慢任务处理完成</h1>";
    }else{
        code=404;
        reason ="Not Found";
        body = "<h1>404 Not Found</h1>";
    }
    string resp=make_response(code,reason,body);
    send(fd,resp.data(),resp.size(),0);
    close(fd);

}
int main(){
    int listen_fd=socket(AF_INET,SOCK_STREAM,0);
    //socket()创建套接字拿到一个fd，没有连接任何东西；


    int opt=1;//开启顺序写入的设置REUSEADDR的变量
    setsockopt(listen_fd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));//开启设置；
    //SO_REUSEADDR的真实作用是在于一个处于time_wait状态的端口，进行ctrl+C关闭的时候，可以直接重启在绑定同一个8080的端口，不用等60秒；

    sockaddr_in addr{};//开始设置套接字的端口等信息；
    addr.sin_family=AF_INET;//使用IPV4地址
    addr.sin_addr.s_addr = INADDR_ANY;   // ← 你漏了这行！它才是"任何地址都能连"
    addr.sin_port=htons(8080);//监听的端口
    bind(listen_fd,(sockaddr*)&addr,sizeof(addr));//将套接字和端口信息融合在一起；
//bind()是将套接字和端口还有地址进行绑定

    listen(listen_fd,10);//首先不是最多连链接10个
    //他的第二个参数是blacklog意思是————还没有被accept的，排队等待的连接最多有10个


    set_nonblocking(listen_fd);//设置非阻塞模式；

    int epfd=epoll_create1(0);//创建一个io多路复用的“监视中心”和线程无关
    //管事件
    epoll_event ev{};//开始设置epoll信息；
    ev.events=EPOLLIN|EPOLLET;
    ev.data.fd=listen_fd;
    epoll_ctl(epfd,EPOLL_CTL_ADD,listen_fd,&ev);
    Threadpool pool(4);//创建线程池，4个worker
    epoll_event events[1024];  //事件存储最多1024；
    printf("多线程 WebServer 启动！\n");
    printf("测试：开两个终端，一个访问 /slow，一个访问 /\n");
    while (true){
        int n=epoll_wait(epfd,events,1024,-1);//在这里一直阻塞事件,并且获取事件数量；
        for(int i=0;i<n;i++){//开始便利事件；
            int fd=events[i].data.fd;//获取事件的fd，文件描述符；

            if(fd==listen_fd){//如果这个fd等于刚才新连接的客户端的监听的fd
                while(true){//循环查看连接
                    int c=accept(listen_fd,nullptr,nullptr);
                    //这里不是进行连接，这里是在已经连接的的队列里取出一个连接；
                    if(c<0){
                        if(errno==EAGAIN||errno==EWOULDBLOCK)
                        break;
                        perror("accept");
                        break;   // ← 
                    }

                    set_nonblocking(c);//设置非阻塞;
                    epoll_event cev{};//把新连接的fd注册进epoll;
                    
                    cev.events = EPOLLIN | EPOLLET;
                    cev.data.fd = c;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, c, &cev);
                    printf("新连接 fd=%d\n", c);

                }
            }else{
                char buf[4096];//创建接受数据的临时变量，为什么不直接用string
                int r=recv(fd,buf,sizeof(buf),0);//将数据赋值给buf，并且返回一个int；
                if(r>0){//如果有数据
                    epoll_ctl(epfd,EPOLL_CTL_DEL,fd,nullptr);
                    pool.submit(process_request,fd,string (buf,r));//提交给线程让线程去完成任务；

                }
                else {
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                    close(fd);
                }
            }
        }
    }
return 0;

}