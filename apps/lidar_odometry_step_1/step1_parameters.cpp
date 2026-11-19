#include "step1_parameters.h"

#include "lidar_odometry.h"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>

using json = nlohmann::json;
using HDMapping::Processing::OperationError;

namespace
{
    constexpr std::array grid_members{ std::pair{ "resolution_X", &NDT::GridParameters::resolution_X },
                                       std::pair{ "resolution_Y", &NDT::GridParameters::resolution_Y },
                                       std::pair{ "resolution_Z", &NDT::GridParameters::resolution_Z },
                                       std::pair{ "bounding_box_extension", &NDT::GridParameters::bounding_box_extension } };

    constexpr std::array grids{ std::pair{ "in_out_params_indoor", &LidarOdometryParams::in_out_params_indoor },
                                std::pair{ "in_out_params_outdoor", &LidarOdometryParams::in_out_params_outdoor } };

    bool is_configuration(std::string_view name)
    {
        return name != "software_version" && name != "config_version" && name != "build_date" && name != "current_output_dir" &&
            name != "working_directory_preview";
    }

    bool is_metadata(std::string_view name)
    {
        return name == "apply_to_gui" || name == "prepared" || name == "processed" || name == "recording_directory" || name == "range" ||
            name == "selected_chunks" || name == "initial_transform";
    }

    template<class T>
    T read_value(const json& value, const std::string& name)
    {
        bool valid = false;
        if constexpr (std::is_same_v<T, bool>)
            valid = value.is_boolean();
        else if constexpr (std::is_same_v<T, int>)
            valid = value.is_number_integer() && value >= std::numeric_limits<int>::min() && value <= std::numeric_limits<int>::max();
        else if constexpr (std::is_same_v<T, double>)
            valid = value.is_number() && std::isfinite(value.get<double>());
        else
            valid = value.is_string() && value.get_ref<const std::string&>().find('\0') == std::string::npos;
        if (!valid)
            throw std::invalid_argument("Invalid type or value for Step 1 parameter " + name);
        return value.get<T>();
    }

    void apply_fields(const json& settings, LidarOdometryParams& params)
    {
        for (const auto& [name, value] : settings.items())
        {
            const auto scalar = TomlIO::POINTERS.find(name);
            const auto motion = TomlIO::MOTION_MODEL_CORRECTION_POINTERS.find(name);
            const auto grid = std::ranges::find_if(
                grids,
                [&](const auto& entry)
                {
                    return name == entry.first;
                });
            if (scalar != TomlIO::POINTERS.end() && is_configuration(name))
            {
                std::visit(
                    [&](auto pointer)
                    {
                        using ValueType = std::remove_cvref_t<decltype(params.*pointer)>;
                        params.*pointer = read_value<ValueType>(value, name);
                    },
                    scalar->second);
            }
            else if (motion != TomlIO::MOTION_MODEL_CORRECTION_POINTERS.end())
                params.motion_model_correction.*(motion->second) = read_value<double>(value, name);
            else if (grid != grids.end() && value.is_object())
            {
                for (const auto& [key, field] : value.items())
                {
                    const auto member = std::ranges::find_if(
                        grid_members,
                        [&](const auto& entry)
                        {
                            return key == entry.first;
                        });
                    if (member == grid_members.end())
                        throw std::invalid_argument("Unknown Step 1 grid parameter " + name + "." + key);
                    (params.*(grid->second)).*(member->second) = read_value<double>(field, name + "." + key);
                }
            }
            else if (!is_metadata(name))
                throw std::invalid_argument("Unknown or invalid Step 1 parameter " + name);
        }
    }

    void require(bool valid, std::string_view name)
    {
        if (!valid)
            throw std::invalid_argument("Step 1 parameter outside its valid range: " + std::string(name));
    }

