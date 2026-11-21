#include "step2_io_operations.h"

#include "NativeSessionResult.h"
#include "step2_result.h"
#include "step2_session_exports.h"

#include <Core/e57_utils.h>
#include <Core/export_laz.h>
#include <Core/gnss.h>
#include <Core/reference_tracks.h>
#include <HDMapping/Processing/ResultPublisher.h>
#include <HDMapping/Processing/SessionSnapshot.h>
#include <HDMapping/SessionPaths.h>

#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <wgs84_do_puwg92/wgs84_do_puwg92.h>

namespace HDMapping::Processing
{
    namespace fs = std::filesystem;
    using nlohmann::json;

    namespace
    {
        constexpr std::array input_formats{ "mjs", "json", "las", "laz", "e57", "resso", "eth", "3dtk" };
        constexpr std::array export_formats{ "mjs",
                                             "las",
                                             "laz",
                                             "e57",
                                             "resso",
                                             "trajectory_las",
                                             "trajectory_laz",
                                             "trajectory_csv",
                                             "trajectory_dxf",
                                             "section_las",
                                             "section_laz",
                                             "gnss_las",
                                             "gnss_laz",
                                             "scale_board_laz",
                                             "e57_poses",
                                             "observations_json",
                                             "intersection_csv",
                                             "constraint_las",
                                             "constraint_laz",
                                             "projected_las",
                                             "projected_laz" };

        OperationError io_error(std::string code, std::string message)
        {
            return { std::move(code), std::move(message), json::object() };
        }

        json object_schema(json properties, json required = json::array())
        {
            return { { "type", "object" },
                     { "properties", std::move(properties) },
                     { "required", std::move(required) },
                     { "additionalProperties", false } };
        }

        json choice(const json& values)
        {
            return { { "type", "string" }, { "enum", values } };
        }

        void add_trajectory(PointCloud& scan)
        {
            auto& node = scan.local_trajectory.emplace_back();
            node.m_pose = Eigen::Affine3d::Identity();
            node.timestamps = { 0, 0 };
            node.imu_om_fi_ka = Eigen::Vector3d::Zero();
            node.imu_diff_angle_om_fi_ka_deg = Eigen::Vector3d::Zero();
        }

        bool read_e57(
            PointClouds& clouds,
            const std::vector<std::string>& paths,
            bool decimate,
            double x,
            double y,
            double z,
            std::string& error,
            std::stop_token stop)
        {
            for (const auto& path : paths)
            {
                std::vector<mandeye::e57io::E57Scan> scans;
                if (stop.stop_requested() || !mandeye::e57io::load_e57(path, scans, error))
                    return false;
                for (std::size_t index = 0; index < scans.size(); ++index)
                {
                    auto& source = scans[index];
                    auto& scan = clouds.point_clouds.emplace_back();
                    scan.file_name = source.name.empty() ? sessionPathText(sessionPathFromText(path).stem()) : source.name;
                    if (source.name.empty() && scans.size() > 1)
                        scan.file_name += "_" + std::to_string(index);
                    scan.file_name += ".laz";
                    scan.laz_in_memory_only = true;
                    if (source.data3d_index > std::numeric_limits<int>::max())
                        return false;
                    scan.e57_source_path = path;
                    scan.e57_scan_index = static_cast<int>(source.data3d_index);
                    scan.m_pose = scan.m_initial_pose = scan.m_pose_temp = source.pose;
                    scan.points_local = std::move(source.points);
                    scan.intensities = std::move(source.intensities);
                    scan.colors = std::move(source.colors);
                    scan.timestamps = std::move(source.timestamps);
                    scan.intensities.resize(scan.points_local.size(), 0);
                    scan.timestamps.resize(scan.points_local.size(), 0.0);
                    if (decimate)
                        scan.decimate(x, y, z);
                }
            }
            return true;
        }

        //! Import through the application loaders, then pin the decoded state in P04's session snapshot.
        class SessionOpenSource final : public CaptureSource
        {
        public:
            explicit SessionOpenSource(const OperationRequest& request)
                : request_(request)
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
                if (std::ranges::find(input_formats, format) == input_formats.end() || request_.scope != json::object())
                    return io_error("invalid_parameter", "Use a supported session.open format and an empty scope");
                const auto paths = options.at("paths").get<std::vector<std::string>>();
                const bool multiple = format == "las" || format == "laz" || format == "e57" || format == "3dtk";
                if (paths.empty() || (!multiple && paths.size() != 1))
                    return io_error("invalid_parameter", "Wrong number of session input paths");
                const std::string extension = format == "resso" ? "reg" : (format == "eth" || format == "3dtk" ? "txt" : format);
                for (const auto& text : paths)
                {
                    const auto path = sessionPathFromText(text);
                    auto ext = sessionPathText(path.extension());
                    for (auto& ch : ext)
                        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    if (text.find('\0') != std::string::npos || !path.is_absolute() || ext != "." + extension || !fs::is_regular_file(path))
                        return io_error("invalid_parameter", "Session inputs must be existing absolute paths matching the selected format");
                }
                const bool decimate = options.value("decimate", false);
                const bool offset = options.value("calculate_offset", false);
                const auto buckets = options.value("buckets_m", std::array{ 0.1, 0.1, 0.1 });
                if (options.contains("buckets_m") && (!options["buckets_m"].is_array() || options["buckets_m"].size() != 3))
                    return io_error("invalid_parameter", "Specify three decimation bucket dimensions");
                for (const auto bucket : buckets)
                    if (!std::isfinite(bucket) || bucket <= 0.0)
                        return io_error("invalid_parameter", "Decimation buckets must be finite and positive");

