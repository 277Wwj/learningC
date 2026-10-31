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
        running_.store(true);      // 在这里初始化：允许"先 quit() 再 loop()"
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
    //
    // ⚠️ 这里【不再】写 running_ = true。
    //    原来那行会覆盖 quit() 提前设置的 false，导致"先 quit 再 loop"永远卡死
    //    （tests/a3_shutdown_semantics.cpp 用例1 就是抓这个）。
    //    现在 running_ 在构造函数里初始化为 true，loop() 只读不写。
    //
    // ⚠️ 退出前补一次 doPendingFunctors()。
    //    正常路径里每轮循环末尾都会跑，但 quit() 可能发生在任务入队之后、
    //    下一轮循环之前。不补这一下，quit 前投递的任务会被静默丢弃
    //    （tests/a3_shutdown_semantics.cpp 用例2）。
    void loop(){
        owner_.store(std::this_thread::get_id());

        while (running_) {
            auto ready = poller_.poll();
            for (auto& [ch, revents] : ready)
            ch->handleEvent(revents);
            doPendingFunctors();
            if (postDispatch_) postDispatch_();
        }
        // 收尾：把 quit() 之后残留的任务跑完，再退出
        doPendingFunctors();
    }
    void quit(){
        running_ = false;
        wakeup();
    }
    // 投递一个任务到 loop 线程执行。
    // 返回 false 表示【明确拒绝】：loop 已停止，任务不会执行。
    // 调用者必须检查返回值 —— 静默丢弃是"关闭前最后一次发送"丢失的根源
    // （见 tests/a3_shutdown_semantics.cpp 用例2）。
    bool runInLoop(Functor f){
        if (isInLoopThread()) {
            if (!f) return false;
            f();
            return true;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_.load())
                return false;          // 已停：拒绝，不留下永远跑不到的僵尸任务
            pending_.push_back(std::move(f));
        }
        wakeup();
        return true;
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
