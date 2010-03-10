/*********************************************************************
 *
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2008, Willow Garage, Inc.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of the Willow Garage nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 *  \author Sachin Chitta, Ioan Sucan
 *********************************************************************/

#include <ros/ros.h>

#include <tf/tf.h>
#include <tf/transform_listener.h>

#include <actionlib/client/action_client.h>
#include <pr2_controllers_msgs/JointTrajectoryAction.h>

#include <actionlib/server/simple_action_server.h>
#include <move_arm_msgs/MoveArmAction.h>

#include <trajectory_msgs/JointTrajectory.h>
#include <kinematics_msgs/GetConstraintAwarePositionIK.h>
#include <kinematics_msgs/GetPositionFK.h>

#include <motion_planning_msgs/FilterJointTrajectoryWithConstraints.h>
#include <geometric_shapes_msgs/Shape.h>
#include <motion_planning_msgs/DisplayTrajectory.h>
#include <motion_planning_msgs/GetMotionPlan.h>
#include <motion_planning_msgs/ConvertToJointConstraint.h>
#include <motion_planning_msgs/convert_messages.h>

#include <visualization_msgs/Marker.h>

#include <planning_environment/util/construct_object.h>
#include <planning_environment/monitors/joint_state_monitor.h>
#include <geometric_shapes/bodies.h>

#include <planning_environment_msgs/GetRobotState.h>
#include <planning_environment_msgs/GetJointTrajectoryValidity.h>
#include <planning_environment_msgs/GetStateValidity.h>
#include <planning_environment_msgs/GetJointsInGroup.h>
#include <planning_environment_msgs/GetEnvironmentSafety.h>
#include <planning_environment_msgs/SetConstraints.h>

#include <visualization_msgs/MarkerArray.h>

#include <std_msgs/Bool.h>

#include <valarray>
#include <algorithm>
#include <cstdlib>

typedef actionlib::ActionClient<pr2_controllers_msgs::JointTrajectoryAction> JointExecutorActionClient;

namespace move_arm
{

enum MoveArmState {
  PLANNING,
  START_CONTROL,
  VISUALIZE_PLAN,
  MONITOR
};

enum ControllerStatus {
  QUEUED,
  ACTIVE,
  SUCCESS,
  FAILED
};

enum EnvironmentServerChecks{
  COLLISION_TEST        = 1,
  PATH_CONSTRAINTS_TEST = 2,
  GOAL_CONSTRAINTS_TEST = 4,
  JOINT_LIMITS_TEST     = 8,
  CHECK_FULL_TRAJECTORY = 16
};	

typedef struct{
  bool accept_partial_plans;
  bool accept_invalid_goals;
  bool disable_ik;
  bool disable_collision_monitoring;
  bool is_pose_goal;
  double allowed_planning_time;
  std::string planner_service_name;
} MoveArmParameters;

static const std::string ARM_IK_NAME             = "arm_ik";
static const std::string ARM_FK_NAME             = "arm_fk";
static const std::string TRAJECTORY_FILTER = "filter_trajectory";
static const std::string DISPLAY_PATH_PUB_TOPIC  = "display_path";
static const std::string DISPLAY_JOINT_GOAL_PUB_TOPIC  = "display_joint_goal";

class MoveArm
{
public:	

  MoveArm(const std::string &group_name) :  
    group_(group_name), 
    private_handle_("~")
  {
    private_handle_.param<double>("move_arm_frequency",move_arm_frequency_, 50.0);
    private_handle_.param<double>("trajectory_filter_allowed_time",trajectory_filter_allowed_time_, 2.0);
    private_handle_.param<double>("ik_allowed_time",ik_allowed_time_, 2.0);

    num_planning_attempts_ = 0;
    state_ = PLANNING;

    ik_client_ = root_handle_.serviceClient<kinematics_msgs::GetConstraintAwarePositionIK>(ARM_IK_NAME);

    check_plan_validity_client_ = root_handle_.serviceClient<planning_environment_msgs::GetJointTrajectoryValidity>("get_trajectory_validity");
    check_env_safe_client_ = root_handle_.serviceClient<planning_environment_msgs::GetEnvironmentSafety>("get_environment_safety");
    check_state_validity_client_ = root_handle_.serviceClient<planning_environment_msgs::GetStateValidity>("get_state_validity");
    check_execution_safe_client_ = root_handle_.serviceClient<planning_environment_msgs::GetJointTrajectoryValidity>("get_execution_safety");
    get_joints_in_group_client_ = root_handle_.serviceClient<planning_environment_msgs::GetJointsInGroup>("get_joints_in_group");      
    get_state_client_ = root_handle_.serviceClient<planning_environment_msgs::GetRobotState>("get_robot_state");      
    allowed_contact_regions_publisher_ = root_handle_.advertise<visualization_msgs::MarkerArray>("allowed_contact_regions_array", 128);
    filter_trajectory_client_ = root_handle_.serviceClient<motion_planning_msgs::FilterJointTrajectoryWithConstraints>("filter_trajectory");      

    ros::service::waitForService(ARM_IK_NAME);
    ros::service::waitForService("get_trajectory_validity");
    ros::service::waitForService("get_environment_safety");
    ros::service::waitForService("get_state_validity");
    ros::service::waitForService("get_execution_safety");
    ros::service::waitForService("get_joints_in_group");
    ros::service::waitForService("get_robot_state");
    ros::service::waitForService("filter_trajectory");

    action_server_.reset(new actionlib::SimpleActionServer<move_arm_msgs::MoveArmAction>(root_handle_, "move_" + group_name, boost::bind(&MoveArm::execute, this, _1)));

    display_path_publisher_ = root_handle_.advertise<motion_planning_msgs::DisplayTrajectory>(DISPLAY_PATH_PUB_TOPIC, 1);
    display_joint_goal_publisher_ = root_handle_.advertise<motion_planning_msgs::DisplayTrajectory>(DISPLAY_JOINT_GOAL_PUB_TOPIC, 1);
    //        fk_client_ = root_handle_.serviceClient<kinematics_msgs::FKService>(ARM_FK_NAME);
  }	
  virtual ~MoveArm()
  {
  }

