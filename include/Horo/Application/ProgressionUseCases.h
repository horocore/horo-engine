#pragma once

/** @file ProgressionUseCases.h
 * @brief Shared application admission for committed gameplay, GUI, CLI and server progression facts.
 */

#include "Horo/PlatformServices/AchievementDefinitionRegistry.h"
#include "Horo/PlatformServices/PlatformProgressionCoalescer.h"
#include "Horo/PlatformServices/PlatformProgressionReplay.h"
#include "Horo/PlatformServices/PlatformServicesFrontend.h"

#include <memory>
#include <vector>

namespace Horo::Application {
    /** @brief Committed semantic intent; callers cannot select authority, policy or mutation algebra. */
    struct ProgressionFact final {
        PlatformServices::PlatformProgressionOccurrenceId occurrence;
        std::variant<PlatformServices::AchievementId, PlatformServices::StatId, PlatformServices::LeaderboardId> definition;
        PlatformServices::PlatformProgressionValue value;
        std::optional<std::uint64_t> expectedRevision;
    };

    /**
     * @brief Host-owned authority port, bound to the actual local or server-world capability.
     * @details A client/listen-server client world must never receive the server implementation. An enum in a fact cannot mint
     * authority. Validation must recheck world/authority and current subject/session/access generations on every admission.
     */
    class IProgressionFactAuthority {
    public:
        virtual ~IProgressionFactAuthority() = default;
        /** @brief Authorizes a committed fact. @param candidate Complete registry-derived intent.
         * @return Success or original structured authority/session failure. Must not publish remote work. */
        [[nodiscard]] virtual Result<void> Authorize(const PlatformServices::PlatformProgressionMutationCandidate &candidate) const = 0;
    };

    /** @brief Composition-owned projection capability check; cannot discover/select/fall back to another provider. */
    class IProgressionProjectionGate {
    public:
        virtual ~IProgressionProjectionGate() = default;
        /** @brief Checks exact qualified atomic/dedupe primitives before receipt allocation.
         * @param envelope Immutable original intent. @return Success or normalized Null, unsupported, policy or lifecycle failure. */
        [[nodiscard]] virtual Result<void> Validate(const PlatformServices::PlatformProgressionMutationEnvelope &envelope) const = 0;
    };

    /**
     * @brief Adapts the active frontend's lifecycle/availability before checking qualified progression semantics.
     * @details Both borrowed objects must outlive this application adapter. The semantics gate must validate exact current
     * generations and qualified atomic/dedupe primitives; ordinary service availability alone never proves those primitives.
     * Calls use the frontend's composition lane. No backend discovery, selection or mutation submission occurs here.
     */
    class FrontendProgressionProjectionGate final : public IProgressionProjectionGate {
    public:
        /** @brief Binds existing composition-owned gates without invoking them.
         * @param frontend Active frontend whose availability/policy/lifecycle remains authoritative.
         * @param semantics Exact provider-qualified progression semantics and generation validator. */
        FrontendProgressionProjectionGate(const PlatformServices::PlatformServicesFrontend &frontend,
                                          const IProgressionProjectionGate &semantics) noexcept;
        /** @copydoc IProgressionProjectionGate::Validate */
        [[nodiscard]] Result<void> Validate(const PlatformServices::PlatformProgressionMutationEnvelope &envelope) const override;

    private:
        const PlatformServices::PlatformServicesFrontend &frontend_;
        const IProgressionProjectionGate &semantics_;
    };

    /** @brief Same-project immutable definition bundle; all snapshots are retained by the application. */
    struct ProgressionDefinitions final {
        std::shared_ptr<const PlatformServices::PlatformStableIdRegistry> identities;
        std::shared_ptr<const PlatformServices::AchievementDefinitionRegistry> achievements;
        std::shared_ptr<const PlatformServices::StatDefinitionRegistry> stats;
        std::shared_ptr<const PlatformServices::LeaderboardDefinitionRegistry> leaderboards;
    };

