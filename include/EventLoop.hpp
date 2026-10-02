#pragma once
#include<atomic>
#include<functional>
#include"Poller.hpp"
#include"Trace.hpp"
using namespace std;
class EventLoop{
    public:
    using Functor=function<void()>;   // "一段待执行的代码"
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
    // 注册"每一轮事件分发完之后"要做的收尾工作。
    //
    // 为什么需要这个？看下面的 loop()：ready 里存的是一批裸指针。如果
    // A 的回调执行期间把 B 的连接关掉并 delete 了对象，而 B 也在这一批里，
    // 循环走到 B 时就是 use-after-free。
    // 有了这个"安全点"，回调里只需把待关闭的连连接挂起来，等这一轮全部分发
    // 完毕、所有回调都已返回，再统一销毁对象 —— 一定安全。
    void setPostDispatch(Functor f){
        postDispatch_=move(f);
    }
    // 主循环：反复"等事件 → 分发到 Channel 的回调"
    void loop(){
        running_=true;
        while(running_){
            auto ready=poller_.poll();
            for(auto &[ch,revents]:ready){
                ch->handleEvent(revents);

            }
            // 安全点：本轮事件都已处理完，现在销毁对象不会碰到别人的栈帧
            if(postDispatch_) postDispatch_();
        }
    }
    void quit(){
        running_=false;
    }
    private:
    Poller poller_;
    atomic<bool> running_;
    Functor postDispatch_;
};
