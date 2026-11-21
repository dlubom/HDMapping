#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace HDMapping
{
    //! Locate the running binary independently of the working directory and argv[0].
    inline std::filesystem::path executablePath()
    {
#ifdef _WIN32
        std::vector<wchar_t> path(32768);
        const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0 || length == path.size())
            throw std::runtime_error("Cannot locate running executable");
        return std::filesystem::path(path.data(), path.data() + length);
#elif defined(__APPLE__)
        std::uint32_t size = 0;
        _NSGetExecutablePath(nullptr, &size);
        std::vector<char> path(size);
        if (_NSGetExecutablePath(path.data(), &size) != 0)
            throw std::runtime_error("Cannot locate running executable");
        return std::filesystem::canonical(path.data());
#else
        return std::filesystem::canonical("/proc/self/exe");
#endif
    }
} // namespace HDMapping
