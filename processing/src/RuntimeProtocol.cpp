#include <HDMapping/Processing/RuntimeProtocol.h>
#include <HDMapping/Processing/SnapshotStore.h>

#include "SessionSnapshotPaths.h"

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

        std::string path_to_utf8(const std::filesystem::path& path)
        {
            const auto utf8 = path.u8string();
            return { utf8.begin(), utf8.end() };
        }

        json target_to_json(const TargetRef& target)
        {
            return { { "state_id", target.state_id }, { "revision", target.revision } };
        }

        json paths_to_json(const std::vector<std::filesystem::path>& paths)
        {
            json result = json::array();
            for (const auto& path : paths)
            {
                result.push_back(path_to_utf8(path));
            }
            return result;
        }

        json manifest_to_json(const ArtifactManifest& manifest)
        {
            return { { "artifact_id", manifest.artifact_id },
                     { "target", target_to_json(manifest.target) },
                     { "snapshot",
                       { { "manifest_path", path_to_utf8(manifest.snapshot.manifest_path) },
                         { "target", target_to_json(manifest.snapshot.target) } } },
                     { "files", paths_to_json(manifest.files) },
                     { "dependencies", paths_to_json(manifest.dependencies) } };
        }

        TargetRef parse_target(const json& target)
        {
            const auto& state = target.at("state_id").get_ref<const std::string&>();
            const auto& revision = target.at("revision");
            if (state.empty() || !revision.is_number_unsigned())
            {
                throw std::invalid_argument("Invalid target state or revision");
            }
            return { state, revision.get<std::uint64_t>() };
        }

        //! One validated request to execute an operation on the supplied snapshot.
        struct RunCommand
        {
            std::string job_id;
            OperationRequest operation;
            SnapshotRef snapshot;
        };

        SnapshotRef parse_snapshot(const json& snapshot)
        {
            const auto& manifest = snapshot.at("manifest_path").get_ref<const std::string&>();
            if (manifest.empty() || manifest.find('\0') != std::string::npos)
            {
                throw std::invalid_argument("Invalid snapshot manifest path");
            }
            return { std::filesystem::path(std::u8string(manifest.begin(), manifest.end())), parse_target(snapshot.at("target")) };
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
            if (context.stop.stop_requested())
            {
                return OperationError{ "cancelled", "Worker was asked to stop", json::object() };
            }
            if (const auto* error = std::get_if<OperationError>(&result))
            {
                return *error;
            }
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

        void run_operation(
            std::istream& input,
            const SendWorkerPacket& send,
            const RunCommand& command,
            const std::filesystem::path& directory,
            const WorkerIdentity& identity,
            const OperationExecutor& execute)
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
                        std::optional<OperationError> error;
                        try
                        {
                            error = execute_run(command, execute, context, identity);
                        } catch (const std::exception& exception)
                        {
                            error = OperationError{ "worker_failed", exception.what(), json::object() };
                        } catch (...)
                        {
                            error = OperationError{ "worker_failed", "Unhandled worker operation exception", json::object() };
                        }
                        if (!error)
                        {
                            send_packet(send, { { "type", "completed" } });
                        }
                        else
                        {
                            send_error(send, *error);
                        }
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
        const OperationExecutor& execute)
    {
        send_packet(send, { { "type", "hello" }, { "wire", wire_version }, { "version", identity.version }, { "pid", identity.pid } });
        std::string type;
        std::optional<RunCommand> command;
        std::optional<SnapshotRef> snapshot;
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
            else if (std::get<SnapshotContents>(result).kind != SessionSnapshotPaths::kind)
            {
                send_error(send, { "invalid_snapshot", "Unsupported snapshot kind", json::object() });
            }
            else
            {
                send_packet(send, { { "type", "snapshot_validated" } });
            }
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
        run_operation(input, send, *command, output_directory, identity, execute);
    }
} // namespace HDMapping::Processing
