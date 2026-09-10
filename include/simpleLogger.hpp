#pragma once

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <fstream>
#include <iostream>

class SimpleLogger {
public:
    explicit SimpleLogger(const std::string& filename) : running_(true) {
        file_.open(filename, std::ios::out | std::ios::app);
        if (!file_.is_open()) {
            throw std::runtime_error("Cannot open log file");
        }
        worker_ = std::thread(&SimpleLogger::background_work, this);
    }

    ~SimpleLogger() {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            running_ = false;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        if (file_.is_open()) file_.close();
    }

    void log(const std::string& msg) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            buffer_.push_back(msg);
            // 不再基于 size 触发，而是每次添加都通知，后台线程会立即处理
            // 这样更实时，避免最后一条丢失
        }
        cv_.notify_one(); // 每次通知，后台线程会立即写
    }

private:
    void background_work() {
        std::vector<std::string> to_write;
        while (true) {
            {
                std::unique_lock<std::mutex> lock(mtx_);
                cv_.wait(lock, [this] {
                    return !running_ || !buffer_.empty();
                });
                if (!running_ && buffer_.empty()) break;
                to_write.swap(buffer_);
            }
            for (const auto& msg : to_write) {
                file_ << msg << std::endl;
            }
            file_.flush();
            to_write.clear();
        }
        // 退出前将剩余数据写入（但此时 buffer_ 应为空，因为循环条件已检查）
        // 为安全起见，再检查一次
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (!buffer_.empty()) {
                for (const auto& msg : buffer_) {
                    file_ << msg << std::endl;
                }
                file_.flush();
            }
        }
    }

    std::ofstream file_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::vector<std::string> buffer_;
    std::thread worker_;
    bool running_;
};