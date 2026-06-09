#pragma once

#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <vector>
#include <memory>
#include <stdexcept>

namespace Service
{

/*
* Thread pool backed by std::thread.
* post(f) enqueues work; join() blocks until the queue drains.
*/
class ThreadPool
{
public:
    explicit ThreadPool(std::size_t thread_count)
    {
        if (thread_count == 0)
            throw std::invalid_argument("ThreadPool: thread_count must be > 0");

        workers_.reserve(thread_count);
        for (std::size_t i = 0; i < thread_count; ++i)
            workers_.emplace_back([this] { WorkerLoop(); });
    }

    ~ThreadPool()
    {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_)
            if (t.joinable()) t.join();
    }

    ThreadPool(const ThreadPool&)            = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    template<typename F>
    void post(F&& f)
    {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (stop_)
                throw std::runtime_error("ThreadPool: post() after shutdown");
            tasks_.emplace(std::forward<F>(f));
        }
        cv_.notify_one();
    }

    void join()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_done_.wait(lock, [this] {
            return tasks_.empty() && active_count_ == 0;
        });
    }

    std::size_t thread_count() const { return workers_.size(); }

private:
    void WorkerLoop()
    {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
                if (stop_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop();
                ++active_count_;
            }

            task();

            {
                std::unique_lock<std::mutex> lock(mutex_);
                --active_count_;
            }
            cv_done_.notify_all();
        }
    }

    std::vector<std::thread>           workers_;
    std::queue<std::function<void()>>  tasks_;
    std::mutex                         mutex_;
    std::condition_variable            cv_;
    std::condition_variable            cv_done_;
    bool                               stop_         = false;
    std::size_t                        active_count_ = 0;
};


class MatchService
{
public:
    MatchService();
    ~MatchService();

    bool Init();
    int  GetState();
    int  SetState(int state);
    void ExecuteQuery();

private:
    int m_NumberOfQueueThreads = 2;
    int m_NumberOfQueryThreads = 4;
    int m_NumberOfDataThreads  = 2;

    std::unique_ptr<ThreadPool> m_QueueThreads;
    std::unique_ptr<ThreadPool> m_QueryThreads;
    std::unique_ptr<ThreadPool> m_DataThreads;

    bool IsCapable() { return true; }
};

} // namespace Service