  bool configure()
  {
    if(!initializeControllerInterface())
    {
      ROS_ERROR("Could not initialize controller interface");
      return false;
    }
    if(!state_monitor_.active_)
    {
      ROS_ERROR("Joint state monitor is not active, aborting.");
      return false;
    }          

    if (group_.empty())
    {
      ROS_ERROR("No 'group' parameter specified. Without the name of the group of joints to plan for, action cannot start");
      return false;
    }
    else
    {
      planning_environment_msgs::GetJointsInGroup::Request req;
      planning_environment_msgs::GetJointsInGroup::Response res;
      req.group_name = group_;
      if(get_joints_in_group_client_.call(req,res))
      {
        if(!res.joint_names.empty())
        {
          group_joint_names_ = res.joint_names;
          return true;
        }
        else
        {
          ROS_ERROR("Could not get the list of joint names in the group: %s",
                    group_.c_str());
          return false;
        }
      }
      else
      {
        ROS_ERROR("Service call to find list of joint names failed on %s",
                  get_joints_in_group_client_.getService().c_str());
        return false;
      }
    }
    return true;
  }
	
private:

  ///
  /// Kinematics
  /// 
  bool convertPoseGoalToJointGoal(motion_planning_msgs::GetMotionPlan::Request &req)
  {
    ROS_DEBUG("Acting on goal to pose ...");// we do IK to find corresponding states
    ROS_DEBUG("Position constraint: %f %f %f",
             req.motion_plan_request.goal_constraints.position_constraints[0].position.x,
             req.motion_plan_request.goal_constraints.position_constraints[0].position.y,
              req.motion_plan_request.goal_constraints.position_constraints[0].position.z);
    ROS_DEBUG("Orientation constraint: %f %f %f %f",
             req.motion_plan_request.goal_constraints.orientation_constraints[0].orientation.x,
             req.motion_plan_request.goal_constraints.orientation_constraints[0].orientation.y,
             req.motion_plan_request.goal_constraints.orientation_constraints[0].orientation.z,
             req.motion_plan_request.goal_constraints.orientation_constraints[0].orientation.w);

    geometry_msgs::PoseStamped tpose = motion_planning_msgs::poseConstraintsToPoseStamped(req.motion_plan_request.goal_constraints.position_constraints[0],
                                                                                          req.motion_plan_request.goal_constraints.orientation_constraints[0]);
    std::string link_name = req.motion_plan_request.goal_constraints.position_constraints[0].link_name;
    sensor_msgs::JointState solution;		

    ROS_INFO("IK request");
    ROS_INFO("link_name   : %s",tpose.header.frame_id.c_str());
    ROS_INFO("frame_id    : %s",tpose.header.frame_id.c_str());
    ROS_INFO("position    : (%f,%f,%f)",tpose.pose.position.x,tpose.pose.position.y,tpose.pose.position.z);
    ROS_INFO("orientation : (%f,%f,%f,%f)",tpose.pose.orientation.x,tpose.pose.orientation.y,tpose.pose.orientation.z,tpose.pose.orientation.w);
    ROS_INFO(" ");
    if (computeIK(tpose, 
                  link_name, 
                  solution))
    {
      /*if(!checkIK(tpose,link_name,solution))
         ROS_ERROR("IK solution does not get to desired pose");
      */
      for (unsigned int i = 0 ; i < solution.name.size() ; ++i)
      {
        motion_planning_msgs::JointConstraint jc;
        jc.joint_name = solution.name[i];
        jc.position = solution.position[i];
        jc.tolerance_below = 0.1;
        jc.tolerance_above = 0.1;
        req.motion_plan_request.goal_constraints.joint_constraints.push_back(jc);
      }
      req.motion_plan_request.goal_constraints.position_constraints.clear();
      req.motion_plan_request.goal_constraints.orientation_constraints.clear();	    
      return true;
    }
    else
      return false;
  }

  bool computeIK(const geometry_msgs::PoseStamped &pose_stamped_msg,  
                 const std::string &link_name, 
                 sensor_msgs::JointState &solution)
  {
    kinematics_msgs::GetConstraintAwarePositionIK::Request request;
    kinematics_msgs::GetConstraintAwarePositionIK::Response response;
	    
    request.ik_request.pose_stamped = pose_stamped_msg;
    request.ik_request.ik_seed_state.joint_state.name = group_joint_names_;	    
    request.ik_request.ik_link_name = link_name;
    request.ik_request.ik_seed_state.joint_state = state_monitor_.getJointState(group_joint_names_);	    
    request.timeout = ros::Duration(ik_allowed_time_);
    request.ordered_collision_operations = original_request_.motion_plan_request.ordered_collision_operations;
    request.allowed_contacts = original_request_.motion_plan_request.allowed_contacts;
    request.constraints = original_request_.motion_plan_request.goal_constraints;
    if (ik_client_.call(request, response))
    {
      move_arm_action_result_.error_code = response.error_code;
      if(response.error_code.val != response.error_code.SUCCESS)
      {
        ROS_ERROR("IK Solution not found, IK returned with error_code: %d",response.error_code.val);
        return false;
      }         
      solution = response.solution.joint_state;
      if (solution.position.size() != request.ik_request.ik_seed_state.joint_state.name.size())
      {
        ROS_ERROR("Incorrect number of elements in IK output.");
        return false;
      }
      for(unsigned int i = 0; i < solution.position.size() ; ++i)
        ROS_DEBUG("IK[%d] = %f", (int)i, solution.position[i]);
    }
    else
    {
      ROS_ERROR("IK service failed");
      return false;
    }	    
    return true;
  }

