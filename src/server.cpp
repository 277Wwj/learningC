#include <bits/stdc++.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include "EventLoop.hpp"
#include "Channel.hpp"
#include "Trace.hpp"
using namespace std;

// 把 fd 设成非阻塞：没数据时 recv/accept 立刻返回 EAGAIN，而不是卡住
void set_nonblocking(int fd) {
    Trace __t("set_nonblocking");
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int main() {
    Trace __t("main");   // 程序入口：初始化 socket，然后进入事件循环
    signal(SIGPIPE, SIG_IGN);

    // ① 监听 socket（同前）
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(8080);
    bind(listen_fd, (sockaddr*)&addr, sizeof(addr));
    listen(listen_fd, 10);
    set_nonblocking(listen_fd);

    // ② 核心对象
    EventLoop loop;
    unordered_map<int, Channel*> channels;   // 记录所有客户端 Channel

    // ③ 监听 fd 包成 Channel：回调负责"接受新连接"
    Channel listen_ch(listen_fd);
    // 监听 fd 的读回调：有新连接到来时，循环 accept，并给每个连接建一个 Channel
    listen_ch.setReadCallback([&] {
        Trace __t("监听Channel回调(accept新连接)");
        while (true) {                       // ET 下循环 accept
            int c = accept(listen_fd, nullptr, nullptr);
            if (c < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                perror("accept");
                break;
            }
            set_nonblocking(c);

            // 每个客户端一个 Channel，注册"读回调"（echo 逻辑）
            Channel* ch = new Channel(c);
            // 客户端 fd 的读回调：循环 recv 读数据、echo 回去；断开则清理
            ch->setReadCallback([&loop, &channels, c] {
                Trace __t("客户端Channel回调(echo逻辑)");
                char buf[1024];
                while (true) {               // ET 下循环读
                    int r = recv(c, buf, sizeof(buf) - 1, 0);
                    if (r > 0) {
                        buf[r] = '\0';
                        printf("fd=%d 收到: %s\n", c, buf);
                        send(c, buf, r, 0);
                        continue;
                    }
                    if (r == 0) {
                        printf("fd=%d 断开\n", c);
                        loop.removeChannel(channels[c]);
                        close(c);
                        channels.erase(c);
                        break;
                    }
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    loop.removeChannel(channels[c]);
                    close(c);
                    channels.erase(c);
                    break;
                }
            });
            ch->enableReading();
            loop.updateChannel(ch);
            channels[c] = ch;
            printf("新连接 fd=%d\n", c);
        }
    });
    listen_ch.enableReading();
    loop.updateChannel(&listen_ch);

    // ④ 进入事件循环（前台开始接电话）
    printf("Reactor 服务器启动，端口 8080（EventLoop+Poller+Channel）\n");
    loop.loop();

    close(listen_fd);
    return 0;
}