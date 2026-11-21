#include <HDMapping/Processing/RuntimeProtocol.h>
#include <HDMapping/Processing/SnapshotStore.h>

#include "NativeSessionResult.h"
#include "SessionSnapshotPaths.h"

#include "RuntimeJson.h"
#include "RuntimeReceipt.h"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <thread>
#include <utility>

namespace HDMapping::Processing
{
    namespace
    {
        using nlohmann::json;

        constexpr unsigned wire_version = 1;
        constexpr std::size_t max_packet_bytes = 1024 * 1024;
        constexpr int max_json_depth = 64;

        std::optional<json> read_packet(std::istream& stream)
        {
            std::string line;
            for (int ch = stream.get(); ch != EOF; ch = stream.get())
            {
                if (ch == '\n')
                {
                    return json::parse(
                        line,
                        [](int depth, json::parse_event_t, json&)
                        {
                            if (depth > max_json_depth)
                            {
                                throw std::invalid_argument("Worker packet exceeds JSON depth limit");
                            }
                            return true;
                        });
                }
                if (line.size() >= max_packet_bytes)
                {
                    throw std::invalid_argument("Worker packet exceeds byte limit");
                }
                line.push_back(static_cast<char>(ch));
            }
            if (!line.empty() || !stream.eof() || stream.bad())
            {
                throw std::invalid_argument("Incomplete worker packet");
            }
            return std::nullopt;
        }

        std::string encode_packet(const json& packet)
        {
            auto line = packet.dump(-1, ' ', false, json::error_handler_t::replace);
            if (line.size() > max_packet_bytes)
            {
                throw std::length_error("Worker response exceeds byte limit");
            }
            line.push_back('\n');
            return line;
        }

        void send_packet(const SendWorkerPacket& send, const json& packet)
        {
            send(encode_packet(packet));
        }

        void send_error(const SendWorkerPacket& send, const OperationError& error)
        {
            send_packet(
                send,
                { { "type", "error" },
                  { "error", { { "code", error.code }, { "message", error.message }, { "details", error.details } } } });
        }

        using namespace RuntimeJson;

        //! One validated request to execute an operation on the supplied snapshot.
        struct RunCommand
        {
            std::string job_id;
            OperationRequest operation;
            SnapshotRef snapshot;
        };

        struct RecordingCommand
        {
            TargetRef target;
            std::filesystem::path recording, toml, destination;
        };

        struct ReferenceCommand
        {
            SnapshotRef base;
            std::vector<std::filesystem::path> files;
            std::filesystem::path destination;
        };

        std::filesystem::path absolute_path(const json& value)
        {
            const auto& text = value.get_ref<const std::string&>();
            const std::filesystem::path path(std::u8string(text.begin(), text.end()));
            if (!path.is_absolute() || text.find('\0') != std::string::npos)
                throw std::invalid_argument("Recording capture requires absolute paths");
            return path;
        }

        std::filesystem::path capture_destination(const json& packet)
        {
            const auto id = packet.at("job_id").get<std::string>();
            const auto destination = absolute_path(packet.at("directory"));
            if (id.size() != 32 || id.find_first_not_of("0123456789abcdef") != std::string::npos ||
                destination.filename() != "inputs-" + id)
                throw std::invalid_argument("Invalid recording capture job directory");
            return destination;
        }

        RecordingCommand parse_recording(const json& packet)
        {
            const auto& operation = packet.at("operation");
            const auto name = operation.at("operation").get<std::string>();
            if (name != "step1.load" && name != "step1.prepare" && name != "step1.odometry")
                throw std::invalid_argument("The requested operation does not accept a raw recording");
            const auto& recording = packet.at("recording");
            const auto toml = recording.contains("toml") ? absolute_path(recording.at("toml")) : std::filesystem::path{};
            return { parse_target(operation.at("target")), absolute_path(recording.at("directory")), toml, capture_destination(packet) };
        }

        std::pair<OperationRequest, std::filesystem::path> parse_session_open(const json& packet)
        {
            const auto& operation = packet.at("operation");
            if (operation.at("operation") != "session.open" || !operation.at("scope").is_object() ||
                !operation.at("parameters").is_object())
                throw std::invalid_argument("Invalid session.open capture");
            return { { "session.open", parse_target(operation.at("target")), operation.at("scope"), operation.at("parameters") },
                     capture_destination(packet) };
        }

        SnapshotRef parse_snapshot(const json& snapshot)
        {
            const auto& manifest = snapshot.at("manifest_path").get_ref<const std::string&>();
            if (manifest.empty() || manifest.find('\0') != std::string::npos)
            {
                throw std::invalid_argument("Invalid snapshot manifest path");
            }
            return { std::filesystem::path(std::u8string(manifest.begin(), manifest.end())), parse_target(snapshot.at("target")) };
        }

