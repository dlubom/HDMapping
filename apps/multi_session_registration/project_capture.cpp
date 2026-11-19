#include "project_capture.h"

#include <Core/session.h>

#include <fstream>

namespace proc = HDMapping::Processing;
namespace fs = std::filesystem;

ProjectCapture::ProjectCapture(
    proc::TargetRef target, const std::vector<Session>& sessions, nlohmann::json project, nlohmann::json parameters)
    : target_(std::move(target))
    , sessions_(sessions)
    , project_(std::move(project))
    , parameters_(std::move(parameters))
{
}

proc::TargetRef ProjectCapture::Target() const
{
    return target_;
}

proc::CaptureResult ProjectCapture::Materialize(const fs::path& directory, proc::RunContext& context)
{
    auto paths = nlohmann::json::array();
    for (const auto& session : sessions_)
    {
        const auto name = std::to_string(paths.size());
        const auto child = directory / name;
        // The reservation pins the parent vector through Materialize and window close.
        const std::shared_ptr<const Session> borrowed(std::shared_ptr<const Session>{}, &session);
        proc::SessionCaptureSource source(
            borrowed,
            [this]
            {
                return target_;
            },
            nlohmann::json::object());
        auto result = proc::CaptureSnapshot(source, target_.revision, child, context);
        if (const auto* err = std::get_if<proc::OperationError>(&result))
            return *err;
        auto contents = proc::ReadSnapshot(std::get<proc::SnapshotRef>(result), context.stop);
        if (const auto* err = std::get_if<proc::OperationError>(&contents))
            return *err;
        const auto& entry = std::get<proc::SnapshotContents>(contents).entry;
        paths.push_back({ { "session_file_name", entry.lexically_relative(directory).generic_string() } });
    }
    project_["session_file_names"] = std::move(paths);
    std::ofstream file(directory / "project.mjp", std::ios::binary);
    file.exceptions(std::ios::failbit | std::ios::badbit);
    file << project_;
    file.close();
    return proc::SnapshotContents{ "project", "project.mjp", parameters_, {} };
}
