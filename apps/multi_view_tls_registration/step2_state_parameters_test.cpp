#include "step2_solver_selection.h"
#include "step2_state_parameters.h"

#include <doctest.h>

TEST_CASE("Step 2 radio controls and incoming solver parameters share one engine state")
{
    TLSRegistration model{};
    REQUIRE_FALSE(updateStep2Parameters(
        model,
        { { "ndt", { { "optimizer", "levenberg_marquardt" }, { "rotation", "rodrigues" }, { "number_of_iterations", 17 } } },
          { "icp", { { "pose_convention", "cw" }, { "rotation", "quaternion" }, { "search_radius", 0.75 } } } }));
    // Two panel frames read the engine, then submit their current radio choices.
    for (int frame = 0; frame < 2; ++frame)
    {
        auto ndt = Step2SolverSelection::read(model.ndt);
        auto icp = Step2SolverSelection::read(model.icp);
        CHECK(ndt.optimizer == 1);
        CHECK(ndt.rotation == 2);
        CHECK(icp.pose_convention == 0);
        CHECK(icp.rotation == 1);
        ndt.write(model.ndt);
        icp.write(model.icp);
    }
    CHECK(model.ndt.number_of_iterations == 17);
    CHECK(model.icp.search_radius == 0.75f);
    Step2SolverSelection{ 0, 1, 0 }.write(model.icp);
    const auto current = describeStep2Parameters(model);
    CHECK(current.at("icp").at("pose_convention") == "wc");
    CHECK(current.at("icp").at("rotation") == "tait_bryan");
    CHECK_FALSE(model.icp.is_quaternion);
    CHECK_FALSE(model.icp.is_rodrigues);

    REQUIRE(updateStep2Parameters(model, { { "icp", { { "search_radius", 3 } } }, { "ndt", { { "number_of_iterations", 9 } } } }));
    CHECK(describeStep2Parameters(model) == current);
    REQUIRE(updateStep2Parameters(model, { { "ndt", { { "bucket_size", { 0.5, 0.5 } } } } }));
    REQUIRE(updateStep2Parameters(model, { { "icp", { { "unrecognized", true } } } }));
}
