#include "GameplayVfxContextInternal.h"

namespace Horo::Gameplay::VfxContextDetail {
    Vfx::VfxSpawnStatus Status(const Detail::GameplayVfxSceneState &state) noexcept {
        if (state.owner != std::this_thread::get_id())
            return Vfx::VfxSpawnStatus::ThreadViolation;
        if (!state.active || !state.world)
            return Vfx::VfxSpawnStatus::StaleScene;
        if (state.busy)
            return Vfx::VfxSpawnStatus::Reentrant;
        Vfx::VfxSpawnAssetInfo witness;
        return state.world->InspectSpawnAsset(state.witness, witness);
    }

    Vfx::VfxSpawnStatus ClientStatus(const Detail::GameplayVfxClientState &client,
                                     std::shared_ptr<Detail::GameplayVfxSceneState> &scene) noexcept {
        if (client.module->owner != std::this_thread::get_id())
            return Vfx::VfxSpawnStatus::ThreadViolation;
        if (client.retired || ScopeRevoked(client.lifetime) || !client.module->active)
            return Vfx::VfxSpawnStatus::ShutDown;
        scene = client.scene.lock();
        return scene ? Status(*scene) : Vfx::VfxSpawnStatus::StaleScene;
    }

    Vfx::VfxSpawnStatus ProducerStatus(const Detail::GameplayVfxProducerState &producer,
                                       std::shared_ptr<Detail::GameplayVfxSceneState> &scene) noexcept {
        if (producer.module->owner != std::this_thread::get_id())
            return Vfx::VfxSpawnStatus::ThreadViolation;
        if (!producer.module->active)
            return Vfx::VfxSpawnStatus::ShutDown;
        scene = producer.scene.lock();
        return scene ? Status(*scene) : Vfx::VfxSpawnStatus::StaleScene;
    }

    bool Allowed(const Detail::GameplayVfxModuleState &module, const Detail::GameplayVfxCompiledBinding &binding) noexcept {
        if (binding.event.IsValid())
            return std::ranges::find(module.grant.events, binding.event) != module.grant.events.end();
        return std::ranges::find(module.grant.tags, binding.tag) != module.grant.tags.end();
    }

    Vfx::VfxSpawnRequest Basis(const Detail::GameplayVfxSceneState &scene, const GameplayVfxReceipt &receipt) noexcept {
        Vfx::VfxSpawnRequest request;
        request.scene = scene.scene;
        request.owner = receipt.owner;
        request.ownerGeneration = receipt.ownerGeneration;
        request.occurrence = receipt.occurrence;
        request.tick = scene.tick;
        return request;
    }

    bool SameReceipt(const GameplayVfxReceipt &left, const GameplayVfxReceipt &right) noexcept {
        return left.scene == right.scene && left.owner == right.owner && left.ownerGeneration == right.ownerGeneration &&
               left.occurrence == right.occurrence;
    }

    GameplayVfxResult Poll(const Detail::GameplayVfxSceneState &scene, const Detail::GameplayVfxModuleState &module,
                           const GameplayVfxReceipt &receipt) noexcept {
        if (receipt.scene != scene.scene || receipt.owner != module.grant.owner ||
            receipt.ownerGeneration != module.grant.ownerGeneration || receipt.occurrence.producer != module.grant.producer)
            return {.status = Vfx::VfxSpawnStatus::PermissionDenied};
        for (const auto &pending : std::span{scene.pending.data(), scene.pendingCount})
            if (SameReceipt(pending.receipt, receipt))
                return {.status = Vfx::VfxSpawnStatus::Accepted, .pending = true};
        for (const auto &result : std::span{scene.results.data(), scene.resultCount})
            if (SameReceipt(result.receipt, receipt))
                return result.result;
        return {.status = Vfx::VfxSpawnStatus::Empty};
    }
}  // namespace Horo::Gameplay::VfxContextDetail

namespace Horo::Gameplay {
    namespace {
        [[nodiscard]] bool AllowsEvent(const Detail::GameplayVfxModuleState &module, const std::string_view event) noexcept {
            return std::ranges::any_of(module.grant.events, [event](const auto &allowed) {
                return allowed.Value() == event;
            });
        }
    }  // namespace

    GameplayVfxContext::GameplayVfxContext(std::shared_ptr<Detail::GameplayVfxClientState> state) : state_(std::move(state)) {}

    /** @copydoc GameplayVfxContext::~GameplayVfxContext */
    GameplayVfxContext::~GameplayVfxContext() = default;

    /** @copydoc GameplayVfxContext::ResolveEvent */
    GameplayVfxLookup GameplayVfxContext::ResolveEvent(const std::string_view event, const std::uint32_t schemaVersion) const noexcept {
        std::shared_ptr<Detail::GameplayVfxSceneState> scene;
        if (const auto status = VfxContextDetail::ClientStatus(*state_, scene); status != Vfx::VfxSpawnStatus::Accepted)
            return {.status = status};
        if (!AllowsEvent(*state_->module, event))
            return {.status = Vfx::VfxSpawnStatus::PermissionDenied};
        return scene->table->FindEvent(event, schemaVersion);
    }

