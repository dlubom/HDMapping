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
        SessionCaptureSource(
            std::shared_ptr<const Session> session,
            std::function<TargetRef()> target,
            nlohmann::json parameters,
            nlohmann::json tracks = nlohmann::json::object());

        TargetRef Target() const override;
        CaptureResult Materialize(const std::filesystem::path& directory, RunContext& context) override;

    private:
        std::shared_ptr<const Session> session_;
        std::function<TargetRef()> target_;
        nlohmann::json parameters_;
        nlohmann::json tracks_;
    };

    //! Metadata for Step 2 exchange formats, independent of private cache filenames.
    nlohmann::json DescribeSessionExchange(const Session& session);

    struct SessionSnapshot
    {
        std::unique_ptr<Session> session;
        nlohmann::json parameters;
        nlohmann::json tracks;
    };

    //! Restore a captured session, or read a native session with verified inventory references.
    std::variant<SessionSnapshot, OperationError> LoadSessionSnapshot(const SnapshotRef& snapshot, std::stop_token stop = {});
} // namespace HDMapping::Processing
