#pragma once
#include <sys/epoll.h>
#include <unistd.h>
#include <vector>
#include <unordered_map>
#include <utility>
#include <stdexcept>
#include "Channel.hpp"
#include "Trace.hpp"
using namespace std;

class Poller{
    public:
    Poller(){
        epfd_=epoll_create1(0);
        if(epfd_<0){
            throw runtime_error("epoll_create1 失败");
        }
    }
    ~Poller(){
        if(epfd_>=0)
        close(epfd_);
    }
    // 把 Channel 注册/更新进 epoll：首次 ADD，之后 MOD
    void updateChannel(Channel* ch){
        Trace __t("Poller::updateChannel");
        int fd=ch->fd();
        struct epoll_event ev{};
        ev.events=ch->events();
        ev.data.ptr=ch;
        int op=channels_.count(fd)?EPOLL_CTL_MOD:EPOLL_CTL_ADD;
        if(epoll_ctl(epfd_,op,fd,&ev)<0){
            perror("epoll_ctl 失败！");
            return;

        }
        channels_[fd]=ch;
    }
    // 把 Channel 从 epoll 移除（连接断开时调用）
    void removeChannel(Channel *ch){
        Trace __t("Poller::removeChannel");
        int fd=ch->fd();
        epoll_ctl(epfd_,EPOLL_CTL_DEL,fd,nullptr);
        channels_.erase(fd);

    }
    // 阻塞等就绪事件，返回 (Channel*, 发生的事件) 列表
    vector<pair<Channel* ,int >>poll(int timeoutMs=-1){
        Trace __t("Poller::poll");
        vector<pair<Channel*,int >> ready;
        struct epoll_event events[1024];
        int n=epoll_wait(epfd_,events,1024,timeoutMs);
        if(n<0){
            perror("epoll_wait 失败");
            return ready;
        }
        for(int i=0;i<n;i++){
            Channel* ch = static_cast<Channel*>(events[i].data.ptr);
            int revents = events[i].events;   // ← 先拷贝出来，变成普通局部变量
            ready.emplace_back(ch, revents);
        }
        return ready;
    }
    private:
    int epfd_;
    unordered_map<int,Channel*>channels_;
};