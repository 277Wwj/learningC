# 第 6 课：Qt 客户端接入 `--server` 模式

> 目标：**UI 代码一行不改**，让同一个 Qt 客户端既能"局域网广播找人"，也能"连中转服务器跨网聊天"。
> 前置：`src/chat_server.cpp` 第 1~5 课已完成（登录/名单/群发/私聊/半写）。
> 本文是**教学讲义**，不是答案。每步只给要害代码，剩下的自己写——写不出来再问。

---

## 一、先搞清楚：这一课到底在练什么

很多人以为第 6 课是"给客户端加个连接功能"。**不是。**

它真正练的是三件事，面试也全在这里：

| 能力 | 具体是什么 | 面试怎么问 |
| --- | --- | --- |
| **接口与实现分离** | UI 只依赖"能发消息的客户端"这个**抽象**，不依赖 UDP 广播还是 TCP 中转 | "你怎么做到换通信方式不改 UI 的？" |
| **协议映射** | 新旧两套协议不一样，怎么在**同一套信号**上对齐 | "两套协议怎么兼容？" |
| **连接生命周期** | 异步连接、断线、重连、登录态、名单失效 | "客户端断网了你怎么办？" |

一句话概括本课的设计：

```
              ┌──────────────────┐
   UI 层  ───► │  ChatClientBase  │  （抽象：start / sendTo / sendBroadcast / onlinePeers + 4 个信号）
              └────────┬─────────┘
                       │ 继承
        ┌──────────────┴───────────────┐
        │                              │
  ChatClient（局域网）            RelayChatClient（中转服务器）
  UDP 发现 + 点对点 TCP           只跟 chat_server 说话
```

**关键认知：UI 不是"改成支持服务器"，而是"改成只认接口"。** 改完之后，UI 甚至不知道自己连的是哪种。

---

## 二、先补 3 个底层认知（不然后面全是黑箱）

### ① Qt 的 `readyRead` 就是"epoll 说这个 fd 可读了"

你在服务器里写的是：

```
epoll_wait  →  Channel::handleEvent  →  onRead  →  recv 到 EAGAIN
```

Qt 里写的是：

```
QApplication::exec()（Qt 自己的事件循环）
    → 底层 poll/epoll 报告 socket 可读
    → 发 readyRead 信号
    → 你的槽函数里 readAll()
```

**同一个思想，只是 Qt 把事件循环藏起来了。** 所以：

- ❌ 不要在 Qt 客户端里再塞一个自研 `EventLoop`——一个进程只能有一个主事件循环，两套混用一定乱；
- ✅ 你在服务器学的"非阻塞 + 事件驱动"，在 Qt 里照样成立，只是换了个名字。

### ② 客户端也有粘包/半包！

`readyRead` 触发**不等于**"一条完整消息到了"：可能给你半个包，也可能一次给你三个包。
所以客户端**必须**也做"攒缓冲 + 循环切帧"，和服务器 `onRead` 一模一样。

> 好消息：你现有的 `ChatClient::onSocketReadyRead` 已经是这个写法了，直接抄结构。

### ③ 客户端不用自己处理"半写"

服务器端你要手写 `out` 缓冲 + `EPOLLOUT` 续发，是因为 `send()` 可能只发一半。
Qt 的 `QTcpSocket::write()` **内部自带发送缓冲**，你只管往里塞，Qt 帮你续发。
这正是"用框架"和"手写框架"的差别——**面试时可以拿这个对比讲**，很有说服力。

---

## 三、动手步骤（按顺序做，每步都能独立验证）

### 第 0 步：服务端先加心跳（5 分钟，先做）

**为什么先做服务端**：客户端一旦连上就会发心跳，服务端不认识就会当"未知类型"→ 直接断开。先让服务端接得住。

`src/chat_server.cpp` 改两处：

```cpp
enum Type : uint32_t {
    // ...原有 1~7
    Ping = 8,   // C→S  心跳，payload 空
    Pong = 9,   // S→C  心跳回应，payload 空
};
```

`typeName()` 里补两个 `case`；`handleFrame()` 里加：

```cpp
case proto::Ping:
    sendFrame(conn, proto::Pong, "");   // 原样回一个 Pong
    break;
```

**验收**：`python3 tests/chat_smoke_test.py` 仍然全绿，再手动发一帧 Ping 能看到 Pong。

> 想一想：`onRead` 里已经有一句 `conn->lastActive = time(nullptr);`，所以**收到 Ping 本身就算"活着"**，不需要额外写超时逻辑。这就是协议层心跳比"TCP keepalive"更可控的原因。

---

### 第 1 步：抽出接口（只加文件，不改逻辑）

新建 `chatclientbase.h`：

