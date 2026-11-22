// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
namespace videosnap {
class audio_output {
public:
    audio_output(std::string filename, double media_origin, int volume,
        std::atomic<bool> &owner_cancel, std::atomic<double> &epoch);
    ~audio_output();
    audio_output(audio_output const &) = delete;
    audio_output &operator=(audio_output const &) = delete;
    bool wait_ready(std::chrono::milliseconds timeout);
    std::string error() const;
    double lead_seconds() const;
private:
    struct implementation;
    std::unique_ptr<implementation> m_impl;
};
}
