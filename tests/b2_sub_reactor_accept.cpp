// B2 实验：子 Reactor 接真实连接
//
// 编译：g++ -std=c++17 -I include tests/b2_sub_reactor_accept.cpp -o /tmp/b2 -pthread
// 运行：timeout 30 /tmp/b2 ; echo "退出码=$?"
//
// 【B1 留下的问题】
//   b1_sub_loops.cpp 的注释写着"先不接连接"——它只验证了 N 个 EventLoop 能各跑线程，
//   但没有任何 socket。真正的连接怎么交给子线程，是这一步要解决的。
//
// 【结构】
//
//         主线程                          4 个子线程
//   ┌──────────────┐
//   │ MainReactor  │  accept 出 fd
//   │ (listen fd)  │──────────┐
//   └──────────────┘          │ runInLoop 把"注册"动作投给目标 loop
//                             ├──────────► SubReactor 0 (epoll0)
//                             ├──────────► SubReactor 1 (epoll1)
//                             ├──────────► SubReactor 2 (epoll2)
//                             └──────────► SubReactor 3 (epoll3)
//
//   分发策略：idx = next++ % 4（严格轮询）
//
// 【为什么"注册"必须投递到目标线程】
//   epoll_ctl 本身线程安全，但 Connection 对象、Channel 的回调、生命周期管理
//   都在子 loop 的上下文里。在别的线程创建它们，后面 read 时就是数据竞争。
//   所以：主线程只管 accept，把 fd 交出去；对象的出生和死亡都在子线程。
//
// 【四个用例】
//   用例1  8 个客户端连上后，每个子 loop 分到 2 个连接
//   用例2  处理数据的线程 id 与所属 loop 的线程 id 一致
//   用例3  客户端连上后立刻发数据，全部被收到（验证跨线程注册不丢事件）
//   用例4  轮询公平：各 loop 连接数之差 <= 1
//
// 【预期结果（当前代码）】
//   用例1 PASS  4 个 loop 各 2 个连接
//   用例2 PASS  每个连接的处理线程 == 它所属 loop 的线程
//   用例3 PASS  8 条消息全部收到
//   用例4 PASS  最大 2 / 最小 2
//
// 【这个实验证明不了什么】
//   · 没验证跨线程"发送"（那是 B3）
//   · 没验证关闭流程（连接在 loop 退出后统一清理）
//   · 用的是 LT 模式。ET 模式下"数据先于注册到达"会丢，见文末注释。

#include "EventLoop.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

constexpr int kSubCount = 4;   // 子 Reactor 数量
constexpr int kClients  = 8;   // 客户端数量（必须是 kSubCount 的整数倍才能验证"平均"）
constexpr char  kMsg[]  = "hello";   // 每个客户端发的内容
constexpr int   kMsgLen = 5;

// ---------------------------------------------------------------------------
// 一条连接。只在所属子线程里被创建和访问，所以内部不加锁。
// ---------------------------------------------------------------------------
struct Conn {
    int fd = -1;
    std::unique_ptr<Channel> channel;
    bool closing = false;
};

} // namespace

// ---------------------------------------------------------------------------
// 子 Reactor：一个 EventLoop + 一个线程 + 它负责的所有连接
// ---------------------------------------------------------------------------
struct SubReactor {
    EventLoop loop;
    std::thread thread;
    std::thread::id threadId;

    // 下面这些都只在子线程里改，外部只在 join 之后读
    std::vector<std::unique_ptr<Conn>> conns;
    std::vector<std::unique_ptr<Conn>> pendingReap;   // 延迟回收（安全点统一销毁）

    // 供主线程读的统计（用 atomic）
    std::atomic<int>  accepted{0};
    std::atomic<int>  bytesRecv{0};
    std::atomic<bool> wrongThread{false};

    void start()
    {
        thread = std::thread([this] {
            threadId = std::this_thread::get_id();
            // 安全点：每轮事件都分发完后，销毁挂起的连接
            loop.setPostDispatch([this] {
                if (!pendingReap.empty())
                    pendingReap.clear();
            });
            loop.loop();
        });
    }