```cpp
#pragma once
#include <QObject>
#include <QString>
#include <QStringList>

// UI 只认这个接口：它不知道对面是 UDP 广播还是中转服务器
class ChatClientBase : public QObject
{
    Q_OBJECT
public:
    explicit ChatClientBase(QObject *parent = nullptr) : QObject(parent) {}
    ~ChatClientBase() override = default;

    virtual bool start(const QString &nick) = 0;
    virtual void sendBroadcast(const QString &text) = 0;
    virtual void sendTo(const QString &nick, const QString &text) = 0;
    virtual QStringList onlinePeers() const = 0;

    virtual QString modeName() const = 0;    // "局域网" / "中转服务器"
    virtual QString statusInfo() const = 0;  // 状态栏那一行文字

signals:
    // 这四个信号是"两个实现共用的契约"，UI 只连这四个
    void peerOnline(const QString &nick);
    void peerOffline(const QString &nick);
    void messageReceived(const QString &from, const QString &text, bool broadcast);
    void systemMessage(const QString &text);
};
```

然后改 `chatclient.h`：

1. `class ChatClient : public ChatClientBase`（原来是 `QObject`）；
2. **删掉**原来那 4 个信号声明（基类已经有了，不能重复声明）；
3. 被重写的函数加 `override`。

**这一步不改变任何行为**，编译通过、局域网模式照旧能用，就算过关。

---

### 第 2 步：写 `RelayChatClient`

新建 `relaychatclient.h` / `relaychatclient.cpp`。骨架：

```cpp
namespace relayproto {
constexpr quint32 Magic = 0x4C434831;
enum Type : quint32 { Login = 1, Welcome = 2, Users = 3, Say = 4,
                      Chat = 5, Bye = 6, Error = 7, Ping = 8, Pong = 9 };
}

class RelayChatClient : public ChatClientBase
{
    Q_OBJECT
public:
    RelayChatClient(const QString &host, quint16 port, QObject *parent = nullptr);
    ~RelayChatClient() override;

    bool start(const QString &nick) override;      // 只负责"开始连"，不阻塞
    void sendBroadcast(const QString &text) override;
    void sendTo(const QString &nick, const QString &text) override;
    QStringList onlinePeers() const override { return m_peers; }
    QString modeName() const override { return QStringLiteral("中转服务器"); }
    QString statusInfo() const override;

private slots:
    void onConnected();
    void onReadyRead();
    void onDisconnected();

private:
    void writeFrame(quint32 type, const QByteArray &payload = {});
    void handleFrame(quint32 type, const QByteArray &payload);
    void applyUserList(const QStringList &list);
    void scheduleReconnect();

    QTcpSocket *m_socket = nullptr;
    QTimer *m_heartbeat = nullptr;
    QTimer *m_reconnect = nullptr;
    QByteArray m_buffer;        // 收缓冲：粘包/半包靠它
    QString m_nick;
    QString m_host;
    quint16 m_port = 0;
    QStringList m_peers;        // 上一次的名单，用来做 diff
    bool m_loggedIn = false;    // 收到 Welcome 才算登录成功
    bool m_wantReconnect = true;
};
```

#### 要害 A：`start()` 是异步的，别假装知道结果

```cpp
bool RelayChatClient::start(const QString &nick)
{
    m_nick = nick;
    m_socket->connectToHost(m_host, m_port);   // 立刻返回，还没连上！
    emit systemMessage(QStringLiteral("正在连接 %1:%2 ……").arg(m_host).arg(m_port));
    return true;                               // true 只表示"开始连了"
}
```

**这是本课第一个坑**：`ChatWindow::start()` 拿 `false` 会弹"启动失败"对话框。可服务器没开时你**也想正常进界面、然后自动重连**，所以这里永远返回 `true`，失败通过 `systemMessage` 告诉用户。

#### 要害 B：切帧（和服务端 `onRead` 一一对应）

```cpp
void RelayChatClient::onReadyRead()
{
    m_buffer.append(m_socket->readAll());
    forever {
        if (m_buffer.size() < 12) return;                     // 半包：等我下次再说

        const quint32 bodyLen = qFromBigEndian<quint32>(m_buffer.constData());
        if (bodyLen < 8 || bodyLen > 64 * 1024 + 8) {          // 非法长度：别傻等
            m_socket->abort(); return;
        }
        if (m_buffer.size() < 4 + int(bodyLen)) return;        // 半包

        const quint32 magic = qFromBigEndian<quint32>(m_buffer.constData() + 4);
        const quint32 type  = qFromBigEndian<quint32>(m_buffer.constData() + 8);
        const QByteArray payload = m_buffer.mid(12, bodyLen - 8);
        m_buffer.remove(0, 4 + bodyLen);                       // 消费掉！

        if (magic != relayproto::Magic) { m_socket->abort(); return; }
        handleFrame(type, payload);
    }
}
```

