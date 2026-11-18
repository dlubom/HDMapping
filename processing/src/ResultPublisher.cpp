#include <HDMapping/Processing/ResultPublisher.h>
#include <HDMapping/SessionPaths.h>

#include "NativeSessionResult.h"
#include "ResultPublisherIO.h"
#include "SnapshotStorage.h"

#include <algorithm>
#include <fstream>
#include <random>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#endif

namespace HDMapping::Processing
{
    namespace fs = std::filesystem;
    using nlohmann::json;

    namespace
    {
        constexpr std::string_view stage_prefix = ".hdmapping-result-stage-";
        constexpr std::string_view commit_prefix = ".hdmapping-result-commit-";
        constexpr std::string_view result_prefix = "result-";
        constexpr std::string_view result_manifest = "result.json";
        constexpr std::string_view snapshot_manifest = "snapshot.json";
        constexpr std::uintmax_t snapshot_limit = 16 * 1024 * 1024;

        struct PublicationConflict : std::runtime_error
        {
            using std::runtime_error::runtime_error;
        };

        OperationError failure(std::string code, const std::exception& error)
        {
            return { std::move(code), error.what(), json::object() };
        }

        std::string named(std::string_view prefix, std::string_view id)
        {
            return std::string(prefix) + std::string(id);
        }

        bool valid_id(std::string_view id)
        {
            constexpr std::string_view characters = "abcdefghijklmnopqrstuvwxyz0123456789-_";
            return !id.empty() && id.size() <= 80 && id.find_first_not_of(characters) == std::string_view::npos;
        }

        std::string staging_id(const fs::path& stage)
        {
            auto name = sessionPathText(stage.filename());
            if (!name.starts_with(stage_prefix) || !valid_id(std::string_view(name).substr(stage_prefix.size())))
            {
                throw std::invalid_argument("Expected a private HDMapping result staging directory");
            }
            return name.substr(stage_prefix.size());
        }

        fs::path location(const fs::path& requested)
        {
            if (requested.empty() || requested.filename().empty() || requested.filename() == "." || requested.filename() == "..")
            {
                throw std::invalid_argument("Expected a named directory with an existing parent");
            }
            return fs::canonical(fs::absolute(requested).parent_path()) / requested.filename();
        }

        void check_destination_name(const fs::path& path)
        {
            const auto name = sessionPathText(path.filename());
            if (name.starts_with(".hdmapping-result-") || name.ends_with(".hdmapping-publish.lock"))
            {
                throw std::invalid_argument("The destination uses a reserved publication name");
            }
#ifdef _WIN32
            if (name.ends_with('.') || name.ends_with(' ') || name.find(':') != std::string::npos)
            {
                throw std::invalid_argument("The destination has an ambiguous Windows name");
            }
#endif
        }

        fs::path destination_path(const fs::path& requested)
        {
            auto path = location(requested);
            check_destination_name(path);
#ifdef _WIN32
            // Resolve existing directory aliases (including NTFS short names) before naming the lock.
            if (fs::symlink_status(path).type() == fs::file_type::directory)
            {
                path = fs::canonical(path);
                check_destination_name(path);
            }
#endif
            for (auto parent = path.parent_path(); parent != parent.root_path(); parent = parent.parent_path())
            {
                if (fs::exists(parent / snapshot_manifest) || fs::exists(parent / result_manifest))
                {
                    throw std::invalid_argument("Publication cannot write inside owned snapshot data");
                }
            }
            return path;
        }