    void validate(const LidarOdometryParams& p)
    {
        require(p.threshold_nr_poses > 0, "threshold_nr_poses");
        require(p.threshold_initial_points >= 0, "threshold_initial_points");
        require(p.nr_iter > 0, "nr_iter");
        require(p.num_constistency_iter >= 0, "num_constistency_iter");
        require(p.calibration_validation_points >= 0, "calibration_validation_points");
        require(p.min_counter_concatenated_trajectory_nodes >= 0, "min_counter_concatenated_trajectory_nodes");
        require(p.robust_and_accurate_lidar_odometry_iterations >= 0, "robust_and_accurate_lidar_odometry_iterations");
        require(p.robust_and_accurate_lidar_odometry_rigid_sf_iterations >= 0, "robust_and_accurate_lidar_odometry_rigid_sf_iterations");
        require(p.imu_preintegration_method >= 0 && p.imu_preintegration_method <= 7, "imu_preintegration_method");
        require(p.filter_threshold_xy_inner >= 0, "filter_threshold_xy_inner");
        require(p.filter_threshold_xy_outer > p.filter_threshold_xy_inner, "filter_threshold_xy_outer");
        require(p.max_distance_lidar > 0, "max_distance_lidar");
        require(p.max_distance_lidar_rigid_sf > 0, "max_distance_lidar_rigid_sf");
        require(p.real_time_threshold_seconds >= 0, "real_time_threshold_seconds");
        require(p.vqf_restFilterTau > 0, "vqf_restFilterTau");
        require(p.vqf_biasForgettingTime > 0, "vqf_biasForgettingTime");
        require(p.vqf_magRefTau > 0, "vqf_magRefTau");
        require(p.vqf_magRejectionFactor > 0, "vqf_magRejectionFactor");
        require(p.distance_bucket > 0 && p.polar_angle_deg > 0 && p.azimutal_angle_deg > 0, "spherical grid resolution");
        require(
            p.distance_bucket_rigid_sf > 0 && p.polar_angle_deg_rigid_sf > 0 && p.azimutal_angle_deg_rigid_sf > 0,
            "rigid spherical grid resolution");
        for (const auto& [key, member] : grids)
        {
            const auto& grid = p.*member;
            require(grid.resolution_X > 0 && grid.resolution_Y > 0 && grid.resolution_Z > 0, key);
        }
        for (const auto& [name, member] : TomlIO::POINTERS)
        {
            if (name.find("_sigma_") == std::string::npos)
                continue;
            require(p.*std::get<double LidarOdometryParams::*>(member) > 0, name);
        }
    }

    json schema(const json& value)
    {
        if (value.is_object())
        {
            auto properties = json::object();
            for (const auto& [key, field] : value.items())
                properties[key] = schema(field);
            return { { "type", "object" }, { "properties", std::move(properties) }, { "additionalProperties", false } };
        }
        const char* type = value.is_boolean() ? "boolean" : value.is_number_integer() ? "integer" : value.is_number() ? "number" : "string";
        return { { "type", type }, { "default", value } };
    }
} // namespace

json Step1Parameters(const LidarOdometryParams& params)
{
    auto fields = json::object();
    for (const auto& [name, member] : TomlIO::POINTERS)
    {
        if (is_configuration(name))
            std::visit(
                [&](auto pointer)
                {
                    fields[name] = params.*pointer;
                },
                member);
    }
    for (const auto& [name, member] : TomlIO::MOTION_MODEL_CORRECTION_POINTERS)
        fields[name] = params.motion_model_correction.*member;
    for (const auto& [name, member] : grids)
    {
        for (const auto& [key, field] : grid_members)
            fields[name][key] = (params.*member).*field;
    }
    return fields;
}

std::optional<OperationError> ApplyStep1Parameters(const json& values, LidarOdometryParams& params)
{
    try
    {
        if (!values.is_object())
            throw std::invalid_argument("Step 1 parameters must be an object");
        auto merged = Step1Parameters(params);
        merged.update(values, true);
        LidarOdometryParams candidate{};
        apply_fields(merged, candidate);
        validate(candidate);
        apply_fields(values, params);
        return std::nullopt;
    } catch (const std::exception& error)
    {
        return OperationError{ "invalid_parameters", error.what(), json::object() };
    }
}

json DescribeStep1Parameters()
{
    LidarOdometryParams defaults{};
    set_lidar_odometry_default_params(defaults);
    return schema(Step1Parameters(defaults));
}
