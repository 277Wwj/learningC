// ===========================================================================
// chat_server.cpp —— 聊天中转服务器（第 1 ~ 6 课完成版）
//
// 【这个程序在干嘛】
//   原来 11_lan_chat 用 UDP 广播找人，只能在同一个网段里用。
//   现在所有客户端都连到这台服务器上，服务器帮它们互相转发消息。
//   于是两个人只要有网、都能连到这台服务器，就能聊天 —— 不再要求同网段。
//
// 【已完成】
//   第 1 课：监听 / accept / 连接生命周期管理（含"延迟回收"这个安全点）
//   第 2 课：TCP 粘包·半包处理（按 [4B长度][4B魔数][4B类型][payload] 切帧）
//   第 3 课：Login / Welcome / Users —— fd↔昵称 双索引 + 上下线名单广播
//   第 4 课：Say → Chat 转发（群发 / 私聊，靠昵称查表 O(1) 路由）
//   第 5 课：半写与 EPOLLOUT 续发（慢客户端不丢消息）+ 空闲连接超时回收
//   第 6 课：Bye 主动退出 + 收尾
//
//   逐函数讲解文档：C++后端/项目/聊天服务器-代码精读.md
//
// 【下一步】Qt 客户端接入 --server 模式（11_lan_chat 项目，路线图第 6 课）
//
// 【编译】用 VS Code 的 Ctrl+Shift+B 也行
//   g++ -std=c++17 -pthread -I include src/chat_server.cpp -o src/chat_server
// 【运行】
//   ./src/chat_server
// 【看事件循环调用链】默认关闭，想看时打开：
//   TRACE=1 ./src/chat_server
// 【冒烟测试】先跑起服务器，另开一个终端：python3 tests/chat_smoke_test.py
// ===========================================================================

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Buffer.hpp"
#include "Channel.hpp"
#include "EventLoop.hpp"

// ---------------------------------------------------------------------------
// 一、协议常量
//
//   +--------+--------+--------+-----------+
//   | 4B 长度 | 4B 魔数 | 4B 类型 |  payload  |
//   +--------+--------+--------+-----------+
//     大端序   0x4C434831
//
//   「长度」= 它自己之后的所有字节数 = 4(魔数) + 4(类型) + payload.size()
//
//   为什么这么设计？因为 TCP 是"字节流"，没有消息边界：你 send 两次，
//   对方可能一次 recv 全收到（粘包），也可能分三次才收全（半包）。
//   有了这个长度前缀，收方先凑够 4 字节就知道"这条消息总长多少"，
//   凑不齐就继续等 —— 这就是解决粘包的"长度前缀法"。
// ---------------------------------------------------------------------------
namespace proto {

constexpr uint16_t kPort       = 12735;         // 服务器监听端口
constexpr uint32_t kMagic      = 0x4C434831;    // 'L' 'C' 'H' '1'：用来认"这是我们的包"
constexpr uint32_t kHeaderLen  = 12;            // 长度4 + 魔数4 + 类型4
constexpr uint32_t kMaxPayload = 64 * 1024;     // 负载上限：防别人报个超大长度把内存打爆
constexpr size_t   kMaxNickLen = 32;            // 昵称上限（字节；UTF-8 下一个汉字 3 字节）

enum Type : uint32_t {
    Login   = 1,   // C→S  payload = 昵称(UTF-8)
    Welcome = 2,   // S→C  payload = 你的昵称，登录成功
    Users   = 3,   // S→C  payload = 全部在线昵称，'\n' 分隔（名单一变就推）
    Say     = 4,   // C→S  payload = [1B 模式]["D"时: 目标昵称+'\0'][正文]
    Chat    = 5,   // S→C  payload = [1B 模式][发送者昵称+'\0'][正文]
    Bye     = 6,   // C→S  主动退出，payload 空
    Error   = 7,   // S→C  错误文本（分两种：致命错误随后断开；可恢复错误只提示不断开）
};

const char* typeName(uint32_t t)
{
    switch (t) {
    case Login:   return "Login";
    case Welcome: return "Welcome";
    case Users:   return "Users";
    case Say:     return "Say";
    case Chat:    return "Chat";
    case Bye:     return "Bye";
    case Error:   return "Error";
    default:      return "??未知??";
    }
}

} // namespace proto

