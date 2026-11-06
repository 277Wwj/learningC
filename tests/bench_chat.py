#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""聊天服务器压测客户端 —— 用来对比"单 Reactor"和"主从 Reactor"。

用法：
    SUB_LOOPS=1 ./build-release/chat_server &       # 被测版本 A
    python3 tests/bench_chat.py --conns 100 --msgs 100 --mode private

    # 只输出一行 JSON（bench_compare.py 用）
    python3 tests/bench_chat.py --json

三种模式：
    say      群发。服务端把每条消息扇出给其余 N-1 个连接 → 测广播 fan-out。
    private  两两私聊。一半同 loop、一半跨 loop → 测全局索引 + 跨 loop 投递。
    ping     只发心跳，服务端不回包 → 测纯收包/拆帧，业务开销最小。
    rtt      两两配对乒乓：A 发 B 收，B 再回 A 收。同一时刻只有 1 条消息在飞，
             所以这里的延迟**不含压测端排队**，是跨 loop 投递的真实代价。

两个指标别混：
    发送耗时  = 客户端 write/drain 花的时间，只说明"客户端把字节交给内核了"，
                不代表服务端处理完；单看它会高估服务端能力。
    投递延迟  = 消息体里塞了 perf_counter 时间戳，接收端收到 Chat 帧时算差值。
                这才是端到端延迟（压测端和服务端在同一台机器上才成立）。

投递 QPS = 服务端实际投递出去的 Chat 帧 / 耗时。say 模式下约等于 发送QPS×(N-1)。

设计要点：
  · 单线程 asyncio 驱动所有连接，避免压测端自己的线程调度干扰结果；
  · 每条连接常驻一个读协程，把服务端发回来的帧全部读掉 ——
    否则广播会把服务端发送缓冲塞满，测到的就是 TCP 窗口大小了；
  · 登录完先同步起跑（start_event），避免前面的连接跑完后后面的才开始。
