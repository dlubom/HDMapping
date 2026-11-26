#include "step3_io_operations.h"

#include "project_operations.h"
#include "step3_export_formats.h"
#include "step3_operations.h"

#include <Core/export_laz.h>
#include <HDMapping/Processing/ResultPublisher.h>
#include <HDMapping/SessionPaths.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

namespace HDMapping::Processing
{
    namespace fs = std::filesystem;
    using nlohmann::json;

    namespace
    {
        constexpr std::array formats{ "project", "poses", "trajectory_tum", "trajectory_csv", "trajectory_las", "trajectory_laz",
                                      "las",     "laz",   "section_laz" };

        struct SessionSelection
        {
            std::size_t session;
            std::vector<std::size_t> scans;
        };

        json object_schema(json properties, json required)
        {
            return { { "type", "object" },
                     { "properties", std::move(properties) },
                     { "required", std::move(required) },
                     { "additionalProperties", false } };
        }

        std::size_t index_in(const json& index, std::size_t count)
        {
            if (!index.is_number_integer() || index < 0 || index >= count)
                throw std::invalid_argument("Project export index out of range");
            return index.get<std::size_t>();
        }

        std::vector<SessionSelection> selection(const json& scope, const std::vector<Session>& sessions)
        {
            const auto& selected = scope.at("sessions");
            if (scope.size() != 1 || !selected.is_array() || selected.empty())
                throw std::invalid_argument("Specify an explicit nonempty list of sessions");
            std::vector<bool> seen(sessions.size());
            std::vector<SessionSelection> result;
            for (const auto& group : selected)
            {
                if (!group.is_object() || group.size() != 1 + static_cast<std::size_t>(group.contains("scan_indices")))
                    throw std::invalid_argument("Each scope entry needs a session_index and may select scan_indices");
                const auto index = index_in(group.at("session_index"), sessions.size());
                if (seen[index])
                    throw std::invalid_argument("Duplicate session index in export scope");
                seen[index] = true;
                const auto count = sessions[index].point_clouds_container.point_clouds.size();
                auto& scans = result.emplace_back(SessionSelection{ index, {} }).scans;
                if (!group.contains("scan_indices"))
                {
                    if (count == 0)
                        throw std::invalid_argument("The selected session has no scans");
                    scans.resize(count);
                    std::iota(scans.begin(), scans.end(), std::size_t{ 0 });
                    continue;
                }
                const auto& indices = group.at("scan_indices");
                if (!indices.is_array() || indices.empty())
                    throw std::invalid_argument("Specify at least one scan index, or omit scan_indices for the entire session");
                for (const auto& scan : indices)
                    scans.push_back(index_in(scan, count));
                std::ranges::sort(scans);
                if (std::ranges::adjacent_find(scans) != scans.end())
                    throw std::invalid_argument("Duplicate scan index in export scope");
            }
            return result;
        }

        bool write_cloud(
            const Session& session,
            const SessionSelection& selected,
            const fs::path& path,
            std::string_view plane,
            double width,
            std::stop_token stop)
        {
            const auto& container = session.point_clouds_container;
            const auto& offset = container.offset;
            LazWriter writer;
            if (!writer.open(sessionPathText(path), offset.x(), offset.y(), offset.z(), false, path.extension() == ".laz"))
                return false;
            for (const auto index : selected.scans)
            {
                const auto& scan = container.point_clouds[index];
                const auto source = static_cast<unsigned short>(std::min<std::size_t>(index, 65535));
                for (std::size_t i = 0; i < scan.points_local.size(); ++i)
                {
                    if (stop.stop_requested())
                        return false;
                    const Eigen::Vector3d point = scan.m_pose * scan.points_local[i];
                    if ((plane == "xz" && std::abs(point.y()) >= width) || (plane == "yz" && std::abs(point.x()) >= width) ||
                        (plane == "xy" && std::abs(point.z()) >= width))
                        continue;
                    const auto intensity = i < scan.intensities.size() ? scan.intensities[i] : static_cast<unsigned short>(0);
                    const auto gps_time = i < scan.timestamps.size() ? scan.timestamps[i] : 0.0;
                    if (!writer.writePointWithGpsTime(point, intensity, gps_time, offset.x(), offset.y(), offset.z(), source))
                        return false;
                }
            }
            return writer.close();
        }

        //! Stage all selected files together under the P05 publisher.
        class ProjectExchange final : public CaptureSource
        {
        public:
            ProjectExchange(const OperationRequest& request, ProjectState& state, std::vector<SessionSelection> selected)
                : request_(request)
                , state_(state)
                , selected_(std::move(selected))
            {
            }

            TargetRef Target() const override
            {
                return request_.target;
            }

