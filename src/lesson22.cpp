#include <bits/stdc++.h>
using namespace std;

int g_shared = 0;   // 全局变量：所有线程共享

void worker(int id) {
    int local = id;             // 局部变量：每个线程独享（各自栈上）
    g_shared++;                 // 改同一个共享变量
    cout << "线程" << id << ": local=" << local
         << " (独享), g_shared 的地址=" << &g_shared
         << " (两个线程地址相同=共享)\n";
}

int main() {
    thread t1(worker, 1);
    thread t2(worker, 2);
    t1.join();
    t2.join();
    cout << "最终 g_shared = " << g_shared << "\n";
    return 0;
}