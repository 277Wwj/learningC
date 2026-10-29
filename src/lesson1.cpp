// ═══════════════════════════════════════════════════════════════════════════
// lesson1.cpp —— 第 1 课（2026-07-29）：从零开始，第一个线程池
//
// 这是整条学习轨迹的起点。原件在 ~/桌面/threadpool.cpp，只有 317 字节，
// 写完类框架就卡住了，而且有 5 个错误。原件长这样：
//
//     #include<bits/stdc++.h>
//     using namespace std;
//     string gerTid (){                    // ① 拼写：应是 getTid
//       ostringstream oss;
//       oss<<this_thread::get_gid();       // ② get_gid 不存在，应是 get_id
//       return oss.str();
//     }
//     class ThreadPool{
//       pubilc:                            // ③ 拼写：应是 public
//       explicit ThreadPool(size_t threadNum):stop_(false)
//       {
//         cout<<"[pool初始化，准备创建]"<<threadNum<<"个人作品"   // ④ 缺分号
//       }                                  // ⑤ 构造函数里没创建任何线程
//
//     };
//     int main(){
//     }
//
// 本文件把那天的目标补齐，保持"第 1 课"的难度：
//   只用 mutex + condition_variable + queue，不引入 packaged_task / future
//   （那是 8/14 的 src/main.cpp 才加上的）。
//
// 编译运行：
//   g++ src/lesson1.cpp -o lesson1 -std=c++17 -pthread
//   ./lesson1
//
// 配套观察（第 2 课，7/31）：
//   ./lesson1 &                       # 后台跑起来
//   ps -T -p $! -o pid,spid,stat,comm # 看这个进程里的每个线程
//   htop                              # 或者直接看线程列表
// ═══════════════════════════════════════════════════════════════════════════

#include <bits/stdc++.h>
using namespace std;

// 把当前线程的 id 转成字符串，方便打印。
// 第 2 课用 ps -T 看到的 spid，和这里打印出来的就是同一个线程。
string getTid()
{
    ostringstream oss;
    oss << this_thread::get_id();
    return oss.str();
}

class ThreadPool
{
public:
    // explicit：防止 size_t 被隐式转换成 ThreadPool
    explicit ThreadPool(size_t threadNum) : stop_(false)
    {
        cout << "[pool 初始化，准备创建 " << threadNum << " 个线程]" << endl;
        for (size_t i = 0; i < threadNum; i++)
        {
            // 线程一创建就开始跑 worker()，在 cv_.wait 处等着被唤醒
            workers_.emplace_back(&ThreadPool::worker, this);
        }
        cout << "[创建完成]" << endl;
    }

    ~ThreadPool()
    {
        {
            // 先置 stop_，通知所有 worker 退出。
            // 这个花括号很关键：lock_guard 必须在 notify 之前析构，
            // 否则 worker 被唤醒后还要抢这把锁 → 双向等待 → 死锁。
            lock_guard<mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();

        for (auto &worker : workers_)
        {
            if (worker.joinable())
                worker.join();
        }
        cout << "[pool 已关闭，所有线程已回收]" << endl;
    }

    ThreadPool(const ThreadPool &) = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;

    void submit(function<void()> task)
    {
        {
            lock_guard<mutex> lock(mutex_);
            if (stop_)
                return;
            tasks_.push(move(task));
        }
        cv_.notify_one();   // 唤醒一个 worker（不是在锁里做的事）
    }

private:
    void worker()
    {
        cout << "  线程 " << getTid() << " 启动" << endl;
        while (true)
        {
            function<void()> task;
            {
                unique_lock<mutex> lock(mutex_);
                // 谓词版 wait：醒来后自己再检查一次条件，
                // 避免虚假唤醒（spurious wakeup），也不需要手写 while 循环
                cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });

                // 要关了、而且任务也取完了 → 退出
                if (stop_ && tasks_.empty())
                    return;

                task = move(tasks_.front());
                tasks_.pop();
            }   // 锁在这里释放，下面执行任务时不持锁

            task();
        }
    }

    vector<thread> workers_;
    queue<function<void()>> tasks_;
    mutex mutex_;
    condition_variable cv_;
    atomic<bool> stop_;
};

int main()
{
    cout << "主线程 " << getTid() << endl << endl;

    ThreadPool pool(4);

    cout << endl << "提交 8 个任务给 4 个线程：" << endl;
    for (int i = 0; i < 8; i++)
    {
        pool.submit([i] {
            cout << "  任务 " << i << " 由线程 " << getTid() << " 执行" << endl;
            this_thread::sleep_for(200ms);   // 假装在干活
        });
    }

    // 等任务跑完，再让 pool 析构。真实项目里不该用 sleep 等，
    // 得靠 future::get() 之类的机制 —— 那是 main.cpp 那一课的内容。
    this_thread::sleep_for(2s);

    cout << endl << "离开 main 的作用域 → pool 析构 → join 所有线程" << endl;
    return 0;
}