  bool checkIK(const geometry_msgs::PoseStamped &pose_stamped_msg,  
               const std::string &link_name, 
               sensor_msgs::JointState &solution)
  {
    kinematics_msgs::GetPositionFK::Request request;
    kinematics_msgs::GetPositionFK::Response response;
	    
    request.robot_state.joint_state.name = group_joint_names_;	    
    request.fk_link_names.resize(1);
    request.fk_link_names[0] = link_name;
    request.robot_state.joint_state.position = solution.position;	    
    request.header = pose_stamped_msg.header;
    if (fk_client_.call(request, response))
    {
      if(response.error_code.val != response.error_code.SUCCESS)
        return false;
      ROS_DEBUG("Obtained FK solution");
      ROS_DEBUG("FK Pose:");
      ROS_DEBUG("Position : (%f,%f,%f)",
                response.pose_stamped[0].pose.position.x,
                response.pose_stamped[0].pose.position.y,
                response.pose_stamped[0].pose.position.z);
      ROS_DEBUG("Rotation : (%f,%f,%f,%f)",
                response.pose_stamped[0].pose.orientation.x,
                response.pose_stamped[0].pose.orientation.y,
                response.pose_stamped[0].pose.orientation.z,
                response.pose_stamped[0].pose.orientation.w);
      ROS_DEBUG(" ");
    }
    else
    {
      ROS_ERROR("FK service failed");
      return false;
    }	    
    return true;
  }
  ///
  /// End Kinematics
  /// 

  ///
  /// Trajectory Filtering
  ///
  bool filterTrajectory(const trajectory_msgs::JointTrajectory &trajectory_in, 
                        trajectory_msgs::JointTrajectory &trajectory_out)
  {
    motion_planning_msgs::FilterJointTrajectoryWithConstraints::Request  req;
    motion_planning_msgs::FilterJointTrajectoryWithConstraints::Response res;
    fillTrajectoryMsg(trajectory_in, req.filter_request.trajectory);
    req.filter_request.allowed_contacts = original_request_.motion_plan_request.allowed_contacts;
    req.filter_request.ordered_collision_operations = original_request_.motion_plan_request.ordered_collision_operations;
    req.filter_request.path_constraints = original_request_.motion_plan_request.path_constraints;
    req.filter_request.goal_constraints = original_request_.motion_plan_request.goal_constraints;
    req.filter_request.allowed_time = ros::Duration(trajectory_filter_allowed_time_);
    if(filter_trajectory_client_.call(req,res))
    {
      trajectory_out = res.trajectory;
      return true;
    }
    else
    {
      ROS_ERROR("Service call to filter trajectory failed.");
      return false;
    }
  }
  ///
  /// End Trajectory Filtering
  ///

