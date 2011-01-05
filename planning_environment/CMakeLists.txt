cmake_minimum_required(VERSION 2.4.6)
include($ENV{ROS_ROOT}/core/rosbuild/rosbuild.cmake)

rosbuild_init()

set(ROS_BUILD_TYPE Release)

#set the default path for built executables to the "bin" directory
set(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/bin)
#set the default path for built libraries to the "lib" directory
set(LIBRARY_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/lib)

rosbuild_add_library(planning_environment src/models/robot_models.cpp
					 src/models/collision_models.cpp
					 src/monitors/kinematic_model_state_monitor.cpp
					 src/monitors/collision_space_monitor.cpp
					 src/monitors/planning_monitor.cpp
					 src/util/kinematic_state_constraint_evaluator.cpp
					 src/util/construct_object.cpp
                                         src/monitors/joint_state_monitor.cpp)
rosbuild_add_openmp_flags(planning_environment)
rosbuild_link_boost(planning_environment thread)

rosbuild_add_executable(environment_server src/monitors/environment_server.cpp src/monitors/environment_server_setup.cpp)
rosbuild_link_boost(environment_server thread)
target_link_libraries(environment_server planning_environment)
 
# Utility apps
rosbuild_add_executable(view_state_validity src/tools/view_state_validity.cpp) 
target_link_libraries(view_state_validity planning_environment)
rosbuild_link_boost(view_state_validity thread)
rosbuild_add_openmp_flags(view_state_validity)

rosbuild_add_executable(clear_known_objects src/tools/clear_known_objects.cpp) 
target_link_libraries(clear_known_objects planning_environment)
rosbuild_link_boost(clear_known_objects thread)
rosbuild_add_openmp_flags(clear_known_objects)

rosbuild_add_executable(display_planner_collision_model src/tools/display_planner_collision_model.cpp) 
target_link_libraries(display_planner_collision_model planning_environment)
rosbuild_link_boost(display_planner_collision_model thread)
rosbuild_add_openmp_flags(display_planner_collision_model)

# Examples
rosbuild_add_executable(remove_object_example src/examples/remove_object_example.cpp) 
target_link_libraries(remove_object_example planning_environment)
rosbuild_link_boost(remove_object_example thread)
rosbuild_add_openmp_flags(remove_object_example)

# Tests - these have moved into pr2_arm_navigation_tests

# Create a model of the PR2
#rosbuild_add_executable(allowable_contacts_test test/allowable_contacts_test.cpp)
#target_link_libraries(allowable_contacts_test gtest)
#rosbuild_add_executable(path_constraints_test test/path_constraints_test.cpp)
#target_link_libraries(path_constraints_test gtest)

rosbuild_add_executable(joint_state_decumulator test/joint_state_decumulator.cpp)
target_link_libraries(joint_state_decumulator planning_environment)

rosbuild_add_executable(test_robot_models test/test_robot_models.cpp) 
#rosbuild_declare_test(test_robot_models)
#rosbuild_add_gtest_build_flags(test_robot_models)
target_link_libraries(test_robot_models planning_environment)
target_link_libraries(test_robot_models gtest)