#include "DecisionSourceWire.h"

#include <algorithm>

namespace Horo::AI::Detail {
    namespace {
        void WriteProvider(DecisionWriter &writer, const std::optional<StateMachineProviderAction> &action) {
            writer.Unsigned(action.has_value(), 1);
            if (!action)
                return;
            WriteProviderNode(writer, action->binding, action->properties);
        }

        std::optional<StateMachineProviderAction> ReadProvider(DecisionReader &reader) {
            if (!reader.Boolean())
                return {};
            StateMachineProviderAction action;
            ReadProviderNode(reader, action.binding, action.properties);
            return action;
        }

        void WriteState(DecisionWriter &writer, const StateMachineState &state) {
            writer.IdentityValue(state.id);
            writer.IdentityValue(state.parent, true);
            writer.IdentityValue(state.initialChild, true);
            writer.IdentityValue(state.entry.value_or(TaskId{}), true);
            writer.IdentityValue(state.update.value_or(TaskId{}), true);
            writer.IdentityValue(state.exit.value_or(TaskId{}), true);
            WriteProvider(writer, state.entryProvider);
            WriteProvider(writer, state.updateProvider);
            WriteProvider(writer, state.exitProvider);
            WriteCall(writer, state.call);
            writer.Enumeration(state.terminal);
            WriteLocation(writer, state.source);
        }

        std::optional<TaskId> ReadTask(DecisionReader &reader) {
            auto task = reader.IdentityValue<TaskId>(true);
            return task.IsValid() ? std::optional{task} : std::nullopt;
        }

        StateMachineState ReadState(DecisionReader &reader) {
            StateMachineState state;
            state.id = reader.IdentityValue<DecisionNodeId>();
            state.parent = reader.IdentityValue<DecisionNodeId>(true);
            state.initialChild = reader.IdentityValue<DecisionNodeId>(true);
            state.entry = ReadTask(reader);
            state.update = ReadTask(reader);
            state.exit = ReadTask(reader);
            state.entryProvider = ReadProvider(reader);
            state.updateProvider = ReadProvider(reader);
            state.exitProvider = ReadProvider(reader);
            state.call = ReadCall(reader);
            state.terminal = reader.Enumeration(DecisionStateTerminal::Count);
            state.source = ReadLocation(reader);
            return state;
        }

        void WriteGuard(DecisionWriter &writer, const StateMachineGuard &guard) {
            writer.IdentityValue(guard.key);
            writer.Enumeration(guard.operation);
            WriteScalar(writer, guard.operand);
        }

        StateMachineGuard ReadGuard(DecisionReader &reader) {
            StateMachineGuard guard;
            guard.key = reader.IdentityValue<BlackboardKeyId>();
            guard.operation = reader.Enumeration(StateMachineGuardOperator::Count);
            guard.operand = ReadScalar(reader);
            return guard;
        }

        void WriteTransition(DecisionWriter &writer, const StateMachineTransition &transition) {
            writer.IdentityValue(transition.id);
            writer.IdentityValue(transition.source);
            writer.IdentityValue(transition.target);
            writer.Unsigned(std::bit_cast<std::uint32_t>(transition.priority), 4);
            writer.IdentityValue(transition.event.value_or(StateMachineEventId{}), true);
            WriteSequence<StateMachineGuard>(writer, transition.guards, StateMachineLimits::GuardsPerTransition, WriteGuard);
            writer.Unsigned(transition.childOutcome.has_value(), 1);
            if (transition.childOutcome)
                writer.Enumeration(*transition.childOutcome);
            WriteRequirements(writer, transition.requirements);
            WriteLocation(writer, transition.location);
        }

        AiTaskState ReadOutcome(DecisionReader &reader) {
            const auto ordinal = reader.Unsigned(1);
            if (ordinal == 0 || ordinal > static_cast<std::uint8_t>(AiTaskState::Cancelled))
                reader.Reject();
            return static_cast<AiTaskState>(ordinal);
        }

        StateMachineTransition ReadTransition(DecisionReader &reader) {
            StateMachineTransition transition;
            transition.id = reader.IdentityValue<DecisionNodeId>();
            transition.source = reader.IdentityValue<DecisionNodeId>();
            transition.target = reader.IdentityValue<DecisionNodeId>();
            transition.priority = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(reader.Unsigned(4)));
            if (const auto event = reader.IdentityValue<StateMachineEventId>(true); event.IsValid())
                transition.event = event;
            transition.guards = ReadSequence<StateMachineGuard>(reader, StateMachineLimits::GuardsPerTransition, ReadGuard);
            if (reader.Boolean())
                transition.childOutcome = ReadOutcome(reader);
            transition.requirements =
                ReadSequence<DecisionBlackboardRequirement>(reader, DecisionAssetValidationHardLimits::RequirementsPerNode,
                                                            ReadRequirement);
            transition.location = ReadLocation(reader);
            return transition;
        }

        void WriteMachineRecords(DecisionWriter &writer, const StateMachineAssetDescriptor &machine) {
            std::vector<const StateMachineState *> states;
            std::vector<const StateMachineTransition *> transitions;
            for (const auto &state : machine.states)
                states.push_back(&state);
            for (const auto &transition : machine.transitions)
                transitions.push_back(&transition);
            std::ranges::sort(states, {}, [](const auto *value) {
                return value->id;
            });
            std::ranges::sort(transitions, {}, [](const auto *value) {
                return value->id;
            });
            writer.Count(states.size(), StateMachineLimits::States);
            for (const auto *state : states) {
                if (!writer.Valid())
                    return;
                WriteState(writer, *state);
            }
            writer.Count(transitions.size(), StateMachineLimits::Transitions);
            for (const auto *transition : transitions) {
                if (!writer.Valid())
                    return;
                WriteTransition(writer, *transition);
            }
        }
    }  // namespace

    void WriteMachine(DecisionWriter &writer, const StateMachineAssetDescriptor &machine) {
        if (machine.states.size() > StateMachineLimits::States || machine.transitions.size() > StateMachineLimits::Transitions ||
            machine.maximumTransitionsPerStep == 0 || machine.maximumTransitionsPerStep > StateMachineLimits::TransitionsPerStep) {
            writer.Reject();
            return;
        }
        writer.IdentityValue(machine.initial);
        writer.Unsigned(machine.maximumTransitionsPerStep, 4);
        WriteMachineRecords(writer, machine);
    }

    StateMachineAssetDescriptor ReadMachine(DecisionReader &reader) {
        StateMachineAssetDescriptor machine;
        machine.initial = reader.IdentityValue<DecisionNodeId>();
        machine.maximumTransitionsPerStep = reader.Count(StateMachineLimits::TransitionsPerStep);
        machine.states = ReadSequence<StateMachineState>(reader, StateMachineLimits::States, ReadState);
        machine.transitions = ReadSequence<StateMachineTransition>(reader, StateMachineLimits::Transitions, ReadTransition);
        return machine;
    }
}  // namespace Horo::AI::Detail
