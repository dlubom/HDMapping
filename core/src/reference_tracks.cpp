#include <Core/reference_tracks.h>

#include <Core/gnss.h>
#include <Core/tum.h>
#include <nlohmann/json.hpp>

namespace HDMapping
{
    namespace
    {
        double optionalNumber(const nlohmann::json& row, const char* key, double fallback = 0.0)
        {
            const auto field = row.find(key);
            return field == row.end() || field->is_null() ? fallback : field->get<double>();
        }
    } // namespace
    nlohmann::json EncodeReferenceTracks(const GNSS& gnss, const TUM& tum)
    {
        auto data = nlohmann::json::object();
        nlohmann::json jorigin;
        jorigin["latitude"] = gnss.WGS84ReferenceLatitude;
        jorigin["longitude"] = gnss.WGS84ReferenceLongitude;
        jorigin["geoid_separation"] = gnss.geoidSeparation;
        jorigin["set_from_first_pose"] = gnss.setWGS84ReferenceFromFirstPose;
        if (!gnss.projected_crs.empty())
        {
            jorigin["projected_crs"] = gnss.projected_crs;
            jorigin["projected_vertical_datum"] = gnss.projected_vertical_datum;
        }
        data["enu_origin"] = std::move(jorigin);

        nlohmann::json jgnss = nlohmann::json::array();
        for (const auto& p : gnss.gnss_poses)
        {
            jgnss.push_back(
                { { "timestamp", p.timestamp },
                  { "lat", p.lat },
                  { "lon", p.lon },
                  { "h_wgs84", p.h_wgs84 },
                  { "undulation", p.undulation },
                  { "alt", p.alt },
                  { "hdop", p.hdop },
                  { "satelites_tracked", p.satelites_tracked },
                  { "height", p.height },
                  { "age", p.age },
                  { "time", p.time },
                  { "fix_quality", p.fix_quality },
                  { "enu_x", p.enu_x },
                  { "enu_y", p.enu_y },
                  { "enu_z", p.enu_z },
                  { "dist_xy_along", p.dist_xy_along } });
        }
        data["gnss_measurements"] = jgnss;

        nlohmann::json jtum = nlohmann::json::array();
        for (const auto& p : tum.tum_poses)
        {
            jtum.push_back(
                { { "timestamp", p.timestamp },
                  { "x", p.x },
                  { "y", p.y },
                  { "z", p.z },
                  { "qx", p.qx },
                  { "qy", p.qy },
                  { "qz", p.qz },
                  { "qw", p.qw } });
        }
        data["tum_trajectory"] = jtum;
        return data;
    }

    void DecodeReferenceTracks(const nlohmann::json& data, GNSS& gnss, TUM& tum)
    {
        if (data.contains("enu_origin"))
        {
            const auto& jorigin = data["enu_origin"];
            gnss.WGS84ReferenceLatitude = jorigin.value("latitude", 0.0);
            gnss.WGS84ReferenceLongitude = jorigin.value("longitude", 0.0);
            gnss.geoidSeparation = jorigin.value("geoid_separation", 0.0);
            // A stored origin takes precedence over re-deriving one from the
            // first GNSS pose; honour an explicit flag if the session carries
            // one, otherwise assume the stored origin should be used as-is.
            gnss.setWGS84ReferenceFromFirstPose = jorigin.value("set_from_first_pose", false);
            gnss.projected_crs = jorigin.value("projected_crs", std::string{});
            gnss.projected_vertical_datum = jorigin.value("projected_vertical_datum", std::string{});
        }

        if (data.contains("gnss_measurements"))
        {
            gnss.gnss_poses.clear();
            for (const auto& j : data["gnss_measurements"])
            {
                GNSS::GlobalPose p{};
                p.timestamp = j.value("timestamp", 0.0);
                p.lat = j.value("lat", 0.0);
                p.lon = j.value("lon", 0.0);
                p.undulation = j.value("undulation", 0.0);
                p.alt = j.value("alt", 0.0);
                p.h_wgs84 = optionalNumber(j, "h_wgs84", p.alt + p.undulation);
                p.hdop = optionalNumber(j, "hdop");
                p.satelites_tracked = optionalNumber(j, "satelites_tracked");
                p.height = optionalNumber(j, "height");
                p.age = optionalNumber(j, "age");
                p.time = optionalNumber(j, "time");
                p.fix_quality = optionalNumber(j, "fix_quality");
                p.enu_x = j.value("enu_x", 0.0);
                p.enu_y = j.value("enu_y", 0.0);
                p.enu_z = j.value("enu_z", 0.0);
                p.dist_xy_along = optionalNumber(j, "dist_xy_along");
                gnss.gnss_poses.push_back(p);
            }
        }

        if (data.contains("tum_trajectory"))
        {
            tum.tum_poses.clear();
            for (const auto& j : data["tum_trajectory"])
            {
                TUM::TumPose p{};
                p.timestamp = j.value("timestamp", 0.0);
                p.x = j.value("x", 0.0);
                p.y = j.value("y", 0.0);
                p.z = j.value("z", 0.0);
                p.qx = j.value("qx", 0.0);
                p.qy = j.value("qy", 0.0);
                p.qz = j.value("qz", 0.0);
                p.qw = j.value("qw", 1.0);
                tum.tum_poses.push_back(p);
            }
            tum.version++;
        }
    }
} // namespace HDMapping
