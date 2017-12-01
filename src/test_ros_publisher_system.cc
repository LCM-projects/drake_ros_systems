#include <memory>
#include "drake/systems/analysis/simulator.h"
#include "drake/systems/framework/diagram.h"
#include "drake/systems/framework/diagram_builder.h"
#include "drake/systems/framework/leaf_system.h"
#include "drake/systems/primitives/constant_value_source.h"
#include "ros/ros.h"
#include "std_msgs/String.h"

#include "../include/drake_ros_systems/ros_publisher_system.h"

using drake::systems::AbstractValue;
using drake::systems::ConstantValueSource;
using drake::systems::Context;
using drake::systems::Diagram;
using drake::systems::DiagramBuilder;
using drake::systems::InputPortDescriptor;
using drake::systems::OutputPort;
using drake::systems::Simulator;

using namespace drake_ros_systems;

int DoMain(ros::NodeHandle& node_handle) {
  DiagramBuilder<double> builder;

  auto msg_publisher = builder.AddSystem(
      RosPublisherSystem<std_msgs::String>::Make("test_publish", &node_handle));
  msg_publisher->set_publish_period(0.25);

  std_msgs::String msg;
  msg.data = "Hello world!";

  auto msg_source =
      builder.AddSystem(std::make_unique<ConstantValueSource<double>>(
          AbstractValue::Make<std_msgs::String>(msg)));

  builder.Connect(msg_source->get_output_port(0),
                  msg_publisher->get_input_port(0));

  auto sys = builder.Build();
  Simulator<double> simulator(*sys);

  simulator.Initialize();
  simulator.set_target_realtime_rate(1.0);
  simulator.StepTo(std::numeric_limits<double>::infinity());

  return 0;
}

int main(int argc, char* argv[]) {
  ros::init(argc, argv, "test_ros_publisher_system");
  ros::NodeHandle node_handle;

  return DoMain(node_handle);
}