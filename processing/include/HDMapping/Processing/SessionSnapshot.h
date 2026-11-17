#pragma once

#include <HDMapping/Processing/SnapshotStore.h>

#include <memory>

class Session;

namespace HDMapping::Processing
{
    //! Pins a session on its owning thread. The caller prevents writes to the pinned data and
    //! supplies a current target that detects replacement of the source while capture is active.
    class SessionCaptureSource final : public CaptureSource
    {
    public:
        SessionCaptureSource(std::shared_ptr<const Session> session, std::function<TargetRef()> target, nlohmann::json parameters);

        TargetRef Target() const override;
        CaptureResult Materialize(const std::filesystem::path& directory, RunContext& context) override;

    private:
        std::shared_ptr<const Session> session_;
        std::function<TargetRef()> target_;
        nlohmann::json parameters_;
    };

    struct SessionSnapshot
    {
        std::unique_ptr<Session> session;
        nlohmann::json parameters;
    };

    //! Restore native session metadata and the exact processing data held in memory at capture.
    std::variant<SessionSnapshot, OperationError> LoadSessionSnapshot(const SnapshotRef& snapshot);
} // namespace HDMapping::Processing