                const auto first = sessionPathFromText(paths.front());
                auto session = std::make_shared<Session>();
                auto& clouds = session->point_clouds_container;
                const auto [x, y, z] = buckets;
                const auto folder = sessionPathText(first.parent_path());
                std::string error;
                json tracks = json::object();
                bool success = false;
                if (format == "mjs" || format == "json")
                {
                    success = session->load(paths.front(), decimate, x, y, z, offset);
                    if (success)
                        tracks = ReadNativeSessionTracks(first);
                }
                else if (format == "las" || format == "laz")
                    success = clouds.load_whu_tls(paths, decimate, x, y, z, offset, false, true);
                else if (format == "e57")
                    success = read_e57(clouds, paths, decimate, x, y, z, error, context.stop);
                else if (format == "resso")
                    success = clouds.load(folder, paths.front(), decimate, x, y, z, false);
                else if (format == "eth")
                    success = clouds.load_eth(folder, paths.front(), decimate, x, y, z);
                else if (format == "3dtk")
                    success = clouds.load_3DTK_tls(paths, decimate, x, y, z);

                if (context.stop.stop_requested())
                    return io_error("cancelled", "Session import cancelled");
                if (!success || clouds.point_clouds.empty() || !clouds.offset.allFinite())
                    return io_error("session_open_failed", error.empty() ? "Could not read a session with scans" : error);
                if (options.value("fill_in_session", false))
                {
                    for (auto& scan : clouds.point_clouds)
                    {
                        if (format == "e57" && scan.local_trajectory.empty())
                            add_trajectory(scan);
                        if (format == "las" || format == "laz")
                        {
                            scan.m_pose = scan.m_pose_temp = scan.m_initial_pose = Eigen::Affine3d::Identity();
                            if (scan.points_local.size() > 100)
                                add_trajectory(scan);
                        }
                    }
                }

                auto metadata = DescribeSessionExchange(*session);
                metadata["session_open"] = true;
                SessionCaptureSource capture(
                    std::move(session),
                    [this]
                    {
                        return Target();
                    },
                    std::move(metadata),
                    std::move(tracks));
                return capture.Materialize(directory, context);
            }

        private:
            const OperationRequest& request_;
        };

        std::vector<std::size_t> select_scans(const SessionSnapshot& source, const json& scope)
        {
            const auto& scans = source.session->point_clouds_container.point_clouds;
            if (!scope.is_object() || scope.size() != 1)
                throw std::invalid_argument("Specify scan_indices or visible:true");
            std::set<std::size_t> selected;
            const auto add = [&](const json& indices)
            {
                if (!indices.is_array())
                    throw std::invalid_argument("scan_indices must be an array");
                for (const auto& index : indices)
                    if (!index.is_number_integer() || index.get<std::uint64_t>() >= scans.size() ||
                        !selected.insert(index.get<std::size_t>()).second)
                        throw std::invalid_argument("Only distinct scan indices from the selected revision are allowed");
            };
            if (scope.contains("scan_indices"))
                add(scope.at("scan_indices"));
            else if (scope.value("visible", false))
            {
                if (source.parameters.contains("visible_scans"))
                    add(source.parameters.at("visible_scans"));
                else
                    for (std::size_t i = 0; i < scans.size(); ++i)
                        if (scans[i].visible)
                            selected.insert(i);
            }
            if (selected.empty())
                throw std::invalid_argument("The scan scope is empty");
            return { selected.begin(), selected.end() };
        }

        std::string scan_file_name(const SessionSnapshot& source, std::size_t index)
        {
            const auto names = source.parameters.find("scan_names");
            if (names != source.parameters.end() && names->is_array() && index < names->size())
                return names->at(index).get<std::string>();
            return sessionPathText(sessionPathFromText(source.session->point_clouds_container.point_clouds[index].file_name).filename());
        }

        std::string scan_name(const SessionSnapshot& source, std::size_t index)
        {
            return sessionPathText(sessionPathFromText(scan_file_name(source, index)).stem());
        }

