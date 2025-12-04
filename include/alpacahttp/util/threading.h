// AlpacaHTTP
// Copyright (c) 2025 Joey Troy and contributors
//
// This file is part of AlpacaHTTP.
//
// AlpacaHTTP is licensed under the Server Side Public License, version 1 (SSPL v1).
// https://github.com/open-astro/AlpacaHTTP/blob/main/LICENSE
//
// If you use this library to provide a network-accessible service, you must comply
// with the SSPL v1 requirements.

#pragma once

#include <thread>
#include <vector>
#include <memory>
#include <functional>

namespace alpacahttp::util {

// Thread pool for handling concurrent requests
class ThreadPool {
public:
    explicit ThreadPool(std::size_t num_threads = std::thread::hardware_concurrency());
    ~ThreadPool();

    // Submit a task to the thread pool
    void submit(std::function<void()> task);

    // Shutdown the thread pool
    void shutdown();

private:
    std::vector<std::thread> threads_;
    std::atomic<bool> shutdown_{false};
    // TODO: Add task queue and synchronization primitives
};

} // namespace alpacahttp::util

