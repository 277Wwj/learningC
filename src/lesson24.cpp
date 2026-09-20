#include <bits/stdc++.h>
using namespace std;

mutex m1, m2;

// 死锁版：两个线程按"相反顺序"拿锁
void threadA() {
    m1.lock();
    cout << "A 拿到 m1\n";
    this_thread::sleep_for(100ms);   // 给 B 时间拿 m2
    m2.lock();                       // A 等 m2
    cout << "A 拿到 m2\n";
    m2.unlock(); m1.unlock();
}

void threadB() {
    m2.lock();
    cout << "B 拿到 m2\n";
    this_thread::sleep_for(100ms);   // 给 A 时间拿 m1
    m1.lock();                       // B 等 m1 → 死锁！
    cout << "B 拿到 m1\n";
    m1.unlock(); m2.unlock();
}

int main() {
    cout << "两个线程按相反顺序加锁，即将死锁...\n";
    thread t1(threadA);
    thread t2(threadB);
    t1.join();   // 永远等不到
    t2.join();
    return 0;
}