#include "sbpl_planning_context.h"

// system includes
#include <moveit/collision_detection/world.h>
#include <eigen_conversions/eigen_msg.h>
#include <leatherman/print.h>
#include <moveit/planning_scene/planning_scene.h>
#include <moveit/robot_state/conversions.h>
#include <moveit_msgs/GetMotionPlan.h>
#include <moveit_msgs/PlanningScene.h>
#include <smpl/ros/propagation_distance_field.h>

// project includes
#include "../collision/collision_world_sbpl.h"
#include "../collision/collision_common_sbpl.h"

static const char* PP_LOGGER = "planning";

namespace smpl = sbpl::motion;

namespace moveit_msgs {

static const char* to_cstring(moveit_msgs::MoveItErrorCodes code)
{
    switch (code.val) {
    case moveit_msgs::MoveItErrorCodes::SUCCESS:
        return "SUCCESS";
    case moveit_msgs::MoveItErrorCodes::FAILURE:
        return "FAILURE";

    case moveit_msgs::MoveItErrorCodes::PLANNING_FAILED:
        return "PLANNING_FAILED";
    case moveit_msgs::MoveItErrorCodes::INVALID_MOTION_PLAN:
        return "INVALID_MOTION_PLAN";
    case moveit_msgs::MoveItErrorCodes::MOTION_PLAN_INVALIDATED_BY_ENVIRONMENT_CHANGE:
        return "MOTION_PLAN_INVALIDATED_BY_ENVIRONMENT_CHANGE";
    case moveit_msgs::MoveItErrorCodes::CONTROL_FAILED:
        return "CONTROL_FAILED";
    case moveit_msgs::MoveItErrorCodes::UNABLE_TO_AQUIRE_SENSOR_DATA:
        return "UNABLE_TO_AQUIRE_SENSOR_DATA";
    case moveit_msgs::MoveItErrorCodes::TIMED_OUT:
        return "TIMED_OUT";
    case moveit_msgs::MoveItErrorCodes::PREEMPTED:
        return "PREEMPTED";

    case moveit_msgs::MoveItErrorCodes::START_STATE_IN_COLLISION:
        return "START_STATE_IN_COLLISION";
    case moveit_msgs::MoveItErrorCodes::START_STATE_VIOLATES_PATH_CONSTRAINTS:
        return "START_STATE_VIOLATES_PATH_CONSTRAINTS";

    case moveit_msgs::MoveItErrorCodes::GOAL_IN_COLLISION:
        return "GOAL_IN_COLLISION";
    case moveit_msgs::MoveItErrorCodes::GOAL_VIOLATES_PATH_CONSTRAINTS:
        return "GOAL_VIOLATES_PATH_CONSTRAINTS";
    case moveit_msgs::MoveItErrorCodes::GOAL_CONSTRAINTS_VIOLATED:
        return "GOAL_CONSTRAINTS_VIOLATED";

    case moveit_msgs::MoveItErrorCodes::INVALID_GROUP_NAME:
        return "INVALID_GROUP_NAME";
    case moveit_msgs::MoveItErrorCodes::INVALID_GOAL_CONSTRAINTS:
        return "INVALID_GOAL_CONSTRAINTS";
    case moveit_msgs::MoveItErrorCodes::INVALID_ROBOT_STATE:
        return "INVALID_ROBOT_STATE";
    case moveit_msgs::MoveItErrorCodes::INVALID_LINK_NAME:
        return "INVALID_LINK_NAME";
    case moveit_msgs::MoveItErrorCodes::INVALID_OBJECT_NAME:
        return "INVALID_OBJECT_NAME";

    case moveit_msgs::MoveItErrorCodes::FRAME_TRANSFORM_FAILURE:
        return "FRAME_TRANSFORM_FAILURE";
    case moveit_msgs::MoveItErrorCodes::COLLISION_CHECKING_UNAVAILABLE:
        return "COLLISION_CHECKING_UNAVAILABLE";
    case moveit_msgs::MoveItErrorCodes::ROBOT_STATE_STALE:
        return "ROBOT_STATE_STALE";
    case moveit_msgs::MoveItErrorCodes::SENSOR_INFO_STALE:
        return "SENSOR_INFO_STALE";

    case moveit_msgs::MoveItErrorCodes::NO_IK_SOLUTION:
        return "NO_IK_SOLUTION";

    default:
        return "UNRECOGNIZED";
    }
}

} // namespace moveit_msgs

