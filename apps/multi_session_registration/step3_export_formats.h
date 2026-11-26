#pragma once

#include <filesystem>
#include <stop_token>
#include <string_view>

class Session;

namespace HDMapping::Processing
{
    //! Write the visible scan trajectories using the native Step 3 text or LAS/LAZ layout.
    bool WriteProjectTrajectory(
        const Session& session,
        const std::filesystem::path& path,
        std::string_view format,
        std::string_view timestamps,
        std::string_view pose,
        bool sort_tum,
        std::stop_token stop = {});
} // namespace HDMapping::Processing
