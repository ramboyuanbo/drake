#pragma once

#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#include "drake/common/text_logging.h"
#include "drake/systems/framework/basic_vector.h"
#include "drake/systems/framework/context.h"
#include "drake/systems/framework/diagram_continuous_state.h"
#include "drake/systems/framework/input_port_evaluator_interface.h"
#include "drake/systems/framework/state.h"
#include "drake/systems/framework/supervector.h"
#include "drake/systems/framework/system_input.h"
#include "drake/systems/framework/system_output.h"

namespace drake {
namespace systems {

/// The DiagramContext is a container for all of the data necessary to uniquely
/// determine the computations performed by a Diagram. Specifically, a
/// DiagramContext contains contexts and outputs for all the constituent
/// Systems, wired up as specified by calls to `DiagramContext::Connect`.
///
/// In general, users should not need to interact with a DiagramContext
/// directly. Use the accessors on Diagram instead.
///
/// @tparam T The mathematical type of the context, which must be a valid Eigen
///           scalar.
template <typename T>
class DiagramContext : public Context<T> {
 public:
  typedef int SystemIndex;
  typedef int PortIndex;
  typedef std::pair<SystemIndex, PortIndex> PortIdentifier;

  /// Constructs a DiagramContext with the given @p num_subsystems, which is
  /// final: you cannot resize a DiagramContext after construction.
  explicit DiagramContext(const int num_subsystems)
      : outputs_(num_subsystems),
        contexts_(num_subsystems) {
    Context<T>::BuildCacheTickets();
  }

  /// Declares a new subsystem in the DiagramContext. Subsystems are identified
  /// by number. If the subsystem has already been declared, aborts.
  ///
  /// User code should not call this method. It is for use during Diagram
  /// context allocation only.
  void AddSystem(SystemIndex index, std::unique_ptr<Context<T>> context,
                 std::unique_ptr<SystemOutput<T>> output) {
    DRAKE_DEMAND(index >= 0 && index < num_subsystems());
    DRAKE_DEMAND(contexts_[index] == nullptr);
    DRAKE_DEMAND(outputs_[index] == nullptr);
    context->set_parent_and_index(this, index);

    // Take ownership of the context and the output.
    contexts_[index] = std::move(context);
    outputs_[index] = std::move(output);
  }

  /// Declares that a particular input port of a particular subsystem is an
  /// input to the entire Diagram that allocates this Context. Aborts if the
  /// subsystem has not been added to the DiagramContext.
  ///
  /// User code should not call this method. It is for use during Diagram
  /// context allocation only.
  void ExportInput(const PortIdentifier& id) {
    const SystemIndex system_index = id.first;
    DRAKE_DEMAND(contexts_[system_index] != nullptr);
    input_ids_.emplace_back(id);
  }

  /// Declares that a particular output port of a particular subsystem is an
  /// output of the entire Diagram that allocates this Context. Aborts if the
  /// subsystem has not been added to the DiagramContext.
  ///
  /// User code should not call this method. It is for use during Diagram
  /// context allocation only.
  void ExportOutput(const PortIdentifier& id) {
    const SystemIndex system_index = id.first;
    DRAKE_DEMAND(contexts_[system_index] != nullptr);
    output_ids_.emplace_back(id);
  }

  /// Declares that the output port specified by @p src is connected to the
  /// input port specified by @p dest.
  ///
  /// User code should not call this method. It is for use during Diagram
  /// context allocation only.
  void Connect(const PortIdentifier& src, const PortIdentifier& dest) {
    // Identify and validate the source port.
    SystemIndex src_system_index = src.first;
    PortIndex src_port_index = src.second;
    SystemOutput<T>* src_ports = GetSubsystemOutput(src_system_index);
    DRAKE_DEMAND(src_port_index >= 0);
    DRAKE_DEMAND(src_port_index < src_ports->get_num_ports());
    OutputPort* output_port = src_ports->get_mutable_port(src_port_index);

    // Identify and validate the destination port.
    SystemIndex dest_system_index = dest.first;
    PortIndex dest_port_index = dest.second;
    Context<T>* dest_context = GetMutableSubsystemContext(dest_system_index);
    DRAKE_DEMAND(dest_port_index >= 0);
    DRAKE_DEMAND(dest_port_index < dest_context->get_num_input_ports());

    // Construct and install the destination port.
    auto input_port = std::make_unique<DependentInputPort>(output_port);
    dest_context->SetInputPort(dest_port_index, std::move(input_port));

    // Remember the graph structure.
    dependency_graph_[dest] = src;
    inverse_dependency_graph_[src].push_back(dest);
  }