namespace sbpl_interface {

template <class T, class... Args>
auto make_unique(Args&&... args) -> std::unique_ptr<T> {
    return std::unique_ptr<T>(new T(args...));
}

static
bool InitPlanningParams(
    const std::string& planning_frame,
    const std::map<std::string, std::string>& config,
    smpl::PlanningParams* pp);

static
bool InitGridParams(
    const std::map<std::string, std::string>& config,
    double* grid_res_x,
    double* grid_res_y,
    double* grid_res_z,
    smpl::PlanningParams* params);

static
bool TranslateRequest(
    const planning_interface::MotionPlanRequest& req_in,
    const std::string& planner_id,
    moveit_msgs::MotionPlanRequest& req);

static
auto CreateHeuristicGrid(
    const planning_scene::PlanningScene& scene,
    const moveit_msgs::WorkspaceParameters& workspace,
    const std::string& group_name,
    double res_y,
    double res_x,
    double res_z,
    double max_distance)
    -> std::unique_ptr<sbpl::OccupancyGrid>;

static
bool GetPlanningFrameWorkspaceAABB(
    const moveit_msgs::WorkspaceParameters& workspace,
    const planning_scene::PlanningScene& scene,
    moveit_msgs::OrientedBoundingBox& aabb);

static
void CopyDistanceField(
    const sbpl::DistanceMapInterface& dfin,
    sbpl::DistanceMapInterface& dfout);

SBPLPlanningContext::SBPLPlanningContext(
    MoveItRobotModel* robot_model,
    const std::string& name,
    const std::string& group)
:
    Base(name, group),
    m_robot_model(robot_model),
    m_collision_checker(),
    m_grid(),
    m_planner()
{
    ROS_DEBUG_NAMED(PP_LOGGER, "Constructed SBPL Planning Context");
}

SBPLPlanningContext::~SBPLPlanningContext()
{
    ROS_DEBUG_NAMED(PP_LOGGER, "Destructed SBPL Planning Context");
}

bool SBPLPlanningContext::solve(planning_interface::MotionPlanResponse& res)
{
    auto& scene = getPlanningScene();
    assert(scene);

    auto robot = scene->getRobotModel();
    assert(robot);

    auto& req = getMotionPlanRequest();

    moveit_msgs::MotionPlanRequest req_msg;
    if (!TranslateRequest(req, m_planner_id, req_msg)) {
        ROS_WARN_NAMED(PP_LOGGER, "Unable to translate Motion Plan Request to SBPL Motion Plan Request");
        return false;
    }

    // Apply requested deltas/overrides to the current start state for a
    // complete start state
    auto start_state = scene->getCurrentStateUpdated(req_msg.start_state);
    if (!start_state) {
        ROS_WARN_NAMED(PP_LOGGER, "Unable to update start state with requested start state overrides");
        return false;
    }
    moveit::core::robotStateToRobotStateMsg(*start_state, req_msg.start_state);

    // Terminate early if there are no goal constraints
    if (req_msg.goal_constraints.empty()) {
        // :3
        res.trajectory_.reset(
                new robot_trajectory::RobotTrajectory(robot, getGroupName()));
        res.trajectory_->addSuffixWayPoint(*start_state, 0.0);
        res.planning_time_ = 0.0;
        res.error_code_.val = moveit_msgs::MoveItErrorCodes::SUCCESS;
        return true;
    }

    std::string why;
    if (!initSBPL(scene, *start_state, req.workspace_parameters, why)) {
        ROS_WARN_NAMED(PP_LOGGER, "Failed to initialize SBPL (%s)", why.c_str());
        res.planning_time_ = 0.0;
        res.error_code_.val = moveit_msgs::MoveItErrorCodes::PLANNING_FAILED;
        return false;
    }

    ROS_DEBUG_NAMED(PP_LOGGER, "Successfully initialized SBPL");

    // translate planning scene to planning scene message
    moveit_msgs::PlanningScene scene_msg;
    scene->getPlanningSceneMsg(scene_msg);

    moveit_msgs::MotionPlanResponse res_msg;
    if (!m_planner->solve(scene_msg, req_msg, res_msg)) {
        res.trajectory_.reset();
        res.planning_time_ = res_msg.planning_time;
        res.error_code_ = res_msg.error_code;
        return false;
    }

    ROS_DEBUG_NAMED(PP_LOGGER, "Call to solve() succeeded");

    ROS_DEBUG_NAMED(PP_LOGGER, "Create RobotTrajectory from path with %zu joint trajectory points and %zu multi-dof joint trajectory points",
            res_msg.trajectory.joint_trajectory.points.size(),
            res_msg.trajectory.multi_dof_joint_trajectory.points.size());
    robot_trajectory::RobotTrajectoryPtr traj(
            new robot_trajectory::RobotTrajectory(robot, getGroupName()));
    traj->setRobotTrajectoryMsg(*start_state, res_msg.trajectory);

    // TODO: Is there any reason to use res_msg.trajectory_start as the
    // reference state or res_msg.group_name in the above RobotTrajectory
    // constructor?

    ROS_INFO_NAMED(PP_LOGGER, "Motion Plan Response:");
    ROS_INFO_NAMED(PP_LOGGER, "  Trajectory: %zu points", traj->getWayPointCount());
    ROS_INFO_NAMED(PP_LOGGER, "  Planning Time: %0.3f seconds", res_msg.planning_time);
    ROS_INFO_NAMED(PP_LOGGER, "  Error Code: %d (%s)", res_msg.error_code.val, to_cstring(res_msg.error_code));

    res.trajectory_ = traj;
    res.planning_time_ = res_msg.planning_time;
    res.error_code_ = res_msg.error_code;
    return true;
}

bool SBPLPlanningContext::solve(
    planning_interface::MotionPlanDetailedResponse& res)
{
    ROS_INFO_NAMED(PP_LOGGER, "SBPLPlanningContext::solve(planning_interface::MotionPlanDetailedResponse&)");
    planning_interface::MotionPlanResponse simple_res;
    if (!solve(simple_res)) {
        return false;
    }

    res.trajectory_.push_back(simple_res.trajectory_);
    res.description_.push_back("sbpl_result");
    res.processing_time_.push_back(simple_res.planning_time_);
    res.error_code_ = simple_res.error_code_;
    return true;
}

bool SBPLPlanningContext::terminate()
{
    ROS_INFO_NAMED(PP_LOGGER, "SBPLPlanningContext::terminate()");
    return true;
}

void SBPLPlanningContext::clear()
{
    ROS_INFO_NAMED(PP_LOGGER, "SBPLPlanningContext::clear()");
}

bool SBPLPlanningContext::init(const std::map<std::string, std::string>& config)
{
    ROS_DEBUG_NAMED(PP_LOGGER, "Initialize SBPL Planning Context");

    // TODO: implement a way to pass down planner-specific parameters to the
    // Planner Interface from above and to query the interface for
    // expected parameters

    if (!m_robot_model->initialized()) {
        ROS_ERROR_NAMED(PP_LOGGER, "MoveIt! Robot Model is not initialized");
        return false;
    }

    // TODO: the only required parameters here should be "search", "heuristic",
    // "graph", and "shortcutter"...reframe PlanningParams to take the
    // key/value parameter mapping and determine whether it contains sufficient
    // parameters for initialization
    auto required_params =
    {
        "search",
        "heuristic",
        "graph",
        "shortcutter",

        // post-processing
        "shortcut_path",
        "interpolate_path"
    };

    // check for all required parameters
    for (auto& req_param : required_params) {
        if (config.find(req_param) == end(config)) {
            ROS_ERROR_NAMED(PP_LOGGER, "Missing parameter '%s'", req_param);
            return false;
        }
    }

    auto& search_name = config.at("search");
    auto& heuristic_name = config.at("heuristic");
    auto& graph_name = config.at("graph");
    m_planner_id = search_name + "." + heuristic_name + "." + graph_name;
    ROS_INFO("  Request planner '%s'", m_planner_id.c_str());

    m_use_grid = (heuristic_name == "bfs" || heuristic_name == "mfbfs" || heuristic_name == "bfs_egraph");

    ROS_DEBUG_NAMED(PP_LOGGER, " -> Required Parameters Found");

    smpl::PlanningParams pp;
    double grid_res_x, grid_res_y, grid_res_z;

    if (m_use_grid) {
        if (!InitGridParams(config, &grid_res_x, &grid_res_y, &grid_res_z, &pp)) {
            return false;
        }
    } else {
        grid_res_x = grid_res_y = grid_res_z = 0.0;
        pp.planning_link_sphere_radius = 0.0;
    }

    if (!InitPlanningParams(m_robot_model->planningFrame(), config, &pp)) {
        return false;
    }

    m_config = config; // save config, for science
    m_pp = pp; // save fully-initialized config

    // these parameters are for us
    m_grid_res_x = grid_res_x;
    m_grid_res_y = grid_res_y;
    m_grid_res_z = grid_res_z;

    return true;
}

bool SBPLPlanningContext::initSBPL(
    const planning_scene::PlanningSceneConstPtr& scene,
    const moveit::core::RobotState& start_state,
    const moveit_msgs::WorkspaceParameters& workspace,
    std::string& why)
{
    // Update the collision checker interface to use the complete start state
    // as the reference state
    {
        if (!m_collision_checker.init(m_robot_model, start_state, scene)) {
            why = "Failed to initialize sbpl Collision Checker "
                    "from Planning Scene and Robot Model";
            return false;
        }
    }

    // Create an occupancy grid (distance map) if required by the planner
    if (m_use_grid) {
        m_grid = CreateHeuristicGrid(
                *scene,
                workspace,
                m_robot_model->planningGroupName(),
                m_grid_res_x,
                m_grid_res_y,
                m_grid_res_z,
                m_pp.planning_link_sphere_radius);
        if (!m_grid) {
            why = "Failed to initialize heuristic information";
            return false;
        }
    }

    m_planner = std::make_shared<smpl::PlannerInterface>(
            m_robot_model, &m_collision_checker, m_grid.get());

    if (!m_planner->init(m_pp)) {
        why = "Failed to initialize Planner Interface";
        return false;
    }

    return true;
}

bool InitPlanningParams(
    const std::string& planning_frame,
    const std::map<std::string, std::string>& config,
    smpl::PlanningParams* pp)
{
    pp->planning_frame = planning_frame;

    //////////////////////////////////
    // parse state space parameters //
    //////////////////////////////////

    // NOTE: default cost function parameters

    //////////////////////////////////////
    // parse post-processing parameters //
    //////////////////////////////////////

    typedef std::unordered_map<std::string, smpl::ShortcutType> ShortcutTypeNameToValueMap;
    const ShortcutTypeNameToValueMap shortcut_name_to_value =
    {
        { "joint_space", smpl::ShortcutType::JOINT_SPACE },
        { "joint_position_velocity_space", smpl::ShortcutType::JOINT_POSITION_VELOCITY_SPACE },
        { "workspace", smpl::ShortcutType::EUCLID_SPACE },
    };
    const std::string default_shortcut_type = "joint_space";

    pp->shortcut_path = config.at("shortcut_path") == "true";
    pp->shortcut_type = shortcut_name_to_value.at(default_shortcut_type);
    if (pp->shortcut_path) {
        auto it = config.find("shortcutter");
        if (it != end(config)) {
            auto svit = shortcut_name_to_value.find(it->second);
            if (svit == end(shortcut_name_to_value)) {
                ROS_WARN_NAMED(PP_LOGGER, "parameter 'shortcutter' has unrecognized value. recognized values are:");
                for (auto& entry : shortcut_name_to_value) {
                    ROS_WARN_NAMED(PP_LOGGER, "  %s", entry.first.c_str());
                }
                ROS_WARN_NAMED(PP_LOGGER, "defaulting to '%s'", default_shortcut_type.c_str());
            }
            else {
                pp->shortcut_type = svit->second;
            }
        }
        else {
            ROS_WARN_NAMED(PP_LOGGER, "parameter 'shortcutter' not found. defaulting to '%s'", default_shortcut_type.c_str());
        }
    }
    pp->interpolate_path = config.at("interpolate_path") == "true";

    //////////////////////////////
    // parse logging parameters //
    //////////////////////////////

    {
        auto it = config.find("plan_output_dir");
        if (it != end(config)) {
            pp->plan_output_dir = it->second;
        } else {
            pp->plan_output_dir.clear();
        }
    }

    //////////////////////////////////////////////
    // initialize structures against parameters //
    //////////////////////////////////////////////

    for (auto& entry : config) {
        pp->addParam(entry.first, entry.second);
    }

    return true;
}

bool InitGridParams(
    const std::map<std::string, std::string>& config,
    double* grid_res_x,
    double* grid_res_y,
    double* grid_res_z,
    smpl::PlanningParams* params)
{
    const std::vector<std::string>& grid_required_params =
    {
        "bfs_res_x",
        "bfs_res_y",
        "bfs_res_z",
        "bfs_sphere_radius"
    };

    for (const std::string& req_param : grid_required_params) {
        if (config.find(req_param) == end(config)) {
            ROS_ERROR_NAMED(PP_LOGGER, "Missing parameter '%s'", req_param.c_str());
            return false;
        }
    }

    ////////////////////////////////
    // parse heuristic parameters //
    ////////////////////////////////

    params->planning_link_sphere_radius = 0.0;
    try {
        *grid_res_x = std::stod(config.at("bfs_res_x"));
        *grid_res_y = std::stod(config.at("bfs_res_y"));
        *grid_res_z = std::stod(config.at("bfs_res_z"));
        params->planning_link_sphere_radius = std::stod(config.at("bfs_sphere_radius"));

        if (*grid_res_x != *grid_res_y || *grid_res_x != *grid_res_z) {
            ROS_WARN_NAMED(PP_LOGGER, "Distance field currently only supp->rts uniformly discretized grids. Using x resolution (%0.3f) as resolution for all dimensions", *grid_res_x);
        }
    } catch (const std::logic_error& ex) { // thrown by std::stod
        ROS_ERROR_NAMED(PP_LOGGER, "Failed to convert grid resolutions to floating-point values");
        return false;
    }

    return true;
}


// Make any necessary corrections to the motion plan request to conform to
// smpl::PlannerInterface conventions
bool TranslateRequest(
    const planning_interface::MotionPlanRequest& req_in,
    const std::string& planner_id,
    moveit_msgs::MotionPlanRequest& req)
{
    // TODO: translate goal position constraints into planning frame
    // TODO: translate goal orientation constraints into planning frame
    req = req_in;
    req.planner_id = planner_id;
    return true;
}

bool GetPlanningFrameWorkspaceAABB(
    const moveit_msgs::WorkspaceParameters& workspace,
    const planning_scene::PlanningScene& scene,
    moveit_msgs::OrientedBoundingBox& aabb)
{
    if (!scene.knowsFrameTransform(workspace.header.frame_id)) {
        ROS_ERROR("Frame '%s' is not known to the Planning Scene", workspace.header.frame_id.c_str());
        return false;
    }
    if (!scene.knowsFrameTransform(scene.getPlanningFrame())) {
        ROS_ERROR("Frame '%s' is not known to the Planning Scene", scene.getPlanningFrame().c_str());
        return false;
    }

    auto& T_scene_workspace = scene.getFrameTransform(workspace.header.frame_id);
    auto& T_scene_planning = scene.getFrameTransform(scene.getPlanningFrame());

    Eigen::Affine3d T_planning_workspace =
            T_scene_planning.inverse() * T_scene_workspace;

    // l = left, r = right, n = near, f = far, b = bottom, t = top

    // w = workspace
    Eigen::Vector3d cw[8] = {
        Eigen::Vector3d(workspace.min_corner.x, workspace.min_corner.y, workspace.min_corner.z),
        Eigen::Vector3d(workspace.min_corner.x, workspace.min_corner.y, workspace.max_corner.z),
        Eigen::Vector3d(workspace.min_corner.x, workspace.max_corner.y, workspace.min_corner.z),
        Eigen::Vector3d(workspace.min_corner.x, workspace.max_corner.y, workspace.max_corner.z),
        Eigen::Vector3d(workspace.max_corner.x, workspace.min_corner.y, workspace.min_corner.z),
        Eigen::Vector3d(workspace.max_corner.x, workspace.min_corner.y, workspace.max_corner.z),
        Eigen::Vector3d(workspace.max_corner.x, workspace.max_corner.y, workspace.min_corner.z),
        Eigen::Vector3d(workspace.max_corner.x, workspace.max_corner.y, workspace.max_corner.z),
    };

    // p = planning
    Eigen::Vector3d cp[8] = {
        T_planning_workspace * cw[0],
        T_planning_workspace * cw[1],
        T_planning_workspace * cw[2],
        T_planning_workspace * cw[3],
        T_planning_workspace * cw[4],
        T_planning_workspace * cw[5],
        T_planning_workspace * cw[6],
        T_planning_workspace * cw[7],
    };
    Eigen::Vector3d min_planning = cp[0];
    Eigen::Vector3d max_planning = cp[0];
    for (int i = 1; i < 8; ++i) {
        min_planning.x() = std::min(min_planning.x(), cp[i].x());
        min_planning.y() = std::min(min_planning.y(), cp[i].y());
        min_planning.z() = std::min(min_planning.z(), cp[i].z());

        max_planning.x() = std::max(max_planning.x(), cp[i].x());
        max_planning.y() = std::max(max_planning.y(), cp[i].y());
        max_planning.z() = std::max(max_planning.z(), cp[i].z());
    }

    auto mid_x = 0.5 * (min_planning.x() + max_planning.x());
    auto mid_y = 0.5 * (min_planning.y() + max_planning.y());
    auto mid_z = 0.5 * (min_planning.z() + max_planning.z());

    auto size_x = fabs(max_planning.x() - min_planning.x());
    auto size_y = fabs(max_planning.y() - min_planning.y());
    auto size_z = fabs(max_planning.z() - min_planning.z());

    aabb.pose.position.x = mid_x;
    aabb.pose.position.y = mid_y;
    aabb.pose.position.z = mid_z;
    aabb.pose.orientation.w = 1.0;
    aabb.extents.x = size_x;
    aabb.extents.y = size_y;
    aabb.extents.z = size_z;

    return true;
}

auto CreateHeuristicGrid(
    const planning_scene::PlanningScene& scene,
    const moveit_msgs::WorkspaceParameters& workspace,
    const std::string& group_name,
    double res_y,
    double res_x,
    double res_z,
    double max_distance)
    -> std::unique_ptr<sbpl::OccupancyGrid>
{
    // create a distance field in the planning frame that represents the
    // workspace boundaries

    /////////////////////////////////////////
    // Determine Distance Field Parameters //
    /////////////////////////////////////////

    moveit_msgs::OrientedBoundingBox workspace_aabb;
    if (!GetPlanningFrameWorkspaceAABB(workspace, scene, workspace_aabb)) {
        ROS_ERROR_NAMED(PP_LOGGER, "Failed to get workspace boundaries in the planning frame");
        return NULL;
    }

    ROS_DEBUG_NAMED(PP_LOGGER, "AABB of workspace in planning frame:");
    ROS_DEBUG_NAMED(PP_LOGGER, "  pose:");
    ROS_DEBUG_NAMED(PP_LOGGER, "    position: (%0.3f, %0.3f, %0.3f)", workspace_aabb.pose.position.x, workspace_aabb.pose.position.y, workspace_aabb.pose.position.z);
    ROS_DEBUG_NAMED(PP_LOGGER, "    orientation: (%0.3f, %0.3f, %0.3f, %0.3f)", workspace_aabb.pose.orientation.w, workspace_aabb.pose.orientation.x, workspace_aabb.pose.orientation.y, workspace_aabb.pose.orientation.z);

    // TODO: block off sections of the aabb that do not include the original
    // workspace

    auto size_x = workspace_aabb.extents.x;
    auto size_y = workspace_aabb.extents.y;
    auto size_z = workspace_aabb.extents.z;

    Eigen::Affine3d T_planning_workspace;
    T_planning_workspace = Eigen::Translation3d(
            workspace_aabb.pose.position.x - 0.5 * workspace_aabb.extents.x,
            workspace_aabb.pose.position.y - 0.5 * workspace_aabb.extents.y,
            workspace_aabb.pose.position.z - 0.5 * workspace_aabb.extents.z);

    Eigen::Vector3d workspace_pos_in_planning(T_planning_workspace.translation());

    ROS_DEBUG_NAMED(PP_LOGGER, "Initialize workspace distance field:");
    ROS_DEBUG_NAMED(PP_LOGGER, "  size_x: %0.3f", size_x);
    ROS_DEBUG_NAMED(PP_LOGGER, "  size_y: %0.3f", size_y);
    ROS_DEBUG_NAMED(PP_LOGGER, "  size_z: %0.3f", size_z);
    ROS_DEBUG_NAMED(PP_LOGGER, "  res: %0.3f", res_x);
    ROS_DEBUG_NAMED(PP_LOGGER, "  origin_x: %0.3f", workspace_pos_in_planning.x());
    ROS_DEBUG_NAMED(PP_LOGGER, "  origin_y: %0.3f", workspace_pos_in_planning.y());
    ROS_DEBUG_NAMED(PP_LOGGER, "  origin_z: %0.3f", workspace_pos_in_planning.z());

    auto hdf = std::make_shared<sbpl::PropagationDistanceField>(
            workspace_pos_in_planning.x(),
            workspace_pos_in_planning.y(),
            workspace_pos_in_planning.z(),
            size_x, size_y, size_z,
            res_x,
            max_distance);

    ////////////////////////////////////////////////////////
    // Try to Copy Distance Field from CollisionWorldSBPL //
    ////////////////////////////////////////////////////////

    auto cworld = scene.getCollisionWorld();

    using collision_detection::CollisionWorldSBPL;
    auto* sbpl_cworld = dynamic_cast<const CollisionWorldSBPL*>(cworld.get());

    if (sbpl_cworld != NULL) {
        ROS_DEBUG_NAMED(PP_LOGGER, "Use collision information from Collision World SBPL for heuristic!!!");

        auto* df = sbpl_cworld->distanceField(scene.getRobotModel()->getName(), group_name);
        if (df != NULL) {
            // copy the collision information
            // TODO: The distance field at this point should contain the
            // planning scene world, but should probably add an explicit
            // function to force an update
            ROS_DEBUG_NAMED(PP_LOGGER, "Copy collision information");
            CopyDistanceField(*df, *hdf);

            ROS_INFO_NAMED(PP_LOGGER, "Successfully initialized heuristic grid from sbpl collision checker");
            auto grid = make_unique<sbpl::OccupancyGrid>(hdf);
            grid->setReferenceFrame(scene.getPlanningFrame());
            return std::move(grid);
        } else {
            ROS_WARN_NAMED(PP_LOGGER, "Just kidding! Collision World SBPL's distance field is uninitialized");
        }
    }

    ///////////////////////////////
    // Create New Distance Field //
    ///////////////////////////////

    // TODO: the collision checker might be mature enough to consider
    // instantiating a full cspace here and using available voxels state
    // information for a more accurate heuristic

    auto grid = make_unique<sbpl::OccupancyGrid>(hdf);
    grid->setReferenceFrame(scene.getPlanningFrame());

    // temporary storage for collision shapes/objects
    std::vector<std::unique_ptr<sbpl::collision::CollisionShape>> shapes;
    std::vector<std::unique_ptr<sbpl::collision::CollisionObject>> collision_objects;
    sbpl::collision::WorldCollisionModel cmodel(grid.get());

    // insert world objects into the collision model
    auto& world = cworld->getWorld();
    if (world) {
        int insert_count = 0;
        for (auto oit = world->begin(); oit != world->end(); ++oit) {

            collision_objects.push_back(std::unique_ptr<sbpl::collision::CollisionObject>());
            ConvertObjectToCollisionObjectShallow(oit->second, shapes, collision_objects.back());
            auto& collision_object = collision_objects.back();

            if (!cmodel.insertObject(collision_object.get())) {
                ROS_WARN_NAMED(PP_LOGGER, "Failed to insert object '%s' into heuristic grid", oit->first.c_str());
            }
            else {
                ++insert_count;
            }
        }
        ROS_DEBUG_NAMED(PP_LOGGER, "Inserted %d objects into the heuristic grid", insert_count);
    }
    else {
        ROS_WARN_NAMED(PP_LOGGER, "Attempt to insert null World into heuristic grid");
    }

    // note: collision world and going out of scope here will
    // not destroy the prepared distance field and occupancy grid

    return std::move(grid);
}

void CopyDistanceField(
    const sbpl::DistanceMapInterface& dfin,
    sbpl::DistanceMapInterface& dfout)
{
    std::vector<Eigen::Vector3d> points;
    for (int x = 0; x < dfout.numCellsX(); ++x) {
    for (int y = 0; y < dfout.numCellsY(); ++y) {
    for (int z = 0; z < dfout.numCellsZ(); ++z) {
        double wx, wy, wz;
        dfout.gridToWorld(x, y, z, wx, wy, wz);
        int gx, gy, gz;
        dfin.worldToGrid(wx, wy, wz, gx, gy, gz);
        if (!dfin.isCellValid(gx, gy, gz)) {
            points.emplace_back(wx, wy, wz);
        } else if (dfin.getCellDistance(gx, gy, gz) <= 0.0) {
            points.emplace_back(wx, wy, wz);
        }
    }
    }
    }

    ROS_DEBUG_NAMED(PP_LOGGER, "Add %zu points to the distance field", points.size());
    dfout.addPointsToMap(points);
}

} // namespace sbpl_interface