        std::optional<OperationError> import_session_data(
            const OperationRequest& request, SessionSnapshot& source, const std::vector<std::size_t>& selected)
        {
            const auto files = request.parameters.at("paths").get<std::vector<std::string>>();
            if (files.size() != 1 || !sessionPathFromText(files.front()).is_absolute())
                return io_error("invalid_parameter", "Specify one absolute import path");
            const auto path = sessionPathFromText(files.front());
            auto& scans = source.session->point_clouds_container.point_clouds;
            if (request.operation == import_session_poses_operation)
            {
                const auto mode = request.parameters.value("mode", std::string("current"));
                if (mode != "current" && mode != "initial" && mode != "inverse")
                    return io_error("invalid_parameter", "Use initial, current or inverse RESSO poses");
                PointClouds poses;
                std::set<std::string> names;
                for (const auto index : selected)
                {
                    auto name = scan_file_name(source, index);
                    if (name.empty() || !names.insert(name).second)
                        return io_error("invalid_scope", "Selected scan names must match distinct RESSO entries");
                    poses.point_clouds.emplace_back().file_name = std::move(name);
                }
                if (!poses.update_poses_from_RESSO({}, path, true, true))
                    return io_error("session_import_failed", "The RESSO file does not contain exactly the selected scan names");
                source.parameters["session_io_pose_scope"] = { { "scan_indices", selected } };
                source.parameters["session_io_initial_poses"] = mode == "initial";
                for (std::size_t i = 0; i < selected.size(); ++i)
                {
                    auto pose = poses.point_clouds[i].m_pose;
                    if (mode == "inverse")
                        pose = pose.inverse();
                    const auto determinant = pose.linear().determinant();
                    if (!pose.matrix().allFinite() || !std::isfinite(determinant) || std::abs(determinant) <= 1e-12 ||
                        pose.translation().cwiseAbs().maxCoeff() > std::numeric_limits<float>::max())
                        return io_error("invalid_parameter", "The RESSO file requires finite, invertible poses");
                    auto& scan = scans[selected[i]];
                    if (mode == "initial")
                        scan.m_initial_pose = pose;
                    else
                        scan.m_pose = pose;
                }
            }
            else
            {
                std::ifstream input(path);
                const auto document = json::parse(input);
                const auto& observations = document.at("observations").get_ref<const json::array_t&>();
                auto imported = json::array();
                for (const auto& observation : observations)
                {
                    auto group = json::array();
                    std::set<std::size_t> seen;
                    for (const auto& point : observation.get_ref<const json::array_t&>())
                    {
                        const auto& scan = point.at("scan_id");
                        const auto& coordinates = point.at("local_coords");
                        if (!scan.is_number_integer() || scan.get<std::uint64_t>() >= scans.size() || !coordinates.is_array() ||
                            coordinates.size() != 3)
                            return io_error("session_import_failed", "Invalid scan index or observation coordinates");
                        for (const auto& coordinate : coordinates)
                            if (!coordinate.is_number() || !std::isfinite(coordinate.get<double>()))
                                return io_error("session_import_failed", "Invalid observation coordinates");
                        const auto index = scan.get<std::size_t>();
                        if (!seen.insert(index).second)
                            return io_error("session_import_failed", "Duplicate scan in an observation");
                        if (std::ranges::binary_search(selected, index))
                            group.push_back(point);
                    }
                    if (!group.empty())
                        imported.push_back(std::move(group));
                }
                source.parameters["observations"] = std::move(imported);
            }
            source.parameters["session_io_import"] = request.operation;
            return {};
        }

        void keep_scans(SessionSnapshot& source, const std::vector<std::size_t>& selected)
        {
            auto& session = *source.session;
            auto& scans = session.point_clouds_container.point_clouds;
            std::vector<int> mapping(scans.size(), -1);
            std::vector<PointCloud> subset;
            auto names = json::array();
            auto visible = json::array();
            auto origins = json::array();
            const auto origin_list = source.parameters.find("e57_origins");
            const auto visibility = source.parameters.find("visible_scans");
            for (const auto index : selected)
            {
                mapping[index] = static_cast<int>(subset.size());
                names.push_back(scan_file_name(source, index));
                const bool originally_visible = visibility == source.parameters.end()
                    ? scans[index].visible
                    : std::ranges::find(*visibility, json(index)) != visibility->end();
                if (originally_visible)
                    visible.push_back(subset.size());
                if (origin_list != source.parameters.end())
                    origins.push_back(origin_list->at(index));
                subset.push_back(std::move(scans[index]));
            }
            const auto remap = [&](int& index)
            {
                if (index < 0 || static_cast<std::size_t>(index) >= mapping.size())
                    return false;
                index = mapping[static_cast<std::size_t>(index)];
                return index >= 0;
            };
            auto& edges = session.pose_graph_loop_closure.edges;
            std::erase_if(
                edges,
                [&](const auto& edge)
                {
                    return edge.index_from < 0 || edge.index_to < 0 || static_cast<std::size_t>(edge.index_from) >= mapping.size() ||
                        static_cast<std::size_t>(edge.index_to) >= mapping.size() || mapping[edge.index_from] < 0 ||
                        mapping[edge.index_to] < 0;
                });
            for (auto& edge : edges)
            {
                remap(edge.index_from);
                remap(edge.index_to);
            }
            auto& cp = session.control_points.cps;
            std::erase_if(
                cp,
                [&](const auto& point)
                {
                    auto index = point.index_to_pose;
                    return !remap(index);
                });
            for (auto& point : cp)
                remap(point.index_to_pose);
            auto& gcp = session.ground_control_points.gpcs;
            std::erase_if(
                gcp,
                [&](const auto& point)
                {
                    auto index = point.index_to_node_inner;
                    return !remap(index);
                });
            for (auto& point : gcp)
                remap(point.index_to_node_inner);
            if (auto observations = source.parameters.find("observations"); observations != source.parameters.end())
            {
                for (auto& observation : *observations)
                {
                    auto filtered = json::array();
                    for (auto& point : observation)
                    {
                        int index = point.at("scan_id").get<int>();
                        if (remap(index))
                        {
                            point["scan_id"] = index;
                            filtered.push_back(std::move(point));
                        }
                    }
                    observation = std::move(filtered);
                }
            }
            scans = std::move(subset);
            source.parameters["scan_names"] = std::move(names);
            if (!origins.empty())
                source.parameters["e57_origins"] = std::move(origins);
            source.parameters["source_scan_indices"] = selected;
            source.parameters["visible_scans"] = std::move(visible);
        }

