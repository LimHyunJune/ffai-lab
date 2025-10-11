#pragma once

#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

template<typename T>
class TSQueue
{
private:
	std::queue<T> queue_;
	std::mutex mutex_;
	std::condition_variable cv_;
	bool running = true;

public:
	void push(T item)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		queue_.push(item);
		cv_.notify_one();
	}

	bool pop(T& item)
	{
		std::unique_lock<std::mutex> lock(mutex_);
		cv_.wait(lock, [this] { return !queue_.empty() || !running; });
		if (queue_.empty())
		{
			//std::chrono::microseconds timespan(2000);
			//std::this_thread::sleep_for(timespan);
			return false;
		}
		item = std::move(queue_.front());
		queue_.pop();
		return true;
	}

	void stop()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		running = false;
		cv_.notify_all();
	}
};