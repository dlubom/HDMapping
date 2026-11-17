#include <HDMapping/Processing/SnapshotStore.h>

#include "SessionSnapshotPaths.h"

#include <array>
#include <fstream>
#include <set>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace HDMapping::Processing
{
    namespace fs = std::filesystem;
    using nlohmann::json;

    namespace
    {
        constexpr std::string_view manifest_name = "snapshot.json";
        constexpr std::string_view staging_suffix = ".preparing";
        constexpr std::uintmax_t manifest_limit = 16 * 1024 * 1024;

        struct Cancelled : std::exception
        {
        };

        void check_stop(std::stop_token stop)
        {
            if (stop.stop_requested())
            {
                throw Cancelled{};
            }
        }

        OperationError error(std::string code, std::string message)
        {
            return { std::move(code), std::move(message), json::object() };
        }

        std::string path_text(const fs::path& path)
        {
            const auto bytes = path.generic_u8string();
            return { bytes.begin(), bytes.end() };
        }

        fs::path read_path(const json& value)
        {
            const auto& bytes = value.get_ref<const std::string&>();
            if (bytes.empty() || bytes.find('\0') != std::string::npos)
            {
                throw std::runtime_error("Invalid snapshot path");
            }
            return fs::path(std::u8string(bytes.begin(), bytes.end()));
        }

        bool within(const fs::path& path, const fs::path& directory)
        {
            const auto relative = path.lexically_relative(directory);
            return !relative.empty() && !relative.has_root_path() && *relative.begin() != "..";
        }

        void check_external(const fs::path& path, const fs::path& own_directory)
        {
            if (within(path, own_directory))
            {
                throw std::runtime_error("External dependency points into owned data");
            }
            for (auto directory = path.parent_path(); directory != directory.root_path(); directory = directory.parent_path())
            {
                if (path_text(directory.filename()).ends_with(staging_suffix) || fs::exists(directory / manifest_name))
                {
                    throw std::runtime_error("External dependency points into another snapshot: " + path_text(path));
                }
            }
        }

        // Fingerprint detects input changes, including same-size edits. The local manifest
        // is not an authentication mechanism. External owners must still enforce immutability.
        json fingerprint(const fs::path& path, std::stop_token stop)
        {
            if (fs::symlink_status(path).type() != fs::file_type::regular)
            {
                throw std::runtime_error("Missing or non-regular snapshot input: " + path_text(path));
            }
            const auto size = fs::file_size(path);
            const auto modified = fs::last_write_time(path);
            std::ifstream input(path, std::ios::binary);
            std::array<char, 64 * 1024> buffer{};
            std::uint64_t hash = 14695981039346656037ULL;
            std::uintmax_t count = 0;
            while (input)
            {
                check_stop(stop);
                input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const auto read = input.gcount();
                count += static_cast<std::uintmax_t>(read);
                for (std::streamsize index = 0; index < read; ++index)
                {
                    hash ^= static_cast<unsigned char>(buffer.at(static_cast<std::size_t>(index)));
                    hash *= 1099511628211ULL;
                }
            }
            if (!input.eof() || input.bad() || count != size || modified != fs::last_write_time(path) || size != fs::file_size(path))
            {
                throw std::runtime_error("Snapshot input changed or could not be read: " + path_text(path));
            }
            return { { "size", size }, { "fnv1a64", hash } };
        }

        void sync_path(const fs::path& path, bool directory)
        {
#ifdef _WIN32
            if (directory)
            {
                return; // Publication uses a write-through move on Windows.
            }
            HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                throw fs::filesystem_error(
                    "Open snapshot input for sync", path, std::error_code(static_cast<int>(GetLastError()), std::system_category()));
            }
            const auto status = FlushFileBuffers(file) ? ERROR_SUCCESS : GetLastError();
            CloseHandle(file);
            if (status != ERROR_SUCCESS)
            {
                throw fs::filesystem_error("Sync snapshot input", path, std::error_code(static_cast<int>(status), std::system_category()));
            }
#else
            const int file = open(path.c_str(), (directory ? O_RDONLY | O_DIRECTORY : O_RDONLY) | O_CLOEXEC);
            if (file < 0)
            {
                throw fs::filesystem_error("Open snapshot input for sync", path, std::error_code(errno, std::generic_category()));
            }
            const auto status = fsync(file) == 0 ? 0 : errno;
            close(file);
            if (status != 0)
            {
                throw fs::filesystem_error("Sync snapshot input", path, std::error_code(status, std::generic_category()));
            }
#endif
        }

        class Stage
        {
        public:
            explicit Stage(const fs::path& destination)
                : path(destination)
            {
                path += staging_suffix;
                if (!fs::create_directory(path))
                {
                    throw std::runtime_error("Snapshot staging directory already exists");
                }
            }

            ~Stage()
            {
                if (!published)
                {
                    std::error_code ec;
                    fs::remove_all(path, ec);
                }
            }

            void publish(const fs::path& destination)
            {
                if (fs::symlink_status(destination).type() != fs::file_type::not_found)
                {
                    throw std::runtime_error("Snapshot destination already exists");
                }
#ifdef _WIN32
                if (!MoveFileExW(path.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH))
                {
                    throw fs::filesystem_error(
                        "Publish snapshot", destination, std::error_code(static_cast<int>(GetLastError()), std::system_category()));
                }
#else
                fs::rename(path, destination);
#endif
                path = destination;
                sync_path(destination.parent_path(), true);
                published = true;
            }

            fs::path path;
            bool published = false;
        };

        json inventory(const fs::path& directory, std::stop_token stop, bool capture)
        {
            json files = json::object();
            std::vector<fs::path> directories;
            for (const auto& input : fs::recursive_directory_iterator(directory))
            {
                check_stop(stop);
                const auto type = input.symlink_status().type();
                const auto relative = input.path().lexically_relative(directory);
                if (type == fs::file_type::directory)
                {
                    directories.push_back(input.path());
                    continue;
                }
                if (type != fs::file_type::regular || fs::hard_link_count(input.path()) != 1)
                {
                    throw std::runtime_error("Snapshot owned files must be regular and independent: " + path_text(input.path()));
                }
                if (relative != manifest_name)
                {
                    files[path_text(relative)] = fingerprint(input.path(), stop);
                    if (capture)
                    {
                        sync_path(input.path(), false);
                    }
                }
            }
            if (capture)
            {
                for (auto it = directories.rbegin(); it != directories.rend(); ++it)
                {
                    sync_path(*it, true);
                }
            }
            return files;
        }

        void check_contents(const SnapshotContents& contents, const json& files)
        {
            if (contents.kind.empty() || !contents.parameters.is_object() || !files.contains(path_text(contents.entry)))
            {
                throw std::runtime_error("Snapshot needs a kind, an owned entry file and a parameters object");
            }
        }

        void check_session_layout(const fs::path& directory, const SnapshotContents& contents, const json& files)
        {
            namespace layout = SessionSnapshotPaths;
            if (contents.kind != layout::kind)
            {
                return;
            }
            if (contents.entry != layout::entry || !files.contains(layout::poses) || !files.contains(layout::initial_poses) ||
                fs::file_size(directory / contents.entry) > manifest_limit)
            {
                throw std::runtime_error("Session snapshot is missing its metadata or native poses");
            }
            std::ifstream input(directory / contents.entry, std::ios::binary);
            const auto metadata = json::parse(input);
            const auto& settings = metadata.at("Session Settings");
            const auto& scans = metadata.at("laz_file_names");
            const std::string_view current = settings.at("poses_file_name").get_ref<const std::string&>();
            const std::string_view initial = settings.at("initial_poses_file_name").get_ref<const std::string&>();
            if (current != layout::poses || initial != layout::initial_poses || !scans.is_array() || scans.empty())
            {
                throw std::runtime_error("Invalid session snapshot references");
            }
            for (std::size_t i = 0; i < scans.size(); ++i)
            {
                const auto name = path_text(layout::cloud(i));
                if (scans.at(i).at("file_name") != name || !files.contains(name))
                {
                    throw std::runtime_error("Session snapshot is missing a cloud: " + name);
                }
            }
        }

        json read_manifest(const fs::path& path)
        {
            if (!path.is_absolute() || path.filename() != manifest_name ||
                path_text(path.parent_path().filename()).ends_with(staging_suffix) || fs::canonical(path) != path ||
                fs::symlink_status(path).type() != fs::file_type::regular || fs::file_size(path) > manifest_limit)
            {
                throw std::runtime_error("Invalid snapshot manifest path or size");
            }
            std::ifstream input(path, std::ios::binary);
            return json::parse(input);
        }
    } // namespace

    std::variant<SnapshotRef, OperationError> CaptureSnapshot(
        CaptureSource& source, std::uint64_t expected_revision, const fs::path& destination, RunContext& context)
    {
        try
        {
            check_stop(context.stop);
            const auto target = source.Target();
            if (target.state_id.empty() || target.revision != expected_revision)
            {
                return error("revision_conflict", "Capture target is missing or its revision changed");
            }
            const auto requested = fs::absolute(destination).lexically_normal();
            if (requested.filename().empty() || path_text(requested.filename()).ends_with(staging_suffix) ||
                fs::symlink_status(requested).type() != fs::file_type::not_found)
            {
                throw std::runtime_error("Snapshot requires a new destination directory");
            }
            const auto final = fs::canonical(requested.parent_path()) / requested.filename();
            Stage stage(final);
            auto materialized = source.Materialize(stage.path, context);
            check_stop(context.stop);
            if (const auto* failure = std::get_if<OperationError>(&materialized))
            {
                return *failure;
            }
            const auto& contents = std::get<SnapshotContents>(materialized);
            const auto files = inventory(stage.path, context.stop, true);
            check_contents(contents, files);
            check_session_layout(stage.path, contents, files);
            json dependencies = json::object();
            for (const auto& dependency : contents.dependencies)
            {
                if (!dependency.is_absolute())
                {
                    throw std::runtime_error("Snapshot external dependencies must be absolute");
                }
                const auto path = fs::canonical(dependency);
                check_external(path, stage.path);
                dependencies[path_text(path)] = fingerprint(path, context.stop);
            }
            const auto manifest_path = stage.path / manifest_name;
            const std::string manifest = json{
                { "schema", 1 },
                { "target", { { "state_id", target.state_id }, { "revision", target.revision } } },
                { "kind", contents.kind },
                { "entry", path_text(contents.entry) },
                { "parameters", contents.parameters },
                { "files", files },
                { "dependencies", dependencies }
            }.dump();
            if (manifest.size() > manifest_limit || fs::exists(manifest_path))
            {
                throw std::runtime_error("Snapshot manifest exceeds limit or uses a reserved file name");
            }
            std::ofstream output(manifest_path, std::ios::binary);
            output.exceptions(std::ios::failbit | std::ios::badbit);
            output << manifest;
            output.close();
            sync_path(manifest_path, false);
            sync_path(stage.path, true);
            if (source.Target() != target)
            {
                return error("revision_conflict", "Capture target changed during handoff");
            }
            check_stop(context.stop);
            stage.publish(final);
            return SnapshotRef{ final / manifest_name, target };
        } catch (const Cancelled&)
        {
            return error("cancelled", "Snapshot capture cancelled");
        } catch (const std::exception& failure)
        {
            if (context.stop.stop_requested())
            {
                return error("cancelled", "Snapshot capture cancelled");
            }
            return error("snapshot_capture_failed", failure.what());
        }
    }

    CaptureResult ReadSnapshot(const SnapshotRef& snapshot, std::stop_token stop)
    {
        try
        {
            check_stop(stop);
            const auto manifest = read_manifest(snapshot.manifest_path);
            const auto& target = manifest.at("target");
            if (manifest.at("schema") != 1 || snapshot.target.state_id.empty() || target.at("state_id") != snapshot.target.state_id ||
                !target.at("revision").is_number_unsigned() || target.at("revision") != snapshot.target.revision)
            {
                throw std::runtime_error("Unsupported snapshot schema or wrong target revision");
            }
            const auto root = snapshot.manifest_path.parent_path();
            const auto& files = manifest.at("files");
            SnapshotContents contents{
                manifest.at("kind").get<std::string>(), read_path(manifest.at("entry")), manifest.at("parameters"), {}
            };
            check_contents(contents, files);
            if (files != inventory(root, stop, false))
            {
                throw std::runtime_error("Snapshot owned files do not match the manifest");
            }
            check_session_layout(root, contents, files);
            const auto& dependencies = manifest.at("dependencies").get_ref<const json::object_t&>();
            for (const auto& [name, expected] : dependencies)
            {
                const auto path = read_path(name);
                if (!path.is_absolute() || fs::canonical(path) != path)
                {
                    throw std::runtime_error("Snapshot dependency does not have a canonical path: " + name);
                }
                check_external(path, root);
                if (fingerprint(path, stop) != expected)
                {
                    throw std::runtime_error("Snapshot dependency is missing or changed: " + name);
                }
                contents.dependencies.push_back(path);
            }
            check_stop(stop);
            contents.entry = root / contents.entry;
            return contents;
        } catch (const Cancelled&)
        {
            return error("cancelled", "Snapshot validation cancelled");
        } catch (const std::exception& failure)
        {
            return error("invalid_snapshot", failure.what());
        }
    }

    std::optional<OperationError> ValidateSnapshot(const SnapshotRef& snapshot, std::stop_token stop)
    {
        auto result = ReadSnapshot(snapshot, stop);
        if (auto* failure = std::get_if<OperationError>(&result))
        {
            return std::move(*failure);
        }
        return std::nullopt;
    }
} // namespace HDMapping::Processing