// 运行参数
static constexpr int kHousekeepSec   = 5;    // 定时器周期：多久扫一遍空闲连接
static constexpr int kIdleTimeoutSec = 120;  // 多久没收到任何数据就踢掉。
                                             // 更"正统"的做法是协议层心跳（Ping/Pong），
                                             // 等接 Qt 客户端时一起升级（见精读文档"下一步"）

// ---------------------------------------------------------------------------
// 二、小工具函数
// ---------------------------------------------------------------------------
namespace {

void setNonBlocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// 带时间戳的日志，输出到 stderr。
// 注意：这只是"学习阶段"的简单日志，正式版应该换成异步日志（你的 simpleLogger.hpp）。
void logLine(const char* fmt, ...)
{
    char ts[32];
    time_t now = time(nullptr);
    struct tm tmv{};
    localtime_r(&now, &tmv);
    strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);

    fprintf(stderr, "[%s] ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

// 从字节流里读一个大端的 uint32。
// 用 memcpy 而不是指针强转：网络包的内存不保证 4 字节对齐，强转在 ARM 上会挂。
uint32_t readBe32(const char* p)
{
    uint32_t v = 0;
    memcpy(&v, p, sizeof(v));
    return ntohl(v);          // 网络序(大端) → 主机序
}

// 组一个完整的帧，准备发出去
std::string packFrame(uint32_t type, const std::string& payload)
{
    const uint32_t bodyLen = 8 + static_cast<uint32_t>(payload.size());  // 魔数4 + 类型4 + 负载
    const uint32_t beLen   = htonl(bodyLen);
    const uint32_t beMagic = htonl(proto::kMagic);
    const uint32_t beType  = htonl(type);

    std::string frame;
    frame.reserve(proto::kHeaderLen + payload.size());
    frame.append(reinterpret_cast<const char*>(&beLen),   4);
    frame.append(reinterpret_cast<const char*>(&beMagic), 4);
    frame.append(reinterpret_cast<const char*>(&beType),  4);
    frame.append(payload);
    return frame;
}

// 把 payload 里的不可打印字节换成 '.'，只为打日志好看
std::string printable(const std::string& s, size_t maxLen = 80)
{
    std::string out;
    const size_t n = std::min(s.size(), maxLen);
    out.reserve(n + 3);
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        out.push_back((c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '.');
    }
    if (s.size() > maxLen) out += "...";
    return out;
}

// 昵称合法性检查：非空、不超长、不含控制字符。
// 特别是 '\n' —— Users 名单用它当分隔符，昵称里混进换行整个名单就乱了。
bool isValidNick(const std::string& nick)
{
    if (nick.empty() || nick.size() > proto::kMaxNickLen)
        return false;
    for (unsigned char c : nick) {
        if (c < 0x20 || c == 0x7F)   // 控制字符（含 \n \r \0 和 ESC 之类）
            return false;
    }
    return true;
}

// 组 Chat 帧的 payload：[1B 模式][发送者昵称 + '\0'][正文]
// （'\0' 是分隔符：把前面变长的昵称和后面变长的正文切开）
std::string buildChatPayload(char mode, const std::string& from, const std::string& body)
{
    std::string p;
    p.reserve(2 + from.size() + body.size());
    p.push_back(mode);
    p += from;
    p.push_back('\0');
    p += body;
    return p;
}

// ---- 信号处理 ----
// 信号处理函数里只能调"异步信号安全"的函数：write/read 可以，printf/malloc 不行。
// 所以这里只做一件事：往 eventfd 写 1 个字节，把事件循环叫醒，剩下的活儿交给主循环。
int g_wakeupFd = -1;
void onSigint(int)
{
    const uint64_t one = 1;
    ssize_t n = ::write(g_wakeupFd, &one, sizeof(one));
    (void)n;
}

} // namespace

// ---------------------------------------------------------------------------
// 三、一条连接
//
// 注意所有权：Connection 用 unique_ptr 管理，Channel 被 Connection 持有。
// 回调里只捕获裸指针 Connection*，安全性由"延迟回收 + closing 标记"保证。
// ---------------------------------------------------------------------------
struct Connection {
    int fd = -1;
    std::unique_ptr<Channel> channel;
    Buffer in;                  // 收：可能收到半个包，先攒着
    std::string out;            // 发：send 没发完的剩在这里，等 EPOLLOUT 续发（第 5 课）
    std::string nick;           // 登录后才有值（第 3 课）；同时也是"名单里的我"
    time_t lastActive = 0;      // 最后一次"有动静"的时间，用于踢空闲连接
    bool closing = false;       // 已在关闭流程中：所有回调看到它就立刻返回
};

// ---------------------------------------------------------------------------
// 四、服务器
// ---------------------------------------------------------------------------
class ChatServer {
public:
    bool start(uint16_t port);
    void run();

private:
    // ---- 事件回调 ----
    void onAccept();
    void onRead(Connection* conn);
    void onWrite(Connection* conn);      // EPOLLOUT：内核可写了，续发剩下的
    void onIdleTick();
    void onWakeup();