        // Keep the lock file so that every process uses the same lock even across a crash.
        // The OS releases the lock when the publishing process exits.
        class DestinationLock
        {
        public:
            explicit DestinationLock(const fs::path& destination)
            {
                auto path = destination;
                path += ".hdmapping-publish.lock";
#ifdef _WIN32
                handle = CreateFileW(
                    path.c_str(),
                    GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr,
                    OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                    nullptr);
                if (handle == INVALID_HANDLE_VALUE)
                {
                    throw fs::filesystem_error("Open result lock", path, { static_cast<int>(GetLastError()), std::system_category() });
                }
                BY_HANDLE_FILE_INFORMATION info{};
                OVERLAPPED range{};
                if (!GetFileInformationByHandle(handle, &info) ||
                    (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0 ||
                    !LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &range))
                {
                    CloseHandle(handle);
                    throw PublicationConflict("Could not reserve the result destination");
                }
#else
                handle = open(path.c_str(), O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
                if (handle < 0)
                {
                    throw fs::filesystem_error("Open result lock", path, { errno, std::generic_category() });
                }
                if (flock(handle, LOCK_EX | LOCK_NB) != 0)
                {
                    close(handle);
                    throw PublicationConflict("Could not reserve the result destination");
                }
#endif
            }

            DestinationLock(const DestinationLock&) = delete;
            DestinationLock& operator=(const DestinationLock&) = delete;

            ~DestinationLock()
            {
#ifdef _WIN32
                CloseHandle(handle);
#else
                close(handle);
#endif
            }

        private:
#ifdef _WIN32
            HANDLE handle;
#else
            int handle;
#endif
        };

        // Manage only an exclusively owned path. Never arm cleanup for a move that failed.
        struct TemporaryDirectory
        {
            fs::path path;
            bool owned = true;

            ~TemporaryDirectory()
            {
                if (owned)
                {
                    std::error_code ignored;
                    fs::remove_all(path, ignored);
                }
            }
        };

        void move_path(const fs::path& from, const fs::path& to, bool replace)
        {
#ifdef _WIN32
            const DWORD flags = MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0);
            if (!MoveFileExW(from.c_str(), to.c_str(), flags))
            {
                throw fs::filesystem_error(
                    "Commit published result", from, to, { static_cast<int>(GetLastError()), std::system_category() });
            }
#else
            if (replace)
            {
                fs::rename(from, to);
                return;
            }
#if defined(__APPLE__)
            if (renamex_np(from.c_str(), to.c_str(), RENAME_EXCL) == 0)
#elif defined(__linux__)
            if (renameat2(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE) == 0)
#else
            errno = ENOTSUP;
            if (false)
#endif
            {
                return;
            }
            throw fs::filesystem_error("Move published result without overwrite", from, to, { errno, std::generic_category() });
#endif
        }

        void write_manifest(const fs::path& path, const std::string& text)
        {
            std::ofstream output(path, std::ios::binary);
            output.exceptions(std::ios::failbit | std::ios::badbit);
            output << text;
            output.close();
            SyncSnapshotPath(path, false);
        }

        json read_json(const fs::path& path, std::uintmax_t limit)
        {
            if (fs::symlink_status(path).type() != fs::file_type::regular || fs::file_size(path) > limit)
            {
                throw std::runtime_error("Missing or invalid result manifest: " + sessionPathText(path));
            }
            std::ifstream input(path, std::ios::binary);
            return json::parse(input);
        }

        std::string current_id(const fs::path& destination)
        {
            if (fs::symlink_status(destination).type() != fs::file_type::directory)
            {
                throw PublicationConflict("Destination is not a published result directory");
            }
            const auto manifest = read_json(destination / result_manifest, 1024);
            const auto id = manifest.at("artifact_id").get<std::string>();
            const auto generation = destination / named(result_prefix, id);
            if (manifest.at("schema") != 1 || !valid_id(id) || fs::symlink_status(generation).type() != fs::file_type::directory ||
                fs::symlink_status(generation / snapshot_manifest).type() != fs::file_type::regular)
            {
                throw PublicationConflict("Invalid active result manifest");
            }
            return id;
        }

        std::pair<ArtifactManifest, SnapshotContents> read_artifact(const fs::path& root, const std::string& id)
        {
            const auto path = root / snapshot_manifest;
            const auto header = read_json(path, snapshot_limit);
            const auto& target = header.at("target");
            const TargetRef identity{ target.at("state_id").get<std::string>(), target.at("revision").get<std::uint64_t>() };
            SnapshotRef ref{ path, identity };
            auto snapshot = ReadSnapshot(ref);
            if (const auto* error = std::get_if<OperationError>(&snapshot))
            {
                throw std::runtime_error(error->message);
            }
            auto contents = std::get<SnapshotContents>(std::move(snapshot));
            if (contents.kind != native_session_kind)
            {
                throw std::runtime_error("Publication requires a native session snapshot");
            }
            ArtifactManifest artifact{ id, identity, std::move(ref), { path }, contents.dependencies };
            for (const auto& name : header.at("files").items())
            {
                artifact.files.push_back(root / sessionPathFromText(name.key()));
            }
            return { std::move(artifact), std::move(contents) };
        }

