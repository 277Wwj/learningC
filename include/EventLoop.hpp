#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include <sys/eventfd.h>
#include <unistd.h>

#include "Poller.hpp"      // 已经间接带了 Channel
#include "Trace.hpp"

using namespace std;
class EventLoop{
    public:
    using Functor=function<void()>;   // "一段待执行的代码"
    EventLoop(){
        wakeupFd_=::eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);
        if(wakeupFd_<0){
            throw runtime_error("eventfd 创建失败");

        }
        wakeupCh_=make_unique<Channel>(wakeupFd_);
        wakeupCh_->setReadCallback([this]{handleWakeup();});
        wakeupCh_->enableReading();
        poller_.updateChannel(wakeupCh_.get());
    }
    EventLoop(const EventLoop&) = delete;              // 事件循环不可复制
    EventLoop& operator=(const EventLoop&) = delete;
    ~EventLoop()
    {
        if (wakeupFd_ >= 0)
            ::close(wakeupFd_);
    }
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
        running_ = true;
        owner_.store(std::this_thread::get_id());

        while (running_) {
            auto ready = poller_.poll();
            for (auto& [ch, revents] : ready)
            ch->handleEvent(revents);
            doPendingFunctors();
            if (postDispatch_) postDispatch_();
        }
    }
    void quit(){
        running_ = false;
        wakeup();
    }
    void runInLoop(Functor f){
        if (isInLoopThread()) {
            f();
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_.push_back(std::move(f));
        }
        wakeup();
    }
    bool isInLoopThread() const {
        return owner_.load() == std::this_thread::get_id();
    }
    private:
    void wakeup(){
        const uint64_t one =1;
        ssize_t n=::write(wakeupFd_,&one,sizeof(one));
        (void)n;
    }
    void handleWakeup(){
        uint64_t value = 0;
        ssize_t n = ::read(wakeupFd_, &value, sizeof(value));
        (void)n;
    }
    void doPendingFunctors(){
            std::vector<Functor> locals;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                locals.swap(pending_);
            }
            for (auto& f : locals)
                if (f) f();
     }
    Poller poller_;
    atomic<bool> running_;
    int wakeupFd_ = -1;
    unique_ptr<Channel> wakeupCh_;
    std::mutex mutex_;
    std::vector<Functor> pending_;
    std::atomic<std::thread::id> owner_{std::thread::id{}};
    Functor postDispatch_;

};
