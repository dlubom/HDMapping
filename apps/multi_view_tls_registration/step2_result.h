#pragma once

#include <HDMapping/Processing/Operation.h>

class Session;

namespace HDMapping::Processing
{
    //! Publish a complete native session from the worker-owned registration state.
    RunResult PublishStep2Result(
        Session& session, const TargetRef& target, nlohmann::json parameters, const nlohmann::json& tracks, RunContext& context);
} // namespace HDMapping::Processing
