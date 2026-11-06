            #!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""压测对比编排：单 Reactor vs 主从 Reactor。

它负责把"公平对比"这件事做对：
  1. 每组都重启服务端，跑 --repeat 轮，结果取中位数（单跑一次会被调度抖动骗）；
  2. 同时采样服务端 CPU（/proc/<pid>/stat）和每个线程的 CPU，
     用来判断"是不是已经把单核打满了""主从版到底有没有真用上多核"；
  3. 报告压测端 CPU：压测端先到瓶颈时，QPS 数字根本不能用来判定服务端；
  4. 关掉服务端逐条日志（CHAT_LOG=0），否则测到的是 fprintf 的速度；
  5. 聊天压测默认开多个客户端进程（脚本自己就是瓶颈时，单进程推不满服务器）。

用法：
  python3 tests/bench_compare.py --build                        # chat + http 全套
  python3 tests/bench_compare.py --suite chat --sub-loops 1,2,3 --mode private
  python3 tests/bench_compare.py --suite chat --baseline-ref 0fb7518   # 加改造前的单线程版
  python3 tests/bench_compare.py --suite http --repeat 3
"""

import argparse
import bisect
import json
import os
import re
import resource
import signal
import socket
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / 'build-release'
TICK = os.sysconf('SC_CLK_TCK')          # 每秒多少 CPU tick
HTTP_PORT = 8080
CHAT_PORT = 12735


# --------------------------------------------------------------------------
# /proc 采样
# --------------------------------------------------------------------------
def proc_ticks(pid):
    """返回 (进程总 tick, {tid: tick})。进程没了返回 (None, {})"""
    try:
        fields = Path(f'/proc/{pid}/stat').read_text().rsplit(') ', 1)[1].split()
        total = int(fields[11]) + int(fields[12])      # utime + stime
    except (FileNotFoundError, ProcessLookupError, IndexError):
        return None, {}

    threads = {}
    try:
        for task in Path(f'/proc/{pid}/task').iterdir():
            try:
                t = (task / 'stat').read_text().rsplit(') ', 1)[1].split()
                threads[int(task.name)] = int(t[11]) + int(t[12])
            except (FileNotFoundError, ProcessLookupError, IndexError, ValueError):
                continue
    except (FileNotFoundError, ProcessLookupError):
        pass
    return total, threads


class CpuSampler(threading.Thread):
    """按 20ms 采样服务端 CPU，之后在压测端报告的 [t0, t1] 里插值取差值。

    为什么不"跑前读一次、跑后读一次"？
    登录阶段（一堆连接上线 + 名单广播）开销不小，一次性读会把它算进测速窗口。
    """

    def __init__(self, pid, interval=0.02):
        super().__init__(daemon=True)
        self.pid = pid
        self.interval = interval
        self.samples = []
        self._stopped = threading.Event()

    def run(self):
        while not self._stopped.is_set():
            ts = time.time()
            total, threads = proc_ticks(self.pid)
            if total is not None:
                self.samples.append((ts, total, threads))
            self._stopped.wait(self.interval)

    def stop(self):
        self._stopped.set()
        self.join(timeout=1.0)

    def at(self, ts):
        """线性插值出 ts 时刻的 (总 tick, {tid: tick})"""
        if not self.samples:
            return None, {}
        if ts <= self.samples[0][0]:
            return self.samples[0][1], self.samples[0][2]
        if ts >= self.samples[-1][0]:
            return self.samples[-1][1], self.samples[-1][2]
        i = bisect.bisect_left([x[0] for x in self.samples], ts)
        t0, v0, h0 = self.samples[i - 1]
        t1, v1, h1 = self.samples[i]
        if t1 <= t0:
            return v1, h1
        k = (ts - t0) / (t1 - t0)
        total = v0 + (v1 - v0) * k
        threads = {tid: h0.get(tid, 0) + (h1.get(tid, 0) - h0.get(tid, 0)) * k
                   for tid in set(h0) | set(h1)}
        return total, threads

    def window(self, t0, t1):
        """返回 [t0, t1] 区间的 (服务端CPU%, 最忙线程CPU%, 热线程数)"""
        wall = t1 - t0
        if wall <= 0:
            return 0.0, 0.0, 0
        a, ta = self.at(t0)
        b, tb = self.at(t1)
        if a is None or b is None:
            return 0.0, 0.0, 0
        pct = (b - a) / TICK / wall * 100
        per_thread = [(tb.get(tid, 0) - ta.get(tid, 0)) / TICK / wall * 100
                      for tid in set(ta) | set(tb)]
        per_thread = [x for x in per_thread if x > 0]
        return pct, (max(per_thread) if per_thread else 0.0), \
            sum(1 for x in per_thread if x > 10.0)


def cpu_window(cpu0, th0, cpu1, th1, wall):
    """一次性采样版（HTTP 用：没有登录阶段，前后对比就够）"""
    if cpu0 is None or cpu1 is None or wall <= 0:
        return 0.0, 0.0, 0
    pct = (cpu1 - cpu0) / TICK / wall * 100
    per_thread = [(v - th0.get(tid, 0)) / TICK / wall * 100 for tid, v in th1.items()]
    per_thread = [x for x in per_thread if x > 0]
    return pct, (max(per_thread) if per_thread else 0.0), \
        sum(1 for x in per_thread if x > 10.0)


# --------------------------------------------------------------------------
# 进程启停 / 小工具
# --------------------------------------------------------------------------
def ensure_port_free(host, port):
    """端口被占时直接报错。

    否则很可能连上的是"上一次没关掉的旧服务端"：新进程 bind 失败秒退，
    压测却打在旧进程上，数据全废（而且 CPU 会采样成 0，很难看出哪里错了）。
    """
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            s.bind((host, port))
        except OSError:
            raise RuntimeError(
                f'{host}:{port} 已被占用，先用 `ss -lptn "sport = :{port}"` 查并关掉占用的进程')


def wait_port(host, port, proc, timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f'服务端启动失败（退出码 {proc.returncode}）')
        try:
            with socket.create_connection((host, port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError(f'等待 {host}:{port} 监听超时')


def stop_server(proc):
    if proc.poll() is None:
        proc.send_signal(signal.SIGINT)     # 让服务端走优雅退出
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


def median(xs):
    xs = [x for x in xs if x is not None]
    return statistics.median(xs) if xs else 0.0


def taskset(cpus):
    return ['taskset', '-c', cpus] if cpus else []


# --------------------------------------------------------------------------
# 聊天服务器
# --------------------------------------------------------------------------
def build_baseline(ref):
    """从 git 历史取出改造前的单线程 chat_server，编成对照可执行文件。"""
    out = subprocess.run(['git', 'show', f'{ref}:src/chat_server.cpp'],
                         cwd=ROOT, capture_output=True, text=True, check=True)
    src = out.stdout

    # 旧版本没有 CHAT_LOG 开关，插入同样的静音逻辑，保证两边测量条件一致。
    old_sig = 'void logLine(const char* fmt, ...)\n{\n    char ts[32];'
    new_sig = ('void logLine(const char* fmt, ...)\n{\n'
               '    static const bool on = [] {\n'
               '        const char* e = ::getenv("CHAT_LOG");\n'
               "        return !(e != nullptr && e[0] == '0' && e[1] == '\\0');\n"
               '    }();\n'
               '    if (!on) return;\n'
               '    char ts[32];')
    if old_sig not in src:
        raise RuntimeError(f'{ref} 里找不到 logLine 的签名，无法插入静音开关')
    src = src.replace(old_sig, new_sig)
    if '#include <cstdlib>' not in src:
        src = src.replace('#include <ctime>', '#include <cstdlib>\n#include <ctime>', 1)

    BUILD.mkdir(exist_ok=True)
    cpp = BUILD / f'chat_server_baseline_{ref[:7]}.cpp'
    bin_ = BUILD / f'chat_server_baseline_{ref[:7]}'
    cpp.write_text(src)
    subprocess.run(['g++', '-std=c++17', '-O2', '-I', str(ROOT / 'include'),
                    str(cpp), '-o', str(bin_), '-pthread'], check=True)
    return bin_


def merge_chat(runs):
    """把多个压测进程的结果合成一份。"""
    t0 = min(r['t0_epoch'] for r in runs)
    t1 = max(r['t1_epoch'] for r in runs)
    wall = max(t1 - t0, 1e-9)
    sent = sum(r['sent'] for r in runs)
    delivered = sum(r['delivered'] for r in runs)

    lat = []
    for r in runs:
        lat.extend(r.get('lat_sample') or [])
    lat.sort()

    def pct(q):
        if not lat:
            return 0.0
        return lat[min(len(lat) - 1, int(len(lat) * q / 100))] * 1e3

    return {
        't0_epoch': t0,
        't1_epoch': t1,
        'wall': wall,
        'logged_in': sum(r['logged_in'] for r in runs),
        'failures': sum(r['failures'] for r in runs),
        'recv_timeouts': sum(r['recv_timeouts'] for r in runs),
        'sent': sent,
        'delivered': delivered,
        'ingest_qps': sent / max(max(r['t_send_end_epoch'] for r in runs) - t0, 1e-9),
        'deliver_qps': delivered / max(
            max(r['t_last_frame_epoch'] for r in runs) - t0, 1e-9) if delivered else 0.0,
        'deliv_p50_ms': pct(50),
        'deliv_p99_ms': pct(99),
        'client_cpu_pct': sum(r['client_cpu_sec'] for r in runs) / wall * 100,
        'procs': len(runs),
    }


def run_chat_once(binary, env_extra, cfg):
    env = os.environ.copy()
    env.update(env_extra)
    env['CHAT_LOG'] = '0'

    ensure_port_free(cfg.host, cfg.port)
    srv = subprocess.Popen(taskset(cfg.server_cpus) + [str(binary)], env=env,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    sampler = None
    procs = []
    try:
        wait_port(cfg.host, cfg.port, srv)
        time.sleep(0.2)                      # 让子 loop 线程都跑起来
        sampler = CpuSampler(srv.pid)
        sampler.start()

        procs = []
        for i in range(cfg.client_procs):
            cpu = None
            if cfg.pin:
                cpu = str(cfg.client_cpu_list[i % len(cfg.client_cpu_list)])
            cmd = taskset(cpu) + [
                sys.executable, str(ROOT / 'tests' / 'bench_chat.py'), '--json',
                '--conns', str(cfg.per_conns), '--msgs', str(cfg.msgs),
                '--mode', cfg.mode, '--recv-timeout', str(cfg.recv_timeout),
                '--tag', f'p{i}r{cfg.run_seq}',
            ]
            procs.append(subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                          stderr=subprocess.PIPE, text=True))
        outs = [p.communicate(timeout=cfg.timeout) for p in procs]
        crashed = srv.poll() is not None
    finally:
        if sampler:
            sampler.stop()
        stop_server(srv)

    results = []
    for p, (out, err) in zip(procs, outs):
        if p.returncode != 0 or not out.strip():
            raise RuntimeError(f'压测客户端失败（rc={p.returncode}）：{err.strip()[:300]}')
        results.append(json.loads(out.strip().splitlines()[-1]))

    r = merge_chat(results)
    r['server_cpu_pct'], r['max_thread_pct'], r['hot_threads'] = sampler.window(
        r['t0_epoch'], r['t1_epoch'])
    r['crashed'] = crashed
    return r


# --------------------------------------------------------------------------
# HTTP
# --------------------------------------------------------------------------
def run_http_once(binary, cfg):
    ensure_port_free('127.0.0.1', HTTP_PORT)
    srv = subprocess.Popen(taskset(cfg.server_cpus) + [str(binary)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_port('127.0.0.1', HTTP_PORT, srv)
        time.sleep(0.2)
        cpu0, th0 = proc_ticks(srv.pid)
        ru0 = resource.getrusage(resource.RUSAGE_CHILDREN)

        cmd = taskset(cfg.client_cpus) + [
            'wrk', '-t', str(cfg.http_threads), '-c', str(cfg.http_conns),
            '-d', f'{cfg.duration}s', '--latency', f'http://127.0.0.1:{HTTP_PORT}/']
        t_a = time.time()
        client = subprocess.run(cmd, capture_output=True, text=True,
                                timeout=cfg.duration + 60)
        wall = time.time() - t_a
        ru1 = resource.getrusage(resource.RUSAGE_CHILDREN)

        cpu1, th1 = proc_ticks(srv.pid)
        crashed = srv.poll() is not None
    finally:
        stop_server(srv)

    m = re.search(r'Requests/sec:\s+([\d.]+)', client.stdout)
    if not m:
        raise RuntimeError(f'wrk 失败：{client.stderr.strip()[:400]}')
    lm = re.search(r'Latency\s+([\d.]+\w+)\s+([\d.]+\w+)\s+([\d.]+\w+)', client.stdout)

    pct, _, hot = cpu_window(cpu0, th0, cpu1, th1, wall)
    return {
        'qps': float(m.group(1)),
        'latency': lm.group(2) if lm else '-',      # 50% 分位
        'server_cpu_pct': pct,
        'hot_threads': hot,
        'client_cpu_pct': ((ru1.ru_utime - ru0.ru_utime) +
                           (ru1.ru_stime - ru0.ru_stime)) / max(wall, 1e-9) * 100,
        'crashed': crashed,
    }


# --------------------------------------------------------------------------
# 输出
# --------------------------------------------------------------------------
def print_chat_table(rows):
    width = 116
    print()
    print('=' * width)
    print(f'聊天服务器对比（{rows[0]["mode"]} 模式，每格为 {rows[0]["repeat"]} 轮中位数，'
          f'压测端 {rows[0]["procs"]} 进程）')
    print('=' * width)
    print(f'{"变体":<18}{"连接/消息":>12}{"发送QPS":>12}{"投递QPS":>12}'
          f'{"延迟P50":>10}{"延迟P99":>10}{"服务端CPU%":>12}{"热线程":>8}'
          f'{"压测端CPU%":>12}{"超时":>6}')
    print('-' * width)
    for r in rows:
        print(f'{r["label"]:<18}{f"{r["conns"]}/{r["msgs"]}":>12}'
              f'{r["ingest_qps"]:>12,.0f}{r["deliver_qps"]:>12,.0f}'
              f'{r["deliv_p50_ms"]:>10.2f}{r["deliv_p99_ms"]:>10.2f}'
              f'{r["server_cpu_pct"]:>12.1f}{r["hot_threads"]:>8.0f}'
              f'{r["client_cpu_pct"]:>12.1f}{r["recv_timeouts"]:>6.0f}')
    print('=' * width)
    print('  发送QPS   ：客户端视角（含压测端自身开销，可能先于服务端到顶）')
    print('  投递QPS   ：服务端实际投递的 Chat 帧 / 秒（say 模式约为 发送QPS×(人数-1)）')
    print('  延迟      ：消息体带时间戳，接收端收到时的端到端延迟（含压测端排队，'
          '要纯净延迟用 --mode rtt）')
    print('  热线程    ：CPU 占用 >10% 的服务端线程数 ≈ 真正用到的核数')
    print('  压测端CPU%：多进程会累加；接近"可用核数×100"时压测端先到瓶颈，'
          '这组数字不能用来判定服务端')


def print_http_table(rows, cfg):
    width = 104
    print()
    print('=' * width)
    print(f'HTTP 对比（wrk -t{cfg.http_threads} -c{cfg.http_conns} -d{cfg.duration}s，'
          f'每格为 {cfg.repeat} 轮中位数）')
    print('=' * width)
    print(f'{"版本":<28}{"QPS":>12}{"延迟P50":>12}{"服务端CPU%":>12}'
          f'{"热线程":>8}{"压测端CPU%":>12}')
    print('-' * width)
    for r in rows:
        print(f'{r["label"]:<28}{r["qps"]:>12,.0f}{r["latency"]:>12}'
              f'{r["server_cpu_pct"]:>12.1f}{r["hot_threads"]:>8.0f}'
              f'{r["client_cpu_pct"]:>12.1f}')
    print('=' * width)


# --------------------------------------------------------------------------
# 套件
# --------------------------------------------------------------------------
def chat_variants(cfg):
    variants = []
    if cfg.baseline_ref:
        print(f'[*] 从 git 历史编译改造前的单线程版本：{cfg.baseline_ref}')
        variants.append(('单线程(改造前)', build_baseline(cfg.baseline_ref), {}))
    for n in cfg.sub_loop_list:
        variants.append((f'SUB_LOOPS={n}', BUILD / 'chat_server', {'SUB_LOOPS': str(n)}))
    return variants


def run_chat_suite(cfg):
    rows = []
    for label, binary, env_extra in chat_variants(cfg):
        runs = []
        for i in range(cfg.repeat):
            cfg.run_seq = i
            print(f'[*] {label} 第 {i + 1}/{cfg.repeat} 轮 ...', flush=True)
            try:
                r = run_chat_once(binary, env_extra, cfg)
            except Exception as e:
                print(f'    [!] 跳过：{e}')
                break
            if r.get('crashed'):
                print('    [!] 服务端在压测过程中退出，这轮数据不可信')
            if cfg.verbose:
                print(f'    轮 {i + 1}: 发送 {r["ingest_qps"]:,.0f} qps | 投递 '
                      f'{r["deliver_qps"]:,.0f} qps | 服务端 {r["server_cpu_pct"]:.1f}% CPU | '
                      f'热线程 {r["hot_threads"]:.0f} | 压测端 {r["client_cpu_pct"]:.0f}%')
            runs.append(r)
        if not runs:
            continue
        rows.append({
            'label': label,
            'conns': cfg.per_conns * cfg.client_procs,
            'msgs': cfg.msgs,
            'mode': cfg.mode,
            'repeat': len(runs),
            'procs': cfg.client_procs,
            'ingest_qps': median([r['ingest_qps'] for r in runs]),
            'deliver_qps': median([r['deliver_qps'] for r in runs]),
            'deliv_p50_ms': median([r['deliv_p50_ms'] for r in runs]),
            'deliv_p99_ms': median([r['deliv_p99_ms'] for r in runs]),
            'server_cpu_pct': median([r['server_cpu_pct'] for r in runs]),
            'hot_threads': median([r['hot_threads'] for r in runs]),
            'client_cpu_pct': median([r['client_cpu_pct'] for r in runs]),
            'recv_timeouts': median([r['recv_timeouts'] for r in runs]),
        })
    if rows:
        print_chat_table(rows)


def run_http_suite(cfg):
    pairs = [('单线程 webserver', 'webserver'),
             ('主从 webserver_multithread', 'webserver_multithread')]
    rows = []
    for label, name in pairs:
        runs = []
        for i in range(cfg.repeat):
            print(f'[*] {label} 第 {i + 1}/{cfg.repeat} 轮 ...', flush=True)
            try:
                r = run_http_once(BUILD / name, cfg)
            except Exception as e:
                print(f'    [!] 跳过：{e}')
                break
            if r.get('crashed'):
                print('    [!] 服务端在压测过程中退出，这轮数据不可信')
            runs.append(r)
        if not runs:
            continue
        rows.append({
            'label': label,
            'qps': median([r['qps'] for r in runs]),
            'latency': runs[0]['latency'],
            'server_cpu_pct': median([r['server_cpu_pct'] for r in runs]),
            'hot_threads': median([r['hot_threads'] for r in runs]),
            'client_cpu_pct': median([r['client_cpu_pct'] for r in runs]),
            'procs': 0,
        })
    if rows:
        print_http_table(rows, cfg)


def main():
    cpus = sorted(os.sched_getaffinity(0))
    default_procs = max(1, min(4, len(cpus) - 1))

    ap = argparse.ArgumentParser(description='单 Reactor vs 主从 Reactor 压测对比')
    ap.add_argument('--suite', choices=['chat', 'http', 'all'], default='all')
    ap.add_argument('--sub-loops', default=','.join(str(i) for i in range(1, max(2, len(cpus) - 1))),
                    help='要对比的 SUB_LOOPS 取值，逗号分隔（默认按核数推导）')
    ap.add_argument('--baseline-ref', default=None,
                    help='额外对比一个 git 历史版本（如 0fb7518 = 改造前的单线程版）')
    ap.add_argument('--conns', type=int, default=100, help='聊天总连接数（默认 100）')
    ap.add_argument('--msgs', type=int, default=1000, help='每连接消息数（默认 1000）')
    ap.add_argument('--mode', choices=['say', 'private', 'ping', 'rtt'], default='private',
                    help='聊天压测模式（默认 private：压测端开销小，最不容易先到瓶颈）')
    ap.add_argument('--client-procs', type=int, default=default_procs,
                    help=f'聊天压测端进程数（默认 {default_procs}；多进程才推得满服务器）')
    ap.add_argument('--repeat', type=int, default=3, help='每种配置跑几轮取中位数（默认 3）')
    ap.add_argument('--duration', type=int, default=10, help='wrk 每轮压测秒数（默认 10）')
    ap.add_argument('--http-conns', type=int, default=1000, help='wrk 连接数（默认 1000）')
    ap.add_argument('--http-threads', type=int, default=4, help='wrk 线程数（默认 4）')
    ap.add_argument('--recv-timeout', type=float, default=15.0)
    ap.add_argument('--timeout', type=float, default=300.0, help='单轮压测客户端上限（秒）')
    ap.add_argument('--pin', action='store_true',
                    help='把服务端钉在第一个核、每个压测进程钉一个核（默认不绑核，交给调度器）')
    ap.add_argument('--server-cpus', default=None, help='配合 --pin 指定服务端核，如 0-1')
    ap.add_argument('--client-cpus', default=None, help='配合 --pin 指定压测端核，如 2,3,4')
    ap.add_argument('--verbose', action='store_true', help='打印每一轮的原始数据（看抖动）')
    ap.add_argument('--build', action='store_true', help='先 cmake 编译 Release 版本')
    args = ap.parse_args()

    if len(cpus) < 2:
        print('至少需要 2 个 CPU 核', file=sys.stderr)
        return 1

    args.host = '127.0.0.1'
    args.port = CHAT_PORT
    args.sub_loop_list = [int(x) for x in args.sub_loops.split(',') if x.strip()]
    args.client_procs = max(1, args.client_procs)

    # 每个压测进程分到的连接数；private/rtt 要求偶数
    per = max(2, args.conns // args.client_procs)
    if args.mode in ('private', 'rtt') and per % 2:
        per -= 1
    args.per_conns = per
    args.run_seq = 0

    # 绑核配置
    if args.pin:
        args.server_cpus = args.server_cpus or str(cpus[0])
        if args.client_cpus:
            args.client_cpu_list = [int(x) for x in args.client_cpus.replace('-', ',').split(',') if x]
        else:
            args.client_cpu_list = cpus[1:]
        args.client_cpus = args.client_cpus or ','.join(str(c) for c in args.client_cpu_list)
        if len(args.client_cpu_list) < args.client_procs:
            print(f'[!] 压测进程 {args.client_procs} 个，但只给了 {len(args.client_cpu_list)} 个核')
    else:
        args.server_cpus = args.server_cpus or None
        args.client_cpus = args.client_cpus or None
        args.client_cpu_list = [None]

    if args.build:
        print('[*] 编译 Release ...')
        subprocess.run(['cmake', '-S', str(ROOT), '-B', str(BUILD),
                        '-DCMAKE_BUILD_TYPE=Release'], check=True, stdout=subprocess.DEVNULL)
        subprocess.run(['cmake', '--build', str(BUILD), '-j', str(len(cpus))],
                       check=True, stdout=subprocess.DEVNULL)

    print()
    print(f'机器核数        : {len(cpus)}（可用 {cpus[0]}-{cpus[-1]}）')
    print(f'绑核            : {"服务端 " + str(args.server_cpus) + " / 压测端 " + str(args.client_cpus) if args.pin else "不绑核"}')
    print(f'聊天压测        : {args.mode} 模式，{args.client_procs} 个压测进程 × '
          f'{args.per_conns} 连接 × {args.msgs} 条（合计 {args.per_conns * args.client_procs} 连接）')
    print(f'编译产物目录    : {BUILD}')
    if not (BUILD / 'chat_server').exists():
        print(f'[!] 找不到 {BUILD / "chat_server"}，请加 --build', file=sys.stderr)
        return 1

    try:
        if args.suite in ('chat', 'all'):
            run_chat_suite(args)
        if args.suite in ('http', 'all'):
            run_http_suite(args)
    except KeyboardInterrupt:
        print('\n中断', file=sys.stderr)
        return 130

    print()
    print('怎么读这张表：')
    print('  1. 先看"服务端CPU%"：单线程版接近 100%（一个核打满）才说明瓶颈在服务端单核，')
    print('     主从版才有提升空间；如果只有 20~40%，说明瓶颈在别处（压测端/回环/内核），')
    print('     这时换主从只会多一次跨线程投递，QPS 甚至可能下降。')
    print('  2. 再看"热线程"：SUB_LOOPS=3 却只有 1 个热线程 → 连接没分出去，或全局锁、')
    print('     主 loop 成了串行点（accept、Users 广播、nameIndex_ 写锁都会串行化）。')
    print('  3. 最后看"压测端CPU%"：它先到顶时，降低连接数/消息数或加 --client-procs，')
    print('     否则测的是压测脚本而不是服务器。')
    return 0


if __name__ == '__main__':
    sys.exit(main())
