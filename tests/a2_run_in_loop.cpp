// A2 实验：验证 runInLoop 的三个语义
//   1) 跨线程投递 100 个任务，一个都不能丢
//   2) 所有任务都在 loop 所在线程执行
//   3) 在 loop 线程里再调 runInLoop，应当【直接执行】（同线程快路径）
//
// 编译：g++ -std=c++17 -I include tests/a2_run_in_loop.cpp -o /tmp/a2 -pthread
#include "EventLoop.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace std::chrono_literals;

int main()
{
    EventLoop loop;
    std::thread t([&] { loop.loop(); });
    std::this_thread::sleep_for(200ms);          // 等它进入 epoll_wait

    const std::thread::id loopId = t.get_id();

    std::atomic<int>  counter{0};
    std::atomic<int>  counter2{0};
    std::atomic<bool> wrongThread{false};
    std::atomic<bool> nestedRan{false};
    std::atomic<bool> nestedWasDirect{false};

    // ---- 1) 跨线程：主线程投递 100 个任务 ----
    for (int i = 0; i < 100; ++i) {
        loop.runInLoop([&] {
            ++counter;
            if (std::this_thread::get_id() != loopId)
                wrongThread = true;
        });
    }
    std::this_thread::sleep_for(500ms);
    

    // ---- 2) 同线程：在外层任务里再投递一个 ----
    //     外层任务是在 loop 线程里执行的，所以里层应该【直接执行】，
    //     判断依据：nestedRan 在外层任务返回前就已经为 true。
    loop.runInLoop([&] {
        loop.runInLoop([&] { nestedRan = true; });
        nestedWasDirect = nestedRan.load();      // 如果入队了，这里还是 false
    });

    std::this_thread::sleep_for(500ms);          // 给循环时间把任务跑完

    loop.quit();
    t.join();
    const bool pass = (counter.load() == 100) && wrongThread.load() == false && nestedWasDirect.load();
    printf("【用例1】执行任务数        : %d（期望 100）\n", counter.load());
    printf("【用例1】是否跑在错误线程  : %s（期望 否）\n", wrongThread.load() ? "是" : "否");
    printf("【用例1】嵌套任务是否直接执行: %s（期望 是）\n", nestedWasDirect.load() ? "是" : "否");
    printf("【用例1】%s\n", pass ? "A2 PASS" : "A2 FAIL");
    printf("\n");
    printf("\n");


    wrongThread=false;
  
    EventLoop b;
    thread tb([&]{ b.loop(); });
    const std::thread::id loopId2 = tb.get_id();
    std::this_thread::sleep_for(200ms); 
    for (int i = 0; i < 10000; ++i) {
        b.runInLoop([&] {
            ++counter2;
            if (std::this_thread::get_id() != loopId2)
                wrongThread = true;
        });
    }
     
    b.quit();
    tb.join();

    
    

    const bool pass2 = (counter2.load() == 10000) && wrongThread.load() == false;
    printf("【用例2】执行任务数: %d / 10000（观察项，不一定是 10000）\n", counter2.load());
    printf("【用例2】是否跑在错误线程: %s（期望 否）\n", wrongThread.load() ? "是" : "否");
    printf("【用例2】%s\n", pass2 ? "PASS：quit 未丢弃已入队的任务"
                                 : "观察：有任务没执行完，见下面分析");
    return (pass && pass2) ? 0 : 1;
}
