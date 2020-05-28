// Copyright (c) 2013, Open Source Robotics Foundation. All rights reserved.
// Copyright (c) 2013, The Johns Hopkins University. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//    * Redistributions of source code must retain the above copyright
//      notice, this list of conditions and the following disclaimer.
//
//    * Redistributions in binary form must reproduce the above copyright
//      notice, this list of conditions and the following disclaimer in the
//      documentation and/or other materials provided with the distribution.
//
//    * Neither the name of the Open Source Robotics Foundation nor the names of its
//      contributors may be used to endorse or promote products derived from
//      this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

/* Author: Dave Coleman, Jonathan Bohren
   Desc:   Gazebo plugin for ros_control that allows 'hardware_interfaces' to be plugged in
   using pluginlib
*/

#include <string>
#include <memory>
#include <vector>

#include "gazebo_ros2_control/gazebo_ros2_control_plugin.hpp"

#include "urdf/model.h"
#include "yaml-cpp/yaml.h"

namespace gazebo_ros2_control
{

GazeboRosControlPlugin::~GazeboRosControlPlugin()
{
  // Disconnect from gazebo events
  update_connection_.reset();
}

// Overloaded Gazebo entry point
void GazeboRosControlPlugin::Load(gazebo::physics::ModelPtr parent, sdf::ElementPtr sdf)
{
  RCLCPP_INFO_STREAM(
    rclcpp::get_logger("gazebo_ros2_control"),
    "Loading gazebo_ros2_control plugin");


  // Save pointers to the model
  parent_model_ = parent;
  sdf_ = sdf;

  // Error message if the model couldn't be found
  if (!parent_model_) {
    RCLCPP_ERROR_STREAM(rclcpp::get_logger("loadThread"), "parent model is NULL");
    return;
  }

  // Check that ROS has been initialized
  if (!rclcpp::ok()) {
    RCLCPP_FATAL_STREAM(
      rclcpp::get_logger(
        "gazebo_ros2_control"),
      "A ROS node for Gazebo has not been initialized, unable to load plugin. " <<
        "Load the Gazebo system plugin 'libgazebo_ros_api_plugin.so' in the gazebo_ros package)");
    return;
  }

  // Get namespace for nodehandle
  if (sdf_->HasElement("robotNamespace")) {
    robot_namespace_ = sdf_->GetElement("robotNamespace")->Get<std::string>();
  } else {
    robot_namespace_ = parent_model_->GetName();  // default
  }

  // Get robot_description ROS param name
  if (sdf_->HasElement("robotParam")) {
    robot_description_ = sdf_->GetElement("robotParam")->Get<std::string>();
  } else {
    robot_description_ = "robot_description";  // default
  }

  // Get the robot simulation interface type
  if (sdf_->HasElement("robotSimType")) {
    robot_hw_sim_type_str_ = sdf_->Get<std::string>("robotSimType");
  } else {
    robot_hw_sim_type_str_ = "gazebo_ros2_control/DefaultRobotHWSim";
    RCLCPP_DEBUG_STREAM(
      rclcpp::get_logger(
        "loadThread"),
      "Using default plugin for RobotHWSim (none specified in URDF/SDF)\"" <<
        robot_hw_sim_type_str_ << "\"");
  }

  // temporary fix to bug regarding the robotNamespace in default_robot_hw_sim.cpp (see #637)
  std::string robot_ns = robot_namespace_;
  if (robot_hw_sim_type_str_ == "gazebo_ros2_control/DefaultRobotHWSim") {
    if (sdf_->HasElement("legacyModeNS")) {
      if (sdf_->GetElement("legacyModeNS")->Get<bool>() ) {
        robot_ns = "";
      }
    } else {
      robot_ns = "";
      RCLCPP_ERROR(
        rclcpp::get_logger(
          "loadThread"),
        "GazeboRosControlPlugin missing <legacyModeNS> while using DefaultRobotHWSim, defaults to"
        " true.\nThis setting assumes you have an old package with an old implementation of "
        "DefaultRobotHWSim, where the robotNamespace is disregarded and absolute paths are used "
        "instead.\n If you do not want to fix this issue in an old package just set <legacyModeNS> "
        "to true.\n"
      );
    }
  }

  if (sdf_->HasElement("parameters")) {
    param_file = sdf_->GetElement("parameters")->Get<std::string>();
  } else {
    RCLCPP_ERROR(
      rclcpp::get_logger(
        "loadThread"), "No parameter file provided. Configuration might be wrong");
  }

  // Get the Gazebo simulation period
#if GAZEBO_MAJOR_VERSION >= 8
  rclcpp::Duration gazebo_period(parent_model_->GetWorld()->Physics()->GetMaxStepSize());
#else
  rclcpp::Duration gazebo_period(parent_model_->GetWorld()->GetPhysicsEngine()->GetMaxStepSize());
#endif

  // Decide the plugin control period
  if (sdf_->HasElement("controlPeriod")) {
    control_period_ = rclcpp::Duration(sdf_->Get<double>("controlPeriod"));

    // Check the period against the simulation period
    if (control_period_ < gazebo_period) {
      RCLCPP_ERROR_STREAM(
        rclcpp::get_logger(
          "gazebo_ros2_control"),
        "Desired controller update period (" << control_period_.seconds() <<
          " s) is faster than the gazebo simulation period (" << gazebo_period.seconds() << " s).");
    } else if (control_period_ > gazebo_period) {
      RCLCPP_WARN_STREAM(
        rclcpp::get_logger(
          "gazebo_ros2_control"),
        " Desired controller update period (" << control_period_.seconds() <<
          " s) is slower than the gazebo simulation period (" << gazebo_period.seconds() << " s).");
    }
  } else {
    control_period_ = gazebo_period;
    RCLCPP_DEBUG_STREAM(
      rclcpp::get_logger(
        "gazebo_ros2_control"),
      "Control period not found in URDF/SDF, defaulting to Gazebo period of " <<
        control_period_.seconds());
  }

  // Get parameters/settings for controllers from ROS param server
  model_nh_ = std::make_shared<rclcpp::Node>(parent_model_->GetName(), robot_namespace_);

  // Initialize the emergency stop code.
  e_stop_active_ = false;
  last_e_stop_active_ = false;
  if (sdf_->HasElement("eStopTopic")) {
    using std::placeholders::_1;
    const std::string e_stop_topic = sdf_->GetElement("eStopTopic")->Get<std::string>();
    e_stop_sub_ = model_nh_->create_subscription<std_msgs::msg::Bool>(
      e_stop_topic, 1,
      std::bind(&GazeboRosControlPlugin::eStopCB, this, _1));
  }
  RCLCPP_INFO(
    rclcpp::get_logger(
      "gazebo_ros2_control"), "Starting gazebo_ros2_control plugin in namespace: %s",
    robot_namespace_.c_str());

  // Read urdf from ros parameter server then
  // setup actuators and mechanism control node.
  // This call will block if ROS is not properly initialized.
  const std::string urdf_string = getURDF(robot_description_);
  if (!parseTransmissionsFromURDF(urdf_string)) {
    RCLCPP_ERROR(
      rclcpp::get_logger(
        "gazebo_ros2_control"),
      "Error parsing URDF in gazebo_ros2_control plugin, plugin not active.\n");
    return;
  }

  // Load the RobotHWSim abstraction to interface the controllers with the gazebo model
  try {
    robot_hw_sim_loader_.reset(
      new pluginlib::ClassLoader<gazebo_ros2_control::RobotHWSim>(
        "gazebo_ros2_control",
        "gazebo_ros2_control::RobotHWSim"));

    robot_hw_sim_ = robot_hw_sim_loader_->createSharedInstance(robot_hw_sim_type_str_);

    urdf::Model urdf_model;
    const urdf::Model * const urdf_model_ptr =
      urdf_model.initString(urdf_string) ? &urdf_model : NULL;

    if (!robot_hw_sim_->initSim(
        robot_ns, model_nh_, parent_model_, urdf_model_ptr,
        transmissions_))
    {
      RCLCPP_FATAL(
        rclcpp::get_logger(
          "gazebo_ros2_control"), "Could not initialize robot simulation interface");
      return;
    }

    executor_ = std::make_shared<rclcpp::executors::MultiThreadedExecutor>();
    executor_->add_node(model_nh_);

    // Create the controller manager
    RCLCPP_ERROR(rclcpp::get_logger("ros2_control_plugin"), "Loading controller_manager");
#if 1  // @todo
    controller_manager_.reset(
      new controller_manager::ControllerManager(
        robot_hw_sim_, executor_,
        "gazebo_controller_manager"));
#endif
#if 1
    // @todo:Coded example here. should disable when spawn functionality of controller manager is up
    auto load_params_from_yaml_node = [](rclcpp_lifecycle::LifecycleNode::SharedPtr lc_node,
        YAML::Node & yaml_node, const std::string & prefix)
      {
        std::function<void(YAML::Node, const std::string &,
          std::shared_ptr<rclcpp_lifecycle::LifecycleNode>, const std::string &)>
        feed_yaml_to_node_rec =
          [&](YAML::Node yaml_node, const std::string & key,
            std::shared_ptr<rclcpp_lifecycle::LifecycleNode> node, const std::string & prefix)
          {
            if (node->get_name() != prefix) {
              return;
            }
            static constexpr char separator = '.';
            if (yaml_node.Type() == YAML::NodeType::Scalar) {
              std::string val_str = yaml_node.as<std::string>();
              // const char* val_str = yaml_node.as<const char*>();
              // std::string val_str;
              // YAML::convert<std::string>::decode(yaml_node, val_str);

              // TODO(ddengster): Do stricter typing for set_parameter value types.
              // (ie. Numbers should be converted to int/double/etc instead of strings)
              if (!node->has_parameter(key)) {
                node->declare_parameter(key);
              }

              auto is_number = [](const std::string & str) -> bool {
                  return str.find_first_not_of("0123456789.-") == std::string::npos;
                  // @note: bugs with .05 or 15.
                };

              if (is_number(val_str)) {
                std::stringstream ss(val_str);
                double v = 0.0;
                ss >> v;
                node->set_parameter(rclcpp::Parameter(key, v));
              } else {
                node->set_parameter(rclcpp::Parameter(key, val_str));
              }

              return;
            } else if (yaml_node.Type() == YAML::NodeType::Map) {
              for (auto yaml_node_it : yaml_node) {
                std::string newkey = yaml_node_it.first.as<std::string>();
                if (newkey == prefix || newkey == "ros__parameters") {
                  newkey = "";
                } else if (!key.empty()) {
                  newkey = key + separator + newkey;
                }
                feed_yaml_to_node_rec(yaml_node_it.second, newkey, node, prefix);
              }
            } else if (yaml_node.Type() == YAML::NodeType::Sequence) {
              auto it = yaml_node.begin();
              if (yaml_node.size()) {
                if (it->IsScalar()) {
                  // submit as array of parameters
                  std::vector<std::string> val;
                  for (auto yaml_node_it : yaml_node) {
                    std::string name = yaml_node_it.as<std::string>();
                    val.push_back(name);
                  }
                  if (!node->has_parameter(key)) {
                    node->declare_parameter(key);
                  }
                  node->set_parameter({rclcpp::Parameter(key, val)});

                  if (key == "joints") {
                    if (!node->has_parameter("write_op_modes")) {
                      node->declare_parameter("write_op_modes");
                    }
                    node->set_parameter(rclcpp::Parameter("write_op_modes", val));
                  }
                } else {
                  size_t index = 0;
                  for (auto yaml_node_it : yaml_node) {
                    std::string newkey = std::to_string((index++));
                    if (!key.empty()) {
                      newkey = key + separator + newkey;
                    }
                    feed_yaml_to_node_rec(yaml_node_it, newkey, node, prefix);
                  }
                }
              }
            }
          };
        if (lc_node->get_name() != prefix) {
          return;
        }
        feed_yaml_to_node_rec(yaml_node, prefix, lc_node, prefix);
      };
    auto load_params_from_yaml = [&](rclcpp_lifecycle::LifecycleNode::SharedPtr lc_node,
        const std::string & yaml_config_file, const std::string & prefix)
      {
        if (yaml_config_file.empty()) {
          throw std::runtime_error("yaml config file path is empty");
        }

        YAML::Node root_node = YAML::LoadFile(yaml_config_file);
        for (auto yaml : root_node) {
          auto nodename = yaml.first.as<std::string>();
          RCLCPP_ERROR(rclcpp::get_logger("ros_control_plugin"), "nodename: %s", nodename.c_str());
          if (nodename == prefix) {
            load_params_from_yaml_node(lc_node, yaml.second, prefix);
          }
        }
      };

    YAML::Node root_node = YAML::LoadFile(param_file);
    for (auto yaml : root_node) {
      auto controller_name = yaml.first.as<std::string>();
      for (auto yaml_node_it : yaml.second) {  // ros__parameters
        for (auto yaml_node_it2 : yaml_node_it.second) {
          auto param_name = yaml_node_it2.first.as<std::string>();
          if (param_name == "type") {
            auto controller_type = yaml_node_it2.second.as<std::string>();
            auto controller = controller_manager_->load_controller(
              controller_name,
              controller_type);
            controllers_.push_back(controller);
            load_params_from_yaml(controller->get_lifecycle_node(), param_file, controller_name);
          }
        }
      }
    }

    auto spin = [this]()
      {
        while (rclcpp::ok()) {
          executor_->spin_once();
        }
      };
    thread_executor_spin_ = std::thread(spin);

    if (controller_manager_->configure() !=
      controller_interface::CONTROLLER_INTERFACE_RET_SUCCESS)
    {
      RCLCPP_ERROR(rclcpp::get_logger("cm"), "failed to configure");
    }
    if (controller_manager_->activate() != controller_interface::CONTROLLER_INTERFACE_RET_SUCCESS) {
      RCLCPP_ERROR(rclcpp::get_logger("cm"), "failed to activate");
    }
#endif
    // Listen to the update event. This event is broadcast every simulation iteration.
    update_connection_ =
      gazebo::event::Events::ConnectWorldUpdateBegin(
      boost::bind(
        &GazeboRosControlPlugin::Update,
        this));
  } catch (pluginlib::LibraryLoadException & ex) {
    RCLCPP_ERROR(
      rclcpp::get_logger(
        "gazebo_ros2_control"), "Failed to create robot simulation interface loader: %s ",
      ex.what());
  }

  RCLCPP_ERROR(rclcpp::get_logger("gazebo_ros2_control"), "Loaded gazebo_ros2_control.");
}

void
spin(std::shared_ptr<rclcpp::executors::MultiThreadedExecutor> exe)
{
  exe->spin();
}

// Called by the world update start event
void GazeboRosControlPlugin::Update()
{
  // Get the simulation time and period
#if GAZEBO_MAJOR_VERSION >= 8
  gazebo::common::Time gz_time_now = parent_model_->GetWorld()->SimTime();
#else
  gazebo::common::Time gz_time_now = parent_model_->GetWorld()->GetSimTime();
#endif
  rclcpp::Time sim_time_ros(gz_time_now.sec, gz_time_now.nsec);
  rclcpp::Duration sim_period = sim_time_ros - last_update_sim_time_ros_;

  robot_hw_sim_->eStopActive(e_stop_active_);

  // Check if we should update the controllers
  if (sim_period >= control_period_) {
    // Store this simulation time
    last_update_sim_time_ros_ = sim_time_ros;

    // Update the robot simulation with the state of the gazebo model
    robot_hw_sim_->readSim(sim_time_ros, sim_period);

    // Compute the controller commands
    bool reset_ctrlrs;
    if (e_stop_active_) {
      reset_ctrlrs = false;
      last_e_stop_active_ = true;
    } else {
      if (last_e_stop_active_) {
        reset_ctrlrs = true;
        last_e_stop_active_ = false;
      } else {
        reset_ctrlrs = false;
      }
    }

    controller_manager_->update();
  }

  // Update the gazebo model with the result of the controller
  // computation
  robot_hw_sim_->writeSim(sim_time_ros, sim_time_ros - last_write_sim_time_ros_);
  last_write_sim_time_ros_ = sim_time_ros;
}

// Called on world reset
void GazeboRosControlPlugin::Reset()
{
  // Reset timing variables to not pass negative update periods to controllers on world reset
  last_update_sim_time_ros_ = rclcpp::Time();
  last_write_sim_time_ros_ = rclcpp::Time();
}

// Get the URDF XML from the parameter server
std::string GazeboRosControlPlugin::getURDF(std::string param_name) const
{
  std::string urdf_string;

  using namespace std::chrono_literals;
  auto parameters_client = std::make_shared<rclcpp::SyncParametersClient>(
    model_nh_,
    "robot_state_publisher");
  while (!parameters_client->wait_for_service(0.5s)) {
    if (!rclcpp::ok()) {
      RCLCPP_ERROR(
        rclcpp::get_logger(
          "gazebo_ros2_control"), "Interrupted while waiting for the service. Exiting.");
      return 0;
    }
    RCLCPP_ERROR(
      rclcpp::get_logger(
        "gazebo_ros2_control"), "service not available, waiting again...");
  }

  RCLCPP_ERROR(
    rclcpp::get_logger(
      "gazebo_ros2_control"), "connected to service!! /robot_state_publisher");

  // search and wait for robot_description on param server
  while (urdf_string.empty()) {
    std::string search_param_name;
    RCLCPP_ERROR(rclcpp::get_logger("gazebo_ros2_control"), "param_name %s", param_name.c_str());

    urdf_string = parameters_client->get_parameter<std::string>(param_name, "");
    if (!urdf_string.empty()) {
      break;
    } else {
      RCLCPP_ERROR(
        rclcpp::get_logger("gazebo_ros2_control"), "gazebo_ros2_control plugin is waiting for model"
        " URDF in parameter [%s] on the ROS param server.", search_param_name.c_str());
    }
    usleep(100000);
  }
  RCLCPP_ERROR(
    rclcpp::get_logger(
      "gazebo_ros2_control"), "Recieved urdf from param server, parsing...");

  return urdf_string;
}

// Get Transmissions from the URDF
bool GazeboRosControlPlugin::parseTransmissionsFromURDF(const std::string & urdf_string)
{
  transmission_interface::TransmissionParser::parse(urdf_string, transmissions_);
  return true;
}

// Emergency stop callback
void GazeboRosControlPlugin::eStopCB(const std::shared_ptr<std_msgs::msg::Bool> e_stop_active)
{
  e_stop_active_ = e_stop_active->data;
}

// Register this plugin with the simulator
GZ_REGISTER_MODEL_PLUGIN(GazeboRosControlPlugin);
}  // namespace gazebo_ros2_control
