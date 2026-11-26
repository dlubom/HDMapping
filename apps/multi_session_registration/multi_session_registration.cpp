#include <array>
#include <cmath>
#include <filesystem>
#include <map>

#include <imgui.h>
#include <imgui_internal.h>
#include <rlImGui.h>

#include <ImGuizmo.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <Eigen/Eigen>

#include <Core/export_laz.h>
#include <Core/icp.h>
#include <Core/ndt.h>
#include <Core/observation_picking.h>
#include <Core/pfd_wrapper.hpp>
#include <Core/registration_plane_feature.h>
#include <Core/session.h>

#ifdef _WIN32
// windows.h (pulled in by portable-file-dialogs.h) declares CloseWindow/ShowCursor like raylib.h does.
#define CloseWindow CloseWindow_win32
#define ShowCursor ShowCursor_win32
#endif
#include <portable-file-dialogs.h>
#ifdef _WIN32
#undef CloseWindow
#undef ShowCursor
#endif

#include <HDMapping/Assert.h>
#include <HDMapping/HDMAPPING_ConfigureInfo.hpp>

#ifdef _WIN32
#include "resource.h"

#endif

#ifdef HDMAPPING_GUI_BRIDGE
#include "project_capture.h"
#include "project_operations.h"
#include <HDMapping/Processing/GuiStateBridge.h>
#include <HDMapping/Processing/GuiStateConnection.h>
#endif
#include "raylib_utils.h"
#include "step3_export_formats.h"
#include "step3_operations.h"
#ifdef HDMAPPING_GUI_BRIDGE
#include "step3_io_operations.h"
#endif
#include <HDMapping/SessionPaths.h>

std::string winTitle = std::string("Step 3 (Multi session registration) ") + HDMAPPING_CONFIGURE_PROJECT_VERSION;

std::vector<std::string> infoLines = {
    "This program is third/final step in MANDEYE process",
    "",
    "First step: create project by adding sessions (result of 'multi_view_tls_registration_step_2' program)",
    "Last step: save project",
    "To produce map use 'multi_view_tls_registration_step_2' export functionality"
};

// App specific shortcuts (Type and Shortcut are just for easy reference)
static const std::vector<ShortcutEntry> appShortcuts = { { "Normal keys", "A", "" },
                                                         { "", "Ctrl+A", "Add session(s)" },
                                                         { "", "B", "" },
                                                         { "", "Ctrl+B", "" },
                                                         { "", "C", "" },
                                                         { "", "Ctrl+C", "" },
                                                         { "", "D", "" },
                                                         { "", "Ctrl+D", "Remove session(s)" },
                                                         { "", "E", "" },
                                                         { "", "Ctrl+E", "" },
                                                         { "", "F", "" },
                                                         { "", "Ctrl+F", "" },
                                                         { "", "G", "" },
                                                         { "", "Ctrl+G", "Move center of rotation to gizmo" },
                                                         { "", "H", "" },
                                                         { "", "Ctrl+H", "" },
                                                         { "", "I", "" },
                                                         { "", "Ctrl+I", "" },
                                                         { "", "J", "" },
                                                         { "", "Ctrl+K", "" },
                                                         { "", "K", "" },
                                                         { "", "Ctrl+K", "" },
                                                         { "", "L", "" },
                                                         { "", "Ctrl+L", "Load sessions" },
                                                         { "", "M", "" },
                                                         { "", "Ctrl+M", "Manual loop closure" },
                                                         { "", "N", "" },
                                                         { "", "Ctrl+N", "" },
                                                         { "", "O", "" },
                                                         { "", "Ctrl+O", "Open project" },
                                                         { "", "P", "" },
                                                         { "", "Ctrl+P", "" },
                                                         { "", "Q", "" },
                                                         { "", "Ctrl+Q", "" },
                                                         { "", "R", "" },
                                                         { "", "Ctrl+R", "Random colors per session" },
                                                         { "", "Shift+R", "" },
                                                         { "", "S", "" },
                                                         { "", "Ctrl+S", "Save project" },
                                                         { "", "Ctrl+Shift+S", "Save project as" },
                                                         { "", "T", "" },
                                                         { "", "Ctrl+T", "" },
                                                         { "", "U", "" },
                                                         { "", "Ctrl+U", "" },
                                                         { "", "V", "" },
                                                         { "", "Ctrl+V", "" },
                                                         { "", "W", "" },
                                                         { "", "Ctrl+W", "" },
                                                         { "", "X", "" },
                                                         { "", "Ctrl+X", "" },
                                                         { "", "Y", "" },
                                                         { "", "Ctrl+Y", "" },
                                                         { "", "Z", "" },
                                                         { "", "Ctrl+Z", "" },
                                                         { "", "Shift+Z", "" },
                                                         { "", "1-9", "" },
                                                         { "Special keys", "Up arrow", "" },
                                                         { "", "Shift + up arrow", "" },
                                                         { "", "Ctrl + up arrow", "" },
                                                         { "", "Down arrow", "" },
                                                         { "", "Shift + down arrow", "" },
                                                         { "", "Ctrl + down arrow", "" },
                                                         { "", "Left arrow", "" },
                                                         { "", "Shift + left arrow", "" },
                                                         { "", "Ctrl + left arrow", "" },
                                                         { "", "Right arrow", "" },
                                                         { "", "Shift + right arrow", "" },
                                                         { "", "Ctrl + right arrow", "" },
                                                         { "", "Pg down", "" },
                                                         { "", "Pg up", "" },
                                                         { "", "- key", "" },
                                                         { "", "+ key", "" },
                                                         { "Mouse related", "Left click + drag", "" },
                                                         { "", "Right click + drag", "n" },
                                                         { "", "Scroll", "" },
                                                         { "", "Shift + scroll", "" },
                                                         { "", "Shift + drag", "" },
                                                         { "", "Ctrl + left click", "" },
                                                         { "", "Ctrl + right click", "change center of rotation" },
                                                         { "", "Ctrl + middle click", "change center of rotation" } };

float m_gizmo[] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

bool is_decimate = true;
double bucket_x = 0.1;
double bucket_y = 0.1;
double bucket_z = 0.1;
bool calculate_offset = false;
ObservationPicking observation_picking;
int index_loop_closure_source = -1;
int index_loop_closure_target = -1;
int first_session_index = -1;
int second_session_index = -1;
double search_radius = 0.3;
int nr_iter = 100;
bool loaded_sessions = false;
bool gizmo_all_sessions = false;
//!< Stashed Gizmo update when ImGuizmo::IsUsing(). index is sessionId
std::map<size_t, std::vector<Eigen::Affine3d>> session_drag_preview_poses;
bool is_ndt_gui = false;
bool is_loop_closure_gui = false;
bool prev_is_loop_closure_gui = false;
bool remove_gui = false;
NDT ndt;

bool update_rotation_center = false;

bool is_settings_gui = true;

int number_visible_sessions = 0;
std::set<int> visible_sessions;
int index_gt = -1;
int old_index_gt = -1;
int index_gizmo = -1;
int old_index_gizmo = -1;

double time_stamp_offset = 0.0;

ProjectSettings project_settings;

// Project file opened or last saved; empty until then (Save project falls back to Save project as).
std::string project_file_name;

auto& edges = project_settings.edges;
int index_active_edge = -1;
//!< Last index_active_edge the edge list was scrolled to (loop_closure_gui()).
int edge_list_scrolled_to = -1;
bool manipulate_active_edge = false;
bool edge_gizmo = false;

std::vector<Session> sessions;

int viewer_reduce_rendered_trajectory = 1;
namespace fs = std::filesystem;

int num_edge_extended_before = 0;
int num_edge_extended_after = 0;

// Cross-section slicing, ported from step 2's per-session "Intersections" menu. Step 3 has no
// single active session, so -- same pattern as View > Points size above -- these are pushed to
// every session's point_clouds_container when changed, instead of being one session's fields.
bool gui_xz_intersection = false;
bool gui_yz_intersection = false;
bool gui_xy_intersection = false;
double gui_intersection_width = 0.1;
bool gui_xz_grid_10x10 = false;
bool gui_xz_grid_1x1 = false;
bool gui_xz_grid_01x01 = false;
bool gui_yz_grid_10x10 = false;
bool gui_yz_grid_1x1 = false;
bool gui_yz_grid_01x01 = false;
bool gui_xy_grid_10x10 = false;
bool gui_xy_grid_1x1 = false;
bool gui_xy_grid_01x01 = false;

TaitBryanPose motion_model_weights = { 0.01, 0.01, 0.01, 0.1, 0.1, 0.1 };
///////////////////////////////////////////////////////////////////////////////////

namespace proc = HDMapping::Processing;
using nlohmann::json;

#ifdef HDMAPPING_GUI_BRIDGE
namespace
{
    bool projectJobActive();
    void submitProjectJob(const proc::OperationRequest& request, bool mutate);
    std::unique_ptr<proc::GuiStateBridge> guiState;
    std::unique_ptr<proc::GuiStateConnection> guiConnection;
    proc::ProjectSessionSources projectSources;
} // namespace
#endif

static void markProjectChanged()
{
#ifdef HDMAPPING_GUI_BRIDGE
    if (guiState)
        guiState->Changed();
#endif
}

static void showProjectError(const std::exception& error);

static bool editProjectFromGui(std::string operation, json scope, json parameters)
{
    try
    {
        proc::EditStep3({ std::move(operation), {}, std::move(scope), std::move(parameters) }, project_settings, sessions);
        markProjectChanged();
        return true;
    } catch (const std::exception& error)
    {
        showProjectError(error);
        return false;
    }
}

static bool projectWorkAvailable()
{
#ifdef HDMAPPING_GUI_BRIDGE
    if (projectJobActive())
        return false;
#endif
    return loaded_sessions && !sessions.empty() && session_drag_preview_poses.empty();
}

static void runProjectFromGui(std::string operation, json scope, json parameters)
{
    const proc::OperationRequest request{ std::move(operation), {}, std::move(scope), std::move(parameters) };
#ifdef HDMAPPING_GUI_BRIDGE
    submitProjectJob(request, true);
#else
    try
    {
        proc::RunStep3(request, project_settings, sessions, {});
        markProjectChanged();
    } catch (const std::exception& error)
    {
        showProjectError(error);
    }
#endif
}

static std::array<double, 6> poseComponents(const TaitBryanPose& pose)
{
    return { pose.px, pose.py, pose.pz, pose.om, pose.fi, pose.ka };
}

static void optimizeProjectFromGui()
{
    auto indices = json::array();
    for (std::size_t i = 0; i < edges.size(); ++i)
        indices.push_back(i);
    runProjectFromGui(
        "optimize_project",
        { { "edge_indices", std::move(indices) } },
        { { "motion_model_sigmas", poseComponents(motion_model_weights) }, { "iterations", nr_iter } });
}

