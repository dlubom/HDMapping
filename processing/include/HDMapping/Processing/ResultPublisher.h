#pragma once

#include <HDMapping/Processing/Operation.h>

namespace HDMapping::Processing
{
    struct PublishOptions
    {
        bool replace = false;
        std::string expected_artifact_id;
    };

    //! Consume a private sibling .hdmapping-result-stage-<id> built with CaptureSnapshot
    //! (kind native_session). The id is unique for each attempt. Shared inputs must be pinned
    //! independently of job cleanup. Replacement compares the current artifact id.
    RunResult PublishResult(
        const std::filesystem::path& staging_directory, const std::filesystem::path& destination, const PublishOptions& options = {});

    //! Discard an attempt's staging only after its writer has exited. The caller owns the attempt id.
    std::optional<OperationError> DiscardResultStaging(
        const std::filesystem::path& staging_directory, const std::filesystem::path& destination);

    //! Resolve the current committed result. The returned generation remains readable after replacement.
    RunResult ReadPublishedResult(const std::filesystem::path& destination);

    //! Copy a published session and its inputs into a relocatable result at a new destination.
    RunResult ExportComplete(const ArtifactManifest& source, const std::filesystem::path& destination);
} // namespace HDMapping::Processing
