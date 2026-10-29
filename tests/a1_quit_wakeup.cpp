// A1 实验：quit() 从别的线程调用，能不能叫醒 loop？
//
// 编译：g++ -std=c++17 -I include tests/a1_quit_wakeup.cpp -o /tmp/a1 -pthread
// 运行：/tmp/a1
//
// 预期（正常版）：打印"join 返回"，程序退出
// 预期（去掉 wakeup 的版本）：卡在 join，只能 Ctrl+C
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


    EventLoop b;
    b.quit();
     thread tb([&]{ b.loop(); });  
    auto fut = async(launch::async, [&]{ tb.join(); }); 
    if (fut.wait_for(1s) == future_status::ready){
          printf("用例2】FAIL：意外退出了\n");
          fflush(stdout);
    }
      else
        {  printf("【用例2】PASS：1 秒内未退出（已知缺陷：loop() 覆盖 running_）\n");
            fflush(stdout);
          _exit(0);  
                // 进程直接结束，避免卡死   
        }  
    return 0;
}