    // ---- 连接生命周期 ----
    void closeConn(Connection* conn, const std::string& reason);
    void reapPending();

    // ---- 收发 ----
    void sendFrame(Connection* conn, uint32_t type, const std::string& payload);
    void sendError(Connection* conn, const std::string& text, bool fatal);
    void flush(Connection* conn);
    void handleFrame(Connection* conn, uint32_t type, const std::string& payload);

    // ---- 业务：登录 / 转发 / 名单（第 3、4 课）----
    void handleLogin(Connection* conn, const std::string& payload);
    void handleSay(Connection* conn, const std::string& payload);
    void handleBye(Connection* conn);
    std::vector<Connection*> loggedConns() const;   // 已登录连接快照（遍历时的安全做法）
    std::string onlineNicks() const;                // Users 名单：全部在线昵称，'\n' 分隔
    void broadcastUsers();                          // 名单变了 → 推给所有已登录的人
    void broadcastChat(Connection* sender, const std::string& body);   // 群发转发

    EventLoop loop_;
    int listenFd_ = -1;
    std::unique_ptr<Channel> listenCh_;
    std::unique_ptr<Channel> timerCh_;
    std::unique_ptr<Channel> wakeupCh_;

    std::unordered_map<int, std::unique_ptr<Connection>> conns_;   // fd → 连接（IO 世界的地址）
    std::vector<std::unique_ptr<Connection>> pendingReap_;         // 待回收（安全点统一销毁）
    std::unordered_map<std::string, int> nameIndex_;               // 昵称 → fd（业务世界的地址）
    bool usersDirty_ = false;   // 名单变过吗？→ 安全点统一广播一次（防重入，见 reapPending）
};

// ---------------------------------------------------------------------------
// 启动：监听 + 定时器 + 信号唤醒
// ---------------------------------------------------------------------------
bool ChatServer::start(uint16_t port)
{
    // ---- 1. 监听 socket ----
    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        perror("socket");
        return false;
    }

    int opt = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));  // 重启时不用等 TIME_WAIT

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);   // 监听本机所有网卡
    addr.sin_port        = htons(port);

    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind");
        return false;
    }
    if (::listen(listenFd_, SOMAXCONN) < 0) {
        perror("listen");
        return false;
    }
    setNonBlocking(listenFd_);   // 配合下面的 accept 循环：accept 到 EAGAIN 为止

    listenCh_ = std::make_unique<Channel>(listenFd_);
    listenCh_->setReadCallback([this] { onAccept(); });
    listenCh_->enableReading();
    loop_.updateChannel(listenCh_.get());   // enableReading 只改内存，必须同步给 epoll

    // ---- 2. 定时器：定期扫一遍空闲连接（沿用 webserver.cpp 里 timerfd 的套路）----
    const int tfd = ::timerfd_create(CLOCK_MONOTONIC, 0);
    itimerspec spec{};
    spec.it_value.tv_sec    = kHousekeepSec;
    spec.it_interval.tv_sec = kHousekeepSec;    // 每 5 秒响一次
    ::timerfd_settime(tfd, 0, &spec, nullptr);

    timerCh_ = std::make_unique<Channel>(tfd);
    timerCh_->setReadCallback([this, tfd] {
        uint64_t expirations = 0;
        ssize_t n = ::read(tfd, &expirations, sizeof(expirations));   // 必须把定时器读掉，否则一直可读
        (void)n;
        onIdleTick();
    });
    timerCh_->enableReading();
    loop_.updateChannel(timerCh_.get());

    // ---- 3. 唤醒：SIGINT → eventfd → 在事件循环里优雅退出 ----
    g_wakeupFd = ::eventfd(0, EFD_NONBLOCK);
    ::signal(SIGINT, onSigint);
    ::signal(SIGPIPE, SIG_IGN);   // 别让"往已关闭的 socket 写"直接把进程打死

    wakeupCh_ = std::make_unique<Channel>(g_wakeupFd);
    wakeupCh_->setReadCallback([this] { onWakeup(); });
    wakeupCh_->enableReading();
    loop_.updateChannel(wakeupCh_.get());

    // ---- 4. 安全点：每轮事件分发结束后的收尾工作 ----
    loop_.setPostDispatch([this] { reapPending(); });

    return true;
}

