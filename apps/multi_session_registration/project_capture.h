#pragma once

#include <HDMapping/Processing/SessionSnapshot.h>

//! Materializes each loaded child while the GUI holds the project's reservation.
class ProjectCapture final : public HDMapping::Processing::CaptureSource
{
public:
    ProjectCapture(
        HDMapping::Processing::TargetRef target, const std::vector<Session>& sessions, nlohmann::json project, nlohmann::json parameters);
    HDMapping::Processing::TargetRef Target() const override;
    HDMapping::Processing::CaptureResult Materialize(
        const std::filesystem::path& directory, HDMapping::Processing::RunContext& context) override;

private:
    HDMapping::Processing::TargetRef target_;
    const std::vector<Session>& sessions_;
    nlohmann::json project_;
    nlohmann::json parameters_;
};
