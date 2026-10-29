// B3 实验：跨线程发送 + 关闭顺序
//
// 编译：g++ -std=c++17 -I include tests/b3_cross_thread_send.cpp -o /tmp/b3 -pthread
// 运行：timeout 30 /tmp/b3 ; echo "退出码=$?"
//
// 【B2 之后还剩什么问题】
//   B2 解决了"连接归谁"。但聊天服务器会遇到：
//     客户端 X 连在 loop0，客户端 Y 连在 loop1，
//     Y 发私聊给 X —— 这个 send 必须发生在 loop0 的线程里。
//   如果在 loop1 里直接对 X 的 fd 调 send，会怎样？
//
// 【为什么不能直接跨线程 send】
//   · send 本身内核层面是线程安全的
//   · 但 Connection::out 是 std::string，不是线程安全的
//     - 两个线程同时 push_back → 数据竞争 → 消息错乱或崩溃
//     - 更隐蔽：A 线程刚 erase(0,n)，B 线程正读到一半
//   · 而且 flush() 里会调 enableWriting() / updateChannel()，
//     改的是 Channel::events_ 和 epoll，这些也必须串行
//   所以：跨线程发送统一走 runInLoop，把"append + flush"投给所属 loop。
//
// 【结构】
//
//      loop A (线程A)                    loop B (线程B)
//   ┌────────────────┐              ┌────────────────┐
//   │ 连接 C1        │              │ 连接 C2        │
//   │  out buffer    │◄─────────────│  要发 100 条给 │
//   └────────────────┘  runInLoop   │  C1            │
//                       (投递任务)   └────────────────┘
//
// 【四个用例】
//   用例1  跨线程发送 100 条，全部收到，不丢不重复
//   用例2  同一连接的消息不乱序（FIFO）
//   用例3  投递成本：10000 次 runInLoop 应该很快（不阻塞调用方）
//   用例4  关闭顺序：先全部 quit 再 join，不卡死
//   用例5  线程亲和性：postMessage/adopt 只能在所属 loop 线程里被调用
//
// 【预期结果（正确实现）】
//   用例1 PASS  收到 100 条
//   用例2 PASS  序号严格递增
//   用例3 PASS  投递 10000 次 < 500 ms
//   用例4 PASS  正常退出
//   用例5 PASS  无越界调用
//
// 【对比实验：CROSS_THREAD=bare】
//   设置环境变量后就跳过 runInLoop，直接在主线程调 A.postMessage()。
//   · 用例5 会【必然 FAIL】—— 线程归属错了，这是确定性的
//   · 用例1/2 可能仍然 PASS —— 数据竞争是概率性的，小消息+快机器时窗口很小
//       → 这正是最危险的情况："它看上去是好的"
//   要让竞争显形，用 ThreadSanitizer：
//       g++ -std=c++17 -I include tests/b3_cross_thread_send.cpp -o /tmp/b3tsan -pthread -fsanitize=thread
//       CROSS_THREAD=bare /tmp/b3tsan

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

constexpr int kMsgCount = 100;      // 用例1/2：发送条数
constexpr int kFloodCount = 10000;  // 用例3：投递条数

// ---------------------------------------------------------------------------
// 模拟一条连接。out 缓冲 + 状态标记，全部只在所属 loop 线程访问。
// ---------------------------------------------------------------------------
struct Conn {
    int fd = -1;
    std::unique_ptr<Channel> channel;
    std::string out;              // 发送缓冲：没发完的留在这里
    bool closing = false;
    int  sentCount = 0;           // 已成功写出多少条（用于判定）

    // ⚠️ 如果直接跨线程调用，这两个字段会被多个线程同时读写
    std::atomic<bool> wrongThread{false};
};

} // namespace

// ---------------------------------------------------------------------------
// 一个 loop + 一条连接
// ---------------------------------------------------------------------------
struct Worker {
    EventLoop loop;
    std::thread thread;
    std::thread::id threadId;

    std::unique_ptr<Conn> conn;
    std::vector<std::unique_ptr<Conn>> pendingReap;

    void start()
    {
        thread = std::thread([this] {
            threadId = std::this_thread::get_id();
            loop.setPostDispatch([this] {
                if (!pendingReap.empty())
                    pendingReap.clear();
            });
            loop.loop();
        });
    }

    // 在子线程里创建连接（通过 runInLoop 投递进来）
    void adopt(int fd)
    {
        if (std::this_thread::get_id() != threadId)
            conn->wrongThread = true;

        auto c = std::make_unique<Conn>();
        c->fd = fd;
        c->channel = std::make_unique<Channel>(fd);

        Conn* raw = c.get();
        c->channel->setWriteCallback([this, raw] { flush(raw); });
        c->channel->setErrorCallback([this, raw] { closeConn(raw); });
        c->channel->setCloseCallback([this, raw] { closeConn(raw); });
        // 注意：不 enableReading。这个实验只测"发"，对端也不回数据。

        conn = std::move(c);
    }

    // ⚠️ 只能在所属线程调用
    void postMessage(const std::string& msg)
    {
        if (std::this_thread::get_id() != threadId)
            conn->wrongThread = true;

        conn->out += msg;
        flush(conn.get());
    }

