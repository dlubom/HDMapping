#include "NativeSessionResult.h"

#include <Core/session.h>
#include <HDMapping/SessionPaths.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>

namespace HDMapping::Processing
{
    namespace fs = std::filesystem;
    using nlohmann::json;

    namespace
    {
        constexpr std::uintmax_t max_metadata_size = 16 * 1024 * 1024;

        fs::path reference(const json& value)
        {
            auto path = value.get<std::string>();
            if (path.empty() || path.find('\0') != std::string::npos)
            {
                throw std::runtime_error("Empty or invalid native session reference");
            }
            std::replace(path.begin(), path.end(), '\\', '/');
            return sessionPathFromText(path);
        }

        bool owned_reference(const fs::path& path)
        {
            return !path.has_root_path() && path == path.lexically_normal() && *path.begin() != "..";
        }

        json read_metadata(const fs::path& entry)
        {
            if (entry.extension() != ".mjs" || fs::file_size(entry) > max_metadata_size)
            {
                throw std::runtime_error("Expected a native MJS within the session metadata size limit");
            }
            std::ifstream input(entry, std::ios::binary);
            return json::parse(input);
        }

        void copy_owned_file(const fs::path& source, const fs::path& target)
        {
            fs::copy_file(source, target);
#ifdef _WIN32
            // CopyFile2 preserves the read-only attribute, while syncing needs a write handle.
            fs::permissions(target, fs::perms::owner_write, fs::perm_options::add);
#endif
        }

        void check_owned_metadata(const fs::path& base, const json& settings, const std::set<fs::path>& files)
        {
            for (const auto* key :
                 { "initial_poses_file_name", "poses_file_name", "out_poses_file_name", "point_sizes_path", "index_poses_path" })
            {
                const bool required = std::string_view(key) == "initial_poses_file_name" || std::string_view(key) == "poses_file_name";
                if (!required && settings.value(key, "").empty())
                {
                    continue;
                }
                const auto path = reference(settings.at(key));
                if (!owned_reference(path) || !files.contains(base / path) || fs::file_size(base / path) == 0)
                {
                    throw std::runtime_error(std::string("Native metadata must reference an owned, nonempty file: ") + key);
                }
            }
            for (const auto* key : { "folder_name", "out_folder_name" })
            {
                if (const auto value = settings.value(key, "."); !value.empty() && value != ".")
                {
                    throw std::runtime_error(std::string("Native session folder must refer to the entry directory: ") + key);
                }
            }
        }
    } // namespace

    json ReadNativeSessionTracks(const fs::path& entry)
    {
        auto metadata = read_metadata(entry);
        auto tracks = json::object();
        for (const auto* key : { "enu_origin", "gnss_measurements", "tum_trajectory" })
            if (auto field = metadata.find(key); field != metadata.end())
                tracks[key] = std::move(*field);
        return tracks;
    }

    void WriteNativeSessionTracks(const fs::path& entry, const json& tracks)
    {
        if (tracks.empty())
            return;
        auto metadata = read_metadata(entry);
        for (const auto* key : { "enu_origin", "gnss_measurements", "tum_trajectory" })
            if (const auto field = tracks.find(key); field != tracks.end())
                metadata[key] = *field;
        std::ofstream output(entry, std::ios::binary);
        output.exceptions(std::ios::failbit | std::ios::badbit);
        output << metadata;
        output.close();
    }

    void ValidateNativeSessionReferences(const SnapshotContents& contents, const ArtifactManifest& manifest)
    {
        if (contents.kind != native_session_kind)
        {
            throw std::runtime_error("Publication requires a native session snapshot");
        }
        const auto base = contents.entry.parent_path();
        const std::set<fs::path> files(manifest.files.begin(), manifest.files.end());
        const std::set<fs::path> inputs(contents.dependencies.begin(), contents.dependencies.end());
        const auto metadata = read_metadata(contents.entry);
        check_owned_metadata(base, metadata.at("Session Settings"), files);

        std::set<fs::path> used_inputs;
        std::set<fs::path> basenames;
        const auto& scans = metadata.at("laz_file_names").get_ref<const json::array_t&>();
        if (scans.empty())
        {
            throw std::runtime_error("Native session has no scans");
        }
        for (const auto& scan : scans)
        {
            const auto ref = reference(scan.at("file_name"));
            const auto cloud = base / ref;
            if (!basenames.insert(cloud.filename()).second)
            {
                throw std::runtime_error("Native pose names must uniquely identify their scans");
            }
            const auto sidecar = sessionTrajectoryPath(cloud);
            for (const auto& path : { cloud, sidecar })
            {
                if (path == sidecar && (sidecar.empty() || fs::symlink_status(sidecar).type() == fs::file_type::not_found))
                {
                    continue;
                }
                if (ref.is_absolute() && inputs.contains(path))
                {
                    used_inputs.insert(path);
                }
                else if (!owned_reference(ref) || !files.contains(path))
                {
                    throw std::runtime_error("Native scan or trajectory is missing from the result inventory: " + sessionPathText(path));
                }
            }
        }
        if (used_inputs != inputs)
        {
            throw std::runtime_error("Result declares external inputs not used by its native session");
        }
    }

