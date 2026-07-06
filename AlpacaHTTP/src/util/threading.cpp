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

