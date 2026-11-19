#include "GuiRelayProcess.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <crt_externs.h>
#include <mach-o/dyld.h>
#else
extern char** environ;
#endif
#endif

namespace HDMapping::Processing
{
    namespace
    {
        namespace fs = std::filesystem;
        constexpr std::size_t max_frame = 4U << 20;

        fs::path executablePath()
        {
#ifdef _WIN32
            std::vector<wchar_t> path(32768);
            const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
            if (length == 0 || length == path.size())
                throw std::runtime_error("Cannot locate GUI executable");
            return fs::path(path.data(), path.data() + length);
#elif defined(__APPLE__)
            std::uint32_t size = 0;
            _NSGetExecutablePath(nullptr, &size);
            std::vector<char> path(size);
            if (_NSGetExecutablePath(path.data(), &size) != 0)
                throw std::runtime_error("Cannot locate GUI executable");
            return fs::canonical(path.data());
#else
            return fs::canonical("/proc/self/exe");
#endif
        }

        fs::path companion(std::string_view gui)
        {
            const auto self = executablePath();
            auto name = self.filename();
#ifdef _WIN32
            name.replace_extension();
#endif
            const auto binary = name.native();
            const auto suffix = fs::path(gui).native();
            if (!binary.ends_with(suffix))
                throw std::runtime_error("Unrecognized GUI executable name");
            auto relay = fs::path(binary.substr(0, binary.size() - suffix.size()) + fs::path("hdmapping_mcp").native());
#ifdef _WIN32
            relay += L".exe";
#endif
            return self.parent_path() / relay;
        }

#ifdef _WIN32
        struct Handle
        {
            HANDLE value{};
            ~Handle()
            {
                if (value)
                    CloseHandle(value);
            }
        };

        struct Attributes
        {
            std::vector<unsigned char> storage;
            LPPROC_THREAD_ATTRIBUTE_LIST list{};
            Attributes()
            {
                SIZE_T size = 0;
                InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
                storage.resize(size);
                list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
                if (!InitializeProcThreadAttributeList(list, 1, 0, &size))
                    throw std::runtime_error("Cannot create relay handle list");
            }
            ~Attributes()
            {
                DeleteProcThreadAttributeList(list);
            }
        };
#else
        struct Descriptor
        {
            int value{ -1 };
            ~Descriptor()
            {
                if (value >= 0)
                    close(value);
            }
        };

        struct FileActions
        {
            posix_spawn_file_actions_t value;
            FileActions()
            {
                if (int error = posix_spawn_file_actions_init(&value))
                    throw std::system_error(error, std::generic_category(), "Cannot initialize relay stdio");
            }
            ~FileActions()
            {
                posix_spawn_file_actions_destroy(&value);
            }
        };
#endif
    } // namespace

    struct GuiRelayProcess::Process
    {
#ifdef _WIN32
        Handle input, output, child;
        explicit Process(const fs::path& relay)
        {
            SECURITY_ATTRIBUTES sa{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
            Handle child_in, child_out, child_err;
            if (!CreatePipe(&child_in.value, &input.value, &sa, 0) || !CreatePipe(&output.value, &child_out.value, &sa, 0) ||
                !SetHandleInformation(input.value, HANDLE_FLAG_INHERIT, 0) || !SetHandleInformation(output.value, HANDLE_FLAG_INHERIT, 0))
                throw std::runtime_error("Cannot create private GUI relay pipes");
            child_err.value = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
            if (child_err.value == INVALID_HANDLE_VALUE)
                throw std::runtime_error("Cannot open relay stderr");
            Attributes attributes;
            HANDLE handles[] = { child_in.value, child_out.value, child_err.value };
            if (!UpdateProcThreadAttribute(
                    attributes.list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles), nullptr, nullptr))
                throw std::runtime_error("Cannot restrict GUI relay handle inheritance");
            STARTUPINFOEXW startup{};
            startup.StartupInfo.cb = sizeof(startup);
            startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            startup.StartupInfo.hStdInput = child_in.value;
            startup.StartupInfo.hStdOutput = child_out.value;
            startup.StartupInfo.hStdError = child_err.value;
            startup.lpAttributeList = attributes.list;
            PROCESS_INFORMATION info{};
            std::wstring command = L"\"" + relay.wstring() + L"\" --gui-bridge";
            if (!CreateProcessW(
                    relay.c_str(),
                    command.data(),
                    nullptr,
                    nullptr,
                    TRUE,
                    CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                    nullptr,
                    nullptr,
                    &startup.StartupInfo,
                    &info))
                throw std::runtime_error("Cannot launch installed GUI relay");
            child.value = info.hProcess;
            CloseHandle(info.hThread);
        }

        ~Process()
        {
            CloseHandle(std::exchange(input.value, nullptr));
            if (WaitForSingleObject(child.value, 2000) == WAIT_TIMEOUT)
            {
                terminate();
                WaitForSingleObject(child.value, 2000);
            }
        }

