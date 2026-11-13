#pragma once

/** @file PlatformProgressionReplay.h
 * @brief Exact-envelope handoff from the trusted progression qualification owner to durable delivery.
 */

#include "Horo/PlatformServices/PlatformProgressionIdempotency.h"

namespace Horo::PlatformServices {
    /** @brief Distinct qualified primitives; none is inferred from ordinary service availability. */
    enum class PlatformProgressionReplaySafety : std::uint8_t {
        IntrinsicIdempotence,
        AtomicComparison,
        ConditionalRevision,
        DurableMutationIdDeduplication
    };

    /**
     * @brief Captured immutable provenance required by one accepted logical mutation.
     * @details Fingerprints must come from their real owners. This inert value grants no authority, creates no provider
     * capability and cannot establish SDK conformance. The qualifier must compare it to its actual effective context.
     */
    struct PlatformProgressionReplayRequirements final {
        PlatformProviderId provider;
        PlatformProviderGeneration providerGeneration;
        PlatformProgressionPolicyRevision policy;
        Sha256Digest providerQualificationFingerprint{}; /**< Frozen provider/SDK/gateway conformance record. */
        Sha256Digest effectiveSemanticsFingerprint{};    /**< Actual qualified operation/value/ordering policy intersection. */
        Sha256Digest projectPolicyFingerprint{};         /**< PlatformProjectConfiguration::Fingerprint(). */
        Sha256Digest stableIdRegistryFingerprint{};      /**< PlatformStableIdRegistry::Fingerprint(). */
        Sha256Digest definitionRegistryFingerprint{};    /**< Owning achievement/stat/leaderboard definition fingerprint. */
        Sha256Digest mappingGenerationFingerprint{};     /**< PlatformProviderManifestCookOutput::generationFingerprint. */

        /** @brief Checks only completeness of inert provenance. @return True for nonzero identities and fingerprints. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] bool operator==(const PlatformProgressionReplayRequirements &) const noexcept = default;
    };

    class IPlatformProgressionReplayQualifier;

    /**
     * @brief Immutable exact envelope plus eligibility evidence issued by the injected trusted qualifier.
     * @details There is one envelope source; durable policy projections must not reconstruct it or allocate another mutation ID.
     * This is qualification evidence, not durable acceptance or permission to replay after a session change. Only the PLS-007
     * owner may persist/admit it, and dispatch must revalidate through the actual qualifier/session authority.
     */
    class PlatformProgressionQualifiedReplayIntent final {
    public:
        /** @brief Returns the exact original logical mutation. @return Borrowed immutable envelope. */
        [[nodiscard]] const PlatformProgressionMutationEnvelope &Envelope() const noexcept;
        /** @brief Returns captured effective policy and provenance. @return Borrowed immutable requirements. */
        [[nodiscard]] const PlatformProgressionReplayRequirements &Requirements() const noexcept;
        /** @brief Returns the qualified primitive, never a generic retryable flag. @return Replay safety class. */
        [[nodiscard]] PlatformProgressionReplaySafety Safety() const noexcept;
        /** @brief Returns the authored score ordering. @return HighestFirst for non-leaderboard intents. */
        [[nodiscard]] LeaderboardOrdering Ordering() const noexcept;

    private:
        friend class IPlatformProgressionReplayQualifier;
        PlatformProgressionQualifiedReplayIntent(PlatformProgressionMutationEnvelope envelope,
                                                 PlatformProgressionReplayRequirements requirements, PlatformProgressionReplaySafety safety,
                                                 LeaderboardOrdering ordering);
        PlatformProgressionMutationEnvelope envelope_;
        PlatformProgressionReplayRequirements requirements_;
        PlatformProgressionReplaySafety safety_;
        LeaderboardOrdering ordering_;
    };

    /**
     * @brief Trusted qualification-owner port; neither the application nor the queue implements a competing capability authority.
     * @details Composition must inject an implementation bound to actual effective provider qualification and current session
     * authority. The current generic backend snapshot/void projection gate does not implement this port. No default issuer is
     * supplied. Tests may inject a scripted issuer but do not thereby establish production-provider conformance.
     */
    class IPlatformProgressionReplayQualifier {
    public:
        virtual ~IPlatformProgressionReplayQualifier() = default;
        /** @brief Qualifies one already authorized fact against actual atomic/dedupe primitives and exact provenance.
         * @param envelope Original validated intent; success must retain every field unchanged.
         * @param requirements Exact captured provider, registry, mapping and policy requirements.
         * @param ordering Authored definition ordering.
         * @return Qualified intent or original structured unsupported, authority, session or qualification failure. */
        [[nodiscard]] virtual Result<PlatformProgressionQualifiedReplayIntent> Qualify(
            const PlatformProgressionMutationEnvelope &envelope, const PlatformProgressionReplayRequirements &requirements,
            LeaderboardOrdering ordering) const = 0;
        /** @brief Revalidates retained evidence before every durable dispatch without changing its original envelope.
         * @param intent Previously qualified exact intent.
         * @return Success only under real current effective semantics and PLS-006 binding/reauthorization evidence; otherwise failure.
         * @details Same-binding reauthorization must come from its real session owner. The queue cannot fabricate it, retarget
         * another subject or replace the old envelope's generations to make validation pass. */
        [[nodiscard]] virtual Result<void> Revalidate(const PlatformProgressionQualifiedReplayIntent &intent) const = 0;

    protected:
        /** @brief Seals already established qualification after structural and algebra-class checks.
         * @param envelope Exact authorized original intent. @param requirements Real captured provenance.
         * @param safety Primitive proven by this implementation's effective provider qualification.
         * @param ordering Immutable authored ordering.
         * @return Immutable handoff or InvalidMutation; this helper never proves provider support or authorizes a fact itself. */
        [[nodiscard]] static Result<PlatformProgressionQualifiedReplayIntent> SealQualifiedIntent(
            const PlatformProgressionMutationEnvelope &envelope, const PlatformProgressionReplayRequirements &requirements,
            PlatformProgressionReplaySafety safety, LeaderboardOrdering ordering);
    };
}  // namespace Horo::PlatformServices
