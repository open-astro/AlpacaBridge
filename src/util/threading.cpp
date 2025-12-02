// AlpacaHTTP
// Copyright (c) 2025 Joey Troy
//
// This file is part of AlpacaHTTP.
//
// Licensed under the Server Side Public License, v1.
// https://www.mongodb.com/licensing/server-side-public-license

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