    /** @brief Finite semantic fact-identity retention for one captured application scope. */
    struct ProgressionUseCasesConfig final {
        std::size_t maximumAcceptedFacts{256}; /**< Terminal, cancelled and prepared-durable facts retain their original binding. */
    };

    /**
     * @brief Common provider-neutral use cases for every calling surface.
     * @details The host serializes calls on its application lane and owns the borrowed authority, gate and coalescer for this
     * object's lifetime. Only composition receives coalescer dispatch/completion access. Adapters receive this admission surface.
     * No SDK, provider selection, retries or durable scheduling exist here. Recreate after generation changes; the authority and
     * gate must reject revoked sessions even before recreation. Structured failures are returned unchanged. Compose one shared
     * instance per captured scope; its bounded fact ledger rejects occurrence/payload or delivery-owner changes. It is neither a
     * coalescer nor a durable journal. Views must borrow this instance rather than copy an independent semantic authority.
     */
    class ProgressionUseCases final {
    public:
        /** @brief Creates shared admission over one exact registry/session/policy generation.
         * @param definitions Retained same-project registries. @param scope Captured protected subject/session scope.
         * @param policy Captured product policy revision. @param authority Borrowed trusted authority validator.
         * @param gate Borrowed exact provider capability validator. @param coalescer Borrowed volatile logical owner.
         * @param config Finite semantic occurrence retention.
         * @return Use cases, InvalidConfiguration, or CapacityExceeded on allocation failure. */
        [[nodiscard]] static Result<ProgressionUseCases> Create(ProgressionDefinitions definitions,
                                                                PlatformServices::PlatformProgressionSessionScope scope,
                                                                PlatformServices::PlatformProgressionPolicyRevision policy,
                                                                const IProgressionFactAuthority &authority,
                                                                const IProgressionProjectionGate &gate,
                                                                PlatformServices::PlatformProgressionCoalescer &coalescer,
                                                                ProgressionUseCasesConfig config = {});

        ProgressionUseCases(const ProgressionUseCases &) = delete;
        ProgressionUseCases &operator=(const ProgressionUseCases &) = delete;
        /** @brief Transfers semantic fact ownership without allocation; the old view rejects admission after the move.
         * @param other Previous owner. */
        ProgressionUseCases(ProgressionUseCases &&other) noexcept;
        ProgressionUseCases &operator=(ProgressionUseCases &&) noexcept = delete;

        /** @brief Validates and accepts one committed typed fact. @param fact Owned Horo intent.
         * @param tick Monotonic application tick. @return Stable receipt or original structured validation/authority/gate failure. */
        [[nodiscard]] Result<PlatformServices::PlatformProgressionAdmission> Submit(const ProgressionFact &fact, std::uint64_t tick);
        /** @brief Prepares the exact qualified handoff for the sole durable owner without admitting volatile work.
         * @param fact Committed typed fact. @param requirements Real captured qualification, registry, mapping and policy provenance.
         * @param qualifier Trusted composition-owned issuer bound to actual effective provider semantics.
         * @return Qualified exact intent or original typed failure; success is not durable acceptance.
         * @details Composition must select one delivery owner before first admission and pass success directly to PLS-007.
         * A failed durable prepare/admission must never call Submit as a fallback. PLS-007 must revalidate using its own trusted
         * qualifier before dispatch; an object's C++ type alone cannot authenticate its issuer. */
        [[nodiscard]] Result<PlatformServices::PlatformProgressionQualifiedReplayIntent> PrepareDurableIntent(
            const ProgressionFact &fact, const PlatformServices::PlatformProgressionReplayRequirements &requirements,
            const PlatformServices::IPlatformProgressionReplayQualifier &qualifier);
        /** @brief Resolves a GUI/CLI/authored key through the same typed application admission.
         * @param kind Expected definition kind. @param key Canonical key or alias. @param fact Occurrence/value/precondition;
         * its definition is replaced by the resolved typed ID. @param tick Monotonic application tick.
         * @return Same receipt/error as Submit; no unchecked string reaches the provider. */
        [[nodiscard]] Result<PlatformServices::PlatformProgressionAdmission> SubmitByKey(PlatformServices::PlatformServiceIdKind kind,
                                                                                         std::string_view key, ProgressionFact fact,
                                                                                         std::uint64_t tick);
        /** @brief Cancels an undispatched receipt through its semantic owner. @param mutation Receipt identity.
         * @return Original cancellation result; dispatched effects are never reported rolled back. */
        [[nodiscard]] Result<void> Cancel(PlatformServices::PlatformProgressionMutationId mutation);
        /** @brief Observes a retained logical receipt through the shared application boundary.
         * @param mutation Receipt identity returned by this scope's admission.
         * @return Atomic outcome/category pair, or empty for unknown or other-subject/session receipts. */
        [[nodiscard]] std::optional<PlatformServices::PlatformProgressionReceiptSnapshot> Query(
            PlatformServices::PlatformProgressionMutationId mutation) const;