        bool write_e57(const fs::path& path, const SessionSnapshot& source, const std::vector<std::size_t>& selected, std::string& error)
        {
            std::vector<mandeye::e57io::E57WriteScan> scans;
            const auto& clouds = source.session->point_clouds_container;
            for (const auto index : selected)
            {
                const auto& pc = clouds.point_clouds[index];
                auto& scan = scans.emplace_back();
                scan.name = scan_name(source, index);
                scan.points = &pc.points_local;
                scan.intensities = &pc.intensities;
                scan.colors = &pc.colors;
                scan.timestamps = &pc.timestamps;
                scan.pose = pc.m_pose;
                scan.pose.translation() += clouds.offset;
            }
            if (!mandeye::e57io::save_e57(sessionPathText(path), scans, error))
                return false;
            std::vector<mandeye::e57io::E57Scan> readback;
            return mandeye::e57io::load_e57(sessionPathText(path), readback, error) && readback.size() == scans.size();
        }

        std::optional<fs::path> separate_clouds(
            const fs::path& directory,
            const SessionSnapshot& source,
            const std::vector<std::size_t>& selected,
            const std::string& format,
            std::stop_token stop)
        {
            const auto& clouds = source.session->point_clouds_container;
            const auto& offset = clouds.offset;
            fs::path first;
            for (const auto index : selected)
            {
                const auto& scan = clouds.point_clouds[index];
                const auto name = sessionPathFromText(std::to_string(index) + "_processed." + format);
                LazWriter writer;
                const bool rgb = !scan.colors.empty() && scan.colors.size() == scan.points_local.size();
                if (!writer.open(sessionPathText(directory / name), offset.x(), offset.y(), offset.z(), rgb))
                    return {};
                for (std::size_t i = 0; i < scan.points_local.size(); ++i)
                {
                    if (stop.stop_requested())
                        return {};
                    std::array<std::uint16_t, 3> color{};
                    if (rgb)
                        for (std::size_t c = 0; c < color.size(); ++c)
                        {
                            const auto value = scan.colors[i][static_cast<Eigen::Index>(c)];
                            if (std::isfinite(value))
                                color[c] = static_cast<std::uint16_t>(std::round(std::clamp(value * 256, 0.0, 65535.0)));
                        }
                    const auto intensity = i < scan.intensities.size() ? scan.intensities[i] : static_cast<unsigned short>(0);
                    const auto time = i < scan.timestamps.size() ? scan.timestamps[i] : 0.0;
                    if (!writer.writePointWithGpsTime(
                            scan.m_pose * scan.points_local[i],
                            intensity,
                            time,
                            offset.x(),
                            offset.y(),
                            offset.z(),
                            static_cast<unsigned short>(std::min<std::size_t>(index, 65535)),
                            rgb ? color.data() : nullptr))
                        return {};
                }
                if (!writer.close())
                    return {};
                if (first.empty())
                    first = name;
            }
            return first;
        }

        bool write_observations(const fs::path& path, const json& observations, const std::vector<std::size_t>& selected)
        {
            auto scoped = json::array();
            for (const auto& observation : observations)
            {
                auto points = json::array();
                for (const auto& point : observation)
                    if (std::ranges::binary_search(selected, point.at("scan_id").get<std::size_t>()))
                        points.push_back(point);
                if (!points.empty())
                    scoped.push_back(std::move(points));
            }
            std::ofstream file(path);
            file.exceptions(std::ios::badbit | std::ios::failbit);
            file << json{ { "observations", std::move(scoped) } }.dump(2);
            file.close();
            return true;
        }

