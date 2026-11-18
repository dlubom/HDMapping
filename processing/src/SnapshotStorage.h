#pragma once

#include <filesystem>

namespace HDMapping::Processing
{
    //! Flush an owned file or directory during publication.
    void SyncSnapshotPath(const std::filesystem::path& path, bool directory);
} // namespace HDMapping::Processing
