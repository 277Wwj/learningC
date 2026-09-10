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