        bool write_roi(const fs::path& directory, SessionSnapshot& source, const std::vector<std::size_t>& selected, std::stop_token stop)
        {
            const auto& regions = source.parameters.at("intersections");
            const auto& observations = source.parameters.at("observations");
            if (regions.empty())
                return false;
            auto& clouds = source.session->point_clouds_container;
            std::ofstream rms(directory / "rms.csv");
            rms.exceptions(std::ios::badbit | std::ios::failbit);
            rms << "index_roi, rms_initial, rms_result\n";
            for (std::size_t roi_index = 0; roi_index < regions.size(); ++roi_index)
            {
                const auto& roi = regions.at(roi_index);
                const auto translation = roi.at("translation").get<std::array<double, 3>>();
                const auto rotation = roi.at("rotation").get<std::array<double, 3>>();
                const auto size = roi.at("size").get<std::array<double, 3>>();
                const TaitBryanPose pose{ translation[0], translation[1], translation[2], rotation[0], rotation[1], rotation[2] };
                const auto local = affine_matrix_from_pose_tait_bryan(pose).inverse();
                const Eigen::Vector3d half_size(size[0] * 0.5, size[1] * 0.5, size[2] * 0.5);
                const auto stem = "intersection_" + std::to_string(roi_index);
                std::ofstream initial(directory / (stem + "_initial.csv"));
                std::ofstream result(directory / (stem + "_result.csv"));
                for (auto* stream : { &initial, &result })
                {
                    stream->exceptions(std::ios::badbit | std::ios::failbit);
                    *stream << "x;y;z;pc_index;is_initial;index_intersection;file\n";
                }
                for (const auto index : selected)
                {
                    const auto name = scan_file_name(source, index);
                    const auto& scan = clouds.point_clouds[index];
                    for (const auto& point : scan.points_local)
                    {
                        if (stop.stop_requested())
                            return false;
                        const Eigen::Vector3d before = local * (scan.m_initial_pose * point);
                        const Eigen::Vector3d after = local * (scan.m_pose * point);
                        if ((before.cwiseAbs().array() < half_size.array()).all())
                            initial << before.x() << ';' << before.y() << ';' << before.z() << ';' << index << ";1;" << roi_index << ';'
                                    << name << '\n';
                        if ((after.cwiseAbs().array() < half_size.array()).all())
                            result << after.x() << ';' << after.y() << ';' << after.z() << ';' << index << ";0;" << roi_index << ';' << name
                                   << '\n';
                    }
                }
                initial.close();
                result.close();

                if (roi_index >= observations.size())
                    continue;
                std::vector<Eigen::Vector2d> before, after;
                for (const auto& point : observations.at(roi_index))
                {
                    const auto index = point.at("scan_id").get<std::size_t>();
                    if (std::ranges::binary_search(selected, index))
                    {
                        const auto coords = point.at("local_coords").get<std::array<double, 3>>();
                        const Eigen::Map<const Eigen::Vector3d> location(coords.data());
                        const auto& scan = clouds.point_clouds[index];
                        before.push_back((scan.m_initial_pose * location).head<2>());
                        after.push_back((scan.m_pose * location).head<2>());
                    }
                }
                double before_sum = 0, after_sum = 0;
                std::size_t pair_count = 0;
                for (std::size_t a = 0; a < before.size(); ++a)
                    for (std::size_t b = a + 1; b < before.size(); ++b)
                    {
                        before_sum += (before[a] - before[b]).squaredNorm();
                        after_sum += (after[a] - after[b]).squaredNorm();
                        ++pair_count;
                    }
                if (pair_count != 0)
                {
                    const auto divisor = 2.0 * static_cast<double>(pair_count);
                    rms << roi_index << ';' << std::sqrt(before_sum / divisor) << ';' << std::sqrt(after_sum / divisor) << '\n';
                }
            }
            rms.close();
            for (const auto index : selected)
                clouds.point_clouds[index].file_name = scan_file_name(source, index);
            return clouds.save_poses(directory / "poses_RESSO.reg", true);
        }

        bool write_constraint(
            const std::string& path, const Session& session, const std::vector<std::size_t>& selected, const json& options)
        {
            const auto& edges = session.pose_graph_loop_closure.edges;
            const auto edge_index = options.at("edge_index").get<std::size_t>();
            const auto endpoint = options.at("endpoint").get<std::string>();
            const auto layout = options.value("layout", std::string("global"));
            if (edge_index >= edges.size() || (endpoint != "source" && endpoint != "target") || (layout != "local" && layout != "global"))
                throw std::invalid_argument("Specify an existing edge, its source or target and local or global coordinates");
            const auto& edge = edges[edge_index];
            const auto& scans = session.point_clouds_container.point_clouds;
            const auto anchor = static_cast<std::size_t>(endpoint == "source" ? edge.index_to : edge.index_from);
            if (anchor >= scans.size())
                throw std::invalid_argument("The selected edge has an invalid endpoint");
            const auto reference = layout == "local" ? scans[anchor].m_pose.inverse() : Eigen::Affine3d::Identity();
            std::vector<Eigen::Vector3d> points;
            std::vector<unsigned short> ids;
            for (const auto index : selected)
            {
                const auto& scan = scans[index];
                const Eigen::Affine3d transform = reference * scan.m_pose;
                for (const auto& point : scan.points_local)
                {
                    points.push_back(transform * point);
                    ids.push_back(static_cast<unsigned short>(std::min<std::size_t>(index, 65535)));
                }
            }
            const std::vector<unsigned short> intensity(points.size(), endpoint == "source" ? 0U : 150U);
            return exportLaz(path, points, intensity, std::vector<double>(points.size(), 0.0), 0, 0, 0, &ids);
        }

