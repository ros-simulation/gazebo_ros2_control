#! /usr/bin/env python3
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

import argparse
import time

from control_msgs.action import FollowJointTrajectory

from controller_manager_msgs.srv import ListControllers

import rclpy
from rclpy.action import ActionClient
from rclpy.executors import MultiThreadedExecutor
from rclpy.node import Node

from trajectory_msgs.msg import JointTrajectoryPoint


class GazeboRos2ControlTester(Node):

    def __init__(
        self,
        timeout: [60],
    ):
        super().__init__(node_name='GazeboRos2ControlTester')

        self.timeout = timeout
        self.executor = MultiThreadedExecutor()

    def controller_list(self):
        self.info_msg('Starting controller_list test')
        controller_service = self.create_client(
            ListControllers, '/controller_manager/list_controllers')
        if not controller_service.wait_for_service(timeout_sec=5.0):
            return False
        req = ListControllers.Request()
        future = controller_service.call_async(req)
        while rclpy.ok():
            rclpy.spin_once(self)
            if future.done():
                try:
                    response = future.result()
                except Exception as e:
                    self.get_logger().info(
                        'Service call failed %r' % (e,))
                else:
                    for controller in response.controller:
                        if controller.state != 'active':
                            return False
                    return True
            break

    def send_goal(self):
        points = []
        point = JointTrajectoryPoint()
        point.time_from_start.sec = 0
        point.time_from_start.nanosec = 0
        point.positions.append(0.0)
        points.append(point)

        point = JointTrajectoryPoint()
        point.time_from_start.sec = 1
        point.time_from_start.nanosec = 0
        point.positions.append(1.0)
        points.append(point)

        goal_msg = FollowJointTrajectory.Goal()
        point.time_from_start.sec = 1
        point.time_from_start.nanosec = 0
        goal_msg.trajectory.joint_names = ['slider_to_cart']
        goal_msg.trajectory.points = points
        return self.action_client.send_goal_async(goal_msg)

    def position_controller_test(self):
        self.info_msg('Starting position_controller_test')

        self.action_client = ActionClient(self,
                                          FollowJointTrajectory,
                                          '/joint_trajectory_controller/follow_joint_trajectory')
        if (not self.action_client.wait_for_server(timeout_sec=5)):
            return False

        future = self.send_goal()
        rclpy.spin_until_future_complete(self, future, self.executor)
        result = future.result()
        self.info_msg(str(result))

        if not result.accepted:
            self.info_msg('Position Goal was not accepted')
            return False
        if result.status == 0:
            return True
        self.info_msg('Position status is not successful')
        return False

    def info_msg(self, msg: str):
        self.get_logger().info('\033[1;37;44m' + msg + '\033[0m')

    def warn_msg(self, msg: str):
        self.get_logger().warn('\033[1;37;43m' + msg + '\033[0m')

    def error_msg(self, msg: str):
        self.get_logger().error('\033[1;37;41m' + msg + '\033[0m')


def test_ArmVehicle(robot_tester):
    return robot_tester.controller_list()


def test_Position(robot_tester):
    return robot_tester.position_controller_test()


def run_all_tests(robot_tester):
    # wait to set up everything
    time.sleep(6)
    result = True
    if (result):
        result = test_ArmVehicle(robot_tester)
    if (result):
        result = test_Position(robot_tester)

    if (result):
        robot_tester.info_msg('Test PASSED')
    else:
        robot_tester.error_msg('Test FAILED')

    return result


def get_testers(args):
    testers = []
    tester = GazeboRos2ControlTester(timeout=args.timeout)
    testers.append(tester)
    return testers


def main(args=None):
    parser = argparse.ArgumentParser(description='System-level drone demo tester node')
    parser.add_argument('-t', '--timeout', action='append', nargs=1,
                        metavar=('time_out'),
                        help='Time out')

    args_par, unknown = parser.parse_known_args()
    rclpy.init(args=args)

    # Create testers
    testers = get_testers(args_par)

    # run tests
    for tester in testers:
        passed = run_all_tests(tester)
        if not passed:
            exit(1)


if __name__ == '__main__':
    main()