  /// Generates the state vector for the entire diagram by wrapping the states
  /// of all the constituent diagrams.
  ///
  /// User code should not call this method. It is for use during Diagram
  /// context allocation only.
  void MakeState() {
    std::vector<ContinuousState<T>*> sub_xcs;
    std::vector<BasicVector<T>*> sub_xds;
    std::vector<AbstractValue*> sub_xms;
    for (auto& context : contexts_) {
      // Continuous
      sub_xcs.push_back(context->get_mutable_continuous_state());
      // Difference
      const std::vector<BasicVector<T>*>& xd_data =
          context->get_mutable_difference_state()->get_data();
      sub_xds.insert(sub_xds.end(), xd_data.begin(), xd_data.end());
      // Modal
      ModalState* xm = context->get_mutable_modal_state();
      for (int i = 0; i < xm->size(); ++i) {
        sub_xms.push_back(&xm->get_mutable_modal_state(i));
      }
    }
    // The wrapper states do not own the constituent state.
    this->set_continuous_state(
        std::make_unique<DiagramContinuousState<T>>(sub_xcs));
    this->set_difference_state(std::make_unique<DifferenceState<T>>(sub_xds));
    this->set_modal_state(std::make_unique<ModalState>(sub_xms));
  }

  /// Returns the output structure for a given constituent system at @p index.
  /// Aborts if @p index is out of bounds, or if no system has been added to the
  /// DiagramContext at that index.
  SystemOutput<T>* GetSubsystemOutput(SystemIndex index) const {
    DRAKE_DEMAND(index >= 0 && index < num_subsystems());
    DRAKE_DEMAND(outputs_[index] != nullptr);
    return outputs_[index].get();
  }

  /// Returns the context structure for a given constituent system @p index.
  /// Aborts if @p index is out of bounds, or if no system has been added to the
  /// DiagramContext at that index.
  /// TODO(david-german-tri): Rename to get_subsystem_context.
  const Context<T>* GetSubsystemContext(SystemIndex index) const {
    DRAKE_DEMAND(index >= 0 && index < num_subsystems());
    DRAKE_DEMAND(contexts_[index] != nullptr);
    return contexts_[index].get();
  }

  /// Returns the context structure for a given subsystem @p index.
  /// Aborts if @p index is out of bounds, or if no system has been added to the
  /// DiagramContext at that index.
  /// TODO(david-german-tri): Rename to get_mutable_subsystem_context.
  Context<T>* GetMutableSubsystemContext(SystemIndex index) {
    DRAKE_DEMAND(index >= 0 && index < num_subsystems());
    DRAKE_DEMAND(contexts_[index] != nullptr);
    return contexts_[index].get();
  }

  /// Recursively sets the time on this context and all subcontexts.
  void set_time(const T& time_sec) override {
    Context<T>::set_time(time_sec);
    for (auto& subcontext : contexts_) {
      if (subcontext != nullptr) {
        subcontext->set_time(time_sec);
      }
    }
  }

  void MarkOutputPortFresh(int port_index) const override {
    DRAKE_ASSERT(port_index >= 0 && port_index < get_num_output_ports());
    const SystemIndex subsystem_index = output_ids_[port_index].first;
    const PortIndex subsystem_port_index = output_ids_[port_index].second;
    contexts_[subsystem_index]->MarkOutputPortFresh(subsystem_port_index);
  }

  bool IsOutputPortFresh(int port_index) const override {
    DRAKE_ASSERT(port_index >= 0 && port_index < get_num_output_ports());
    const SystemIndex subsystem_index = output_ids_[port_index].first;
    const PortIndex subsystem_port_index = output_ids_[port_index].second;
    log()->info("In DiagramContext<T>::IsOutputPortFresh");
    return contexts_[subsystem_index]->IsOutputPortFresh(subsystem_port_index);
  }

