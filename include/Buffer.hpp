#pragma once
#include <vector>
#include <string>
#include <cstring>
#include <sys/socket.h>
#include <errno.h>
using namespace std;
class Buffer{
    public:
    // recv 的三种结局。用来说明"读完了"到底是因为哪种原因停下来的：
    //   Ok          正常：内核缓冲读空了（EAGAIN），连接还活着
    //   PeerClosed  对端关闭了写方向（收到 FIN）。注意：可能同时读到了数据
    //   Error       真错误（ECONNRESET 之类）。此时读到的数据不可信
    //
    // 为什么要把 PeerClosed 和 Error 分开？
    //   EOF 是"带着数据一起发生"的正常事件：对端 send 完就 close，
    //   数据先到、FIN 后到，这一次 recv 循环会先读到数据再撞上 n==0。
    //   如果把它和错误一样处理，刚读到的整帧就会被丢掉。
    enum class ReadStatus { Ok, PeerClosed, Error };

    // 带状态输出的版本。返回值始终是"这次真正读进缓冲的字节数"，
    // 即使返回 0 也要看 status 才知道连接死没死。
    //
    // savedErrno 可选：发生 Error 时把 errno 带出去，方便打日志区分原因。
    int readFd(int fd, ReadStatus& status, int* savedErrno = nullptr)
    {
        status = ReadStatus::Ok;
        int total = 0;
        char buf[4096];
        while(true){
            const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if(n > 0){
                append(buf, static_cast<size_t>(n));
                total += static_cast<int>(n);
                continue;
            }
            if(n == 0){
                // 对端 FIN。已经读到的字节要保留，所以这里 break 而不是 return -1
                status = ReadStatus::PeerClosed;
                return total;
            }
            if(errno == EAGAIN || errno == EWOULDBLOCK)
                break;                 // 读空了：这是正常的结束条件
            if(errno == EINTR)
                continue;              // 被信号打断，不是错误，重试
            if(savedErrno) *savedErrno = errno;
            status = ReadStatus::Error;
            return total;
        }
        return total;
    }

    // 兼容旧签名：读到的字节数，-1 表示"连接已经不可用"。
    // ⚠️ 这个版本分不清 EOF 和 Error，也拿不到"EOF 之前读到的数据"。
    //    新代码请用上面那个带 ReadStatus 的版本。
    int readFd(int fd){
        ReadStatus st;
        const int n = readFd(fd, st);
        return st == ReadStatus::Ok ? n : -1;
    }
    size_t readableBytes()const{
        return writeIndex_-readIndex_;
    }
    const char *peek()const{
        return data_.data()+readIndex_;
    }
    void retrieve(size_t n){
        if(n>=readableBytes()){
            readIndex_=writeIndex_=0;
            
        }
        else
        {
            readIndex_+=n;
        }
    }
    string retrieveAllAsString(){
        string s(peek(),readableBytes());
        retrieve(readableBytes());
        return s;
    }
    private:
    void append(const char* data,size_t len){
        if(data_.size()<writeIndex_+len){
            data_.resize(writeIndex_+len);

        }
        memcpy(data_.data()+writeIndex_,data,len);
        writeIndex_+=len;
    }
    vector<char>data_;
    size_t readIndex_=0;
    size_t writeIndex_=0;
};


