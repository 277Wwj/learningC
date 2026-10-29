// B1 实验：创建 N 个子 Reactor（先不接连接）
// 验证三件事：
//   1) N 个 EventLoop 能各自跑在自己的线程里（线程 id 各不相同）
//   2) runInLoop 能把任务精确送到指定的 loop
//   3) 关闭顺序正确：先 quit 全部 -> 再逐个 join（顺序反了会卡死/崩溃）
//
// 编译：g++ -std=c++17 -I include tests/b1_sub_loops.cpp -o /tmp/b1 -pthread
#include "EventLoop.hpp"

#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

int main()
{
    const int kSubLoops = 4;

    std::vector<std::unique_ptr<EventLoop>> loops;
    std::vector<std::thread>                threads;

    for (int i = 0; i < kSubLoops; ++i)
        loops.push_back(std::make_unique<EventLoop>());

    for (int i = 0; i < kSubLoops; ++i)
        threads.emplace_back([&loops, i] { loops[i]->loop(); });

    std::this_thread::sleep_for(300ms);      // 等所有子线程进入 epoll_wait
    printf("已启动 %d 个子 Reactor\n", kSubLoops);

    // 每个 loop 投一个任务，验证"任务落到了指定的 loop"
    for (int i = 0; i < kSubLoops; ++i) {
        loops[i]->runInLoop([i] {
            printf("  loop %d 执行任务，线程 id = %zu\n",
                   i, std::hash<std::thread::id>{}(std::this_thread::get_id()));
        });
    }

    std::this_thread::sleep_for(300ms);

    // ---- 关闭顺序：先全部 quit，再全部 join ----
    for (auto& l : loops)
        l->quit();

    for (auto& t : threads)
        t.join();

    printf("全部子线程已退出，程序结束\n");
    return 0;
}
