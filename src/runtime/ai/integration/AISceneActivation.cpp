#include "Horo/AI/AISceneActivation.h"

#include "Horo/AI/AIErrors.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <new>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Horo::AI {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor, std::string message) {
            return Result<T>::Failure(MakeError(descriptor, std::move(message)));
        }

        [[nodiscard]] Result<void> Failure(const ErrorCodeDescriptor &descriptor, std::string message) {
            return Result<void>::Failure(MakeError(descriptor, std::move(message)));
        }

        struct AgentRuntimeState final {
            AiAgentRuntimeRecord record;
            std::unique_ptr<BlackboardInstance> blackboard;
            std::unique_ptr<AiTaskLifecycle> task;
            CancellationSource cancellation;
            bool retired{};
        };

        struct EntityRefHash final {
            [[nodiscard]] std::size_t operator()(const Runtime::EntityRef &entity) const noexcept {
                std::size_t result = std::hash<std::uint64_t>{}(entity.runtime.value);
                result ^= std::hash<std::uint32_t>{}(entity.entity.index) + static_cast<std::size_t>(0x9e3779b9U) + (result << 6U) +
                          (result >> 2U);
                result ^= std::hash<std::uint32_t>{}(entity.entity.generation) + static_cast<std::size_t>(0x9e3779b9U) + (result << 6U) +
                          (result >> 2U);
                return result;
            }
        };

        [[nodiscard]] AgentRuntimeState *FindAgent(Detail::AiSceneRuntimeState &state, const AgentHandle handle) noexcept;
        [[nodiscard]] const AgentRuntimeState *FindAgent(const Detail::AiSceneRuntimeState &state, const AgentHandle handle) noexcept;

        [[nodiscard]] const AiControllerDescriptor *FindDescriptor(const std::span<const AiControllerDescriptor> descriptors,
                                                                   const ControllerTypeId controller) noexcept {
            const auto found = std::ranges::find(descriptors, controller, &AiControllerDescriptor::controller);
            return found == descriptors.end() ? nullptr : &*found;
        }

        [[nodiscard]] bool HasCapabilities(const AiCapabilitySet available, const AiCapabilitySet required) noexcept {
            return (required.bits & ~available.bits) == 0;
        }

        void CancelOwnedWork(AgentRuntimeState &agent) noexcept {
            agent.cancellation.RequestCancellation();
            if (agent.task) {
                (void)agent.task->RequestCancellation(AiTaskCancellationReason::OwnerShutdown);
                (void)agent.task->RetireAgentGeneration();
                if (const Result<bool> claimed = agent.task->ClaimCleanup(); claimed.HasValue() && claimed.Value())
                    (void)agent.task->CompleteCleanup();
                agent.task.reset();
            }
            if (agent.blackboard) {
                (void)agent.blackboard->TeardownAtBlackboardSync();
                agent.blackboard.reset();
            }
            agent.record.hasRunningTask = false;
            agent.record.hasBlackboard = false;
            agent.record.stagedCapabilities = {};
        }

        void ShutdownStateContents(Detail::AiSceneRuntimeState &state) noexcept;

        [[nodiscard]] Result<AiSceneActivationBinding> MakeBinding(const Runtime::RuntimeSceneView scene) {
            if (!scene.IsCurrent() || !scene.RuntimeId().IsValid())
                return Failure<AiSceneActivationBinding>(AIErrors::SceneActivationInvalid,
                                                         "The RuntimeScene view is stale or has no valid runtime identity.");
            const auto incarnation = AiRuntimeIncarnation::Create(scene.RuntimeId().value);
            if (incarnation.HasError())
                return Result<AiSceneActivationBinding>::Failure(incarnation.ErrorValue());
            return Result<AiSceneActivationBinding>::Success(
                AiSceneActivationBinding{.incarnation = incarnation.Value(), .scene = scene.RuntimeId()});
        }
    }  // namespace

    namespace Detail {
        struct AiSceneRuntimeState final {
            AiSceneActivationBinding binding;
            std::vector<AgentRuntimeState> agents;
            std::unordered_multimap<Runtime::EntityRef, std::size_t, EntityRefHash> agentsByOwner; /**< Stable owner-to-slot lookup. */
            std::uint32_t nextTaskSlot{};
        };
    }  // namespace Detail

    namespace {
        [[nodiscard]] AgentRuntimeState *FindAgent(Detail::AiSceneRuntimeState &state, const AgentHandle handle) noexcept {
            if (!handle.IsValid() || handle.incarnation != state.binding.incarnation || handle.slot.index >= state.agents.size())
                return nullptr;
            AgentRuntimeState &candidate = state.agents[handle.slot.index];
            if (candidate.retired || candidate.record.handle != handle)
                return nullptr;
            return &candidate;
        }

        [[nodiscard]] const AgentRuntimeState *FindAgent(const Detail::AiSceneRuntimeState &state, const AgentHandle handle) noexcept {
            if (!handle.IsValid() || handle.incarnation != state.binding.incarnation || handle.slot.index >= state.agents.size())
                return nullptr;
            const AgentRuntimeState &candidate = state.agents[handle.slot.index];
            if (candidate.retired || candidate.record.handle != handle)
                return nullptr;
            return &candidate;
        }

        void ShutdownStateContents(Detail::AiSceneRuntimeState &state) noexcept {
            for (AgentRuntimeState &agent : state.agents)
                CancelOwnedWork(agent);
            state.agents.clear();
            state.agentsByOwner.clear();
        }

        [[nodiscard]] Result<void> ValidateSceneInputs(const std::span<const AiSceneAgentDescriptor> agents,
                                                       const std::span<const AiControllerDescriptor> descriptors,
                                                       const AiSceneActivationBinding binding) {
            try {
                std::vector<AiSceneComponentView> views;
                views.reserve(agents.size());
                {
                    std::unordered_set<std::uint64_t> descriptorTypes;
                    descriptorTypes.reserve(descriptors.size());
                    for (const AiControllerDescriptor &descriptor : descriptors) {
                        if (const Result<void> valid = ValidateAiControllerDescriptor(descriptor); valid.HasError())
                            return Result<void>::Failure(valid.ErrorValue());
                        if (!descriptorTypes.insert(descriptor.controller.Value()).second)
                            return Failure(AIErrors::DescriptorConflict,
                                           "AI controller descriptor identities must be unique in one activation catalog.");
                    }
                }
                {
                    std::unordered_set<Runtime::EntityRef, EntityRefHash> owners;
                    owners.reserve(agents.size());
                    for (const AiSceneAgentDescriptor &agent : agents) {
                        views.push_back({.agent = &agent.agent, .controller = agent.controller ? &*agent.controller : nullptr});
                        if (!agent.owner.IsValid() || agent.owner.runtime != binding.scene || !owners.insert(agent.owner).second)
                            return Failure(AIErrors::SceneActivationInvalid,
                                           "AI scene agents must reference unique live entity generations.");
                    }
                }
                return ValidateAiSceneComponents(views);
            } catch (const std::bad_alloc &) {
                return Failure(AIErrors::AgentCapacityExceeded, "AI scene admission could not allocate bounded validation storage.");
            }
        }

        [[nodiscard]] Result<void> PrepareAgentRuntimeState(const AiSceneActivationBinding binding, const AiSceneAgentDescriptor &input,
                                                            const std::span<const AiControllerDescriptor> descriptors,
                                                            const AiCapabilitySet availableCapabilities, const std::uint32_t slotIndex,
                                                            std::optional<AgentRuntimeState> &prepared) {
            prepared.reset();
            const bool sceneStartup = input.agent.enabled && input.agent.startupPolicy == AiStartupPolicy::OnSceneActivation;
            const bool controllerStartup =
                input.controller && input.controller->enabled && input.controller->startupPolicy == AiStartupPolicy::OnSceneActivation;
            const AgentHandle handle{binding.incarnation, Horo::Handle<AgentHandleTag>{slotIndex, 1}};
            if (!sceneStartup || !controllerStartup) {
                prepared.emplace(AgentRuntimeState{.record = AiAgentRuntimeRecord{.handle = handle,
                                                                                  .owner = input.owner,
                                                                                  .agent = input.agent,
                                                                                  .controller = input.controller,
                                                                                  .state = AiAgentActivationState::Disabled}});
                return Result<void>::Success();
            }

            const AiControllerDescriptor *descriptor = FindDescriptor(descriptors, input.controller->controller);
            if (descriptor == nullptr)
                return Failure(AIErrors::ControllerDescriptorMissing,
                               "An enabled AI controller has no matching immutable activation descriptor.");
            if (const Result<void> valid = ValidateAiControllerBinding(*input.controller, *descriptor); valid.HasError())
                return Result<void>::Failure(valid.ErrorValue());
            if (!HasCapabilities(availableCapabilities, input.controller->requiredCapabilities))
                return Failure(AIErrors::CapabilityUnavailable, "An enabled AI controller requires an unavailable host capability.");

            const auto schema = descriptor->blackboardSchema;
            const BlackboardInstanceBinding blackboardBinding{.agent = handle,
                                                              .schema = schema->Identity(),
                                                              .schemaVersion = schema->Version(),
                                                              .schemaGeneration = 1,
                                                              .instanceGeneration = 1};
            auto blackboard = BlackboardInstance::Create(blackboardBinding, schema);
            if (blackboard.HasError())
                return Result<void>::Failure(blackboard.ErrorValue());

            prepared.emplace(AgentRuntimeState{
                .record = AiAgentRuntimeRecord{.handle = handle,
                                               .owner = input.owner,
                                               .agent = input.agent,
                                               .controller = input.controller,
                                               .stagedCapabilities = input.controller->requiredCapabilities,
                                               .state = AiAgentActivationState::Active,
                                               .hasBlackboard = true,
                                               .hasRunningTask = false},
                .blackboard = std::move(blackboard).Value(),
            });
            return Result<void>::Success();
        }

        [[nodiscard]] Result<std::unique_ptr<Detail::AiSceneRuntimeState>> BuildSceneState(
            const AiSceneActivationBinding binding, const std::span<const AiSceneAgentDescriptor> agents,
            const std::span<const AiControllerDescriptor> descriptors, const AiCapabilitySet availableCapabilities) {
            std::unique_ptr<Detail::AiSceneRuntimeState> state;
            try {
                state = std::make_unique<Detail::AiSceneRuntimeState>();
                state->binding = binding;
                state->agents.reserve(agents.size());
                state->agentsByOwner.reserve(agents.size());
                for (const AiSceneAgentDescriptor &input : agents) {
                    std::optional<AgentRuntimeState> prepared;
                    const std::size_t slotIndex = state->agents.size();
                    const Result<void> ready = PrepareAgentRuntimeState(binding, input, descriptors, availableCapabilities,
                                                                        static_cast<std::uint32_t>(slotIndex), prepared);
                    if (ready.HasError()) {
                        ShutdownStateContents(*state);
                        return Result<std::unique_ptr<Detail::AiSceneRuntimeState>>::Failure(ready.ErrorValue());
                    }
                    if (prepared) {
                        state->agents.push_back(std::move(*prepared));
                        state->agentsByOwner.emplace(input.owner, slotIndex);
                    }
                }
            } catch (const std::bad_alloc &) {
                if (state != nullptr)
                    ShutdownStateContents(*state);
                return Failure<
                    std::unique_ptr<Detail::AiSceneRuntimeState>>(AIErrors::AgentCapacityExceeded,
                                                                  "AI scene activation could not allocate bounded runtime storage.");
            }
            return Result<std::unique_ptr<Detail::AiSceneRuntimeState>>::Success(std::move(state));
        }
    }  // namespace

    /** @copydoc AiSceneSnapshot::Find */
    Result<AiAgentRuntimeRecord> AiSceneSnapshot::Find(const AgentHandle handle) const {
        if (const Result<void> valid = ValidateAiRuntimeHandle(handle, binding_.incarnation); valid.HasError())
            return Result<AiAgentRuntimeRecord>::Failure(valid.ErrorValue());
        if (handle.slot.index >= slotLookup_.size() || !slotLookup_[handle.slot.index])
            return Failure<AiAgentRuntimeRecord>(AIErrors::HandleInvalid, "The AI agent handle is not resident in this scene publication.");
        const AiAgentRuntimeRecord &found = agents_[*slotLookup_[handle.slot.index]];
        if (found.handle != handle)
            return Failure<AiAgentRuntimeRecord>(AIErrors::HandleInvalid, "The AI agent handle is not resident in this scene publication.");
        return Result<AiAgentRuntimeRecord>::Success(found);
    }

    AiSceneActivationCandidate::AiSceneActivationCandidate(AiSceneRuntime &runtime, const AiSceneActivationBinding binding,
                                                           std::unique_ptr<Detail::AiSceneRuntimeState> state,
                                                           const std::uint64_t publicationToken) noexcept
        : runtime_(&runtime), binding_(binding), state_(std::move(state)), publicationToken_(publicationToken) {}

    AiSceneActivationCandidate::~AiSceneActivationCandidate() {
        Shutdown();
    }

    /** @copydoc AiSceneActivationCandidate::ValidatePublication */
    Result<void> AiSceneActivationCandidate::ValidatePublication() const {
        if (published_)
            return Result<void>::Success();
        if (runtime_ == nullptr || state_ == nullptr)
            return Failure(AIErrors::SceneActivationInvalid, "The AI scene activation candidate has no owned staged state.");
        return runtime_->ValidateCandidate(binding_);
    }

    /** @copydoc AiSceneActivationCandidate::Publish */
    void AiSceneActivationCandidate::Publish() noexcept {
        if (published_ || runtime_ == nullptr || state_ == nullptr)
            return;
        published_ = runtime_->PublishCandidate(state_, binding_, publicationToken_);
    }

    /** @copydoc AiSceneActivationCandidate::Shutdown */
    void AiSceneActivationCandidate::Shutdown() noexcept {
        if (runtime_ == nullptr)
            return;
        if (state_ != nullptr) {
            runtime_->ShutdownState(*state_);
            state_.reset();
        }
        if (published_) {
            runtime_->RetirePublication(publicationToken_);
            published_ = false;
        }
    }

    /** @copydoc AiSceneRuntime::Create */
    Result<AiSceneRuntime> AiSceneRuntime::Create(const AiSceneRuntimeSettings settings) {
        if (!settings.IsValid())
            return Failure<AiSceneRuntime>(AIErrors::AgentCapacityExceeded, "AI scene runtime settings exceed a bounded product limit.");
        return Result<AiSceneRuntime>::Success(AiSceneRuntime{settings});
    }

    AiSceneRuntime::AiSceneRuntime(const AiSceneRuntimeSettings settings) noexcept : settings_(settings) {}

    AiSceneRuntime::AiSceneRuntime(AiSceneRuntime &&other) noexcept
        : settings_(other.settings_), active_(std::move(other.active_)), nextPublicationToken_(other.nextPublicationToken_),
          activePublicationToken_(other.activePublicationToken_), shutdown_(other.shutdown_) {
        other.activePublicationToken_ = 0;
        other.shutdown_ = true;
    }

    AiSceneRuntime::~AiSceneRuntime() {
        BeginShutdown();
    }

    /** @copydoc AiSceneRuntime::PrepareScene */
    Result<std::unique_ptr<AiSceneActivationCandidate>> AiSceneRuntime::PrepareScene(
        const AiSceneActivationBinding binding, const std::span<const AiSceneAgentDescriptor> agents,
        const std::span<const AiControllerDescriptor> descriptors) {
        if (shutdown_)
            return Failure<std::unique_ptr<AiSceneActivationCandidate>>(AIErrors::RuntimeUnavailable, "The AI scene runtime is shut down.");
        if (!binding.IsValid())
            return Failure<std::unique_ptr<AiSceneActivationCandidate>>(AIErrors::SceneActivationInvalid,
                                                                        "The AI scene activation binding is invalid.");
        if (agents.size() > settings_.maximumAgents)
            return Failure<std::unique_ptr<AiSceneActivationCandidate>>(AIErrors::AgentCapacityExceeded,
                                                                        "The AI scene exceeds its configured agent capacity.");

        if (const Result<void> valid = ValidateSceneInputs(agents, descriptors, binding); valid.HasError())
            return Result<std::unique_ptr<AiSceneActivationCandidate>>::Failure(valid.ErrorValue());

        auto state = BuildSceneState(binding, agents, descriptors, settings_.availableCapabilities);
        if (state.HasError())
            return Result<std::unique_ptr<AiSceneActivationCandidate>>::Failure(state.ErrorValue());
        std::unique_ptr<Detail::AiSceneRuntimeState> ownedState = std::move(state).Value();

        if (nextPublicationToken_ == std::numeric_limits<std::uint64_t>::max()) {
            ShutdownStateContents(*ownedState);
            return Failure<std::unique_ptr<AiSceneActivationCandidate>>(AIErrors::SceneActivationInvalid,
                                                                        "AI scene publication generation is exhausted.");
        }
        const std::uint64_t publicationToken = nextPublicationToken_++;
        try {
            std::unique_ptr<AiSceneActivationCandidate> candidate{
                new AiSceneActivationCandidate{*this, binding, std::move(ownedState), publicationToken}};  // NOSONAR(cpp:S5950)
            return Result<std::unique_ptr<AiSceneActivationCandidate>>::Success(std::move(candidate));
        } catch (const std::bad_alloc &) {
            if (ownedState != nullptr)
                ShutdownStateContents(*ownedState);
            return Failure<std::unique_ptr<AiSceneActivationCandidate>>(AIErrors::AgentCapacityExceeded,
                                                                        "AI scene activation candidate storage is unavailable.");
        }
    }

    /** @copydoc AiSceneRuntime::StartTaskAtSafePoint */
    Result<TaskHandle> AiSceneRuntime::StartTaskAtSafePoint(const AgentHandle agent, const TaskId taskDefinition) {
        if (shutdown_ || active_ == nullptr)
            return Failure<TaskHandle>(AIErrors::RuntimeUnavailable, "The AI scene runtime is not active.");
        if (!taskDefinition.IsValid())
            return Failure<TaskHandle>(AIErrors::TaskContextInvalid, "An AI task requires a valid persistent task identity.");
        if (const Result<void> valid = ValidateAiRuntimeHandle(agent, active_->binding.incarnation); valid.HasError())
            return Result<TaskHandle>::Failure(valid.ErrorValue());
        AgentRuntimeState *slot = FindAgent(*active_, agent);
        if (slot == nullptr || slot->record.state != AiAgentActivationState::Active)
            return Failure<TaskHandle>(AIErrors::HandleInvalid, "The AI agent handle is stale or disabled.");
        if (slot->task != nullptr)
            return Failure<TaskHandle>(AIErrors::TaskTransitionInvalid, "The AI agent already owns a running task.");
        if (active_->nextTaskSlot == Horo::Handle<TaskHandleTag>::InvalidIndex)
            return Failure<TaskHandle>(AIErrors::TaskCapacityExceeded, "The AI task handle slot range is exhausted.");

        const TaskHandle task{active_->binding.incarnation, {active_->nextTaskSlot++, 1}};
        auto lifecycle = std::make_unique<AiTaskLifecycle>();
        const Result<AiTaskTransitionDisposition> started =
            lifecycle->Start(AiTaskOperationContext{.taskDefinition = taskDefinition,
                                                    .task = task,
                                                    .agent = agent,
                                                    .cancellation = slot->cancellation.Token()});
        if (started.HasError())
            return Result<TaskHandle>::Failure(started.ErrorValue());
        slot->task = std::move(lifecycle);
        slot->record.hasRunningTask = true;
        return Result<TaskHandle>::Success(task);
    }

    /** @copydoc AiSceneRuntime::DisableAtSafePoint */
    Result<void> AiSceneRuntime::DisableAtSafePoint(const AgentHandle agent) {
        if (shutdown_ || active_ == nullptr)
            return Failure(AIErrors::RuntimeUnavailable, "The AI scene runtime is not active.");
        if (const Result<void> valid = ValidateAiRuntimeHandle(agent, active_->binding.incarnation); valid.HasError())
            return valid;
        AgentRuntimeState *slot = FindAgent(*active_, agent);
        if (slot == nullptr)
            return Failure(AIErrors::HandleInvalid, "The AI agent handle is stale or retired.");
        if (slot->record.state == AiAgentActivationState::Disabled)
            return Result<void>::Success();
        CancelOwnedWork(*slot);
        slot->record.state = AiAgentActivationState::Disabled;
        return Result<void>::Success();
    }

    /** @copydoc AiSceneRuntime::RetireOwnerAtSafePoint */
    Result<std::size_t> AiSceneRuntime::RetireOwnerAtSafePoint(const Runtime::EntityRef owner) {
        if (shutdown_ || active_ == nullptr)
            return Failure<std::size_t>(AIErrors::RuntimeUnavailable, "The AI scene runtime is not active.");
        if (!owner.IsValid() || owner.runtime != active_->binding.scene)
            return Failure<std::size_t>(AIErrors::HandleInvalid, "The AI entity owner is stale or belongs to another scene.");
        std::size_t retired = 0;
        const auto owners = active_->agentsByOwner.equal_range(owner);
        for (auto ownerIt = owners.first; ownerIt != owners.second; ++ownerIt) {
            AgentRuntimeState &slot = active_->agents[ownerIt->second];
            if (slot.retired || slot.record.owner != owner)
                continue;
            CancelOwnedWork(slot);
            slot.record.state = AiAgentActivationState::Retired;
            slot.retired = true;
            ++retired;
        }
        return Result<std::size_t>::Success(retired);
    }

    /** @copydoc AiSceneRuntime::Snapshot */
    Result<AiSceneSnapshot> AiSceneRuntime::Snapshot() const {
        if (shutdown_ || active_ == nullptr)
            return Failure<AiSceneSnapshot>(AIErrors::RuntimeUnavailable, "The AI scene runtime is not active.");
        AiSceneSnapshot snapshot;
        snapshot.binding_ = active_->binding;
        try {
            snapshot.slotLookup_.resize(active_->agents.size());
            snapshot.agents_.reserve(active_->agents.size());
            for (std::size_t slotIndex = 0; slotIndex < active_->agents.size(); ++slotIndex) {
                const AgentRuntimeState &slot = active_->agents[slotIndex];
                if (slot.retired)
                    continue;
                snapshot.slotLookup_[slotIndex] = snapshot.agents_.size();
                snapshot.agents_.push_back(slot.record);
            }
        } catch (const std::bad_alloc &) {
            return Failure<AiSceneSnapshot>(AIErrors::AgentCapacityExceeded, "AI scene snapshot storage exceeded its bounded capacity.");
        }
        return Result<AiSceneSnapshot>::Success(std::move(snapshot));
    }

    /** @copydoc AiSceneRuntime::Find */
    Result<AiAgentRuntimeRecord> AiSceneRuntime::Find(const AgentHandle agent) const {
        if (shutdown_ || active_ == nullptr)
            return Failure<AiAgentRuntimeRecord>(AIErrors::RuntimeUnavailable, "The AI scene runtime is not active.");
        if (const Result<void> valid = ValidateAiRuntimeHandle(agent, active_->binding.incarnation); valid.HasError())
            return Result<AiAgentRuntimeRecord>::Failure(valid.ErrorValue());
        const AgentRuntimeState *slot = FindAgent(*active_, agent);
        if (slot == nullptr)
            return Failure<AiAgentRuntimeRecord>(AIErrors::HandleInvalid, "The AI agent handle is stale or retired.");
        return Result<AiAgentRuntimeRecord>::Success(slot->record);
    }

    /** @copydoc AiSceneRuntime::ActiveBinding */
    Result<AiSceneActivationBinding> AiSceneRuntime::ActiveBinding() const {
        if (shutdown_ || active_ == nullptr)
            return Failure<AiSceneActivationBinding>(AIErrors::RuntimeUnavailable, "The AI scene runtime is not active.");
        return Result<AiSceneActivationBinding>::Success(active_->binding);
    }

    /** @copydoc AiSceneRuntime::BeginShutdown */
    void AiSceneRuntime::BeginShutdown() noexcept {
        if (shutdown_)
            return;
        shutdown_ = true;
        if (active_ != nullptr) {
            ShutdownStateContents(*active_);
            active_.reset();
        }
        activePublicationToken_ = 0;
    }

    Result<void> AiSceneRuntime::ValidateCandidate(const AiSceneActivationBinding &binding) const {
        if (shutdown_)
            return Failure(AIErrors::RuntimeUnavailable, "The AI scene runtime is shut down.");
        if (!binding.IsValid())
            return Failure(AIErrors::SceneActivationInvalid, "The AI scene activation binding is invalid.");
        return Result<void>::Success();
    }

    bool AiSceneRuntime::PublishCandidate(std::unique_ptr<Detail::AiSceneRuntimeState> &state, const AiSceneActivationBinding &binding,
                                          const std::uint64_t publicationToken) noexcept {
        if (shutdown_ || state == nullptr || state->binding != binding)
            return false;
        std::unique_ptr<Detail::AiSceneRuntimeState> previous = std::move(active_);
        active_ = std::move(state);
        activePublicationToken_ = publicationToken;
        if (previous != nullptr)
            ShutdownStateContents(*previous);
        return true;
    }

    void AiSceneRuntime::ShutdownState(Detail::AiSceneRuntimeState &state) noexcept {
        ShutdownStateContents(state);
    }

    void AiSceneRuntime::RetirePublication(const std::uint64_t publicationToken) noexcept {
        if (publicationToken == 0 || publicationToken != activePublicationToken_)
            return;
        std::unique_ptr<Detail::AiSceneRuntimeState> previous = std::move(active_);
        activePublicationToken_ = 0;
        if (previous != nullptr)
            ShutdownStateContents(*previous);
    }

    /** @copydoc AiSceneActivationParticipant::Prepare */
    Result<std::unique_ptr<Runtime::SceneActivationCandidate>> AiSceneActivationParticipant::Prepare(
        const Runtime::RuntimeSceneDefinition &definition, const Runtime::RuntimeSceneView scene) {
        if (runtime_ == nullptr)
            return Failure<std::unique_ptr<Runtime::SceneActivationCandidate>>(AIErrors::SceneActivationInvalid,
                                                                               "The AI scene participant has no runtime owner.");
        const auto binding = MakeBinding(scene);
        if (binding.HasError())
            return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(binding.ErrorValue());

        std::vector<AiSceneAgentDescriptor> agents;
        try {
            agents.reserve(definition.Entities().size());
            for (const Runtime::RuntimeEntityDefinition &entity : definition.Entities()) {
                if (!entity.components.aiAgent && !entity.components.aiController)
                    continue;
                const std::optional<Runtime::EntityRef> owner = scene.Find(entity.object);
                if (!owner)
                    return Failure<std::unique_ptr<
                        Runtime::SceneActivationCandidate>>(AIErrors::SceneActivationInvalid,
                                                            "An AI component references an entity absent from the runtime scene.");
                agents.push_back(AiSceneAgentDescriptor{.owner = *owner,
                                                        .agent = entity.components.aiAgent.value_or(AiAgentComponent{}),
                                                        .controller = entity.components.aiController});
            }
        } catch (const std::bad_alloc &) {
            return Failure<
                std::unique_ptr<Runtime::SceneActivationCandidate>>(AIErrors::AgentCapacityExceeded,
                                                                    "AI scene component projection exceeded its bounded storage.");
        }

        auto candidate = runtime_->PrepareScene(binding.Value(), agents, descriptors_);
        if (candidate.HasError())
            return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(candidate.ErrorValue());
        return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Success(std::move(candidate).Value());
    }
}  // namespace Horo::AI
