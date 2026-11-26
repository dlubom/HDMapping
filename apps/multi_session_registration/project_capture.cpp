#include "project_capture.h"

#include "NativeSessionResult.h"

#include <HDMapping/SessionPaths.h>

#include <algorithm>
#include <array>
#include <fstream>

namespace proc = HDMapping::Processing;
namespace fs = std::filesystem;

ProjectCapture::ProjectCapture(
    proc::TargetRef target,
    const std::vector<Session>& sessions,
    nlohmann::json project,
    nlohmann::json parameters,
    bool native,
    proc::ProjectSessionSources sources)
    : target_(std::move(target))
    , sessions_(sessions)
    , project_(std::move(project))
    , parameters_(std::move(parameters))
    , native_(native)
    , sources_(std::move(sources))
{
}

proc::TargetRef ProjectCapture::Target() const
{
    return target_;
}

proc::CaptureResult ProjectCapture::Materialize(const fs::path& directory, proc::RunContext& context)
{
    auto paths = nlohmann::json::array();
    auto children = nlohmann::json::array();
    for (const auto& session : sessions_)
    {
        const auto name = std::to_string(paths.size());
        const auto child = directory / name;
        // A parent mutation invalidates all child revisions, including index changes on removal.
        const auto identity = proc::ProjectChildTarget(target_, paths.size());
        const std::shared_ptr<const Session> borrowed(std::shared_ptr<const Session>{}, &session);
        auto tracks = nlohmann::json::object();
        if (const auto file = sessionPathFromText(session.session_file_name); file.extension() == ".mjs" && fs::is_regular_file(file))
            tracks = proc::ReadNativeSessionTracks(file);
        const auto metadata = proc::DescribeSessionExchange(session);
        fs::path entry;
        if (native_)
        {
            fs::create_directory(child);
            auto result = proc::WriteNativeSession(session, child, metadata, tracks, context);
            if (const auto* error = std::get_if<proc::OperationError>(&result))
                return *error;
            entry = child / std::get<proc::SnapshotContents>(result).entry;
        }
        else
        {
            // The reservation pins the parent vector through Materialize and window close.
            proc::SessionCaptureSource source(
                borrowed,
                [identity]
                {
                    return identity;
                },
                metadata,
                std::move(tracks));
            auto result = proc::CaptureSnapshot(source, identity.revision, child, context);
            if (const auto* error = std::get_if<proc::OperationError>(&result))
                return *error;
            auto contents = proc::ReadSnapshot(std::get<proc::SnapshotRef>(result), context.stop);
            if (const auto* error = std::get_if<proc::OperationError>(&contents))
                return *error;
            entry = std::get<proc::SnapshotContents>(contents).entry;
        }
        paths.push_back({ { "session_file_name", sessionPathText(entry.lexically_relative(directory)) } });
        const auto original = sources_.find(session.session_file_name);
        auto previous_poses = nlohmann::json::array();
        for (const auto& scan : session.point_clouds_container.point_clouds)
        {
            std::array<double, 16> matrix;
            std::copy_n(scan.m_pose_temp.matrix().data(), matrix.size(), matrix.begin());
            previous_poses.push_back(matrix);
        }
        children.push_back(
            { { "state_id", identity.state_id },
              { "revision", identity.revision },
              { "source", original == sources_.end() ? session.session_file_name : original->second },
              { "visible", session.visible },
              { "previous_poses", std::move(previous_poses) } });
        if (context.progress)
            context.progress({ "write_project_sessions", paths.size(), sessions_.size(), {} });
    }
    project_["session_file_names"] = std::move(paths);
    std::ofstream file(directory / "project.mjp", std::ios::binary);
    file.exceptions(std::ios::failbit | std::ios::badbit);
    file << project_.dump(2);
    file.close();
    parameters_["project_sessions"] = std::move(children);
    return proc::SnapshotContents{ native_ ? std::string(proc::native_project_kind) : "project", "project.mjp", parameters_, {} };
}
