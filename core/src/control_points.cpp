#include <pch/pch.h>

#include <Core/control_points.h>

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

void ControlPoints::imgui(PointClouds& point_clouds_container, const Eigen::Vector3f& rotation_center, const std::function<void()>& align)
{
    if (ImGui::Begin("Control Point", &is_imgui))
    {
        ImGui::Checkbox("Draw uncertainty", &draw_uncertainty);
        ImGui::SameLine();
        ImGui::Checkbox("Track pose with camera", &track_pose_with_camera);

        ImGui::Text("Pose index: ");
        ImGui::SameLine();
        ImGui::PushItemWidth(ImGuiNumberWidth);
        ImGui::SliderInt("##lcss", &index_pose, 0, point_clouds_container.point_clouds.size() - 1);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("max %zu", point_clouds_container.point_clouds.size() - 1);
        ImGui::SameLine();
        ImGui::InputInt("##lcsi", &index_pose, 1, 5);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("max %zu", point_clouds_container.point_clouds.size() - 1);
        if (index_pose < 0)
            index_pose = 0;
        if (index_pose >= point_clouds_container.point_clouds.size() - 1)
            index_pose = point_clouds_container.point_clouds.size() - 1;

        ImGui::Text("To show Control Point (CP) candidate please press 'CTRL'");
        ImGui::Text("To pick point node please press 'CTRL + left/middle mouse'");
        ImGui::Text("At least 3 CPs needed!");

        if (index_picked_point != -1)
        {
            if (ImGui::Button("Add CP"))
            {
                ControlPoint cp; // = picked_control_point;
                cp.x_source_local = point_clouds_container.point_clouds[index_pose].points_local[index_picked_point].x();
                cp.y_source_local = point_clouds_container.point_clouds[index_pose].points_local[index_picked_point].y();
                cp.z_source_local = point_clouds_container.point_clouds[index_pose].points_local[index_picked_point].z();

                cp.x_target_global = rotation_center.x();
                cp.y_target_global = rotation_center.y();
                cp.z_target_global = rotation_center.z();

                cp.sigma_x = 0.01;
                cp.sigma_y = 0.01;
                cp.sigma_z = 0.01;
                strcpy(cp.name, "default_name");
                cp.index_to_pose = index_pose;

                cps.push_back(cp);

                index_picked_point = -1;
            }
        }

        ImGui::NewLine();
        ImGui::Separator();
        ImGui::NewLine();

        int remove_gcp_index = -1;
        for (int i = 0; i < cps.size(); i++)
        {
            const auto gui_text = (std::string("CP_") + std::to_string(i) + " [" + cps[i].name + "]");
            ImGui::Text("%s", gui_text.c_str());
            ImGui::SameLine();
            if (ImGui::Button(("Remove##" + std::to_string(i)).c_str()))
            {
                remove_gcp_index = i;
            }

            ImGui::PushItemWidth(ImGuiNumberWidth);

            ImGui::InputText(("CP name##" + std::to_string(i)).c_str(), cps[i].name, IM_ARRAYSIZE(cps[i].name));
            ImGui::SameLine();
            ImGui::Checkbox(("is z_0##" + std::to_string(i)).c_str(), &cps[i].is_z_0);

            if (!cps[i].is_z_0)
            {
                ImGui::Text("sigma [m]:");
                ImGui::InputDouble(("X##" + std::to_string(i)).c_str(), &cps[i].sigma_x, 0.0, 0.0, "%.3f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(xText);
                ImGui::SameLine();
                ImGui::InputDouble(("Y##" + std::to_string(i)).c_str(), &cps[i].sigma_y, 0.0, 0.0, "%.3f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(yText);
                ImGui::SameLine();
                ImGui::InputDouble(("Z##" + std::to_string(i)).c_str(), &cps[i].sigma_z, 0.0, 0.0, "%.3f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(zText);

                ImGui::Text("target_global [m]:");
                ImGui::InputDouble(("X##t" + std::to_string(i)).c_str(), &cps[i].x_target_global);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(xText);
                ImGui::SameLine();
                ImGui::InputDouble(("Y##t" + std::to_string(i)).c_str(), &cps[i].y_target_global);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(yText);
                ImGui::SameLine();
                ImGui::InputDouble(("Z##t" + std::to_string(i)).c_str(), &cps[i].z_target_global);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(zText);
            }
            else
                ImGui::InputDouble(("sigma_z##" + std::to_string(i)).c_str(), &cps[i].sigma_z);
            ImGui::PopItemWidth();

            ImGui::NewLine();
        }

        if (remove_gcp_index != -1)
        {
            std::vector<ControlPoint> new_gcps;
            for (int i = 0; i < cps.size(); i++)
            {
                if (i != remove_gcp_index)
                {
                    new_gcps.push_back(cps[i]);
                }
            }
            cps = new_gcps;
        }

        if (cps.size() >= 3)
        {
            if (ImGui::Button("Register session to CPs"))
            {
                if (align)
                    align();
                else if (!HDMapping::AlignControlPoints(point_clouds_container, cps))
                    std::cerr << "Could not align session to CPs" << std::endl;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("trajectory is rigid");
        }
        ImGui::End();
    }
    return;
}

void ControlPoints::render(const PointClouds& point_clouds_container, bool show_pc)
{
    if (show_pc)
    {
        if (index_pose < point_clouds_container.point_clouds.size() && point_clouds_container.point_clouds.size() > 0)
        {
            glColor3f(1.0, 0.0, 0.0);
            glPointSize(1.0);
            glBegin(GL_POINTS);
            for (int i = 0; i < point_clouds_container.point_clouds[index_pose].points_local.size(); i++)
            {
                glColor3f(
                    point_clouds_container.point_clouds[index_pose].intensities[i],
                    0.0,
                    1 - point_clouds_container.point_clouds[index_pose].intensities[i]);
                auto p = point_clouds_container.point_clouds[index_pose].m_pose *
                    point_clouds_container.point_clouds[index_pose].points_local[i];
                glVertex3f(p.x(), p.y(), p.z());
            }
            glEnd();

            glColor3f(0.0, 1.0, 0.0);
            glLineWidth(2.0);
            glBegin(GL_LINE_STRIP);
            for (int i = 0; i < point_clouds_container.point_clouds[index_pose].local_trajectory.size(); i++)
            {
                auto pose = point_clouds_container.point_clouds[index_pose].m_pose *
                    point_clouds_container.point_clouds[index_pose].local_trajectory[i].m_pose;
                glVertex3f(pose(0, 3), pose(1, 3), pose(2, 3));
            }

            glEnd();
            glLineWidth(1.0);
        }
    }

    for (int i = 0; i < cps.size(); i++)
    {
        Eigen::Vector3d p(cps[i].x_source_local, cps[i].y_source_local, cps[i].z_source_local);
        Eigen::Vector3d c = point_clouds_container.point_clouds[cps[i].index_to_pose].m_pose * p;

        Eigen::Vector3d g(cps[i].x_target_global, cps[i].y_target_global, cps[i].z_target_global);

        glColor3f(0.7f, 0.3f, 0.5f);
        glBegin(GL_LINES);
        glVertex3f(g.x() - 0.05, g.y(), g.z());
        glVertex3f(g.x() + 0.05, g.y(), g.z());

        glVertex3f(g.x(), g.y() - 0.05, g.z());
        glVertex3f(g.x(), g.y() + 0.05, g.z());

        glVertex3f(g.x() - 0.01, g.y(), g.z());
        glVertex3f(g.x() + 0.01, g.y(), g.z());

        glVertex3f(g.x(), g.y() - 0.01, g.z());
        glVertex3f(g.x(), g.y() + 0.01, g.z());

        glVertex3f(g.x(), g.y(), g.z());
        glVertex3f(g.x(), g.y(), g.z());
        glEnd();

        glColor3f(0.0f, 0.3f, 0.6f);
        glBegin(GL_LINES);
        glVertex3f(c.x(), c.y(), c.z());
        glVertex3f(cps[i].x_target_global, cps[i].y_target_global, cps[i].z_target_global);
        glEnd();

        if (draw_uncertainty)
        {
            Eigen::Matrix3d covar;
            if (cps[i].is_z_0)
            {
                covar(0, 0) = 0.01 * 0.01;
                covar(0, 1) = 0;
                covar(0, 2) = 0;

                covar(1, 0) = 0;
                covar(1, 1) = 0.01 * 0.01;
                covar(1, 2) = 0;

                covar(2, 0) = 0;
                covar(2, 1) = 0;
                covar(2, 2) = cps[i].sigma_z * cps[i].sigma_z;
            }
            else
            {
                covar(0, 0) = cps[i].sigma_x * cps[i].sigma_x;
                covar(0, 1) = 0;
                covar(0, 2) = 0;

                covar(1, 0) = 0;
                covar(1, 1) = cps[i].sigma_y * cps[i].sigma_y;
                covar(1, 2) = 0;

                covar(2, 0) = 0;
                covar(2, 1) = 0;
                covar(2, 2) = cps[i].sigma_z * cps[i].sigma_z;
            }
            Eigen::Vector3d gcp(cps[i].x_target_global, cps[i].y_target_global, cps[i].z_target_global);
            draw_ellipse(covar, gcp, Eigen::Vector3f(0.5, 0.5, 0.5), 1.0f);
        }

        glColor3f(0, 0, 0);
        glRasterPos3f(cps[i].x_target_global, cps[i].y_target_global, cps[i].z_target_global + 0.1);
        glutBitmapString(GLUT_BITMAP_TIMES_ROMAN_24, (const unsigned char*)cps[i].name);

        glRasterPos3f(cps[i].x_target_global, cps[i].y_target_global, cps[i].z_target_global);
        glutBitmapString(GLUT_BITMAP_TIMES_ROMAN_10, (const unsigned char*)(("CP_" + std::to_string(i)).c_str()));

        glColor3f(0.7f, 0.3f, 0.5f);
        glRasterPos3f(c.x(), c.y(), c.z());
        glutBitmapString(GLUT_BITMAP_TIMES_ROMAN_10, (const unsigned char*)(("CP_" + std::to_string(i) + ": initial location").c_str()));
    }

    return;
}

void ControlPoints::draw_ellipse(const Eigen::Matrix3d& covar, const Eigen::Vector3d& mean, const Eigen::Vector3f& color, float nstd)
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