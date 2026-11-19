#pragma once

#include <HDMapping/Processing/Operation.h>

struct LidarOdometryParams;

//! Serialize the configurable Step 1 fields, with their existing TOML names.
nlohmann::json Step1Parameters(const LidarOdometryParams& params);

//! Validate and apply a partial set of fields without changing the parameters on failure.
std::optional<HDMapping::Processing::OperationError> ApplyStep1Parameters(const nlohmann::json& values, LidarOdometryParams& params);

//! Describe the shared solver parameter schema using the CLI preset as defaults.
nlohmann::json DescribeStep1Parameters();
