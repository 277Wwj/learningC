#pragma once
#include <cstdio>

// 一个轻量"调用跟踪器"：
// 构造时打印"进入某函数"，析构时打印"退出某函数"，
// 并用缩进表示调用嵌套层级，帮你直观看到程序里数据的流动路线。
class Trace{
    public:
    Trace (const char*func):func_(func){
        for(int i=0;i<depth_;i++) fprintf(stderr,"  ");   // 按嵌套深度缩进
        fprintf(stderr,"--> 进入 %s\n",func_);            // 打印"进入"
        depth_++;                                          // 深度 +1
    }
    ~Trace(){
        depth_--;                                          // 深度 -1
        for(int i=0;i<depth_;i++) fprintf(stderr,"  ");
        fprintf(stderr,"<-- 退出 %s\n",func_);            // 打印"退出"
    }
    private:
    const char* func_;
    static inline int depth_ = 0;   // C++17 内联静态成员，头文件里可直接定义，避免多文件重复定义
};