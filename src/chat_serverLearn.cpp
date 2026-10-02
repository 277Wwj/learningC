#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <unistd.h>

#include <ctime>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Buffer.hpp"
#include "Channel.hpp"
#include "EventLoop.hpp"
using namespace std;

constexpr uint16_t kPort = 12735;

#define LOG(fmt, ...) fprintf(stderr, "[chat] " fmt "\n", ##__VA_ARGS__)

void setNonBlocking(int fd){
    int flags=fcntl(fd,F_GETFL,0);
    fcntl(fd,F_SETFL,flags|O_NONBLOCK);

}
struct Connection{
    int fd=-1;
    unique_ptr<Channel>channel;
    Buffer in;
    string out;
    string nick;
    time_t lastActive=0;
    bool closing =false;

};
class ChatServer{
    public:
    bool start(uint16_t port){
        listenFd_=::socket(AF_INET,SOCK_STREAM,0);
        if(listenFd_<0){
            perror("socket");
            return false;
        }
        sockaddr_in addr{};
        addr.sin_family=AF_INET;
        addr.sin_addr.s_addr=htonl(INADDR_ANY);
        addr.sin_port=htons(port);
        if(::bind(listenFd_,(sockaddr*)&addr,sizeof(addr))<0){perror("bind");return false;}
        if(::listen(listenFd_,SOMAXCONN)<0){perror("listen");return false;}
        setNonBlocking (listenFd_);
        listenCh_=std::make_unique<Channel>(listenFd_);
        listenCh_->setReadCallback([this]{onAccept();});
        listenCh_->enableReading();
        loop_.updateChannel(listenCh_.get());
        loop_.setPostDispatch([this]{reapPending();});
        return true;

    };
    void run(){
        LOG("服务器启动，监听 0.0.0.0:%u", kPort);
        loop_.loop();
    };
    private:
    void onAccept(){
        while(true){
            int fd=::accept(listenFd_,nullptr,nullptr);
            if(fd<0){
                if(errno==EAGAIN||errno==EWOULDBLOCK)break;
                if(errno==EINTR)continue;
                perror("accept");
                break;
            }
            setNonBlocking(fd);
            auto conn       =make_unique<Connection>();
            conn->fd        =fd;
            conn->lastActive=time(nullptr);
            conn->channel   =make_unique<Channel>(fd);
            Connection *raw=conn .get();
            conn->channel->setReadCallback([this,raw]{onRead(raw);});

        }
    };
    void onRead(Connection* conn){
        if(conn->closing)return;
        int n=conn->in.readFd(conn->fd);
        if(n<0){
            closeConn(conn,"对端关闭");
            return;
        }
        LOG("[<] fd=%d 收到 %d 字节，缓冲区现有 %zu 字节", conn->fd, n, conn->in.readableBytes());
        
        while(true){
            uint32_t L=0;
            if(conn ->in.readableBytes()<4)
            break;
            memcpy(&L, conn->in.peek(), 4);  
            L = ntohl(L);
            if(L<8||L>64*1024){
                closeConn(conn);
                return;
            }
            if(conn->in.readableBytes()<4+L)
            break;
            uint32_t type = 0;
            string payload(conn->in.peek() + 8, L - 8); 
            memcpy(&type, conn->in.peek() + 4, 4); 
            type = ntohl(type);
            conn->in.retrieve(4 + L);  
            LOG("[<] fd=%d 收到一帧: type=%u payload=%s", conn->fd, type, payload.c_str());
        }
    };
    void closeConn(Connection* conn, const std::string& reason);
    void reapPending(){
        if (pendingReap_.empty())
        return;

        logLine("[reap] 安全回收 %zu 个连接对象（此刻所有回调均已返回）", pendingReap_.size());
        pendingReap_.clear();    // unique_ptr 析构 → Channel 析构
    };
    EventLoop loop_;
    int listenFd_ = -1;
    std::unique_ptr<Channel> listenCh_;

    std::unordered_map<int, std::unique_ptr<Connection>> conns_;  // fd → 连接
    std::vector<std::unique_ptr<Connection>> pendingReap_; 
};