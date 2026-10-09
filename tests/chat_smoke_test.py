#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
chat_server 冒烟测试（对应 chat_server.cpp 第 3~6 课的功能）

用法：
    1) 先启动服务器：./src/chat_server
    2) 另开一个终端：python3 tests/chat_smoke_test.py

覆盖场景：
    ① 登录 / Welcome / Users 名单广播
    ② 重复登录：只提示、不断开
    ③ 第三人上线：所有人（含新人自己）都收到新名单
    ④ 重名拒绝后可以换个名字重试
    ⑤ 慢客户端不丢消息（服务器半写 + EPOLLOUT 续发）
    ⑥ 群发 / 私聊 / 对方不在线提示
    ⑦ 未登录就发言 → Error + 断开
    ⑧ Bye 退出 → 名单更新 + 检查没有"幽灵用户"
"""

import socket
import struct
import sys
import time

HOST = '127.0.0.1'
PORT = 12735

MAGIC = 0x4C434831
LOGIN, WELCOME, USERS, SAY, CHAT, BYE, ERROR = 1, 2, 3, 4, 5, 6, 7


# ---- 协议小工具：和 chat_server.cpp 里的 packFrame 一一对应 ----
def pack_frame(t, payload=b''):
    # [4B 长度][4B 魔数][4B 类型][payload]
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


def expect(s, t, payload=None, contains=None):
    ft, fp = recv_frame(s)
    assert ft == t, f'期望类型 {t}，实际 {ft}（payload={fp!r}）'
    if payload is not None:
        assert fp == payload, f'期望 payload={payload!r}，实际 {fp!r}'
    if contains is not None:
        assert contains.encode() in fp, f'期望包含 {contains!r}，实际 {fp!r}'
    return fp


def nick_set(payload):
    return set(payload.decode().split('\n'))


def connect(rcvbuf=None):
    s = socket.socket()
    if rcvbuf:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
    s.settimeout(10)
    s.connect((HOST, PORT))
    return s


def main():
    # ---- 场景 1：两个用户登录 + 名单广播 ----
    a = connect()
    a.sendall(pack_frame(LOGIN, b'alice'))
    expect(a, WELCOME, b'alice')
    expect(a, USERS, b'alice')

    b = connect(rcvbuf=4096)      # 故意把接收缓冲调小，逼出服务器端的"半写"
    b.sendall(pack_frame(LOGIN, b'bob'))
    expect(b, WELCOME, b'bob')
    assert nick_set(expect(b, USERS)) == {'alice', 'bob'}
    assert nick_set(expect(a, USERS)) == {'alice', 'bob'}
    print('PASS 场景1：登录 / Welcome / Users 名单广播')

    # ---- 场景 2：重复登录（可恢复错误，不断开）----
    a.sendall(pack_frame(LOGIN, b'alice'))
    expect(a, ERROR, contains='已经登录过')
    print('PASS 场景2：重复登录只提示、不断开')

    # ---- 场景 3：第三人上线 ----
    d = connect()
    d.sendall(pack_frame(LOGIN, b'carol'))
    expect(d, WELCOME, b'carol')
    assert nick_set(expect(d, USERS)) == {'alice', 'bob', 'carol'}
    assert nick_set(expect(a, USERS)) == {'alice', 'bob', 'carol'}
    assert nick_set(expect(b, USERS)) == {'alice', 'bob', 'carol'}
    print('PASS 场景3：上线广播（新人也收到完整名单）')

    # ---- 场景 4：重名拒绝（还能换个名字重试）----
    e = connect()
    e.sendall(pack_frame(LOGIN, b'alice'))
    expect(e, ERROR, contains='已被占用')
    e.sendall(pack_frame(LOGIN, b'dave'))
    expect(e, WELCOME, b'dave')
    expect(e, USERS)
    for s in (a, b, d):
        assert nick_set(expect(s, USERS)) == {'alice', 'bob', 'carol', 'dave'}
    print('PASS 场景4：重名拒绝后可以换名重试')

    # ---- 场景 5：慢客户端不丢消息（半写 + EPOLLOUT 续发）----
    # 内核发送缓冲上限 = /proc/sys/net/ipv4/tcp_wmem 的第三个值（本机 4MB）。
    # 数据量必须超过它，才能保证服务器真的走到 EAGAIN → EPOLLOUT 续发那条路。
    chunk = b'x' * (50 * 1024)
    n = 120                       # 120 条 × 50KB = 6MB > 4MB，必然触发半写
    for _ in range(n):
        a.sendall(pack_frame(SAY, b'B' + chunk))
    time.sleep(2.0)               # b 故意不读，让服务器的发送缓冲堵满
    for i in range(n):            # 再一口气全部读出来，一条都不能少
        t, fp = recv_frame(b)
        assert t == CHAT and fp == b'B' + b'alice' + b'\0' + chunk, f'第 {i} 条消息损坏'
    print(f'PASS 场景5：慢客户端 {n} 条 × 50KB（共 6MB）一条不丢')

    # ---- 场景 6：群发 + 私聊 ----
    a.sendall(pack_frame(SAY, b'B' + '大家好'.encode()))
    expect(b, CHAT, b'B' + b'alice' + b'\0' + '大家好'.encode())

    b.sendall(pack_frame(SAY, b'D' + b'alice\0' + '悄悄话'.encode()))
    expect(a, CHAT, b'D' + b'bob' + b'\0' + '悄悄话'.encode())

    a.sendall(pack_frame(SAY, b'D' + b'nobody\0' + '在吗'.encode()))
    expect(a, ERROR, contains='对方不在线：nobody')
    print('PASS 场景6：群发 / 私聊 / 对方不在线提示')

    # ---- 场景 7：未登录就发言 → Error + 断开 ----
    c = connect()
    c.sendall(pack_frame(SAY, b'B' + '偷跑'.encode()))
    expect(c, ERROR, contains='先 Login')
    assert c.recv(1) == b''       # 服务器已经断开这条连接
    c.close()
    print('PASS 场景7：未登录发言被断开')

    # ---- 场景 8：Bye 退出 + 名单更新 + 幽灵用户检查 ----
    b.sendall(pack_frame(BYE))
    assert b.recv(1) == b''       # 服务器关闭了 b 的连接
    assert nick_set(expect(a, USERS)) == {'alice', 'carol', 'dave'}

    a.sendall(pack_frame(SAY, b'D' + b'bob\0' + '还在吗'.encode()))
    expect(a, ERROR, contains='对方不在线：bob')   # nameIndex_ 清理干净，没有幽灵用户
    print('PASS 场景8：Bye 退出 / 名单更新 / 无幽灵用户')

    for s in (a, d, e):
        s.close()
    print('\nALL PASS ✅  chat_server 第 3~6 课功能全部正常')


if __name__ == '__main__':
    try:
        main()
    except Exception as exc:      # noqa: BLE001
        print(f'\nFAIL ❌  {type(exc).__name__}: {exc}')
        sys.exit(1)
