#include "step2_loop_candidate_operations.h"

#include "bow_loop_closure_detector.h"

#include <Core/session.h>
#include <HDMapping/Processing/SessionSnapshot.h>
#include <HDMapping/SessionPaths.h>

#include <cmath>
#include <fstream>
#include <map>
#include <set>

namespace HDMapping::Processing
{
    namespace
    {
        namespace fs = std::filesystem;
        using bow_loop_closure::DetectorParams;
        using nlohmann::json;

        OperationError cancelled()
        {
            return { "cancelled", "Loop candidate detection cancelled", json::object() };
        }

        json settings(const DetectorParams& params)
        {
            return { { "min_arc_gap_m", params.min_arc_gap_m }, { "max_xy_dist_m", params.max_xy_dist_m },
                     { "min_bow_score", params.min_bow_score }, { "min_inliers", params.min_inliers },
                     { "orb_features", params.orb_features },   { "max_image_size_px", params.max_image_size_px } };
        }

        DetectorParams readSettings(const json& parameters)
        {
            DetectorParams p;
            for (const auto& [key, value] : parameters.items())
            {
                if (key == "camera_dirs")
                    continue;
                if (!value.is_number() || !std::isfinite(value.get<double>()) || value.get<double>() < 0)
                    throw std::invalid_argument("Invalid loop detection setting: " + key);
                if (key == "min_arc_gap_m")
                    p.min_arc_gap_m = value.get<double>();
                else if (key == "max_xy_dist_m")
                    p.max_xy_dist_m = value.get<double>();
                else if (key == "min_bow_score" && value.get<double>() <= 1.0)
                    p.min_bow_score = value.get<double>();
                else if (value.is_number_integer() && value.get<double>() <= std::numeric_limits<int>::max())
                {
                    const int integer = value.get<int>();
                    if (key == "min_inliers" && integer > 0)
                        p.min_inliers = integer;
                    else if (key == "orb_features" && integer > 0)
                        p.orb_features = integer;
                    else if (key == "max_image_size_px" && (integer == 0 || integer >= 2))
                        p.max_image_size_px = integer;
                    else
                        throw std::invalid_argument("Invalid loop detection setting: " + key);
                }
                else
                    throw std::invalid_argument("Invalid loop detection setting: " + key);
            }
            return p;
        }

        std::vector<std::string> cameraDirectories(const json& parameters)
        {
            const auto cameras = parameters.at("camera_dirs").get<std::vector<std::string>>();
            if (cameras.empty())
                throw std::invalid_argument("Supply at least one camera folder");
            for (const auto& camera : cameras)
                if (camera.find('\0') != std::string::npos || !sessionPathFromText(camera).is_absolute())
                    throw std::invalid_argument("Camera folders must be absolute local paths");
            return cameras;
        }

        //! Write the report and own a single copy of every camera image used by its candidates.
        class CandidateReport final : public CaptureSource
        {
        public:
            CandidateReport(
                const TargetRef& target,
                const PointClouds& scans,
                const std::vector<bow_loop_closure::CameraFrame>& frames,
                const std::vector<bow_loop_closure::Candidate>& candidates,
                const DetectorParams& params,
                std::size_t camera_count)
                : target_(target)
                , scans_(scans)
                , frames_(frames)
                , candidates_(candidates)
                , params_(params)
                , camera_count_(camera_count)
            {
            }

            TargetRef Target() const override
            {
                return target_;
            }

