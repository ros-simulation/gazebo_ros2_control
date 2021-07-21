cmake_minimum_required(VERSION 3.5)
project(gazebo_ros2_control_tests)

find_package(ament_cmake REQUIRED)

if(BUILD_TESTING)
  find_package(ament_lint_auto REQUIRED)
  ament_lint_auto_find_test_dependencies()

  find_package(ament_cmake_gtest REQUIRED)
  find_package(ament_cmake_pytest REQUIRED)

  add_subdirectory(src/system)
endif()

ament_package()
