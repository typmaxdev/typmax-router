// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 The typmax-router authors
// Stands in for KiCad's include/thread_pool.h (BS::priority_thread_pool): the
// service runs each request on ONE worker thread, so a "pool" here runs a
// submitted loop inline, in index order. NODE::NearestObstacle is its one user
// in the router core; it reduces the per-index results in index order, so the
// answer is the same as KiCad's parallel run, without threads.
#pragma once

#include <cstddef>

class thread_pool {
public:
    struct futures {
        void wait() const {}
    };
    template <typename T, typename F>
    futures submit_loop(T first, T last, F&& f, std::size_t /*blocks*/ = 0)
    {
        for (T i = first; i < last; ++i)
            f(i);
        return {};
    }
};

inline thread_pool& GetKiCadThreadPool()
{
    static thread_pool pool;
    return pool;
}