            CaptureResult Materialize(const fs::path& directory, RunContext& context) override
            {
                const auto& options = request_.parameters;
                const auto format = options.at("format").get<std::string>();
                const auto timestamps = options.value("timestamps", std::string("lidar"));
                const auto pose = options.value("pose", std::string("matrix"));
                const auto plane = options.value("plane", std::string());
                const auto width = options.value("width_m", 0.0);
                fs::path entry;
                auto outputs = json::array();
                for (const auto& group : selected_)
                {
                    if (context.stop.stop_requested())
                        return OperationError{ "cancelled", "Project export cancelled", json::object() };
                    auto& session = state_.sessions[group.session];
                    auto& clouds = session.point_clouds_container;
                    for (auto& scan : clouds.point_clouds)
                        scan.visible = false;
                    for (const auto scan : group.scans)
                        clouds.point_clouds[scan].visible = true;

                    const auto suffix = format.substr(format.find_last_of('_') + 1);
                    const auto file = fs::path("session-" + std::to_string(group.session) + "-" + format)
                                          .replace_extension(format == "poses" ? "mrp" : (suffix == "tum" ? "txt" : suffix));
                    const auto path = directory / file;
                    bool written = false;
                    if (format == "poses")
                    {
                        for (const auto index : group.scans)
                        {
                            auto& scan = clouds.point_clouds[index];
                            if (!scan.source_scan_name.empty())
                                scan.file_name = scan.source_scan_name;
                        }
                        written = clouds.save_poses(path, true, false, true);
                    }
                    else if (format == "las" || format == "laz" || format == "section_laz")
                        written = write_cloud(session, group, path, plane, width, context.stop);
                    else
                        written =
                            WriteProjectTrajectory(session, path, format, timestamps, pose, options.value("sort_tum", true), context.stop);
                    if (!written)
                        return OperationError{ context.stop.stop_requested() ? "cancelled" : "project_export_failed",
                                               "Could not write the selected project export",
                                               { { "session_index", group.session }, { "format", format } } };
                    if (entry.empty())
                        entry = file;
                    outputs.push_back(
                        { { "session_index", group.session }, { "scan_indices", group.scans }, { "file", sessionPathText(file) } });
                    if (context.progress)
                        context.progress({ "export_project_sessions", outputs.size(), selected_.size(), {} });
                }
                return SnapshotContents{
                    std::string(session_export_kind), entry, { { "format", format }, { "options", options }, { "sessions", outputs } }, {}
                };
            }

        private:
            const OperationRequest& request_;
            ProjectState& state_;
            std::vector<SessionSelection> selected_;
        };
    } // namespace

    OperationDescriptor DescribeProjectExport()
    {
        const json index = { { "type", "integer" }, { "minimum", 0 } };
        const auto scans = json{ { "type", "array" }, { "uniqueItems", true }, { "minItems", 1 }, { "items", index } };
        const auto sessions =
            json{ { "type", "array" },
                  { "minItems", 1 },
                  { "items", object_schema({ { "session_index", index }, { "scan_indices", scans } }, { "session_index" }) } };
        auto alternatives = json::array();
        for (const std::string_view format : formats)
        {
            auto properties = json{ { "format", { { "type", "string" }, { "const", format } } } };
            auto required = json::array({ "format" });
            if (format == "trajectory_csv")
            {
                properties["timestamps"] = { { "enum", { "lidar", "unix", "both" } }, { "default", "lidar" } };
                properties["pose"] = { { "enum", { "matrix", "quaternion" } }, { "default", "matrix" } };
            }
            if (format == "trajectory_tum")
                properties["sort_tum"] = { { "type", "boolean" }, { "default", true } };
            if (format == "section_laz")
            {
                properties["plane"] = { { "enum", { "xy", "xz", "yz" } } };
                properties["width_m"] = { { "type", "number" }, { "minimum", 0.001 } };
                required.push_back("plane");
                required.push_back("width_m");
            }
            alternatives.push_back(object_schema(std::move(properties), std::move(required)));
        }
        return { std::string(project_export_operation),
                 object_schema({ { "sessions", sessions } }, json::array()),
                 { { "oneOf", std::move(alternatives) } },
                 { { "type", "object" }, { "required", { "artifact_id", "target", "snapshot", "files", "dependencies" } } },
                 OperationClass::Heavy };
    }

