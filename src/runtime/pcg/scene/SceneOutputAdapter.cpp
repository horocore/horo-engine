#include "Horo/PCGScene/SceneOutputAdapter.h"

#include "Horo/PCG/PCGErrors.h"

#include <algorithm>
#include <memory>
#include <new>
#include <nlohmann/json.hpp>

namespace Horo::PCG {
    namespace {
        template <typename T> Result<T> Reject(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        /** @brief Charge only after proving that addition fits the finite caller envelope. */
        bool Charge(const std::size_t amount, const std::size_t limit, std::size_t &bytes) {
            if (amount > limit || bytes > limit - amount)
                return false;
            bytes += amount;
            return true;
        }

        /** @brief Bound nested behavior field storage before invoking the scene codec. */
        bool FitsBehavior(const Gameplay::BehaviorComponent &behavior, const std::size_t limit, std::size_t &bytes) {
            if (behavior.fields.size() > 256 || !Charge(behavior.typeId.Value().size(), limit, bytes))
                return false;
            for (const auto &field : behavior.fields) {
                if (!Charge(field.name.size(), limit, bytes))
                    return false;
                if (const auto *value = std::get_if<std::string>(&field.value); value && !Charge(value->size(), limit, bytes))
                    return false;
            }
            return true;
        }

        /** @brief Bound variable payloads before canonical codec allocation. */
        bool FitsInput(const SceneSource::SceneObjectSnapshot &object, const std::size_t limit) {
            std::size_t bytes = object.name.size();
            const auto &components = object.components;
            if (bytes > limit || components.colliders.size() > 128 || components.physicsConstraints.size() > 128 ||
                components.behaviors.size() > 128 || components.gameplayComponents.size() > 128)
                return false;
            for (const auto &collider : components.colliders)
                if (collider.materials.size() > 128 ||
                    !Charge(collider.materials.size() * sizeof(Runtime::PhysicsColliderMaterialBinding), limit, bytes))
                    return false;
            for (const auto &component : components.gameplayComponents)
                if (!Charge(component.payload.size(), limit, bytes) || !Charge(component.typeId.Value().size(), limit, bytes))
                    return false;
            for (const auto &behavior : components.behaviors)
                if (!FitsBehavior(behavior, limit, bytes))
                    return false;
            return true;
        }

        /** @brief Charge copied vector metadata and owned strings/payloads without counting borrowed target storage. */
        std::size_t OwnedBytes(const SceneSource::SceneObjectSnapshot &object) {
            const auto &c = object.components;
            std::size_t bytes = object.name.size() + 1 + c.colliders.size() * sizeof(Runtime::ColliderComponent) +
                                c.physicsConstraints.size() * sizeof(Runtime::PhysicsConstraintComponent) +
                                c.behaviors.size() * sizeof(Gameplay::BehaviorComponent) +
                                c.gameplayComponents.size() * sizeof(Gameplay::SerializedComponent);
            for (const auto &component : c.gameplayComponents)
                bytes += component.payload.size() + component.typeId.Value().size() + 1;
            for (const auto &collider : c.colliders)
                bytes += collider.materials.size() * sizeof(Runtime::PhysicsColliderMaterialBinding);
            for (const auto &behavior : c.behaviors) {
                bytes += behavior.typeId.Value().size() + 1 + behavior.fields.size() * sizeof(Gameplay::BehaviorField);
                for (const auto &field : behavior.fields) {
                    bytes += field.name.size() + 1;
                    if (const auto *value = std::get_if<std::string>(&field.value))
                        bytes += value->size() + 1;
                }
            }
            return bytes;
        }

        /** @brief Validate using the canonical scene codec instead of inventing another component schema. */
        Result<std::string> Encode(const SceneSource::SceneObjectSnapshot &object, const std::size_t limit) {
            if (limit == 0 || limit > 16 * 1024 * 1024 || !FitsInput(object, limit))
                return Reject<std::string>(PCGErrors::GenerationPlanCapacityExceeded);
            if (!object.id.IsValid() || (object.parent && (!object.parent->IsValid() || *object.parent == object.id)) ||
                (object.meshAsset && (!object.meshAsset->IsValid() || object.primitiveMesh)))
                return Reject<std::string>(PCGErrors::GenerationPlanInvalid);
            try {
                auto bytes = SceneSource::EncodeSceneSource({std::span{&object, 1}, {}});
                if (bytes.size() > limit)
                    return Reject<std::string>(PCGErrors::GenerationPlanCapacityExceeded);
                if (auto decoded = SceneSource::DecodeSceneSource(bytes); decoded.HasError())
                    return Result<std::string>::Failure(decoded.ErrorValue());
                return Result<std::string>::Success(std::move(bytes));
            } catch (const nlohmann::json::type_error &) {
                return Reject<std::string>(PCGErrors::GenerationPlanInvalid);
            } catch (const nlohmann::json::parse_error &) {
                return Reject<std::string>(PCGErrors::GenerationPlanInvalid);
            } catch (const nlohmann::json::out_of_range &) {
                return Reject<std::string>(PCGErrors::GenerationPlanInvalid);
            }
        }

        /** @brief Preserve the existing generation-plan owner, provenance, capability and lifecycle validation. */
        Result<PCGGenerationPlan> Revalidate(const PCGGenerationPlan &plan, const PCGGenerationPlanContext &context) {
            PCGGenerationPlanCandidate candidate{plan.Version(),
                                                 plan.Id(),
                                                 plan.Execution(),
                                                 plan.Seed(),
                                                 plan.Lineage(),
                                                 plan.Set(),
                                                 plan.SetRevision(),
                                                 plan.Cell(),
                                                 plan.Target().owner,
                                                 plan.RequiredCapabilities(),
                                                 plan.Target(),
                                                 {plan.Dependencies().begin(), plan.Dependencies().end()},
                                                 {plan.Outputs().begin(), plan.Outputs().end()}};
            return CreatePCGGenerationPlan(std::move(candidate), context);
        }

        /** @brief Require mesh receipts to match both the declared dependency and this exact output's closure. */
        bool ValidMesh(const PCGSceneOutputIntent &intent, const PCGOutputDelta &delta, const PCGGenerationPlan &plan) {
            if (!intent.desired.meshAsset)
                return !intent.mesh;
            if (!intent.mesh || intent.mesh->asset != *intent.desired.meshAsset)
                return false;
            const auto &dependency = intent.mesh->dependency;
            return std::ranges::find(delta.dependencies, dependency.id) != delta.dependencies.end() &&
                   std::ranges::find(plan.Dependencies(), dependency) != plan.Dependencies().end();
        }

        /** @brief Validate complete owner-assigned mappings, including collisions with authored objects. */
        Result<void> ValidateMappings(const PCGSceneOutputContext &context) {
            if (context.currentMappings.size() > context.maximumCommands || context.occupiedObjects.size() > 65536)
                return Reject<void>(PCGErrors::GenerationPlanCapacityExceeded);
            for (std::size_t i = 0; i < context.occupiedObjects.size(); ++i) {
                const auto object = context.occupiedObjects[i];
                if (!object.IsValid() || (i != 0 && context.occupiedObjects[i - 1] >= object))
                    return Reject<void>(PCGErrors::GenerationPlanInvalid);
            }
            for (std::size_t i = 0; i < context.currentMappings.size(); ++i) {
                const auto &mapping = context.currentMappings[i];
                if (!mapping.logical.IsValid() || !mapping.object.IsValid() ||
                    !std::ranges::binary_search(context.occupiedObjects, mapping.object))
                    return Reject<void>(PCGErrors::GenerationOwnershipMismatch);
                for (const auto &previous : context.currentMappings.first(i))
                    if (previous.logical == mapping.logical || previous.object == mapping.object)
                        return Reject<void>(PCGErrors::GenerationOwnershipMismatch);
            }
            return Result<void>::Success();
        }

        /** @brief Index staged parent edges once and walk each edge at most twice without recursion. */
        Result<void> ValidateParents(const std::span<const PCGSceneOutputCommand> commands,
                                     const std::span<const SceneSource::SceneObjectId> occupied) {
            std::vector<const PCGSceneOutputCommand *> objects;
            objects.reserve(commands.size());
            for (const auto &command : commands)
                objects.push_back(&command);
            const auto objectId = [](const PCGSceneOutputCommand *command) {
                return command->object;
            };
            std::ranges::sort(objects, {}, objectId);
            const auto terminal = commands.size();
            std::vector parents(commands.size(), terminal);
            for (std::size_t index = 0; index < commands.size(); ++index) {
                const auto &command = commands[index];
                if (!command.desired || !command.desired->parent)
                    continue;
                const auto parent = *command.desired->parent;
                const auto found = std::ranges::lower_bound(objects, parent, {}, objectId);
                if (found == objects.end() || (*found)->object != parent) {
                    if (!std::ranges::binary_search(occupied, parent))
                        return Reject<void>(PCGErrors::GenerationPlanInvalid);
                } else {
                    if (!(*found)->desired)
                        return Reject<void>(PCGErrors::GenerationOwnershipMismatch);
                    parents[index] = static_cast<std::size_t>(*found - commands.data());
                }
            }
            // 0: unseen, 1: current chain, 2: fully validated. No target state is touched.
            std::vector<std::uint8_t> colors(commands.size());
            for (std::size_t root = 0; root < commands.size(); ++root) {
                auto index = root;
                while (index != terminal && colors[index] == 0) {
                    colors[index] = 1;
                    index = parents[index];
                }
                if (index != terminal && colors[index] == 1)
                    return Reject<void>(PCGErrors::GenerationPlanInvalid);
                index = root;
                while (index != terminal && colors[index] == 1) {
                    colors[index] = 2;
                    index = parents[index];
                }
            }
            return Result<void>::Success();
        }

        /** @brief Validate and own one desired payload while preserving exact owner mapping and byte admission. */
        Result<void> StageDesired(PCGSceneOutputCommand &command, const PCGSceneOutputIntent &intent, const PCGSceneOutputMapping *mapping,
                                  const PCGGenerationPlan &plan, const PCGSceneOutputContext &context, std::size_t &bytes) {
            const auto &delta = command.delta;
            command.object = intent.desired.id;
            if (const bool occupied = std::ranges::binary_search(context.occupiedObjects, command.object);
                (delta.kind == PCGOutputDeltaKind::Create && (occupied || mapping)) ||
                (delta.kind == PCGOutputDeltaKind::Update && (!mapping || mapping->object != command.object)))
                return Reject<void>(PCGErrors::GenerationOwnershipMismatch);
            if (!ValidMesh(intent, delta, plan))
                return Reject<void>(PCGErrors::GenerationPlanInvalid);
            auto encoded = Encode(intent.desired, context.maximumPayloadBytes);
            if (encoded.HasError())
                return Result<void>::Failure(encoded.ErrorValue());
            if (ComputeSha256(std::as_bytes(std::span{encoded.Value().data(), encoded.Value().size()})) != delta.desiredContent)
                return Reject<void>(PCGErrors::GenerationPlanStale);
            const auto payloadBytes = OwnedBytes(intent.desired);
            const auto preparationBytes = encoded.Value().size();
            if (payloadBytes > context.maximumRetainedBytes - bytes ||
                preparationBytes > context.maximumRetainedBytes - bytes - payloadBytes)
                return Reject<void>(PCGErrors::GenerationPlanCapacityExceeded);
            bytes += payloadBytes + preparationBytes;
            command.desired = intent.desired;
            command.mesh = intent.mesh;
            return Result<void>::Success();
        }

        /** @brief Translate exactly one create/update/remove and account for its dependency storage. */
        Result<PCGSceneOutputCommand> TranslateDelta(const PCGOutputDelta &delta, const PCGGenerationPlan &plan,
                                                     const std::span<const PCGSceneOutputIntent> intents,
                                                     const PCGSceneOutputContext &context, std::size_t &bytes, std::size_t &usedIntents) {
            const auto found = std::ranges::find(context.currentMappings, delta.provenance.logicalOutput, &PCGSceneOutputMapping::logical);
            const auto *mapping = found == context.currentMappings.end() ? nullptr : std::to_address(found);
            PCGSceneOutputCommand command{delta, {}, {}, {}};
            if (delta.kind == PCGOutputDeltaKind::Remove) {
                if (!mapping)
                    return Reject<PCGSceneOutputCommand>(PCGErrors::GenerationOwnershipMismatch);
                command.object = mapping->object;
            } else {
                const auto intent = std::ranges::find(intents, delta.provenance.logicalOutput, &PCGSceneOutputIntent::logical);
                if (intent == intents.end() || std::ranges::count(intents, intent->logical, &PCGSceneOutputIntent::logical) != 1)
                    return Reject<PCGSceneOutputCommand>(PCGErrors::GenerationPlanInvalid);
                ++usedIntents;
                auto staged = StageDesired(command, *intent, mapping, plan, context, bytes);
                if (staged.HasError())
                    return Result<PCGSceneOutputCommand>::Failure(staged.ErrorValue());
            }
            const auto dependencyBytes = delta.dependencies.size() * sizeof(GenerationDependencyId);
            if (dependencyBytes > context.maximumRetainedBytes - bytes)
                return Reject<PCGSceneOutputCommand>(PCGErrors::GenerationPlanCapacityExceeded);
            bytes += dependencyBytes;
            return Result<PCGSceneOutputCommand>::Success(std::move(command));
        }

        /** @brief Build the canonical detached batch and validate its complete staged hierarchy under a finite charge. */
        Result<std::vector<PCGSceneOutputCommand>> TranslateCommands(const PCGGenerationPlan &plan,
                                                                     const std::span<const PCGSceneOutputIntent> intents,
                                                                     const PCGSceneOutputContext &context, std::size_t &bytes) {
            using Commands = std::vector<PCGSceneOutputCommand>;
            Commands commands;
            commands.reserve(plan.Outputs().size());
            bytes = sizeof(PCGSceneOutputBatch) + plan.Outputs().size() * sizeof(PCGSceneOutputCommand);
            if (bytes > context.maximumRetainedBytes)
                return Reject<Commands>(PCGErrors::GenerationPlanCapacityExceeded);
            std::size_t usedIntents{};
            for (const auto &delta : plan.Outputs()) {
                auto command = TranslateDelta(delta, plan, intents, context, bytes, usedIntents);
                if (command.HasError())
                    return Result<Commands>::Failure(command.ErrorValue());
                if (std::ranges::find(commands, command.Value().object, &PCGSceneOutputCommand::object) != commands.end())
                    return Reject<Commands>(PCGErrors::GenerationOwnershipMismatch);
                commands.push_back(std::move(command).Value());
            }
            if (usedIntents != intents.size())
                return Reject<Commands>(PCGErrors::GenerationPlanInvalid);
            const auto hierarchyBytes =
                commands.size() * (sizeof(const PCGSceneOutputCommand *) + sizeof(std::size_t) + sizeof(std::uint8_t));
            if (hierarchyBytes > context.maximumRetainedBytes - bytes)
                return Reject<Commands>(PCGErrors::GenerationPlanCapacityExceeded);
            bytes += hierarchyBytes;
            if (auto parents = ValidateParents(commands, context.occupiedObjects); parents.HasError())
                return Result<Commands>::Failure(parents.ErrorValue());
            return Result<Commands>::Success(std::move(commands));
        }
    }  // namespace

