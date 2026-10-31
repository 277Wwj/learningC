// A1 实验：quit() 从别的线程调用，能不能叫醒 loop？
//
// 编译：g++ -std=c++17 -I include tests/a1_quit_wakeup.cpp -o /tmp/a1 -pthread
// 运行：/tmp/a1
//
// 用例1：quit() 在 loop 运行中调用 → 应立刻唤醒，join 耗时 < 100 ms
// 用例2：quit() 先于 loop() 调用 → 也应立刻退出
//        （2026-10-31 A3 修复前，这里会因为 loop() 覆盖 running_ 而卡死）
#include "EventLoop.hpp"

#include <chrono>
#include <cstdio>
#include <thread>
#include <future>
#include <chrono>
using namespace std::chrono_literals;

int main()
{
    EventLoop loop;

    // loop 跑在子线程 —— 和线程池里创建 worker 一模一样
    std::thread t([&] { loop.loop(); });

    std::this_thread::sleep_for(200ms);   // 等它进入 epoll_wait 阻塞

    printf("主线程调用 quit()\n");
    fflush(stdout);
     auto t0 = std::chrono::steady_clock::now();
    loop.quit();

    printf("主线程等待 join()...\n");
    fflush(stdout);

    t.join();                              // 这一步能不能返回？
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
     printf("【用例1】join 耗时 %lld ms\n", (long long)ms);
    if (ms > 100) printf("【用例1】FAIL：唤醒太慢\n");
    else          printf("【用例1】PASS\n");              
    printf("join 返回，程序正常退出\n");


    // ---- 用例2：quit() 先于 loop()，应当立刻退出 ----
    //
    // 历史：这正是 a3 修复的那个缺陷。
    //   修复前：loop() 第一行 `running_ = true` 覆盖了 quit() 设的 false，
    //           线程永远阻塞在 epoll_wait，join 不会返回。
    //   修复后：running_ 在构造函数里初始化，loop() 只读不写，join 立刻返回。
    // 所以这个用例的断言已经**反过来**了：现在期望"能退出"。
    EventLoop b;
    b.quit();
    thread tb([&]{ b.loop(); });
    // 用 async 只负责"等 1 秒"，不负责 join —— 否则会和主线程重复 join 同一条线程
    auto fut = async(launch::async, [&]{ return tb.joinable() ? tb.join(), true : true; });
    if (fut.wait_for(1s) == future_status::ready){
          printf("【用例2】PASS：quit() 生效，loop() 立即退出\n");
          fflush(stdout);
          fut.get();          // async 里已经 join 过了
    }
      else
        {  printf("【用例2】FAIL：quit() 无效，loop() 卡在 epoll_wait（running_ 被覆盖）\n");
            fflush(stdout);
          _exit(1);
                // 进程直接结束，避免卡死
        }
    return 0;
}
