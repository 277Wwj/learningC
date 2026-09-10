#include <bits/stdc++.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include "EventLoop.hpp"
#include "Channel.hpp"
#include "Buffer.hpp"
#include <sys/timerfd.h>
#include<sys/eventfd.h>
int g_wakeupFd;
void onSigint(int){
    uint64_t one =1;
    write(g_wakeupFd,&one,sizeof(one));
}   
using namespace std;
void set_nonblocking(int fd){
    int flags=fcntl(fd,F_GETFL,0);
    fcntl(fd,F_SETFL,flags|O_NONBLOCK);

}
struct Connection{
    Channel *channel;
    Buffer input;
    time_t lastActive;
};


string makeResponse(int code ,const string &reason ,const string &body,bool keepAlive){
    string resp;
    resp += "HTTP/1.1 " + to_string(code) + " " + reason + "\r\n";
    resp += "Content-Type: text/html; charset=utf-8\r\n";
    resp += "Content-Length: " + to_string(body.size()) + "\r\n";
    resp += keepAlive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";   // 关键：不关闭，保持连接
    resp += "\r\n";
    resp += body;        // 正文
    return resp;
}
int main(){
    signal(SIGPIPE,SIG_IGN);
    int listen_fd=socket(AF_INET,SOCK_STREAM,0);
    int opt =1;
    setsockopt(listen_fd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
    sockaddr_in addr{};
    addr.sin_addr.s_addr=INADDR_ANY;
    addr.sin_family=AF_INET;
    addr.sin_port=htons(8080);
    bind(listen_fd,(sockaddr*)&addr,sizeof(addr));
    listen(listen_fd,10);
    set_nonblocking(listen_fd);
    EventLoop loop ;
    unordered_map<int ,Connection>conns;
    Channel listen_ch(listen_fd);
    listen_ch.setReadCallback([&]{
        while(true){
            int c=accept(listen_fd,nullptr,nullptr);
            
            if(c<0){
                if(errno==EAGAIN||errno==EWOULDBLOCK){
                    break;
                }
            }
            if (c < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;             // 没新连接了，正常退出
                }
                perror("accept");
                break;                 // 真错误，也退出
            }
            set_nonblocking(c);
            Connection conn;
            conn.lastActive=time(nullptr);
            conn.channel=new Channel(c);
            conn.channel->setReadCallback([&loop, &conns,c]{

                Connection& conn=conns[c];
                int n=conn.input.readFd(c);
                conn.lastActive=time(nullptr);
                while(true){
                    string all (conn.input.peek(),conn.input.readableBytes());
                    size_t pos=all.find("\r\n\r\n");
                    if(pos==string ::npos){
                        break;
                    }
                    string head=all.substr(0,pos);
                    size_t lineEnd=head.find("\r\n");
                    stringstream ss(head.substr(0,lineEnd));
                    string method,path,version;
                    ss>>method>>path>>version;
                    bool keepAlive = (version =="HTTP/1.1"); 
                    printf("收到请求：%s %s\n",method.c_str(),path.c_str());
                    string body;
                    int code=200;
                    string reason="OK";
                    if (path == "/") {
                        body = "<h1>首页</h1>";
                    } else if (path == "/hello") {
                        body = "<h1>Hello!</h1>";
                    } else {
                        code = 404;
                        reason = "Not Found";
                        body = "<h1>404 Not Found</h1>";
                    }
                    string resp = makeResponse(code, reason, body,keepAlive);
                    send(c, resp.data(), resp.size(), 0);
                    
                    // ⑤ 关键：只消费掉这一个请求（含 \r\n\r\n 共 4 字节）
                    conn.input.retrieve(pos + 4);
                    if(!keepAlive){
                        loop.removeChannel(conn.channel);
                        close(c);
                        conns.erase(c);
                        return;
                    }
                }
                if (n == -1) {
                    loop.removeChannel(conn.channel);
                    close(c);
                    conns.erase(c);
                    printf("fd=%d 断开\n", c);
                }
                
            });
            conn.channel->enableReading();
            loop.updateChannel(conn.channel);
            conns[c] = std::move(conn);
            printf("新连接 fd=%d\n", c);
        }

    });
    listen_ch.enableReading();
    loop.updateChannel(&listen_ch);

    printf("WebServer 启动！浏览器访问 http://127.0.0.1:8080/\n");
    int tfd=timerfd_create(CLOCK_MONOTONIC,0);
    struct itimerspec spec{};
    spec.it_value.tv_sec = 5;
    spec.it_interval.tv_sec = 5;
    timerfd_settime(tfd,0,&spec,nullptr);
    Channel timer_ch(tfd);
    timer_ch.setReadCallback([&loop,&conns,tfd]{
        uint64_t exp;
        read(tfd,&exp,sizeof(exp));
        time_t now=time(nullptr);
        const int TIMEOUT=10;
        for(auto it=conns.begin();it!=conns.end();){
            int fd =it->first;
            if(now-it->second.lastActive>TIMEOUT){
                loop.removeChannel(it->second.channel);
                close(fd);
                it=conns.erase(it);
                printf("连接 fd=%d 超时关闭\n", fd);

            }
            else
            {
                ++it;

            }
        }
    });
    timer_ch.enableReading();
    loop.updateChannel(&timer_ch);
    g_wakeupFd=eventfd(0,EFD_NONBLOCK);
    signal(SIGINT,onSigint);
    Channel wakeup_ch(g_wakeupFd);
    wakeup_ch.setReadCallback([&loop]{
        uint64_t one;
        read(g_wakeupFd,&one,sizeof(one));
        printf("\n收到退出信号，正在优雅关闭...\n");
        loop.quit();

    });
    wakeup_ch.enableReading();
    loop.updateChannel(&wakeup_ch);
    loop.loop();
    printf("服务器已优雅关闭，再见！\n");
    close(listen_fd);
    return 0;

}