void ChatServer::run()
{
    logLine("聊天中转服务器已启动，监听 0.0.0.0:%u", proto::kPort);
    logLine("测试：printf '\\x00\\x00\\x00\\x0a\\x4c\\x43\\x48\\x31\\x00\\x00\\x00\\x01hi' | nc 127.0.0.1 %u",
            proto::kPort);
    logLine("（上面那串十六进制就是一帧 Login，登录昵称 \"hi\"；Ctrl+C 优雅退出）");
    logLine("（完整功能冒烟测试：python3 tests/chat_smoke_test.py；看事件循环调用链：TRACE=1）");

    loop_.loop();

    // 收尾：把还活着的连接全关掉（loop 已退出，此处单线程操作，安全）
    for (auto& [fd, conn] : conns_) {
        loop_.removeChannel(conn->channel.get());
        ::close(conn->fd);
    }
    conns_.clear();
    logLine("服务器已优雅退出");
}

// ---------------------------------------------------------------------------
// 接受新连接
// ---------------------------------------------------------------------------
void ChatServer::onAccept()
{
    while (true) {
        const int fd = ::accept(listenFd_, nullptr, nullptr);

        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;   // 新连接都取完了，正常退出
            if (errno == EINTR) continue;                         // 被信号打断，重试
            perror("accept");
            break;
        }

        setNonBlocking(fd);

        auto conn      = std::make_unique<Connection>();
        conn->fd       = fd;
        conn->lastActive = time(nullptr);
        conn->channel  = std::make_unique<Channel>(fd);

        // 回调里只捕获裸指针 Connection*。
        // 安全性靠两条保证：
        //   ① 连接对象不会当场销毁（挂到 pendingReap_，等安全点再销毁）
        //   ② 每个回调开头检查 closing，已在关闭流程中就立刻返回
        Connection* raw = conn.get();
        conn->channel->setReadCallback ([this, raw] { onRead(raw); });
        conn->channel->setWriteCallback([this, raw] { onWrite(raw); });   // EPOLLOUT：续发半写
        conn->channel->setErrorCallback([this, raw] { closeConn(raw, "EPOLLERR"); });
        conn->channel->setCloseCallback([this, raw] { closeConn(raw, "EPOLLHUP"); });

        conn->channel->enableReading();
        loop_.updateChannel(conn->channel.get());   // 注册进 epoll（第一次是 ADD）

        conns_[fd] = std::move(conn);   // 注意：raw 在 move 之前就取好了
        logLine("[+] 新连接 fd=%d（当前 %zu 个连接）", fd, conns_.size());
    }
}