        bool write_projected(
            const std::string& path,
            const SessionSnapshot& source,
            const std::vector<std::size_t>& selected,
            const json& options,
            bool track)
        {
            GNSS gnss;
            TUM tum;
            DecodeReferenceTracks(source.tracks, gnss, tum);
            const auto crs = options.at("crs").get<std::string>();
            const auto geoid = options.value("geoid", std::string{});
            const bool legacy = !track && crs == "PUWG92_legacy";
            if ((!legacy && !CRTs::SupportedCRTs.contains(crs)) ||
                (!geoid.empty() &&
                 [&]
                 {
                     const auto geoids = GNSS::get_available_geoids();
                     return std::ranges::find(geoids, geoid) == geoids.end();
                 }()))
                throw std::invalid_argument("Specify an existing Step 2 CRS and an available geoid");
            if (gnss.gnss_poses.empty() || (!track && !gnss.projected_crs.empty()))
                throw std::invalid_argument("The selected projection requires a GNSS reference track and a topocentric metascan");

            std::vector<Eigen::Vector3d> points;
            std::vector<unsigned short> intensity, ids;
            std::vector<double> timestamps;
            if (track)
            {
                for (const auto& fix : gnss.gnss_poses)
                {
                    points.emplace_back(fix.lat, fix.lon, fix.h_wgs84);
                    intensity.push_back(static_cast<unsigned short>(std::clamp(fix.hdop, 0.0, 65535.0)));
                    timestamps.push_back(fix.timestamp);
                }
            }
            else
            {
                const auto& scans = source.session->point_clouds_container.point_clouds;
                for (const auto index : selected)
                {
                    const auto& scan = scans[index];
                    for (std::size_t point = 0; point < scan.points_local.size(); ++point)
                    {
                        points.push_back(scan.m_pose * scan.points_local[point]);
                        intensity.push_back(point < scan.intensities.size() ? scan.intensities[point] : 0);
                        timestamps.push_back(point < scan.timestamps.size() ? scan.timestamps[point] : 0.0);
                        ids.push_back(static_cast<unsigned short>(std::min<std::size_t>(index, 65535)));
                    }
                }
            }
            if (legacy)
            {
                double north{}, east{};
                wgs84_do_puwg92(gnss.WGS84ReferenceLatitude, gnss.WGS84ReferenceLongitude, &north, &east);
                return exportLaz(path, points, intensity, timestamps, east, north, gnss.gnss_poses.front().alt, &ids);
            }
            if (!track)
                points = gnss.unproject_using_proj(points);
            auto projected = gnss.CRTConvert(points, crs, geoid);
            if (projected.empty() || projected.size() != intensity.size() ||
                !std::ranges::all_of(
                    projected,
                    [](const auto& p)
                    {
                        return p.allFinite();
                    }))
                return false;
            const Eigen::Vector3d offset = projected.front();
            for (auto& point : projected)
                point -= offset;
            return exportLaz(path, projected, intensity, timestamps, offset.x(), offset.y(), offset.z(), &ids);
        }

