#pragma once

#include <algorithm>
#include <chrono>
#include <string>

// The Lua control session is serialized on the Agent's RPC thread. Stream
// waits must finish before the companion's 30-second relay deadline; native
// and JIT worker streams have their own cancellation path and do not use this.
class LuaStreamWaitScope {
  public:
    using Clock = std::chrono::steady_clock;
    static constexpr int kMaximumWaitMs = 10000;
    static constexpr int kEvaluationBudgetMs = 25000;

    explicit LuaStreamWaitScope(Clock::time_point now = Clock::now()) : previous_(current_) {
        if (current_ == nullptr) {
            state_.deadline = now + std::chrono::milliseconds(kEvaluationBudgetMs);
            current_ = &state_;
        }
    }
    ~LuaStreamWaitScope() {
        current_ = previous_;
    }

    LuaStreamWaitScope(const LuaStreamWaitScope &) = delete;
    LuaStreamWaitScope &operator=(const LuaStreamWaitScope &) = delete;

    static bool limit(long long requested, int *timeout_ms, std::string *error,
                      Clock::time_point now = Clock::now()) {
        if (requested < 0 || requested > kMaximumWaitMs) {
            *error = "Lua stream timeout must be between 0 and 10000 ms; use a native/JIT worker "
                     "for indefinite waits";
            return false;
        }
        if (current_ != nullptr) {
            const auto remaining = current_->deadline - now;
            if (remaining <= Clock::duration::zero()) {
                *error = "Lua stream wait budget exhausted; return from this control request "
                         "before waiting again";
                return false;
            }
            const auto remaining_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
            if (requested > 0 && remaining_ms == 0) {
                *error = "Lua stream wait budget exhausted at millisecond resolution";
                return false;
            }
            *timeout_ms = static_cast<int>(std::min<long long>(requested, remaining_ms));
        } else {
            *timeout_ms = static_cast<int>(requested);
        }
        return true;
    }

  private:
    struct State {
        Clock::time_point deadline;
    } state_;
    State *previous_;
    inline static thread_local State *current_ = nullptr;
};