// ---------------------------------------------------------------------------
// 【第 2 课的核心】读到数据 → 循环切出"完整帧"
//
// 为什么要循环？
//   一次 recv 可能收到 3 个半包（粘包），也可能只收到半个包（半包）。
//   所以必须：读数 → 攒进缓冲 → 只要凑得出一条完整消息就取走 → 直到凑不出为止。
//
// 判断"凑不凑得出一条完整消息"就靠那 4 字节长度前缀。
// ---------------------------------------------------------------------------
void ChatServer::onRead(Connection* conn)
{
    if (conn->closing) return;   // 已经在关闭流程里，别碰它

    const int total = conn->in.readFd(conn->fd);
    if (total < 0) {             // readFd 返回 -1 = 对端关闭 / 读出错
        closeConn(conn, "对端关闭");
        return;
    }
    if (total > 0)
        conn->lastActive = time(nullptr);

    while (!conn->closing) {
        const size_t readable = conn->in.readableBytes();

        // ---- ① 连包头（12 字节）都没凑齐：半包，等下次数据来了再说 ----
        if (readable < proto::kHeaderLen)
            break;

        const char* p = conn->in.peek();     // 只"偷看"，不消费

        const uint32_t bodyLen = readBe32(p);        // 长度字段
        const uint32_t magic   = readBe32(p + 4);
        const uint32_t type    = readBe32(p + 8);

        // ---- ② 校验长度：这是防"恶意/损坏数据"的第一道门 ----
        // 长度字段最小也得是 8（魔数+类型），最大不能超过我们允许的负载上限。
        // 如果有人在头部塞一个 0xFFFFFFFF，会导致我们傻等 4GB 数据（内存爆炸）。
        if (bodyLen < 8 || bodyLen > proto::kMaxPayload + 8) {
            logLine("[!] fd=%d 长度字段非法(%u)，断开", conn->fd, bodyLen);
            closeConn(conn, "协议错误：长度非法");
            return;
        }

        const uint32_t frameLen = 4 + bodyLen;       // 整帧长度 = 长度字段本身 + body

        // ---- ③ 整帧还没收全：半包，继续等 ----
        if (readable < frameLen)
            break;

        // ---- ④ 凑齐了：把这一帧完整取出来，从缓冲里消费掉 ----
        std::string payload(p + proto::kHeaderLen, bodyLen - 8);
        conn->in.retrieve(frameLen);      // 关键：消费掉，否则下一轮又读到同一条

        if (magic != proto::kMagic) {
            logLine("[!] fd=%d 魔数不对(0x%08X)，断开", conn->fd, magic);
            closeConn(conn, "协议错误：魔数不对");
            return;
        }

        // ---- ⑤ 交给业务层。注意：handleFrame 里可能把这条连接关掉 ----
        handleFrame(conn, type, payload);
    }
}

// ---------------------------------------------------------------------------
// 业务层：一帧收全后，按"类型"分派 —— 这就是一个简单的状态机。
//
//   Connected（还没登录）→ 只接受 Login / Bye
//   LoggedIn （已登录）  → 接受 Say / Bye（再来 Login 就提示忽略）
// ---------------------------------------------------------------------------
void ChatServer::handleFrame(Connection* conn, uint32_t type, const std::string& payload)
{
    logLine("[<] fd=%d type=%s(%u) payload=%zu字节 \"%s\"",
            conn->fd, proto::typeName(type), type,
            payload.size(), printable(payload).c_str());

    switch (type) {
    case proto::Login: handleLogin(conn, payload); break;
    case proto::Say:   handleSay(conn, payload);   break;
    case proto::Bye:   handleBye(conn);            break;
    default:
        // Welcome / Users / Chat / Error 都是"服务器→客户端"的方向，
        // 客户端不该发过来；收到就说明对方不是我们的客户端，或客户端写飞了
        sendError(conn, "协议错误：不认识的消息类型", /*fatal=*/true);
        break;
    }
}

// ---------------------------------------------------------------------------
// 【第 3 课】Login：客户端"报到"，服务器把昵称登记进两张表
//
// 四步：
//   ① 校验昵称（非空、不超长、没有控制字符）
//   ② 查重：名字被占了就 Error 提示，但**不断开** —— 让用户换个名字重试
//   ③ 登记：conn->nick 记住名字；nameIndex_[名字] = fd（业务地址 → IO 地址）
//   ④ 回 Welcome；名单广播不在这里做，交给安全点统一播（usersDirty_ 标记）
// ---------------------------------------------------------------------------
void ChatServer::handleLogin(Connection* conn, const std::string& payload)
{
    if (!conn->nick.empty()) {
        // 已经登录过了：多半是客户端 bug —— 提示一下，但不断开
        sendError(conn, "你已经登录过了（昵称=" + conn->nick + "）", /*fatal=*/false);
        return;
    }
    if (!isValidNick(payload)) {
        sendError(conn, "昵称不合法（1~32 字节，不能含换行等控制字符）", /*fatal=*/false);
        return;
    }
    if (nameIndex_.count(payload) > 0) {
        sendError(conn, "昵称「" + payload + "」已被占用，请换一个", /*fatal=*/false);
        return;
    }

    conn->nick          = payload;
    nameIndex_[payload] = conn->fd;      // 第二张表：昵称 → fd

    logLine("[+] 用户上线：%s（fd=%d，当前在线 %zu 人）",
            conn->nick.c_str(), conn->fd, nameIndex_.size());

    sendFrame(conn, proto::Welcome, conn->nick);   // 只回给对方：登录成功
    usersDirty_ = true;                            // 名单变了 → 安全点统一广播（含新人自己）
}