        //! Write the selected export to P05 staging and publish the complete inventory together.
        class ExportSource final : public CaptureSource
        {
        public:
            ExportSource(const OperationRequest& request, SessionSnapshot& source, const std::vector<std::size_t>& selected)
                : request_(request)
                , source_(source)
                , selected_(selected)
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
                const auto suffix = format.substr(format.find_last_of('_') + 1);
                const auto entry = format == "intersection_csv"
                    ? fs::path("rms.csv")
                    : fs::path("result").replace_extension(format == "resso" ? "reg" : (format == "e57_poses" ? "e57" : suffix));
                const auto path = directory / entry;
                const auto text = sessionPathText(path);
                auto& session = *source_.session;
                auto& clouds = session.point_clouds_container;
                auto metadata = json{ { "format", format }, { "scan_indices", selected_ } };
                std::string error;
                bool written = false;
                if (format == "las" || format == "laz")
                {
                    const auto layout = options.value("layout", std::string("global"));
                    if (layout == "separate")
                    {
                        auto first = separate_clouds(directory, source_, selected_, format, context.stop);
                        if (first)
                            return SnapshotContents{ std::string(session_export_kind), std::move(*first), std::move(metadata), {} };
                    }
                    else if (layout == "global" || layout == "local")
                        written = save_all_to_las(session, text, layout == "local", options.value("skip_zero_timestamps", true));
                    else
                        return io_error("invalid_parameter", "Unknown LAS/LAZ layout");
                }
                else if (format == "e57")
                    written = write_e57(path, source_, selected_, error);
                else if (format == "e57_poses")
                {
                    const auto files = options.at("paths").get<std::vector<std::string>>();
                    const auto indices = options.at("e57_scan_indices").get<std::vector<int>>();
                    if (files.size() != 1 || indices.size() != selected_.size() || !sessionPathFromText(files.front()).is_absolute())
                        return io_error(
                            "invalid_parameter", "Specify one E57 file and its Data3D index for each selected scan in session order");
                    std::map<int, Eigen::Affine3d> poses;
                    for (std::size_t i = 0; i < indices.size(); ++i)
                    {
                        auto pose = clouds.point_clouds[selected_[i]].m_pose;
                        pose.translation() += clouds.offset;
                        if (indices[i] < 0 || !poses.emplace(indices[i], pose).second)
                            return io_error("invalid_parameter", "E57 Data3D indices must be unique and nonnegative");
                    }
                    written = mandeye::e57io::rewrite_e57_poses(files.front(), text, poses, error);
                }
                else if (format == "observations_json")
                    written = write_observations(path, source_.parameters.value("observations", json::array()), selected_);
                else if (format == "intersection_csv")
                    written = write_roi(directory, source_, selected_, context.stop);
                else if (format.starts_with("constraint_"))
                    written = write_constraint(text, session, selected_, options);
                else if (format.starts_with("projected_"))
                    written = write_projected(text, source_, selected_, options, false);
                else if (format == "resso")
                {
                    // Native RESSO writes scan names from file_name; use the pinned names instead of private cache filenames.
                    for (const auto index : selected_)
                        clouds.point_clouds[index].file_name = scan_file_name(source_, index);
                    written = clouds.save_poses(path, true, options.value("initial", false));
                }
                else if (format.starts_with("trajectory_"))
                {
                    const auto curve = options.value("curve_distance_m", 1.0f);
                    const auto straight = options.value("straight_distance_m", 0.05f);
                    const auto downsample = options.value("downsample", false);
                    if (!std::isfinite(curve) || !std::isfinite(straight) || curve <= 0 || straight <= 0)
                        return io_error("invalid_parameter", "Trajectory distances must be finite and positive");
                    if (suffix == "las" || suffix == "laz")
                        written = save_trajectories_to_laz(session, text, curve, straight, downsample);
                    else
                    {
                        const auto time = options.value("timestamps", std::string("lidar"));
                        const auto pose = options.value("pose", std::string("matrix"));
                        if ((time != "lidar" && time != "unix" && time != "both") || (pose != "matrix" && pose != "quaternion"))
                            return io_error("invalid_parameter", "Unknown trajectory timestamp or pose representation");
                        written = save_trajectories(
                            session,
                            text,
                            curve,
                            straight,
                            downsample,
                            time != "unix",
                            time != "lidar",
                            pose == "quaternion",
                            suffix == "dxf");
                    }
                }
                else if (format.starts_with("section_"))
                {
                    const auto plane = options.at("plane").get<std::string>();
                    const auto width = options.at("width_m").get<double>();
                    if ((plane != "xy" && plane != "xz" && plane != "yz") || !std::isfinite(width) || width < 0.001)
                        return io_error("invalid_parameter", "Specify xy, xz or yz and a slab half-width of at least 0.001 m");
                    written = save_intersection(session, text, plane == "xz", plane == "yz", plane == "xy", width, true);
                }
                else if (format.starts_with("gnss_"))
                {
                    GNSS gnss;
                    TUM tum;
                    DecodeReferenceTracks(source_.tracks, gnss, tum);
                    if (options.contains("crs"))
                        written = write_projected(text, source_, selected_, options, true);
                    else if (!gnss.gnss_poses.empty())
                        written = gnss.save_to_laz(text, clouds.offset.x(), clouds.offset.y(), clouds.offset.z());
                }
                else if (format == "scale_board_laz")
                {
                    const auto spacing = options.at("spacing_m").get<float>();
                    const auto side = options.value("side_m", -1.0f);
                    if (!(side == -1.0f && (spacing == 0.1f || spacing == 1.0f || spacing == 10.0f)) &&
                        !(side == 10000.0f && (spacing == 10.0f || spacing == 100.0f || spacing == 1000.0f)))
                        return io_error("invalid_parameter", "Use a scale board size and spacing offered by Step 2");
                    written = save_scale_board_to_laz(session, text, spacing, side);
                }
                if (!written)
                    return io_error("session_export_failed", error.empty() ? "Could not write the selected session export" : error);
                return SnapshotContents{ std::string(session_export_kind), entry, std::move(metadata), {} };
            }

