#pragma once

#include <chrono>
#include <string>
#include <utility>

namespace fpng {

class Timer {
public:
    using Clock = std::chrono::high_resolution_clock;
    using Duration = Clock::duration;
    using TimePoint = Clock::time_point;

    Timer() : start_(Clock::now()) {}

    void reset() noexcept { start_ = Clock::now(); }

    double elapsed_seconds() const noexcept {
        return std::chrono::duration<double>(Clock::now() - start_).count();
    }

    double elapsed_milliseconds() const noexcept {
        return std::chrono::duration<double, std::milli>(Clock::now() - start_).count();
    }

    double elapsed_microseconds() const noexcept {
        return std::chrono::duration<double, std::micro>(Clock::now() - start_).count();
    }

    static double seconds_since(const TimePoint& tp) noexcept {
        return std::chrono::duration<double>(Clock::now() - tp).count();
    }

    TimePoint now() const noexcept { return Clock::now(); }

private:
    TimePoint start_;
};

struct ScopedTimer {
    Timer timer;
    std::string label;

    ScopedTimer(std::string l) : timer(), label(std::move(l)) {}

    // Returns elapsed seconds, prints the label and time
    double stop() {
        double elapsed = timer.elapsed_seconds();
        return elapsed;
    }

    double elapsed() const { return timer.elapsed_seconds(); }
};

} // namespace fpng
