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
        nlohmann::json settings);
    HDMapping::Processing::TargetRef Target() const override;
    HDMapping::Processing::CaptureResult Materialize(
        const std::filesystem::path& directory, HDMapping::Processing::RunContext& context) override;

private:
    HDMapping::Processing::TargetRef target_;
    const std::vector<WorkerData>& workers_;
    const LidarOdometryParams& params_;
    nlohmann::json settings_;
};
