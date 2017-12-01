#pragma once

#include <condition_variable>
#include <memory>
#include <string>
#include <vector>

#include "drake/common/drake_copyable.h"
#include "drake/systems/framework/leaf_system.h"

#include "ros/ros.h"

namespace drake_ros_systems {

using namespace drake;

/**
 * Receives ROS messages from a given topic and outputs them to a
 * System<double>'s port. This class stores the most recently processed ROS
 * message in the State. When a ROS message arrives asynchronously, an update
 * event is scheduled to process the message and store it in the State at the
 * earliest possible simulation time. The output is always consistent with the
 * State.
 *
 * To process a ROS message, CalcNextUpdateTime() needs to be called first to
 * check for new messages and schedule a callback event if a new ROS message
 * has arrived. The message is then processed and stored in the Context by
 * CalcDiscreteVariableUpdates() or CalcUnrestrictedUpdate() depending on the
 * output type. When this system is evaluated by the Simulator, all these
 * operations are taken care of by the Simulator. On the other hand, the user
 * needs to manually replicate this process without the Simulator.
 *
 * (Direct clone of LcmSubscriberSystem with pared-down features.)
 */
template <typename RosMessage>
class RosSubscriberSystem : public systems::LeafSystem<double> {
 public:
  DRAKE_NO_COPY_NO_MOVE_NO_ASSIGN(RosSubscriberSystem)

  /**
   * A factory method that returns an %RosSubscriberSystem that takes
   * Value<RosMessage> message objects on its sole abstract-valued input port.
   *
   * @tparam RosMessage message type to serialize.
   *
   * @param[in] topic The ROS topic to subscribe to.
   *
   * @param node_handle ROS node handle for context (to make the subscriber).
   */
  static std::unique_ptr<RosSubscriberSystem<RosMessage>> Make(
      const std::string& topic, ros::NodeHandle* node_handle) {
    return std::make_unique<RosSubscriberSystem<RosMessage>>(topic,
                                                             node_handle);
  }

  /**
   * Constructor that returns a subscriber System that provides message objects
   * on its sole abstract-valued output port.  The type of the message object is
   * determined by the @p serializer.
   *
   * @param[in] channel The ROS topic on which to subscribe.
   *
   * @param node_handle The ROS context.
   */
  RosSubscriberSystem(const std::string& topic, ros::NodeHandle* node_handle)
      : topic_(topic), node_handle_(node_handle) {
    DRAKE_DEMAND(node_handle_);

    subscriber_ = node_handle->subscribe(
        topic, 100, &RosSubscriberSystem<RosMessage>::HandleMessage, this);

    DeclareAbstractOutputPort(
        [this](const systems::Context<double>&) {
          return this->AllocateOutputValue();
        },
        [this](const systems::Context<double>& context,
               systems::AbstractValue* out) {
          this->CalcOutputValue(context, out);
        });

    set_name(make_name(topic_));
  }

  ~RosSubscriberSystem() override{};

  const std::string& get_topic_name() const { return topic_; }

  /// Returns the default name for a system that publishes @p topic.
  static std::string make_name(const std::string& topic) {
    return "RosSubscriberSystem(" + topic + ")";
  }

  /**
   * Blocks the caller until @p old_message_count is different from the
   * internal message counter, and the internal message counter is returned.
   */
  int WaitForMessage(int old_message_count) const {
    // The message buffer and counter are updated in HandleMessage(), which is
    // a callback function invoked by a potentially different thread. Thus,
    // for thread safety, these need to be properly protected by a mutex.
    std::unique_lock<std::mutex> lock(received_message_mutex_);

    // This while loop is necessary to guard for spurious wakeup:
    // https://en.wikipedia.org/wiki/Spurious_wakeup
    while (old_message_count == received_message_count_)
      // When wait returns, lock is atomically acquired. So it's thread safe to
      // read received_message_count_.
      received_message_condition_variable_.wait(lock);
    int new_message_count = received_message_count_;
    lock.unlock();

    return new_message_count;
  }

