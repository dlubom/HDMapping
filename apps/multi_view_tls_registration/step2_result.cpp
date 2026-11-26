#include "step2_result.h"

#include "NativeSessionResult.h"

#include <HDMapping/Processing/ResultPublisher.h>
#include <HDMapping/Processing/SnapshotStore.h>

#include <Core/session.h>

namespace HDMapping::Processing
{
    namespace fs = std::filesystem;
    using nlohmann::json;

    namespace
    {
        OperationError cancelled()
        {
            return { "cancelled", "Step 2 registration cancelled", {} };
        }

        //! Materialize worker-owned point clouds and poses in the existing MJS/LAZ formats.
        class RegistrationResultSource final : public CaptureSource
        {
        public:
            RegistrationResultSource(Session& session, const TargetRef& target, json parameters, const json& tracks)
                : session_(session)
                , target_(target)
                , parameters_(std::move(parameters))
                , tracks_(tracks)
            {
            }

            TargetRef Target() const override
            {
                return target_;
            }

            CaptureResult Materialize(const fs::path& directory, RunContext& context) override
            {
                return WriteNativeSession(session_, directory, std::move(parameters_), tracks_, context);
            }

        private:
            Session& session_;
            TargetRef target_;
            json parameters_;
            const json& tracks_;
        };
    } // namespace

    RunResult PublishStep2Result(Session& session, const TargetRef& target, json parameters, const json& tracks, RunContext& context)
    {
        const auto stage = context.output_directory / ".hdmapping-result-stage-step2";
        const auto destination = context.output_directory / "native";
        RegistrationResultSource source(session, target, std::move(parameters), tracks);
        const auto captured = CaptureSnapshot(source, target.revision, stage, context);
        if (const auto* failure = std::get_if<OperationError>(&captured))
            return *failure;
        if (context.stop.stop_requested())
        {
            DiscardResultStaging(stage, destination);
            return cancelled();
        }
        return PublishResult(stage, destination);
    }
} // namespace HDMapping::Processing
