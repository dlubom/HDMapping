#pragma once

#include <HDMapping/Processing/Operation.h>

namespace HDMapping::Processing
{
    struct SnapshotContents
    {
        std::string kind;
        std::filesystem::path entry;
        nlohmann::json parameters;
        //! Persistent immutable inputs, independent of job cleanup. Copy temporary inputs into the capture directory.
        std::vector<std::filesystem::path> dependencies;
    };

    using CaptureResult = std::variant<SnapshotContents, OperationError>;

    //! The caller holds a source reservation through CaptureSnapshot. Materialize reads pinned data;
    //! Target is synchronized with edits, and a changed revision invalidates the handoff.
    class CaptureSource
    {
    public:
        virtual ~CaptureSource() = default;
        virtual TargetRef Target() const = 0;
        //! Write owned files into directory. Use the entry relative to directory and absolute dependency paths.
        virtual CaptureResult Materialize(const std::filesystem::path& directory, RunContext& context) = 0;
    };

    //! Publish a complete snapshot at a new directory on the same filesystem as its staging directory.
    std::variant<SnapshotRef, OperationError> CaptureSnapshot(
        CaptureSource& source, std::uint64_t expected_revision, const std::filesystem::path& destination, RunContext& context);

    //! Verify identity, owned files and immutable dependencies before admission or reuse.
    std::optional<OperationError> ValidateSnapshot(const SnapshotRef& snapshot, std::stop_token stop = {});

    //! Read the verified entry, parameters and external references for a domain loader.
    //! If requested, return the inventory of verified owned files.
    CaptureResult ReadSnapshot(
        const SnapshotRef& snapshot, std::stop_token stop = {}, std::vector<std::filesystem::path>* owned_files = nullptr);
} // namespace HDMapping::Processing
