#pragma once

#include <HDMapping/Processing/Operation.h>

#include <istream>
#include <string_view>

namespace HDMapping::Processing
{
    struct WorkerIdentity
    {
        std::string version;
        std::uint64_t pid{};
    };

    using SendWorkerPacket = std::function<void(std::string_view)>;
    using CaptureRecording = std::function<std::variant<SnapshotRef, OperationError>(
        const TargetRef&, const std::filesystem::path&, const std::filesystem::path&, RunContext&)>;
    using CaptureReferences = std::function<std::variant<SnapshotRef, OperationError>(
        const SnapshotRef&, const std::vector<std::filesystem::path>&, RunContext&)>;

    //! Serve one worker command. After a run starts, the supervisor sends cancel or closes input after the terminal packet.
    void ServeWorker(
        std::istream& input,
        const SendWorkerPacket& send,
        const WorkerIdentity& identity,
        const std::filesystem::path& output_directory,
        const std::vector<OperationDescriptor>& operations,
        const OperationExecutor& execute,
        const CaptureRecording& capture_recording = {},
        const CaptureReferences& capture_references = {});
} // namespace HDMapping::Processing