    private:
        struct PreparedIntent final {
            PlatformServices::PlatformProgressionMutationEnvelope envelope;
            PlatformServices::LeaderboardOrdering ordering;
        };
        enum class DeliveryOwner : std::uint8_t {
            Volatile,
            Durable
        };
        enum class FactDisposition : std::uint8_t {
            New,
            Existing
        };

        struct AcceptedFact final {
            PlatformServices::PlatformProgressionOccurrenceId occurrence;
            PreparedIntent intent;
            DeliveryOwner delivery;
            std::optional<PlatformServices::PlatformProgressionReplayRequirements> requirements;
        };

        ProgressionUseCases(ProgressionDefinitions definitions, PlatformServices::PlatformProgressionSessionScope scope,
                            PlatformServices::PlatformProgressionPolicyRevision policy, const IProgressionFactAuthority &authority,
                            const IProgressionProjectionGate &gate, PlatformServices::PlatformProgressionCoalescer &coalescer,
                            ProgressionUseCasesConfig config);
        /** @brief Derives the immutable registry contract. @param fact Committed typed input.
         * @param candidate Receives registry-owned identity, authority and algebra. @return Ordering or original definition error. */
        [[nodiscard]] Result<PlatformServices::LeaderboardOrdering> ApplyDefinition(
            const ProgressionFact &fact, PlatformServices::PlatformProgressionMutationCandidate &candidate) const;
        /** @brief Shares definition/authority/availability validation across the disjoint delivery paths.
         * @param fact Original committed fact. @return Exact envelope and ordering, or original structured failure. */
        [[nodiscard]] Result<PreparedIntent> PrepareFact(const ProgressionFact &fact) const;
        /** @brief Checks a retained occurrence/definition binding before either delivery owner can accept changed intent.
         * @param fact Committed occurrence. @param intent Validated canonical intent. @param delivery Selected single owner.
         * @param requirements Original durable provenance, absent for volatile admission.
         * @return New, Existing, conflict or CapacityExceeded. No state changes on failure. */
        [[nodiscard]] Result<FactDisposition> CheckFact(
            const ProgressionFact &fact, const PreparedIntent &intent, DeliveryOwner delivery,
            const std::optional<PlatformServices::PlatformProgressionReplayRequirements> &requirements) const;
        ProgressionDefinitions definitions_;
        PlatformServices::PlatformProgressionSessionScope scope_;
        PlatformServices::PlatformProgressionPolicyRevision policy_;
        const IProgressionFactAuthority &authority_;
        const IProgressionProjectionGate &gate_;
        PlatformServices::PlatformProgressionCoalescer &coalescer_;
        ProgressionUseCasesConfig config_;
        // Moving a checked MSVC vector can allocate its iterator proxy; transfer the entire ledger instead.
        std::unique_ptr<std::vector<AcceptedFact>> facts_;
        bool closed_{};
    };
}  // namespace Horo::Application
