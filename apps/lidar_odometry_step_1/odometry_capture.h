#pragma once

#include "lidar_odometry_utils.h"
#include <HDMapping/Processing/SnapshotStore.h>

//! Reads a prepared opening while its owner holds the GUI bridge reservation.
class OdometryCapture final : public HDMapping::Processing::CaptureSource
{
public:
    OdometryCapture(
        HDMapping::Processing::TargetRef target,
        const std::vector<WorkerData>& workers,
        const LidarOdometryParams& params,
        nlohmann::json settings,
        std::vector<std::filesystem::path> reference_files = {});
    HDMapping::Processing::TargetRef Target() const override;
    HDMapping::Processing::CaptureResult Materialize(
        const std::filesystem::path& directory, HDMapping::Processing::RunContext& context) override;

private:
    HDMapping::Processing::TargetRef target_;
    const std::vector<WorkerData>& workers_;
    const LidarOdometryParams& params_;
    nlohmann::json settings_;
    std::vector<std::filesystem::path> reference_files_;
};

//! Owned paths of reference inputs in capture order.
std::vector<std::filesystem::path> ReferenceCapturePaths(const std::vector<std::filesystem::path>& sources);

//! Restore the numerical state from a verified prepared snapshot. Parameters are read separately.
std::optional<HDMapping::Processing::OperationError> RestoreOdometry(
    const HDMapping::Processing::SnapshotContents& contents,
    std::vector<WorkerData>& workers,
    LidarOdometryParams& params,
    HDMapping::Processing::RunContext& context);

//! Verify and restore a prepared state.
std::optional<HDMapping::Processing::OperationError> RestoreOdometry(
    const HDMapping::Processing::SnapshotRef& snapshot,
    std::vector<WorkerData>& workers,
    LidarOdometryParams& params,
    HDMapping::Processing::RunContext& context);
