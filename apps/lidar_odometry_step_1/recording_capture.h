#pragma once

#include <HDMapping/Processing/SnapshotStore.h>

#include <memory>

namespace HDMapping::Processing
{
    //! Materialize a Mandeye recording and optional TOML before a job may be queued.
    class RecordingCapture final : public CaptureSource
    {
    public:
        RecordingCapture(
            TargetRef target,
            std::filesystem::path recording,
            std::filesystem::path toml = {},
            nlohmann::json settings = nlohmann::json::object(),
            std::unique_ptr<CaptureSource> seed = {});

        TargetRef Target() const override;
        CaptureResult Materialize(const std::filesystem::path& directory, RunContext& context) override;

    private:
        TargetRef target_;
        std::filesystem::path recording_, toml_;
        nlohmann::json settings_;
        std::unique_ptr<CaptureSource> seed_;
    };

    //! Capture new reference inputs together with the existing prepared state.
    std::variant<SnapshotRef, OperationError> CaptureStep1References(
        const SnapshotRef& base, const std::vector<std::filesystem::path>& references, RunContext& context);

    //! Copy a mutable Step 1 input into staging, stopping between blocks.
    bool CopyStep1Input(const std::filesystem::path& from, const std::filesystem::path& to, std::stop_token stop);
} // namespace HDMapping::Processing