"""

import argparse
import asyncio
import json
import resource
import statistics
import struct
import sys
import time

MAGIC = 0x4C434831
LOGIN, WELCOME, USERS, SAY, CHAT, BYE, ERROR, PING, PONG = 1, 2, 3, 4, 5, 6, 7, 8, 9
HOST, PORT = '127.0.0.1', 12735
TS = struct.Struct('>d')          # 消息体开头的 8 字节时间戳


def pack(type_, payload=b''):
    """组一帧：[4B 长度][4B 魔数][4B 类型][payload]"""
    return struct.pack('>III', 8 + len(payload), MAGIC, type_) + payload


async def read_frame(reader, timeout=5.0):
    """读一帧，返回 (type, payload)；超时/断链返回 (None, None)。

    注意：不要把 readexactly 包进 wait_for。asyncio.wait_for 每次调用都会新建一个
    TimerHandle 挂到事件循环的定时器堆上，这里每秒要调几万次，光定时器就吃掉一半 CPU。
    只在"确定会到但迟迟不到"的地方（登录、投递静止检测）才用它。
    """
    try:
        head = await reader.readexactly(12)
    except (asyncio.IncompleteReadError, ConnectionError):
        return None, None
    body_len, magic, type_ = struct.unpack('>III', head)
    if magic != MAGIC or body_len < 8 or body_len > (1 << 20):
        return None, None
    payload = b''
    if body_len > 8:
        try:
            payload = await reader.readexactly(body_len - 8)
        except (asyncio.IncompleteReadError, ConnectionError):
            return None, None
    return type_, payload


def frame_with_ts(type_, head, tail):
    """组一个"中间要插 8 字节时间戳"的帧，返回 (前缀, 后缀)。

    body = head + 8字节时间戳 + tail，长度字段要按这个最终长度写。
    调用方每帧只需 pre + TS.pack(now) + tail，省掉拼字符串。
    """
    body_len = 8 + len(head) + TS.size + len(tail)
    pre = struct.pack('>III', body_len, MAGIC, type_) + head
    return pre, tail


def chat_body(payload):
    """Chat 帧 payload 布局：[1B 模式][昵称]['\\0'][正文] → 取正文"""
    sep = payload.find(b'\0', 1)
    return payload[sep + 1:] if sep != -1 else b''


async def login(reader, writer, nick):
    """登录并等 Welcome。期间收到的 Users 名单帧忽略。"""
    writer.write(pack(LOGIN, nick.encode()))
    await writer.drain()
    while True:
        t, p = await read_frame(reader)
        if t is None:
            return False
        if t == WELCOME:
            return True
        if t == ERROR:
            print(f'  登录失败 {nick}: {p.decode(errors="replace")}', file=sys.stderr)
            return False


class Stats:
    def __init__(self):
        self.logged_in = 0
        self.sent = 0
        self.delivered = 0
        self.failures = 0
        self.recv_timeouts = 0
        self.send_lat = []        # 单条 write+drain（秒）
        self.deliv_lat = []       # 单向投递延迟（秒）
        self.send_end = 0.0       # 最后一个发送完成的时刻（perf_counter）
        self.send_end_epoch = 0.0 # 同上，但用 epoch 时间（供编排脚本对齐 CPU 采样）
        self.deliv_end = 0.0      # 最后一帧真正到达的时刻
        self.last_frame_at = 0.0  # 最近一次收到 Chat 帧的时刻（判断"投递静止"）
        self.last_frame_epoch = 0.0
        self.sends_done = 0       # 已发完的连接数
        # perf_counter → epoch 的换算基准：省掉每帧一次 time.time()
        self.perf_base = 0.0
        self.epoch_base = 0.0


class Readiness:
    """所有连接登录完成后放行，避免边连边跑。"""

    def __init__(self, total):
        self.total = total
        self.count = 0
        self.event = asyncio.Event()

    def done(self):
        self.count += 1
        if self.count >= self.total:
            self.event.set()


async def drain(reader, stats, timeout):
    """常驻读协程：把服务端发回来的一切都读掉，并统计 Chat 帧。

    不用"收满预期条数"判断结束 —— 多进程压测时谁先跑完、谁先关连接都会让条数对不上。
    改成"所有连接都发完之后，投递静止 quiet 秒"才算结束，既稳又不会误判。
    """
    try:
        while True:
            t, p = await read_frame(reader, timeout)
            if t is None:
                return
            if t != CHAT:
                continue          # Users 等名单帧不计入业务投递
            stats.delivered += 1
            stats.last_frame_at = time.perf_counter()
            stats.last_frame_epoch = stats.epoch_base + stats.last_frame_at - stats.perf_base
            body = chat_body(p)
            if len(body) >= TS.size:
                try:
                    stats.deliv_lat.append(time.perf_counter() - TS.unpack(body[:TS.size])[0])
                except struct.error:
                    pass
    except asyncio.CancelledError:
        raise
    except Exception:
        return


async def worker(idx, cfg, stats, ready, start_event, targets):
    nick = f'b{idx}_{cfg.tag}'
    try:
        reader, writer = await asyncio.open_connection(cfg.host, cfg.port)
    except Exception as e:
        print(f'  连接失败 #{idx}: {e}', file=sys.stderr)
        stats.failures += 1
        ready.done()
        return

    if not await login(reader, writer, nick):
        stats.failures += 1
        writer.close()
        ready.done()
        return
    stats.logged_in += 1
    ready.done()

    await start_event.wait()

    drain_task = asyncio.create_task(drain(reader, stats, cfg.recv_timeout))
    await asyncio.sleep(0)      # 让读协程先挂到 socket 上，否则第一条延迟里混进了整个发送阶段

    # 帧的固定部分提到循环外：每帧只剩 1 次 struct.pack + 2 次拼接。
    # 压测端每条消息省几微秒，10 万条/秒就是几十个百分点的差别。
    # 注意：长度字段必须按"最终 body"算（含 8 字节时间戳），不能事后切片。
    if cfg.mode == 'say':
        pre, tail = frame_with_ts(SAY, b'B', b'x' * 24)
    elif cfg.mode == 'private':
        pre, tail = frame_with_ts(SAY, b'D' + targets[idx].encode() + b'\0', b'y' * 24)
    else:
        pre, tail = pack(PING), None          # Ping 不带时间戳：直接整帧复用

    ts_pack = TS.pack
    for i in range(cfg.msgs):
        # 时间戳必须"发这一帧的时候现取"，不能提到循环外，否则延迟全是假的
        frame = pre if tail is None else pre + ts_pack(time.perf_counter()) + tail

        t0 = time.perf_counter()
        try:
            writer.write(frame)
            await writer.drain()
        except Exception:
            stats.failures += 1
            break
        stats.send_lat.append(time.perf_counter() - t0)
        stats.sent += 1
        if i % 64 == 63:
            await asyncio.sleep(0)      # 定期把控制权还给读协程，避免"只发不收"
                                        # （太频繁会变成压测端自己空转，CPU 花在调度上）

    now = time.perf_counter()
    if now > stats.send_end:
        stats.send_end = now
        stats.send_end_epoch = time.time()
    stats.sends_done += 1

    if cfg.mode != 'ping':
        # 本进程的连接都发完了，再等"所有连接都发完 + 投递静止 quiet 秒"
        deadline = time.perf_counter() + cfg.recv_timeout
        while time.perf_counter() < deadline:
            quiet = time.perf_counter() - stats.last_frame_at
            if stats.sends_done >= cfg.conns and quiet > cfg.quiet:
                break
            await asyncio.sleep(0.05)
        else:
            stats.recv_timeouts += 1
        stats.deliv_end = max(stats.deliv_end, stats.last_frame_at)

    drain_task.cancel()
    try:
        await drain_task
    except asyncio.CancelledError:
        pass
    try:
        writer.close()
        await writer.wait_closed()
    except Exception:
        pass


async def worker_rtt(idx, cfg, stats, ready, start_event):
    """一对连接做乒乓。同一时刻只有 1 条消息在飞，延迟里没有压测端排队。"""
    nick_a, nick_b = f'a{idx}_{cfg.tag}', f'b{idx}_{cfg.tag}'
    try:
        ra, wa = await asyncio.open_connection(cfg.host, cfg.port)
        rb, wb = await asyncio.open_connection(cfg.host, cfg.port)
    except Exception as e:
        print(f'  连接失败 pair#{idx}: {e}', file=sys.stderr)
        stats.failures += 1
        ready.done(); ready.done()
        return

    ok = await login(ra, wa, nick_a) and await login(rb, wb, nick_b)
    if not ok:
        stats.failures += 1
        for w in (wa, wb):
            w.close()
        ready.done(); ready.done()
        return
    stats.logged_in += 2
    ready.done(); ready.done()

    await start_event.wait()

    target_a, target_b = nick_a.encode(), nick_b.encode()

    async def recv_chat(reader):
        while True:
            t, _ = await read_frame(reader, cfg.recv_timeout)
            if t is None:
                return False
            if t == CHAT:
                return True

    for _ in range(cfg.msgs):
        for writer, reader, target in ((wa, rb, target_b), (wb, ra, target_a)):
            ts = time.perf_counter()
            try:
                writer.write(pack(SAY, b'D' + target + b'\0' + TS.pack(ts) + b'x' * 24))
                await writer.drain()
            except Exception:
                stats.failures += 1
                return
            stats.sent += 1
            if not await recv_chat(reader):
                stats.failures += 1
                return
            stats.delivered += 1
            stats.deliv_lat.append(time.perf_counter() - ts)

    now = time.perf_counter()
    stats.send_end = max(stats.send_end, now)
    stats.send_end_epoch = stats.epoch_base + stats.send_end - stats.perf_base
    stats.deliv_end = max(stats.deliv_end, now)
    stats.last_frame_epoch = stats.send_end_epoch
    for w in (wa, wb):
        try:
            w.close()
        except Exception:
            pass


def sample(values, limit=2000):
    """多进程压测时把延迟样本抽稀后随 JSON 一起输出，供编排脚本合并分位数。"""
    if len(values) <= limit:
        return list(values)
    step = len(values) / limit
    return [values[int(i * step)] for i in range(limit)]


def pct(values, p):
    if not values:
        return 0.0
    s = sorted(values)
    return s[min(len(s) - 1, int(len(s) * p / 100))]


async def run(cfg):
    targets = [None] * cfg.conns
    if cfg.mode == 'private':
        half = cfg.conns // 2
        for i in range(cfg.conns):
            targets[i] = f'b{(i + half) % cfg.conns}_{cfg.tag}'

    stats = Stats()
    ready = Readiness(cfg.conns)
    start_event = asyncio.Event()

    if cfg.mode == 'rtt':
        ready = Readiness(cfg.conns)
        tasks = [asyncio.create_task(worker_rtt(i, cfg, stats, ready, start_event))
                 for i in range(cfg.conns // 2)]
    else:
        tasks = [asyncio.create_task(worker(i, cfg, stats, ready, start_event, targets))
                 for i in range(cfg.conns)]

    try:
        await asyncio.wait_for(ready.event.wait(), timeout=cfg.login_timeout)
    except asyncio.TimeoutError:
        print('登录阶段超时，用已就绪的连接继续', file=sys.stderr)

    t0 = time.perf_counter()
    stats.last_frame_at = t0
    stats.perf_base = t0
    stats.epoch_base = time.time()
    t0_epoch = stats.epoch_base
    ru0 = resource.getrusage(resource.RUSAGE_SELF)
    start_event.set()
    await asyncio.gather(*tasks)
    wall = time.perf_counter() - t0
    ru1 = resource.getrusage(resource.RUSAGE_SELF)
    t1_epoch = time.time()

    ingest_wall = max(stats.send_end - t0, 1e-9)
    deliver_wall = max(stats.deliv_end - t0, 1e-9)
    result = {
        'mode': cfg.mode,
        'conns': cfg.conns,
        'msgs': cfg.msgs,
        'logged_in': stats.logged_in,
        'failures': stats.failures,
        'recv_timeouts': stats.recv_timeouts,
        'sent': stats.sent,
        'delivered': stats.delivered,
        'wall': wall,
        # 测速窗口的服务端 CPU 采样要对齐这两个时刻（登录阶段的开销不算进去）
        't0_epoch': t0_epoch,
        't1_epoch': t1_epoch,
        't_send_end_epoch': stats.send_end_epoch or t0_epoch,
        't_last_frame_epoch': stats.last_frame_epoch or t0_epoch,
        'client_cpu_pct': ((ru1.ru_utime - ru0.ru_utime) +
                           (ru1.ru_stime - ru0.ru_stime)) / max(wall, 1e-9) * 100,
        'client_cpu_sec': (ru1.ru_utime - ru0.ru_utime) + (ru1.ru_stime - ru0.ru_stime),
        'lat_sample': sample(stats.deliv_lat),
        'ingest_wall': ingest_wall,
        'deliver_wall': deliver_wall,
        'ingest_qps': stats.sent / ingest_wall,
        'deliver_qps': (stats.delivered / deliver_wall) if stats.deliv_end else 0.0,
        'send_p50_ms': pct(stats.send_lat, 50) * 1e3,
        'send_p99_ms': pct(stats.send_lat, 99) * 1e3,
        'deliv_p50_ms': pct(stats.deliv_lat, 50) * 1e3,
        'deliv_p95_ms': pct(stats.deliv_lat, 95) * 1e3,
        'deliv_p99_ms': pct(stats.deliv_lat, 99) * 1e3,
        'deliv_max_ms': (max(stats.deliv_lat) * 1e3) if stats.deliv_lat else 0.0,
        'deliv_mean_ms': (statistics.mean(stats.deliv_lat) * 1e3) if stats.deliv_lat else 0.0,
    }
    return result


def report(r):
    print()
    print('=' * 62)
    print('压测结果')
    print('=' * 62)
    print(f'  模式            : {r["mode"]}    连接 {r["conns"]}   每连接 {r["msgs"]} 条')
    print(f'  登录成功/失败   : {r["logged_in"]}/{r["failures"]}')
    print(f'  发送消息数      : {r["sent"]:,}')
    print(f'  发送 QPS        : {r["ingest_qps"]:,.0f}   （发送端视角，含客户端开销）')
    if r['delivered']:
        print(f'  投递 Chat 帧数  : {r["delivered"]:,}')
        print(f'  投递 QPS        : {r["deliver_qps"]:,.0f}   （服务端真实扇出量）')
        print(f'  端到端延迟      : 平均 {r["deliv_mean_ms"]:.3f} ms | '
              f'P50 {r["deliv_p50_ms"]:.3f} | P95 {r["deliv_p95_ms"]:.3f} | '
              f'P99 {r["deliv_p99_ms"]:.3f} | 最大 {r["deliv_max_ms"]:.3f}')
    if r['recv_timeouts']:
        print(f'  ⚠ 收满超时的连接: {r["recv_timeouts"]}（投递一直没静止，可能有丢帧或压测端跟不上）')
    print('=' * 62)


def main():
    ap = argparse.ArgumentParser(description='聊天服务器压测客户端')
    ap.add_argument('--conns', type=int, default=100, help='并发连接数（默认 100）')
    ap.add_argument('--msgs', type=int, default=20, help='每连接消息数（默认 20）')
    ap.add_argument('--mode', choices=['say', 'private', 'ping', 'rtt'], default='say',
                    help='压测模式（默认 say）')
    ap.add_argument('--host', default=HOST)
    ap.add_argument('--port', type=int, default=PORT)
    ap.add_argument('--recv-timeout', type=float, default=10.0, help='等投递完成的上限（秒）')
    ap.add_argument('--quiet', type=float, default=0.5,
                    help='连续多少秒没有新 Chat 帧就认为投递结束（默认 0.5）')
    ap.add_argument('--login-timeout', type=float, default=30.0, help='登录阶段上限（秒）')
    ap.add_argument('--tag', default=str(int(time.time()) % 100000), help='昵称后缀，避免重名')
    ap.add_argument('--json', action='store_true', help='只输出一行 JSON，便于脚本对比')
    args = ap.parse_args()

    if args.conns < 2:
        print('连接数至少为 2', file=sys.stderr)
        return 1
    if args.mode in ('private', 'rtt') and args.conns % 2:
        print(f'{args.mode} 模式要求连接数为偶数', file=sys.stderr)
        return 1

    try:
        result = asyncio.run(run(args))
    except KeyboardInterrupt:
        print('\n中断', file=sys.stderr)
        return 130

    if args.json:
        print(json.dumps(result, ensure_ascii=False))
    else:
        report(result)
    return 0


if __name__ == '__main__':
    sys.exit(main())
