// AlpacaHTTP
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaHTTP.
//
// AlpacaHTTP is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

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

