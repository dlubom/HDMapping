#include <pch/pch.h>

#include <Core/tum.h>
#include <HDMapping/SessionPaths.h>

#include <Eigen/Eigen>
#include <spdlog/spdlog.h>

bool TUM::load_data_from_tum(const std::vector<std::string>& input_file_names, bool subtract_first_pose)
{
    tum_poses.clear();

    std::cout << "loading TUM trajectory data from following files:" << std::endl;
    for (const auto& fn : input_file_names)
    {
        std::cout << fn << std::endl;
    }

    for (const auto& fn : input_file_names)
    {
        std::ifstream infile(sessionPathFromText(fn));
        if (!infile.good())
        {
            std::cout << "problem with file: '" << fn << "'" << std::endl;
            return false;
        }
        std::string line;
        while (std::getline(infile, line))
        {
            TumPose pose{};
            std::istringstream fields(line);
            if (!(fields >> pose.timestamp >> pose.x >> pose.y >> pose.z >> pose.qx >> pose.qy >> pose.qz >> pose.qw))
                continue;
            const Eigen::Vector3d position(pose.x, pose.y, pose.z);
            const Eigen::Quaterniond rotation(pose.qw, pose.qx, pose.qy, pose.qz);
            if (std::isfinite(pose.timestamp) && position.allFinite() && rotation.coeffs().allFinite() &&
                std::isfinite(rotation.squaredNorm()) && rotation.squaredNorm() > 0.0)
                tum_poses.push_back(pose);
        }
        infile.close();
    }

    std::sort(
        tum_poses.begin(),
        tum_poses.end(),
        [](const TUM::TumPose& a, const TUM::TumPose& b)
        {
            return (a.timestamp < b.timestamp);
        });

    std::cout << "loaded " << tum_poses.size() << " TUM poses" << std::endl;

    if (subtract_first_pose && !tum_poses.empty())
    {
        const TumPose& first = tum_poses.front();
        Eigen::Affine3d T0 = Eigen::Affine3d::Identity();
        T0.translate(Eigen::Vector3d(first.x, first.y, first.z));
        T0.rotate(Eigen::Quaterniond(first.qw, first.qx, first.qy, first.qz).normalized());
        Eigen::Affine3d T0_inv = T0.inverse();

        for (auto& p : tum_poses)
        {
            Eigen::Affine3d T = Eigen::Affine3d::Identity();
            T.translate(Eigen::Vector3d(p.x, p.y, p.z));
            T.rotate(Eigen::Quaterniond(p.qw, p.qx, p.qy, p.qz).normalized());

            Eigen::Affine3d rel = T0_inv * T;
            const Eigen::Vector3d t = rel.translation();
            const Eigen::Quaterniond q(rel.rotation());

            p.x = t.x();
            p.y = t.y();
            p.z = t.z();
            p.qx = q.x();
            p.qy = q.y();
            p.qz = q.z();
            p.qw = q.w();
        }
    }

    if (!tum_poses.empty())
    {
        Eigen::Vector3d min_p(std::numeric_limits<double>::max(), std::numeric_limits<double>::max(), std::numeric_limits<double>::max());
        Eigen::Vector3d max_p(
            std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest());
        for (const auto& p : tum_poses)
        {
            min_p = min_p.cwiseMin(Eigen::Vector3d(p.x, p.y, p.z));
            max_p = max_p.cwiseMax(Eigen::Vector3d(p.x, p.y, p.z));
        }
        spdlog::info(
            "TUM bounding box: min=({}, {}, {}) max=({}, {}, {})", min_p.x(), min_p.y(), min_p.z(), max_p.x(), max_p.y(), max_p.z());
    }

    ++version;

    return true;
}
