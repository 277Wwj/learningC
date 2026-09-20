#include <bits/stdc++.h>
using namespace std;

int g_global = 0;          // 全局变量：数据段

void func() {
    int stack_var = 0;             // 栈变量
    int* heap_var = new int(0);    // 堆变量
    static int s_static = 0;       // 静态变量：数据段

    cout << "栈变量地址:   " << &stack_var << "  (栈)\n";
    cout << "堆变量地址:   " << heap_var  << "  (堆)\n";
    cout << "全局变量地址: " << &g_global  << "  (数据段)\n";
    cout << "静态变量地址: " << &s_static << "  (数据段)\n";
    delete heap_var;
}

int main() {
    func();
    return 0;
}