void ndt_gui()
{
    if (ImGui::Begin("Normal Distributions Transform", &is_ndt_gui, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::InputFloat3("Bucket size [m] (x, y,z)", ndt.bucket_size);
        if (ndt.bucket_size[0] < 0.01)
            ndt.bucket_size[0] = 0.01f;
        if (ndt.bucket_size[1] < 0.01)
            ndt.bucket_size[1] = 0.01f;
        if (ndt.bucket_size[2] < 0.01)
            ndt.bucket_size[2] = 0.01f;

        ImGui::PushItemWidth(ImGuiNumberWidth);
        ImGui::InputInt("Number of threads", &ndt.number_of_threads);
        if (ndt.number_of_threads < 1)
            ndt.number_of_threads = 1;
        ImGui::SameLine();
        ImGui::InputInt("Number of iterations", &ndt.number_of_iterations);
        if (ndt.number_of_iterations < 1)
            ndt.number_of_iterations = 1;
        ImGui::PopItemWidth();

        ImGui::BeginDisabled(!projectWorkAvailable());
        if (ImGui::Button("NDT optimization"))
        {
            for (auto& session : sessions)
                session.is_gizmo = false;
            index_gizmo = old_index_gizmo = -1;

            auto pairs = json::array();
            for (std::size_t from = 0; from < sessions.size(); ++from)
                for (std::size_t to = from + 1; to < sessions.size(); ++to)
                    pairs.push_back({ { "from", from }, { "to", to } });
            auto scope = sessions.size() == 1 ? json{ { "session_indices", { 0 } } } : json{ { "session_pairs", std::move(pairs) } };
            runProjectFromGui(
                "project.register_sessions",
                std::move(scope),
                { { "method", "ndt" },
                  { "bucket_size_m", { ndt.bucket_size[0], ndt.bucket_size[1], ndt.bucket_size[2] } },
                  { "threads", ndt.number_of_threads },
                  { "iterations", ndt.number_of_iterations } });
        }
        ImGui::EndDisabled();
        ImGui::End();
    }

#if 0
    static bool compute_mean_and_cov_for_bucket = false;
    ImGui::Checkbox("ndt fix_first_node (add I to first pose in Hessian)", &ndt.is_fix_first_node);

    ImGui::Checkbox("ndt Gauss-Newton", &ndt.is_gauss_newton);
    if (ndt.is_gauss_newton)
    {
        ndt.is_levenberg_marguardt = false;
    }

    ImGui::SameLine();
    ImGui::Checkbox("ndt Levenberg-Marguardt", &ndt.is_levenberg_marguardt);
    if (ndt.is_levenberg_marguardt)
    {
        ndt.is_gauss_newton = false;
    }

    ImGui::Checkbox("ndt poses expressed as camera<-world (cw)", &ndt.is_cw);
    if (ndt.is_cw)
    {
        ndt.is_wc = false;
    }
    ImGui::SameLine();
    ImGui::Checkbox("ndt poses expressed as camera->world (wc)", &ndt.is_wc);
    if (ndt.is_wc)
    {
        ndt.is_cw = false;
    }

    ImGui::Checkbox("ndt Tait-Bryan angles (om fi ka: RxRyRz)", &ndt.is_tait_bryan_angles);
    if (ndt.is_tait_bryan_angles)
    {
        ndt.is_quaternion = false;
        ndt.is_rodrigues = false;
    }

    ImGui::SameLine();
    ImGui::Checkbox("ndt Quaternion (q0 q1 q2 q3)", &ndt.is_quaternion);
    if (ndt.is_quaternion)
    {
        ndt.is_tait_bryan_angles = false;
        ndt.is_rodrigues = false;
    }

    ImGui::SameLine();
    ImGui::Checkbox("ndt Rodrigues (sx sy sz)", &ndt.is_rodrigues);
    if (ndt.is_rodrigues)
    {
        ndt.is_tait_bryan_angles = false;
        ndt.is_quaternion = false;
    }

    if (ImGui::Button("compute mean mahalanobis distance"))
    {
        double rms_initial = 0.0;
        double rms_final = 0.0;
        double mui = 0.0;
        ndt.optimize(session.point_clouds_container.point_clouds, true, compute_mean_and_cov_for_bucket);
    }

    ImGui::Text("--------------------------------------------------------------------------------------------------------");

    if (ImGui::Button("ndt_optimization(Lie-algebra left Jacobian)"))
    {
        // icp.optimize_source_to_target_lie_algebra_left_jacobian(point_clouds_container);
        ndt.optimize_lie_algebra_left_jacobian(session.point_clouds_container.point_clouds, compute_mean_and_cov_for_bucket);
    }
    if (ImGui::Button("ndt_optimization(Lie-algebra right Jacobian)"))
    {
        // icp.optimize_source_to_target_lie_algebra_right_jacobian(point_clouds_container);
        ndt.optimize_lie_algebra_right_jacobian(session.point_clouds_container.point_clouds, compute_mean_and_cov_for_bucket);
    }

    ImGui::Text("--------------------------------------------------------------------------------------------------------");

    ImGui::Checkbox("generalized", &ndt.is_generalized);

    if (ndt.is_generalized)
    {
        ImGui::InputDouble("sigma_r", &ndt.sigma_r, 0.01, 0.01);
        ImGui::InputDouble("sigma_polar_angle_rad", &ndt.sigma_polar_angle, 0.0001, 0.0001);
        ImGui::InputDouble("sigma_azimuthal_angle_rad", &ndt.sigma_azimuthal_angle, 0.0001, 0.0001);
        ImGui::InputInt("num_extended_points", &ndt.num_extended_points, 1, 1);

        ImGui::Checkbox("compute_mean_and_cov_for_bucket", &compute_mean_and_cov_for_bucket);
    }

    if (ImGui::Button("Set Zoller+Fröhlich TLS Imager 5006i errors"))
    {
        ndt.sigma_r = 0.0068;
        ndt.sigma_polar_angle = 0.007 * DEG_TO_RAD;
        ndt.sigma_azimuthal_angle = 0.007 * DEG_TO_RAD;
    }

    if (ImGui::Button("Set Zoller+Fröhlich TLS Imager 5010C errors"))
    {
        ndt.sigma_r = 0.01;
        ndt.sigma_polar_angle = 0.007 * DEG_TO_RAD;
        ndt.sigma_azimuthal_angle = 0.007 * DEG_TO_RAD;
    }

    if (ImGui::Button("Set Zoller+Fröhlich TLS Imager 5016 errors"))
    {
        ndt.sigma_r = 0.00025;
        ndt.sigma_polar_angle = 0.004 * DEG_TO_RAD;
        ndt.sigma_azimuthal_angle = 0.004 * DEG_TO_RAD;
    }
    if (ImGui::Button("Set Faro Focus3D errors"))
    {
        ndt.sigma_r = 0.001;
        ndt.sigma_polar_angle = 19.0 * (1.0 / 3600.0) * DEG_TO_RAD;
        ndt.sigma_azimuthal_angle = 19.0 * (1.0 / 3600.0) * DEG_TO_RAD;
    }
    if (ImGui::Button("Set Leica ScanStation C5 C10 errors"))
    {
        ndt.sigma_r = 0.006;
        ndt.sigma_polar_angle = 0.00006;
        ndt.sigma_azimuthal_angle = 0.00006;
    }
    if (ImGui::Button("Set Riegl VZ400 errors"))
    {
        ndt.sigma_r = 0.005;
        ndt.sigma_polar_angle = 0.0005 * DEG_TO_RAD + 0.0003;     // Laser Beam Dicvergence
        ndt.sigma_azimuthal_angle = 0.0005 * DEG_TO_RAD + 0.0003; // Laser Beam Dicvergence
    }
    if (ImGui::Button("Set Leica HDS6100 errors"))
    {
        ndt.sigma_r = 0.009;
        ndt.sigma_polar_angle = 0.000125;
        ndt.sigma_azimuthal_angle = 0.000125;
    }
    if (ImGui::Button("Set Leica P40 errors"))
    {
        ndt.sigma_r = 0.0012;
        ndt.sigma_polar_angle = 8.0 / 3600;
        ndt.sigma_azimuthal_angle = 8.0 / 3600;
    }
#endif
}

// Places the edge gizmo at the active edge's target pose.
void setGizmoFromActiveEdge()
{
    HDMAPPING_ASSERT(index_active_edge < edges.size(), "requested edge outside edges");
    const Edge& e = edges[index_active_edge];

    HDMAPPING_ASSERT(e.index_session_from < sessions.size(), "requested session from outside session vector");
    HDMAPPING_ASSERT(
        e.index_from < sessions[e.index_session_from].point_clouds_container.point_clouds.size(), "scan index outside session");
    const Eigen::Affine3d m_to = sessions[e.index_session_from].point_clouds_container.point_clouds[e.index_from].m_pose *
        affine_matrix_from_pose_tait_bryan(e.relative_pose_tb);
    Eigen::Map<Eigen::Matrix4f> gizmo(m_gizmo);
    gizmo = m_to.matrix().cast<float>();
}

bool canEdgeBeActivated(const int i)
{
    if (i < 0 || i >= (int)edges.size())
    {
        return false;
    }
    const auto& e = edges[i];
    return visible_sessions.contains(e.index_session_from) && visible_sessions.contains(e.index_session_to);
}

// Makes edge `i` active. The manipulate-edge view draws the scans picked by first/second_session_index and
// index_loop_closure_source/target, so these follow the edge; a gizmo that is on is moved to the new edge,
// otherwise it would write the previous edge's pose into this one.
void setActiveEdge(const int i)
{
    HDMAPPING_ASSERT(i >= 0 && i < (int)edges.size(), "Edge outside bounds");

    if (i < 0 || i >= (int)edges.size())
        return;

    const Edge& e = edges[i];

    HDMAPPING_ASSERT(e.index_session_from < (int)sessions.size(), "First session outside");
    HDMAPPING_ASSERT(e.index_session_to < (int)sessions.size(), "Second session outside");

    HDMAPPING_ASSERT(visible_sessions.contains(e.index_session_from), "Cannot edit edge in hidden session");
    HDMAPPING_ASSERT(visible_sessions.contains(e.index_session_to), "Cannot edit edge in hidden session");

    index_active_edge = i;
    first_session_index = e.index_session_from;
    second_session_index = e.index_session_to;
    index_loop_closure_source = e.index_from;
    index_loop_closure_target = e.index_to;

    HDMAPPING_ASSERT(
        e.index_from < (int)sessions[e.index_session_from].point_clouds_container.point_clouds.size(),
        "Edge index_from outside its session's point clouds");
    HDMAPPING_ASSERT(
        e.index_to < (int)sessions[e.index_session_to].point_clouds_container.point_clouds.size(),
        "Edge index_to outside its session's point clouds");

    if (edge_gizmo)
        setGizmoFromActiveEdge();
}

static void refineProjectEdge(double radius, int iterations, int scans_before = 0, int scans_after = 0)
{
    runProjectFromGui(
        "project.constraints.run_icp",
        { { "edge_indices", { index_active_edge } } },
        { { "search_radius_m", radius }, { "iterations", iterations }, { "scans_before", scans_before }, { "scans_after", scans_after } });
}

void on_loop_closure_gui_open()
{
    // Session gizmos are not drawn in this mode and would block the edge gizmo, so drop the selection.
    if (index_gizmo != -1)
    {
        index_gizmo = -1;
        old_index_gizmo = -1;
        for (auto& s : sessions)
            s.is_gizmo = false;
    }
    manipulate_active_edge = false;
}
void on_loop_closure_gui_close()
{
    // reset
    manipulate_active_edge = false;
    index_loop_closure_target = 0;
    index_loop_closure_source = 0;
}

void loop_closure_gui()
{
    if (ImGui::Begin("Manual Pose Graph Loop Closure Mode", &is_loop_closure_gui, ImGuiWindowFlags_AlwaysAutoResize))
    {
        std::stringstream ss;
        for (auto& s : visible_sessions)
        {
            ss << s << " ";
        }
        ImGui::Text("Visible sessions %s", ss.str().c_str());

        ImGui::BeginDisabled(!projectWorkAvailable());
        if (ImGui::Button("Optimize GRAPH"))
            optimizeProjectFromGui();
        ImGui::EndDisabled();

        ImGui::Checkbox("update_rotation_center", &update_rotation_center);

        // visible session
        const auto& firstSessionRef = sessions[first_session_index];
        const auto& secondSessionRef = sessions[second_session_index];
        HDMAPPING_ASSERT(!firstSessionRef.point_clouds_container.point_clouds.empty(), "First session should have at least one cloud");
        HDMAPPING_ASSERT(!secondSessionRef.point_clouds_container.point_clouds.empty(), "Second session should have at least one cloud");

        const int firstSessionUpper = firstSessionRef.point_clouds_container.point_clouds.size();
        const int secondSessionUpper = secondSessionRef.point_clouds_container.point_clouds.size();

        {
            // Same control and the same push-to-every-session behavior as View > Points
            // size -- was a separate "gui_point_size" that unconditionally overwrote every
            // session's point_size every frame, permanently fighting View > Points size.
            int tmp_point_size = point_size;
            ImGui::InputInt("Points size", &point_size);
            if (point_size < 1)
                point_size = 1;
            if (tmp_point_size != point_size)
                for (auto& session : sessions)
                    for (auto& point_cloud : session.point_clouds_container.point_clouds)
                        point_cloud.point_size = point_size;
        }

        ImGui::Text("Manualy adding edges from session %d to %d", first_session_index, second_session_index);
        ImGui::Text("Num edge extended:");

        ImGui::Text("before: ");
        ImGui::SameLine();
        ImGui::PushItemWidth(ImGuiNumberWidth);
        ImGui::SliderInt("##fs", &num_edge_extended_before, 0, firstSessionUpper);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("min 0; max %zu", firstSessionUpper);
        ImGui::SameLine();
        ImGui::InputInt("##fi", &num_edge_extended_before, 1, 5);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("min 0; max %zu", firstSessionUpper);
        if (num_edge_extended_before < 0)
            num_edge_extended_before = 0;
        if (num_edge_extended_before >= firstSessionUpper)
            num_edge_extended_before = firstSessionUpper;

        ImGui::Text(" after: ");
        ImGui::SameLine();

        ImGui::SliderInt("##ts", &num_edge_extended_after, index_loop_closure_target, static_cast<int>(secondSessionUpper));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("min 0; max %zu", secondSessionUpper);
        ImGui::SameLine();
        ImGui::InputInt("##ti", &num_edge_extended_after, 1, 5);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("min 0; max %zu", secondSessionUpper);
        if (num_edge_extended_after < 0)
            num_edge_extended_after = 0;
        if (num_edge_extended_after >= secondSessionUpper)
            num_edge_extended_after = secondSessionUpper;
        ImGui::PopItemWidth();
        //

        if (!manipulate_active_edge)
        {
            ImGui::SliderInt(
                "##index_loop_closure_source_slider",
                &index_loop_closure_source,
                0,
                static_cast<int>(sessions[first_session_index].point_clouds_container.point_clouds.size() - 1));
            ImGui::SameLine();
            ImGui::InputInt("index_loop_closure_source", &index_loop_closure_source);
            if (index_loop_closure_source < 0)
                index_loop_closure_source = 0;
            if (index_loop_closure_source >= sessions[first_session_index].point_clouds_container.point_clouds.size() - 1)
                index_loop_closure_source = sessions[first_session_index].point_clouds_container.point_clouds.size() - 1;
            ImGui::SliderInt(
                "##index_loop_closure_target_slider",
                &index_loop_closure_target,
                0,
                static_cast<int>(sessions[second_session_index].point_clouds_container.point_clouds.size() - 1));
            ImGui::SameLine();
            ImGui::InputInt("index_loop_closure_target", &index_loop_closure_target);
            if (index_loop_closure_target < 0)
                index_loop_closure_target = 0;
            if (index_loop_closure_target >= sessions[second_session_index].point_clouds_container.point_clouds.size() - 1)
                index_loop_closure_target = sessions[second_session_index].point_clouds_container.point_clouds.size() - 1;
        }
        if (ImGui::Button("Add edge"))
        {
            Edge edge;
            edge.index_from = index_loop_closure_source;
            edge.index_to = index_loop_closure_target;
            edge.index_session_from = first_session_index;
            edge.index_session_to = second_session_index;

            edge.relative_pose_tb = pose_tait_bryan_from_affine_matrix(
                sessions[first_session_index].point_clouds_container.point_clouds[index_loop_closure_source].m_pose.inverse() *
                sessions[second_session_index].point_clouds_container.point_clouds[index_loop_closure_target].m_pose);

            edge.relative_pose_tb_weights.px = 1000000.0;
            edge.relative_pose_tb_weights.py = 1000000.0;
            edge.relative_pose_tb_weights.pz = 1000000.0;
            edge.relative_pose_tb_weights.om = 1000000.0;
            edge.relative_pose_tb_weights.fi = 1000000.0;
            edge.relative_pose_tb_weights.ka = 1000000.0;

            if (editProjectFromGui(
                    "project.constraints.add",
                    json::object(),
                    { { "edge", proc::ProjectEdgesJson(std::span<const Edge>(&edge, 1)).at(0) } }))
                setActiveEdge(static_cast<int>(edges.size()) - 1);
        }

        std::string number_active_edges = "number_edges: " + std::to_string(edges.size());
        ImGui::Text(number_active_edges.c_str());

        // Switching edges is blocked while a session gizmo is on, as for the index_active_edge input below.
        bool is_session_gizmo = false;
        for (const auto& s : sessions)
            is_session_gizmo |= s.is_gizmo;

        ImGui::BeginDisabled(is_session_gizmo);
        int delete_edge_index = -1;
        // Scrollable list, as step 2's "LIO segments" list, so many edges don't grow the
        // window (AlwaysAutoResize) and push the active-edge controls below off screen.
        // Fixed height (up to kEdgeListMaxRows rows) since an auto-resizing window can't
        // give a child "the remaining space".
        constexpr int kEdgeListMaxRows = 10;
        const int edgeListRows = std::clamp((int)edges.size(), 1, kEdgeListMaxRows);
        const float edgeListHeight = edgeListRows * ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().WindowPadding.y * 2.0f;
        ImGui::BeginChild("Edges", ImVec2(0, edgeListHeight), true);
        for (int i = 0; i < (int)edges.size(); i++)
        {
            const auto& e = edges[i];
            // session:scan -> session:scan
            ImGui::Text(
                "%s edge %d: %d:%d -> %d:%d",
                (manipulate_active_edge && i == index_active_edge) ? ">" : " ",
                i,
                e.index_session_from,
                e.index_from,
                e.index_session_to,
                e.index_to);
            ImGui::SameLine();
            char buttonLabel[128];
            snprintf(buttonLabel, sizeof(buttonLabel), "Set Active##%d", i);

            ImGui::BeginDisabled(!canEdgeBeActivated(i));
            if (ImGui::Button(buttonLabel))
            {
                setActiveEdge(i);
                manipulate_active_edge = true;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            snprintf(buttonLabel, sizeof(buttonLabel), "Delete##%d", i);
            if (ImGui::Button(buttonLabel))
                delete_edge_index = i;

            // Keep the active edge in view when it changes (slider/input below, Add edge).
            if (i == index_active_edge && index_active_edge != edge_list_scrolled_to)
            {
                ImGui::SetScrollHereY(0.5f);
                edge_list_scrolled_to = index_active_edge;
            }
        }
        ImGui::EndChild();
        ImGui::EndDisabled();

        // Deleted after the loop so the list is not modified while it is drawn. The active edge keeps pointing at
        // the same edge; deleting the active edge itself leaves manipulate mode, as "remove active edge" does.
        if (delete_edge_index != -1 &&
            editProjectFromGui("project.constraints.remove", { { "edge_index", delete_edge_index } }, json::object()))
        {
            if (delete_edge_index == index_active_edge)
            {
                edge_gizmo = false;
                manipulate_active_edge = false;
                index_active_edge = edges.empty() ? -1 : std::clamp(delete_edge_index - 1, 0, (int)edges.size() - 1);
            }
            else if (delete_edge_index < index_active_edge)
            {
                index_active_edge--;
            }
        }
        if (edges.size() > 0)
        {
            const bool isEdgeOk = canEdgeBeActivated(index_active_edge);
            ImGui::BeginDisabled(!isEdgeOk);
            if (ImGui::Checkbox("manipulate_active_edge", &manipulate_active_edge) && manipulate_active_edge)
                setActiveEdge(std::clamp(index_active_edge, 0, (int)edges.size() - 1));
            if (manipulate_active_edge)
            {
                int prev_index_active_edge = index_active_edge;

                if (!edge_gizmo)
                {
                    bool is_gizmo = false;

                    for (const auto& s : sessions)
                    {
                        if (s.is_gizmo)
                            is_gizmo = true;
                    }

                    if (!is_gizmo)
                    {
                        ImGui::SliderInt("##index_active_edge_slider", &index_active_edge, 0, (int)edges.size() - 1);
                        ImGui::SameLine();
                        ImGui::InputInt("index_active_edge", &index_active_edge);

                        if (index_active_edge < 0)
                            index_active_edge = 0;
                        if (index_active_edge >= (int)edges.size())
                            index_active_edge = (int)edges.size() - 1;
                        if (index_active_edge != prev_index_active_edge)
                            setActiveEdge(index_active_edge);
                    }
                }

                std::string txt = "index_session_from: " + std::to_string(edges[index_active_edge].index_session_from);
                ImGui::Text(txt.c_str());
                txt = "index_session_to: " + std::to_string(edges[index_active_edge].index_session_to);
                ImGui::Text(txt.c_str());
                txt = "index_from: " + std::to_string(edges[index_active_edge].index_from);
                ImGui::Text(txt.c_str());
                txt = "index_to: " + std::to_string(edges[index_active_edge].index_to);
                ImGui::Text(txt.c_str());

                const bool prev_gizmo = edge_gizmo;
                ImGui::Checkbox("gizmo", &edge_gizmo);

                if (prev_gizmo != edge_gizmo)
                    setGizmoFromActiveEdge();
                if (!edge_gizmo)
                {
                    ImGui::BeginDisabled(!projectWorkAvailable());
                    if (ImGui::Button("ICP"))
                        refineProjectEdge(search_radius, 10, num_edge_extended_before, num_edge_extended_after);
                    ImGui::SameLine();
                    ImGui::InputDouble("search_radius", &search_radius);
                    if (search_radius < 0.01)
                        search_radius = 0.01;

                    /////////////////////////////////
                    if (ImGui::Button("ICP [search radius 2m]"))
                        refineProjectEdge(2.0, 30);

                    ImGui::SameLine();
                    if (ImGui::Button("ICP [search radius 1m]"))
                        refineProjectEdge(1.0, 30);
                    ImGui::SameLine();
                    if (ImGui::Button("ICP [search radius 0.5m]"))
                        refineProjectEdge(0.5, 30);
                    ImGui::SameLine();
                    if (ImGui::Button("ICP [search radius 0.25m]"))
                        refineProjectEdge(0.25, 30);
                    ImGui::SameLine();
                    if (ImGui::Button("ICP [search radius 0.1m]"))
                        refineProjectEdge(0.1, 30);
                    ImGui::EndDisabled();
#if 0
                    if (ImGui::Button("Save src"))
                    {
                        const auto output_file_name = mandeye::fd::SaveFileDialog("Output file name", mandeye::fd::LAS_LAZ_filter, ".laz");
                        std::cout << "laz file to save: '" << output_file_name << "'" << std::endl;

                        if (output_file_name.size() > 0)
                        {
                            std::vector<Eigen::Vector3d> source = sessions[edges[index_active_edge].index_session_to].point_clouds_container.point_clouds[edges[index_active_edge].index_to].points_local;
                            std::vector<Eigen::Vector3d> pointcloud;
                            std::vector<unsigned short> intensity;
                            std::vector<double> timestamps;

                            for (size_t i = 0; i < source.size(); i++)
                            {
                                pointcloud.push_back(source[i]);
                                intensity.push_back(0);
                                timestamps.push_back(0.0);
                            }

                            exportLaz(
                                output_file_name[0],
                                pointcloud,
                                intensity,
                                timestamps);
                        }

                        /*int number_of_iterations = 30;
                        PairWiseICP icp;
                        auto m_pose = affine_matrix_from_pose_tait_bryan(edges[index_active_edge].relative_pose_tb);

                        std::vector<Eigen::Vector3d> source = sessions[edges[index_active_edge].index_session_to].point_clouds_container.point_clouds[edges[index_active_edge].index_to].points_local;
                        std::vector<Eigen::Vector3d> target = sessions[edges[index_active_edge].index_session_from].point_clouds_container.point_clouds[edges[index_active_edge].index_from].points_local;

                        if (icp.compute(source, target, sr, number_of_iterations, m_pose))
                        {
                            edges[index_active_edge].relative_pose_tb = pose_tait_bryan_from_affine_matrix(m_pose);
                        }*/
                        //save
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Save trg (transfromed only by rotation)"))
                    {
                    }
#endif
                    //////////////////////////////////
                }
            }
            ImGui::EndDisabled();
        }

        ImGui::End();
    }
}

#ifdef HDMAPPING_GUI_BRIDGE
static void publish_project_files(const json& parameters)
{
    auto selected = json::array();
    const bool poses = parameters.at("format") == "poses";
    for (std::size_t i = 0; i < sessions.size(); ++i)
    {
        const auto& session = sessions.at(i);
        if (poses && (session.is_ground_truth || Session::is_point_cloud_file(session.session_file_name)))
            continue;
        auto scans = json::array();
        const auto& clouds = session.point_clouds_container.point_clouds;
        for (std::size_t scan = 0; scan < clouds.size(); ++scan)
            if (poses || clouds[scan].visible)
                scans.push_back(scan);
        if (!scans.empty())
            selected.push_back({ { "session_index", i }, { "scan_indices", std::move(scans) } });
    }
    if (selected.empty())
    {
        pfd::message("Export failed", "No scans in the selected sessions", pfd::choice::ok, pfd::icon::error);
        return;
    }
    submitProjectJob({ std::string(proc::project_export_operation), {}, { { "sessions", std::move(selected) } }, parameters }, false);
}
#endif

static void export_project_files(const json& parameters, const std::string& suffix, bool common_folder = false)
{
    const auto format = parameters.at("format").get<std::string>();
    for (std::size_t i = 0; i < sessions.size(); ++i)
    {
        const auto& session = sessions[i];
        const auto dir = sessionPathFromText(project_settings.session_file_names.at(i)).parent_path();
        auto output = dir;
        try
        {
            if (common_folder)
            {
                output = dir.parent_path() / "all_tum_files";
                fs::create_directories(output);
            }
            auto file = dir.filename();
            file += suffix;
            const auto path = output / file;
            bool written = false;
            if (format == "section_laz")
            {
                const auto plane = parameters.at("plane").get<std::string>();
                written = save_intersection(
                    session, sessionPathText(path), plane == "xz", plane == "yz", plane == "xy", parameters.at("width_m").get<double>());
            }
            else
                written = proc::WriteProjectTrajectory(
                    session,
                    path,
                    format,
                    parameters.value("timestamps", std::string("lidar")),
                    parameters.value("pose", std::string("matrix")),
                    parameters.value("sort_tum", true));
            if (!written)
                spdlog::error("Could not write '{}'", sessionPathText(path));
        } catch (const std::exception& error)
        {
            spdlog::error("Project export failed: {}", error.what());
        }
    }
}

void createDXFPolyline(const std::string& filename, const std::vector<Eigen::Vector3d>& points)
{
    std::ofstream dxfFile(filename);
    dxfFile << std::setprecision(20);
    if (!dxfFile.is_open())
    {
        std::cerr << "Failed to open file: " << filename << std::endl;
        return;
    }

    // DXF header
    dxfFile << "0\nSECTION\n2\nHEADER\n0\nENDSEC\n";
    dxfFile << "0\nSECTION\n2\nTABLES\n0\nENDSEC\n";

    // Start the ENTITIES section
    dxfFile << "0\nSECTION\n2\nENTITIES\n";

    // Start the POLYLINE entity
    dxfFile << "0\nPOLYLINE\n";
    dxfFile << "8\n0\n"; // Layer 0
    dxfFile << "66\n1\n"; // Indicates the presence of vertices
    dxfFile << "70\n8\n"; // 1 = Open polyline

    // Write the VERTEX entities
    for (const auto& point : points)
    {
        dxfFile << "0\nVERTEX\n";
        dxfFile << "8\n0\n"; // Layer 0
        dxfFile << "10\n" << point.x() << "\n"; // X coordinate
        dxfFile << "20\n" << point.y() << "\n"; // Y coordinate
        dxfFile << "30\n" << point.z() << "\n"; // Z coordinate
    }

    // End the POLYLINE
    dxfFile << "0\nSEQEND\n";

    // End the ENTITIES section
    dxfFile << "0\nENDSEC\n";

    // End the DXF file
    dxfFile << "0\nEOF\n";

    dxfFile.close();
    std::cout << "DXF file created: " << filename << std::endl;
}

void save_trajectories(
    Session& session,
    const std::string& output_file_name,
    float curve_consecutive_distance_meters,
    float not_curve_consecutive_distance_meters,
    bool is_trajectory_export_downsampling,
    bool write_lidar_timestamp,
    bool write_unix_timestamp,
    bool use_quaternions,
    bool save_to_dxf)
{
    std::ofstream outfile;
    if (!save_to_dxf)
    {
        outfile.open(output_file_name);
    }
    if (save_to_dxf || outfile.good())
    {
        float consecutive_distance = 0;
        std::vector<Eigen::Vector3d> polylinePoints;
        for (auto& p : session.point_clouds_container.point_clouds)
        {
            if (p.visible)
            {
                for (size_t i = 0; i < p.local_trajectory.size(); i++)
                {
                    const auto& m = p.local_trajectory[i].m_pose;
                    Eigen::Affine3d pose = p.m_pose * m;
                    pose.translation() += session.point_clouds_container.offset;

                    if (i > 0)
                    {
                        double dist = (p.local_trajectory[i].m_pose.translation() - p.local_trajectory[i - 1].m_pose.translation()).norm();
                        consecutive_distance += dist;
                    }

                    bool is_curve = false;

                    if (i > 100 && i < p.local_trajectory.size() - 100)
                    {
                        Eigen::Vector3d position_prev = p.local_trajectory[i - 100].m_pose.translation();
                        Eigen::Vector3d position_curr = p.local_trajectory[i].m_pose.translation();
                        Eigen::Vector3d position_next = p.local_trajectory[i + 100].m_pose.translation();

                        Eigen::Vector3d v1 = position_curr - position_prev;
                        Eigen::Vector3d v2 = position_next - position_curr;

                        if (v1.norm() > 0 && v2.norm() > 0)
                        {
                            double angle_deg = fabs(acos(v1.dot(v2) / (v1.norm() * v2.norm())) * RAD_TO_DEG);

                            if (angle_deg > 10.0)
                                is_curve = true;
                        }
                    }
                    double tol = not_curve_consecutive_distance_meters;

                    if (is_curve)
                        tol = curve_consecutive_distance_meters;

                    if (!is_trajectory_export_downsampling || (is_trajectory_export_downsampling && consecutive_distance >= tol))
                    {
                        if (is_trajectory_export_downsampling)
                            consecutive_distance = 0;
                        if (save_to_dxf)
                            polylinePoints.push_back(pose.translation());
                        else
                        {
                            outfile << std::setprecision(20);

                            if (write_lidar_timestamp)
                                outfile << p.local_trajectory[i].timestamps.first << ",";
                            if (write_unix_timestamp)
                                outfile << p.local_trajectory[i].timestamps.second << ",";

                            outfile << pose(0, 3) << "," << pose(1, 3) << "," << pose(2, 3) << ",";
                            if (use_quaternions)
                            {
                                Eigen::Quaterniond q(pose.rotation());
                                outfile << q.x() << "," << q.y() << "," << q.z() << "," << q.w() << std::endl;
                            }
                            else
                                outfile << pose(0, 0) << "," << pose(0, 1) << "," << pose(0, 2) << "," << pose(1, 0) << "," << pose(1, 1)
                                        << "," << pose(1, 2) << "," << pose(2, 0) << "," << pose(2, 1) << "," << pose(2, 2) << std::endl;
                        }
                    }
                }
            }
        }
        if (!save_to_dxf)
            outfile.close();
        else
            createDXFPolyline(output_file_name, polylinePoints);
    }
}

static bool isManagedProjectFile(const fs::path& file)
{
    std::error_code ec;
    auto path = fs::weakly_canonical(file, ec);
    if (ec)
        return false;
    for (auto parent = path.parent_path(); parent != parent.root_path(); parent = parent.parent_path())
        if (fs::is_regular_file(parent / "snapshot.json", ec) && fs::is_regular_file(parent.parent_path() / "result.json", ec))
            return true;
    return false;
}

bool save_results(std::vector<Session>& sessions)
{
    std::string failed;
    for (auto& session : sessions)
    {
        if (!session.is_ground_truth)
        {
            // Sessions loaded from a single point cloud live in memory only and have no poses file
            if (Session::is_point_cloud_file(session.session_file_name))
            {
                std::cout << "not saving poses of point cloud session: " << session.session_file_name << std::endl;
                continue;
            }
            if (isManagedProjectFile(sessionPathFromText(session.point_clouds_container.poses_file_name)))
            {
                failed += "'" + session.point_clouds_container.poses_file_name + "' (published result)\n";
                continue;
            }
            std::cout << "saving result to: " << session.point_clouds_container.poses_file_name << std::endl;
            if (!session.point_clouds_container.save_poses(fs::path(session.point_clouds_container.poses_file_name).string(), false))
                failed += "'" + session.point_clouds_container.poses_file_name + "'\n";
        }
    }

    if (!failed.empty())
    {
        pfd::message("Saving poses failed", "Could not write poses file(s):\n" + failed, pfd::choice::ok, pfd::icon::error);
        return false;
    }
    return true;
}

Eigen::Vector3d GLWidgetGetOGLPos(int x, int y, const ObservationPicking& observation_picking)
{
    const auto laser_beam = GetLaserBeam(x, y);

    RegistrationPlaneFeature::Plane pl;

    pl.a = 0;
    pl.b = 0;
    pl.c = 1;
    pl.d = -observation_picking.picking_plane_height;

    Eigen::Vector3d pos = rayIntersection(laser_beam, pl);

    std::cout << "intersection: " << pos.x() << " " << pos.y() << " " << pos.z() << std::endl;

    return pos;
}

static void updateProjectTitle(const std::string& file_name)
{
    project_file_name = file_name;
    const auto title = winTitle + " - " + truncPath(file_name);
    SetWindowTitle(title.c_str());
}

static void refreshVisibleSessions()
{
    visible_sessions.clear();
    for (std::size_t index = 0; index < sessions.size(); ++index)
        if (sessions[index].visible)
            visible_sessions.insert(static_cast<int>(index));
    number_visible_sessions = static_cast<int>(visible_sessions.size());
    first_session_index = visible_sessions.empty() ? -1 : *visible_sessions.begin();
    second_session_index = visible_sessions.empty() ? -1 : *visible_sessions.rbegin();
}

static void resetProjectEdgeSelection()
{
    index_active_edge = edge_list_scrolled_to = -1;
    manipulate_active_edge = edge_gizmo = false;
    refreshVisibleSessions();
    index_loop_closure_source = index_loop_closure_target = 0;
}

static void resetProjectSelection()
{
    session_drag_preview_poses.clear();
    resetProjectEdgeSelection();
    is_loop_closure_gui = false;
    index_gizmo = old_index_gizmo = -1;
    index_gt = -1;
    for (std::size_t i = 0; i < sessions.size(); ++i)
    {
        sessions[i].is_gizmo = false;
        if (sessions[i].is_ground_truth)
            index_gt = static_cast<int>(i);
    }
    old_index_gt = index_gt;
}

static void showProjectError(const std::exception& error)
{
    pfd::message("Project I/O", error.what(), pfd::choice::ok, pfd::icon::error);
}

bool loadProject(const std::string& file_name)
{
    try
    {
        auto loaded = ReadProjectSettings(sessionPathFromText(file_name));
#ifdef HDMAPPING_GUI_BRIDGE
        if (guiState)
            guiState->Open();
        projectSources.clear();
#endif
        project_settings = std::move(loaded);
        sessions.clear();
        resetProjectSelection();
        invalidateSessionRenderers();
        loaded_sessions = false;
        time_stamp_offset = 0.0;
        updateProjectTitle(file_name);
        return true;
    } catch (const std::exception& error)
    {
        showProjectError(error);
        return false;
    }
}

void openProject()
{
    std::string input_file_name = "";
    input_file_name = mandeye::fd::OpenFileDialogOneFile("Open project", mandeye::fd::Project_filter);

    if (input_file_name.size() > 0)
    {
        loadProject(input_file_name);
    }
}

void saveProjectTo(const std::string& output_file_name)
{
    try
    {
        const auto path = sessionPathFromText(output_file_name);
        if (isManagedProjectFile(path))
            throw std::invalid_argument("Choose a project file outside the published result.");
        WriteProjectSettings(path, project_settings);
        updateProjectTitle(output_file_name);
    } catch (const std::exception& error)
    {
        showProjectError(error);
    }
}

void saveProjectAs()
{
    std::string output_file_name = "";
    output_file_name = mandeye::fd::SaveFileDialog("Save project file", mandeye::fd::Project_filter, ".mjp", "project");

    if (output_file_name.size() > 0)
        saveProjectTo(output_file_name);
}

void saveProject()
{
    if (project_file_name.empty() || isManagedProjectFile(sessionPathFromText(project_file_name)))
        saveProjectAs();
    else
        saveProjectTo(project_file_name);
}

void loadSessions();
void finishLoadingSessions();

static Session readSessionForGui(const std::string& path)
{
    return ReadProjectSession(
        sessionPathText(fs::absolute(sessionPathFromText(path))),
        { is_decimate, { bucket_x, bucket_y, bucket_z }, calculate_offset, false });
}

bool addSessionFile(const std::string& path)
{
    try
    {
#ifdef HDMAPPING_GUI_BRIDGE
        const auto absolute = sessionPathText(fs::absolute(sessionPathFromText(path)).lexically_normal());
        if (std::ranges::any_of(
                sessions,
                [&absolute](const Session& session)
                {
                    const auto source = projectSources.find(session.session_file_name);
                    return source != projectSources.end() && source->second == absolute;
                }))
            return false;
#endif
        if (!loaded_sessions)
        {
            if (!AddProjectSessionPath(project_settings, path))
                return false;
            markProjectChanged();
            return true;
        }
        // Preserve the current sessions and any unsaved poses.
        auto session = readSessionForGui(path);
        sessions.reserve(sessions.size() + 1);
        if (!AddProjectSessionPath(project_settings, path))
            return false;
        if (session.is_ground_truth)
            SetProjectGroundTruth(sessions, -1);
        sessions.push_back(std::move(session));
        return true;
    } catch (const std::exception& error)
    {
        showProjectError(error);
        return false;
    }
}

void addSession()
{
    static const std::vector<std::string> filter = {
        "Session or point cloud (*.mjs, *.json, *.laz, *.las)", "*.mjs *.json *.laz *.las", "All files", "*"
    };
    bool added = false;
    for (const auto& file : mandeye::fd::OpenFileDialog("Add session(s)", filter, true))
        added |= addSessionFile(file);
    if (added && loaded_sessions)
        finishLoadingSessions();
}

// Next file dialog opens in the folder of a file loaded without one (drag & drop, command line).
void setLocationHintFromFile(const fs::path& file)
{
    std::error_code ec;
    const fs::path absolute = fs::absolute(file, ec);
    mandeye::fd::SetLastLocationHint((ec ? file : absolute).parent_path().string());
}

// Drag & drop: a project (*.mjp) replaces the current one; session files (*.mjs, *.json) and single point clouds
// (*.laz, *.las) are added and loaded right away (appended to already loaded sessions, which keep their in-memory state).
void loadDroppedFiles()
{
    FilePathList dropped_files = LoadDroppedFiles();
    std::vector<fs::path> paths(dropped_files.paths, dropped_files.paths + dropped_files.count);
    UnloadDroppedFiles(dropped_files);

    auto ext_of = [](const fs::path& p)
    {
        std::string ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        return ext;
    };

    for (const auto& path : paths)
    {
        if (ext_of(path) == ".mjp")
        {
            setLocationHintFromFile(path);
            loadProject(sessionPathText(path));
            return;
        }
    }

    std::vector<std::string> added;
    for (const auto& path : paths)
    {
        const std::string ext = ext_of(path);
        if (ext != ".mjs" && ext != ".json" && !Session::is_point_cloud_file(path.string()))
        {
            std::cout << "Ignoring dropped file: '" << path.string() << "'" << std::endl;
            continue;
        }
        if (addSessionFile(path.string()))
            added.push_back(path.string());
    }

    if (added.empty())
    {
        pfd::message(
            "Unsupported file",
            "Drop a project (*.mjp), session files (*.mjs, *.json) or point clouds (*.laz, *.las).",
            pfd::choice::ok,
            pfd::icon::warning);
        return;
    }
    setLocationHintFromFile(added.back());

    if (loaded_sessions)
        finishLoadingSessions();
    else
        loadSessions();
}

// Refresh the window after committing a complete revision of the project.
static void refreshProjectSessions()
{
    resetProjectSelection();
    for (auto& session : sessions)
    {
        auto& clouds = session.point_clouds_container;
        clouds.xz_intersection = gui_xz_intersection;
        clouds.yz_intersection = gui_yz_intersection;
        clouds.xy_intersection = gui_xy_intersection;
        clouds.intersection_width = gui_intersection_width;
        clouds.xz_grid_10x10 = gui_xz_grid_10x10;
        clouds.xz_grid_1x1 = gui_xz_grid_1x1;
        clouds.xz_grid_01x01 = gui_xz_grid_01x01;
        clouds.yz_grid_10x10 = gui_yz_grid_10x10;
        clouds.yz_grid_1x1 = gui_yz_grid_1x1;
        clouds.yz_grid_01x01 = gui_yz_grid_01x01;
        clouds.xy_grid_10x10 = gui_xy_grid_10x10;
        clouds.xy_grid_1x1 = gui_xy_grid_1x1;
        clouds.xy_grid_01x01 = gui_xy_grid_01x01;
        for (auto& scan : clouds.point_clouds)
            scan.point_size = point_size;
    }
    assignDistinctSessionColors(sessions);
    invalidateSessionRenderers();
    time_stamp_offset = 0.0;
    // Preserve Step 3's existing latest starting timestamp on load.
    for (const auto& session : sessions)
    {
        const auto& scans = session.point_clouds_container.point_clouds;
        if (!scans.empty() && !scans.front().local_trajectory.empty())
            time_stamp_offset = std::max(time_stamp_offset, scans.front().local_trajectory.front().timestamps.first);
    }
}

void finishLoadingSessions()
{
    OrderProjectGroundTruthFirst(project_settings, sessions);
    refreshProjectSessions();
    markProjectChanged();
}

void loadSessions()
{
    try
    {
        std::vector<Session> loaded;
        for (const auto& path : project_settings.session_file_names)
            loaded.push_back(readSessionForGui(path));
        ValidateProjectScans(project_settings, loaded);
        sessions = std::move(loaded);
        loaded_sessions = true;
        finishLoadingSessions();
    } catch (const std::exception& error)
    {
        showProjectError(error);
    }
}

void settings_gui()
{
    if (ImGui::Begin("Settings", &is_settings_gui))
    {
        ImGui::Checkbox("Downsample during load", &is_decimate);
        ImGui::SameLine();
        ImGui::Text("Bucket [m]:");
        ImGui::PushItemWidth(ImGuiNumberWidth);
        ImGui::InputDouble("X##b", &bucket_x, 0.0, 0.0, "%.3f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(xText);
        ImGui::SameLine();
        ImGui::InputDouble("Y##b", &bucket_y, 0.0, 0.0, "%.3f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(yText);
        ImGui::SameLine();
        ImGui::InputDouble("Z##b", &bucket_z, 0.0, 0.0, "%.3f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(zText);
        ImGui::PopItemWidth();

        ImGui::NewLine();

        ImGui::Separator();

        ImGui::Text("Benchmark settings:");

        ImGui::PushItemWidth(ImGuiNumberWidth * 2);

        static double fast_plus = 100000000.0;
        static double fast_plus_plus = 1000000000.0;

        ImGui::InputDouble("Increment", &fast_plus);
        ImGui::InputDouble("Fast increment", &fast_plus_plus);
        ImGui::InputDouble("Timestamp offset", &time_stamp_offset, fast_plus, fast_plus_plus);
        ImGui::PopItemWidth();
        ImGui::SameLine();
        if (ImGui::Button("Set to origin"))
        {
            markProjectChanged();
            bool is_first_gt = false;

            if (sessions.size() > 0)
            {
                if (sessions[0].is_ground_truth)
                    is_first_gt = true;
            }

            Eigen::Affine3d m_gt = Eigen::Affine3d::Identity();
            if (sessions.size() > 0)
            {
                int index_point_clouds = -1;
                int index_local_trajectory = -1;
                bool found = false;
                for (size_t a = 0; a < sessions[0].point_clouds_container.point_clouds.size(); a++)
                {
                    for (size_t b = 0; b < sessions[0].point_clouds_container.point_clouds[a].local_trajectory.size(); b++)
                    {
                        if (sessions[0].point_clouds_container.point_clouds[a].local_trajectory[b].timestamps.first > time_stamp_offset)
                        {
                            if (!found)
                            {
                                found = true;
                                index_point_clouds = a;
                                index_local_trajectory = b;
                                break;
                            }
                        }
                    }
                }

                if (index_point_clouds != -1 && index_local_trajectory != -1)
                {
                    m_gt = sessions[0].point_clouds_container.point_clouds[index_point_clouds].m_pose *
                        sessions[0].point_clouds_container.point_clouds[index_point_clouds].local_trajectory[index_local_trajectory].m_pose;
                }
            }

            // for (auto& session : sessions)
            for (auto& session : sessions)
            {
                if (is_first_gt)
                {
                    if (session.is_ground_truth)
                        continue;
                }

                int index_point_clouds = -1;
                int index_local_trajectory = -1;
                bool found = false;
                for (size_t a = 0; a < session.point_clouds_container.point_clouds.size(); a++)
                {
                    for (size_t b = 0; b < session.point_clouds_container.point_clouds[a].local_trajectory.size(); b++)
                    {
                        if (session.point_clouds_container.point_clouds[a].local_trajectory[b].timestamps.first > time_stamp_offset)
                        {
                            if (!found)
                            {
                                found = true;
                                index_point_clouds = a;
                                index_local_trajectory = b;
                                break;
                            }
                        }
                    }
                }

                if (index_point_clouds != -1 && index_local_trajectory != -1)
                {
                    auto m1 = session.point_clouds_container.point_clouds[index_point_clouds].m_pose;
                    auto m2 =
                        session.point_clouds_container.point_clouds[index_point_clouds].local_trajectory[index_local_trajectory].m_pose;

                    auto inv = Eigen::Affine3d::Identity();
                    inv = (m1 * m2).inverse();
                    for (size_t index = 0; index < session.point_clouds_container.point_clouds.size(); index++)
                        session.point_clouds_container.point_clouds[index].m_pose =
                            inv * session.point_clouds_container.point_clouds[index].m_pose;

                    for (size_t index = 0; index < session.point_clouds_container.point_clouds.size(); index++)
                        session.point_clouds_container.point_clouds[index].m_pose =
                            m_gt * session.point_clouds_container.point_clouds[index].m_pose;
                }
            }
        }

        static bool open_import_popup = false;
        static bool show_instruction = false;

        ImGui::Dummy(ImVec2(0, 10));

        if (ImGui::Button("Import Benchmark Output Folders"))
        {
            open_import_popup = true;
            ImGui::OpenPopup("Import Benchmark");
        }

        if (ImGui::BeginPopupModal("Import Benchmark", NULL, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("Import benchmark sessions");
            ImGui::Separator();
            ImGui::SetNextWindowSize(ImVec2(740, 340), ImGuiCond_Once);
            if (ImGui::Button("Instruction"))
            {
                show_instruction = !show_instruction;
            }

            ImGui::Dummy(ImVec2(0, 10));
            if (ImGui::Button("Select Folder"))
            {
                const std::string algorithms[] = { "ct-icp",    "glim",      "super-lio", "dlio",
                                                   "i2ekf-lo",  "superOdom", "lego-loam", "faster-lio",
                                                   "kiss-icp",  "fast-lio",  "lio-ekf",   "genz-icp",
                                                   "point-lio", "ig-lio",    "dlo",       "lidar_odometry_ros_wrapper" };

                std::vector<fs::path> missing;
                std::unordered_map<std::string, std::string> algo_map;

                for (const auto& algo : algorithms)
                {
                    if (algo == "kiss-icp")
                    {
                        algo_map[algo] = "output_hdmapping-kiss";
                    }
                    else if (algo == "genz-icp")
                    {
                        algo_map[algo] = "output_hdmapping-genz";
                    }
                    else if (algo == "lidar_odometry_ros_wrapper")
                    {
                        algo_map[algo] = "output_hdmapping-lidar-odometry-ros";
                    }
                    else
                    {
                        algo_map[algo] = "output_hdmapping-" + algo;
                    }
                }

                fs::path path = fs::path(mandeye::fd::SelectFolder("Add sessions"));

                for (const auto& algo : algorithms)
                {
                    fs::path output_folder = path / algo / algo_map[algo];
                    fs::path session_file = output_folder / "session.json";

                    if (fs::is_directory(output_folder))
                    {
                        auto it =
                            std::find(project_settings.session_file_names.begin(), project_settings.session_file_names.end(), session_file);

                        if (it == project_settings.session_file_names.end())
                        {
                            std::cout << "Adding session file: '" << session_file << "'" << std::endl;
                            project_settings.session_file_names.push_back(session_file.string());
                        }
                    }
                    else
                    {
                        missing.push_back(output_folder);
                    }
                }

                for (const auto& miss : missing)
                {
                    std::cout << miss << " doesn't exist" << std::endl;
                }
            }

            ImGui::SameLine();

            if (ImGui::Button("Close"))
            {
                ImGui::CloseCurrentPopup();
                show_instruction = false;
            }

            if (show_instruction)
            {
                ImGui::BeginChild("InstructionChild", ImVec2(720, 300), true, ImGuiWindowFlags_HorizontalScrollbar);
                ImGui::Separator();

                ImGui::TextWrapped("Required folder structure:");

                ImGui::Spacing();

                ImGui::TextWrapped("Folders must follow the structure generated in benchmark-HDMapping-Orchestration (step 3):");
                ImGui::TextWrapped("https://github.com/MapsHD/benchmark-HDMapping-Orchestration");

                ImGui::Spacing();
                ImGui::Separator();

                ImGui::BulletText("chosen_folder/");
                ImGui::BulletText("  ct-icp/output_hdmapping-ct-icp/session.json");
                ImGui::BulletText("  glim/output_hdmapping-glim/session.json");
                ImGui::BulletText("  kiss-icp/output_hdmapping-kiss/session.json");
                ImGui::BulletText("  fast-lio/output_hdmapping-fast-lio/session.json");
                ImGui::BulletText("  ... (same pattern for other algorithms)");

                ImGui::Spacing();
                ImGui::EndChild();
            }

            ImGui::EndPopup();
        }
        if (project_settings.session_file_names.size() > 0)
        {
            ImGui::Separator();

            ImGui::Text("Sessions:");

            for (size_t i = 0; i < project_settings.session_file_names.size(); i++)
            {
                ImGui::Text(truncPath(project_settings.session_file_names[i]).c_str());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(project_settings.session_file_names[i].c_str());

                if (project_settings.session_file_names.size() == sessions.size())
                {
                    ImGui::BeginDisabled(is_loop_closure_gui);
                    {
                        ImGui::SameLine();
                        ImGui::Checkbox(("Visible##" + std::to_string(i)).c_str(), &sessions[i].visible);
                    }
                    ImGui::EndDisabled();

                    ImGui::SameLine();
                    if (ImGui::RadioButton(("Ground truth##" + std::to_string(i)).c_str(), &index_gt, i))
                        if (old_index_gt == i)
                            index_gt = -1; // unselect
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Select session as unmovable reference");

                    ImGui::BeginDisabled(!sessions[i].visible);
                    {
                        ImGui::BeginDisabled(sessions[i].is_ground_truth || is_loop_closure_gui);
                        {
                            ImGui::SameLine();
                            if (ImGui::RadioButton(("Gizmo##" + std::to_string(i)).c_str(), &index_gizmo, i))
                                if (old_index_gizmo == i)
                                    index_gizmo = -1; // unselect
                        }
                        ImGui::EndDisabled();

                        ImGui::SameLine();
                        ImGui::ColorEdit3(
                            ("Color##" + std::to_string(i)).c_str(), (float*)&sessions[i].render_color, ImGuiColorEditFlags_NoInputs);
                        for (auto& pc : sessions[i].point_clouds_container.point_clouds)
                        {
                            pc.traj_color[0] = sessions[i].render_color[0];
                            pc.traj_color[1] = sessions[i].render_color[1];
                            pc.traj_color[2] = sessions[i].render_color[2];
                            pc.render_color[0] = sessions[i].render_color[0];
                            pc.render_color[1] = sessions[i].render_color[1];
                            pc.render_color[2] = sessions[i].render_color[2];
                        }
                    }
                    ImGui::EndDisabled();

                    //
                    if (sessions[i].point_clouds_container.point_clouds.size() > 0)
                    {
                        if (sessions[i].point_clouds_container.point_clouds[0].local_trajectory.size() > 0)
                        {
                            if (sessions[i]
                                    .point_clouds_container.point_clouds[sessions[i].point_clouds_container.point_clouds.size() - 1]
                                    .local_trajectory.size() > 0)
                            {
                                ImGui::SameLine();

                                int index_last = sessions[i].point_clouds_container.point_clouds.size() - 1;
                                int index_last2 = sessions[i].point_clouds_container.point_clouds[index_last].local_trajectory.size() - 1;

                                ImGui::Text(
                                    "Timestamp range: <%.0f, %.0f>",
                                    sessions[i].point_clouds_container.point_clouds[0].local_trajectory[0].timestamps.first,
                                    sessions[i]
                                        .point_clouds_container.point_clouds[index_last]
                                        .local_trajectory[index_last2]
                                        .timestamps.first);
                            }
                        }
                    }
                }
            }

            if (project_settings.session_file_names.size() == sessions.size())
            {
                if ((old_index_gt != index_gt) || (old_index_gizmo != index_gizmo))
                {
                    if (old_index_gt != index_gt)
                    {
                        SetProjectGroundTruth(sessions, index_gt);
                        markProjectChanged();
                    }
                    for (size_t i = 0; i < sessions.size(); i++)
                        sessions[i].is_gizmo = (i == index_gizmo);

                    // set rotation center to gizmo
                    if (index_gizmo >= 0 && index_gizmo < sessions.size() &&
                        sessions[index_gizmo].point_clouds_container.point_clouds.size() > 0)
                    {
                        setNewRotationCenter(sessions[index_gizmo].point_clouds_container.point_clouds[0].m_pose.translation());
                    }
                    old_index_gt = index_gt;
                    old_index_gizmo = index_gizmo;
                }

                // Skipped while index_gizmo's gizmo is mid-drag: that session's m_pose is
                // deliberately left untouched until the drag ends (see session_drag_preview_poses),
                // so resetting m_gizmo from it here every frame would feed ImGuizmo::Manipulate()
                // the frozen pre-drag pose instead of its own last output, breaking the drag.
                if (index_gizmo != -1 && index_gizmo < sessions.size() &&
                    session_drag_preview_poses.find(static_cast<size_t>(index_gizmo)) == session_drag_preview_poses.end())
                {
                    HDMAPPING_ASSERT(index_gizmo < sessions.size());
                    HDMAPPING_ASSERT(!sessions[index_gizmo].point_clouds_container.point_clouds.empty());
                    // sessions[index_gizmo].is_gizmo = true;
                    // Column-major 4x4, matching the Eigen::Map<const Eigen::Matrix4f>(m_gizmo)
                    // reads elsewhere in this file -- this is just that assignment reversed.
                    Eigen::Map<Eigen::Matrix4f> gizmo_map(m_gizmo);
                    gizmo_map = sessions[index_gizmo].point_clouds_container.point_clouds[0].m_pose.matrix().cast<float>();
                }
            }

            ImGui::BeginDisabled((project_settings.session_file_names.size() < 2) || (index_gizmo == -1));
            {
                ImGui::Checkbox("Gizmo all sessions", &gizmo_all_sessions);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Gizmo will move all sessions except ground truth one");
            }
            ImGui::EndDisabled();

            ImGui::Separator();

            ImGui::NewLine();

            refreshVisibleSessions();
        }
    }

    ImGui::End();
}

#ifdef HDMAPPING_GUI_BRIDGE
namespace
{
    json projectParameters()
    {
        auto selection = json::array();
        for (size_t i = 0; i < sessions.size(); ++i)
            if (sessions[i].visible)
                selection.push_back(i);
        return { { "visible_sessions", std::move(selection) },
                 { "sections",
                   { { "xz", gui_xz_intersection },
                     { "yz", gui_yz_intersection },
                     { "xy", gui_xy_intersection },
                     { "width", gui_intersection_width } } },
                 { "graph_iterations", nr_iter },
                 { "ndt_threads", ndt.number_of_threads },
                 { "ndt_iterations", ndt.number_of_iterations },
                 { "ndt_bucket_size", { ndt.bucket_size[0], ndt.bucket_size[1], ndt.bucket_size[2] } },
                 { "icp_search_radius", search_radius },
                 { "decimate", is_decimate },
                 { "load_buckets", { bucket_x, bucket_y, bucket_z } },
                 { "time_offset", time_stamp_offset },
                 { "motion_weights",
                   { motion_model_weights.px,
                     motion_model_weights.py,
                     motion_model_weights.pz,
                     motion_model_weights.om,
                     motion_model_weights.fi,
                     motion_model_weights.ka } },
                 { "edge_range",
                   { index_loop_closure_source, index_loop_closure_target, num_edge_extended_before, num_edge_extended_after } },
                 { "active_edge", index_active_edge },
                 { "ground_truth", index_gt } };
    }

    proc::StateSummary describeProject()
    {
        auto children = json::array();
        for (std::size_t i = 0; i < sessions.size(); ++i)
            children.push_back(
                { { "index", i },
                  { "ground_truth", sessions[i].is_ground_truth },
                  { "visible", sessions[i].visible },
                  { "scans", sessions[i].point_clouds_container.point_clouds.size() } });
        return { {},
                 projectParameters(),
                 { { "step", 3 },
                   { "path", project_file_name },
                   { "sessions", std::move(children) },
                   { "edges", edges.size() },
                   { "active_edges", proc::ProjectEdgesJson(edges) } } };
    }

    proc::MutationResult mutateProject(const proc::OperationRequest& request)
    {
        if (request.operation == "project.remove_session" || request.operation == "project.set_ground_truth")
        {
            if (!session_drag_preview_poses.empty())
                return proc::OperationError{ "state_busy", "Finish the active pose edit before editing the project", json::object() };
            try
            {
                proc::EditProject(request, project_settings, sessions);
                if (request.operation == "project.remove_session")
                    refreshProjectSessions();
                else
                    index_gt = old_index_gt = request.scope.at("session_index").get<int>();
                return describeProject();
            } catch (const std::exception& error)
            {
                return proc::OperationError{ "invalid_parameters", error.what(), json::object() };
            }
        }
        if (proc::IsStep3Edit(request.operation))
        {
            if (!session_drag_preview_poses.empty() || (edge_gizmo && ImGuizmo::IsUsing()))
                return proc::OperationError{ "state_busy", "Finish the active pose edit before editing the project", json::object() };
            try
            {
                proc::EditStep3(request, project_settings, sessions);
                if (request.operation == "project.constraints.remove" || request.operation == "project.constraints.replace" ||
                    (index_active_edge >= 0 && !canEdgeBeActivated(index_active_edge)))
                    resetProjectEdgeSelection();
                else if (index_active_edge >= 0 && manipulate_active_edge)
                    setActiveEdge(index_active_edge);
                return describeProject();
            } catch (const std::exception& error)
            {
                return proc::OperationError{ "invalid_parameters", error.what(), json::object() };
            }
        }
        if (request.operation != "step3.set_parameters" || !request.scope.empty() || request.parameters.empty())
            return proc::OperationError{ "invalid_request", "Expected project parameters and an empty scope", json::object() };
        auto iterations = ndt.number_of_iterations;
        auto threads = ndt.number_of_threads;
        auto radius = search_radius;
        std::array<float, 3> buckets{ ndt.bucket_size[0], ndt.bucket_size[1], ndt.bucket_size[2] };
        for (const auto& [key, value] : request.parameters.items())
        {
            if ((key == "ndt_threads" || key == "ndt_iterations") && value.is_number_integer() && value >= 1 &&
                value <= std::numeric_limits<int>::max())
                (key == "ndt_threads" ? threads : iterations) = value.get<int>();
            else if (key == "icp_search_radius" && value.is_number() && value >= 0.01 && std::isfinite(value.get<double>()))
                radius = value.get<double>();
            else if (key == "ndt_bucket_size" && value.is_array() && value.size() == 3)
            {
                for (size_t i = 0; i < 3; ++i)
                {
                    if (!value[i].is_number() || value[i] < 0.01 || value[i] > std::numeric_limits<float>::max())
                        return proc::OperationError{ "invalid_parameters", "Invalid NDT bucket size", json::object() };
                    buckets[i] = value[i].get<float>();
                }
            }
            else
                return proc::OperationError{ "invalid_parameters", "Invalid project parameter: " + key, json::object() };
        }
        ndt.number_of_iterations = iterations;
        ndt.number_of_threads = threads;
        search_radius = radius;
        std::copy(buckets.begin(), buckets.end(), ndt.bucket_size);
        return describeProject();
    }

    json projectJob = json::object();
    bool projectJobPending = false;
    std::string projectJobError;

    bool projectJobActive()
    {
        const auto status = projectJob.value("status", std::string{});
        return projectJobPending || status == "preparing" || status == "queued" || status == "running" || status == "cancelling" ||
            projectJob.value("gui_apply_status", std::string{}) == "pending";
    }

    void submitProjectJob(const proc::OperationRequest& request, bool mutate)
    {
        if (projectJobActive())
            return;
        static std::uint64_t sequence = 0;
        const auto target = guiState->Target();
        projectJob = json::object();
        projectJobError.clear();
        projectJobPending = guiConnection->RequestJob(
            "jobs.submit",
            { { "request_key", target.state_id + "-step3-" + std::to_string(++sequence) },
              { "operation",
                { { "operation", request.operation },
                  { "target", { { "state_id", target.state_id }, { "revision", target.revision } } },
                  { "scope", request.scope },
                  { "parameters", request.parameters } } },
              { "capture_source", target.state_id },
              { "source", "gui" },
              { "access", mutate ? "mutate" : "read" } });
        if (!projectJobPending)
            projectJobError = "Could not submit project job";
    }

    void pollProjectJob()
    {
        if (auto reply = guiConnection->TakeJobReply())
        {
            projectJobPending = false;
            if (const auto* error = std::get_if<proc::OperationError>(&*reply))
                projectJobError = error->message;
            else
            {
                projectJobError.clear();
                projectJob = std::get<json>(std::move(*reply));
            }
        }
        if (!projectJobPending && projectJobActive())
        {
            static auto next_poll = std::chrono::steady_clock::now();
            if (std::chrono::steady_clock::now() >= next_poll)
            {
                projectJobPending = guiConnection->RequestJob("jobs.get", projectJob.at("job_id"));
                next_poll = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
            }
        }
    }

    void drawProjectJob()
    {
        if (projectJob.empty() && !projectJobPending && projectJobError.empty())
            return;
        ImGui::Begin("Step 3 processing", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
        const auto status = projectJob.value("status", std::string{});
        const auto id = projectJob.value("job_id", std::string{});
        ImGui::TextUnformatted(status.empty() ? (projectJobPending ? "Submitting" : "Failed") : status.c_str());
        if (!id.empty())
            ImGui::Text("Job %s", id.c_str());
        if (projectJob.contains("progress") && projectJob.at("progress").is_object())
        {
            const auto& progress = projectJob.at("progress");
            const auto stage = progress.value("stage", std::string{});
            if (!stage.empty())
                ImGui::TextUnformatted(stage.c_str());
        }
        const auto apply = projectJob.value("gui_apply_status", std::string{});
        if (!apply.empty() && apply != "not_requested")
            ImGui::Text("Workspace update: %s", apply.c_str());
        if (projectJob.contains("error"))
            ImGui::TextWrapped("%s", projectJob.at("error").value("message", "").c_str());
        if (!projectJobError.empty())
            ImGui::TextWrapped("%s", projectJobError.c_str());
        if (status == "completed" && projectJob.contains("result"))
        {
            const auto& snapshot = projectJob.at("result").at("snapshot");
            const auto manifest = sessionPathFromText(snapshot.at("manifest_path").get<std::string>());
            const auto operation = projectJob.at("request").at("operation").at("operation");
            const auto path = operation == proc::project_export_operation ? manifest.parent_path() : manifest.parent_path() / "project.mjp";
            ImGui::TextWrapped("%s", sessionPathText(path).c_str());
        }
        if (!projectJobPending && (status == "preparing" || status == "queued" || status == "running") && ImGui::Button("Cancel job"))
            projectJobPending = guiConnection->RequestJob("jobs.cancel", id);
        if (!projectJobActive() && ImGui::Button("Dismiss"))
        {
            projectJob = json::object();
            projectJobError.clear();
        }
        ImGui::End();
    }

    bool sameProjectSessions(const proc::ProjectState& project)
    {
        if (sessions.size() != project.sessions.size())
            return false;
        for (std::size_t i = 0; i < sessions.size(); ++i)
        {
            const auto& name = sessions[i].session_file_name;
            const auto source = projectSources.find(name);
            const auto new_source = project.sources.find(project.sessions[i].session_file_name);
            if (new_source == project.sources.end() || new_source->second != (source == projectSources.end() ? name : source->second))
                return false;
        }
        return true;
    }

    std::array<int, 4> projectEdgeEndpoints(const Edge& edge)
    {
        return { edge.index_session_from, edge.index_from, edge.index_session_to, edge.index_to };
    }

    void connectProject()
    {
        guiState = std::make_unique<proc::GuiStateBridge>(proc::GuiStateCallbacks{
            describeProject,
            mutateProject,
            {},
            [](const proc::ArtifactManifest& artifact) -> std::optional<proc::OperationError>
            {
                auto restored = proc::LoadProjectSnapshot(artifact.snapshot);
                if (const auto* error = std::get_if<proc::OperationError>(&restored))
                    return *error;
                auto& project = std::get<proc::ProjectState>(restored);
                const bool keep_loop_editor = is_loop_closure_gui && sameProjectSessions(project);
                const int previous_edge = index_active_edge;
                std::optional<std::array<int, 4>> previous_endpoints;
                if (keep_loop_editor && canEdgeBeActivated(previous_edge))
                    previous_endpoints = projectEdgeEndpoints(edges[previous_edge]);
                const bool previous_manipulation = manipulate_active_edge;
                const bool previous_gizmo = edge_gizmo;
                project_settings = std::move(project.settings);
                sessions = std::move(project.sessions);
                projectSources = std::move(project.sources);
                loaded_sessions = true;
                refreshProjectSessions();
                if (keep_loop_editor && (number_visible_sessions == 1 || number_visible_sessions == 2))
                {
                    is_loop_closure_gui = true;
                    if (previous_endpoints && canEdgeBeActivated(previous_edge) &&
                        *previous_endpoints == projectEdgeEndpoints(edges[previous_edge]))
                    {
                        manipulate_active_edge = previous_manipulation;
                        edge_gizmo = previous_gizmo;
                        setActiveEdge(previous_edge);
                    }
                }
                updateProjectTitle(sessionPathText(artifact.snapshot.manifest_path.parent_path() / "project.mjp"));
                return std::nullopt;
            },
            [](const proc::OperationRequest& request) -> std::unique_ptr<proc::CaptureSource>
            {
                if (request.operation == "project.open")
                {
                    static const std::vector<Session> empty;
                    return std::make_unique<ProjectCapture>(request.target, empty, json::object(), json::object());
                }
                if (!proc::IsProjectOperation(request.operation) || sessions.size() != project_settings.session_file_names.size() ||
                    !session_drag_preview_poses.empty() || (edge_gizmo && ImGuizmo::IsUsing()))
                    return {};
                return std::make_unique<ProjectCapture>(
                    request.target, sessions, ProjectSettingsJson(project_settings), projectParameters(), false, projectSources);
            },
            {},
            [](const json& scope)
            {
                return proc::ProjectItemsPage(sessions, edges, scope);
            } });
        guiConnection = std::make_unique<proc::GuiStateConnection>(*guiState, "multi_session_registration_step_3");
    }
} // namespace
#endif

void display()
{
#ifdef HDMAPPING_GUI_BRIDGE
    static std::optional<json> previous;
    const auto observed = []
    {
        return json::array({ projectParameters(), project_settings.session_file_names, edges.size(), loaded_sessions });
    };
    const auto current = observed();
    if (previous && *previous != current)
        guiState->Changed();
    const auto before = guiState->Target();
    guiConnection->Poll();
    pollProjectJob();
    previous = before == guiState->Target() ? current : observed();
#endif
    syncSessionRenderers(sessions);

    ImGuiIO& io = ImGui::GetIO();
    rlViewport(0, 0, GetRenderWidth(), GetRenderHeight());

    ClearBackground(ColorFromNormalized(Vector4{ bg_color.x * bg_color.w, bg_color.y * bg_color.w, bg_color.z * bg_color.w, bg_color.w }));
    rlEnableDepthTest();

    rlMatrixMode(RL_PROJECTION);
    rlLoadIdentity();
    float ratio = float(io.DisplaySize.x) / float(io.DisplaySize.y);

    updateCameraTransition();

    viewLocal = Eigen::Affine3f::Identity();

    if (!is_ortho)
    {
        reshape((int)io.DisplaySize.x, (int)io.DisplaySize.y);

        if (is_loop_closure_gui)
        {
            // sessions[first_session_index].point_clouds_container.point_clouds.at(index_loop_closure_source).render(false,
            // observation_picking, viewer_decmiate_point_cloud, false, false, false, false, false, false, false, false, false, false,
            // false, false, 100000);
            // sessions[second_session_index].point_clouds_container.point_clouds.at(index_loop_closure_target).render(false,
            // observation_picking, viewer_decmiate_point_cloud, false, false, false, false, false, false, false, false, false, false,
            // false, false, 100000);

            if (index_loop_closure_source >= 0 &&
                index_loop_closure_source < (int)sessions[first_session_index].point_clouds_container.point_clouds.size())
            {
                if (update_rotation_center)
                {
                    rotation_center.x() = sessions[first_session_index]
                                              .point_clouds_container.point_clouds[index_loop_closure_source]
                                              .m_pose.translation()
                                              .x();
                    rotation_center.y() = sessions[first_session_index]
                                              .point_clouds_container.point_clouds[index_loop_closure_source]
                                              .m_pose.translation()
                                              .y();
                    rotation_center.z() = sessions[first_session_index]
                                              .point_clouds_container.point_clouds[index_loop_closure_source]
                                              .m_pose.translation()
                                              .z();
                }
            }

            if (manipulate_active_edge)
            {
                if (edges.size() > 0)
                {
                    int index_src = edges[index_active_edge].index_from;
                    Eigen::Affine3d m_src =
                        sessions[edges[index_active_edge].index_session_from].point_clouds_container.point_clouds.at(index_src).m_pose;

                    if (update_rotation_center)
                    {
                        rotation_center.x() = m_src(0, 3);
                        rotation_center.y() = m_src(1, 3);
                        rotation_center.z() = m_src(2, 3);
                    }
                }
            }

            /*if (session.pose_graph_loop_closure.manipulate_active_edge)
            {
                if (session.pose_graph_loop_closure.edges.size() > 0)
                {
                    if (session.pose_graph_loop_closure.index_active_edge < session.pose_graph_loop_closure.edges.size())
                    {
                        rotation_center.x() =
            session.point_clouds_container.point_clouds[session.pose_graph_loop_closure.edges[session.pose_graph_loop_closure.index_active_edge].index_from].m_pose(0,
            3); rotation_center.y() =
            session.point_clouds_container.point_clouds[session.pose_graph_loop_closure.edges[session.pose_graph_loop_closure.index_active_edge].index_from].m_pose(1,
            3); rotation_center.z() =
            session.point_clouds_container.point_clouds[session.pose_graph_loop_closure.edges[session.pose_graph_loop_closure.index_active_edge].index_from].m_pose(2,
            3);
                    }
                }
            }*/
        }

        viewLocal.translate(rotation_center);

        viewLocal.translate(Eigen::Vector3f(translate_x, translate_y, translate_z));
        if (!lock_z)
            viewLocal.rotate(Eigen::AngleAxisf(rotate_x * DEG_TO_RAD, Eigen::Vector3f::UnitX()));
        else
            viewLocal.rotate(Eigen::AngleAxisf(-90.0 * DEG_TO_RAD, Eigen::Vector3f::UnitX()));
        viewLocal.rotate(Eigen::AngleAxisf(rotate_y * DEG_TO_RAD, Eigen::Vector3f::UnitZ()));

        viewLocal.translate(-rotation_center);

        rlMultMatrixf(viewLocal.matrix().data());
    }
    else
        updateOrthoView();
    captureFrameMatrices();

    showAxes();
    drawIntersectionGrids(sessions);

    if (is_loop_closure_gui)
    {
        if (manipulate_active_edge)
        {
            if (edges.size() > 0)
            {
                /*int index_src = edges[index_active_edge].index_from;
                int index_trg = edges[index_active_edge].index_to;

                Eigen::Affine3d m_src =
                    sessions[edges[index_active_edge].index_session_from].point_clouds_container.point_clouds.at(index_src).m_pose;
                Eigen::Affine3d m_trg = m_src * affine_matrix_from_pose_tait_bryan(edges[index_active_edge].relative_pose_tb);

                sessions[edges[index_active_edge].index_session_from].point_clouds_container.point_clouds.at(index_src).render(
                    m_src,
                    viewer_decimate_point_cloud,
                    viewer_reduce_rendered_trajectory,
                    sessions[edges[index_active_edge].index_session_from].point_clouds_container.point_clouds.at(index_src).render_color);
                sessions[edges[index_active_edge].index_session_to].point_clouds_container.point_clouds.at(index_trg).render(
                    m_trg,
                    viewer_decimate_point_cloud,
                    viewer_reduce_rendered_trajectory,
                    sessions[edges[index_active_edge].index_session_to].point_clouds_container.point_clouds.at(index_trg).render_color);*/

                int index_src = edges[index_active_edge].index_from;
                int index_trg = edges[index_active_edge].index_to;

                // The active edge's own sessions, not the first/second_session_index globals: those
                // are re-synced every frame by settings_gui()'s visibility loop for the "pick two
                // sessions for a new edge" UI, independent of which edge is active here, so they can
                // (and do) drift from this edge's actual sessions and index into the wrong one.
                const int active_session_from = edges[index_active_edge].index_session_from;
                const int active_session_to = edges[index_active_edge].index_session_to;

                Eigen::Affine3d _m_src = sessions[active_session_from].point_clouds_container.point_clouds.at(index_src).m_pose;
                Eigen::Affine3d _m_trg = _m_src * affine_matrix_from_pose_tait_bryan(edges[index_active_edge].relative_pose_tb);

                Eigen::Affine3d m_src_0 =
                    sessions[active_session_from].point_clouds_container.point_clouds.at(index_loop_closure_source).m_pose;

                for (int i = index_loop_closure_source - num_edge_extended_before; i <= index_loop_closure_source + num_edge_extended_after;
                     i++)
                {
                    if (i >= 0 && i < sessions[active_session_from].point_clouds_container.point_clouds.size() &&
                        sessions[active_session_from].point_clouds_container.point_clouds.size() > 0)
                    {
                        // ObservationPicking observation_picking;
                        // point_clouds_container.point_clouds.at(i).render(false, observation_picking, 1, 1, false, false, false, 10000,
                        // false);

                        Eigen::Affine3d m_src_curr = sessions[active_session_from].point_clouds_container.point_clouds.at(i).m_pose;
                        Eigen::Affine3d m_src = _m_src * (m_src_0.inverse() * m_src_curr);

                        renderScanAtPose(
                            active_session_from,
                            i,
                            m_src,
                            viewer_decimate_point_cloud,
                            viewer_reduce_rendered_trajectory,
                            sessions[active_session_from].point_clouds_container.point_clouds.at(i).render_color,
                            /*useSceneColorMode=*/false,
                            sessions[active_session_from].point_clouds_container.xz_intersection,
                            sessions[active_session_from].point_clouds_container.yz_intersection,
                            sessions[active_session_from].point_clouds_container.xy_intersection,
                            sessions[active_session_from].point_clouds_container.intersection_width);
                    }
                }

                Eigen::Affine3d m_trg_0 =
                    sessions[active_session_to].point_clouds_container.point_clouds.at(index_loop_closure_target).m_pose;

                for (int i = index_loop_closure_target - num_edge_extended_before; i <= index_loop_closure_target + num_edge_extended_after;
                     i++)
                {
                    if (i >= 0 && i < sessions[active_session_to].point_clouds_container.point_clouds.size() &&
                        sessions[active_session_to].point_clouds_container.point_clouds.size() > 0)
                    {
                        // ObservationPicking observation_picking;
                        // point_clouds_container.point_clouds.at(i).render(false, observation_picking, 1, 1, false, false, false, 10000,
                        // false);
                        Eigen::Affine3d m_trg_curr = sessions[active_session_to].point_clouds_container.point_clouds.at(i).m_pose;
                        Eigen::Affine3d m_trg = _m_trg * (m_trg_0.inverse() * m_trg_curr);

                        renderScanAtPose(
                            active_session_to,
                            i,
                            m_trg,
                            viewer_decimate_point_cloud,
                            viewer_reduce_rendered_trajectory,
                            sessions[active_session_to].point_clouds_container.point_clouds.at(i).render_color,
                            /*useSceneColorMode=*/false,
                            sessions[active_session_to].point_clouds_container.xz_intersection,
                            sessions[active_session_to].point_clouds_container.yz_intersection,
                            sessions[active_session_to].point_clouds_container.xy_intersection,
                            sessions[active_session_to].point_clouds_container.intersection_width);
                    }
                }
            }
        }
        else
        {
            ObservationPicking observation_picking;

            /*sessions[first_session_index]
                .point_clouds_container.point_clouds.at(index_loop_closure_source)
                .render(
                    false,
                    observation_picking,
                    viewer_decimate_point_cloud,
                    viewer_reduce_rendered_trajectory,
                    false,
                    false,
                    false,
                    100000,
                    false);*/

            for (int i = index_loop_closure_source - num_edge_extended_before; i <= index_loop_closure_source + num_edge_extended_after;
                 i++)
            {
                if (i >= 0 && i < sessions[first_session_index].point_clouds_container.point_clouds.size() &&
                    sessions[first_session_index].point_clouds_container.point_clouds.size() > 0)
                {
                    // ObservationPicking observation_picking;
                    // point_clouds_container.point_clouds.at(i).render(false, observation_picking, 1, 1, false, false, false, 10000,
                    // false);
                    Eigen::Affine3d m_src = sessions[first_session_index].point_clouds_container.point_clouds.at(i).m_pose;

                    renderScan(
                        first_session_index,
                        i,
                        false,
                        observation_picking,
                        viewer_decimate_point_cloud,
                        viewer_reduce_rendered_trajectory,
                        sessions[first_session_index].point_clouds_container.xz_intersection,
                        sessions[first_session_index].point_clouds_container.yz_intersection,
                        sessions[first_session_index].point_clouds_container.xy_intersection,
                        sessions[first_session_index].point_clouds_container.intersection_width,
                        false);
                }
            }

            /*sessions[second_session_index]
                .point_clouds_container.point_clouds.at(index_loop_closure_target)
                .render(
                    false,
                    observation_picking,
                    viewer_decimate_point_cloud,
                    viewer_reduce_rendered_trajectory,
                    false,
                    false,
                    false,
                    100000,
                    false);*/

            for (int i = index_loop_closure_target - num_edge_extended_before; i <= index_loop_closure_target + num_edge_extended_after;
                 i++)
            {
                if (i >= 0 && i < sessions[second_session_index].point_clouds_container.point_clouds.size() &&
                    sessions[second_session_index].point_clouds_container.point_clouds.size() > 0)
                {
                    // ObservationPicking observation_picking;
                    // point_clouds_container.point_clouds.at(i).render(false, observation_picking, 1, 1, false, false, false, 10000,
                    // false);
                    Eigen::Affine3d m_src = sessions[second_session_index].point_clouds_container.point_clouds.at(i).m_pose;

                    renderScan(
                        second_session_index,
                        i,
                        false,
                        observation_picking,
                        viewer_decimate_point_cloud,
                        viewer_reduce_rendered_trajectory,
                        sessions[second_session_index].point_clouds_container.xz_intersection,
                        sessions[second_session_index].point_clouds_container.yz_intersection,
                        sessions[second_session_index].point_clouds_container.xy_intersection,
                        sessions[second_session_index].point_clouds_container.intersection_width,
                        false);
                }
            }
        }

        // sessions[first_session_index].point_clouds_container.render();

        beginLineStrip();
        for (auto& pc : sessions[first_session_index].point_clouds_container.point_clouds)
        {
            color3f(pc.render_color[0], pc.render_color[1], pc.render_color[2]);
            lineStripVertex3f(pc.m_pose(0, 3), pc.m_pose(1, 3), pc.m_pose(2, 3));
        }
        endLineStrip();

        int i = 0;
        for (auto& pc : sessions[first_session_index].point_clouds_container.point_clouds)
        {
            color3f(pc.render_color[0], pc.render_color[1], pc.render_color[2]);
            labelPos3f(pc.m_pose(0, 3), pc.m_pose(1, 3), pc.m_pose(2, 3) + 0.1);
            labelText(std::to_string(i).c_str());
            i++;
        }

        beginLineStrip();
        for (auto& pc : sessions[second_session_index].point_clouds_container.point_clouds)
        {
            color3f(pc.render_color[0], pc.render_color[1], pc.render_color[2]);
            lineStripVertex3f(pc.m_pose(0, 3), pc.m_pose(1, 3), pc.m_pose(2, 3));
        }
        endLineStrip();

        i = 0;
        for (auto& pc : sessions[second_session_index].point_clouds_container.point_clouds)
        {
            color3f(pc.render_color[0], pc.render_color[1], pc.render_color[2]);
            labelPos3f(pc.m_pose(0, 3), pc.m_pose(1, 3), pc.m_pose(2, 3) + 0.1);
            labelText(std::to_string(i).c_str());
            i++;
        }

        for (size_t i = 0; i < sessions.size(); i++)
        {
            for (size_t j = 0; j < sessions[i].pose_graph_loop_closure.edges.size(); j++)
            {
                int index_src = sessions[i].pose_graph_loop_closure.edges[j].index_from;
                int index_trg = sessions[i].pose_graph_loop_closure.edges[j].index_to;

                color3f(0.0f, 0.0f, 1.0f);
                rlBegin(RL_LINES);
                auto v1 = sessions[i].point_clouds_container.point_clouds.at(index_src).m_pose.translation();
                auto v2 = sessions[i].point_clouds_container.point_clouds.at(index_trg).m_pose.translation();
                rlVertex3f(v1.x(), v1.y(), v1.z());
                rlVertex3f(v2.x(), v2.y(), v2.z());

                rlVertex3f((v1.x() + v2.x()) * 0.5, (v1.y() + v2.y()) * 0.5, (v1.z() + v2.z()) * 0.5);
                rlVertex3f((v1.x() + v2.x()) * 0.5, (v1.y() + v2.y()) * 0.5, (v1.z() + v2.z()) * 0.5 + 10);
                rlEnd();

                labelPos3f((v1.x() + v2.x()) * 0.5, (v1.y() + v2.y()) * 0.5, (v1.z() + v2.z()) * 0.5 + 10 + 0.1);
                labelText(std::to_string(j).c_str());
            }
        }

        for (size_t i = 0; i < edges.size(); i++)
        {
            int index_src = edges[i].index_from;
            int index_trg = edges[i].index_to;

            int index_session_from = edges[i].index_session_from;
            int index_session_to = edges[i].index_session_to;

            if (sessions[index_session_from].is_ground_truth || sessions[index_session_to].is_ground_truth)
                color3f(0.0f, 1.0f, 1.0f);
            else
                color3f(1.0f, 1.0f, 0.0f);

            rlBegin(RL_LINES);
            auto v1 = sessions[index_session_from].point_clouds_container.point_clouds.at(index_src).m_pose.translation();
            auto v2 = sessions[index_session_to].point_clouds_container.point_clouds.at(index_trg).m_pose.translation();
            rlVertex3f(v1.x(), v1.y(), v1.z());
            rlVertex3f(v2.x(), v2.y(), v2.z());

            rlVertex3f((v1.x() + v2.x()) * 0.5, (v1.y() + v2.y()) * 0.5, (v1.z() + v2.z()) * 0.5);
            rlVertex3f((v1.x() + v2.x()) * 0.5, (v1.y() + v2.y()) * 0.5, (v1.z() + v2.z()) * 0.5 + 10);
            rlEnd();

            labelPos3f((v1.x() + v2.x()) * 0.5, (v1.y() + v2.y()) * 0.5, (v1.z() + v2.z()) * 0.5 + 10 + 0.1);
            labelText(std::to_string(i).c_str());
        }
    }
    else
    {
        for (auto& session : sessions)
        {
            if (session.visible)
            {
                const size_t session_idx = static_cast<size_t>(&session - sessions.data());
                const auto preview_it = session_drag_preview_poses.find(session_idx);
                if (preview_it != session_drag_preview_poses.end())
                {
                    // This session's gizmo is actively being dragged this frame: draw straight
                    // from its cached GPU buffers with the in-progress pose, no re-upload.
                    const auto& preview_poses = preview_it->second;
                    for (size_t j = 0; j < session.point_clouds_container.point_clouds.size() && j < preview_poses.size(); j++)
                    {
                        renderScanAtPose(
                            static_cast<int>(session_idx),
                            static_cast<int>(j),
                            preview_poses[j],
                            viewer_decimate_point_cloud,
                            viewer_reduce_rendered_trajectory,
                            session.point_clouds_container.point_clouds[j].render_color,
                            /*useSceneColorMode=*/true,
                            session.point_clouds_container.xz_intersection,
                            session.point_clouds_container.yz_intersection,
                            session.point_clouds_container.xy_intersection,
                            session.point_clouds_container.intersection_width);
                    }
                }
                else
                {
                    renderSession(session, observation_picking, viewer_decimate_point_cloud, viewer_reduce_rendered_trajectory);
                }
                renderGroundControlPoints(session.ground_control_points, session.point_clouds_container);
                renderControlPoints(session.control_points, session.point_clouds_container);

                ////
                int index_point_clouds = -1;
                int index_local_trajectory = -1;
                bool found = false;
                for (size_t a = 0; a < session.point_clouds_container.point_clouds.size(); a++)
                {
                    for (size_t b = 0; b < session.point_clouds_container.point_clouds[a].local_trajectory.size(); b++)
                    {
                        if (session.point_clouds_container.point_clouds[a].local_trajectory[b].timestamps.first > time_stamp_offset)
                        {
                            if (!found)
                            {
                                found = true;
                                index_point_clouds = a;
                                index_local_trajectory = b;
                                break;
                            }
                        }
                    }
                }

                if (index_point_clouds != -1 && index_local_trajectory != -1)
                {
                    if (index_local_trajectory < session.point_clouds_container.point_clouds[index_point_clouds].local_trajectory.size())
                    {
                        color3f(
                            session.point_clouds_container.point_clouds[index_point_clouds].render_color[0],
                            session.point_clouds_container.point_clouds[index_point_clouds].render_color[1],
                            session.point_clouds_container.point_clouds[index_point_clouds].render_color[2]);
                        rlBegin(RL_LINES);

                        auto m1 = session.point_clouds_container.point_clouds[index_point_clouds].m_pose;
                        auto m2 =
                            session.point_clouds_container.point_clouds[index_point_clouds].local_trajectory[index_local_trajectory].m_pose;

                        auto v1 = (m1 * m2).translation();

                        rlVertex3f(v1.x() - 5.0, v1.y(), v1.z());
                        rlVertex3f(v1.x() + 5.0, v1.y(), v1.z());

                        rlVertex3f(v1.x(), v1.y() - 5.0, v1.z());
                        rlVertex3f(v1.x(), v1.y() + 5.0, v1.z());

                        rlVertex3f(v1.x(), v1.y(), v1.z() - 5.0);
                        rlVertex3f(v1.x(), v1.y(), v1.z() + 5.0);

                        rlEnd();
                    }
                }
            }
        }
    }

    rlImGuiBegin();

    ShowMainDockSpace();
#ifdef HDMAPPING_GUI_BRIDGE
    drawProjectJob();
    if (guiState->Reserved())
    {
        if (IsFileDropped())
        {
            auto dropped = LoadDroppedFiles();
            UnloadDroppedFiles(dropped);
        }
        view_kbd_shortcuts();
        ImGui::TextUnformatted("Processing data reserved");
        end3DAndDrawLabels();
        if (compass_ruler)
            drawMiniCompassWithRuler();
        rlImGuiEnd();
        return;
    }
#endif

    if (!is_loop_closure_gui)
    {
        Eigen::Affine3d prev_pose_manipulated = Eigen::Affine3d::Identity();
        Eigen::Affine3d prev_pose_after_gismo = Eigen::Affine3d::Identity();
        bool gizmo_dragging_this_frame = false;

        // Commits `candidate_poses` into session i's point clouds for real: writes m_pose,
        // pose and the gui_translation/gui_rotation mirrors. This is the only place that
        // mutates the session data model, so it's the only frame syncSessionRenderers()
        // sees a pose change and rebuilds that session's GPU buffers.
        auto commit_session_poses = [](size_t i, const std::vector<Eigen::Affine3d>& candidate_poses)
        {
            markProjectChanged();
            for (size_t j = 0; j < candidate_poses.size(); j++)
            {
                auto& pc = sessions[i].point_clouds_container.point_clouds[j];
                pc.m_pose = candidate_poses[j];
                pc.pose = pose_tait_bryan_from_affine_matrix(pc.m_pose);
                pc.gui_translation[0] = (float)pc.pose.px;
                pc.gui_translation[1] = (float)pc.pose.py;
                pc.gui_translation[2] = (float)pc.pose.pz;
                pc.gui_rotation[0] = (float)(pc.pose.om * RAD_TO_DEG);
                pc.gui_rotation[1] = (float)(pc.pose.fi * RAD_TO_DEG);
                pc.gui_rotation[2] = (float)(pc.pose.ka * RAD_TO_DEG);
            }
        };

        for (size_t i = 0; i < sessions.size(); i++)
        {
            // guizmo_all_sessions;
            if (sessions[i].is_gizmo && !sessions[i].is_ground_truth)
            {
                if (sessions[i].point_clouds_container.point_clouds.size() > 0)
                {
                    prev_pose_manipulated = sessions[i].point_clouds_container.point_clouds[0].m_pose;
                    std::vector<Eigen::Affine3d> all_m_poses;
                    for (size_t j = 0; j < sessions[i].point_clouds_container.point_clouds.size(); j++)
                        all_m_poses.push_back(sessions[i].point_clouds_container.point_clouds[j].m_pose);

                    ImGuiIO& io = ImGui::GetIO();

                    ImGuizmo::BeginFrame();
                    ImGuizmo::Enable(true);
                    ImGuizmo::SetRect(0, 0, io.DisplaySize.x, io.DisplaySize.y);

                    if (!is_ortho)
                    {
                        float projection[16];
                        getProjectionMatrix(projection);

                        float modelview[16];
                        getModelviewMatrix(modelview);

                        ImGuizmo::Manipulate(
                            modelview,
                            projection,
                            ImGuizmo::TRANSLATE | ImGuizmo::ROTATE_Z | ImGuizmo::ROTATE_X | ImGuizmo::ROTATE_Y,
                            ImGuizmo::WORLD,
                            m_gizmo,
                            NULL);
                    }
                    else
                        ImGuizmo::Manipulate(
                            m_ortho_gizmo_view,
                            m_ortho_projection,
                            ImGuizmo::TRANSLATE_X | ImGuizmo::TRANSLATE_Y | ImGuizmo::ROTATE_Z,
                            ImGuizmo::WORLD,
                            m_gizmo,
                            NULL);

                    gizmo_dragging_this_frame = ImGuizmo::IsUsing();

                    // Candidate pose chain from this frame's gizmo value. Computed either way
                    // (cheap, CPU-only) but only ever written into the session data model --
                    // which is what triggers ScanRenderer's GPU re-upload -- once the drag ends.
                    const Eigen::Affine3d new_pose0(Eigen::Map<const Eigen::Matrix4f>(m_gizmo).cast<double>());
                    prev_pose_after_gismo = new_pose0;

                    std::vector<Eigen::Affine3d> candidate_poses(sessions[i].point_clouds_container.point_clouds.size());
                    candidate_poses[0] = new_pose0;
                    Eigen::Affine3d curr_m_pose = new_pose0;
                    for (size_t j = 1; j < all_m_poses.size(); j++)
                    {
                        curr_m_pose = curr_m_pose * (all_m_poses[j - 1].inverse() * all_m_poses[j]);
                        candidate_poses[j] = curr_m_pose;
                    }

                    if (gizmo_dragging_this_frame)
                    {
                        // Actively being dragged this frame: don't touch the session's pose --
                        // that's what used to force ScanRenderer::syncPoses() to tear down and
                        // re-upload every scan's full GPU buffer every single frame. The render
                        // loop draws this session's preview from this entry instead.
                        session_drag_preview_poses[i] = std::move(candidate_poses);
                    }
                    else
                    {
                        const bool was_dragging = session_drag_preview_poses.erase(i) > 0;
                        if (was_dragging)
                        {
                            // Drag just ended: commit the final pose for real, exactly once --
                            // the only frame this session's GPU buffers actually get rebuilt.
                            commit_session_poses(i, candidate_poses);
                        }
                        // else: gizmo is merely selected, not being dragged -- leave the
                        // session's pose untouched, so selecting a gizmo doesn't by itself
                        // cause any per-frame GPU churn.
                    }
                }
            }
        }
        if (gizmo_all_sessions)
        {
            for (size_t i = 0; i < sessions.size(); i++)
            {
                // guizmo_all_sessions;
                if (!sessions[i].is_gizmo && !sessions[i].is_ground_truth && !sessions[i].point_clouds_container.point_clouds.empty())
                {
                    std::vector<Eigen::Affine3d> all_m_poses;
                    for (size_t j = 0; j < sessions[i].point_clouds_container.point_clouds.size(); j++)
                        all_m_poses.push_back(sessions[i].point_clouds_container.point_clouds[j].m_pose);

                    Eigen::Affine3d m_rel_org = prev_pose_manipulated.inverse() * all_m_poses[0];
                    Eigen::Affine3d m_new = prev_pose_after_gismo * m_rel_org;

                    std::vector<Eigen::Affine3d> candidate_poses(all_m_poses.size());
                    candidate_poses[0] = m_new;
                    Eigen::Affine3d curr_m_pose = m_new;
                    for (size_t j = 1; j < all_m_poses.size(); j++)
                    {
                        curr_m_pose = curr_m_pose * (all_m_poses[j - 1].inverse() * all_m_poses[j]);
                        candidate_poses[j] = curr_m_pose;
                    }

                    if (gizmo_dragging_this_frame)
                    {
                        session_drag_preview_poses[i] = std::move(candidate_poses);
                    }
                    else
                    {
                        const bool was_dragging = session_drag_preview_poses.erase(i) > 0;
                        if (was_dragging)
                            commit_session_poses(i, candidate_poses);
                    }
                }
            }
        }
    }
    else // if (is_loop_closure_gui)
    {
        // ImGuizmo -----------------------------------------------
        if (edge_gizmo && edges.size() > 0)
        {
            ImGuizmo::BeginFrame();
            ImGuizmo::Enable(true);
            ImGuizmo::SetRect(0, 0, io.DisplaySize.x, io.DisplaySize.y);

            if (!is_ortho)
            {
                float projection[16];
                getProjectionMatrix(projection);

                float modelview[16];
                getModelviewMatrix(modelview);

                ImGuizmo::Manipulate(
                    &modelview[0],
                    &projection[0],
                    ImGuizmo::TRANSLATE | ImGuizmo::ROTATE_Z | ImGuizmo::ROTATE_X | ImGuizmo::ROTATE_Y,
                    ImGuizmo::WORLD,
                    m_gizmo,
                    NULL);
            }
            else
                ImGuizmo::Manipulate(
                    m_ortho_gizmo_view,
                    m_ortho_projection,
                    ImGuizmo::TRANSLATE_X | ImGuizmo::TRANSLATE_Y | ImGuizmo::ROTATE_Z,
                    ImGuizmo::WORLD,
                    m_gizmo,
                    NULL);

            Eigen::Affine3d m_g = Eigen::Affine3d::Identity();

            m_g.matrix() = Eigen::Map<const Eigen::Matrix4f>(m_gizmo).cast<double>();

            const int& index_src = edges[index_active_edge].index_from;

            const Eigen::Affine3d& m_src =
                sessions[edges[index_active_edge].index_session_from].point_clouds_container.point_clouds.at(index_src).m_pose;
            const auto updated = pose_tait_bryan_from_affine_matrix(m_src.inverse() * m_g);
#ifdef HDMAPPING_GUI_BRIDGE
            const auto& previous = edges[index_active_edge].relative_pose_tb;
            if (previous.px != updated.px || previous.py != updated.py || previous.pz != updated.pz || previous.om != updated.om ||
                previous.fi != updated.fi || previous.ka != updated.ka)
                guiState->Changed();
#endif
            edges[index_active_edge].relative_pose_tb = updated;
        }
    }

    view_kbd_shortcuts();

    if (IsFileDropped())
        loadDroppedFiles();

    // No synthetic key/modifier release after handling a shortcut here:
    // rlImGui only forwards modifier edges, so a faked Ctrl release is never
    // undone while Ctrl stays held, and the next key press falls through to
    // the plain-key view shortcuts (e.g. Ctrl+R, R -> camera "Right" preset).
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false))
        addSession();

    if ((project_settings.session_file_names.size() > 0) && !loaded_sessions)
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_L, false))
            loadSessions();

    // Same enable rule as the "Manual Loop Closure" menu item; closing is always allowed.
    if (is_loop_closure_gui || number_visible_sessions == 1 || number_visible_sessions == 2)
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_M, false))
            is_loop_closure_gui = !is_loop_closure_gui;

    // Ctrl+G: bring the rotation center to the gizmo (session or edge gizmo, both kept in m_gizmo).
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_G, false))
    {
        const bool session_gizmo_visible = std::any_of(
            sessions.begin(),
            sessions.end(),
            [](const Session& s)
            {
                return s.is_gizmo && !s.is_ground_truth && !s.point_clouds_container.point_clouds.empty();
            });
        if (session_gizmo_visible || (edge_gizmo && !edges.empty()))
            setNewRotationCenter(Eigen::Map<const Eigen::Matrix4f>(m_gizmo).block<3, 1>(0, 3).cast<double>());
    }

    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O, false))
        openProject();

    if (project_settings.session_file_names.size() > 0)
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false))
            remove_gui = true;

    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_R, false)) // random colors per session
    {
        for (auto& session : sessions)
        {
            session.render_color[0] = float(rand() % 255) / 255.0f;
            session.render_color[1] = float(rand() % 255) / 255.0f;
            session.render_color[2] = float(rand() % 255) / 255.0f;

            for (auto& pc : session.point_clouds_container.point_clouds)
            {
                pc.traj_color[0] = session.render_color[0];
                pc.traj_color[1] = session.render_color[1];
                pc.traj_color[2] = session.render_color[2];
                pc.render_color[0] = session.render_color[0];
                pc.render_color[1] = session.render_color[1];
                pc.render_color[2] = session.render_color[2];
            }
        }
    }

    if (sessions.size() > 0)
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
        {
            if (io.KeyShift)
                saveProjectAs();
            else
                saveProject();
        }

    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("Open project", "Ctrl+O"))
                openProject();
            if (ImGui::MenuItem("Save project", "Ctrl+S", nullptr, project_settings.session_file_names.size() > 0))
                saveProject();
            if (ImGui::MenuItem("Save project as...", "Ctrl+Shift+S", nullptr, project_settings.session_file_names.size() > 0))
                saveProjectAs();
            if (ImGui::IsItemHovered() && !project_file_name.empty())
                ImGui::SetTooltip("Current project: %s", project_file_name.c_str());

