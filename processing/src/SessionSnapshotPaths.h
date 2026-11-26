#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace HDMapping::Processing::SessionSnapshotPaths
{
    inline constexpr std::string_view kind = "session";
    inline constexpr std::string_view entry = "session.mjs";
    inline constexpr std::string_view poses = "poses.mrp";
    inline constexpr std::string_view initial_poses = "initial.mri";

    inline std::filesystem::path cloud(std::size_t index)
    {
        return "cloud_" + std::to_string(index) + ".bin";
    }
} // namespace HDMapping::Processing::SessionSnapshotPaths