        ReferenceCommand parse_references(const json& packet)
        {
            auto base = parse_snapshot(packet.at("snapshot"));
            const auto& operation = packet.at("operation");
            const auto& scope = operation.at("scope");
            if (!base.manifest_path.is_absolute() || parse_target(operation.at("target")) != base.target ||
                operation.at("operation") != "step1.reference" || scope.at("mode") != "load" || !scope.at("files").is_array() ||
                scope.at("files").empty())
                throw std::invalid_argument("Invalid Step 1 reference capture request");
            std::vector<std::filesystem::path> files;
            for (const auto& item : scope.at("files"))
            {
                const auto& text = item.get_ref<const std::string&>();
                if (text.empty() || text.find('\0') != std::string::npos)
                    throw std::invalid_argument("Invalid path in Step 1 reference capture");
                files.emplace_back(std::u8string(text.begin(), text.end()));
            }
            return { std::move(base), std::move(files), capture_destination(packet) };
        }

        std::variant<json, OperationError> captured_snapshot(std::variant<SnapshotRef, OperationError> captured)
        {
            if (auto* failure = std::get_if<OperationError>(&captured))
                return std::move(*failure);
            const auto& ref = std::get<SnapshotRef>(captured);
            return json{ { "type", "snapshot_captured" },
                         { "snapshot",
                           { { "manifest_path", path_to_utf8(ref.manifest_path) }, { "target", target_to_json(ref.target) } } } };
        }

        RunCommand parse_run(const json& packet)
        {
            const auto& request = packet.at("operation");
            auto snapshot = parse_snapshot(packet.at("snapshot"));
            const auto& operation = request.at("operation").get_ref<const std::string&>();
            const auto& job_id = packet.at("job_id").get_ref<const std::string&>();
            const auto& scope = request.at("scope");
            const auto& parameters = request.at("parameters");
            const auto target = parse_target(request.at("target"));
            if (operation.empty() || job_id.empty() || !scope.is_object() || !parameters.is_object() || target != snapshot.target)
            {
                throw std::invalid_argument("Invalid operation request or mismatched snapshot target");
            }
            return { job_id, { operation, target, scope, parameters }, std::move(snapshot) };
        }

        std::string_view operation_class(OperationClass category)
        {
            switch (category)
            {
            case OperationClass::Read:
                return "read";
            case OperationClass::Mutate:
                return "mutate";
            case OperationClass::Heavy:
                return "heavy";
            }
            throw std::invalid_argument("Invalid operation class");
        }

        json describe(const std::vector<OperationDescriptor>& operations)
        {
            json descriptors = json::array();
            for (const auto& operation : operations)
            {
                descriptors.push_back(
                    { { "id", operation.id },
                      { "scope_schema", operation.scope_schema },
                      { "parameters_schema", operation.parameters_schema },
                      { "result_schema", operation.result_schema },
                      { "class", operation_class(operation.operation_class) } });
            }
            return { { "type", "operations" }, { "operations", std::move(descriptors) } };
        }

        void send_progress(const SendWorkerPacket& send, const ProgressEvent& event)
        {
            json progress{ { "stage", event.stage }, { "message", event.message } };
            if (event.current)
            {
                progress["current"] = *event.current;
            }
            if (event.total)
            {
                progress["total"] = *event.total;
            }
            send_packet(send, { { "type", "progress" }, { "progress", std::move(progress) } });
        }

        bool is_output_file(const std::filesystem::path& directory, const std::filesystem::path& file)
        {
            if (file.empty())
            {
                return false;
            }
            const auto absolute = std::filesystem::canonical(directory / file);
            const auto relative = absolute.lexically_relative(directory);
            return !relative.empty() && *relative.begin() != ".." && std::filesystem::is_regular_file(absolute);
        }

        std::optional<OperationError> execute_run(
            const RunCommand& command, const OperationExecutor& execute, RunContext& context, const WorkerIdentity& identity)
        {
            const auto result = execute(command.operation, command.snapshot, context);
            if (const auto* error = std::get_if<OperationError>(&result))
            {
                if (context.stop.stop_requested())
                    return OperationError{ "cancelled", "Worker was asked to stop", json::object() };
                return *error;
            }
            // A returned artifact has crossed its publication boundary. A late stop cannot undo it.
            const auto& manifest = std::get<ArtifactManifest>(result);
            const auto directory = std::filesystem::canonical(context.output_directory);
            if (manifest.target != command.operation.target || manifest.artifact_id.empty() || manifest.snapshot.target.state_id.empty() ||
                !is_output_file(directory, manifest.snapshot.manifest_path) ||
                !std::ranges::all_of(
                    manifest.files,
                    [&](const auto& file)
                    {
                        return is_output_file(directory, file);
                    }))
            {
                throw std::invalid_argument("Operation returned an invalid artifact manifest");
            }
            PublishRuntimeReceipt(
                context.output_directory,
                encode_packet(
                    { { "job_id", command.job_id },
                      { "wire", wire_version },
                      { "version", identity.version },
                      { "manifest", manifest_to_json(manifest) } }));
            return std::nullopt;
        }