  ///
  /// State and trajectory validity checks
  ///
  /** \brief Check the joint goal if it is valid */
  bool checkJointGoal(motion_planning_msgs::GetMotionPlan::Request &req)
  {
    ROS_DEBUG("Checking validity of joint goal");
    if (isStateValid(motion_planning_msgs::jointConstraintsToJointState(req.motion_plan_request.goal_constraints.joint_constraints),
                     JOINT_LIMITS_TEST))
    {
      ROS_DEBUG("Joint goal passed joint limits test");
      return true;
    }
    else
    {
      if(move_arm_parameters_.accept_invalid_goals)
        return true;
      else
      {
        ROS_ERROR("Will not plan to requested joint goal since it violates joint limits constraints");
        move_arm_action_result_.error_code.val = move_arm_action_result_.error_code.JOINT_LIMITS_VIOLATED;
        return false;
      }
    }
  }
  bool isEnvironmentSafe()
  {
    planning_environment_msgs::GetEnvironmentSafety::Request req;
    planning_environment_msgs::GetEnvironmentSafety::Response res;
    if(check_env_safe_client_.call(req,res))
    {
      if(res.error_code.val == res.error_code.SUCCESS)
        return true;
      else
        return false;
    }
    else
    {
      ROS_ERROR("Service call to environment server failed on %s",check_env_safe_client_.getService().c_str());
      return false;
    }
  }
  bool isTrajectoryValid(const trajectory_msgs::JointTrajectory &traj)
  {
    planning_environment_msgs::GetJointTrajectoryValidity::Request req;
    planning_environment_msgs::GetJointTrajectoryValidity::Response res;
        
    ROS_DEBUG("Received trajectory has %d points with %d joints",
              (int) traj.points.size(),
              (int)traj.joint_names.size());

    req.trajectory = traj;
    if(!getRobotState(req.robot_state))
    {
      ROS_ERROR("Could not get robot state");
      return false;
    }

    req.check_path_constraints = true;
    req.check_collisions = true;
    req.allowed_contacts = original_request_.motion_plan_request.allowed_contacts;
    req.ordered_collision_operations = original_request_.motion_plan_request.ordered_collision_operations;
    req.path_constraints = original_request_.motion_plan_request.path_constraints;
    req.goal_constraints = original_request_.motion_plan_request.goal_constraints;
 
    if(check_plan_validity_client_.call(req,res))
    {
      if(res.error_code.val == res.error_code.SUCCESS)
        return true;
      else
      {
        move_arm_action_result_.error_code = res.error_code;
        ROS_ERROR("Trajectory invalid");
        return false;
      }
    }
    else
    {
      ROS_ERROR("Service call to check trajectory validity failed on %s",check_plan_validity_client_.getService().c_str());
      return false;
    }
  }
  bool isExecutionSafe()
  {
    planning_environment_msgs::GetJointTrajectoryValidity::Request req;
    planning_environment_msgs::GetJointTrajectoryValidity::Response res;
        
    req.trajectory = current_trajectory_;
    if(!getRobotState(req.robot_state))
      return false;
    req.check_path_constraints = true;
    req.check_collisions = true;
    req.allowed_contacts = original_request_.motion_plan_request.allowed_contacts;
    req.ordered_collision_operations = original_request_.motion_plan_request.ordered_collision_operations;
    req.path_constraints = original_request_.motion_plan_request.path_constraints;
    req.goal_constraints = original_request_.motion_plan_request.goal_constraints;
    if(check_execution_safe_client_.call(req,res))
    {
      if(res.error_code.val == res.error_code.SUCCESS)
        return true;
      else
      {
        move_arm_action_result_.error_code = res.error_code;
        return false;
      }
    }
    else
    {
      ROS_ERROR("Service call to check execution safety failed on %s",
                check_execution_safe_client_.getService().c_str());
      return false;
    }
  }
  bool isStateValidAtGoal(const sensor_msgs::JointState &joint_state)
  {
    planning_environment_msgs::GetStateValidity::Request req;
    planning_environment_msgs::GetStateValidity::Response res;
    req.robot_state.joint_state = joint_state;
    req.robot_state.joint_state.header.stamp = ros::Time::now();
    req.check_goal_constraints = true;
    req.check_collisions = true;
    req.allowed_contacts = original_request_.motion_plan_request.allowed_contacts;
    req.ordered_collision_operations = original_request_.motion_plan_request.ordered_collision_operations;
    req.path_constraints = original_request_.motion_plan_request.path_constraints;
    req.goal_constraints = original_request_.motion_plan_request.goal_constraints;
    if(check_state_validity_client_.call(req,res))
    {
      if(res.error_code.val == res.error_code.SUCCESS)
        return true;
      else
        return false;
    }
    else
    {
      ROS_ERROR("Service call to check goal validity failed %s",
                check_state_validity_client_.getService().c_str());
      return false;
    }
  }
  bool isStateValid(const sensor_msgs::JointState &joint_state, 
                    int flag=0)
  {
    planning_environment_msgs::GetStateValidity::Request req;
    planning_environment_msgs::GetStateValidity::Response res;
    req.robot_state.joint_state = joint_state;
    req.robot_state.joint_state.header.stamp = ros::Time::now();        
    addCheckFlags(req,COLLISION_TEST | flag);
    req.allowed_contacts = original_request_.motion_plan_request.allowed_contacts;
    req.ordered_collision_operations = original_request_.motion_plan_request.ordered_collision_operations;
    req.path_constraints = original_request_.motion_plan_request.path_constraints;
    req.goal_constraints = original_request_.motion_plan_request.goal_constraints;
    if(check_state_validity_client_.call(req,res))
    {
      if(res.error_code.val == res.error_code.SUCCESS)
        return true;
      else
      {
        move_arm_action_result_.error_code = res.error_code;
        return false;
      }
    }
    else
    {
      ROS_ERROR("Service call to check state validity failed on %s",check_state_validity_client_.getService().c_str());
      return false;
    }
  }
  ///
  /// State and trajectory validity checks
  ///

  ///
  /// Helper functions
  ///
  bool isPoseGoal(motion_planning_msgs::GetMotionPlan::Request &req)
  {
    if (req.motion_plan_request.goal_constraints.joint_constraints.empty() &&         // we have no joint constraints on the goal,
        req.motion_plan_request.goal_constraints.position_constraints.size() == 1 &&      // we have a single position constraint on the goal
        req.motion_plan_request.goal_constraints.orientation_constraints.size() ==  1)  // that is active on all 6 DOFs
      return true;
    else
      return false;
  }       
  bool isJointGoal(motion_planning_msgs::GetMotionPlan::Request &req)
  {
    if (req.motion_plan_request.goal_constraints.position_constraints.empty() && 
        req.motion_plan_request.goal_constraints.orientation_constraints.empty() && 
        !req.motion_plan_request.goal_constraints.joint_constraints.empty())
      return true;
    else
      return false;
  }                   
  void addCheckFlags(planning_environment_msgs::GetStateValidity::Request &req, 
                     const int &flag)
  {
    if(flag & COLLISION_TEST)
      req.check_collisions = true;
    if(flag & PATH_CONSTRAINTS_TEST)
      req.check_path_constraints = true;
    if(flag & GOAL_CONSTRAINTS_TEST)
      req.check_goal_constraints = true;
    if(flag & JOINT_LIMITS_TEST)
      req.check_joint_limits = true;
  }
  bool getRobotState(motion_planning_msgs::RobotState &robot_state)
  {
    planning_environment_msgs::GetRobotState::Request req;
    planning_environment_msgs::GetRobotState::Response res;
    if(get_state_client_.call(req,res))
    {
      robot_state = res.robot_state;
      return true;
    }
    else
    {
      ROS_ERROR("Service call to get robot state failed on %s",
                get_state_client_.getService().c_str());
      return false;
    }
  }
  ///
  /// End Helper Functions
  ///

