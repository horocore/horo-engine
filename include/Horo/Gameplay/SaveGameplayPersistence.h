#pragma once
#include "Horo/Gameplay/BehaviorRestore.h"
#include "Horo/Gameplay/PersistenceInstallation.h"

#include <functional>
#include <optional>

/**
 * @file SaveGameplayPersistence.h
 * @brief Explicit durable gameplay state binding for canonical save capture and staged restore.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Gameplay/PersistenceRegistration.h"
#include "Horo/Runtime/Save/SaveCaptureSnapshot.h"
#include "Horo/Runtime/Save/SaveRestoreTransaction.h"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Horo::Gameplay::Detail {
    class PreparedGameplaySourceBinding;
}

namespace Horo::Runtime {
    /** @brief Exact SDK-facing runtime source used by the save integration adapter. */
    using IGameplayPersistenceSource = Gameplay::IPersistenceSource;
    /** @brief Exact SDK-facing no-fail publication candidate used by staged restore. */
    using IPreparedGameplayPersistenceState = Gameplay::IPreparedPersistenceState;

    /** @brief Explicit host contract for published native preparation retirement. */
    enum class GameplayPersistenceRestorePolicy : std::uint8_t {
        PreservePreparedOwnership,
        ReloadableScene
    };

    /**
     * @brief Capture binding and restore factory pinned to one live gameplay module generation.
     *
     * The caller registers this adapter through SaveParticipationClient and must pass a lease
     * that prevents dynamic-library unload until all registry snapshots and restore receipts retire.
     */
    class GameplayPersistenceAdapter final : public ICanonicalStateAdapter {
        /** @brief Factory-only key preventing construction without descriptor validation. */
        class ValidatedConstruction final {
            friend class GameplayPersistenceAdapter;
            ValidatedConstruction() = default;

        public:
            ValidatedConstruction(const ValidatedConstruction &) = default;
        };

    public:
        /** @brief Constructs validated state; the private key is available only to Create.
         * @param key Factory validation token.
         * @param descriptor Validated durable declaration.
         * @param source Runtime state owner.
         * @param moduleLease Exact-generation unload barrier, retired after source destruction.
         */
        GameplayPersistenceAdapter(ValidatedConstruction key, GameplayPersistenceDescriptor descriptor,
                                   std::shared_ptr<IGameplayPersistenceSource> source, std::shared_ptr<void> moduleLease) noexcept;
        GameplayPersistenceAdapter(const GameplayPersistenceAdapter &) = delete;
        GameplayPersistenceAdapter &operator=(const GameplayPersistenceAdapter &) = delete;
        /** @brief Validates the inert declaration and takes exact-generation source ownership.
         * @param descriptor Stable participant, record, module and exact-version metadata.
         * @param source Runtime-only state owner; never an authoring serializer.
         * @param moduleLease Non-null exact-generation unload barrier.
         * @return Adapter or typed descriptor/lifetime failure.
         */
        [[nodiscard]] static Result<std::shared_ptr<GameplayPersistenceAdapter>> Create(GameplayPersistenceDescriptor descriptor,
                                                                                        std::shared_ptr<IGameplayPersistenceSource> source,
                                                                                        std::shared_ptr<void> moduleLease);

        /** @copydoc ICanonicalStateAdapter::Capture */
        [[nodiscard]] Result<CanonicalCaptureDisposition> Capture(const CanonicalCaptureContext &context,
                                                                  ICanonicalCaptureSink &sink) const override;

        /** @brief Creates an operation-owned staged restore receipt from an archive-owned record.
         * @param savedSchema Schema declared by the validated archive manifest.
         * @param savedRecord Stable record identity declared by the validated archive.
         * @param bytes Borrowed canonical record bytes, copied before returning.
         * @return Receipt for StagedRestoreTransaction or a typed compatibility/bounds failure.
         */
        [[nodiscard]] Result<std::unique_ptr<IStagedRestoreParticipant>> StageRestore(ParticipantSchemaVersion savedSchema,
                                                                                      SaveRecordId savedRecord,
                                                                                      std::span<const std::byte> bytes) const;
        /** @brief Stages under explicit reloadable Scene policy; actual native retirement capability is required before publication. */
        [[nodiscard]] Result<std::unique_ptr<IStagedRestoreParticipant>> StageRestore(ParticipantSchemaVersion savedSchema,
                                                                                      SaveRecordId savedRecord,
                                                                                      std::span<const std::byte> bytes,
                                                                                      GameplayPersistenceRestorePolicy policy) const;

        /** @brief Binds through this privately installed adapter's existing native owner/admission/generation authority.
         * @param target Actual scoped candidate (or actual published target for ordinary capture binding).
         * @return Exact installed adapter or typed foreign-thread/revoked/missing capability failure; no worker dispatch.
         */
        [[nodiscard]] Result<GameplayPersistenceInstallation> BindRestoreCandidate(Gameplay::BehaviorPersistenceTarget target) const;

        /** @brief Returns inert metadata for explicit host registration. */
        [[nodiscard]] const GameplayPersistenceDescriptor &Descriptor() const noexcept;

    private:
        friend class Gameplay::LoadedGameModule;
        [[nodiscard]] static Result<std::shared_ptr<GameplayPersistenceAdapter>> CreateBound(
            GameplayPersistenceDescriptor, std::shared_ptr<IGameplayPersistenceSource>, std::shared_ptr<void>,
            Gameplay::BehaviorPersistenceTarget, std::shared_ptr<Gameplay::Detail::PreparedGameplaySourceBinding>);
        std::function<Result<GameplayPersistenceInstallation>(Gameplay::BehaviorPersistenceTarget)> candidateBinder_;
        std::optional<Gameplay::BehaviorPersistenceTarget> behaviorTarget_;
        std::shared_ptr<Gameplay::Detail::PreparedGameplaySourceBinding> bindingPublication_;
        GameplayPersistenceDescriptor descriptor_;
        std::shared_ptr<void> moduleLease_;
        std::shared_ptr<IGameplayPersistenceSource> source_;
    };

}  // namespace Horo::Runtime
