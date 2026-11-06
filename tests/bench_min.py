#!/usr/bin/env python3
# 极简压测：单进程、只测 private、只打印 发送QPS / 投递QPS。
# 砍掉了 bench_chat.py 里的 多进程 / 静默检测 / CPU采样 / JSON 输出，只留骨架。
import asyncio, struct, sys, time

MAGIC, LOGIN, WELCOME, SAY, CHAT, PORT = 0x4C434831, 1, 2, 4, 5, 12735
TS = struct.Struct('>d')                                    # 正文前 8 字节 = 发送时刻

def pack(t, p=b''):                                         # 组帧：[长度][魔数][类型][payload]
    return struct.pack('>III', 8 + len(p), MAGIC, t) + p

async def read_frame(r):                                    # 拆帧
    bl, _, t = struct.unpack('>III', await r.readexactly(12))
    return t, (await r.readexactly(bl - 8) if bl > 8 else b'')

async def conn(i, N, msgs, st, go):
    try:
        r, w = await asyncio.open_connection('127.0.0.1', PORT)
    except OSError as e:
        if st['failed'] == 0:                # 只报一次，避免 100 条连接刷屏
            print(f'连接失败（服务器没启动？）：{e}', file=sys.stderr)
        st['failed'] += 1
        st['ready'] += 1                     # 也算"到齐"，否则主流程会一直等
        return
    w.write(pack(LOGIN, f'c{i}'.encode())); await w.drain()
    while (await read_frame(r))[0] != WELCOME: pass         # 登录：等到 Welcome
    st['ready'] += 1
    await go.wait()                                         # 所有人登录完一起起跑；否则先登录的先跑完

    async def reader():                                     # 常驻读协程：把服务端回包全部读掉
        while True:
            t, p = await read_frame(r)
            if t != CHAT: continue                          # Users 名单帧不算投递
            st['delivered'] += 1
            st['last'] = time.perf_counter()          # 最后一帧到达的时刻
            body = p[p.find(b'\0', 1) + 1:]                 # Chat 正文 = [模式][昵称\0][时间戳+正文]
            st['lat'].append(time.perf_counter() - TS.unpack(body[:8])[0])

    rt = asyncio.create_task(reader())
    target = f'c{(i + N // 2) % N}'.encode()                # 前半发给后半
    for _ in range(msgs):
        w.write(pack(SAY, b'D' + target + b'\0' + TS.pack(time.perf_counter()) + b'x' * 24))
        await w.drain()
        st['sent'] += 1
    st['send_end'] = max(st['send_end'], time.perf_counter())
    await asyncio.sleep(2)                                  # 简化：定死等 2 秒收尾
    rt.cancel(); w.close()

async def main(N, msgs):
    st = {'ready': 0, 'failed': 0, 'sent': 0, 'delivered': 0,
          'lat': [], 'send_end': 0.0, 'last': 0.0}
    go = asyncio.Event()
    tasks = [asyncio.create_task(conn(i, N, msgs, st, go)) for i in range(N)]
    t_login = time.perf_counter()
    while st['ready'] < N:                                  # 等登录完成
        if time.perf_counter() - t_login > 10:              # 别无限等：卡死时给个明确报错
            print(f'登录超时：{st["ready"]}/{N} 就绪', file=sys.stderr)
            break
        await asyncio.sleep(0.01)
    if st['failed']:
        print(f'{st["failed"]}/{N} 条连接失败，先启动服务器：'
              f'CHAT_LOG=0 SUB_LOOPS=3 ./build-release/chat_server &', file=sys.stderr)
        return
    t0 = time.perf_counter(); go.set()
    await asyncio.gather(*tasks)
    send_wall = max(st['send_end'] - t0, 1e-9)
    lat = sorted(st['lat'])
    print(f'发送 QPS : {st["sent"] / send_wall:,.0f}    （{st["sent"]} 条 / {send_wall:.2f}s）')
    deliv_wall = max(st['last'] - t0, 1e-9)
    print(f'投递 QPS : {st["delivered"] / deliv_wall:,.0f}    （服务端回了 {st["delivered"]} 个 Chat 帧）')
    if lat:
        print(f'投递延迟 : P50 {lat[len(lat)//2]*1e3:.2f} ms | '
              f'P99 {lat[int(len(lat)*.99)]*1e3:.2f} ms')

asyncio.run(main(int(sys.argv[1]), int(sys.argv[2])))
