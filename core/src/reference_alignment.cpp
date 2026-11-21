#include <Core/control_points.h>
#include <Core/gnss.h>
#include <Core/ground_control_points.h>
#include <Core/reference_alignment.h>
#include <Core/tum.h>

#include <Eigen/Cholesky>
#include <python-scripts/point-to-point-metrics/point_to_point_source_to_target_tait_bryan_wc_jacobian.h>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace HDMapping
{
    namespace
    {
        using Vector6 = Eigen::Matrix<double, 6, 1>;
        using Matrix6 = Eigen::Matrix<double, 6, 6>;

        struct Observation
        {
            Eigen::Vector3d source;
            Eigen::Vector3d target;
            Eigen::Vector3d weight;
        };

        bool validIndex(int index, std::size_t size)
        {
            return index >= 0 && static_cast<std::size_t>(index) < size;
        }

        bool validSigma(double sigma)
        {
            return sigma > 0.0 && std::isfinite(sigma) && std::isfinite(1.0 / (sigma * sigma)) && 1.0 / (sigma * sigma) > 0.0;
        }

        Eigen::Vector3d weight(double x, double y, double z)
        {
            return { 1.0 / (x * x), 1.0 / (y * y), 1.0 / (z * z) };
        }

        bool fit(PointClouds& clouds, const std::vector<Observation>& observations, int iterations, const std::function<bool()>& proceed)
        {
            if (observations.size() < 3)
                return false;

            Eigen::Affine3d correction = Eigen::Affine3d::Identity();
            for (int iteration = 0; iteration < iterations; ++iteration)
            {
                if (proceed && !proceed())
                    return false;
                Matrix6 normal = Matrix6::Zero();
                Vector6 rhs = Vector6::Zero();
                for (const auto& observation : observations)
                {
                    const Eigen::Vector3d source = correction * observation.source;
                    Eigen::Matrix<double, 3, 6, Eigen::RowMajor> jacobian;
                    point_to_point_source_to_target_tait_bryan_wc_jacobian(jacobian, 0, 0, 0, 0, 0, 0, source.x(), source.y(), source.z());
                    const Eigen::Matrix<double, 6, 3> weighted = -jacobian.transpose() * observation.weight.asDiagonal();
                    normal += weighted * -jacobian;
                    rhs += weighted * (observation.target - source);
                }
                if (!normal.allFinite() || !rhs.allFinite())
                    return false;
                const Eigen::LLT<Matrix6> solver(normal);
                if (solver.info() != Eigen::Success)
                    return false;
                const Vector6 delta = solver.solve(rhs);
                if (!delta.allFinite())
                    return false;
                TaitBryanPose pose{};
                pose.px = delta[0];
                pose.py = delta[1];
                pose.pz = delta[2];
                pose.om = delta[3];
                pose.fi = delta[4];
                pose.ka = delta[5];
                correction = affine_matrix_from_pose_tait_bryan(pose) * correction;
            }

            if (proceed && !proceed())
                return false;
            for (const auto& scan : clouds.point_clouds)
                if (!(correction * scan.m_pose).matrix().allFinite())
                    return false;
            for (auto& scan : clouds.point_clouds)
            {
                scan.m_pose = correction * scan.m_pose;
                scan.pose = pose_tait_bryan_from_affine_matrix(scan.m_pose);
                scan.gui_translation[0] = static_cast<float>(scan.pose.px);
                scan.gui_translation[1] = static_cast<float>(scan.pose.py);
                scan.gui_translation[2] = static_cast<float>(scan.pose.pz);
                scan.gui_rotation[0] = static_cast<float>(rad2deg(scan.pose.om));
                scan.gui_rotation[1] = static_cast<float>(rad2deg(scan.pose.fi));
                scan.gui_rotation[2] = static_cast<float>(rad2deg(scan.pose.ka));
            }
            return true;
        }

        template<typename Pose>
        bool alignTrajectory(
            PointClouds& clouds,
            const std::vector<Pose>& poses,
            double nanoseconds,
            double tolerance,
            Eigen::Vector3d (*position)(const Pose&),
            const std::function<bool()>& proceed)
        {
            const auto timestamp = [nanoseconds](const Pose& pose)
            {
                return pose.timestamp * nanoseconds;
            };
            if (!std::ranges::all_of(
                    poses,
                    [&](const Pose& pose)
                    {
                        return std::isfinite(timestamp(pose)) && position(pose).allFinite();
                    }) ||
                !std::ranges::is_sorted(poses, {}, timestamp))
                return false;

            std::vector<Observation> observations;
            observations.reserve(clouds.point_clouds.size());
            for (const auto& scan : clouds.point_clouds)
            {
                if (scan.timestamps.empty() || !std::isfinite(scan.timestamps.front()))
                    continue;
                const auto time = scan.timestamps.front();
                const auto next = std::ranges::lower_bound(poses, time, {}, timestamp);
                // Match the existing fusion rule: preceding sample, excluding the first.
                if (next - poses.begin() < 2)
                    continue;
                const auto& matched = *std::prev(next);
                if (time - timestamp(matched) < tolerance)
                    observations.push_back({ scan.m_pose.translation(), position(matched) - clouds.offset, Eigen::Vector3d::Ones() });
            }
            return fit(clouds, observations, 30, proceed);
        }
    } // namespace

    const char* CheckControlPoint(const PointClouds& clouds, const ControlPoint& point)
    {
        if (!validIndex(point.index_to_pose, clouds.point_clouds.size()))
            return "CP pose index is out of range";
        if (!Eigen::Vector3d(point.x_source_local, point.y_source_local, point.z_source_local).allFinite() ||
            !Eigen::Vector3d(point.x_target_global, point.y_target_global, point.z_target_global).allFinite() ||
            !validSigma(point.sigma_z) || (!point.is_z_0 && (!validSigma(point.sigma_x) || !validSigma(point.sigma_y))))
            return "CP requires finite coordinates and positive sigmas on the weighted axes";
        return nullptr;
    }

    const char* CheckGroundControlPoint(const PointClouds& clouds, const GroundControlPoint& point)
    {
        if (!validIndex(point.index_to_node_inner, clouds.point_clouds.size()) ||
            !validIndex(point.index_to_node_outer, clouds.point_clouds[point.index_to_node_inner].local_trajectory.size()))
            return "GCP scan or trajectory node index is out of range";
        if (!Eigen::Vector3d(point.x, point.y, point.z + point.lidar_height_above_ground).allFinite() || !validSigma(point.sigma_x) ||
            !validSigma(point.sigma_y) || !validSigma(point.sigma_z))
            return "GCP requires finite coordinates and sensor height, and positive sigmas";
        return nullptr;
    }

    bool AlignControlPoints(PointClouds& clouds, std::span<const ControlPoint> points, const std::function<bool()>& proceed)
    {
        std::vector<Observation> observations;
        observations.reserve(points.size());
        for (const auto& point : points)
        {
            if (CheckControlPoint(clouds, point))
                return false;
            const Eigen::Vector3d local(point.x_source_local, point.y_source_local, point.z_source_local);
            const Eigen::Vector3d target(point.x_target_global, point.y_target_global, point.is_z_0 ? 0.0 : point.z_target_global);
            const Eigen::Vector3d weights = point.is_z_0 ? Eigen::Vector3d(1e-6, 1e-6, 1.0 / (point.sigma_z * point.sigma_z))
                                                         : weight(point.sigma_x, point.sigma_y, point.sigma_z);
            observations.push_back({ clouds.point_clouds[point.index_to_pose].m_pose * local, target, weights });
        }
        return fit(clouds, observations, 1, proceed);
    }

    bool AlignGroundControlPoints(PointClouds& clouds, std::span<const GroundControlPoint> points, const std::function<bool()>& proceed)
    {
        std::vector<Observation> observations;
        observations.reserve(points.size());
        for (const auto& point : points)
        {
            if (CheckGroundControlPoint(clouds, point))
                return false;
            const auto& scan = clouds.point_clouds[point.index_to_node_inner];
            observations.push_back(
                { scan.m_pose * scan.local_trajectory[point.index_to_node_outer].m_pose.translation(),
                  Eigen::Vector3d(point.x, point.y, point.z + point.lidar_height_above_ground),
                  weight(point.sigma_x, point.sigma_y, point.sigma_z) });
        }
        return fit(clouds, observations, 1, proceed);
    }

    bool AlignGnss(PointClouds& clouds, const GNSS& gnss, const std::function<bool()>& proceed)
    {
        return alignTrajectory<GNSS::GlobalPose>(
            clouds,
            gnss.gnss_poses,
            1.0,
            10e12,
            [](const auto& p)
            {
                return Eigen::Vector3d(p.enu_x, p.enu_y, p.enu_z);
            },
            proceed);
    }

    bool AlignTum(PointClouds& clouds, const TUM& tum, const std::function<bool()>& proceed)
    {
        return alignTrajectory<TUM::TumPose>(
            clouds,
            tum.tum_poses,
            1e9,
            5e8,
            [](const auto& p)
            {
                return Eigen::Vector3d(p.x, p.y, p.z);
            },
            proceed);
    }
} // namespace HDMapping
