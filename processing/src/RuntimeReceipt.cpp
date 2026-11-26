#include "RuntimeReceipt.h"

#include <system_error>

#ifdef _WIN32
#include <windows.h>

#include <memory>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace HDMapping::Processing
{
    namespace
    {
#ifdef _WIN32
        //! Close the non-inheritable staging handle on failure or after publication.
        struct CloseFile
        {
            void operator()(void* handle) const noexcept
            {
                CloseHandle(handle);
            }
        };

        [[noreturn]] void file_error(const char* operation)
        {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
        }
#else
        //! Own the staging file or directory descriptor.
        class FileDescriptor
        {
        public:
            explicit FileDescriptor(int fd) noexcept
                : fd_(fd)
            {
            }
            FileDescriptor(const FileDescriptor&) = delete;
            FileDescriptor& operator=(const FileDescriptor&) = delete;
            ~FileDescriptor()
            {
                if (fd_ >= 0)
                {
                    close(fd_);
                }
            }
            int get() const noexcept
            {
                return fd_;
            }

        private:
            int fd_;
        };

        [[noreturn]] void file_error(const char* operation)
        {
            throw std::system_error(errno, std::generic_category(), operation);
        }

        void sync_file(int fd)
        {
            int result;
            do
            {
                result = fsync(fd);
            } while (result < 0 && errno == EINTR);
            if (result < 0)
            {
                file_error("sync worker receipt");
            }
        }

        void sync_directory(const std::filesystem::path& path)
        {
            const FileDescriptor directory(open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
            if (directory.get() < 0)
            {
                file_error("open worker output directory");
            }
            sync_file(directory.get());
        }
#endif
    } // namespace

    void PublishRuntimeReceipt(const std::filesystem::path& directory, std::string_view contents)
    {
        const auto temporary = directory / "result.json.tmp";
        const auto destination = directory / "result.json";
#ifdef _WIN32
        const auto handle =
            CreateFileW(temporary.c_str(), GENERIC_WRITE, FILE_SHARE_DELETE, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
        {
            file_error("create worker receipt");
        }
        const std::unique_ptr<void, CloseFile> file(handle);
        DWORD written = 0;
        if (!WriteFile(file.get(), contents.data(), static_cast<DWORD>(contents.size()), &written, nullptr))
        {
            file_error("write worker receipt");
        }
        if (written != contents.size())
        {
            throw std::system_error(std::make_error_code(std::errc::io_error), "short worker receipt write");
        }
        if (!FlushFileBuffers(file.get()))
        {
            file_error("sync worker receipt");
        }
        if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH))
        {
            file_error("publish worker receipt");
        }
        if (!FlushFileBuffers(file.get()))
        {
            file_error("sync published worker receipt");
        }
#else
        const FileDescriptor file(open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
        if (file.get() < 0)
        {
            file_error("create worker receipt");
        }
        while (!contents.empty())
        {
            const auto written = write(file.get(), contents.data(), contents.size());
            if (written < 0 && errno == EINTR)
            {
                continue;
            }
            if (written <= 0)
            {
                file_error("write worker receipt");
            }
            contents.remove_prefix(static_cast<std::size_t>(written));
        }
        sync_file(file.get());
        if (rename(temporary.c_str(), destination.c_str()) < 0)
        {
            file_error("publish worker receipt");
        }
        sync_directory(directory);
        const auto parent = std::filesystem::absolute(directory).lexically_normal().parent_path();
        sync_directory(parent);
#ifdef __APPLE__
        int result;
        do
        {
            result = fcntl(file.get(), F_FULLFSYNC);
        } while (result < 0 && errno == EINTR);
        if (result < 0)
        {
            file_error("flush published worker receipt");
        }
#endif
#endif
    }
} // namespace HDMapping::Processing
