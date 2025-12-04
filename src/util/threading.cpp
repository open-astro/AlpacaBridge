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

#include <alpacahttp/util/threading.h>
#include <algorithm>

namespace alpacahttp::util {

ThreadPool::ThreadPool(std::size_t num_threads) {
    // TODO: Implement full thread pool with task queue
    // For now, this is a placeholder
}

ThreadPool::~ThreadPool() {
    shutdown();
}

void ThreadPool::submit(std::function<void()> task) {
    // TODO: Implement task submission
    (void)task;
}

void ThreadPool::shutdown() {
    shutdown_ = true;
    for (auto& thread : threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    threads_.clear();
}

} // namespace alpacahttp::util