    /** @copydoc GameplayVfxContext::ResolveTag */
    GameplayVfxLookup GameplayVfxContext::ResolveTag(const GameplayVfxEffectTag tag, const std::uint32_t schemaVersion) const noexcept {
        std::shared_ptr<Detail::GameplayVfxSceneState> scene;
        if (const auto status = VfxContextDetail::ClientStatus(*state_, scene); status != Vfx::VfxSpawnStatus::Accepted)
            return {.status = status};
        if (std::ranges::find(state_->module->grant.tags, tag) == state_->module->grant.tags.end())
            return {.status = Vfx::VfxSpawnStatus::PermissionDenied};
        return scene->table->FindTag(tag, schemaVersion);
    }

    /** @copydoc GameplayVfxContext::Emit */
    GameplayVfxSubmission GameplayVfxContext::Emit(const GameplayVfxEventKey key, const GameplayVfxEventPayload &payload) const noexcept {
        std::shared_ptr<Detail::GameplayVfxSceneState> scene;
        if (const auto status = VfxContextDetail::ClientStatus(*state_, scene); status != Vfx::VfxSpawnStatus::Accepted)
            return {.status = status};
        if (!state_->callback || !state_->entity.IsValid() || !scene->sourceOpen)
            return {.status = Vfx::VfxSpawnStatus::PermissionDenied};
        return VfxContextDetail::Stage(*scene, *state_->module, key, payload, state_->lifetime);
    }

    /** @copydoc GameplayVfxContext::Poll */
    GameplayVfxResult GameplayVfxContext::Poll(const GameplayVfxReceipt &receipt) const noexcept {
        std::shared_ptr<Detail::GameplayVfxSceneState> scene;
        if (const auto status = VfxContextDetail::ClientStatus(*state_, scene); status != Vfx::VfxSpawnStatus::Accepted)
            return {.status = status};
        return VfxContextDetail::Poll(*scene, *state_->module, receipt);
    }

    /** @copydoc GameplayVfxContext::IsUsable */
    bool GameplayVfxContext::IsUsable() const noexcept {
        std::shared_ptr<Detail::GameplayVfxSceneState> scene;
        return VfxContextDetail::ClientStatus(*state_, scene) == Vfx::VfxSpawnStatus::Accepted;
    }

    /** @copydoc GameplayVfxContext::Scene */
    Vfx::VfxIdentityScope GameplayVfxContext::Scene() const noexcept {
        return state_->module->scene;
    }

    /** @copydoc GameplayVfxContext::ModuleId */
    std::string_view GameplayVfxContext::ModuleId() const noexcept {
        return state_->module->grant.moduleId;
    }

    GameplayVfxCommittedProducer::GameplayVfxCommittedProducer(std::shared_ptr<Detail::GameplayVfxProducerState> state)
        : state_(std::move(state)) {}

    /** @copydoc GameplayVfxCommittedProducer::~GameplayVfxCommittedProducer */
    GameplayVfxCommittedProducer::~GameplayVfxCommittedProducer() = default;

    /** @copydoc GameplayVfxCommittedProducer::ResolveEvent */
    GameplayVfxLookup GameplayVfxCommittedProducer::ResolveEvent(const std::string_view event,
                                                                 const std::uint32_t schemaVersion) const noexcept {
        std::shared_ptr<Detail::GameplayVfxSceneState> scene;
        if (const auto status = VfxContextDetail::ProducerStatus(*state_, scene); status != Vfx::VfxSpawnStatus::Accepted)
            return {.status = status};
        if (!AllowsEvent(*state_->module, event))
            return {.status = Vfx::VfxSpawnStatus::PermissionDenied};
        return scene->table->FindEvent(event, schemaVersion);
    }

    /** @copydoc GameplayVfxCommittedProducer::Emit */
    GameplayVfxSubmission GameplayVfxCommittedProducer::Emit(const GameplayVfxEventKey key, const GameplayVfxEventPayload &payload,
                                                             const std::uint64_t committedTick) const noexcept {
        std::shared_ptr<Detail::GameplayVfxSceneState> scene;
        if (const auto status = VfxContextDetail::ProducerStatus(*state_, scene); status != Vfx::VfxSpawnStatus::Accepted)
            return {.status = status};
        return VfxContextDetail::EmitCommitted(*scene, *state_->module, key, payload, committedTick);
    }

    /** @copydoc GameplayVfxCommittedProducer::IsUsable */
    bool GameplayVfxCommittedProducer::IsUsable() const noexcept {
        std::shared_ptr<Detail::GameplayVfxSceneState> scene;
        return VfxContextDetail::ProducerStatus(*state_, scene) == Vfx::VfxSpawnStatus::Accepted;
    }
}  // namespace Horo::Gameplay
