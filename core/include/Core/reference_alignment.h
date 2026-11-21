#pragma once

#include <functional>
#include <span>

class GNSS;
class TUM;
class PointClouds;
struct ControlPoint;
struct GroundControlPoint;

namespace HDMapping
{
    //! Validate pose associations and metric observations for the rigid reference fit.
    const char* CheckControlPoint(const PointClouds& clouds, const ControlPoint& point);
    const char* CheckGroundControlPoint(const PointClouds& clouds, const GroundControlPoint& point);

    //! Apply one rigid adjustment using the existing CP or GCP weights (coordinates and sigmas in metres).
    //! Return false on invalid or indeterminate input. No scan poses change on failure.
    bool AlignControlPoints(PointClouds& clouds, std::span<const ControlPoint> points, const std::function<bool()>& proceed = {});
    bool AlignGroundControlPoints(
        PointClouds& clouds, std::span<const GroundControlPoint> points, const std::function<bool()>& proceed = {});

    //! Rigidly align using timestamped GNSS (nanoseconds) or TUM (seconds) positions.
    bool AlignGnss(PointClouds& clouds, const GNSS& gnss, const std::function<bool()>& proceed = {});
    bool AlignTum(PointClouds& clouds, const TUM& tum, const std::function<bool()>& proceed = {});
} // namespace HDMapping
