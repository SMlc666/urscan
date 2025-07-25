#include <gtest/gtest.h>
#include "ur/thread_pool.hpp"
#include <future>
#include <chrono>

// 测试线程池可以执行一个简单的任务
TEST(ThreadPoolTest, SimpleTask) {
    ur::ThreadPool pool(1);
    auto future = pool.enqueue([] { return 42; });
    EXPECT_EQ(future.get(), 42);
}

// 测试线程池可以执行多个任务
TEST(ThreadPoolTest, MultipleTasks) {
    ur::ThreadPool pool(4);
    std::vector<std::future<int>> futures;
    for (int i = 0; i < 10; ++i) {
        futures.emplace_back(pool.enqueue([i] { return i; }));
    }

    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(futures[i].get(), i);
    }
}

// 测试工作窃取
TEST(ThreadPoolTest, WorkStealing) {
    ur::ThreadPool pool(2);
    std::atomic<int> counter = 0;

    // Enqueue a long-running task to one thread
    auto future1 = pool.enqueue([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        counter++;
    });

    // Enqueue many short tasks
    std::vector<std::future<void>> futures;
    for (int i = 0; i < 100; ++i) {
        futures.emplace_back(pool.enqueue([&] {
            counter++;
        }));
    }

    future1.get();
    for (auto& f : futures) {
        f.get();
    }

    EXPECT_EQ(counter.load(), 101);
}