    // ⚠️ 必须在子线程里调用（通过 runInLoop 投递进来）
    void addConn(int fd, int index)
    {
        if (std::this_thread::get_id() != threadId)
            wrongThread = true;

        auto conn = std::make_unique<Conn>();
        conn->fd = fd;
        conn->channel = std::make_unique<Channel>(fd);

        Conn* raw = conn.get();
        conn->channel->setReadCallback([this, raw] { onReadable(raw); });
        conn->channel->setErrorCallback([this, raw] { closeConn(raw); });
        conn->channel->setCloseCallback([this, raw] { closeConn(raw); });
        conn->channel->enableReading();
        loop.updateChannel(conn->channel.get());

        conns.push_back(std::move(conn));
        ++accepted;
        printf("    [sub] loop #%d 收下 fd=%d（第 %d 个）\n",
               index, fd, accepted.load());
    }

    void onReadable(Conn* conn)
    {
        if (conn->closing)
            return;
        if (std::this_thread::get_id() != threadId)
            wrongThread = true;

        char buf[512];
        const ssize_t n = ::recv(conn->fd, buf, sizeof(buf), 0);
        if (n > 0) {
            bytesRecv += static_cast<int>(n);
            return;
        }
        if (n == 0) {          // 对端关闭
            closeConn(conn);
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        closeConn(conn);
    }

    void closeConn(Conn* conn)
    {
        if (conn->closing)
            return;            // 幂等
        conn->closing = true;

        loop.removeChannel(conn->channel.get());
        ::close(conn->fd);

        // 延迟回收：此刻可能正在 conn 自己的回调栈里
        for (auto it = conns.begin(); it != conns.end(); ++it) {
            if (it->get() == conn) {
                pendingReap.push_back(std::move(*it));
                conns.erase(it);
                break;
            }
        }
    }
};

// ---------------------------------------------------------------------------
// 监听 socket：非阻塞
// ---------------------------------------------------------------------------
static int makeListener(uint16_t* outPort)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;                 // 让内核分配端口，避免和别的实验撞车
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind"); ::close(fd); return -1;
    }
    if (::listen(fd, SOMAXCONN) < 0) {
        perror("listen"); ::close(fd); return -1;
    }

    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in actual{};
    socklen_t len = sizeof(actual);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&actual), &len);
    *outPort = ntohs(actual.sin_port);
    return fd;
}