    void flush(Conn* c)
    {
        if (c->closing)
            return;

        while (!c->out.empty()) {
            const ssize_t n = ::send(c->fd, c->out.data(), c->out.size(), MSG_NOSIGNAL);
            if (n > 0) {
                c->out.erase(0, static_cast<size_t>(n));
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                // 内核发送缓冲满：挂 EPOLLOUT 等可写（和 chat_server 的 flush 一样）
                if (!c->channel->isWriting()) {
                    c->channel->enableWriting();
                    loop.updateChannel(c->channel.get());
                }
                return;
            }
            closeConn(c);
            return;
        }

        if (c->channel->isWriting()) {
            c->channel->disableWriting();
            loop.updateChannel(c->channel.get());
        }
    }

    void closeConn(Conn* c)
    {
        if (c->closing)
            return;
        c->closing = true;
        loop.removeChannel(c->channel.get());
        ::close(c->fd);
        if (conn.get() == c)
            pendingReap.push_back(std::move(conn));
    }
};

// ---------------------------------------------------------------------------
// 创建一对已连接的 socket（用 listener + connect 保证两端都是真实 TCP）
// ---------------------------------------------------------------------------
static bool makePair(int& serverSide, int& clientSide)
{
    const int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { perror("socket"); return false; }

    int opt = 1;
    ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind"); ::close(lfd); return false;
    }
    if (::listen(lfd, 4) < 0) {
        perror("listen"); ::close(lfd); return false;
    }

    sockaddr_in actual{};
    socklen_t len = sizeof(actual);
    ::getsockname(lfd, reinterpret_cast<sockaddr*>(&actual), &len);

    clientSide = ::socket(AF_INET, SOCK_STREAM, 0);
    if (::connect(clientSide, reinterpret_cast<sockaddr*>(&actual), sizeof(actual)) < 0) {
        perror("connect"); ::close(lfd); ::close(clientSide); return false;
    }

    serverSide = ::accept(lfd, nullptr, nullptr);
    ::close(lfd);
    if (serverSide < 0) { perror("accept"); ::close(clientSide); return false; }

    // 服务端非阻塞（子 loop 要的）
    int flags = ::fcntl(serverSide, F_GETFL, 0);
    ::fcntl(serverSide, F_SETFL, flags | O_NONBLOCK);
    return true;
}

