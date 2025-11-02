#pragma once

#include "Logger.h"
#include "Timer.h"
#include <queue>
#include <thread>
#include <mutex>
#include <chrono>
#include <condition_variable>
#include <atomic>
#include <string>

template<typename T>
class TSQueue
{
private:
    // 내부 로직 용
    std::string name_;
    std::queue<T> queue_;
    std::mutex mutex_;
    std::condition_variable cv_not_empty_;
    std::condition_variable cv_not_full_;
    bool running = true;

    bool has_popped_ = false;

    size_t prepop_max_depth_ = 10000; // pop 되지 않은 경우 최대 임계 값
    size_t capacity_ = 0; // 0이면 제한 없음

    // 매트릭
    std::atomic<uint64_t> push_cnt{0}, pop_cnt{0}, drop_cnt{0}, pop_fail{0};
    std::atomic<int64_t> depth{0};

    Timer timer;

    void check_every_second()
    {
        if(timer.elapsed() < 1)	return;
        timer.reset();
        
        uint64_t push = push_cnt.exchange(0); // 다른 스레드가 끼어들지 못하게 값을 안전하게 교환
        uint64_t pop  = pop_cnt.exchange(0);
        uint64_t drop = drop_cnt.exchange(0);
        uint64_t pf   = pop_fail.exchange(0);
        int64_t  d    = depth.load();
        
        BOOST_LOG(debug) << "[" << name_ << "]"	
                        << " push=" << push
                        << " pop="  << pop
                        << " drop=" << drop
                        << " fail=" << pf
                        << " depth="<< d;
    }

    void drop_oldest_one() {
        if (!queue_.empty()) 
        {
            BOOST_LOG(debug) << "[" << name_ << "]" << " packet dropped !";
            queue_.pop();
            depth--;
            drop_cnt++;
        }
    }

    void prune_if_no_consumer_yet() {
        if (prepop_max_depth_ == 0) return;           // 0이면 비활성화
        if (has_popped_) return; // 이미 pop 됐으면 종료
        while (queue_.size() >= prepop_max_depth_) {
            drop_oldest_one();
        }
    }

public:
    TSQueue(std::string name){
        name_ = name;
        if(name == "INPUT")
            capacity_ = 0;
        else if(name == "DECODER")
            capacity_ = 10;
        else if(name == "FILTER")
            capacity_ = 10;
        else if(name == "ENCODER")
            capacity_ = 0;
    }
    TSQueue(){}

    void push(T item)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        prune_if_no_consumer_yet();
        if (capacity_ > 0) {
            cv_not_full_.wait(lock, [this]{	return !running || queue_.size() < capacity_;});
            if (!running) return;
        }

        queue_.push(std::move(item));
        depth++;
        push_cnt++;

        lock.unlock();
        cv_not_empty_.notify_one();
        check_every_second();
    }

    bool pop(T& item)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_not_empty_.wait(lock, [this] { return !queue_.empty() || !running; });
        if (queue_.empty())
        {
            pop_fail++;
            check_every_second();
            return false;
        }
        item = std::move(queue_.front());
        queue_.pop();
        pop_cnt++;
        depth--;
        has_popped_ = true;
        cv_not_full_.notify_all();
        check_every_second();
        return true;
    }

    void stop()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running = false;
        cv_not_empty_.notify_all();
        cv_not_full_.notify_all();
        BOOST_LOG(debug) << "[" << name_ << "] queue stop";
    }
};
