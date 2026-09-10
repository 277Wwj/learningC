// webserver_flow.cpp —— 面向过程的"流水线"版 WebServer
// 不用类，所有代码在一个文件里，按数据流动的顺序排布，方便看懂整体流程。
// 功能与 OOP 版（EventLoop/Poller/Channel/Buffer）完全一样。

#include <bits/stdc++.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
using namespace std;

// ================= 配置 =================
const int PORT = 8080;
const int MAX_EVENTS = 1024;
const int CONN_TIMEOUT = 10;   // 连接超过 10 秒没活动就关闭

// ================= 全局 =================
int g_wakeup_fd;                // 优雅关闭用的 eventfd

// ================= 连接状态（普通结构体，不是类） =================
struct Conn {
    string buf;                 // 接收缓冲区：收到但还没处理的字节
    time_t lastActive;          // 最后活动时间
};

// ================= 工具函数 =================
// 把 fd 设为非阻塞
void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// 构造 HTTP 响应报文
string make_response(int code, const string& reason, const string& body, bool keepAlive) {
    string resp;
    resp += "HTTP/1.1 " + to_string(code) + " " + reason + "\r\n";
    resp += "Content-Type: text/html; charset=utf-8\r\n";
    resp += "Content-Length: " + to_string(body.size()) + "\r\n";
    resp += keepAlive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
    resp += "\r\n";
    resp += body;
    return resp;
}

// ================= 三个"事件处理"函数（流水线的三个分支） =================

// 分支 1：有新连接 → accept，注册进 epoll
void handle_accept(int listen_fd, int epfd, map<int, Conn>& conns) {
    while (true) {                      // ET 下循环 accept
        int c = accept(listen_fd, nullptr, nullptr);
        if (c < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // 没新连接了
            perror("accept");
            break;
        }
        set_nonblocking(c);
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLET;  // 边缘触发
        ev.data.fd = c;
        epoll_ctl(epfd, EPOLL_CTL_ADD, c, &ev);

        conns[c].lastActive = time(nullptr);   // 记录连接建立时间
        printf("新连接 fd=%d\n", c);
    }
}

// 分支 2：客户端有数据 → 读、解析、响应
void handle_read(int fd, int epfd, map<int, Conn>& conns) {
    Conn& conn = conns[fd];
    bool closed = false;

    // ① 循环读数据进缓冲区（读到 EAGAIN 或对端关闭为止）
    char tmp[4096];
    while (true) {
        int r = recv(fd, tmp, sizeof(tmp), 0);
        if (r > 0) {
            conn.buf.append(tmp, r);
            conn.lastActive = time(nullptr);   // 有数据，刷新活动时间
        } else if (r == 0) {
            closed = true;                     // 对端关闭
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // 读完了
            closed = true;                     // 真错误
            break;
        }
    }

    // ② 只要缓冲区里有完整请求，就一直处理（keep-alive）
    while (true) {
        size_t pos = conn.buf.find("\r\n\r\n");   // 找"空行"= 请求头结束
        if (pos == string::npos) break;           // 没有完整请求，等下次

        // ③ 解析请求行：方法 路径 版本
        string head = conn.buf.substr(0, pos);
        size_t lineEnd = head.find("\r\n");
        stringstream ss(head.substr(0, lineEnd));
        string method, path, version;
        ss >> method >> path >> version;
        printf("收到请求: %s %s\n", method.c_str(), path.c_str());

        // ④ 路由：根据路径决定返回什么
        string body;
        int code = 200;
        string reason = "OK";
        if (path == "/") {
            body = "<h1>首页</h1>";
        } else if (path == "/hello") {
            body = "<h1>Hello!</h1>";
        } else {
            code = 404;
            reason = "Not Found";
            body = "<h1>404 Not Found</h1>";
        }

        // ⑤ 决定是否 keep-alive（HTTP/1.0 响应完就关）
        bool keepAlive = (version == "HTTP/1.1");
        string resp = make_response(code, reason, body, keepAlive);
        send(fd, resp.data(), resp.size(), 0);

        // ⑥ 消费掉这个请求（含 \r\n\r\n 共 4 字节），留下后面的
        conn.buf.erase(0, pos + 4);

        // ⑦ 如果是短连接，响应完关闭
        if (!keepAlive) {
            closed = true;
            break;
        }
    }

    // ⑧ 需要关闭就清理
    if (closed) {
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
        close(fd);
        conns.erase(fd);
        printf("fd=%d 断开\n", fd);
    }
}

// 分支 3：定时器到期 → 扫描并关闭超时连接
void handle_timeout(int epfd, map<int, Conn>& conns) {
    time_t now = time(nullptr);
    for (auto it = conns.begin(); it != conns.end(); ) {
        int fd = it->first;
        if (now - it->second.lastActive > CONN_TIMEOUT) {
            epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
            close(fd);
            it = conns.erase(it);
            printf("连接 fd=%d 超时关闭\n", fd);
        } else {
            ++it;
        }
    }
}

// 信号处理器：只做异步信号安全的事——写一个字节
void on_sigint(int) {
    uint64_t one = 1;
    write(g_wakeup_fd, &one, sizeof(one));
}

// ================= 主流程 =================
int main() {
    signal(SIGPIPE, SIG_IGN);

    // 第 1 步：监听 socket
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);
    bind(listen_fd, (sockaddr*)&addr, sizeof(addr));
    listen(listen_fd, 10);
    set_nonblocking(listen_fd);

    // 第 2 步：创建 epoll，注册监听 fd
    int epfd = epoll_create1(0);
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = listen_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    // 第 3 步：创建 timerfd（每 5 秒扫描超时）
    int timer_fd = timerfd_create(CLOCK_MONOTONIC, 0);
    itimerspec spec{};
    spec.it_value.tv_sec = 5;
    spec.it_interval.tv_sec = 5;
    timerfd_settime(timer_fd, 0, &spec, nullptr);
    epoll_event tev{};
    tev.events = EPOLLIN;
    tev.data.fd = timer_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, timer_fd, &tev);

    // 第 4 步：创建 eventfd（优雅关闭）
    g_wakeup_fd = eventfd(0, EFD_NONBLOCK);
    signal(SIGINT, on_sigint);
    epoll_event wev{};
    wev.events = EPOLLIN;
    wev.data.fd = g_wakeup_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, g_wakeup_fd, &wev);

    // 第 5 步：主循环（流水线的"总调度"）
    map<int, Conn> conns;      // fd -> 连接状态
    epoll_event events[MAX_EVENTS];
    bool running = true;

    printf("WebServer 启动！访问 http://127.0.0.1:%d/\n", PORT);
    while (running) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);   // 等事件
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            if (fd == listen_fd) {                          // 新连接
                handle_accept(listen_fd, epfd, conns);
            } else if (fd == timer_fd) {                    // 定时器到期
                uint64_t exp;
                read(timer_fd, &exp, sizeof(exp));
                handle_timeout(epfd, conns);
            } else if (fd == g_wakeup_fd) {                 // 优雅关闭
                uint64_t one;
                read(g_wakeup_fd, &one, sizeof(one));
                printf("\n收到退出信号，优雅关闭...\n");
                running = false;
                break;
            } else {                                        // 客户端数据
                handle_read(fd, epfd, conns);
            }
        }
    }

    // 第 6 步：清理退出
    close(listen_fd);
    close(epfd);
    close(timer_fd);
    close(g_wakeup_fd);
    printf("服务器已关闭，再见！\n");
    return 0;
}
