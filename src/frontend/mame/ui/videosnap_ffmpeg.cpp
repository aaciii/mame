// SPDX-License-Identifier: BSD-3-Clause
#include "emu.h"
#include "videosnap_ffmpeg.h"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif
namespace videosnap {
namespace {
namespace fs = std::filesystem;
std::mutex g_mutex;
std::unique_ptr<ffmpeg_api> g_api;
std::string g_error = "FFmpeg libraries have not been loaded";
std::string g_attempt_folder;
bool g_attempted = false;
std::chrono::steady_clock::time_point g_attempt_time;
std::string path_text(fs::path const &path) {
    auto const utf8 = path.u8string();
    return std::string(reinterpret_cast<char const *>(utf8.data()), utf8.size());
}
fs::path executable_directory() {
    std::error_code ec;
#if defined(_WIN32)
    std::vector<wchar_t> buffer(32768);
    DWORD const length = GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));
    if (length && (length < buffer.size()))
        return fs::path(std::wstring(buffer.data(), length)).parent_path();
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size ? size : 1);
    if (size && !_NSGetExecutablePath(buffer.data(), &size))
        return fs::path(buffer.data()).parent_path();
#else
    auto const link = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) return link.parent_path();
#endif
    auto const current = fs::current_path(ec);
    return ec ? fs::path() : current;
}
std::string library_name(char const *stem, int major) {
#if defined(_WIN32)
    return std::string(stem) + "-" + std::to_string(major) + ".dll";
#elif defined(__APPLE__)
    return std::string("lib") + stem + "." + std::to_string(major) + ".dylib";
#else
    return std::string("lib") + stem + ".so." + std::to_string(major);
#endif
}
void *open_library(fs::path const &directory, std::string const &name) {
#if defined(_WIN32)
    if (directory.empty())
        return reinterpret_cast<void *>(LoadLibraryW(fs::path(name).wstring().c_str()));
    auto const full = fs::absolute(directory / name);
    return reinterpret_cast<void *>(LoadLibraryExW(full.wstring().c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS));
#else
    auto const full = directory.empty() ? fs::path(name) : (directory / name);
    return dlopen(path_text(full).c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}
void *find_symbol(void *library, char const *name) {
#if defined(_WIN32)
    return reinterpret_cast<void *>(GetProcAddress(reinterpret_cast<HMODULE>(library), name));
#else
    return dlsym(library, name);
#endif
}
void close_library(void *library) {
#if defined(_WIN32)
    FreeLibrary(reinterpret_cast<HMODULE>(library));
#else
    dlclose(library);
#endif
}
struct library_info { char const *stem; int major; };
std::string try_load(fs::path const &directory, ffmpeg_api &api, std::vector<void *> &opened) {
    library_info const libraries[5] = {
        { "avutil", LIBAVUTIL_VERSION_MAJOR },
        { "swresample", LIBSWRESAMPLE_VERSION_MAJOR },
        { "swscale", LIBSWSCALE_VERSION_MAJOR },
        { "avcodec", LIBAVCODEC_VERSION_MAJOR },
        { "avformat", LIBAVFORMAT_VERSION_MAJOR } };
    void *handles[5] = { };
    for (int i = 0; i < 5; ++i) {
        std::string const name = library_name(libraries[i].stem, libraries[i].major);
        handles[i] = open_library(directory, name);
        if (!handles[i]) {
            std::string const where = directory.empty() ? std::string("the system search path") : path_text(directory);
            return "cannot load " + name + " from " + where;
        }
        opened.push_back(handles[i]);
    }
#define VIDEOSNAP_FF_LOAD(index, name) \
    if (!(api.name = reinterpret_cast<decltype(api.name)>(find_symbol(handles[index], #name)))) \
        return std::string("missing function ") + #name;
#define VIDEOSNAP_FF_L0(name) VIDEOSNAP_FF_LOAD(0, name)
#define VIDEOSNAP_FF_L1(name) VIDEOSNAP_FF_LOAD(1, name)
#define VIDEOSNAP_FF_L2(name) VIDEOSNAP_FF_LOAD(2, name)
#define VIDEOSNAP_FF_L3(name) VIDEOSNAP_FF_LOAD(3, name)
#define VIDEOSNAP_FF_L4(name) VIDEOSNAP_FF_LOAD(4, name)
    VIDEOSNAP_FF_UTIL(VIDEOSNAP_FF_L0)
    VIDEOSNAP_FF_SWR(VIDEOSNAP_FF_L1)
    VIDEOSNAP_FF_SWS(VIDEOSNAP_FF_L2)
    VIDEOSNAP_FF_CODEC(VIDEOSNAP_FF_L3)
    VIDEOSNAP_FF_FORMAT(VIDEOSNAP_FF_L4)
    unsigned const found[5] = { api.avutil_version(), api.swresample_version(), api.swscale_version(),
        api.avcodec_version(), api.avformat_version() };
    for (int i = 0; i < 5; ++i) {
        if ((found[i] >> 16) != unsigned(libraries[i].major))
            return std::string(libraries[i].stem) + " has the wrong version";
    }
    return std::string();
}
}
ffmpeg_api const *ffmpeg(std::string const &folder) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_api) return g_api.get();
    auto const now = std::chrono::steady_clock::now();
    if (g_attempted && (folder == g_attempt_folder) && ((now - g_attempt_time) < std::chrono::seconds(2)))
        return nullptr;
    g_attempted = true;
    g_attempt_folder = folder;
    g_attempt_time = now;
    auto const base = executable_directory();
    fs::path configured = fs::path(std::u8string(folder.begin(), folder.end()));
    if (configured.empty()) configured = "ffmpeg";
    if (configured.is_relative()) configured = base / configured;
    std::vector<fs::path> candidates;
    candidates.push_back(configured.lexically_normal());
    auto const fallback = (base / "ffmpeg").lexically_normal();
    if (fallback != candidates.front()) candidates.push_back(fallback);
    candidates.emplace_back();
    std::string first_error;
#if defined(_WIN32)
    UINT const old_mode = SetErrorMode(SEM_FAILCRITICALERRORS);
#endif
    for (auto const &directory : candidates) {
        auto api = std::make_unique<ffmpeg_api>();
        std::vector<void *> opened;
        std::string const problem = try_load(directory, *api, opened);
        if (problem.empty()) {
            g_api = std::move(api);
            break;
        }
        for (auto it = opened.rbegin(); it != opened.rend(); ++it) close_library(*it);
        if (first_error.empty()) first_error = problem;
    }
#if defined(_WIN32)
    SetErrorMode(old_mode);
#endif
    if (g_api) {
        g_error.clear();
        return g_api.get();
    }
    g_error = "FFmpeg not available: " + first_error;
    return nullptr;
}
std::string ffmpeg_error() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_error;
}
}
