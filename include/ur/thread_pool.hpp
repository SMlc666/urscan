#pragma once

#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <stdexcept>
#include <type_traits>
#include <atomic>

namespace ur {

// A simple work-stealing queue based on std::deque and a mutex.
// This is not lock-free, but distributes contention across queues,
// which can significantly improve performance under load.
template<typename T>
class WorkStealingQueue {
public:
    WorkStealingQueue() = default;
    WorkStealingQueue(const WorkStealingQueue&) = delete;
    WorkStealingQueue& operator=(const WorkStealingQueue&) = delete;

    WorkStealingQueue(WorkStealingQueue&& other) noexcept {
        std::lock_guard<std::mutex> lock(other.mutex_);
        queue_ = std::move(other.queue_);
    }

    WorkStealingQueue& operator=(WorkStealingQueue&& other) noexcept {
        if (this != &other) {
            std::scoped_lock lock(mutex_, other.mutex_);
            queue_ = std::move(other.queue_);
        }
        return *this;
    }

    // Push a task to the front of the deque.
    void push(T item) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_front(item);
    }

    // Pop a task from the front of the deque (LIFO for owner).
    bool pop(T& item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return false;
        }
        item = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    // Steal a task from the back of the deque (FIFO for thieves).
    bool steal(T& item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return false;
        }
        item = std::move(queue_.back());
        queue_.pop_back();
        return true;
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

private:
    std::deque<T> queue_;
    mutable std::mutex mutex_;
};

class ThreadPool {
public:
    ThreadPool(size_t threads = std::thread::hardware_concurrency()) : stop_(false), active_threads_(0) {
        if (threads == 0) {
            threads = 1;
        }
        thread_count_ = threads;
        queues_.resize(threads);
        workers_.reserve(threads);
        for (size_t i = 0; i < threads; ++i) {
            workers_.emplace_back([this, i] { worker_loop(i); });
        }
    }

    ~ThreadPool() {
        stop_.store(true);
        condition_.notify_all();
        for (std::thread& worker : workers_) {
            worker.join();
        }
    }

    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>
    {
        using return_type = std::invoke_result_t<F, Args...>;

        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );

        std::future<return_type> res = task->get_future();
        
        if (stop_.load()) {
            throw std::runtime_error("enqueue on stopped ThreadPool");
        }

        // Distribute tasks to worker queues round-robin
        size_t queue_idx = submission_idx_.fetch_add(1) % thread_count_;
        queues_[queue_idx].push([task]() { (*task)(); });

        if (active_threads_ < thread_count_) {
            condition_.notify_one();
        } else {
            condition_.notify_all();
        }
        return res;
    }

private:
    using Task = std::function<void()>;

    void worker_loop(size_t id) {
        while (!stop_.load()) {
            Task task;
            
            active_threads_++;
            // First, try to pop a task from our own queue.
            if (queues_[id].pop(task)) {
                task();
                active_threads_--;
                continue;
            }

            // If our queue is empty, try to steal a task from another thread.
            bool stolen = false;
            for (size_t i = 0; i < thread_count_; ++i) {
                if (i == id) continue;
                if (queues_[(id + i) % thread_count_].steal(task)) {
                    stolen = true;
                    break;
                }
            }
            active_threads_--;

            if (stolen) {
                task();
            } else {
                // If no task was found, wait for a notification.
                std::unique_lock<std::mutex> lock(wait_mutex_);
                condition_.wait(lock, [this] {
                    if (stop_.load()) return true;
                    // Wake up if any queue has tasks. This prevents missed notifications.
                    for (size_t i = 0; i < thread_count_; ++i) {
                        if (!queues_[i].empty()) return true;
                    }
                    return false;
                });
            }
        }
    }

    size_t thread_count_;
    std::vector<std::thread> workers_;
    std::vector<WorkStealingQueue<Task>> queues_;

    std::atomic<bool> stop_;
    std::atomic<size_t> submission_idx_{0};
    std::atomic<size_t> active_threads_;

    std::mutex wait_mutex_;
    std::condition_variable condition_;
};

} // namespace ur