// ---------------------------------------------------------------------------
// 【第 4 课】Say → Chat：转发
//
// payload 布局：[1B 模式]['D' 时: 目标昵称 + '\0'][正文]
//   'B' 群发：转发给除发送者外的所有已登录用户
//   'D' 私聊：拿目标昵称查 nameIndex_ 找到 fd，只发那一个人（O(1) 路由）
// ---------------------------------------------------------------------------
void ChatServer::handleSay(Connection* conn, const std::string& payload)
{
    if (conn->nick.empty()) {   // 状态机：先登录才能发言
        sendError(conn, "请先 Login 再发言", /*fatal=*/true);
        return;
    }
    if (payload.empty()) {
        sendError(conn, "Say 格式错误：缺模式字节", /*fatal=*/true);
        return;
    }

    const char mode = payload[0];

    if (mode == 'B') {
        const std::string body = payload.substr(1);
        logLine("[>] %s 群发（正文 %zu 字节）", conn->nick.c_str(), body.size());
        broadcastChat(conn, body);
        return;
    }

    if (mode == 'D') {
        // 'D' 后面是"目标昵称 + '\0'"：用 '\0' 把变长的昵称和变长的正文切开
        const size_t sep = payload.find('\0', 1);
        if (sep == std::string::npos) {
            sendError(conn, "Say(D) 格式错误：目标昵称后缺 '\\0' 分隔符", /*fatal=*/true);
            return;
        }
        const std::string target = payload.substr(1, sep - 1);
        const std::string body   = payload.substr(sep + 1);

        auto it = nameIndex_.find(target);
        if (it == nameIndex_.end()) {
            // 可恢复错误：对方不在线，只提示发送者，不踢人
            sendError(conn, "对方不在线：" + target, /*fatal=*/false);
            return;
        }

        auto cit = conns_.find(it->second);
        if (cit == conns_.end() || cit->second->closing) {
            // 理论上到不了（关闭时两张表是同步清理的）；防一手，真到了也当"不在线"
            sendError(conn, "对方不在线：" + target, /*fatal=*/false);
            return;
        }

        logLine("[>] %s 私聊 → %s（正文 %zu 字节）", conn->nick.c_str(), target.c_str(), body.size());
        sendFrame(cit->second.get(), proto::Chat, buildChatPayload('D', conn->nick, body));
        return;
    }

    sendError(conn, "Say 格式错误：模式必须是 'B' 或 'D'", /*fatal=*/true);
}

// 【第 6 课】Bye：客户端主动说"我要走了" → 走统一的关闭流程
// （closeConn 里会做：清 nameIndex_、标记名单要广播、从 epoll 摘掉、延迟回收）
void ChatServer::handleBye(Connection* conn)
{
    closeConn(conn, "客户端主动退出（Bye）");
}

// ---------------------------------------------------------------------------
// 名单与广播
// ---------------------------------------------------------------------------

// 取一份"已登录连接"的快照（裸指针列表）。
// 为什么先拷贝？sendFrame 失败会顺手 closeConn → erase conns_，
// 边遍历 unordered_map 边改它是 UB。先快照、再操作，最稳。
std::vector<Connection*> ChatServer::loggedConns() const
{
    std::vector<Connection*> v;
    for (const auto& [fd, conn] : conns_) {
        if (!conn->closing && !conn->nick.empty())
            v.push_back(conn.get());
    }
    return v;
}

// 组一份在线名单：所有已登录昵称，'\n' 分隔。
// 排个序再拼，让每次推送的顺序稳定（unordered_map 的遍历顺序不保证）。
std::string ChatServer::onlineNicks() const
{
    std::vector<std::string> nicks;
    for (const auto& [fd, conn] : conns_) {
        if (!conn->closing && !conn->nick.empty())
            nicks.push_back(conn->nick);
    }
    std::sort(nicks.begin(), nicks.end());

    std::string list;
    for (const std::string& n : nicks) {
        if (!list.empty()) list += '\n';
        list += n;
    }
    return list;
}

