#include "Horo/Debug/ConsoleSceneBindings.h"

#include "Horo/Debug/ConsoleProduct.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"

#include <algorithm>
#include <array>
#include <charconv>

#if !defined(HORO_PROFILE_EDITOR) && !defined(HORO_PROFILE_DEVELOPMENT)
#error "Development scene inspection must be omitted from other product artifacts."
#endif

namespace Horo::Debug {
    namespace {
        /** @brief Formats only bounded public values, with one owned result buffer per explicit inspection request. */
        class SceneText final {
        public:
            explicit SceneText(std::size_t limit) : limit_(limit) {
                text_.reserve(limit);
            }

            /** @brief Appends finite ASCII presentation text under the command output budget. */
            void Append(std::string_view text) {
                text_.append(text.substr(0, limit_ - text_.size()));
            }

            /** @brief Formats one typed number without locale state or per-number allocation. */
            template <typename T> void Number(T value) {
                std::array<char, 64> buffer{};
                const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
                if (error == std::errc{})
                    Append({buffer.data(), static_cast<std::size_t>(end - buffer.data())});
            }

            /** @brief Prints a complete existing entity identity without resolving a different world. */
            void Reference(const Runtime::EntityRef entity) {
                Number(entity.runtime.value);
                Append(":");
                Number(entity.entity.index);
                Append(":");
                Number(entity.entity.generation);
            }

            /** @brief Reports byte-budget exhaustion. @return True when no more output fits. */
            [[nodiscard]] bool Full() const noexcept {
                return text_.size() == limit_;
            }

            /** @brief Transfers the bounded presentation output. @return Owned safe ASCII text. */
            [[nodiscard]] DebugCommandOutput Take() {
                return {std::move(text_)};
            }

        private:
            std::size_t limit_;
            std::string text_;
        };

        /** @brief Retains a narrow public Scene owner and acquires views only at command execution. */
        class SceneCommand final : public ICommandHandler {
        public:
            SceneCommand(DebugCommandId id, std::shared_ptr<Runtime::RuntimeSceneService> scene, ConsoleSceneInspectionLimits limits,
                         std::size_t outputLimit)
                : id_(id), scene_(std::move(scene)), limits_(limits), outputLimit_(outputLimit), owner_(std::this_thread::get_id()) {}

            /** @copydoc ICommandHandler::Execute */
            Result<DebugCommandOutput> Execute(const DebugCommandContext &, std::span<const ArgumentValue> arguments,
                                               const CancellationToken &cancellation, const JobExecutionContext *) override {
                if (std::this_thread::get_id() != owner_)
                    return Result<DebugCommandOutput>::Failure(MakeCommandError(CommandError::WrongThread));
                if (cancellation.IsCancellationRequested())
                    return Result<DebugCommandOutput>::Failure(JobCancelled().ErrorValue());
                const auto view = scene_->ActiveScene();
                if (!view || !view->IsCurrent())
                    return Result<DebugCommandOutput>::Failure(MakeCommandError(CommandError::MissingCapability));
                if (id_ == SceneTreeCommandId)
                    return Tree(*view, cancellation);
                const auto reference = std::get<DebugEntityReference>(arguments.front());
                auto entity = view->Get({Runtime::SceneRuntimeId{reference.scene}, {reference.index, reference.generation}});
                if (entity.HasError())
                    return Result<DebugCommandOutput>::Failure(entity.ErrorValue());
                SceneText text(outputLimit_);
                const auto &value = entity.Value();
                text.Reference(value.entity);
                if (value.parent) {
                    text.Append(" parent=");
                    text.Reference(*value.parent);
                }
                if (value.localTransform) {
                    text.Append(" translation=");
                    text.Number(value.localTransform->translation.x);
                    text.Append(",");
                    text.Number(value.localTransform->translation.y);
                    text.Append(",");
                    text.Number(value.localTransform->translation.z);
                    text.Append(" scale=");
                    text.Number(value.localTransform->scale.x);
                    text.Append(",");
                    text.Number(value.localTransform->scale.y);
                    text.Append(",");
                    text.Number(value.localTransform->scale.z);
                }
                if (value.components) {
                    text.Append(" colliders=");
                    text.Number(value.components->colliders.size());
                    text.Append(" constraints=");
                    text.Number(value.components->physicsConstraints.size());
                    text.Append(" behaviors=");
                    text.Number(value.components->behaviors.size());
                    text.Append(" gameplay_components=");
                    text.Number(value.components->gameplayComponents.size());
                    text.Append(" camera=");
                    text.Number(static_cast<unsigned>(value.components->camera.has_value()));
                    text.Append(" rigid_body=");
                    text.Number(static_cast<unsigned>(value.components->rigidBody.has_value()));
                }
                return Result<DebugCommandOutput>::Success(text.Take());
            }

