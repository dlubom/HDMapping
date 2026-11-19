#pragma once

#include "multi_view_tls_registration.h"

#include <HDMapping/Processing/Operation.h>

//! Describe the registration settings used by the Step 2 controls and solver.
nlohmann::json describeStep2Parameters(const TLSRegistration& registration);

//! Validate the complete patch before assigning any solver settings.
std::optional<HDMapping::Processing::OperationError> updateStep2Parameters(TLSRegistration& registration, const nlohmann::json& patch);