    /** @copydoc FingerprintPCGSceneOutput */
    Result<PCGGenerationDigest> FingerprintPCGSceneOutput(const SceneSource::SceneObjectSnapshot &desired, const std::size_t maximumBytes) {
        try {
            auto authored = desired;
            authored.generated.reset();
            auto encoded = Encode(authored, maximumBytes);
            if (encoded.HasError())
                return Result<PCGGenerationDigest>::Failure(encoded.ErrorValue());
            return Result<PCGGenerationDigest>::Success(
                ComputeSha256(std::as_bytes(std::span{encoded.Value().data(), encoded.Value().size()})));
        } catch (const std::bad_alloc &) {
            return Reject<PCGGenerationDigest>(PCGErrors::GenerationPlanCapacityExceeded);
        }
    }

    /** @copydoc TranslatePCGSceneOutputs */
    Result<PCGSceneOutputBatch> TranslatePCGSceneOutputs(const PCGGenerationPlan &plan, const std::span<const PCGSceneOutputIntent> intents,
                                                         const PCGSceneOutputContext &context) {
        if (context.generation.admission != PCGGenerationPlanAdmission::Accepting)
            return Reject<PCGSceneOutputBatch>(PCGErrors::GenerationPlanLifecycleUnavailable);
        if (context.maximumCommands == 0 || context.maximumCommands > 4096 || context.maximumRetainedBytes == 0 ||
            plan.Outputs().size() > context.maximumCommands || intents.size() > plan.Outputs().size())
            return Reject<PCGSceneOutputBatch>(PCGErrors::GenerationPlanCapacityExceeded);
        if (!plan.RequiredCapabilities().Contains(PCGCapability::SceneOutput))
            return Reject<PCGSceneOutputBatch>(PCGErrors::UnsupportedCapability);
        try {
            if (auto valid = ValidateMappings(context); valid.HasError())
                return Result<PCGSceneOutputBatch>::Failure(valid.ErrorValue());
            if (auto checked = Revalidate(plan, context.generation); checked.HasError())
                return Result<PCGSceneOutputBatch>::Failure(checked.ErrorValue());
            std::size_t bytes{};
            auto commands = TranslateCommands(plan, intents, context, bytes);
            if (commands.HasError())
                return Result<PCGSceneOutputBatch>::Failure(commands.ErrorValue());
            return Result<PCGSceneOutputBatch>::Success(PCGSceneOutputBatch{plan, std::move(commands).Value(), bytes});
        } catch (const std::bad_alloc &) {
            return Reject<PCGSceneOutputBatch>(PCGErrors::GenerationPlanCapacityExceeded);
        }
    }
}  // namespace Horo::PCG
