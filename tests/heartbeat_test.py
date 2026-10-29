#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
第 7 课验收：应用层心跳 Ping

用法：
    1) 先启动服务器：./build-release/chat_server
    2) 另开一个终端：python3 tests/heartbeat_test.py

覆盖场景：
    ① Ping 不引起断开、也不产生任何回应帧（保持单向心跳的兼容性）
    ② 每 20 秒发一次 Ping，持续 70 秒（> kIdleTimeoutSec=65）不被踢
       —— 这是第 7 课的核心：心跳让"静默"的长连接活下来
    ③ 反证：登录后什么都不发，70 秒后被"空闲超时"踢掉
    ④ Ping 不计入聊天消息：发心跳期间对方收不到任何 Chat 帧
    ⑤ 单向兼容性：服务器绝不主动发 Pong（旧客户端收到未知帧会断开）

注意：
    场景 ② ③ 需要 70 秒，整轮约 2.5 分钟。默认全部执行；
    只想快速验证协议形态的话：python3 tests/heartbeat_test.py --fast
"""

import socket
import struct
import sys
import time

HOST = '127.0.0.1'
PORT = 12735

MAGIC = 0x4C434831
LOGIN, WELCOME, USERS, SAY, CHAT, BYE, ERROR, PING, PONG = 1, 2, 3, 4, 5, 6, 7, 8, 9

IDLE_TIMEOUT = 65      # 和 chat_server.cpp 里的 kIdleTimeoutSec 保持一致
PING_INTERVAL = 20     # 和客户端的发送周期保持一致
FAST = '--fast' in sys.argv

# 每轮用唯一昵称：上一轮如果失败退出，服务端可能还留着同名连接，
# 直接复用 'hb_alice' 会被"昵称已被占用"挡掉，让人误以为是心跳的问题。
RUN = str(int(time.time()))[-5:]


def pack_frame(t, payload=b''):
    return struct.pack('>III', 8 + len(payload), MAGIC, t) + payload


def recvn(s, n):
    buf = b''
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            raise EOFError('连接被对端关闭')
        buf += chunk
    return buf


def recv_frame(s):
    length, magic, t = struct.unpack('>III', recvn(s, 12))
    assert magic == MAGIC, f'magic 不对: {magic:#x}'
    return t, recvn(s, length - 8)


def nick_set(payload):
    return set(payload.decode().split('\n'))


def connect():
    s = socket.socket()
    s.settimeout(10)
    s.connect((HOST, PORT))
    return s


def login(s, nick):
    s.sendall(pack_frame(LOGIN, nick.encode()))
    t, p = recv_frame(s)
    assert t == WELCOME and p == nick.encode(), f'{nick} 登录失败: type={t} payload={p!r}'
    t, p = recv_frame(s)          # Users 名单
    assert t == USERS, f'期望 Users，实际 {t}'
    return nick_set(p)


def expect_silence(s, seconds, what):
    """在 seconds 秒内不应收到任何数据。用短超时轮询，避免阻塞太久。"""
    s.settimeout(0.5)
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            data = s.recv(4096)
            if data == b'':
                raise AssertionError(f'{what}：连接被关闭了（应为静默）')
            raise AssertionError(f'{what}：收到了意外的数据 {data[:40]!r}')
        except socket.timeout:
            continue
        except BlockingIOError:
            continue
    s.settimeout(10)


def is_closed(s):
    """服务器是否已经关掉这条连接（1.5 秒内没数据就当作还活着）。"""
    s.settimeout(1.5)
    try:
        return s.recv(1) == b''
    except socket.timeout:
        return False


def drain(s, quiet=0.4):
    """把已经排队的帧全部读掉，直到安静 quiet 秒。

    为什么要这个：`Users` 是"状态推送"，服务端在名单变化时推、登录时也推，
    数量不固定（上一轮残留的连接掉线还会再推一次）。
    断言"心跳期间必须静默"之前，先把这些合法帧排空，否则会误报。
    """
    s.settimeout(quiet)
    while True:
        try:
            if s.recv(4096) == b'':
                raise AssertionError('drain：连接已被关闭')
        except socket.timeout:
            break
        except BlockingIOError:
            break
    s.settimeout(10)


# ---------------------------------------------------------------- 场景 1
def case_ping_no_response():
    a = connect()
    login(a, f'hb_alice_{RUN}')
    drain(a)                       # 排空登录产生的名单帧

    a.sendall(pack_frame(PING))
    # 服务器不回 Pong，也不断开：之后 1 秒内必须静默
    expect_silence(a, 1.0, '场景1：Ping 后不该有回应')

    # 还能正常干活，说明连接确实活着
    a.sendall(pack_frame(SAY, b'B' + 'ping之后还能说话'.encode()))
    drain(a)                       # 只有自己在线，群发不会回显；这里主要防意外帧
    a.sendall(pack_frame(BYE))
    assert is_closed(a), '场景1：Bye 后连接应被关闭'
    a.close()
    print('PASS 场景1：Ping 无回应、不断开，连接仍可用（单向心跳）')


# ---------------------------------------------------------------- 场景 2
def case_ping_keeps_alive():
    if FAST:
        print('SKIP 场景2（--fast 模式，跳过 70 秒长测）')
        return

    a = connect()
    login(a, f'hb_keeper_{RUN}')
    b = connect()
    login(b, f'hb_observer_{RUN}')

    # 先排空两边的名单帧（数量不固定），再验证两人确实互通 ——
    # 这比"断言只收到一帧 Users"更稳，也更接近真实客户端的行为。
    drain(a)
    drain(b)
    b.sendall(pack_frame(SAY, b'D' + f'hb_keeper_{RUN}'.encode() + b'\0' + b'hello'))
    t, p = recv_frame(a)
    assert t == CHAT, f'场景2：期望收到 Chat，实际 type={t}'
    assert p == b'D' + f'hb_observer_{RUN}'.encode() + b'\0' + b'hello', \
        f'场景2：私聊内容不符 {p!r}'
    drain(a)

    total = IDLE_TIMEOUT + 5          # 70 秒，超过超时阈值
    start = time.time()
    pings = 0
    while time.time() - start < total:
        # 两个连接都必须发心跳 —— 只让 a 发、b 裸奔的话，b 会在 65 秒时
        # 被"空闲超时"正常踢掉，进而给 a 推一份新名单，
        # 那会被误判成"心跳收到了回应"。真实客户端也是各自发心跳的。
        a.sendall(pack_frame(PING))
        b.sendall(pack_frame(PING))
        pings += 1
        expect_silence(a, PING_INTERVAL, '场景2：心跳期间 a 不该收到任何帧')
        expect_silence(b, 0.1, '场景2：心跳期间 b 不该收到任何帧')
        assert not is_closed(a), '场景2：发着心跳的 a 被踢了！'
        assert not is_closed(b), '场景2：发着心跳的 b 被踢了！'

    # 心跳期间，对方不该收到任何 Chat（心跳不是聊天消息）
    expect_silence(b, 0.5, '场景2：对方不该收到心跳产生的消息')

    elapsed = time.time() - start
    alive = (not is_closed(a)) and (not is_closed(b))
    a.close()
    b.close()
    assert alive, '场景2：长测结束时连接已掉'
    print(f'PASS 场景2：{elapsed:.0f} 秒内两个连接各发 {pings} 次 Ping，均存活（阈值 {IDLE_TIMEOUT}s）')


# ---------------------------------------------------------------- 场景 3
def case_idle_timeout():
    if FAST:
        print('SKIP 场景3（--fast 模式，跳过 70 秒长测）')
        return

    c = connect()
    login(c, f'hb_idle_{RUN}')
    drain(c)

    print(f'      等待 {IDLE_TIMEOUT + 8} 秒，验证无数据会被踢...')
    c.settimeout(IDLE_TIMEOUT + 8)
    t0 = time.time()
    try:
        data = c.recv(4096)
    except socket.timeout:
        raise AssertionError('场景3：超过空闲阈值仍未断开，超时逻辑失效')
    dt = time.time() - t0
    assert data == b'', f'场景3：期望连接被关闭，却收到了 {data[:40]!r}'
    assert dt >= IDLE_TIMEOUT - 2, f'场景3：{dt:.0f}s 就被踢了，早于阈值 {IDLE_TIMEOUT}s'
    c.close()
    print(f'PASS 场景3：静默 {dt:.0f} 秒后被踢（阈值 {IDLE_TIMEOUT}s），反证心跳是必需的')


# ---------------------------------------------------------------- 场景 4
def case_ping_before_login():
    """未登录发 Ping：心跳属于传输层，不该被当成"未登录发言"。"""
    d = connect()
    d.sendall(pack_frame(PING))
    expect_silence(d, 0.8, '场景4：未登录 Ping 不该有回应')

    # 之后还能正常登录，证明没被踢
    d.sendall(pack_frame(LOGIN, f'hb_late_{RUN}'.encode()))
    t, p = recv_frame(d)
    assert t == WELCOME and p == f'hb_late_{RUN}'.encode(), \
        f'未登录 Ping 后登录失败: type={t}, payload={p!r}'
    drain(d)
    d.sendall(pack_frame(BYE))
    d.close()
    print('PASS 场景4：未登录发 Ping 不被误判、不影响随后登录')


def main():
    print('=== 第 7 课：心跳 Ping 验收 ===')
    print(f'    服务器 {HOST}:{PORT}   空闲阈值 {IDLE_TIMEOUT}s   心跳周期 {PING_INTERVAL}s')
    if FAST:
        print('    --fast 模式：只跑协议形态，跳过两个 70 秒长测')
    print()

    case_ping_no_response()
    case_ping_before_login()
    case_ping_keeps_alive()
    case_idle_timeout()

    print('\nALL PASS ✅  第 7 课（应用层心跳）验收通过')


if __name__ == '__main__':
    try:
        main()
    except Exception as exc:      # noqa: BLE001
        print(f'\nFAIL ❌  {type(exc).__name__}: {exc}')
        sys.exit(1)
