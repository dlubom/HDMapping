#include "step2_georeference_operations.h"

#include <Core/reference_alignment.h>
#include <Core/reference_tracks.h>
#include <Core/session.h>
#include <HDMapping/SessionPaths.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace HDMapping::Processing
{
    using nlohmann::json;

    namespace
    {
        constexpr std::string_view align = "step2.align_reference";
        constexpr std::string_view importGnss = "gnss.import";
        constexpr std::string_view projectGnss = "gnss.project";
        constexpr std::string_view importTum = "tum.import";
        constexpr std::array pointEdits{ "cp.import", "cp.update", "cp.remove", "gcp.import", "gcp.update", "gcp.remove" };

        OperationError error(std::string code, std::string message)
        {
            return { std::move(code), std::move(message), json::object() };
        }

        json object(json properties = json::object(), json required = json::array())
        {
            return { { "type", "object" },
                     { "properties", std::move(properties) },
                     { "required", std::move(required) },
                     { "additionalProperties", false } };
        }

        json array(const json& item, int minItems)
        {
            return { { "type", "array" }, { "items", item }, { "minItems", minItems } };
        }

        json choice(json values)
        {
            return { { "type", "string" }, { "enum", std::move(values) } };
        }

        void checkKeys(const json& value, std::initializer_list<std::string_view> keys)
        {
            if (!value.is_object())
                throw std::invalid_argument("Expected an object");
            for (const auto& item : value.items())
                if (std::ranges::find(keys, item.key()) == keys.end())
                    throw std::invalid_argument("Unknown field: " + item.key());
        }

        template<std::size_t N>
        std::string pointName(const char (&name)[N])
        {
            return { name, std::find(name, name + N, '\0') };
        }

        template<std::size_t N>
        void setName(char (&storage)[N], const json& point)
        {
            const auto& name = point.at("name").get_ref<const std::string&>();
            if (name.size() >= N || name.find('\0') != std::string::npos)
                throw std::invalid_argument("Point name must fit within 63 UTF-8 bytes and contain no NUL");
            std::ranges::fill(storage, '\0');
            std::ranges::copy(name, storage);
        }

        int index(const json& v)
        {
            if (!v.is_number_integer() || v.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
                throw std::invalid_argument("Point associations require zero-based integer indices");
            return v.get<int>();
        }

        double number(const json& value)
        {
            if (!value.is_number() || !std::isfinite(value.get<double>()))
                throw std::invalid_argument("Expected a finite number");
            return value.get<double>();
        }

        std::array<double, 3> coordinates(const json& value)
        {
            if (!value.is_array() || value.size() != 3)
                throw std::invalid_argument("Expected three metric coordinates or sigmas");
            return { number(value[0]), number(value[1]), number(value[2]) };
        }

        json encode(const ControlPoint& point)
        {
            return { { "name", pointName(point.name) },
                     { "scan_index", point.index_to_pose },
                     { "local_m", { point.x_source_local, point.y_source_local, point.z_source_local } },
                     { "target_m", { point.x_target_global, point.y_target_global, point.z_target_global } },
                     { "sigma_m", { point.sigma_x, point.sigma_y, point.sigma_z } },
                     { "is_z_0", point.is_z_0 } };
        }

        json encode(const GroundControlPoint& point)
        {
            return { { "name", pointName(point.name) },
                     { "scan_index", point.index_to_node_inner },
                     { "trajectory_index", point.index_to_node_outer },
                     { "target_m", { point.x, point.y, point.z } },
                     { "sigma_m", { point.sigma_x, point.sigma_y, point.sigma_z } },
                     { "lidar_height_m", point.lidar_height_above_ground } };
        }

        void decode(const PointClouds& clouds, const json& value, ControlPoint& point)
        {
            checkKeys(value, { "name", "scan_index", "local_m", "target_m", "sigma_m", "is_z_0" });
            setName(point.name, value);
            point.index_to_pose = index(value.at("scan_index"));
            const auto [lx, ly, lz] = coordinates(value.at("local_m"));
            const auto [x, y, z] = coordinates(value.at("target_m"));
            const auto [sx, sy, sz] = coordinates(value.at("sigma_m"));
            point.x_source_local = lx;
            point.y_source_local = ly;
            point.z_source_local = lz;
            point.x_target_global = x;
            point.y_target_global = y;
            point.z_target_global = z;
            point.sigma_x = sx;
            point.sigma_y = sy;
            point.sigma_z = sz;
            point.is_z_0 = value.value("is_z_0", false);
            if (const auto* message = CheckControlPoint(clouds, point))
                throw std::invalid_argument(message);
        }

        void decode(const PointClouds& clouds, const json& value, GroundControlPoint& point)
        {
            checkKeys(value, { "name", "scan_index", "trajectory_index", "target_m", "sigma_m", "lidar_height_m" });
            setName(point.name, value);
            point.index_to_node_inner = index(value.at("scan_index"));
            point.index_to_node_outer = index(value.at("trajectory_index"));
            const auto [x, y, z] = coordinates(value.at("target_m"));
            const auto [sx, sy, sz] = coordinates(value.at("sigma_m"));
            point.x = x;
            point.y = y;
            point.z = z;
            point.sigma_x = sx;
            point.sigma_y = sy;
            point.sigma_z = sz;
            point.lidar_height_above_ground = number(value.at("lidar_height_m"));
            if (const auto* message = CheckGroundControlPoint(clouds, point))
                throw std::invalid_argument(message);
        }

        template<typename Point>
        json encodePoints(const std::vector<Point>& points)
        {
            auto result = json::array();
            for (const auto& point : points)
                result.push_back(encode(point));
            return result;
        }

        template<typename Point>
        void edit(const PointClouds& clouds, const OperationRequest& request, std::vector<Point>& points)
        {
            if (request.operation.ends_with(".import"))
            {
                checkKeys(request.scope, {});
                checkKeys(request.parameters, { "points" });
                const auto& entries = request.parameters.at("points");
                if (!entries.is_array() || entries.empty())
                    throw std::invalid_argument("Supply a nonempty points array");
                std::vector<Point> added;
                added.reserve(entries.size());
                for (const auto& entry : entries)
                    decode(clouds, entry, added.emplace_back());
                points.insert(points.end(), added.begin(), added.end());
                return;
            }

            checkKeys(request.scope, { "point_index" });
            const auto selected = index(request.scope.at("point_index"));
            if (static_cast<std::size_t>(selected) >= points.size())
                throw std::invalid_argument("Point index is outside the requested revision");
            if (request.operation.ends_with(".remove"))
            {
                checkKeys(request.parameters, {});
                points.erase(points.begin() + selected);
                return;
            }

            checkKeys(request.parameters, { "point" });
            const auto& patch = request.parameters.at("point");
            if (!patch.is_object() || patch.empty())
                throw std::invalid_argument("Supply one or more point fields to update");
            auto value = encode(points[selected]);
            value.update(patch);
            Point updated{};
            decode(clouds, value, updated);
            points[selected] = updated;
        }

        std::vector<std::string> paths(const json& parameters)
        {
            const auto& input = parameters.at("paths");
            if (!input.is_array() || input.empty())
                throw std::invalid_argument("Supply a nonempty paths array");
            auto result = input.get<std::vector<std::string>>();
            for (const auto& path : result)
            {
                if (path.find('\0') != std::string::npos || !sessionPathFromText(path).is_absolute())
                    throw std::invalid_argument("Trajectory import requires absolute local file paths");
            }
            return result;
        }

        std::optional<OperationError> loadGnss(PointClouds& clouds, const json& parameters, GNSS& gnss)
        {
            checkKeys(parameters, { "paths", "format", "projection", "localize", "set_from_first_pose", "latitude_deg", "longitude_deg" });
            const auto files = paths(parameters);
            const auto format = parameters.at("format").get<std::string>();
            const auto projection = parameters.at("projection").get<std::string>();
            if (format != "gnss" && format != "nmea")
                return error("invalid_parameters", "GNSS format must be gnss or nmea");
            if (projection != "puwg92" && projection != "mercator" && projection != "topocentric")
                return error("unsupported_crs", "GNSS import projection must be topocentric, mercator or puwg92");
            if (projection == "puwg92")
            {
                if (format != "gnss")
                    return error("invalid_parameters", "Legacy PUWG92 import requires .gnss files");
                Eigen::Vector3d offset;
                if (!gnss.load_data_from_gnss_and_convert_to_92(files, offset, parameters.value("localize", false)))
                    return error("reference_import_failed", "Could not load valid PUWG92 positions");
                clouds.offset_to_apply = offset;
            }
            else
            {
                gnss.setWGS84ReferenceFromFirstPose = parameters.value("set_from_first_pose", gnss.setWGS84ReferenceFromFirstPose);
                if (parameters.contains("latitude_deg"))
                    gnss.WGS84ReferenceLatitude = number(parameters.at("latitude_deg"));
                if (parameters.contains("longitude_deg"))
                    gnss.WGS84ReferenceLongitude = number(parameters.at("longitude_deg"));
                if (std::abs(gnss.WGS84ReferenceLatitude) > 90 || std::abs(gnss.WGS84ReferenceLongitude) > 180)
                    return error("invalid_parameters", "WGS84 origin latitude or longitude is outside its valid range");
                const bool loaded = format == "nmea" ? gnss.load_raw_data_from_nmea(files) : gnss.load_raw_data_from_gnss(files);
                if (!loaded || gnss.gnss_poses.empty() ||
                    !(projection == "topocentric" ? gnss.project_using_proj() : gnss.project_to_mercator_projection()))
                    return error("reference_import_failed", "Could not load and project valid GNSS positions");
            }
            if (!std::ranges::all_of(
                    gnss.gnss_poses,
                    [](const auto& p)
                    {
                        return Eigen::Vector3d(p.enu_x, p.enu_y, p.enu_z).allFinite();
                    }))
                return error("projection_failed", "GNSS projection produced a nonfinite coordinate");
            return {};
        }

        std::optional<OperationError> convertGnss(const json& parameters, GNSS& gnss)
        {
            checkKeys(parameters, { "crs", "geoid" });
            const auto crs = parameters.at("crs").get<std::string>();
            if (!CRTs::SupportedCRTs.contains(crs))
                return error("unsupported_crs", "Supported projected systems are PUWG92, UTM_AUTO and WebMercator");
            const auto geoid = parameters.value("geoid", std::string{});
            if (!geoid.empty())
            {
                const auto models = GNSS::get_available_geoids();
                if (std::ranges::find(models, geoid) == models.end())
                    return error("missing_geoid", "The requested geoid must be available in the worker installation");
            }
            if (gnss.gnss_poses.empty())
                return error("missing_reference", "Import GNSS before projecting its WGS84 positions");

            std::vector<Eigen::Vector3d> lla;
            lla.reserve(gnss.gnss_poses.size());
            for (const auto& pose : gnss.gnss_poses)
            {
                if (!std::isfinite(pose.lat) || !std::isfinite(pose.lon) || !std::isfinite(pose.h_wgs84) || std::abs(pose.lat) > 90 ||
                    std::abs(pose.lon) > 180)
                    return error("invalid_reference", "GNSS projection requires finite WGS84 latitude, longitude and height");
                lla.emplace_back(pose.lat, pose.lon, pose.h_wgs84);
            }
            const auto projected = gnss.CRTConvert(lla, crs, geoid);
            if (projected.size() != lla.size() ||
                !std::ranges::all_of(
                    projected,
                    [](const auto& p)
                    {
                        return p.allFinite();
                    }))
                return error("projection_failed", "PROJ could not transform the entire GNSS track using the selected CRS and datum");
            for (std::size_t i = 0; i < projected.size(); ++i)
            {
                auto& pose = gnss.gnss_poses[i];
                pose.enu_x = projected[i].x();
                pose.enu_y = projected[i].y();
                pose.enu_z = projected[i].z();
            }
            gnss.projected_crs = crs;
            gnss.projected_vertical_datum = geoid.empty() ? "WGS84Ellipsoid" : sessionPathText(sessionPathFromText(geoid).filename());
            return {};
        }
    } // namespace

    std::vector<OperationDescriptor> DescribeStep2Georeference()
    {
        const json integer{ { "type", "integer" }, { "minimum", 0 } };
        const json text{ { "type", "string" } };
        const json scalar{ { "type", "number" } };
        const json flag{ { "type", "boolean" } };
        const json vector{
            { "type", "array" }, { "items", scalar }, { "minItems", 3 }, { "maxItems", 3 }, { "description", "Ordered X, Y, Z in metres" }
        };
        const json cp{ { "name", text },       { "scan_index", integer }, { "local_m", vector },
                       { "target_m", vector }, { "sigma_m", vector },     { "is_z_0", flag } };
        const json gcp{ { "name", text },       { "scan_index", integer }, { "trajectory_index", integer },
                        { "target_m", vector }, { "sigma_m", vector },     { "lidar_height_m", scalar } };
        const auto scope = object({ { "point_index", integer } }, { "point_index" });
        const json result{ { "type", "object" }, { "description", "Published native Step 2 session and its target revision" } };
        std::vector<OperationDescriptor> operations;
        for (const auto* id : pointEdits)
        {
            const std::string_view name(id);
            const bool isCp = name.starts_with("cp.");
            const auto& fields = isCp ? cp : gcp;
            const json required = isCp ? json::array({ "name", "scan_index", "local_m", "target_m", "sigma_m" })
                                       : json::array({ "name", "scan_index", "trajectory_index", "target_m", "sigma_m", "lidar_height_m" });
            if (name.ends_with(".import"))
                operations.push_back(
                    { id,
                      object(),
                      object({ { "points", array(object(fields, required), 1) } }, { "points" }),
                      result,
                      OperationClass::Mutate });
            else if (name.ends_with(".update"))
                operations.push_back({ id, scope, object({ { "point", object(fields) } }, { "point" }), result, OperationClass::Mutate });
            else
                operations.push_back({ id, scope, object(), result, OperationClass::Mutate });
        }
        operations.push_back(
            { std::string(align),
              object(),
              object({ { "source", choice({ "cp", "gcp", "gnss", "tum" }) } }, { "source" }),
              result,
              OperationClass::Heavy });
        operations.push_back(
            { std::string(importGnss),
              object(),
              object(
                  { { "paths", array(text, 1) },
                    { "format", choice({ "gnss", "nmea" }) },
                    { "projection", choice({ "puwg92", "mercator", "topocentric" }) },
                    { "localize", flag },
                    { "set_from_first_pose", flag },
                    { "latitude_deg", scalar },
                    { "longitude_deg", scalar } },
                  { "paths", "format", "projection" }),
              result,
              OperationClass::Heavy });
        operations.push_back(
            { std::string(projectGnss),
              object(),
              object({ { "crs", choice(CRTs::SupportedCRTs) }, { "geoid", text } }, { "crs" }),
              result,
              OperationClass::Heavy });
        operations.push_back(
            { std::string(importTum),
              object(),
              object({ { "paths", array(text, 1) }, { "subtract_first_pose", flag } }, { "paths" }),
              result,
              OperationClass::Heavy });
        return operations;
    }

    bool IsStep2GeoreferenceOperation(std::string_view operation)
    {
        return operation == align || operation == importGnss || operation == projectGnss || operation == importTum ||
            std::ranges::find(pointEdits, operation) != pointEdits.end();
    }

    json DescribeControlPoints(const Session& session)
    {
        return encodePoints(session.control_points.cps);
    }

    json DescribeGroundControlPoints(const Session& session)
    {
        return encodePoints(session.ground_control_points.gpcs);
    }

    std::optional<OperationError> EditStep2Georeference(Session& session, const OperationRequest& request)
    {
        if (std::ranges::find(pointEdits, request.operation) == pointEdits.end())
            return error("unknown_operation", "Expected a short CP or GCP edit");
        try
        {
            if (request.operation.starts_with("cp."))
                edit(session.point_clouds_container, request, session.control_points.cps);
            else
                edit(session.point_clouds_container, request, session.ground_control_points.gpcs);
            return {};
        } catch (const std::exception& exception)
        {
            return error("invalid_control_point", exception.what());
        }
    }

    std::optional<OperationError> RunStep2Georeference(Session& session, const OperationRequest& request, json& tracks, RunContext& context)
    {
        const auto proceed = [&context]
        {
            return !context.stop.stop_requested();
        };
        if (!proceed())
            return error("cancelled", "Step 2 reference operation cancelled");
        if (std::ranges::find(pointEdits, request.operation) != pointEdits.end())
            return EditStep2Georeference(session, request);
        try
        {
            checkKeys(request.scope, {});
            auto& clouds = session.point_clouds_container;
            GNSS gnss;
            TUM tum;
            if (request.operation != align || request.parameters.value("source", std::string{}) == "gnss" ||
                request.parameters.value("source", std::string{}) == "tum")
                DecodeReferenceTracks(tracks, gnss, tum);
            if (request.operation == align)
            {
                checkKeys(request.parameters, { "source" });
                const auto source = request.parameters.at("source").get<std::string>();
                if (context.progress)
                    context.progress({ "align_reference", {}, {}, "Compute rigid reference alignment" });
                bool aligned{};
                if (source == "cp")
                    aligned = AlignControlPoints(clouds, session.control_points.cps, proceed);
                else if (source == "gcp")
                    aligned = AlignGroundControlPoints(clouds, session.ground_control_points.gpcs, proceed);
                else if (source == "gnss")
                    aligned = AlignGnss(clouds, gnss, proceed);
                else if (source == "tum")
                    aligned = AlignTum(clouds, tum, proceed);
                else
                    return error("invalid_parameters", "Reference source must be cp, gcp, gnss or tum");
                if (!proceed())
                    return error("cancelled", "Step 2 reference alignment cancelled");
                if (!aligned)
                    return error("alignment_failed", "Reference observations are invalid, insufficient or do not constrain a rigid fit");
                return {};
            }
            if (context.progress)
                context.progress({ "load_reference", {}, {}, "Load or project reference positions" });
            if (request.operation == importGnss)
            {
                if (auto failure = loadGnss(clouds, request.parameters, gnss))
                    return failure;
            }
            else if (request.operation == projectGnss)
            {
                if (auto failure = convertGnss(request.parameters, gnss))
                    return failure;
            }
            else if (request.operation == importTum)
            {
                checkKeys(request.parameters, { "paths", "subtract_first_pose" });
                if (!tum.load_data_from_tum(paths(request.parameters), request.parameters.value("subtract_first_pose", false)) ||
                    tum.tum_poses.empty())
                    return error("reference_import_failed", "Could not load valid TUM poses");
            }
            else
                return error("unknown_operation", "Unknown Step 2 reference operation");
            if (!proceed())
                return error("cancelled", "Step 2 reference operation cancelled");
            tracks.update(EncodeReferenceTracks(gnss, tum));
            return {};
        } catch (const std::exception& exception)
        {
            return error("reference_operation_failed", exception.what());
        }
    }
} // namespace HDMapping::Processing