#ifdef HDMAPPING_GUI_BRIDGE
            if (ImGui::MenuItem("Save complete project", nullptr, nullptr, loaded_sessions && !sessions.empty() && !projectJobActive()))
                submitProjectJob({ std::string(proc::project_export_operation), {}, json::object(), { { "format", "project" } } }, false);
#endif
            ImGui::Separator();

            if (ImGui::MenuItem("Add session(s)", "Ctrl+A"))
                addSession();
            if (ImGui::MenuItem("Remove session(s)", "Ctrl+D", nullptr, project_settings.session_file_names.size() > 0))
                remove_gui = true;

            if (ImGui::MenuItem("Load sessions", "Ctrl+L", nullptr, (project_settings.session_file_names.size() > 0) && !loaded_sessions))
                loadSessions();

            ImGui::Separator();
            if (ImGui::MenuItem("Save session poses", nullptr, nullptr, loaded_sessions && !sessions.empty()))
                save_results(sessions);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Overwrite the poses file of every session except ground truth");

            if (ImGui::BeginMenu("Export trajectory as TUM", loaded_sessions && !sessions.empty() && projectWorkAvailable()))
            {
                for (std::size_t i = 0; i < sessions.size(); ++i)
                {
                    const auto& session = sessions[i];
                    if (Session::is_point_cloud_file(session.session_file_name))
                        continue;
                    const auto session_path = sessionPathFromText(session.session_file_name);
                    const auto folder_name = sessionPathText(session_path.parent_path().filename());
                    const std::string label = folder_name + "/" + sessionPathText(session_path.filename()) + "##tum" + std::to_string(i);
                    if (ImGui::MenuItem(label.c_str()))
                    {
                        const auto output = mandeye::fd::SaveFileDialog(
                            "Export trajectory as TUM", mandeye::fd::Tum_filter, ".txt", folder_name + "_trajectory_tum.txt");
                        if (!output.empty() &&
                            !proc::WriteProjectTrajectory(
                                session, sessionPathFromText(output), "trajectory_tum", "lidar", "quaternion", true))
                            pfd::message("Export failed", "Could not write '" + output + "'", pfd::choice::ok, pfd::icon::error);
                    }
                }
                ImGui::EndMenu();
            }

            const auto trajectory_formats = [](bool publish)
            {
                const auto export_files = [publish](json parameters, const std::string& suffix)
                {
#ifdef HDMAPPING_GUI_BRIDGE
                    if (publish)
                        return publish_project_files(parameters);
#else
                    static_cast<void>(publish);
#endif
                    export_project_files(parameters, suffix);
                };
                if (ImGui::MenuItem("Save all as las/laz files"))
                    export_files({ { "format", "trajectory_laz" } }, "_trajectory_laz.laz");
                for (const bool quaternion : { false, true })
                {
                    ImGui::Separator();
                    ImGui::PushID(quaternion ? "quaternion" : "matrix");
                    ImGui::TextUnformatted(quaternion ? "(x,y,z,qx,qy,qz,qw)" : "(x,y,z,r00,r01,r02,r10,r11,r12,r20,r21,r22)");
                    for (const auto& [label, timestamps, suffix] :
                         { std::array{ "Save all as csv (timestamp Lidar)", "lidar", "Lidar" },
                           std::array{ "Save all as csv (timestamp Unix)", "unix", "Unix" },
                           std::array{ "Save all as csv (timestamp Lidar, Unix)", "both", "LidarUnix" } })
                        if (ImGui::MenuItem(label))
                            export_files(
                                { { "format", "trajectory_csv" },
                                  { "timestamps", timestamps },
                                  { "pose", quaternion ? "quaternion" : "matrix" } },
                                std::string("_trajectory_timestamp") + suffix + (quaternion ? "_q.csv" : "_r.csv"));
                    ImGui::PopID();
                }
                if (ImGui::MenuItem("Save all as TUM TXT"))
                    export_files({ { "format", "trajectory_tum" }, { "sort_tum", false } }, "_trajectory_tum.txt");
            };
            if (ImGui::BeginMenu("Save all marked trajectories", projectWorkAvailable()))
            {
                trajectory_formats(false);
                if (ImGui::MenuItem("Save all as TUM TXT (single folder)"))
                    export_project_files({ { "format", "trajectory_tum" }, { "sort_tum", false } }, "_trajectory_tum.txt", true);
                ImGui::EndMenu();
            }