        class CompleteExport final : public CaptureSource
        {
        public:
            CompleteExport(const ArtifactManifest& manifest, const SnapshotContents& snapshot)
                : artifact(manifest)
                , contents(snapshot)
            {
            }

            TargetRef Target() const override
            {
                return artifact.target;
            }

            CaptureResult Materialize(const fs::path& directory, RunContext&) override
            {
                const auto copied = CopyNativeSession(contents, artifact, directory);
                if (const auto error = ValidateSnapshot(artifact.snapshot))
                {
                    return *error;
                }
                return copied;
            }

        private:
            const ArtifactManifest& artifact;
            const SnapshotContents& contents;
        };

        fs::path export_stage(const fs::path& base)
        {
            auto random = std::random_device{};
            std::string id;
            constexpr std::string_view hex = "0123456789abcdef";
            for (unsigned int index = 0; index < 32; ++index)
            {
                id.push_back(hex[random() % hex.size()]);
            }
            return base / named(stage_prefix, id);
        }
    } // namespace

    RunResult PublishResult(
        const fs::path& staging_directory, const fs::path& destination, const PublishOptions& options, const PublicationIO& io)
    {
        bool committed = false;
        try
        {
            const auto stage = location(staging_directory);
            const auto final = destination_path(destination);
            const auto id = staging_id(stage);
            if (stage.parent_path() != final.parent_path() || fs::symlink_status(stage).type() != fs::file_type::directory)
            {
                throw std::invalid_argument("Publication staging must be an owned sibling of its destination");
            }
            // Declare cleanup before acquiring the lock. A competing job only cleans its own staging.
            TemporaryDirectory input{ stage };
            if (options.replace != !options.expected_artifact_id.empty())
            {
                throw std::invalid_argument("Replacement requires an explicit expected artifact id");
            }
            const DestinationLock lock(final);
            const bool exists = fs::symlink_status(final).type() != fs::file_type::not_found;
            if (exists != options.replace || (exists && current_id(final) != options.expected_artifact_id))
            {
                throw PublicationConflict("Result destination already exists or its identity changed");
            }

            const auto generation = named(result_prefix, id);
            const auto published = final / generation;
            if (exists && fs::symlink_status(published).type() != fs::file_type::not_found)
            {
                throw PublicationConflict("Publication attempt identity was already used");
            }
            auto [artifact, contents] = read_artifact(stage, id);
            ValidateNativeSessionReferences(contents, artifact);
            VerifyNativeSessionRead(contents.entry);
            artifact.snapshot.manifest_path = published / snapshot_manifest;
            for (auto& file : artifact.files)
            {
                file = published / file.lexically_relative(stage);
            }

            const auto work = final.parent_path() / named(commit_prefix, id);
            if (!fs::create_directory(work))
            {
                throw PublicationConflict("Publication staging identity is already in use");
            }
            TemporaryDirectory transaction{ work };
            const auto head = work / result_manifest;
            io.write_manifest(
                head, json{ { "schema", 1 }, { "artifact_id", id }, { "destination", sessionPathText(final.filename()) } }.dump());
            const auto prepared = work / generation;
            io.move(stage, prepared, false);
            input.owned = false;
            SyncSnapshotPath(work, true);
            if (exists)
            {
                TemporaryDirectory unpublished{ published, false };
                io.move(prepared, published, false);
                unpublished.owned = true;
                SyncSnapshotPath(final, true);
                io.move(head, final / result_manifest, true);
                unpublished.owned = false;
            }
            else
            {
                io.move(work, final, false);
                transaction.owned = false;
            }
            committed = true;
            SyncSnapshotPath(exists ? final : final.parent_path(), true);
            return artifact;
        } catch (const PublicationConflict& error)
        {
            return failure("publication_conflict", error);
        } catch (const std::exception& error)
        {
            return failure(committed ? "publication_durability_unknown" : "publication_failed", error);
        }
    }