int main()
{
    printf("===== B2：子 Reactor 接真实连接 =====\n\n");

    // ---- 1. 起 4 个子 Reactor ----
    auto subs = std::make_unique<SubReactor[]>(kSubCount);
    for (int i = 0; i < kSubCount; ++i)
        subs[i].start();
    std::this_thread::sleep_for(300ms);      // 等子线程进入 epoll_wait
    printf("已启动 %d 个子 Reactor\n", kSubCount);

    // ---- 2. 监听 socket，挂在主 Reactor 上 ----
    uint16_t port = 0;
    const int listenFd = makeListener(&port);
    if (listenFd < 0)
        return 1;
    printf("监听 127.0.0.1:%u\n\n", port);

    std::atomic<int> next{0};
    EventLoop mainLoop;
    Channel listenCh(listenFd);
    listenCh.setReadCallback([&] {
        while (true) {
            const int fd = ::accept(listenFd, nullptr, nullptr);
            if (fd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;   // 掏空了
                if (errno == EINTR) continue;
                perror("accept");
                break;
            }
            // 新 fd 不会继承 listenFd 的 O_NONBLOCK，必须自己设
            int flags = ::fcntl(fd, F_GETFL, 0);
            ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

            const int idx = next++ % kSubCount;      // 轮询分配
            printf("  [main] accept fd=%d -> loop #%d\n", fd, idx);

            // 关键：把"注册"这件事投递到目标 loop 的线程里执行
            subs[idx].loop.runInLoop([&subs, fd, idx] {
                subs[idx].addConn(fd, idx);
            });
        }
    });
    listenCh.enableReading();
    mainLoop.updateChannel(&listenCh);

    // ---- 3. 测试驱动线程：开 8 个客户端 ----
    std::thread driver([&] {
        std::this_thread::sleep_for(200ms);
        printf("\n[driver] 开始创建 %d 个客户端\n", kClients);

        for (int i = 0; i < kClients; ++i) {
            const int c = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons(port);

            if (::connect(c, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
                perror("connect");
                ::close(c);
                continue;
            }
            // 连上之后立刻发数据：验证"注册还没完成时数据就到了"这种竞态
            const ssize_t w = ::send(c, kMsg, kMsgLen, 0);
            (void)w;
            ::close(c);                     // 发完就关（B2 不验证半关闭语义）
            std::this_thread::sleep_for(60ms);
        }

        std::this_thread::sleep_for(800ms);  // 给子 loop 时间处理完所有事件

        // 关闭顺序：先全部 quit，再 join（B1 的教训）
        for (int i = 0; i < kSubCount; ++i)
            subs[i].loop.quit();
        mainLoop.quit();
    });

    // ---- 4. 主线程跑主 Reactor ----
    mainLoop.loop();

    // ---- 5. 收尾 ----
    for (int i = 0; i < kSubCount; ++i)
        subs[i].thread.join();
    driver.join();

    ::close(listenFd);

    // ---- 6. 判定 ----
    printf("\n===== 结果 =====\n");
    int totalAccepted = 0, totalBytes = 0;
    int minConn = 999999, maxConn = 0;
    bool threadOk = true;

    for (int i = 0; i < kSubCount; ++i) {
        const int a = subs[i].accepted.load();
        const int b = subs[i].bytesRecv.load();
        const bool wt = subs[i].wrongThread.load();
        printf("  loop #%d: 连接 %d 个, 收到 %d 字节, 线程错误=%s\n",
               i, a, b, wt ? "是" : "否");
        totalAccepted += a;
        totalBytes += b;
        if (a < minConn) minConn = a;
        if (a > maxConn) maxConn = a;
        if (wt) threadOk = false;
    }

    int failures = 0;
    auto check = [&](const char* name, bool pass, const char* detail) {
        printf("  【%s】%s  %s\n", name, pass ? "PASS" : "FAIL", detail);
        if (!pass) ++failures;
    };

    char detail[256];

    snprintf(detail, sizeof(detail), "连接总数 %d / %d", totalAccepted, kClients);
    check("用例1", totalAccepted == kClients, detail);

    check("用例2", threadOk, "所有回调都在所属 loop 的线程执行");

    snprintf(detail, sizeof(detail), "收到 %d 字节 / 期望 %d",
             totalBytes, kClients * kMsgLen);
    check("用例3", totalBytes == kClients * kMsgLen, detail);

    snprintf(detail, sizeof(detail), "最多 %d 个, 最少 %d 个", maxConn, minConn);
    check("用例4", (maxConn - minConn) <= 1, detail);

    printf("\n===== 汇总：%d 个用例未通过 =====\n", failures);
    return failures == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// 【延伸思考】
//
// 1) 如果 Channel 用的是 ET（边缘触发），用例 3 会怎样？
//    客户端连上就 send，数据可能先于 epoll_ctl 到达。LT 下注册后立刻报可读，
//    所以能读到；ET 下注册这个动作本身就是一次"状态变化"，理论上也会先报一次，
//    但如果注册晚于数据到达且中间没有新的边沿，就可能永久丢失。
//    结论：ET 模式下必须"注册完成后再让对端发数据"，或者注册后立刻主动读一次。
//
// 2) 为什么 addConn 里的 conns.push_back 不用加锁？
//    因为 addConn 只通过 runInLoop 在子线程执行，onReadable/closeConn 也在
//    子线程执行。整个 conns / pendingReap 都是"线程私有"的。
//    这正是主从 Reactor 的核心约定：**一个连接只属于一个 loop**。
//
// 3) 如果主线程直接 subs[idx].addConn(fd, idx)（不投递）会怎样？
//    会在主线程里创建 Channel、调 loop.updateChannel（改的是子 loop 的 epoll），
//    同时 push_back 到子线程的 vector。子线程正在遍历 conns 时会数据竞争。
//    把 wrongThread 改成在主线程里检查，就能观察到这个问题。