#ifdef HDMAPPING_GUI_BRIDGE
            if (ImGui::BeginMenu("Publish project exports", projectWorkAvailable()))
            {
                if (ImGui::MenuItem("Session poses"))
                    publish_project_files({ { "format", "poses" } });
                if (ImGui::MenuItem("Marked point clouds as LAS"))
                    publish_project_files({ { "format", "las" } });
                if (ImGui::MenuItem("Marked point clouds as LAZ"))
                    publish_project_files({ { "format", "laz" } });
                if (ImGui::MenuItem("Marked trajectories as LAS"))
                    publish_project_files({ { "format", "trajectory_las" } });
                if (ImGui::BeginMenu("Trajectories"))
                {
                    trajectory_formats(true);
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("Sections"))
                {
                    for (const auto* plane : { "xy", "xz", "yz" })
                        if (ImGui::MenuItem(plane))
                            publish_project_files(
                                { { "format", "section_laz" }, { "plane", plane }, { "width_m", gui_intersection_width } });
                    ImGui::EndMenu();
                }
                ImGui::EndMenu();
            }
#endif

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Tools"))
        {
            ImGui::MenuItem("Normal Distributions Transform", nullptr, &is_ndt_gui, !is_loop_closure_gui && (sessions.size() > 0));
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::Text("Point cloud alignment (registration) algorithm");
                ImGui::Text(
                    "Probabilistic alternative to ICP that models one cloud (the target)\nas a set of Gaussian distributions "
                    "rather than raw points");
                ImGui::Text(
                    "Robust for rough initial poses but can converge to a local optimum\nif the initial misalignment is very large");
                ImGui::Text(
                    "Known for being faster and smoother in optimization because\nit replaces discrete point-point correspondences "
                    "with continuous probability density functions.");
                ImGui::EndTooltip();
            }

            if (ImGui::MenuItem("Optimize GRAPH", nullptr, false, projectWorkAvailable()))
                optimizeProjectFromGui();
            ImGui::MenuItem(
                "Manual Loop Closure", "Ctrl+M", &is_loop_closure_gui, (number_visible_sessions == 1 || number_visible_sessions == 2));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Manually connect overlapping scan sections");

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Intersections"))
        {
            bool changed = false;

            ImGui::SetNextItemWidth(ImGuiNumberWidth);
            changed |= ImGui::InputDouble("Intersection width [m]", &gui_intersection_width, 0.0, 0.0, "%.2f");
            if (gui_intersection_width < 0.001)
                gui_intersection_width = 0.001;

            ImGui::Separator();
            changed |= ImGui::MenuItem("xz_intersection", nullptr, &gui_xz_intersection);
            changed |= ImGui::MenuItem("10m grid##xz", nullptr, &gui_xz_grid_10x10);
            changed |= ImGui::MenuItem("1m grid##xz", nullptr, &gui_xz_grid_1x1);
            changed |= ImGui::MenuItem("0.1m grid##xz", nullptr, &gui_xz_grid_01x01);
            if (ImGui::MenuItem("Export xz intersection", nullptr, false, gui_xz_intersection && projectWorkAvailable()))
                export_project_files(
                    { { "format", "section_laz" }, { "plane", "xz" }, { "width_m", gui_intersection_width } }, "_xz_intersection.laz");

            ImGui::Separator();
            changed |= ImGui::MenuItem("yz_intersection", nullptr, &gui_yz_intersection);
            changed |= ImGui::MenuItem("10m grid##yz", nullptr, &gui_yz_grid_10x10);
            changed |= ImGui::MenuItem("1m grid##yz", nullptr, &gui_yz_grid_1x1);
            changed |= ImGui::MenuItem("0.1m grid##yz", nullptr, &gui_yz_grid_01x01);
            if (ImGui::MenuItem("Export yz intersection", nullptr, false, gui_yz_intersection && projectWorkAvailable()))
                export_project_files(
                    { { "format", "section_laz" }, { "plane", "yz" }, { "width_m", gui_intersection_width } }, "_yz_intersection.laz");

            ImGui::Separator();
            changed |= ImGui::MenuItem("xy_intersection", nullptr, &gui_xy_intersection);
            changed |= ImGui::MenuItem("10m grid##xy", nullptr, &gui_xy_grid_10x10);
            changed |= ImGui::MenuItem("1m grid##xy", nullptr, &gui_xy_grid_1x1);
            changed |= ImGui::MenuItem("0.1m grid##xy", nullptr, &gui_xy_grid_01x01);
            if (ImGui::MenuItem("Export xy intersection", nullptr, false, gui_xy_intersection && projectWorkAvailable()))
                export_project_files(
                    { { "format", "section_laz" }, { "plane", "xy" }, { "width_m", gui_intersection_width } }, "_xy_intersection.laz");

            if (changed)
            {
                for (auto& session : sessions)
                {
                    session.point_clouds_container.xz_intersection = gui_xz_intersection;
                    session.point_clouds_container.yz_intersection = gui_yz_intersection;
                    session.point_clouds_container.xy_intersection = gui_xy_intersection;
                    session.point_clouds_container.intersection_width = gui_intersection_width;

                    session.point_clouds_container.xz_grid_10x10 = gui_xz_grid_10x10;
                    session.point_clouds_container.xz_grid_1x1 = gui_xz_grid_1x1;
                    session.point_clouds_container.xz_grid_01x01 = gui_xz_grid_01x01;
                    session.point_clouds_container.yz_grid_10x10 = gui_yz_grid_10x10;
                    session.point_clouds_container.yz_grid_1x1 = gui_yz_grid_1x1;
                    session.point_clouds_container.yz_grid_01x01 = gui_yz_grid_01x01;
                    session.point_clouds_container.xy_grid_10x10 = gui_xy_grid_10x10;
                    session.point_clouds_container.xy_grid_1x1 = gui_xy_grid_1x1;
                    session.point_clouds_container.xy_grid_01x01 = gui_xy_grid_01x01;
                }
            }

            ImGui::EndMenu();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Intersection menu");

        if (ImGui::BeginMenu("View"))
        {
            ImGui::BeginDisabled(!(sessions.size() > 0));
            {
                auto tmp = point_size;
                ImGui::SetNextItemWidth(ImGuiNumberWidth);
                ImGui::InputInt("Points size", &point_size);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("keyboard 1-9 keys");
                if (point_size < 1)
                    point_size = 1;
                else if (point_size > 10)
                    point_size = 10;

                if (tmp != point_size)
                    for (auto& session : sessions)
                        for (auto& point_cloud : session.point_clouds_container.point_clouds)
                            point_cloud.point_size = point_size;

                ImGui::Separator();
            }
            ImGui::EndDisabled();

            if (ImGui::MenuItem("Orthographic", "key O", &is_ortho))
            {
                if (is_ortho)
                {
                    new_rotation_center = rotation_center;
                    new_rotate_x = 0.0;
                    new_rotate_y = 0.0;
                    new_translate_x = translate_x;
                    new_translate_y = translate_y;
                    new_translate_z = translate_z;
                    camera_transition_active = true;
                }
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Switch between perspective view (3D) and orthographic view (2D/flat)");

            ImGui::MenuItem("Show axes", "key X", &show_axes);
            ImGui::MenuItem("Show compass/ruler", "key C", &compass_ruler);

            ImGui::MenuItem("Lock Z", "Shift + Z", &lock_z, !is_ortho);

            // ImGui::MenuItem("show_covs", nullptr, &show_covs);

            ImGui::Separator();

            ImGui::Text("Colors:");

            ImGui::ColorEdit3("Background", (float*)&bg_color, ImGuiColorEditFlags_NoInputs);
            pointsColorMenu();

            ImGui::Separator();

            ImGui::MenuItem("Settings", nullptr, &is_settings_gui);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Show power user settings window with more parameters");

            ImGui::EndMenu();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Scene view relevant parameters");

        camMenu();

        ImGui::BeginDisabled(sessions.size() <= 0);
        {
            ImGui::SameLine();
            ImGui::Dummy(ImVec2(20, 0));
            ImGui::SameLine();

            ImGui::SetNextItemWidth(ImGuiNumberWidth);
            ImGui::InputInt("Points render downsampling", &viewer_decimate_point_cloud, 10, 100);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("increase for better performance, decrease for rendering more points");
            // ImGui::SameLine();

            if (viewer_decimate_point_cloud < 1)
                viewer_decimate_point_cloud = 1;

            ImGui::SameLine();

            ImGui::InputInt("Trajectory reduce render", &viewer_reduce_rendered_trajectory, 10, 100);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("increase for better performance, decrease for rendering more nodes in the trajectory");
            // ImGui::SameLine();

            if (viewer_reduce_rendered_trajectory < 1)
                viewer_reduce_rendered_trajectory = 1;

            ImGui::SameLine();

            ImGui::Text("(%.1f FPS)", ImGui::GetIO().Framerate);
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::Dummy(ImVec2(20, 0));
        ImGui::SameLine();

        ImGui::SameLine(
            ImGui::GetWindowWidth() - ImGui::CalcTextSize("Info").x - ImGui::GetStyle().ItemSpacing.x * 2 -
            ImGui::GetStyle().FramePadding.x * 2);

        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_Header));
        if (ImGui::SmallButton("Info"))
            info_gui = !info_gui;

        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(3);

        ImGui::EndMainMenuBar();
    }

    if (remove_gui)
    {
        ImGui::OpenPopup("Remove session(s)");
        remove_gui = false;
    }

    if (ImGui::BeginPopupModal("Remove session(s)", NULL, ImGuiWindowFlags_AlwaysAutoResize))
    {
        static std::vector<bool> session_marked_for_removal;
        if (session_marked_for_removal.size() != project_settings.session_file_names.size())
            session_marked_for_removal.resize(project_settings.session_file_names.size(), false);

        ImGui::Text("Select session(s) to remove:");
        ImGui::Separator();

        for (size_t i = 0; i < project_settings.session_file_names.size(); i++)
        {
            bool checked = session_marked_for_removal[i];
            if (ImGui::Checkbox(project_settings.session_file_names[i].c_str(), &checked))
                session_marked_for_removal[i] = checked;
        }

        ImGui::Separator();

        if (ImGui::Button("Remove"))
        {
            for (size_t i = project_settings.session_file_names.size(); i > 0; --i)
            {
                size_t idx = i - 1;

                if (session_marked_for_removal[idx])
                {
                    RemoveProjectSession(project_settings, sessions, idx);
                }
            }
            session_marked_for_removal.clear();
            refreshProjectSessions();
            markProjectChanged();

            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            session_marked_for_removal.clear();
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    if (is_ndt_gui)
        ndt_gui();

    if (!prev_is_loop_closure_gui && is_loop_closure_gui)
    {
        spdlog::debug("Open loop open gui");
        on_loop_closure_gui_open();
    }
    if (is_loop_closure_gui)
    {
        // loop_closure_gui() passes &is_loop_closure_gui as the window's p_open, so clicking
        // its own close (X) button flips is_loop_closure_gui to false inside this call -- the
        // close check below has to run after, or that transition is never observed.
        loop_closure_gui();
    }
    if (prev_is_loop_closure_gui && !is_loop_closure_gui)
    {
        spdlog::debug("Closed loop closure gui");
        on_loop_closure_gui_close();
    }
    prev_is_loop_closure_gui = is_loop_closure_gui;

    cor_window();

    info_window(infoLines, appShortcuts);

    end3DAndDrawLabels();

    if (compass_ruler)
        drawMiniCompassWithRuler();

    // my_display_code();
    /*if (is_ndt_gui)
        ndt_gui();
    if (is_icp_gui)
        icp_gui();
    if (is_pose_graph_slam)
        pose_graph_slam_gui();
    if (is_registration_plane_feature)
        registration_plane_feature_gui();
    if (is_manual_analisys)
        observation_picking_gui();*/
    // if (is_loop_closure_gui)
    //     manual_pose_graph_loop_closure.Gui();

    if (is_settings_gui)
        settings_gui();

    rlImGuiEnd();
}

void mouse(int glut_button, int state, int x, int y)
{
    ImGuiIO& io = ImGui::GetIO();

    if (!io.WantCaptureMouse)
    {
        if ((glut_button == GLUT_MIDDLE_BUTTON || glut_button == GLUT_RIGHT_BUTTON) && state == GLUT_DOWN && (io.KeyCtrl || io.KeyShift) &&
            !manipulate_active_edge)
        {
            // if (s_loop_closure_gui)
            if ((sessions.size() > 0) && (number_visible_sessions > 0) && update_rotation_center
#ifdef HDMAPPING_GUI_BRIDGE
                && !guiState->Reserved()
#endif
            )
            {
                getClosestTrajectoriesPoint(
                    sessions,
                    x,
                    y,
                    first_session_index,
                    second_session_index,
                    number_visible_sessions,
                    index_loop_closure_source,
                    index_loop_closure_target,
                    io.KeyShift,
                    time_stamp_offset);
            }
            else if (io.KeyCtrl)
            {
                setNewRotationCenterToClosestTrajectoryPoint(sessions, x, y);
            }
        }

        if (state == GLUT_DOWN)
        {
            mouse_buttons |= 1 << glut_button;

            /*if (observation_picking.is_observation_picking_mode)
            {
                Eigen::Vector3d p = GLWidgetGetOGLPos(x, y, observation_picking);
                int number_active_pcs = 0;
                int index_picked = -1;
                for (size_t i = 0; i < session.point_clouds_container.point_clouds.size(); i++)
                {
                    if (session.point_clouds_container.point_clouds[i].visible)
                    {
                        number_active_pcs++;
                        index_picked = i;
                    }
                }
                if (number_active_pcs == 1)
                {
                    observation_picking.add_picked_to_current_observation(index_picked, p);
                }
            }*/
        }
        else if (state == GLUT_UP)
        {
            mouse_buttons = 0;
        }
        mouse_old_x = x;
        mouse_old_y = y;
    }
}

int main(int argc, char* argv[])
{
    try
    {
        if (checkClHelp(argc, argv))
        {
            std::cout << winTitle << "\n\n"
                      << "USAGE:\n"
                      << std::filesystem::path(argv[0]).stem().string()
                      << " [--mjp <project.mjp>] [--mjs <session.mjs> ...] [<input_file> ...] /?\n\n"
                      << "where\n"
                      << "   --mjp <project.mjp>  Mandeye JSON Project file to open\n"
                      << "   --mjs <file> [...]   Session file(s) (*.mjs, *.json) to add to the project and load\n"
                      << "   <input_file>         *.mjp opens a project, *.mjs / *.json adds a session\n"
                      << "   -h, /h, --help, /?   Show this help and exit\n\n";

            return 0;
        }

        initGL(&argc, argv, winTitle, display, mouse);

        // --mjp / --mjs take the following non-flag arguments; bare arguments are classified by extension.
        std::string project_file;
        std::vector<std::string> session_files;
        std::string current_flag;
        for (int i = 1; i < argc; i++)
        {
            const std::string arg(argv[i]);
            if (arg == "--mjp" || arg == "--mjs")
            {
                current_flag = arg;
                continue;
            }
            if (arg.rfind("--", 0) == 0)
            {
                std::cerr << "Unknown option: '" << arg << "'" << std::endl;
                current_flag.clear();
                continue;
            }

            std::string ext = fs::path(arg).extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

            if (current_flag == "--mjp" || (current_flag.empty() && ext == ".mjp"))
            {
                if (project_file.empty())
                    project_file = arg;
                else
                    std::cerr << "Only one project can be opened, ignoring: '" << arg << "'" << std::endl;
                current_flag.clear();
            }
            else if (current_flag == "--mjs" || ext == ".mjs" || ext == ".json")
                session_files.push_back(arg);
            else
                std::cerr << "Ignoring argument: '" << arg << "'" << std::endl;
        }

        if (!project_file.empty())
        {
            setLocationHintFromFile(project_file);
            loadProject(project_file);
        }
        if (!session_files.empty())
            setLocationHintFromFile(session_files.back());

        bool added = false;
        for (const auto& session_file : session_files)
            added |= addSessionFile(session_file);
        if (added)
            loadSessions();

#ifdef HDMAPPING_GUI_BRIDGE
        connectProject();
#endif
        mainLoop();
#ifdef HDMAPPING_GUI_BRIDGE
        SetWindowState(FLAG_WINDOW_HIDDEN);
        guiConnection->Close();
#endif

        releaseSessionRenderers();
        shutdownGL();
    } catch (const std::bad_alloc& e)
    {
        std::cerr << "System is out of memory : " << e.what() << std::endl;
        mandeye::fd::OutOfMemMessage();
    }

    return 0;
}