    RunResult PublishResult(const fs::path& stage, const fs::path& destination, const PublishOptions& options)
    {
        return PublishResult(stage, destination, options, { write_manifest, move_path });
    }

    std::optional<OperationError> DiscardResultStaging(const fs::path& staging_directory, const fs::path& destination)
    {
        try
        {
            const auto stage = location(staging_directory);
            const auto final = destination_path(destination);
            const auto id = staging_id(stage);
            if (stage.parent_path() != final.parent_path())
            {
                throw std::invalid_argument("Publication staging must be an owned sibling of its destination");
            }
            const DestinationLock lock(final);
            const auto work = final.parent_path() / named(commit_prefix, id);
            if (fs::symlink_status(work).type() == fs::file_type::directory)
            {
                const auto marker = work / result_manifest;
                const auto generation = named(result_prefix, id);
                const bool moved_out = !fs::exists(stage) && !fs::exists(work / generation);
                if (fs::exists(marker))
                {
                    json pending;
                    try
                    {
                        pending = read_json(marker, 1024);
                    } catch (const json::parse_error&)
                    {
                        // A writer can be interrupted while constructing the marker.
                        // Its own stage is still present until the marker is complete.
                        if (moved_out)
                            throw;
                    }
                    if (!pending.is_null())
                    {
                        const auto target = sessionPathFromText(pending.at("destination").get<std::string>());
                        std::error_code equivalent_error;
                        const bool same_destination = target == final.filename() ||
                            (target == target.filename() && fs::equivalent(final, final.parent_path() / target, equivalent_error));
                        if (pending.at("artifact_id").get<std::string>() != id || !same_destination)
                        {
                            throw std::invalid_argument("Publication staging belongs to a different destination");
                        }
                        // The atomic commit moves the marker away. Its presence is
                        // evidence that this generation was never published.
                        if (moved_out && fs::symlink_status(final / generation).type() == fs::file_type::directory &&
                            current_id(final) != id)
                        {
                            fs::remove_all(final / generation);
                        }
                    }
                }
                fs::remove_all(work);
            }
            auto preparing = stage;
            preparing += ".preparing";
            fs::remove_all(preparing);
            fs::remove_all(stage);
            return std::nullopt;
        } catch (const std::exception& error)
        {
            return failure("staging_cleanup_failed", error);
        }
    }

    RunResult ReadPublishedResult(const fs::path& destination)
    {
        try
        {
            const auto final = destination_path(destination);
            const auto id = current_id(final);
            auto [artifact, contents] = read_artifact(final / named(result_prefix, id), id);
            ValidateNativeSessionReferences(contents, artifact);
            return artifact;
        } catch (const std::exception& error)
        {
            return failure("invalid_published_result", error);
        }
    }

    RunResult ExportComplete(const ArtifactManifest& source, const fs::path& destination)
    {
        try
        {
            const auto final = destination_path(destination);
            if (fs::symlink_status(final).type() != fs::file_type::not_found)
            {
                throw PublicationConflict("Complete export requires a new destination");
            }
            const auto root = source.snapshot.manifest_path.parent_path();
            if (!valid_id(source.artifact_id) || root.filename() != named(result_prefix, source.artifact_id))
            {
                throw std::invalid_argument("Complete export requires a published artifact");
            }
            const auto [artifact, contents] = read_artifact(root, source.artifact_id);
            if (artifact.snapshot.manifest_path != source.snapshot.manifest_path || artifact.snapshot.target != source.snapshot.target ||
                artifact.target != source.target || artifact.files != source.files || artifact.dependencies != source.dependencies)
            {
                throw std::invalid_argument("Export artifact does not match its published snapshot");
            }
            ValidateNativeSessionReferences(contents, artifact);

            CompleteExport export_source(artifact, contents);
            RunContext context{};
            const auto stage = export_stage(final.parent_path());
            auto captured = CaptureSnapshot(export_source, artifact.target.revision, stage, context);
            if (const auto* error = std::get_if<OperationError>(&captured))
            {
                return *error;
            }
            return PublishResult(stage, final);
        } catch (const PublicationConflict& error)
        {
            return failure("publication_conflict", error);
        } catch (const std::exception& error)
        {
            return failure("complete_export_failed", error);
        }
    }
} // namespace HDMapping::Processing