  ///
  /// Motion planning
  ///
  void moveArmGoalToPlannerRequest(const move_arm_msgs::MoveArmGoalConstPtr& goal, 
                                   motion_planning_msgs::GetMotionPlan::Request &req)
  {
    req.motion_plan_request.workspace_parameters.workspace_region_pose.header.stamp = ros::Time::now();
    req.motion_plan_request = goal->motion_plan_request;

    move_arm_parameters_.accept_partial_plans = goal->accept_partial_plans;
    move_arm_parameters_.accept_invalid_goals = goal->accept_invalid_goals;
    move_arm_parameters_.disable_ik           = goal->disable_ik;
    move_arm_parameters_.disable_collision_monitoring = goal->disable_collision_monitoring;
    move_arm_parameters_.allowed_planning_time = goal->motion_plan_request.allowed_planning_time.toSec();
    move_arm_parameters_.planner_service_name = goal->planner_service_name;
    visualizeAllowedContactRegions(req.motion_plan_request.allowed_contacts);
  }
  bool doPrePlanningChecks(motion_planning_msgs::GetMotionPlan::Request &req,  
                           motion_planning_msgs::GetMotionPlan::Response &res)
  {
    //checking current state for validity
    motion_planning_msgs::RobotState current_state;
    current_state.joint_state = state_monitor_.getJointState(group_joint_names_);
    if(!isStateValid(current_state.joint_state) && !move_arm_parameters_.disable_collision_monitoring){
      ROS_ERROR("Current state in collision.  Can't plan.");
      move_arm_action_result_.error_code.val = move_arm_action_result_.error_code.START_STATE_IN_COLLISION;
      action_server_->setAborted();
      return false;
    }
    // processing and checking goal
    if (!move_arm_parameters_.disable_ik && isPoseGoal(req))
    {
      ROS_INFO("Planning to a pose goal");
      if(!convertPoseGoalToJointGoal(req))
      {
        action_server_->setAborted();
        return false;
      }
    }
    if (isJointGoal(req))
      if(!checkJointGoal(req))
      {
        action_server_->setAborted();
        return false;
      }
    return true;
  }
  bool createPlan(motion_planning_msgs::GetMotionPlan::Request &req,  
                  motion_planning_msgs::GetMotionPlan::Response &res)
  {
    if (!isEnvironmentSafe())
    {
      ROS_WARN("Environment is not safe. Will not issue request for planning");
      return false;
    }        
    if(!getRobotState(req.motion_plan_request.start_state))
      return false;

    ros::ServiceClient planning_client = root_handle_.serviceClient<motion_planning_msgs::GetMotionPlan>(move_arm_parameters_.planner_service_name);
    ROS_DEBUG("Issuing request for motion plan");		    
    // call the planner and decide whether to use the path
    if (planning_client.call(req, res))
    {
      if (res.trajectory.joint_trajectory.points.empty())
      {
        ROS_WARN("Motion planner was unable to plan a path to goal");
        return false;
      }
      ROS_INFO("Motion planning succeeded");
      motion_planning_msgs::RobotState last_state_in_trajectory;
      last_state_in_trajectory.joint_state.header = res.trajectory.joint_trajectory.header;
      last_state_in_trajectory.joint_state.position = res.trajectory.joint_trajectory.points.back().positions;
      last_state_in_trajectory.joint_state.name = res.trajectory.joint_trajectory.joint_names;
      if(!isStateValidAtGoal(last_state_in_trajectory.joint_state))
      {
        if(move_arm_parameters_.accept_partial_plans)
        {
          ROS_WARN("Returned path from planner does not go all the way to goal");
          return true;
        }
        else
        {
          ROS_ERROR("Returned path from planner does not go all the way to goal");
          return false;
        }
      }
      return true;
    }
    else
    {
      ROS_ERROR("Motion planning service failed on %s",planning_client.getService().c_str());
      return false;
    }
  }
  ///
  /// End Motion planning
  ///

