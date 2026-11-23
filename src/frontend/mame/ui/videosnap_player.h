// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace videosnap {
struct frame {
    int width = 0, height = 0;
    double display_aspect = 1.0;
    std::uint64_t sequence = 0, loop = 0;
    std::vector<std::uint32_t> argb;
};
enum class state { stopped, opening, playing, failed };

// start/stop/take_frame must be called by a single owning (UI) thread.
class player {
public:
    player() = default;
    ~player();
    player(player const &) = delete;
    player &operator=(player const &) = delete;
    void start(std::string utf8_filename,
               std::chrono::milliseconds delay = std::chrono::milliseconds(250));
    void stop();
    bool take_frame(frame &result);
    // Owner/UI thread only; stops any active playback before changing options.
    void configure_audio(bool enabled, int volume);
    void configure_ffmpeg(std::string folder);
    std::string audio_error() const;
    state status() const;
    std::string error() const;
private:
    void run(std::string const &path, std::chrono::milliseconds delay) noexcept;
    bool wait_until(std::chrono::steady_clock::time_point time);
    std::atomic<bool> m_cancel{false};
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::thread m_thread;
    frame m_latest;
    bool m_ready = false;
    state m_state = state::stopped;
    std::string m_error;
    std::string m_audio_error;
    std::string m_ffmpeg_folder = "ffmpeg";
    bool m_audio_enabled = true;
    int m_audio_volume = 100;
};
}
