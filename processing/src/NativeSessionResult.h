#pragma once

#include <HDMapping/Processing/SnapshotStore.h>

class Session;

namespace HDMapping::Processing
{
    inline constexpr std::string_view native_session_kind = "native_session";

    //! Preserve the Step 2 reference tracks and their ENU origin through capture and publication.
    nlohmann::json ReadNativeSessionTracks(const std::filesystem::path& entry);
    void WriteNativeSessionTracks(const std::filesystem::path& entry, const nlohmann::json& tracks);

    //! Verify that native MJS paths, including implicit trajectories, agree with the inventory.
    void ValidateNativeSessionReferences(const SnapshotContents& contents, const ArtifactManifest& manifest);

    //! Serialize a loaded session's scans, current/initial poses and native metadata into an owned directory.
    CaptureResult WriteNativeSession(
        const Session& session,
        const std::filesystem::path& directory,
        nlohmann::json parameters,
        const nlohmann::json& tracks,
        RunContext& context);

    //! Load the complete native session through HDMapping's MJS, RESSO and LAS/LAZ readers.
    std::size_t VerifyNativeSessionRead(const std::filesystem::path& entry);

    //! Materialize the same native session with only local references.
    SnapshotContents CopyNativeSession(
        const SnapshotContents& source, const ArtifactManifest& manifest, const std::filesystem::path& directory, std::stop_token stop);
} // namespace HDMapping::Processing
