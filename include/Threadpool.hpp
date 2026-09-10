#pragma once
#include<bits/stdc++.h>
#include<fcntl.h>
using namespace std;
class Threadpool{
    public:
    Threadpool(size_t threadCount=thread::hardware_concurrency()):
    stop_(false){
        for(int i=0;i<threadCount;i++){
            workers_.emplace_back(&Threadpool::worker,this);

        }
    }
    ~Threadpool(){
        {
            lock_guard<mutex>lock(mutex_);
            stop_=true;
            
        }
            cv_.notify_all();
            for(auto &worker:workers_){
                if(worker.joinable()){
                    worker.join();
                }
            }
        
    }
    template<typename F,typename ...Args>
    auto submit(F&& f,Args&&...args) -> std::future<decltype(f(args...))> {
        using ReturnType = decltype(f(args...));
        auto task = std::make_shared<std::packaged_task<ReturnType()>>(std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        std::future<ReturnType> result = task->get_future();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(stop_){
                throw std::runtime_error("submit on stopped ThreadPool");

            }
            tasks_.emplace([task](){(*task)();});

        }
        cv_.notify_one();
        return result;
    }

    private:
    void worker(){
        while(true){
            function<void()>task;
            {
                unique_lock<mutex>lock(mutex_);

                cv_.wait(lock,[this](){return stop_||!tasks_.empty();});
                if(stop_&&tasks_.empty())return ;
                task=move(tasks_.front());
                tasks_.pop();
            }

            task();
        }
    };
    
    
    vector<thread>workers_;
    queue<function<void()>>tasks_;
    mutex mutex_;
    condition_variable cv_;
    atomic<bool>stop_;
};