// 名单变了 → 推一份新的 Users 给所有已登录的人（含刚上线的新人自己）。
// 本函数只在安全点（reapPending）被调用一次 —— 见 usersDirty_ 的说明。
void ChatServer::broadcastUsers()
{
    const auto targets     = loggedConns();
    const std::string list = onlineNicks();
    logLine("[>] 广播 Users 名单（%zu 人在线）", targets.size());
    for (Connection* c : targets)
        sendFrame(c, proto::Users, list);
}

// 群发：把发送者的原话包成 Chat 帧，发给除他以外的所有在线用户。
// （发送者自己不回显：客户端在本地就把消息显示出来了，再回一份就重了）
void ChatServer::broadcastChat(Connection* sender, const std::string& body)
{
    const std::string payload = buildChatPayload('B', sender->nick, body);
    for (Connection* c : loggedConns()) {
        if (c == sender) continue;
        sendFrame(c, proto::Chat, payload);
    }
}

// ---------------------------------------------------------------------------
// 关闭一条连接
//
// ⚠️ 关键设计：不当场 delete！
//   此刻我们可能正处在 conn 自己的回调栈里，而且本轮 epoll 返回的 ready 列表里
//   可能还有别的 Channel 指向它。当场 delete 就是 use-after-free。
//   做法：从 epoll 摘掉 + 关 fd + 标记 closing，然后把对象挂到 pendingReap_，
//         等 EventLoop 的安全点（postDispatch）再统一销毁。
// ---------------------------------------------------------------------------
void ChatServer::closeConn(Connection* conn, const std::string& reason)
{
    if (conn->closing) return;   // 幂等：读关闭/写失败/超时可能同时触发同一条连接
    conn->closing = true;

    logLine("[-] fd=%d%s 断开（%s）",
            conn->fd,
            conn->nick.empty() ? "" : (" 昵称=" + conn->nick).c_str(),
            reason.c_str());

    // 第 3 课：同步清理"业务世界的地址"。
    // 忘了这一步 = 幽灵用户：人已经走了，名单里还挂着他的名字，
    // 别人私聊他还能"发出去"（发向一个已关闭的 fd），名单也越列越长。
    if (!conn->nick.empty()) {
        nameIndex_.erase(conn->nick);
        conn->nick.clear();
        usersDirty_ = true;     // 名单少了人 → 安全点广播新名单
    }

    loop_.removeChannel(conn->channel.get());   // 从 epoll 摘掉
    ::close(conn->fd);                          // 关 fd

    auto it = conns_.find(conn->fd);
    if (it != conns_.end()) {
        pendingReap_.push_back(std::move(it->second));   // 所有权移走，对象先不死
        conns_.erase(it);
    }
}

// 安全点到了：本轮所有回调都已返回，可以放心销毁
void ChatServer::reapPending()
{
    if (!pendingReap_.empty()) {
        logLine("[reap] 安全回收 %zu 个连接对象（此刻所有回调均已返回）", pendingReap_.size());
        pendingReap_.clear();    // unique_ptr 析构 → Channel 析构
    }

    // 名单在本轮变过？统一在这里广播一次。
    // 为什么不"谁登录/下线就谁立刻广播"？因为广播里 sendFrame 失败会 closeConn →
    // 又触发广播……层层嵌套（重入），很容易写出难查的 bug。
    // 延迟到安全点、一次播完：逻辑最简单，也最安全。
    if (usersDirty_) {
        usersDirty_ = false;
        broadcastUsers();
    }
}

// ---------------------------------------------------------------------------
// 发送：先塞进 out（发送缓冲），再尽力 flush 出去。
// 发不完没关系 —— flush 会给这条连接挂上 EPOLLOUT，等内核可写了自动续发。
// ---------------------------------------------------------------------------
void ChatServer::sendFrame(Connection* conn, uint32_t type, const std::string& payload)
{
    if (conn->closing) return;

    conn->out += packFrame(type, payload);
    flush(conn);
}