> `qFromBigEndian` 需要 `#include <QtEndian>`。这就是 `ntohl` 的 Qt 版。
> 注意 `remove` 别忘——忘了就是死循环，和服务器端一个道理。

#### 要害 C：`Users` 是"全量快照"，UI 只认"增量事件"

服务端每次推的是**完整名单**，而 UI 的槽只有 `onPeerOnline` / `onPeerOffline`。所以中间要做 **diff**：

```cpp
void RelayChatClient::applyUserList(const QStringList &list)
{
    const QStringList old = m_peers;
    m_peers.clear();
    for (const QString &n : list)
        if (!n.isEmpty() && n != m_nick)      // ⚠️ 名单里有自己，要过滤
            m_peers << n;

    for (const QString &n : m_peers)
        if (!old.contains(n)) emit peerOnline(n);

    for (const QString &n : old)
        if (!m_peers.contains(n)) emit peerOffline(n);
}
```

**这一步是本课最值得想清楚的地方**：

- 服务端的设计是"**状态同步**"（我告诉你现在有谁）；
- UI 的设计是"**事件通知**"（谁上线了、谁下线了）；
- 中间这层 diff，就是把"状态"翻译成"事件"。真实系统里这个模式到处都是（K8s 的 informer、React 的 diff……）。

#### 要害 D：断线时要把名单清空

```cpp
void RelayChatClient::onDisconnected()
{
    m_loggedIn = false;
    m_heartbeat->stop();

    const QStringList stale = m_peers;   // 先抄一份
    m_peers.clear();                     // 名单作废
    for (const QString &n : stale)
        emit peerOffline(n);             // 否则 UI 里全是"幽灵好友"

    emit systemMessage(QStringLiteral("与服务器断开，3 秒后重连……"));
    scheduleReconnect();
}
```

#### 要害 E：中文一定要走 UTF-8

```cpp
// 发：QString → QByteArray，一个汉字 3 字节
QByteArray p("D");
p += target.toUtf8();
p += '\0';                    // 分隔符：QByteArray 允许中间有 NUL
p += text.toUtf8();
writeFrame(relayproto::Say, p);

// 收：QByteArray → QString，先掐掉 '\0' 再转
const int sep = payload.indexOf('\0', 1);
const QString from = QString::fromUtf8(payload.mid(1, sep - 1));
const QString body = QString::fromUtf8(payload.mid(sep + 1));
```

> 别用 `nick.length()` 算字节数，中文会算少——长度永远按**字节**。

---

### 第 3 步：改 `main.cpp` 加 `--server`

```cpp
parser.addOption(QCommandLineOption(QStringList{QStringLiteral("s"), QStringLiteral("server")},
                                    QStringLiteral("中转服务器地址，如 127.0.0.1:12735（不填则用局域网模式）"),
                                    QStringLiteral("ip:port")));
```

解析时用一个小函数（自己写）：

```
"127.0.0.1:12735"  →  host = "127.0.0.1", port = 12735
没写端口           →  默认 12735
格式不对           →  提示并退出
```

然后二选一创建客户端，交给窗口：

```cpp
ChatClientBase *client = nullptr;
if (serverAddr.isEmpty())
    client = new ChatClient;                       // 原来的局域网模式
else
    client = new RelayChatClient(host, port);
// 注意：不要在这里 setParent(window)，窗口构造里做，或干脆在这里给 &app
```

---

### 第 4 步：改 `ChatWindow`

只改 4 处，**布局、消息气泡、数据库一行不动**：

1. `chatwindow.h`：前面加 `class ChatClientBase;`，成员改成 `ChatClientBase *m_client = nullptr;`，构造函数多一个参数：
   ```cpp
   ChatWindow(const QString &nick, ChatClientBase *client, QWidget *parent = nullptr);
   ```
2. 构造函数：删掉 `m_client = new ChatClient(this);`，改成用传进来的，并 `m_client->setParent(this);`（QObject 的父子关系 = 生命周期托管，窗口析构时自动删客户端）。
3. `#include "chatclient.h"` → `#include "chatclientbase.h"`。
4. `statusBar()` 那行改成 `m_client->statusInfo()`——原来写死了 `tcpPort()` 和 `DiscoveryPort`，这两个是局域网专属。

> 改完检查：`chatwindow.cpp` 里**不应该再出现** `chatproto::`、`tcpPort()`、`localAddress()`。
> 如果还有，说明抽象漏了——这就是"接口没抽干净"的信号，回去补 `statusInfo()`。

---

### 第 5 步：验收（这一步最重要，别跳）