  /**
   * Returns the message counter stored in @p context.
   */
  int GetMessageCount(const systems::Context<double>& context) const {
    // Gets the last message count from abstract state.
    return context.get_abstract_state<int>(kStateIndexMessageCount);
  }

 protected:
  void DoCalcNextUpdateTime(const systems::Context<double>& context,
                            systems::CompositeEventCollection<double>* events,
                            double* time) const override {
    const int last_message_count = GetMessageCount(context);

    const int received_message_count = [this]() {
      std::unique_lock<std::mutex> lock(received_message_mutex_);
      return received_message_count_;
    }();

    // Has a new message. Schedule an update event.
    if (last_message_count != received_message_count) {
      // TODO(siyuan): should be context.get_time() once #5725 is resolved.
      *time = context.get_time() + 0.0001;

      systems::EventCollection<systems::UnrestrictedUpdateEvent<double>>&
          uu_events = events->get_mutable_unrestricted_update_events();
      uu_events.add_event(
          std::make_unique<systems::UnrestrictedUpdateEvent<double>>(
              systems::Event<double>::TriggerType::kTimed));
    }
  }

  void DoCalcUnrestrictedUpdate(
      const systems::Context<double>&,
      const std::vector<const systems::UnrestrictedUpdateEvent<double>*>&,
      systems::State<double>* state) const override {
    ProcessMessageAndStoreToAbstractState(state->get_mutable_abstract_state());
  }

  std::unique_ptr<systems::AbstractValues> AllocateAbstractState()
      const override {
    std::vector<std::unique_ptr<systems::AbstractValue>> abstract_vals(2);
    abstract_vals[kStateIndexMessage] =
        this->RosSubscriberSystem::AllocateOutputValue();
    abstract_vals[kStateIndexMessageCount] =
        systems::AbstractValue::Make<int>(0);
    return std::make_unique<systems::AbstractValues>(std::move(abstract_vals));
  }

  void SetDefaultState(const systems::Context<double>& context,
                       systems::State<double>* state) const override {
    ProcessMessageAndStoreToAbstractState(state->get_mutable_abstract_state());
  };

 private:
  void ProcessMessageAndStoreToAbstractState(
      systems::AbstractValues* abstract_state) const {
    std::lock_guard<std::mutex> lock(received_message_mutex_);
    abstract_state->get_mutable_value(kStateIndexMessage)
        .GetMutableValue<RosMessage>() = received_message_;
    abstract_state->get_mutable_value(kStateIndexMessageCount)
        .GetMutableValue<int>() = received_message_count_;
  };

  // Callback entry point from ROS into this class. Also wakes up one thread
  // block on notification_ if it's not nullptr.
  void HandleMessage(const RosMessage& message) {
    SPDLOG_TRACE(drake::log(), "Receiving ROS {} message", topic_);
    std::lock_guard<std::mutex> lock(received_message_mutex_);
    received_message_ = message;
    received_message_count_++;
    received_message_condition_variable_.notify_all();
  }

  // This pair of methods is used for the output port when we're using a
  // serializer.
  std::unique_ptr<systems::AbstractValue> AllocateOutputValue() const {
    return std::make_unique<systems::Value<RosMessage>>(RosMessage{});
  }
  void CalcOutputValue(const systems::Context<double>& context,
                       systems::AbstractValue* output_value) const {
    output_value->SetFrom(
        context.get_abstract_state()->get_value(kStateIndexMessage));
  }

  // The topic on which to receive ROS messages.
  const std::string topic_;

  // The mutex that guards received_message_ and received_message_count_.
  mutable std::mutex received_message_mutex_;

  // A condition variable that's signaled every time the handler is called.
  mutable std::condition_variable received_message_condition_variable_;

  // The most recently received ROS message.
  RosMessage received_message_{};

  // A message counter that's incremented every time the handler is called.
  int received_message_count_{0};

  ros::NodeHandle* const node_handle_{};
  ros::Subscriber subscriber_;

  constexpr static int kStateIndexMessage = 0;
  constexpr static int kStateIndexMessageCount = 1;
};

}  // namespace drake_ros_systems
