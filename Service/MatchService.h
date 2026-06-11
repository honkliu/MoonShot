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

        m_Workers.reserve(thread_count);
        for (std::size_t i = 0; i < thread_count; ++i)
            m_Workers.emplace_back([this] { WorkerLoop(); });
    }

    ~ThreadPool()
    {
        {
            std::unique_lock<std::mutex> lock(m_Mutex);
            m_Stop = true;
        }
        m_Cv.notify_all();
        for (auto& t : m_Workers)
            if (t.joinable()) t.join();
    }

    ThreadPool(const ThreadPool&)            = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    template<typename F>
    void post(F&& f)
    {
        {
            std::unique_lock<std::mutex> lock(m_Mutex);
            if (m_Stop)
                throw std::runtime_error("ThreadPool: post() after shutdown");
            m_Tasks.emplace(std::forward<F>(f));
        }
        m_Cv.notify_one();
    }

    void join()
    {
        std::unique_lock<std::mutex> lock(m_Mutex);
        m_CvDone.wait(lock, [this] {
            return m_Tasks.empty() && m_ActiveCount == 0;
        });
    }

    std::size_t thread_count() const { return m_Workers.size(); }

private:
    void WorkerLoop()
    {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(m_Mutex);
                m_Cv.wait(lock, [this] { return m_Stop || !m_Tasks.empty(); });
                if (m_Stop && m_Tasks.empty()) return;
                task = std::move(m_Tasks.front());
                m_Tasks.pop();
                ++m_ActiveCount;
            }

            task();

            {
                std::unique_lock<std::mutex> lock(m_Mutex);
                --m_ActiveCount;
            }
            m_CvDone.notify_all();
        }
    }

    std::vector<std::thread>           m_Workers;
    std::queue<std::function<void()>>  m_Tasks;
    std::mutex                         m_Mutex;
    std::condition_variable            m_Cv;
    std::condition_variable            m_CvDone;
    bool                               m_Stop         = false;
    std::size_t                        m_ActiveCount = 0;
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
