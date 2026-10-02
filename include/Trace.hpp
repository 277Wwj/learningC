#pragma once
#include <cstdio>
#include <cstdlib>

// 一个轻量"调用跟踪器"：
// 构造时打印"进入某函数"，析构时打印"退出某函数"，
// 并用缩进表示调用嵌套层级，帮你直观看到程序里数据的流动路线。
//
// ⚠️ 性能提醒（重要）：
//   fprintf(stderr, ...) 是很慢的 IO，而 Poller::poll / Channel::handleEvent
//   是"每个网络事件都会走一遍"的热路径。服务器一跑起来，stderr 会被刷爆，
//   压测数字也会被日志拖垮。
//   所以改成"默认关闭，需要看调用链时手动打开"：
//       TRACE=1 ./webserver
//       TRACE=1 ./src/chat_server
class Trace{
    public:
    Trace (const char*func):func_(func){
        if(!enabled()) return;                             // 关着就一点开销都不花
        for(int i=0;i<depth_;i++) fprintf(stderr,"  ");    // 按嵌套深度缩进
        fprintf(stderr,"--> 进入 %s\n",func_);              // 打印"进入"
        depth_++;                                           // 深度 +1
    }
    ~Trace(){
        if(!enabled()) return;
        depth_--;                                           // 深度 -1
        for(int i=0;i<depth_;i++) fprintf(stderr,"  ");
        fprintf(stderr,"<-- 退出 %s\n",func_);              // 打印"退出"
    }
    private:
    // 环境变量只读一次：函数内 static 保证"只初始化一次"，而且 C++11 起天然线程安全
    static bool enabled(){
        static const bool on = (getenv("TRACE") != nullptr);
        return on;
    }
    const char* func_;
    static inline int depth_ = 0;   // C++17 内联静态成员，头文件里可直接定义，避免多文件重复定义
};