            CaptureResult Materialize(const fs::path& directory, RunContext& context) override
            {
                auto rows = json::array();
                for (const auto& c : candidates_)
                {
                    if (context.stop.stop_requested())
                        return cancelled();
                    const auto pose = PoseGraphLoopClosure::make_edge(scans_, c.index_i, c.index_j).relative_pose_tb;
                    rows.push_back(
                        { { "edge",
                            { { "index_from", c.index_i },
                              { "index_to", c.index_j },
                              { "relative_pose", { pose.px, pose.py, pose.pz, pose.om, pose.fi, pose.ka } } } },
                          { "frame_from", saveFrame(directory, c.frame_i) },
                          { "frame_to", saveFrame(directory, c.frame_j) },
                          { "xy_dist_m", c.xy_dist_m },
                          { "arc_gap_m", c.arc_gap_m },
                          { "bow_score", c.bow_score },
                          { "inliers", c.inliers } });
                }
                constexpr auto entry = "candidates.json";
                std::ofstream output(directory / entry, std::ios::binary);
                output.exceptions(std::ios::badbit | std::ios::failbit);
                output << json{ { "candidates", std::move(rows) }, { "settings", settings(params_) }, { "camera_count", camera_count_ } };
                output.close();
                return SnapshotContents{ std::string(loop_candidates_kind), entry, json::object(), {} };
            }

            std::vector<fs::path> files() const
            {
                std::vector<fs::path> paths{ "candidates.json" };
                for (const auto& [frame, path] : images_)
                    paths.push_back(path);
                return paths;
            }

        private:
            json saveFrame(const fs::path& directory, int id)
            {
                const auto& frame = frames_.at(id);
                const auto input = sessionPathFromText(frame.file_path);
                const auto [entry, inserted] = images_.try_emplace(id, "frame-" + std::to_string(id) + ".jpg");
                if (inserted)
                    fs::copy_file(input, directory / entry->second);
                return { { "camera_index", frame.camera },
                         { "timestamp_ns", std::to_string(frame.timestamp_ns) },
                         { "name", sessionPathText(input.filename()) },
                         { "file", sessionPathText(entry->second) } };
            }

            const TargetRef& target_;
            const PointClouds& scans_;
            const std::vector<bow_loop_closure::CameraFrame>& frames_;
            const std::vector<bow_loop_closure::Candidate>& candidates_;
            const DetectorParams& params_;
            std::size_t camera_count_;
            std::map<int, fs::path> images_;
        };
    } // namespace

    OperationDescriptor DescribeLoopCandidateDetection()
    {
        const DetectorParams defaults;
        const auto defaultsJson = settings(defaults);
        json properties{
            { "camera_dirs",
              { { "type", "array" },
                { "minItems", 1 },
                { "items", { { "type", "string" } } },
                { "description", "Absolute camera directories, in camera index order, containing timestamp-named JPGs." } } }
        };
        for (const auto& [key, value] : defaultsJson.items())
            properties[key] = { { "type", value.is_number_integer() ? "integer" : "number" }, { "minimum", 0 }, { "default", value } };
        properties["min_bow_score"]["maximum"] = 1;
        properties["orb_features"]["minimum"] = 1;
        properties["min_inliers"]["minimum"] = 1;
        return { std::string(detect_candidates_operation),
                 { { "type", "object" },
                   { "required", { "scan_indices" } },
                   { "additionalProperties", false },
                   { "properties",
                     { { "scan_indices",
                         { { "type", "array" },
                           { "minItems", 2 },
                           { "uniqueItems", true },
                           { "items", { { "type", "integer" }, { "minimum", 0 } } },
                           { "description", "Eligible scan endpoints. Arc length follows the full session order." } } } } } },
                 { { "type", "object" },
                   { "required", { "camera_dirs" } },
                   { "additionalProperties", false },
                   { "properties", std::move(properties) } },
                 { { "type", "object" },
                   { "required", { "artifact_id", "target", "snapshot", "files", "dependencies" } },
                   { "description",
                     "Snapshot entry candidates.json contains scored candidates with review frames and a P09 edge payload. "
                     "The relative pose comes from the snapshot poses. RANSAC verifies image correspondences." } },
                 OperationClass::Heavy };
    }