  /// 
  /// Control
  ///
  bool initializeControllerInterface()
  {
    std::string controller_action_name;
    private_handle_.param<std::string>("controller_action_name", controller_action_name, "action");
    ROS_INFO("Connecting to controller using action: %s",controller_action_name.c_str());
    controller_action_client_ = new JointExecutorActionClient(controller_action_name);
    if(!controller_action_client_) {
      ROS_ERROR("Controller action client hasn't been initialized yet");
      return false;
    }
    while(!controller_action_client_->waitForActionServerToStart(ros::Duration(1.0))){
      ROS_INFO("Waiting for the joint_trajectory_action server to come up.");
      if(!root_handle_.ok()) {
        return false;
      }
    }
    ROS_INFO("Connected to the controller");
    return true;
  }
  bool stopTrajectory()
  {
    controller_goal_handle_.cancel();
    return true;
  }
  bool sendTrajectory(trajectory_msgs::JointTrajectory current_trajectory)
  {
    pr2_controllers_msgs::JointTrajectoryGoal goal;  
    goal.trajectory = current_trajectory;
    goal.trajectory.header.stamp = ros::Time::now();
    controller_goal_handle_ = controller_action_client_->sendGoal(goal,boost::bind(&MoveArm::controllerTransitionCallback, this, _1));    
    controller_status_ = QUEUED;
    ROS_DEBUG("Send trajectory");
    return true;
  }
  void controllerTransitionCallback(JointExecutorActionClient::GoalHandle gh) 
  {   
    if(gh != controller_goal_handle_)
      return;
    actionlib::CommState comm_state = gh.getCommState();    
    switch( comm_state.state_)
    {
    case actionlib::CommState::WAITING_FOR_GOAL_ACK:
    case actionlib::CommState::PENDING:
    case actionlib::CommState::RECALLING:
      controller_status_ = QUEUED;
      return;
    case actionlib:: CommState::ACTIVE:
    case actionlib::CommState::PREEMPTING:
      controller_status_ = ACTIVE;
      return;
    case actionlib::CommState::DONE:
      {
        switch(gh.getTerminalState().state_)
        {
        case actionlib::TerminalState::RECALLED:
        case actionlib::TerminalState::REJECTED:
        case actionlib::TerminalState::PREEMPTED:
        case actionlib::TerminalState::ABORTED:
        case actionlib::TerminalState::LOST:
          controller_status_ = FAILED;
          return;
        case actionlib::TerminalState::SUCCEEDED:
          controller_status_ = SUCCESS;
          return;
        default:
          ROS_ERROR("Unknown terminal state [%u]. This is a bug in ActionClient", gh.getTerminalState().state_);
        }
      }
    default:
      break;
    }
  } 
  bool isControllerDone()
  {      
    if (controller_status_ == SUCCESS)
    {
      ROS_INFO("Completed trajectory");
      return true;
    }
    if(controller_status_ == FAILED)
    {
      move_arm_action_result_.error_code.val = move_arm_action_result_.error_code.TRAJECTORY_CONTROLLER_FAILED;
      action_server_->publishFeedback(move_arm_action_feedback_);        
      ROS_ERROR("Trajectory controller reports failure");
      resetStateMachine();
      action_server_->setAborted();
      return false;
    }
    return false;
  }
  void fillTrajectoryMsg(const trajectory_msgs::JointTrajectory &trajectory_in, 
                         trajectory_msgs::JointTrajectory &trajectory_out)
  {
    trajectory_out = trajectory_in;
    if(trajectory_in.points.empty())
    {
      ROS_WARN("No points in trajectory");
      return;
    }
    // get the current state
    double d = 0.0;
    sensor_msgs::JointState current = state_monitor_.getJointState(trajectory_out.joint_names);
    for (unsigned int i = 0 ; i < current.position.size() ; ++i)
    {
      double dif = current.position[i] - trajectory_in.points[0].positions[i];
      d += dif * dif;
    }
    d = sqrt(d);	    
    // decide whether we place the current state in front of the trajectory_in
    int include_first = (d > 0.1) ? 1 : 0;
    double offset = 0.0;
    trajectory_out.points.resize(trajectory_in.points.size() + include_first);

    if (include_first)
    {
      trajectory_out.points[0].positions = motion_planning_msgs::jointStateToJointTrajectoryPoint(current).positions;
      trajectory_out.points[0].time_from_start = ros::Duration(0.0);
      offset = 0.3 + d;
    }	    
    for (unsigned int i = 0 ; i < trajectory_in.points.size() ; ++i)
    {
      trajectory_out.points[i+include_first].time_from_start = trajectory_in.points[i].time_from_start;
      trajectory_out.points[i+include_first].positions = trajectory_in.points[i].positions;
    }
    trajectory_out.header.stamp = ros::Time::now();
  }
  /// 
  /// End Control
  ///

