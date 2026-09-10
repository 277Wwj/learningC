#pragma once
#include<atomic>
#include"Poller.hpp"
#include"Trace.hpp"
using namespace std;
class EventLoop{
    public:
    EventLoop():running_(false){}
    // 转交给 Poller：把 Channel 注册进 epoll
    void updateChannel(Channel* ch){
        Trace __t("EventLoop::updateChannel");
        poller_.updateChannel(ch);
    }
    // 转交给 Poller：把 Channel 从 epoll 移除
    void removeChannel(Channel* ch){
        Trace __t("EventLoop::removeChannel");
        poller_.removeChannel(ch);
    }
    // 主循环：反复"等事件 → 分发到 Channel 的回调"
    void loop(){
        Trace __t("EventLoop::loop");
        running_=true;
        while(running_){
            auto ready=poller_.poll();
            for(auto &[ch,revents]:ready){
                ch->handleEvent(revents);

            }
        }
    }
    void quit(){
        running_=false;
    }
    private:
    Poller poller_;
    atomic<bool> running_;
};