        private:
            const OperationRequest& request_;
            SessionSnapshot& source_;
            const std::vector<std::size_t>& selected_;
        };
    } // namespace

    std::vector<OperationDescriptor> DescribeStep2IO()
    {
        const json number = { { "type", "number" }, { "exclusiveMinimum", 0 } };
        const json boolean = { { "type", "boolean" } };
        const auto empty = object_schema(json::object());
        const json selection = { { "oneOf",
                                   json::array(
                                       { object_schema(
                                             { { "scan_indices",
                                                 { { "type", "array" },
                                                   { "minItems", 1 },
                                                   { "uniqueItems", true },
                                                   { "items", { { "type", "integer" }, { "minimum", 0 } } } } } },
                                             { "scan_indices" }),
                                         object_schema({ { "visible", { { "const", true } } } }, { "visible" }) }) } };
        const json input_path = { { "type", "array" }, { "minItems", 1 }, { "maxItems", 1 }, { "items", { { "type", "string" } } } };
        const json artifact = { { "type", "object" } };
        return { { std::string(session_open_operation),
                   empty,
                   object_schema(
                       { { "format", choice(input_formats) },
                         { "paths", { { "type", "array" }, { "minItems", 1 }, { "items", { { "type", "string" } } } } },
                         { "decimate", boolean },
                         { "calculate_offset", boolean },
                         { "fill_in_session", boolean },
                         { "buckets_m", { { "type", "array" }, { "minItems", 3 }, { "maxItems", 3 }, { "items", number } } } },
                       { "format", "paths" }),
                   artifact,
                   OperationClass::Heavy },
                 { std::string(session_save_operation), empty, empty, artifact, OperationClass::Heavy },
                 { std::string(session_subsession_operation), selection, empty, artifact, OperationClass::Heavy },
                 { std::string(import_session_poses_operation),
                   selection,
                   object_schema({ { "paths", input_path }, { "mode", choice({ "initial", "current", "inverse" }) } }, { "paths" }),
                   artifact,
                   OperationClass::Heavy },
                 { std::string(import_session_observations_operation),
                   selection,
                   object_schema({ { "paths", input_path } }, { "paths" }),
                   artifact,
                   OperationClass::Heavy },
                 { std::string(export_session_operation),
                   selection,
                   object_schema(
                       { { "format", choice(export_formats) },
                         { "layout", choice({ "global", "local", "separate" }) },
                         { "skip_zero_timestamps", boolean },
                         { "initial", boolean },
                         { "downsample", boolean },
                         { "curve_distance_m", number },
                         { "straight_distance_m", number },
                         { "timestamps", choice({ "lidar", "unix", "both" }) },
                         { "pose", choice({ "matrix", "quaternion" }) },
                         { "plane", choice({ "xy", "xz", "yz" }) },
                         { "width_m", number },
                         { "spacing_m", number },
                         { "side_m", { { "enum", { -1, 10000 } } } },
                         { "edge_index", { { "type", "integer" }, { "minimum", 0 } } },
                         { "endpoint", choice({ "source", "target" }) },
                         { "crs", { { "type", "string" } } },
                         { "geoid", { { "type", "string" } } },
                         { "paths", { { "type", "array" }, { "minItems", 1 }, { "maxItems", 1 }, { "items", { { "type", "string" } } } } },
                         { "e57_scan_indices", { { "type", "array" }, { "items", { { "type", "integer" }, { "minimum", 0 } } } } } },
                       { "format" }),
                   artifact,
                   OperationClass::Heavy } };
    }

    bool IsStep2IOOperation(std::string_view operation)
    {
        return operation == session_open_operation || operation == session_save_operation || operation == session_subsession_operation ||
            operation == export_session_operation || operation == import_session_poses_operation ||
            operation == import_session_observations_operation;
    }

    std::variant<SnapshotRef, OperationError> CaptureStep2Open(const OperationRequest& request, RunContext& context)
    {
        SessionOpenSource source(request);
        return CaptureSnapshot(source, request.target.revision, context.output_directory, context);
    }

    RunResult ExecuteStep2IO(const OperationRequest& request, const SnapshotRef& snapshot, RunContext& context)
    {
        try
        {
            if (request.target != snapshot.target)
                return io_error("revision_conflict", "Session IO requires the selected snapshot revision");
            const auto format = request.parameters.value("format", std::string{});
            if (request.operation == export_session_operation && std::ranges::find(export_formats, format) == export_formats.end())
                return io_error("invalid_parameter", "Unknown export format");
            if ((request.operation == session_save_operation || request.operation == session_subsession_operation) &&
                request.parameters != json::object())
                return io_error("invalid_parameter", "Native session save takes no parameters");
            if ((request.operation == session_save_operation || request.operation == session_open_operation) &&
                request.scope != json::object())
                return io_error("invalid_scope", "This operation uses the entire session");

            auto restored = LoadSessionSnapshot(snapshot, context.stop);
            if (auto* failure = std::get_if<OperationError>(&restored))
                return std::move(*failure);
            auto& source = std::get<SessionSnapshot>(restored);

            const bool imports =
                request.operation == import_session_poses_operation || request.operation == import_session_observations_operation;
            source.parameters.erase("session_io_import");
            std::vector<std::size_t> selected;
            if (request.operation == session_subsession_operation || request.operation == export_session_operation || imports)
            {
                try
                {
                    selected = select_scans(source, request.scope);
                } catch (const std::exception& error)
                {
                    return io_error("invalid_scope", error.what());
                }
                if (request.operation == session_subsession_operation || format == "mjs")
                    keep_scans(source, selected);
            }
            if (imports)
                if (auto failure = import_session_data(request, source, selected))
                    return *failure;
            if (request.operation != session_open_operation)
                source.parameters.erase("session_open");
            if (request.operation != export_session_operation || format == "mjs")
                return PublishStep2Result(*source.session, request.target, std::move(source.parameters), source.tracks, context);

            auto& scans = source.session->point_clouds_container.point_clouds;
            for (std::size_t i = 0; i < scans.size(); ++i)
                scans[i].visible = std::ranges::binary_search(selected, i);
            ExportSource writer(request, source, selected);
            const auto stage = context.output_directory / ".hdmapping-result-stage-step2-export";
            const auto destination = context.output_directory / "export";
            auto capture = CaptureSnapshot(writer, request.target.revision, stage, context);
            if (auto* failure = std::get_if<OperationError>(&capture))
                return std::move(*failure);
            if (context.stop.stop_requested())
            {
                DiscardResultStaging(stage, destination);
                return io_error("cancelled", "Session export cancelled");
            }
            return PublishResult(stage, destination);
        } catch (const std::exception& error)
        {
            return io_error("session_io_failed", error.what());
        }
    }
} // namespace HDMapping::Processing
