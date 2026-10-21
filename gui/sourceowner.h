// SourceOwner — the one thread that touches a file's TabularSources.
//
// The tabs of one file share state below the TabularSource interface: an
// SQLite file's tabs one sqlite3 handle, a workbook's sheets one reader, an
// HDF5 file's tabs one Store. Every call into those sources therefore runs
// on a single thread per file, in the order it was posted: a filter, a sort
// and a find started back to back queue behind each other instead of reading
// one source from two threads. Tasks check their own cancel flag between
// chunks, so a superseded job ends at its next chunk.
//
// The thread is joined when the last owner reference goes away (the last tab
// of the file closes); the queue is run to its end first, so a posted task
// that destroys a source always runs.
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <utility>

class SourceOwner {
public:
    SourceOwner() : th_([this] { loop(); }) {}
    ~SourceOwner() {
        {
            std::lock_guard<std::mutex> l(m_);
            stop_ = true;
        }
        cv_.notify_all();
        if (th_.joinable()) th_.join();
    }
    SourceOwner(const SourceOwner&) = delete;
    SourceOwner& operator=(const SourceOwner&) = delete;

    // Queue `fn` behind the tasks already posted.
    void post(std::function<void()> fn) {
        {
            std::lock_guard<std::mutex> l(m_);
            q_.push_back(std::move(fn));
        }
        cv_.notify_one();
    }

    // Run `fn` on the owner thread and wait for its result — for the calls
    // that must answer at once (the self-test's synchronous paths, a tab that
    // summarises its own source). Runs inline when called on the owner thread.
    template <class F>
    auto runSync(F&& fn) -> decltype(fn()) {
        using R = decltype(fn());
        if (std::this_thread::get_id() == th_.get_id()) return fn();
        std::packaged_task<R()> task(std::forward<F>(fn));
        std::future<R> fut = task.get_future();
        auto shared = std::make_shared<std::packaged_task<R()>>(std::move(task));
        post([shared] { (*shared)(); });
        return fut.get();
    }

private:
    void loop() {
        for (;;) {
            std::function<void()> fn;
            {
                std::unique_lock<std::mutex> l(m_);
                cv_.wait(l, [this] { return stop_ || !q_.empty(); });
                if (q_.empty()) return;          // stop_ and nothing left
                fn = std::move(q_.front());
                q_.pop_front();
            }
            fn();
        }
    }

    std::mutex                        m_;
    std::condition_variable           cv_;
    std::deque<std::function<void()>> q_;
    bool                              stop_ = false;
    std::thread                       th_;   // last: started after the rest
};
