#pragma once
/** @file EditorPlaySavePrefabServices.h
 * @brief Existing host cooked provider with genuinely Scene-owned revocable spawn scopes and one Save inventory.
 */
#include "EditorPlaySavePrefabInventory.h"
#include "Horo/Gameplay/BehaviorRegistry.h"

namespace Horo::Application::Internal {
    /** @brief Host composition owner; each active/pending Scene owns its own service slot through an actual activation candidate. */
    class PlaySavePrefabServices final : public std::enable_shared_from_this<PlaySavePrefabServices> {
        struct State;

    public:
        /** @brief Binds the existing host provider/Scene authority and canonical component codec; starts no work. */
        PlaySavePrefabServices(Prefab::PrefabTemplateProvider &provider, Runtime::RuntimeSceneService &scenes,
                               const Prefab::PrefabComponentProjector &projector);
        ~PlaySavePrefabServices();
        /** @brief Returns the Scene activation participant owning exact candidate/retired spawn services. */
        [[nodiscard]] std::unique_ptr<Runtime::SceneActivationParticipant> MakeParticipant();
        /** @brief Binds real frozen Gameplay metadata after ordinary initial publication, preserving one current Scene scope. */
        [[nodiscard]] Result<std::shared_ptr<Gameplay::GameplayPrefabContext>> Bind(const Gameplay::BehaviorRegistry &behaviors,
                                                                                    const Gameplay::ComponentRegistry &components,
                                                                                    std::uint64_t tick);
        /** @brief Installs the single entropy-issued Save inventory before any structural prefab queue (pending loads are allowed). */
        [[nodiscard]] Result<void> InstallPersistence(Runtime::SaveWorldId world, std::vector<Runtime::PersistentEntityRecord> authored,
                                                      std::shared_ptr<Security::SecureRandomSource> entropy);
        /** @brief Creates genuine candidate services after exact dynamic reconstruction and before Gameplay restore prehooks. */
        [[nodiscard]] Result<std::shared_ptr<const Gameplay::GameplayPrefabContext>> PrepareRestored(
            Runtime::RuntimeScene &scene, const Prefab::PrefabPersistentState &saved, const Prefab::PrefabRestoreEntities &restored,
            std::uint64_t tick);
        /** @brief Revokes an exact unpublished slot if later preparation fails before Scene participant creation. */
        void CancelPrepared(Runtime::SceneRuntimeId scene) noexcept;
        /** @brief Reads the actual current host fixed-step cursor used for new candidate scopes. */
        [[nodiscard]] std::uint64_t Tick() const noexcept;
        /** @brief Returns only the current Scene's one published Save inventory. */
        [[nodiscard]] std::shared_ptr<PlaySavePrefabInventory> Inventory() const noexcept;
        /** @brief Borrows the same provider used by ordinary Spawn; the host pins it through all accepted settlement. */
        [[nodiscard]] Prefab::PrefabTemplateProvider &Provider() const noexcept;
        /** @brief Creates the adapter only from actual fully settled same-provider leases. */
        [[nodiscard]] Result<std::shared_ptr<Prefab::PrefabRestoreEntities>> PrepareEntities(
            Prefab::PrefabPersistentState saved, std::vector<Prefab::PrefabTemplateLease> leases,
            const CancellationToken &cancellation) const;
        /** @brief Advances the actual current Scene's ordinary spawn service at its existing safe point. */
        [[nodiscard]] Result<void> Advance(std::uint64_t tick);
        /** @brief Applies existing lifecycle/fixed-step calls to the genuinely current Scene slot only. */
        [[nodiscard]] Result<void> OnPhase(Runtime::RuntimePhase phase, const Runtime::FrameContext &frame);
        [[nodiscard]] Result<void> OnFixedUpdate(const Runtime::FixedStepContext &frame);
        /** @brief Revokes current module clients after Save/module settlement, retaining the same Scene's durable inventory. */
        void Quiesce() noexcept;

    private:
        std::shared_ptr<State> state_;
    };
}  // namespace Horo::Application::Internal
