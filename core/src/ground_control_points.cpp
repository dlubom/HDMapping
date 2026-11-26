#include <pch/pch.h>

#include <Core/ground_control_points.h>

#include <Core/reference_alignment.h>

#if WITH_GUI == 1
#include <GL/freeglut.h>

#include <imgui.h>
#include <imgui_impl_glut.h>
#include <imgui_impl_opengl2.h>
#include <imgui_internal.h>

static constexpr float ImGuiNumberWidth = 120.0f;
static constexpr const char* xText = "Longitudinal (forward/backward)";
static constexpr const char* yText = "Lateral (left/right)";
static constexpr const char* zText = "Vertical (up/down)";

void GroundControlPoints::imgui(PointClouds& point_clouds_container, const std::function<void()>& align)
{
    if (ImGui::Begin("Ground Control Point", &is_imgui))
    {
        ImGui::Checkbox("Draw uncertainty", &draw_uncertainty);
        ImGui::SetNextItemWidth(ImGuiNumberWidth);
        ImGui::InputDouble("Default lidar height above ground [m]", &default_lidar_height_above_ground, 0.0, 10.0, "%.3f");

        // ImGui::Text("To show Ground Control Point (GCP) candidate please press 'CTRL'");
        ImGui::Text("To pick trajectory node please press 'CTRL + left/middle mouse'");
        ImGui::Text("At least 3 GCPs needed!");

        if (picking_mode_index_to_node_inner != -1 && picking_mode_index_to_node_outer != -1)
        {
            if (ImGui::Button("Add GCP"))
            {
                GroundControlPoint gcp;
                gcp.index_to_node_inner = picking_mode_index_to_node_inner;
                gcp.index_to_node_outer = picking_mode_index_to_node_outer;
                gcp.lidar_height_above_ground = default_lidar_height_above_ground;
                gcp.sigma_x = 0.1;
                gcp.sigma_y = 0.1;
                gcp.sigma_z = 0.1;
                const auto& p = point_clouds_container.point_clouds[gcp.index_to_node_inner]
                                    .local_trajectory[gcp.index_to_node_outer]
                                    .m_pose.translation();
                Eigen::Vector3d position_global = point_clouds_container.point_clouds[gcp.index_to_node_inner].m_pose * p;

                gcp.x = position_global.x();
                gcp.y = position_global.y();
                gcp.z = position_global.z();
                strcpy(gcp.name, "default_name");

                gpcs.push_back(gcp);
            }
            // int picking_mode_index_to_node_inner = -1;
            // int picking_mode_index_to_node_outer = -1;
        }

        ImGui::NewLine();
        ImGui::Separator();
        ImGui::NewLine();

        int remove_gcp_index = -1;
        for (int i = 0; i < gpcs.size(); i++)
        {
            ImGui::Text(("GCP_" + std::to_string(i) + " (" + gpcs[i].name + ")").c_str());
            ImGui::SameLine();
            if (ImGui::Button(("Remove##" + std::to_string(i)).c_str()))
            {
                remove_gcp_index = i;
            }

            ImGui::PushItemWidth(ImGuiNumberWidth);
            ImGui::InputText(("GCP name##" + std::to_string(i)).c_str(), gpcs[i].name, IM_ARRAYSIZE(gpcs[i].name));

            ImGui::Text("sigma [m]:");
            ImGui::InputDouble(("X##" + std::to_string(i)).c_str(), &gpcs[i].sigma_x, 0.0, 0.0, "%.3f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(xText);
            ImGui::SameLine();
            ImGui::InputDouble(("Y##" + std::to_string(i)).c_str(), &gpcs[i].sigma_y, 0.0, 0.0, "%.3f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(yText);
            ImGui::SameLine();
            ImGui::InputDouble(("Z##" + std::to_string(i)).c_str(), &gpcs[i].sigma_z, 0.0, 0.0, "%.3f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(zText);

            ImGui::Text("coordinates [m]:");
            ImGui::InputDouble(("X##c" + std::to_string(i)).c_str(), &gpcs[i].x);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(xText);
            ImGui::SameLine();
            ImGui::InputDouble(("Y##c" + std::to_string(i)).c_str(), &gpcs[i].y);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(yText);
            ImGui::SameLine();
            ImGui::InputDouble(("Z##c" + std::to_string(i)).c_str(), &gpcs[i].z);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(zText);

            ImGui::InputDouble(("lidar_height_above_ground [m]##" + std::to_string(i)).c_str(), &gpcs[i].lidar_height_above_ground);
            ImGui::PopItemWidth();

            ImGui::NewLine();
        }

        if (remove_gcp_index != -1)
        {
            std::vector<GroundControlPoint> new_gcps;
            for (int i = 0; i < gpcs.size(); i++)
            {
                if (i != remove_gcp_index)
                {
                    new_gcps.push_back(gpcs[i]);
                }
            }
            gpcs = new_gcps;
        }

        if (gpcs.size() >= 3)
        {
            if (ImGui::Button("Register session to GCPs"))
            {
                if (align)
                    align();
                else if (!HDMapping::AlignGroundControlPoints(point_clouds_container, gpcs))
                    std::cerr << "Could not align session to GCPs" << std::endl;
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("trajectory is rigid");
            }
        }

        ImGui::End();
    }

    return;
}

void GroundControlPoints::render(const PointClouds& point_clouds_container)
{
    for (int i = 0; i < gpcs.size(); i++)
    {
        const auto& p = point_clouds_container.point_clouds[gpcs[i].index_to_node_inner]
                            .local_trajectory[gpcs[i].index_to_node_outer]
                            .m_pose.translation();
        Eigen::Vector3d c = point_clouds_container.point_clouds[gpcs[i].index_to_node_inner].m_pose * p;
        Eigen::Vector3d g(gpcs[i].x, gpcs[i].y, gpcs[i].z);

        glColor3f(0.7f, 0.3f, 0.5f);
        glBegin(GL_LINES);
        glVertex3f(g.x() - 0.05, g.y(), g.z());
        glVertex3f(g.x() + 0.05, g.y(), g.z());

        glVertex3f(g.x(), g.y() - 0.05, g.z());
        glVertex3f(g.x(), g.y() + 0.05, g.z());

        glVertex3f(g.x() - 0.01, g.y(), g.z() + gpcs[i].lidar_height_above_ground);
        glVertex3f(g.x() + 0.01, g.y(), g.z() + gpcs[i].lidar_height_above_ground);

        glVertex3f(g.x(), g.y() - 0.01, g.z() + gpcs[i].lidar_height_above_ground);
        glVertex3f(g.x(), g.y() + 0.01, g.z() + gpcs[i].lidar_height_above_ground);

        glVertex3f(g.x(), g.y(), g.z());
        glVertex3f(g.x(), g.y(), g.z() + gpcs[i].lidar_height_above_ground);
        glEnd();

        glColor3f(0.0f, 0.3f, 0.6f);
        glBegin(GL_LINES);
        glVertex3f(c.x(), c.y(), c.z());
        glVertex3f(gpcs[i].x, gpcs[i].y, gpcs[i].z + gpcs[i].lidar_height_above_ground);
        glEnd();

        if (draw_uncertainty)
        {
            Eigen::Matrix3d covar;
            covar(0, 0) = gpcs[i].sigma_x * gpcs[i].sigma_x;
            covar(0, 1) = 0;
            covar(0, 2) = 0;

            covar(1, 0) = 0;
            covar(1, 1) = gpcs[i].sigma_y * gpcs[i].sigma_y;
            covar(1, 2) = 0;

            covar(2, 0) = 0;
            covar(2, 1) = 0;
            covar(2, 2) = gpcs[i].sigma_z * gpcs[i].sigma_z;

            Eigen::Vector3d gcp(gpcs[i].x, gpcs[i].y, gpcs[i].z + gpcs[i].lidar_height_above_ground);
            draw_ellipse(covar, gcp, Eigen::Vector3f(0.5, 0.5, 0.5), 1.0f);
        }

        glColor3f(0.7f, 0.3f, 0.5f);
        glRasterPos3f(gpcs[i].x, gpcs[i].y, gpcs[i].z + gpcs[i].lidar_height_above_ground);
        glutBitmapString(GLUT_BITMAP_TIMES_ROMAN_10, (const unsigned char*)(("GCP_" + std::to_string(i) + ": LiDAR center").c_str()));

        glRasterPos3f(gpcs[i].x, gpcs[i].y, gpcs[i].z);
        glutBitmapString(
            GLUT_BITMAP_TIMES_ROMAN_10, (const unsigned char*)(("GCP_" + std::to_string(i) + ": 'plane on the ground'").c_str()));

        glColor3f(0, 0, 0);
        glRasterPos3f(gpcs[i].x, gpcs[i].y, gpcs[i].z + gpcs[i].lidar_height_above_ground + 0.1);
        glutBitmapString(GLUT_BITMAP_TIMES_ROMAN_24, (const unsigned char*)gpcs[i].name);

        glRasterPos3f(c.x(), c.y(), c.z());
        glutBitmapString(
            GLUT_BITMAP_TIMES_ROMAN_10, (const unsigned char*)(("GCP_" + std::to_string(i) + ": assigned trajectory node").c_str()));
    }

    return;
}

void GroundControlPoints::draw_ellipse(const Eigen::Matrix3d& covar, const Eigen::Vector3d& mean, const Eigen::Vector3f& color, float nstd)
{
    Eigen::LLT<Eigen::Matrix<double, 3, 3>> cholSolver(covar);
    Eigen::Matrix3d transform = cholSolver.matrixL();

    const double pi = 3.141592;
    const double di = 0.02;
    const double dj = 0.04;
    const double du = di * 2 * pi;
    const double dv = dj * pi;
    glColor3f(color.x(), color.y(), color.z());

    for (double i = 0; i < 1.0; i += di) // horizonal
    {
        for (double j = 0; j < 1.0; j += dj) // vertical
        {
            double u = i * 2 * pi; // 0     to  2pi
            double v = (j - 0.5) * pi; //-pi/2 to pi/2

            const Eigen::Vector3d pp0(cos(v) * cos(u), cos(v) * sin(u), sin(v));
            const Eigen::Vector3d pp1(cos(v) * cos(u + du), cos(v) * sin(u + du), sin(v));
            const Eigen::Vector3d pp2(cos(v + dv) * cos(u + du), cos(v + dv) * sin(u + du), sin(v + dv));
            const Eigen::Vector3d pp3(cos(v + dv) * cos(u), cos(v + dv) * sin(u), sin(v + dv));
            Eigen::Vector3d tp0 = transform * (nstd * pp0) + mean;
            Eigen::Vector3d tp1 = transform * (nstd * pp1) + mean;
            Eigen::Vector3d tp2 = transform * (nstd * pp2) + mean;
            Eigen::Vector3d tp3 = transform * (nstd * pp3) + mean;

            glBegin(GL_LINE_LOOP);
            glVertex3dv(tp0.data());
            glVertex3dv(tp1.data());
            glVertex3dv(tp2.data());
            glVertex3dv(tp3.data());
            glEnd();
        }
    }
}
#endif
