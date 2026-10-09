#include<unistd.h>
#include<fcntl.h>
#include<cstdio>
#include <sys/socket.h>   
#include <cerrno> 
using namespace std;
class FdGuard{
    public:
    explicit FdGuard(int fd) noexcept :fd_(fd){}
    ~FdGuard(){
        printf("析构开始执行\n");
        if(fd_>=0){
            ::close(fd_);
        }
    }
    FdGuard(const FdGuard&)=delete;
    FdGuard& operator=(const FdGuard&)=delete;
    FdGuard(FdGuard && other )noexcept : fd_(other.fd_){
        other.fd_=-1;
    }
    FdGuard& operator=(FdGuard&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) ::close(fd_);
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    int get() const noexcept { return fd_; }

private:
    int fd_;
};
int main(){
    int fds[2];
    socketpair(AF_UNIX,SOCK_STREAM,0,fds);
    {
        FdGuard g(fds[0]);
        printf("作用域内：fd 还开着\n");
    }
    errno=0;
    if(fcntl(fds[0],F_GETFD)==-1&&errno==EBADF){
        printf("真的关了");
    }
}