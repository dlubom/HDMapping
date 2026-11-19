#include <HDMapping/Processing/RuntimeProtocol.h>

#include "recording_capture.h"

#include <cerrno>
#include <cstdio>
#include <iostream>
#include <memory>
#include <system_error>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace
{
    struct CloseProtocolFile
    {
        void operator()(std::FILE* file) const noexcept
        {
            std::fclose(file);
        }
    };

    using ProtocolFile = std::unique_ptr<std::FILE, CloseProtocolFile>;

    [[noreturn]] void io_error(const char* operation)
    {
        throw std::system_error(errno, std::generic_category(), operation);
    }

    // Keep a private writer for the wire while routing ordinary engine output to stderr.
    ProtocolFile open_protocol_output()
    {
#ifdef _WIN32
        if (_setmode(_fileno(stdin), _O_BINARY) < 0 || _setmode(_fileno(stdout), _O_BINARY) < 0)
        {
            io_error("configure binary worker protocol");
        }
        const int fd = _dup(_fileno(stdout));
        if (fd < 0)
        {
            io_error("duplicate worker protocol stream");
        }
        std::FILE* file = _fdopen(fd, "wb");
        if (file == nullptr)
        {
            const int original_errno = errno;
            _close(fd);
            errno = original_errno;
            io_error("open worker protocol stream");
        }
        ProtocolFile protocol(file);
        if (!SetHandleInformation(reinterpret_cast<HANDLE>(_get_osfhandle(fd)), HANDLE_FLAG_INHERIT, 0))
        {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "set worker stream handle flags");
        }
        if (_dup2(_fileno(stderr), _fileno(stdout)) < 0)
        {
            io_error("redirect worker output");
        }
        if (!SetStdHandle(STD_OUTPUT_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdout)))))
        {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "redirect worker standard handle");
        }
#else
        std::signal(SIGPIPE, SIG_IGN);
        const int fd = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 0);
        if (fd < 0)
        {
            io_error("duplicate worker protocol stream");
        }
        std::FILE* file = fdopen(fd, "w");
        if (file == nullptr)
        {
            const int original_errno = errno;
            close(fd);
            errno = original_errno;
            io_error("open worker protocol stream");
        }
        ProtocolFile protocol(file);
        if (dup2(STDERR_FILENO, STDOUT_FILENO) < 0)
        {
            io_error("redirect worker output");
        }
#endif
        return protocol;
    }

    std::uint64_t process_id()
    {
#ifdef _WIN32
        return GetCurrentProcessId();
#else
        return static_cast<std::uint64_t>(getpid());
#endif
    }
} // namespace

int main()
{
    using namespace HDMapping::Processing;
    try
    {
        const auto protocol = open_protocol_output();
        std::cin.tie(nullptr);
        ServeWorker(
            std::cin,
            [&](std::string_view packet)
            {
                if (std::fwrite(packet.data(), 1, packet.size(), protocol.get()) != packet.size() || std::fflush(protocol.get()) != 0)
                {
                    io_error("send worker protocol packet");
                }
            },
            { HDMAPPING_WORKER_VERSION, process_id() },
            std::filesystem::current_path(),
            DescribeOperations(),
            ExecuteOperation,
            [](const TargetRef& target, const std::filesystem::path& recording, const std::filesystem::path& toml, RunContext& context)
            {
                RecordingCapture source(target, recording, toml);
                return CaptureSnapshot(source, target.revision, context.output_directory, context);
            },
            CaptureStep1References);
        return 0;
    } catch (const std::exception& error)
    {
        std::cerr << "hdmapping_worker: " << error.what() << '\n';
        return 1;
    }
}