        private:
            /** @brief Emits a bounded ECS hierarchy view; parent references preserve topology without an unbounded tree walk. */
            Result<DebugCommandOutput> Tree(const Runtime::RuntimeSceneView &view, const CancellationToken &cancellation) const {
                SceneText text(outputLimit_);
                text.Append("scene=");
                text.Number(view.RuntimeId().value);
                text.Append(" revision=");
                text.Number(view.StructuralRevision());
                text.Append("\n");
                std::size_t reported{};
                const auto visited = std::min(view.SlotCount(), limits_.maximumVisitedSlots);
                for (std::size_t i = 0; i < visited && reported < limits_.maximumReportedEntities && !text.Full(); ++i) {
                    if (cancellation.IsCancellationRequested())
                        return Result<DebugCommandOutput>::Failure(JobCancelled().ErrorValue());
                    const auto entity = view.EntityAt(i);
                    if (!entity)
                        continue;
                    text.Reference(entity->entity);
                    if (entity->parent) {
                        text.Append(" parent=");
                        text.Reference(*entity->parent);
                    }
                    text.Append("\n");
                    ++reported;
                }
                if (visited < view.SlotCount() || reported == limits_.maximumReportedEntities)
                    text.Append("[bounded view]\n");
                return Result<DebugCommandOutput>::Success(text.Take());
            }

            DebugCommandId id_;
            std::shared_ptr<Runtime::RuntimeSceneService> scene_;
            ConsoleSceneInspectionLimits limits_;
            std::size_t outputLimit_;
            std::thread::id owner_;
        };
    }  // namespace

    /** @copydoc BindConsoleSceneCommands */
    Result<std::vector<CommandBinding>> BindConsoleSceneCommands(std::shared_ptr<const CommandRegistry> registry,
                                                                 std::shared_ptr<Runtime::RuntimeSceneService> scene,
                                                                 const ConsoleSceneInspectionLimits limits) {
        if (!registry || registry->Profile() != CompiledConsoleProfile() || limits.maximumVisitedSlots == 0 ||
            limits.maximumVisitedSlots > 16384 || limits.maximumReportedEntities == 0 || limits.maximumReportedEntities > 512)
            return Result<std::vector<CommandBinding>>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
        std::vector<CommandBinding> bindings;
        for (const auto id : {SceneTreeCommandId, InspectCommandId}) {
            const auto descriptor = registry->Resolve(id, {15, true, true});
            if (descriptor.HasError())
                continue;
            const auto &value = descriptor.Value();
            const bool inspect = id == InspectCommandId;
            if (value.permissions != CommandPermission::Developer || value.availability != CommandAvailability::DevelopmentOnly ||
                value.threadPolicy != CommandThreadPolicy::OwnerThreadNextFrame ||
                value.name != (inspect ? "game.inspect" : "game.scene_tree") || value.arguments.size() != (inspect ? 1U : 0U) ||
                (inspect &&
                 (value.arguments[0].type != DebugArgumentType::EntityId || !value.arguments[0].required || value.arguments[0].option)))
                return Result<std::vector<CommandBinding>>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
            if (!scene)
                return Result<std::vector<CommandBinding>>::Failure(MakeCommandError(CommandError::MissingCapability));
            bindings.push_back({id, std::make_shared<SceneCommand>(id, scene, limits, registry->Limits().outputBytes)});
        }
        return Result<std::vector<CommandBinding>>::Success(std::move(bindings));
    }
}  // namespace Horo::Debug