  bool IsEvaluationFresh(SystemIndex index) const {
    DRAKE_ASSERT(index >= 0 && index < num_subsystems());
    return GetSubsystemContext(index)->AreOutputPortsFresh();
  }

  void MarkEvaluationFresh(SystemIndex index) const {
    DRAKE_ASSERT(index >= 0 && index < num_subsystems());
    GetSubsystemContext(index)->MarkOutputPortsFresh();
  }

  /// Notifies contexts that depend on the output port @p port_index of the
  /// system at @p system_index that the contents of that port are no
  /// longer valid. This may provoke a long, recursive chain of invalidation.
  void PropagateInvalidOutputs(int system_index,
                               int port_index) const override {
    // TODO(david-german-tri): What goes here??
  }

  int get_num_input_ports() const override {
    return static_cast<int>(input_ids_.size());
  }

  int get_num_output_ports() const override {
    return static_cast<int>(output_ids_.size());
  }

  const State<T>& get_state() const override { return state_; }

 protected:
  void DoSetInputPort(int index, std::unique_ptr<InputPort> port) override {
    const PortIdentifier& id = input_ids_[index];
    SystemIndex system_index = id.first;
    PortIndex port_index = id.second;
    GetMutableSubsystemContext(system_index)
        ->SetInputPort(port_index, std::move(port));
  }

  DiagramContext<T>* DoClone() const override {
    DRAKE_ASSERT(contexts_.size() == outputs_.size());
    DiagramContext<T>* clone = new DiagramContext(num_subsystems());

    // Clone all the subsystem contexts and outputs.
    for (int i = 0; i < num_subsystems(); ++i) {
      DRAKE_DEMAND(contexts_[i] != nullptr);
      DRAKE_DEMAND(outputs_[i] != nullptr);
      // When a leaf context is cloned, it will clone the data that currently
      // appears on each of its input ports into a FreestandingInputPort.
      clone->AddSystem(i, contexts_[i]->Clone(), outputs_[i]->Clone());
    }

    // Build a superstate over the subsystem contexts.
    clone->MakeState();

    // Clone the internal graph structure. After this is done, the clone will
    // still have FreestandingInputPorts at the inputs to the Diagram itself,
    // but all of the intermediate nodes will have DependentInputPorts.
    for (const auto& connection : dependency_graph_) {
      const PortIdentifier& src = connection.second;
      const PortIdentifier& dest = connection.first;
      clone->Connect(src, dest);
    }

    // Clone the external input structure.
    for (const PortIdentifier& id : input_ids_) {
      clone->ExportInput(id);
    }
    return clone;
  }

  /// Returns the input port at the given @p index, which of course belongs
  /// to the subsystem whose input was exposed at that index.
  const InputPort* GetInputPort(int index) const override {
    DRAKE_ASSERT(index >= 0 && index < get_num_input_ports());
    const PortIdentifier& id = input_ids_[index];
    SystemIndex system_index = id.first;
    PortIndex port_index = id.second;
    return Context<T>::GetInputPort(*GetSubsystemContext(system_index),
                                    port_index);
  }

 private:
  int num_subsystems() const {
    DRAKE_ASSERT(contexts_.size() == outputs_.size());
    return static_cast<int>(contexts_.size());
  }

  std::vector<PortIdentifier> input_ids_;
  std::vector<PortIdentifier> output_ids_;

  // The outputs are stored in SystemIndex order, and outputs_ is equal in
  // length to the number of subsystems specified at construction time.
  std::vector<std::unique_ptr<SystemOutput<T>>> outputs_;
  // The contexts are stored in SystemIndex order, and contexts_ is equal in
  // length to the number of subsystems specified at construction time.
  std::vector<std::unique_ptr<Context<T>>> contexts_;

  // A map from the input ports of constituent systems, to the output ports of
  // the systems on which they depend.
  std::map<PortIdentifier, PortIdentifier> dependency_graph_;

  // A map from the output ports of constituent systems, to the input ports of
  // the systems on which they depend.
  std::map<PortIdentifier, std::vector<PortIdentifier>>
  inverse_dependency_graph_;

  // The internal state of the System.
  State<T> state_;
};

}  // namespace systems
}  // namespace drake