// 发一条 Error 帧。两种用法：
//   fatal=true  → 错误不可恢复（协议错误），发完就断开；
//   fatal=false → 用户还能修正后重试（比如换个昵称），只提示、不断开。
void ChatServer::sendError(Connection* conn, const std::string& text, bool fatal)
{
    logLine("[!] fd=%d 发 Error：%s%s", conn->fd, text.c_str(), fatal ? "（随后断开）" : "");
    sendFrame(conn, proto::Error, text);
    if (fatal && !conn->closing)
        closeConn(conn, "致命错误：" + text);
}

// ---------------------------------------------------------------------------
// 【第 5 课】把 out 里的字节尽量发出去；发不完就交给 EPOLLOUT 续发
//
// send() 不保证全发出去：内核发送缓冲满了，就只能发一半（或干脆 EAGAIN）。
// 聊天消息可能几十 KB，必须处理"发到一半"：
//   没发完的留在 out 里，给这条连接挂上 EPOLLOUT，等内核腾出缓冲自动续发。
//
// ⚠️ 两个关键点（LT 水平触发）：
//   ① 发完了必须立刻 disableWriting —— LT 下只要发送缓冲还有空位，"可写"就会一直响，
//      不摘掉的话每条连接都会被不停叫醒，事件循环空转、CPU 打满；
//   ② enableWriting 只改内存里的 events_，必须 updateChannel 同步给 epoll，
//      否则"事件永远不来"。
// ---------------------------------------------------------------------------
void ChatServer::flush(Connection* conn)
{
    while (!conn->out.empty()) {
        // MSG_NOSIGNAL：写一个已关闭的 socket 时不要发 SIGPIPE 把进程干掉
        const ssize_t n = ::send(conn->fd, conn->out.data(), conn->out.size(), MSG_NOSIGNAL);

        if (n > 0) {
            conn->out.erase(0, static_cast<size_t>(n));   // 发出去的从缓冲里删掉
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // 内核发送缓冲满了：剩下的留着，挂 EPOLLOUT 等可写
            if (!conn->channel->isWriting()) {
                conn->channel->enableWriting();
                loop_.updateChannel(conn->channel.get());   // 改了 events_ 必须同步！
                logLine("[~] fd=%d 发送缓冲满，剩 %zu 字节，改由 EPOLLOUT 续发",
                        conn->fd, conn->out.size());
            }
            return;
        }

        closeConn(conn, "send 失败");
        return;
    }

    // 全发完了：把 EPOLLOUT 摘掉，免得 LT 模式下一直空转叫醒事件循环
    if (conn->channel->isWriting()) {
        conn->channel->disableWriting();
        loop_.updateChannel(conn->channel.get());
        logLine("[~] fd=%d 续发完成，EPOLLOUT 已摘除", conn->fd);
    }
}

// EPOLLOUT 回调：内核说"这个 fd 现在可写了" → 把剩下的继续发
void ChatServer::onWrite(Connection* conn)
{
    if (conn->closing) return;
    flush(conn);
}

// ---------------------------------------------------------------------------
// 定时任务：踢掉长时间没动静的连接
// 注意别边遍历边改容器 —— closeConn 会 erase conns_，所以先收集再处理
// ---------------------------------------------------------------------------
void ChatServer::onIdleTick()
{
    const time_t now = time(nullptr);

    std::vector<Connection*> dead;
    for (auto& [fd, conn] : conns_) {
        if (now - conn->lastActive > kIdleTimeoutSec)
            dead.push_back(conn.get());
    }
    for (Connection* c : dead)
        closeConn(c, "空闲超时");
}

// ---------------------------------------------------------------------------
// Ctrl+C：通知所有客户端，然后退出事件循环
// 同样先取快照再遍历 —— 因为 sendFrame 失败时会 closeConn（改动容器）
// ---------------------------------------------------------------------------
void ChatServer::onWakeup()
{
    uint64_t one = 0;
    ssize_t n = ::read(g_wakeupFd, &one, sizeof(one));
    (void)n;

    logLine("收到 SIGINT，正在优雅关闭（在线 %zu 个连接）...", conns_.size());

    std::vector<Connection*> all;
    for (auto& [fd, conn] : conns_)
        all.push_back(conn.get());

    for (Connection* c : all)
        sendFrame(c, proto::Error, "服务器正在关闭");

    loop_.quit();
}

// ---------------------------------------------------------------------------
int main()
{
    ChatServer server;
    if (!server.start(proto::kPort))
        return 1;

    server.run();
    return 0;
}
