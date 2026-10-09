#pragma once
#include<functional>
#include<sys/epoll.h>
#include"Trace.hpp"
using namespace std;
class Channel{
    public:
    using Callback=function<void()>;
    explicit Channel(int fd):fd_(fd),events_(0){}
    void setReadCallback(Callback cd){readCallback_=move(cd);}
    void setWriteCallback(Callback cd){writeCallback_=move(cd);}
    void setCloseCallback(Callback cb) { closeCallback_ = std::move(cb); }
    void setErrorCallback(Callback cb) { errorCallback_ = std::move(cb); }
    // 订阅"可读"事件：把 EPOLLIN 位加进 events_（真正生效需 Poller 同步给内核）
    void enableReading(){
        Trace __t("Channel::enableReading");
        events_|=EPOLLIN;
    }
    // 订阅"可写"事件：把 EPOLLOUT 位加进 events_（发送缓冲没发完时用它续发）
    // 和 enableReading 一样：只改内存里的 events_，真正生效要 loop.updateChannel() 同步
    void enableWriting(){
        Trace __t("Channel::enableWriting");
        events_|=EPOLLOUT;
    }
    // 取消"可写"订阅。⚠️ LT 模式下"可写"几乎一直成立，
    // 发完了必须摘掉，否则事件循环会被"可写"空转刷爆
    void disableWriting(){
        Trace __t("Channel::disableWriting");
        events_&=~EPOLLOUT;
    }
    // 当前是否订阅着"可写"（flush 里用来避免重复 epoll_ctl）
    bool isWriting()const{
        return (events_&EPOLLOUT)!=0;
    }
    // 事件分发：内核说这个 fd 发生了哪些事件(revents)，就调用对应回调
    void handleEvent(int revents){
        Trace __t("Channel::handleEvent");
        if((revents&EPOLLIN)&&readCallback_) readCallback_();
        if ((revents & EPOLLOUT) && writeCallback_) writeCallback_();
        if ((revents & EPOLLHUP) && closeCallback_) closeCallback_();
        if ((revents & EPOLLERR) && errorCallback_) errorCallback_();

    }
    int fd()const {return fd_;};
    int events()const{return events_;}
    private:
    int fd_;
    int events_;
    Callback readCallback_;   // 可读时调
    Callback writeCallback_;  // 可写时调
    Callback closeCallback_;  // 对端关闭时调
    Callback errorCallback_;  // 出错时调

};