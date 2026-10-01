#pragma once

#include <cstdint>
#include <mutex>
#include <utility>

namespace webdossier {

class SidecarProcessLifecycle {
public:
    std::uint64_t BeginChild() {
        std::lock_guard<std::mutex> lock(mutex_);
        active_generation_ = ++next_generation_;
        return active_generation_;
    }

    // The callback runs synchronously under mutex_; it must not re-enter this
    // object or wait for a thread that needs this lock. DetachOwner waits for
    // any running callback to finish before it returns.
    template <typename Callback>
    bool NotifyChildExit(std::uint64_t generation, Callback&& callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!owner_alive_ || generation == 0 || generation != active_generation_) {
            return false;
        }
        active_generation_ = 0;
        std::forward<Callback>(callback)();
        return true;
    }

    bool StopChild(std::uint64_t generation) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation == 0 || generation != active_generation_) return false;
        active_generation_ = 0;
        return true;
    }

    void DetachOwner() {
        std::lock_guard<std::mutex> lock(mutex_);
        owner_alive_ = false;
    }

private:
    std::mutex mutex_;
    bool owner_alive_ = true;
    std::uint64_t next_generation_ = 0;
    std::uint64_t active_generation_ = 0;
};

} // namespace webdossier
