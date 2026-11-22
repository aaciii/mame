// SPDX-License-Identifier: BSD-3-Clause
#include "emu.h"
#include "videosnap_paths.h"
#include <filesystem>
#include <system_error>
#include <vector>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__)
#include <unistd.h>
#endif
namespace videosnap {
namespace {
std::filesystem::path executable_directory()
{
#if defined(_WIN32)
    std::vector<wchar_t> buffer(32768);
    DWORD const length = GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));
    if (length && (length < buffer.size()))
        return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size);
    if (size && !_NSGetExecutablePath(buffer.data(), &size))
        return std::filesystem::path(buffer.data()).parent_path();
#elif defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__)
    std::vector<char> buffer(32768);
#if defined(__linux__)
    char const *link = "/proc/self/exe";
#elif defined(__FreeBSD__)
    char const *link = "/proc/curproc/file";
#else
    char const *link = "/proc/curproc/exe";
#endif
    auto const length = readlink(link, buffer.data(), buffer.size());
    if ((length > 0) && (std::size_t(length) < buffer.size()))
        return std::filesystem::path(std::string(buffer.data(), length)).parent_path();
#endif
    std::error_code error;
    auto result = std::filesystem::current_path(error);
    return error ? std::filesystem::path() : result;
}
}
std::string local_video_path(std::string const &relative, std::string const &search_path)
{
    try
    {
        static auto const directory = executable_directory();
        if (directory.empty()) return {};
        auto const child = std::filesystem::path(std::u8string(relative.begin(), relative.end()));
        if (child.is_absolute() || child.has_root_name()) return {};
        for (auto const &part : child)
            if (part == "..") return {};
        std::size_t begin = 0;
        while (begin <= search_path.size())
        {
            auto const end = search_path.find(';', begin);
            auto const entry = search_path.substr(begin,
                end == std::string::npos ? std::string::npos : end - begin);
            if (!entry.empty())
            {
                auto search_directory = std::filesystem::path(std::u8string(entry.begin(), entry.end()));
                if (search_directory.is_relative())
                    search_directory = directory / search_directory;
                auto const filename = search_directory / child;
                std::error_code error;
                if (std::filesystem::is_regular_file(filename, error) && !error)
                {
                    auto const utf8 = filename.u8string();
                    return std::string(reinterpret_cast<char const *>(utf8.data()), utf8.size());
                }
            }
            if (end == std::string::npos)
                break;
            begin = end + 1;
        }
        return {};
    }
    catch (std::filesystem::filesystem_error const &) { return {}; }
}
}