```bash
# 1. 起服务器
./src/chat_server

# 2. 起两个客户端（终端里，或直接双击 build/11_lan_chat）
cd ~/桌面/wechat/11_lan_chat
./build/11_lan_chat --nick 小明 --server 127.0.0.1:12735
./build/11_lan_chat --nick 小红 --server 127.0.0.1:12735
```

逐条打勾：

- [ ] 两个窗口的"在线用户"里都能看到对方
- [ ] 广播：小明发言，小红收到
- [ ] 私聊：小红点小明 → 只有小明收到
- [ ] **中文正常**（不是乱码）
- [ ] 关掉小红 → 小明那边小红**立刻**从列表消失
- [ ] `pkill -x chat_server` → 两个客户端都提示"断开，3 秒后重连"
- [ ] 重新起服务器 → 客户端自动重连并恢复名单
- [ ] 局域网模式（不加 `--server`）**照旧能用** ← 回归测试，证明没改坏

---

## 四、常见坑（按踩到的概率排序）

| # | 现象 | 原因 |
| --- | --- | --- |
| 1 | 界面卡住 / 一直"正在连接" | 在 `start()` 里用了 `waitForConnected()`。**别阻塞 UI 线程**，交给信号 |
| 2 | 收到消息乱码 | 用了 `QString::toLocal8Bit()` 或 `fromLatin1()`；统一 `toUtf8()/fromUtf8()` |
| 3 | 自己出现在好友列表里 | 服务端 `Users` 包含自己，客户端忘了过滤 `n != m_nick` |
| 4 | 断线后列表里全是"幽灵" | `onDisconnected` 没给旧名单逐个发 `peerOffline` |
| 5 | 发第二条消息才收到/顺序乱 | 切帧里忘了 `m_buffer.remove(...)`，或者半包判断写成了 `>` |
| 6 | 编译报 `Cannot declare signal in ...` | 基类和派生类**重复声明了同名信号**，删掉派生类里的 |
| 7 | 重启服务器后客户端还显示旧名单 | 重连成功要**重新 Login**，并且收到新 `Users` 前先清空 |
| 8 | 昵称冲突导致重连失败 | 断线瞬间服务端可能还没回收旧连接——真实系统应该用会话 token，不是昵称 |
| 9 | 界面卡顿 | 所有网络都在 GUI 线程。数据量大时要 `moveToThread` 把网络挪走（本课不做，知道即可） |
| 10 | snap VS Code 终端里启动就崩 | `GTK_PATH` 被污染，`main.cpp` 里已有处理，别删那段 |

---

## 五、知识点总结（面试可直接讲）

**1. 接口与实现分离**
> "我把聊天客户端抽象成一个 `ChatClientBase` 接口，暴露 5 个方法和 4 个信号。局域网版和服务器版各自实现，UI 只连接口。加服务器模式时 UI 一行没改——这就是依赖倒置的实际收益。"

**2. 状态同步 vs 事件通知**
> "服务端推的是全量在线名单（状态），UI 需要的是谁上线/谁下线（事件）。我在客户端做了一次 diff，把状态翻译成事件。这样两套模式对 UI 的语义完全一致。"

**3. 异步连接的正确姿势**
> "`connectToHost` 是异步的，UI 线程绝不能 `waitForConnected` 阻塞。我把'连接中/失败/重连'都变成状态通知，用户感知是流畅的。"

**4. Reactor 与 Qt 事件循环的关系**
> "Qt 的 `readyRead` 本质就是事件循环里的'可读事件'，和我在服务器里写的 epoll+Reactor 是同一个模型。区别是服务器的循环我自己写、能控制线程模型；Qt 的循环被封装了，好处是跨平台，代价是不能自己调度。"

**5. 半写的对比**
> "服务器端我要自己维护发送缓冲 + EPOLLOUT 续发；客户端用 QTcpSocket，它内部已经做了这件事。这让我更清楚'框架替你做了什么'。"

---

## 六、给自己加的练习题（选做，做完这课才算真会）

1. **心跳超时**：发 Ping 后 2 个周期没收到 Pong → 主动 `abort()` 重连。（提示：记一个 `m_lastPong` 时间戳）
2. **重连退避**：重连间隔从 1s 开始翻倍，封顶 30s；连上后重置。（提示：`m_backoffMs`）
3. **发送失败提示**：`sendTo` 时对方不在 `m_peers` 里，直接弹系统消息"对方已离线"，不用等服务端回 Error。
4. **断线期间的离线消息**：想一想为什么"服务器不该替你暂存"？如果要做，数据结构该长什么样？（这题是 RPC/IM 项目的入口）

---

## 七、本课完成的标准

不是"能跑"，而是这三条：

1. `chatwindow.cpp` 里搜不到任何 `chatproto::` / `tcpPort` / `localAddress`；
2. 局域网模式不加参数**照常工作**；
3. 关服务器、重启服务器，客户端能自己回来，且名单正确。