        template<class Work>
        void run_cancellable(std::istream& input, const SendWorkerPacket& send, const std::filesystem::path& directory, const Work& work)
        {
            std::exception_ptr send_failure;
            std::jthread operation(
                [&](std::stop_token stop)
                {
                    try
                    {
                        RunContext context{ directory,
                                            std::move(stop),
                                            [&](const ProgressEvent& event)
                                            {
                                                send_progress(send, event);
                                            } };
                        std::variant<json, OperationError> result;
                        try
                        {
                            result = work(context);
                        } catch (const std::exception& exception)
                        {
                            result = OperationError{ "worker_failed", exception.what(), json::object() };
                        } catch (...)
                        {
                            result = OperationError{ "worker_failed", "Unhandled worker operation exception", json::object() };
                        }
                        if (const auto* error = std::get_if<OperationError>(&result))
                            send_error(send, *error);
                        else
                            send_packet(send, std::get<json>(result));
                    } catch (...)
                    {
                        send_failure = std::current_exception();
                    }
                });
            try
            {
                // Only cancel is valid here. Treat any other input as a broken supervisor
                // session, with the same cooperative stop policy as EOF.
                static_cast<void>(read_packet(input));
            } catch (const std::exception&)
            {
                // The executor still owns all output and may already have committed.
            }
            operation.request_stop();
            operation.join();
            if (send_failure)
            {
                std::rethrow_exception(send_failure);
            }
        }
    } // namespace

    void ServeWorker(
        std::istream& input,
        const SendWorkerPacket& send,
        const WorkerIdentity& identity,
        const std::filesystem::path& output_directory,
        const std::vector<OperationDescriptor>& operations,
        const OperationExecutor& execute,
        const CaptureRecording& capture_recording,
        const CaptureReferences& capture_references,
        const CaptureSessionOpen& capture_session_open)
    {
        send_packet(send, { { "type", "hello" }, { "wire", wire_version }, { "version", identity.version }, { "pid", identity.pid } });
        std::string type;
        std::optional<RunCommand> command;
        std::optional<SnapshotRef> snapshot;
        std::optional<RecordingCommand> recording;
        std::optional<ReferenceCommand> references;
        std::optional<std::pair<OperationRequest, std::filesystem::path>> session_open;
        try
        {
            const auto packet = read_packet(input);
            if (!packet)
            {
                return;
            }
            type = packet->at("type").get<std::string>();
            if (type == "run")
            {
                command = parse_run(*packet);
            }
            else if (type == "validate_snapshot")
            {
                snapshot = parse_snapshot(packet->at("snapshot"));
            }
            else if (type == "capture_step1_recording" && capture_recording)
            {
                recording = parse_recording(*packet);
            }
            else if (type == "capture_step1_references" && capture_references)
            {
                references = parse_references(*packet);
            }
            else if (type == "capture_step2_open" && capture_session_open)
            {
                session_open = parse_session_open(*packet);
            }
        } catch (const std::exception& error)
        {
            send_error(send, { "invalid_request", error.what(), json::object() });
            return;
        }
        if (type == "describe")
        {
            send_packet(send, describe(operations));
            return;
        }
        if (snapshot)
        {
            const auto result = ReadSnapshot(*snapshot);
            if (const auto* failure = std::get_if<OperationError>(&result))
            {
                send_error(send, *failure);
            }
            else if (const auto& kind = std::get<SnapshotContents>(result).kind; kind != SessionSnapshotPaths::kind &&
                     kind != native_session_kind && kind != "step1_recording" && kind != "step1_prepared")
            {
                send_error(send, { "invalid_snapshot", "Unsupported snapshot kind", json::object() });
            }
            else
            {
                send_packet(send, { { "type", "snapshot_validated" } });
            }
            return;
        }
        if (recording)
        {
            run_cancellable(
                input,
                send,
                recording->destination,
                [&](RunContext& context)
                {
                    return captured_snapshot(capture_recording(recording->target, recording->recording, recording->toml, context));
                });
            return;
        }
        if (references)
        {
            run_cancellable(
                input,
                send,
                references->destination,
                [&](RunContext& context)
                {
                    return captured_snapshot(capture_references(references->base, references->files, context));
                });
            return;
        }
        if (session_open)
        {
            run_cancellable(
                input,
                send,
                session_open->second,
                [&](RunContext& context)
                {
                    return captured_snapshot(capture_session_open(session_open->first, context));
                });
            return;
        }
        if (!command)
        {
            send_error(send, { "unsupported_command", "Unknown worker command", { { "command", type } } });
            return;
        }
        if (std::ranges::none_of(
                operations,
                [&](const OperationDescriptor& entry)
                {
                    return entry.id == command->operation.operation;
                }))
        {
            send_error(
                send, { "unsupported_operation", "Unknown processing operation", { { "operation", command->operation.operation } } });
            return;
        }
        run_cancellable(
            input,
            send,
            output_directory,
            [&](RunContext& context) -> std::variant<json, OperationError>
            {
                if (auto error = execute_run(*command, execute, context, identity))
                    return std::move(*error);
                return json{ { "type", "completed" } };
            });
    }
} // namespace HDMapping::Processing
