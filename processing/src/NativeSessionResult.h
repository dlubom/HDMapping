#pragma once

#include <HDMapping/Processing/SnapshotStore.h>

namespace HDMapping::Processing
{
    inline constexpr std::string_view native_session_kind = "native_session";

    //! Verify that native MJS paths, including implicit trajectories, agree with the inventory.
    void ValidateNativeSessionReferences(const SnapshotContents& contents, const ArtifactManifest& manifest);

    //! Load the complete native session through HDMapping's MJS, RESSO and LAS/LAZ readers.
    void VerifyNativeSessionRead(const std::filesystem::path& entry);

    //! Materialize the same native session with only local references.
    SnapshotContents CopyNativeSession(
        const SnapshotContents& source, const ArtifactManifest& manifest, const std::filesystem::path& directory, std::stop_token stop);
} // namespace HDMapping::Processing
