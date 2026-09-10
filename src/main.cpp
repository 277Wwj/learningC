
#include "Threadpool.hpp"
#include "simpleLogger.hpp"
#include <bits/stdc++.h>
using namespace std;
using ll = long long;
int main()
{
    const long long N = 1000000;

    Threadpool pool(4);
    auto future = pool.submit([]
                              { cout << "hello from threadpool" << this_thread::get_id() << endl; });
    auto future2 = pool.submit([](int a, int b) -> int
                               { return a + b; }, 3, 5);
    cout << "3+5=" << future2.get() << endl;
    ;
    future.get();
    auto sum_task = [](ll start, ll end) -> ll
    {
        ll sum = 0;
        for (ll i = start; i <= end; i++)
        {
            sum += i;
        }
        return sum;
    };
    auto start = chrono::high_resolution_clock::now();
    vector<std::future<ll> > futures;
    ll step = N / 4;
    for (int i = 0; i < 4; i++)
    {
        ll start_i = i * step + 1;
        ll end_i = (i == 3) ? N : (i + 1) * step;
        futures.emplace_back(pool.submit(sum_task, start_i, end_i));
    }
    ll total_sum = 0;
    for (auto &&fut : futures)
    {
        total_sum += fut.get();
    }
    auto end = chrono::high_resolution_clock::now();
    chrono::duration<double> duration = end - start;
    cout << "总和 from 1 to " << N << " is " << total_sum << endl;
    cout << "计算耗时: " << duration.count() << " seconds" << endl;
    // 单线程 对比
    start = chrono::high_resolution_clock::now();
    ll single_sum = sum_task(1, N);
    end = chrono::high_resolution_clock::now();
    duration = end - start;
    cout << "单线程总和 from 1 to " << N << " is " << single_sum << endl;
    cout << "单线程计算耗时: " << duration.count() << " seconds" << endl;
    // 测试日志记录
    SimpleLogger logger("log1.txt");
    for (int i = 0; i < 100; i++)
    {
        logger.log("这是第" + to_string(i) + "条日志");
    }
    this_thread::sleep_for(chrono::seconds(2));
    logger.log("日志记录测试完成");
    // 测试线程池和日志记录结合
    Threadpool pool2(4);
    SimpleLogger logger2("log2.txt");

    for (int i = 0; i < 100; i++)
    {
        pool2.submit([i, &logger2]
                     { logger2.log("线程池日志记录第" + to_string(i) + "条日志"); });
    }
    this_thread::sleep_for(chrono::seconds(2));
    return 0;
}