  ///
  /// State machine
  ///
  void resetStateMachine()
  {
    current_trajectory_.points.clear();
    current_trajectory_.joint_names.clear();
    state_ = PLANNING;
  }
  bool executeCycle(motion_planning_msgs::GetMotionPlan::Request &req)
  {
    motion_planning_msgs::GetMotionPlan::Response res;

    switch(state_)
    {
    case PLANNING:
      {
        move_arm_action_feedback_.state = "planning";
        move_arm_action_feedback_.time_to_completion = ros::Duration(req.motion_plan_request.allowed_planning_time);
        action_server_->publishFeedback(move_arm_action_feedback_);

        if(!doPrePlanningChecks(req,res))
          return true;

        visualizeJointGoal(req);
        motion_planning_msgs::RobotState current_state;
        current_state.joint_state = state_monitor_.getJointState(group_joint_names_);
        if(isStateValidAtGoal(current_state.joint_state))
        {
          resetStateMachine();
          action_server_->setSucceeded();
          ROS_INFO("Reached goal");
          return true;
        }

        if(createPlan(req,res))
        {
          ROS_DEBUG("createPlan succeeded");
          if(!isTrajectoryValid(res.trajectory.joint_trajectory))
          {
            ROS_ERROR("Trajectory returned by the planner is in collision with a part of the environment");
            ROS_ERROR("Move arm will abort this goal.");
            resetStateMachine();
            action_server_->setAborted();
            return true;
          }
          else{
            ROS_DEBUG("Trajectory validity check was successful");
          }
          current_trajectory_ = res.trajectory.joint_trajectory;
          //          printTrajectory(current_trajectory_);
          visualizePlan();
          state_ = START_CONTROL;
          ROS_INFO("Done planning. Transitioning to control");
        }
        else if(action_server_->isActive())
        {
          num_planning_attempts_++;
          if(num_planning_attempts_ > req.motion_plan_request.num_planning_attempts)
          {
            resetStateMachine();
            action_server_->setAborted();
            return true;
          }
        }
        else
        {
          ROS_ERROR("create plan failed");
        }
        break;
      }
    case START_CONTROL:
      {
        move_arm_action_feedback_.state = "start_control";
        move_arm_action_feedback_.time_to_completion = ros::Duration(1.0/move_arm_frequency_);
        action_server_->publishFeedback(move_arm_action_feedback_);
        ROS_DEBUG("Filtering Trajectory");
        trajectory_msgs::JointTrajectory filtered_trajectory;
        if(filterTrajectory(current_trajectory_, filtered_trajectory))
        {
          current_trajectory_ = filtered_trajectory;
        }
        ROS_DEBUG("Sending trajectory");
        if(sendTrajectory(current_trajectory_))
        {
          state_ = MONITOR;
        }
        else
        {
          resetStateMachine();
          action_server_->setAborted();
          return true;              
        }
        break;
      }	
    case MONITOR:
      {
        move_arm_action_feedback_.state = "monitor";
        move_arm_action_feedback_.time_to_completion = current_trajectory_.points.back().time_from_start;
        action_server_->publishFeedback(move_arm_action_feedback_);
        ROS_DEBUG("Start to monitor");
        if(isControllerDone())
        {
          motion_planning_msgs::RobotState current_state;
          current_state.joint_state = state_monitor_.getJointState(group_joint_names_);
          if(isStateValidAtGoal(current_state.joint_state))
          {
            resetStateMachine();
            move_arm_action_result_.error_code.val = move_arm_action_result_.error_code.SUCCESS;
            action_server_->setSucceeded();
            ROS_INFO("Reached goal");
            return true;
          }
          else
          {
            state_ = PLANNING;
            break;
          }
        }            
        if(!move_arm_parameters_.disable_collision_monitoring && action_server_->isActive())
        {
          ROS_DEBUG("Monitoring trajectory");
          if(!isExecutionSafe())
          {
            ROS_INFO("Stopping trajectory since it is unsafe");
            stopTrajectory();
            state_ = PLANNING;
          }
        }
        break;
      }
    default:
      {
        ROS_INFO("Should not be here.");
        break;
      }
    }   
    if(!action_server_->isActive())
    {
      ROS_DEBUG("Move arm no longer has an active goal");
      return true;
    }
    return false;		
  }	
  void execute(const move_arm_msgs::MoveArmGoalConstPtr& goal)
  {
    motion_planning_msgs::GetMotionPlan::Request req;	    
    moveArmGoalToPlannerRequest(goal,req);	    
    original_request_ = req;
    ros::Rate move_arm_rate(move_arm_frequency_);

    while(private_handle_.ok())
    {	    	    
      if (action_server_->isPreemptRequested())
      {
        if(action_server_->isNewGoalAvailable())
        {
          moveArmGoalToPlannerRequest((action_server_->acceptNewGoal()),req);
          original_request_ = req;
          stopTrajectory();
          state_ = PLANNING;
        }
        else               //if we've been preempted explicitly we need to shut things down
        {
          ROS_INFO("The move arm action was preempted by the action client. Preempting this goal.");
          stopTrajectory();
          resetStateMachine();
          action_server_->setPreempted();
          return;
        }
      }

      //for timing that gives real time even in simulation
      ros::WallTime start = ros::WallTime::now();

      //the real work on pursuing a goal is done here
      bool done = executeCycle(req);

      if(done)
      {
        return;
      }

      ros::WallDuration t_diff = ros::WallTime::now() - start;
      ROS_DEBUG("Full control cycle time: %.9f\n", t_diff.toSec());

      move_arm_rate.sleep();
    }	    
    //if the node is killed then we'll abort and return
    ROS_INFO("Node was killed, aborting");
    action_server_->setAborted();
  }
  ///
  /// End State machine
  ///
	
