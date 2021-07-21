#!/usr/bin/env python3
# Copyright 2021 Open Source Robotics Foundation, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.


import os
import sys

from ament_index_python.packages import get_package_share_directory

from launch import LaunchDescription
from launch import LaunchService
from launch.actions import ExecuteProcess, IncludeLaunchDescription, SetEnvironmentVariable
from launch.launch_description_sources import PythonLaunchDescriptionSource

from launch_testing.legacy import LaunchTestService


def generate_launch_description():

    gazebo_dir = get_package_share_directory('gazebo_ros2_control_demos')
    included_launch = IncludeLaunchDescription(
        PythonLaunchDescriptionSource(gazebo_dir + '/launch/cart_example_position.launch.py'),
        launch_arguments={'paused': 'false',
                          'physics': 'ode',
                          'gui': 'false'}.items()
        )

    return LaunchDescription([
        SetEnvironmentVariable('RCUTILS_LOGGING_BUFFERED_STREAM', '1'),
        included_launch,
    ])


def main(argv=sys.argv[1:]):
    ld = generate_launch_description()

    test1_action = ExecuteProcess(
        cmd=[os.path.join(os.getenv('TEST_DIR'), 'tester_node.py')],
        name='tester_node',
        output='screen')

    lts = LaunchTestService()
    lts.add_test_action(ld, test1_action)
    ls = LaunchService(argv=argv)
    ls.include_launch_description(ld)
    return lts.run(ls)


if __name__ == '__main__':
    sys.exit(main())