    RunResult ExecuteLoopCandidateDetection(const OperationRequest& request, const SnapshotRef& snapshot, RunContext& context)
    {
        if (request.target != snapshot.target)
            return OperationError{ "revision_conflict",
                                   "Candidate request and snapshot must name the same opening and revision",
                                   json::object() };
        if (context.stop.stop_requested())
            return cancelled();
        if (!request.scope.is_object() || request.scope.size() != 1 || !request.scope.contains("scan_indices") ||
            !request.scope.at("scan_indices").is_array() || request.scope.at("scan_indices").size() < 2)
            return OperationError{ "invalid_scope", "Supply at least two explicit scan indices", json::object() };
        DetectorParams params;
        std::vector<std::string> cameras;
        try
        {
            if (!request.parameters.is_object())
                throw std::invalid_argument("Loop detection parameters must be an object");
            params = readSettings(request.parameters);
            cameras = cameraDirectories(request.parameters);
        } catch (const std::exception& error)
        {
            return OperationError{ "invalid_parameters", error.what(), json::object() };
        }
        try
        {
            if (context.progress)
                context.progress({ "read_session", {}, {}, {} });
            const auto loaded = LoadSessionSnapshot(snapshot, context.stop);
            if (const auto* error = std::get_if<OperationError>(&loaded))
                return *error;
            const auto& clouds = std::get<SessionSnapshot>(loaded).session->point_clouds_container;
            const auto count = clouds.point_clouds.size();
            std::set<std::size_t> eligible;
            for (const auto& index : request.scope.at("scan_indices"))
            {
                if (!index.is_number_integer() || index.get<double>() < 0 || index.get<std::uint64_t>() >= count ||
                    !eligible.insert(index.get<std::size_t>()).second)
                    return OperationError{ "invalid_scope",
                                           "Scan indices must be distinct integers in the target session",
                                           json::object() };
            }
            for (const auto& scan : clouds.point_clouds)
            {
                const auto determinant = scan.m_pose.linear().determinant();
                if (!scan.m_pose.matrix().allFinite() || !std::isfinite(determinant) || std::abs(determinant) <= 1e-12)
                    return OperationError{ "invalid_snapshot",
                                           "Candidate detection requires finite invertible scan poses",
                                           json::object() };
            }
            for (const auto& camera : cameras)
                if (!fs::is_directory(sessionPathFromText(camera)))
                    return OperationError{ "invalid_camera_input", "Camera folder is unavailable: " + camera, json::object() };
            const auto frames = bow_loop_closure::loadCameraFrames(cameras);
            if (frames.empty())
                return OperationError{ "invalid_camera_input", "No timestamped JPGs found in the camera folders", json::object() };
            auto chunkFrames = bow_loop_closure::associateFramesToChunks(clouds, frames, params.max_frame_time_gap_ns);
            for (std::size_t index = 0; index < count; ++index)
                if (!eligible.contains(index))
                    chunkFrames[index].clear();

            if (context.progress)
                context.progress({ "detect_loop_candidates", {}, {}, {} });
            auto vocabulary = bow_loop_closure::loadVocabulary(bow_loop_closure::defaultVocabularyPath());
            const auto candidates = bow_loop_closure::detectCandidates(clouds, frames, chunkFrames, params, vocabulary, context.stop);
            if (context.stop.stop_requested())
                return cancelled();

            CandidateReport report(request.target, clouds, frames, candidates, params, cameras.size());
            const auto captured = CaptureSnapshot(report, request.target.revision, context.output_directory / "loop-candidates", context);
            if (const auto* error = std::get_if<OperationError>(&captured))
                return *error;
            const auto ref = std::get<SnapshotRef>(captured);
            auto files = report.files();
            for (auto& file : files)
                file = ref.manifest_path.parent_path() / file;
            files.push_back(ref.manifest_path);
            return ArtifactManifest{ "loop-candidates", request.target, ref, std::move(files), {} };
        } catch (const std::exception& error)
        {
            return OperationError{ "candidate_detection_failed", error.what(), json::object() };
        }
    }

    void CheckLoopCandidateVocabulary()
    {
        auto vocabulary = bow_loop_closure::loadVocabulary(bow_loop_closure::defaultVocabularyPath());
        const cv::Mat descriptors(1, 32, CV_8UC1, cv::Scalar(0));
        if (vocabulary.transform(descriptors).empty())
            throw std::runtime_error("Shipped ORB vocabulary produced no words");
    }
} // namespace HDMapping::Processing
