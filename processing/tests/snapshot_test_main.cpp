#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest.h>

#include <HDMapping/Processing/SessionSnapshot.h>

#include <Core/session.h>

#include <iomanip>
#include <iostream>
#include <string_view>

int main(int argc, char** argv)
{
    using namespace HDMapping::Processing;
    if (argc == 3 && std::string_view(argv[1]) == "--read-gui-snapshot")
    {
        const SnapshotRef ref{ std::filesystem::canonical(argv[2]) / "snapshot.json", { "step2-probe", 1 } };
        const auto result = LoadSessionSnapshot(ref);
        if (const auto* error = std::get_if<OperationError>(&result))
        {
            std::cerr << error->code << ": " << error->message << '\n';
            return 1;
        }
        const auto& snapshot = std::get<SessionSnapshot>(result);
        const auto& clouds = snapshot.session->point_clouds_container.point_clouds;
        std::size_t points = 0;
        for (const auto& cloud : clouds)
        {
            points += cloud.points_local.size();
        }
        std::cout << std::setprecision(17) << clouds.size() << " scans, " << points << " points, first pose X "
                  << clouds.front().m_pose.translation().x() << ", parameters " << snapshot.parameters.dump() << '\n';
        return 0;
    }
    return doctest::Context(argc, argv).run();
}