  ///
  /// Visualization and I/O	
  ///
  void printTrajectory(const trajectory_msgs::JointTrajectory &trajectory)
  {
    for (unsigned int i = 0 ; i < trajectory.points.size() ; ++i)
    {
      std::stringstream ss;
      for (unsigned int j = 0 ; j < trajectory.points[i].positions.size() ; ++j)
        ss << trajectory.points[i].positions[j] << " ";
      ROS_DEBUG("%s", ss.str().c_str());
    }
  }	 
  void visualizeJointGoal(motion_planning_msgs::GetMotionPlan::Request &req)
  {
    if(!isJointGoal(req))
    {
      ROS_WARN("Only joint goals can be displayed");
      return;
    }
    ROS_DEBUG("Displaying joint goal");
    motion_planning_msgs::DisplayTrajectory d_path;
    d_path.model_id = req.motion_plan_request.group_name;
    d_path.trajectory.joint_trajectory = motion_planning_msgs::jointConstraintsToJointTrajectory(req.motion_plan_request.goal_constraints.joint_constraints);
    if(!getRobotState(d_path.robot_state))
    {
      ROS_ERROR("Could not get robot state");
    }
    else
    {
      display_joint_goal_publisher_.publish(d_path);
      ROS_INFO("Displaying move arm joint goal.");
    }
  }
  void visualizePlan()
  {
    move_arm_action_feedback_.state = "visualizing plan";
    action_server_->publishFeedback(move_arm_action_feedback_);
    motion_planning_msgs::DisplayTrajectory d_path;
    d_path.model_id = original_request_.motion_plan_request.group_name;
    d_path.trajectory.joint_trajectory = current_trajectory_;
    if(!getRobotState(d_path.robot_state))
      ROS_ERROR("Could not get robot state");
    else
      display_path_publisher_.publish(d_path);
  }
  void visualizeAllowedContactRegions(const std::vector<motion_planning_msgs::AllowedContactSpecification> &allowed_contacts)
  {
    static int count = 0;
    visualization_msgs::MarkerArray mk;
    mk.markers.resize(allowed_contacts.size());
    for(unsigned int i=0; i < allowed_contacts.size(); i++) 
    { 
      bool valid_shape = true;
      mk.markers[i].header.stamp = ros::Time::now();
      mk.markers[i].header.frame_id = allowed_contacts[i].pose_stamped.header.frame_id;
      mk.markers[i].ns = allowed_contacts[i].name;
      mk.markers[i].id = count++;
      if(allowed_contacts[i].shape.type == geometric_shapes_msgs::Shape::SPHERE)
      {        
        mk.markers[i].type = visualization_msgs::Marker::SPHERE;
        if(allowed_contacts[i].shape.dimensions.size() >= 1)
          mk.markers[i].scale.x = mk.markers[i].scale.y = mk.markers[i].scale.z = allowed_contacts[i].shape.dimensions[0];
        else
          valid_shape = false;
      }      
      else if (allowed_contacts[i].shape.type == geometric_shapes_msgs::Shape::BOX)
      {
        mk.markers[i].type = visualization_msgs::Marker::CUBE;
        if(allowed_contacts[i].shape.dimensions.size() >= 3)
        {
          mk.markers[i].scale.x = allowed_contacts[i].shape.dimensions[0];
          mk.markers[i].scale.y = allowed_contacts[i].shape.dimensions[1];
          mk.markers[i].scale.z = allowed_contacts[i].shape.dimensions[2];
        }
        else
          valid_shape = false;
      }
      else if (allowed_contacts[i].shape.type == geometric_shapes_msgs::Shape::CYLINDER)
      {
        mk.markers[i].type = visualization_msgs::Marker::CYLINDER;
        if(allowed_contacts[i].shape.dimensions.size() >= 2)
        {
          mk.markers[i].scale.x = allowed_contacts[i].shape.dimensions[0];
          mk.markers[i].scale.y = allowed_contacts[i].shape.dimensions[0];
          mk.markers[i].scale.z = allowed_contacts[i].shape.dimensions[1];
        }
        else
          valid_shape = false;
      }
      else
      {
        mk.markers[i].scale.x = mk.markers[i].scale.y = mk.markers[i].scale.z = 0.01;
        valid_shape = false;
      }        

      mk.markers[i].action = visualization_msgs::Marker::ADD;
      mk.markers[i].pose = allowed_contacts[i].pose_stamped.pose;  
      if(!valid_shape)
      {
        mk.markers[i].scale.x = mk.markers[i].scale.y = mk.markers[i].scale.z = 0.01;
        mk.markers[i].color.a = 0.3;
        mk.markers[i].color.r = 1.0;
        mk.markers[i].color.g = 0.04;
        mk.markers[i].color.b = 0.04;
      }
      else
      {
        mk.markers[i].color.a = 0.3;
        mk.markers[i].color.r = 0.04;
        mk.markers[i].color.g = 1.0;
        mk.markers[i].color.b = 0.04;
      }  
      //mk.markers[i].lifetime = ros::Duration(30.0);  
    }
    allowed_contact_regions_publisher_.publish(mk);
  }
  ///
  /// End Visualization and I/O	
  ///

private:

  std::string group_;

  ros::ServiceClient get_joints_in_group_client_, get_state_client_;
  ros::ServiceClient ik_client_, check_state_at_goal_client_, check_plan_validity_client_;
  ros::ServiceClient check_env_safe_client_, check_execution_safe_client_, check_state_validity_client_;
  ros::ServiceClient trajectory_start_client_,trajectory_cancel_client_,trajectory_query_client_;	
  ros::NodeHandle private_handle_, root_handle_;
  boost::shared_ptr<actionlib::SimpleActionServer<move_arm_msgs::MoveArmAction> > action_server_;	

  tf::TransformListener *tf_;
  MoveArmState state_;
  double move_arm_frequency_;      	
  trajectory_msgs::JointTrajectory current_trajectory_;

  int num_planning_attempts_;

  planning_environment::JointStateMonitor state_monitor_;
  std::vector<std::string> group_joint_names_;
  move_arm_msgs::MoveArmResult move_arm_action_result_;
  move_arm_msgs::MoveArmFeedback move_arm_action_feedback_;

  motion_planning_msgs::GetMotionPlan::Request original_request_;

  ros::Publisher display_path_publisher_;
  ros::Publisher display_joint_goal_publisher_;
  ros::Publisher allowed_contact_regions_publisher_;
  ros::ServiceClient filter_trajectory_client_;
  ros::ServiceClient fk_client_;
  MoveArmParameters move_arm_parameters_;
  ControllerStatus controller_status_;

  JointExecutorActionClient* controller_action_client_;
  JointExecutorActionClient::GoalHandle controller_goal_handle_;

  double trajectory_filter_allowed_time_, ik_allowed_time_;
};
}

int main(int argc, char** argv)
{
  ros::init(argc, argv, "move_arm");
  
  ros::AsyncSpinner spinner(1); // Use 1 thread
  spinner.start();
  ros::NodeHandle nh("~");
  std::string group;
  nh.param<std::string>("group", group, std::string());
  ROS_INFO("Move arm operating on group %s",group.c_str());    
  move_arm::MoveArm move_arm(group);
  if(!move_arm.configure())
  {
    ROS_ERROR("Could not configure move arm, exiting");
    ros::shutdown();
    return 1;
  }
  ROS_INFO("Move arm action started");
  ros::waitForShutdown();
    
  return 0;
}
