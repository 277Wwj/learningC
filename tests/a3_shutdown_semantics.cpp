// A3 实验：EventLoop 关闭语义的三个缺口
//
// 编译：g++ -std=c++17 -I include tests/a3_shutdown_semantics.cpp -o /tmp/a3 -pthread
// 运行：timeout 30 /tmp/a3
//
// 【这个实验想证明什么】
//   A1 已经发现 loop() 会覆盖 quit() 设的 running_。A3 把退出路径上的
//   三个缺口摆到台面上，每个缺口都是一个独立用例，修好一个就 PASS 一个。
//
// 【三个缺口】
//   缺口1  loop() 里 `running_ = true` 无条件覆盖，quit() 提前调用会失效
//   缺口2  quit() 之后入队的任务，永远不会执行（pending_ 被丢弃）
//   缺口3  postDispatch_ 里投递的任务，可能赶上最后一轮而丢失
//
// 【预期结果（当前代码）】
//   用例1 FAIL  卡住 -> timeout 杀掉
//   用例2 FAIL  计数 < 100
//   用例3 FAIL  计数 != 1
//
// 【修复后预期】
//   用例1 PASS  立即退出
//   用例2 PASS  计数 == 100（或 runInLoop 在停止后返回 false 被明确拒绝）
//   用例3 PASS  计数 == 1

#include "EventLoop.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace std::chrono_literals;

namespace {

int g_failures = 0;

void report(const char* name, bool pass, const char* detail)
{
    printf("  【%s】%s  %s\n", name, pass ? "PASS" : "FAIL", detail);
    if (!pass)
        ++g_failures;
}

} // namespace

// ---------------------------------------------------------------------------
// 用例1：quit() 在 loop() 之前调用，应该立刻退出
//
// 当前代码：loop() 第一行 running_ = true，把 quit() 的 false 覆盖掉了
//          -> 线程永远阻塞在 epoll_wait
// ---------------------------------------------------------------------------
void case1_quitBeforeLoop()
{
    printf("用例1：quit() 先于 loop()\n");

    EventLoop loop;
    loop.quit();                      // 期望：之后的 loop() 立刻返回

    std::atomic<bool> exited{false};
    std::thread t([&] {
        loop.loop();
        exited = true;
    });

    // 500ms 内应该退出
    const auto deadline = std::chrono::steady_clock::now() + 500ms;
    while (!exited.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);

    if (!exited.load()) {
        report("用例1", false, "loop() 未退出（running_ 被覆盖）");
        // 兜底：让线程能结束，否则整个实验卡死
        loop.quit();
        t.join();
        return;
    }

    t.join();
    report("用例1", true, "quit() 生效，loop() 立即返回");
}

// ---------------------------------------------------------------------------
// 用例2：quit() 之后入队的任务
//
// 期望语义：runInLoop 在 loop 已停止时应当【明确拒绝】（返回 false）
//          或者把任务执行完再退出。
// 当前代码：直接 push_back 进 pending_，然后没人跑 -> 静默丢失
// ---------------------------------------------------------------------------
void case2_enqueueAfterQuit()
{
    printf("用例2：quit() 之后入队 100 个任务\n");

    EventLoop loop;
    std::thread t([&] { loop.loop(); });
    std::this_thread::sleep_for(100ms);

    loop.quit();                      // 先停
    t.join();                         // 等线程真正退出

    std::atomic<int> ran{0};
    for (int i = 0; i < 100; ++i)
        loop.runInLoop([&] { ++ran; });

    std::this_thread::sleep_for(200ms);

    // 现在的 runInLoop 返回 void，无法感知"被拒绝"。
    // 所以判定标准是：要么执行了（100），要么明确拒绝。二者都没有才算失败。
    const int n = ran.load();
    char detail[128];
    snprintf(detail, sizeof(detail), "实际执行 %d / 100（0 表示静默丢失）", n);
    report("用例2", n == 100, detail);
}

// ---------------------------------------------------------------------------
// 用例3：postDispatch 里投递的任务
//
// postDispatch_ 在 doPendingFunctors() 之后执行。如果它投一个任务，
// 要等下一轮 doPendingFunctors。若此时 running_ 已为 false，任务就丢了。
// ---------------------------------------------------------------------------
void case3_postDispatchEnqueue()
{
    printf("用例3：postDispatch 里 runInLoop\n");

    EventLoop loop;
    std::atomic<int> ran{0};
    std::atomic<int> rounds{0};

    loop.setPostDispatch([&] {
        if (++rounds == 1) {
            // 第一轮安全点里投一个任务
            loop.runInLoop([&] { ++ran; });
            // 并且在这一轮结束时请求退出
            loop.quit();
        }
    });

    std::thread t([&] { loop.loop(); });
    std::this_thread::sleep_for(300ms);
    loop.quit();                      // 兜底
    t.join();

    char detail[128];
    snprintf(detail, sizeof(detail), "任务执行次数 = %d（期望 1）", ran.load());
    report("用例3", ran.load() == 1, detail);
}

int main()
{
    printf("===== A3：EventLoop 关闭语义 =====\n\n");

    case1_quitBeforeLoop();
    case2_enqueueAfterQuit();
    case3_postDispatchEnqueue();

    printf("\n===== 汇总：%d 个用例未通过 =====\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