        void terminate() const
        {
            TerminateProcess(child.value, 1);
        }
        std::size_t send(const char* data, std::size_t size)
        {
            DWORD written{};
            if (!WriteFile(input.value, data, static_cast<DWORD>(size), &written, nullptr))
                return 0;
            return written;
        }
        std::size_t receive(char* data, std::size_t size)
        {
            DWORD count{};
            if (!ReadFile(output.value, data, static_cast<DWORD>(size), &count, nullptr))
                return 0;
            return count;
        }
#else
        Descriptor socket;
        pid_t child{ -1 };
        explicit Process(const fs::path& relay)
        {
            int pair[2];
            if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)
                throw std::system_error(errno, std::generic_category(), "Cannot create GUI relay socket pair");
            socket.value = pair[0];
            Descriptor remote{ pair[1] };
            if (fcntl(socket.value, F_SETFD, FD_CLOEXEC) < 0 || fcntl(remote.value, F_SETFD, FD_CLOEXEC) < 0)
                throw std::system_error(errno, std::generic_category(), "Cannot isolate GUI relay descriptors");
#ifdef __APPLE__
            int enabled = 1;
            if (setsockopt(socket.value, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0)
                throw std::system_error(errno, std::generic_category(), "Cannot configure GUI relay socket");
#endif
            FileActions actions;
            for (int fd : { STDIN_FILENO, STDOUT_FILENO })
                if (int error = posix_spawn_file_actions_adddup2(&actions.value, remote.value, fd))
                    throw std::system_error(error, std::generic_category(), "Cannot redirect GUI relay stdio");
            auto executable = relay.string();
            char flag[] = "--gui-bridge";
            char* argv[] = { executable.data(), flag, nullptr };
#ifdef __APPLE__
            char** env = *_NSGetEnviron();
#else
            char** env = environ;
#endif
            if (int error = posix_spawn(&child, executable.c_str(), &actions.value, nullptr, argv, env))
                throw std::system_error(error, std::generic_category(), "Cannot launch installed GUI relay");
        }

        ~Process()
        {
            close(std::exchange(socket.value, -1));
            for (int attempt = 0; attempt < 100; ++attempt)
            {
                const auto done = waitpid(child, nullptr, WNOHANG);
                if (done == child || (done < 0 && errno != EINTR))
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            terminate();
            while (waitpid(child, nullptr, 0) < 0 && errno == EINTR)
            {
            }
        }
        void terminate() const
        {
            kill(child, SIGKILL);
        }
        std::size_t send(const char* data, std::size_t size)
        {
#ifdef __APPLE__
            constexpr int flags = 0;
#else
            constexpr int flags = MSG_NOSIGNAL;
#endif
            ssize_t n;
            do
            {
                n = ::send(socket.value, data, size, flags);
            } while (n < 0 && errno == EINTR);
            return n > 0 ? static_cast<std::size_t>(n) : 0;
        }
        std::size_t receive(char* data, std::size_t size)
        {
            ssize_t n;
            do
            {
                n = read(socket.value, data, size);
            } while (n < 0 && errno == EINTR);
            return n > 0 ? static_cast<std::size_t>(n) : 0;
        }
#endif
    };

    GuiRelayProcess::GuiRelayProcess(std::string_view executable_name)
        : process_(std::make_unique<Process>(companion(executable_name)))
        , watchdog_(
              [this](std::stop_token stop)
              {
                  std::unique_lock lock(mutex_);
                  while (changed_.wait(
                      lock,
                      stop,
                      [this]
                      {
                          return deadline_.has_value();
                      }))
                  {
                      const auto until = *deadline_;
                      changed_.wait_until(
                          lock,
                          stop,
                          until,
                          [this, until]
                          {
                              return deadline_ != until;
                          });
                      if (stop.stop_requested())
                          break;
                      if (deadline_ == until && std::chrono::steady_clock::now() >= until)
                      {
                          process_->terminate();
                          break;
                      }
                  }
              })
    {
    }
    GuiRelayProcess::~GuiRelayProcess() = default;

    std::string GuiRelayProcess::Exchange(std::string frame)
    {
        if (frame.size() >= max_frame)
            throw std::runtime_error("GUI relay frame exceeds size limit");
        {
            std::lock_guard lock(mutex_);
            deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(35);
            changed_.notify_all();
        }
        frame.push_back('\n');
        std::size_t offset = 0;
        while (offset < frame.size())
        {
            const auto sent = process_->send(frame.data() + offset, frame.size() - offset);
            if (!sent)
                throw std::runtime_error("GUI relay closed its input");
            offset += sent;
        }
        std::string response;
        std::array<char, 8192> buffer{};
        while (response.size() < max_frame)
        {
            const auto received = process_->receive(buffer.data(), buffer.size());
            if (!received)
                throw std::runtime_error("GUI relay closed its output");
            response.append(buffer.data(), received);
            if (response.size() <= max_frame && response.back() == '\n')
            {
                std::lock_guard lock(mutex_);
                deadline_.reset();
                changed_.notify_all();
                return response;
            }
        }
        throw std::runtime_error("GUI relay response exceeds size limit");
    }
} // namespace HDMapping::Processing