    void VerifyNativeSessionRead(const fs::path& entry)
    {
        Session session;
        if (!session.load_published(entry))
        {
            throw std::runtime_error("HDMapping could not read the native session");
        }
        const auto& scans = session.point_clouds_container.point_clouds;
        const auto valid_scan = [&](int index)
        {
            return index >= 0 && static_cast<std::size_t>(index) < scans.size();
        };
        for (const auto& edge : session.pose_graph_loop_closure.edges)
        {
            if (!valid_scan(edge.index_from) || !valid_scan(edge.index_to))
                throw std::runtime_error("Native loop closure refers to a missing scan");
        }
        for (const auto& point : session.control_points.cps)
        {
            if (!valid_scan(point.index_to_pose))
                throw std::runtime_error("Native control point refers to a missing scan");
        }
        for (const auto& point : session.ground_control_points.gpcs)
        {
            if (!valid_scan(point.index_to_node_inner) || point.index_to_node_outer < 0 ||
                static_cast<std::size_t>(point.index_to_node_outer) >=
                    scans.at(static_cast<std::size_t>(point.index_to_node_inner)).local_trajectory.size())
            {
                throw std::runtime_error("Native ground control point refers to a missing trajectory node");
            }
        }
    }

    SnapshotContents CopyNativeSession(
        const SnapshotContents& source, const ArtifactManifest& manifest, const fs::path& directory, std::stop_token stop)
    {
        const auto check_stop = [&]
        {
            if (stop.stop_requested())
                throw std::runtime_error("Complete export cancelled");
        };
        const auto root = manifest.snapshot.manifest_path.parent_path();
        const auto rel_entry = source.entry.lexically_relative(root);
        const auto destination = directory / rel_entry;
        for (const auto& file : manifest.files)
        {
            check_stop();
            if (file != source.entry && file != manifest.snapshot.manifest_path)
            {
                const auto target = directory / file.lexically_relative(root);
                fs::create_directories(target.parent_path());
                copy_owned_file(file, target);
            }
        }
        auto metadata = read_metadata(source.entry);
        std::map<fs::path, fs::path> relocated_directories;
        auto& scans = metadata.at("laz_file_names");
        // External files from the same source folder share a local folder. Preserve the
        // basenames used by the native poses, and the adjacency of implicit trajectories.
        std::map<fs::path, fs::path> relocated_files;
        fs::create_directories(destination.parent_path());
        fs::path files_directory = destination.parent_path() / "shared-clouds";
        if (!source.dependencies.empty())
        {
            while (!fs::create_directory(files_directory))
            {
                files_directory += "-copy";
            }
        }
        for (const auto& dependency : source.dependencies)
        {
            check_stop();
            const auto [folder, added] =
                relocated_directories.try_emplace(dependency.parent_path(), files_directory / std::to_string(relocated_directories.size()));
            if (added)
            {
                fs::create_directory(folder->second);
            }
            const auto target = folder->second / dependency.filename();
            copy_owned_file(dependency, target);
            relocated_files.emplace(dependency, target.lexically_relative(destination.parent_path()));
        }
        for (auto& scan : scans)
        {
            auto& name = scan.at("file_name");
            if (const auto path = reference(name); path.is_absolute())
            {
                name = sessionPathText(relocated_files.at(path));
            }
        }
        std::ofstream output(destination, std::ios::binary);
        output.exceptions(std::ios::failbit | std::ios::badbit);
        output << metadata.dump(2);
        output.close();
        return { source.kind, rel_entry, source.parameters, {} };
    }
} // namespace HDMapping::Processing