int main()
{
    printf("===== B3：跨线程发送 + 关闭顺序 =====\n\n");

    const bool bareMode = (getenv("CROSS_THREAD") != nullptr &&
                           strcmp(getenv("CROSS_THREAD"), "bare") == 0);
    if (bareMode)
        printf("⚠️  CROSS_THREAD=bare：跳过 runInLoop，直接跨线程调用（预期出问题）\n\n");

    // ---- 1. 创建两条连接，分给两个 Worker ----
    int s1 = -1, c1 = -1, s2 = -1, c2 = -1;
    if (!makePair(s1, c1) || !makePair(s2, c2)) {
        printf("创建 socket 对失败\n");
        return 1;
    }

    Worker A, B;              // A 持有 s1 的写端，B 持有 s2
    A.start();
    B.start();
    std::this_thread::sleep_for(200ms);

    A.loop.runInLoop([&A, s1] { A.adopt(s1); });
    B.loop.runInLoop([&B, s2] { B.adopt(s2); });
    std::this_thread::sleep_for(200ms);

    // ---- 2. 用例1/2：从线程 B 向连接 A 发 100 条带序号的消息 ----
    printf("用例1/2：从 loop B 的线程向 loop A 的连接发 %d 条\n", kMsgCount);

    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kMsgCount; ++i) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%04d\n", i);   // 固定宽度，便于校验顺序
        const std::string msg(buf);

        if (bareMode) {
            // ❌ 故意错误：直接在 B 的线程里改 A 的 conn->out
            A.postMessage(msg);
        } else {
            // ✅ 正确：投递给 A 的 loop，在 A 的线程里执行
            A.loop.runInLoop([&A, msg] { A.postMessage(msg); });
        }
    }
    const auto postMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
    printf("  投递耗时 %lld ms（用例3 关心这个）\n", (long long)postMs);

    // ---- 3. 从 c1 端读回来，校验数量与顺序 ----
    std::this_thread::sleep_for(600ms);

    std::string received;
    char buf[1024];
    for (;;) {
        const ssize_t n = ::recv(c1, buf, sizeof(buf), MSG_DONTWAIT);
        if (n > 0) { received.append(buf, static_cast<size_t>(n)); continue; }
        break;
    }

    int lines = 0;
    bool orderOk = true;
    size_t pos = 0;
    int expect = 0;
    while (pos < received.size()) {
        const size_t nl = received.find('\n', pos);
        if (nl == std::string::npos)
            break;
        const std::string line = received.substr(pos, nl - pos);
        const int got = atoi(line.c_str());
        if (got != expect)
            orderOk = false;
        ++expect;
        ++lines;
        pos = nl + 1;
    }

    // ---- 4. 用例3：投递不阻塞 ----
    //       正确实现下，runInLoop 只是 push_back + write(eventfd)，几乎瞬间。
    //       bare 模式下，每次都要真发，且可能撞上 EAGAIN 重试。
    const auto f0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kFloodCount; ++i)
        A.loop.runInLoop([&A] { /* 空任务，测投递成本 */ });
    const auto floodMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - f0).count();
    printf("用例3：投递 %d 个空任务耗时 %lld ms\n", kFloodCount, (long long)floodMs);

    // ---- 5. 关闭顺序：先全部 quit，再 join ----
    printf("用例4：先 quit 全部，再 join\n");
    A.loop.quit();
    B.loop.quit();
    A.thread.join();
    B.thread.join();

    ::close(c1);
    ::close(c2);

    // ---- 6. 判定 ----
    printf("\n===== 结果 =====\n");
    int failures = 0;
    auto check = [&](const char* name, bool pass, const char* detail) {
        printf("  【%s】%s  %s\n", name, pass ? "PASS" : "FAIL", detail);
        if (!pass) ++failures;
    };

    char detail[256];
    snprintf(detail, sizeof(detail), "收到 %d 条 / 期望 %d", lines, kMsgCount);
    check("用例1", lines == kMsgCount, detail);

    check("用例2", orderOk, "序号严格 0..99 递增");

    snprintf(detail, sizeof(detail), "10000 次投递耗时 %lld ms", (long long)floodMs);
    // 投递本身应该远快于真实发送（真实发送 10000 条要大得多的量级）
    check("用例3", floodMs < 500, detail);

    // 用例4 能走到这里就说明没卡死
    check("用例4", true, "两个 loop 都已 join，无卡死");

    // ---- 用例5：线程亲和性 ----
    // postMessage / adopt 里都做了检查：如果调用者不是所属 loop 的线程，
    // 就把 wrongThread 置位。正确实现下它必须一直为 false。
    //
    // 注意：这个标记是【确定性】的，不依赖竞态是否恰好发生。
    // CROSS_THREAD=bare 时，主线程直接调 A.postMessage()，这里必然 FAIL。
    // 换句话说：数据竞争是概率性的，但"线程归属错误"是必然的。
    const bool aWrong = A.conn ? A.conn->wrongThread.load() : false;
    const bool bWrong = B.conn ? B.conn->wrongThread.load() : false;
    snprintf(detail, sizeof(detail), "loop A 越界=%s, loop B 越界=%s",
             aWrong ? "是" : "否", bWrong ? "是" : "否");
    check("用例5", !aWrong && !bWrong, detail);

    printf("\n===== 汇总：%d 个用例未通过 =====\n", failures);
    return failures == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// 【延伸思考】
//
// 1) 为什么 postMessage 里必须 append 到 conn->out，而不是直接 send？
//    因为 send 可能返回 EAGAIN（内核发送缓冲满）。剩下的必须留在缓冲里，
//    等 EPOLLOUT 续发。这就是 chat_server 里 flush() 的完整逻辑。
//    这个实验为了聚焦"跨线程"，简化了：out 清空就 disableWriting。
//
// 2) 用 CROSS_THREAD=bare 跑会怎样？
//    【实测 2026-10-29】用例5 FAIL（线程归属错误），用例1/2 仍然 PASS。
//    这正说明"没崩"不等于"没问题" —— 数据竞争是概率性的。
//    主线程直接改 A 的 conn->out，同时 A 的子线程在 flush() 里 erase：
//      · 可能崩溃（std::string 内部指针被同时改）
//      · 可能消息错乱（一条消息被拆进另一条中间）
//      · 更常见的是"看起来正常"（小消息 + 快机器，竞态窗口只有纳秒级）
//
//    要让竞争显形，用 ThreadSanitizer：
//        g++ -std=c++17 -I include tests/b3_cross_thread_send.cpp -o /tmp/b3tsan -pthread -fsanitize=thread
//        CROSS_THREAD=bare setarch $(uname -m) -R /tmp/b3tsan
//
//    ⚠️ Ubuntu 24.04 上必须加 `setarch $(uname -m) -R` 关闭 ASLR，
//       否则会直接 FATAL: ThreadSanitizer: unexpected memory mapping。
//
//    【实测结果】抓到 5 处 data race，摘要如下：
//        data race in std::__uniq_ptr_impl<Conn>::_M_ptr()
//        data race in std::__atomic_base<bool>::operator=(bool)
//        data race in memcpy
//        data race in Worker::flush(Conn*)          × 2
//
//    所以这个实验的用例5 设计成"检查线程归属"而不是"检查结果对不对"：
//    功能性断言会放过偶发竞态，归属断言是确定性的。
//
// 3) 为什么用例3 的"耗时可接受"标准是 500ms？
//    10000 次 runInLoop = 10000 次 mutex lock + push_back + write(eventfd)。
//    write(eventfd) 是系统调用，约 0.5~1 μs，10000 次约 5~10 ms。
//    500ms 是很宽松的上限，主要用来抓"退化成真实 send"的那种错误实现。