    OperationDescriptor DescribeProjectListing()
    {
        const json index = { { "type", "integer" }, { "minimum", 0 } };
        return { std::string(project_listing_operation),
                 object_schema(
                     { { "kind", { { "enum", { "sessions", "scans", "constraints" } } } },
                       { "session_index", index },
                       { "offset", index },
                       { "limit", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 100 }, { "default", 50 } } } },
                     { "kind" }),
                 object_schema(json::object(), json::array()),
                 object_schema(
                     { { "items", { { "type", "array" } } }, { "total", index }, { "next_offset", index } }, { "items", "total" }),
                 OperationClass::Read };
    }

    json ProjectItemsPage(const std::vector<Session>& sessions, const std::vector<Edge>& edges, const json& request)
    {
        const auto kind = request.at("kind").get<std::string>();
        const bool scan_page = kind == "scans";
        if (kind != "sessions" && kind != "constraints" && !scan_page)
            throw std::invalid_argument("Project page kind must be sessions, scans or constraints");
        const auto expected = 1 + static_cast<std::size_t>(request.contains("offset")) +
            static_cast<std::size_t>(request.contains("limit")) + static_cast<std::size_t>(scan_page);
        if (!request.is_object() || request.size() != expected)
            throw std::invalid_argument("Unexpected project page scope");
        const auto session = scan_page ? index_in(request.at("session_index"), sessions.size()) : 0;
        const auto total = scan_page ? sessions[session].point_clouds_container.point_clouds.size()
                                     : (kind == "sessions" ? sessions.size() : edges.size());
        const auto offset = index_in(request.value("offset", json(0)), total + 1);
        const auto limit = index_in(request.value("limit", json(50)), 101);
        if (limit == 0)
            throw std::invalid_argument("Project page limit must be at least one");
        const auto end = offset + std::min(limit, total - offset);
        auto items = json::array();
        if (kind == "sessions")
            for (auto index = offset; index < end; ++index)
            {
                const auto& child = sessions[index];
                items.push_back(
                    { { "index", index },
                      { "ground_truth", child.is_ground_truth },
                      { "scans", child.point_clouds_container.point_clouds.size() } });
            }
        else if (scan_page)
            for (auto index = offset; index < end; ++index)
            {
                const auto& scan = sessions[session].point_clouds_container.point_clouds[index];
                items.push_back(
                    { { "index", index }, { "points", scan.points_local.size() }, { "trajectory_nodes", scan.local_trajectory.size() } });
            }
        else
        {
            items = ProjectEdgesJson(std::span<const Edge>(edges).subspan(offset, end - offset));
            for (std::size_t i = 0; i < items.size(); ++i)
                items[i]["index"] = offset + i;
        }
        json result{ { "items", std::move(items) }, { "total", total } };
        if (end < total)
            result["next_offset"] = end;
        return result;
    }

    RunResult ExportProject(const OperationRequest& request, ProjectState& state, RunContext& context)
    {
        try
        {
            const auto& parameters = request.parameters;
            const auto format = parameters.at("format").get<std::string>();
            if (std::ranges::find(formats, format) == formats.end())
                throw std::invalid_argument("Unknown project export format");
            if (format == "project")
            {
                if (!request.scope.empty() || parameters.size() != 1)
                    throw std::invalid_argument("A complete project export requires empty scope and only format");
                return PublishProject(state, request.target, context);
            }

            std::size_t recognized = 1;
            if (format == "trajectory_csv")
            {
                const auto time = parameters.value("timestamps", std::string("lidar"));
                const auto pose = parameters.value("pose", std::string("matrix"));
                if ((time != "lidar" && time != "unix" && time != "both") || (pose != "matrix" && pose != "quaternion"))
                    throw std::invalid_argument("Unknown CSV timestamp or pose representation");
                recognized +=
                    static_cast<std::size_t>(parameters.contains("timestamps")) + static_cast<std::size_t>(parameters.contains("pose"));
            }
            if (format == "trajectory_tum" && parameters.contains("sort_tum"))
            {
                static_cast<void>(parameters.at("sort_tum").get<bool>());
                ++recognized;
            }
            if (format == "section_laz")
            {
                const auto plane = parameters.at("plane").get<std::string>();
                const auto width = parameters.at("width_m").get<double>();
                if ((plane != "xy" && plane != "xz" && plane != "yz") || !std::isfinite(width) || width < 0.001)
                    throw std::invalid_argument("Specify xy, xz or yz and a slab half-width of at least 0.001 m");
                recognized += 2;
            }
            if (parameters.size() != recognized)
                throw std::invalid_argument("Unexpected options for the project export format");

            const auto selected = selection(request.scope, state.sessions);
            ProjectExchange exporter(request, state, selected);
            const auto stage = context.output_directory / ".hdmapping-result-stage-project-export";
            const auto destination = context.output_directory / "export";
            const auto captured = CaptureSnapshot(exporter, request.target.revision, stage, context);
            if (const auto* failed = std::get_if<OperationError>(&captured))
                return *failed;
            if (context.stop.stop_requested())
            {
                DiscardResultStaging(stage, destination);
                return OperationError{ "cancelled", "Project export cancelled", json::object() };
            }
            return PublishResult(stage, destination, {}, context.stop);
        } catch (const std::invalid_argument& error)
        {
            return OperationError{ "invalid_parameter", error.what(), json::object() };
        } catch (const json::exception& error)
        {
            return OperationError{ "invalid_parameter", error.what(), json::object() };
        }
    }
} // namespace HDMapping::Processing
