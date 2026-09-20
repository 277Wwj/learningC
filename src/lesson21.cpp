#include <bits/stdc++.h>
using namespace std;

int main() {
    cout << "--- 观察 vector 扩容 ---\n";
    vector<int> v;
    cout << "初始: size=" << v.size() << " capacity=" << v.capacity() << "\n";
    for (int i = 0; i < 10; i++) {
        v.push_back(i);
        cout << "插入 " << i << " 后: size=" << v.size()
             << " capacity=" << v.capacity() << "\n";
    }

    cout << "\n--- reserve 预留空间 ---\n";
    vector<int> v2;
    v2.reserve(100);
    cout << "reserve(100) 后: size=" << v2.size()
         << " capacity=" << v2.capacity() << "\n";
    return 0;
}