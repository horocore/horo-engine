#pragma once

/**
 * @file PlatformSessionBinding.h
 * @brief Protected binding evidence handoff and generation-fenced service reauthorization.
 */

#include "Horo/PlatformServices/PlatformServicesBackend.h"

#include <array>
#include <cstddef>
#include <memory>

namespace Horo::PlatformServices {
    /** @brief Private broker inputs; neither field may contain native identity, credentials or display-derived data. */
    struct ProviderAccountBindingCandidate final {
        PlatformProviderId provider;
        std::array<std::byte, 16> productEnvironmentScope{}; /**< Protected storage owner's stable product/environment namespace. */
        std::array<std::byte, 16> protectedPseudonym{};      /**< Broker-resolved random pseudonym for the authenticated account binding. */
    };

    class ProviderAccountBindingId;
    class PlatformSessionBindingEvidence;

    /**
     * @brief Private identity-broker issuance capability with no current-account or session state.
     * @details Composition retains this object in the authenticated mapping owner and never exposes it to ordinary consumers.
     * A separately created issuer cannot capture evidence for another owner's binding, even with equal numeric inputs.
     * Binding values/evidence carry weak provenance and retain no broker, credentials, provider or request resources.
     * Issuance, revocation and guarded service commits are serialized on the composition owner's engine lane; transported
     * evidence grants no independent cross-thread commit authority.
     */
    class PlatformSessionBindingIssuer final {
    public:
        /** @brief Creates one inert issuer lifetime. @return Issuer owned exclusively by the private broker.
         * @throws std::bad_alloc If its single lifetime marker cannot be allocated. */
        [[nodiscard]] static PlatformSessionBindingIssuer Create();
        PlatformSessionBindingIssuer(const PlatformSessionBindingIssuer &) = delete;
        PlatformSessionBindingIssuer &operator=(const PlatformSessionBindingIssuer &) = delete;
        /** @brief Transfers issuance authority and closes the moved-from value. @param other Original owner. */
        PlatformSessionBindingIssuer(PlatformSessionBindingIssuer &&other) noexcept = default;
        PlatformSessionBindingIssuer &operator=(PlatformSessionBindingIssuer &&) = delete;
        /** @brief Idempotently revokes this issuance lifetime without invoking native code. */
        void Close() noexcept;
        /** @brief Reports whether issuance remains open. @return True before revocation or transfer. */
        [[nodiscard]] bool IsOpen() const noexcept;

    private:
        friend Result<ProviderAccountBindingId> BuildProviderAccountBindingId(const PlatformSessionBindingIssuer &,
                                                                              const ProviderAccountBindingCandidate &);
        friend Result<PlatformSessionBindingEvidence> BuildPlatformSessionBindingEvidence(const PlatformSessionBindingIssuer &,
                                                                                          const ProviderAccountBindingId &,
                                                                                          const PlatformServiceCapabilitySnapshot &,
                                                                                          const PlatformSessionSnapshot &);

        struct Identity final {};

        /** @brief Stores one exclusively owned issuance marker. @param identity Newly allocated marker. */
        explicit PlatformSessionBindingIssuer(std::shared_ptr<const Identity> identity) noexcept;
        /** @brief Checks the exact weak issuing lifetime. @param identity Captured provenance. @return True for this live issuer. */
        [[nodiscard]] bool Owns(const std::weak_ptr<const void> &identity) const noexcept;
        std::shared_ptr<const Identity> identity_;
    };

    /** @brief Issuer-bound opaque view of a protected account binding; not a live subject or serializable/loggable value. */
    class ProviderAccountBindingId final {
    public:
        ProviderAccountBindingId() = default;
        /** @brief Checks representation and issuing lifetime without asserting authentication. @return True for live complete evidence. */
        [[nodiscard]] bool IsValid() const noexcept;
        /** @brief Returns the non-personal Horo provider identity. @return Selected provider, never an SDK account ID. */
        [[nodiscard]] PlatformProviderId Provider() const noexcept;
        /** @brief Compares protected values and their exact issuing lifetime. @param other Binding to compare. @return Exact equality. */
        [[nodiscard]] bool operator==(const ProviderAccountBindingId &other) const noexcept;

    private:
        friend Result<ProviderAccountBindingId> BuildProviderAccountBindingId(const PlatformSessionBindingIssuer &,
                                                                              const ProviderAccountBindingCandidate &);
        friend Result<PlatformSessionBindingEvidence> BuildPlatformSessionBindingEvidence(const PlatformSessionBindingIssuer &,
                                                                                          const ProviderAccountBindingId &,
                                                                                          const PlatformServiceCapabilitySnapshot &,
                                                                                          const PlatformSessionSnapshot &);
        PlatformProviderId provider_;
        std::array<std::byte, 16> scope_{};
        std::array<std::byte, 16> pseudonym_{};
        std::weak_ptr<const void> issuer_;
    };

    /** @brief Immutable captured association, containing no mutable current-account or session authority. */
    class PlatformSessionBindingEvidence final {
    public:
        PlatformSessionBindingEvidence() = default;
        /** @brief Checks representation only. @return True for a bound captured subject and access revision. */
        [[nodiscard]] bool IsValid() const noexcept;
        /** @brief Returns the protected opaque binding. @return Equality-only identity for an explicit expected-binding comparison. */
        [[nodiscard]] const ProviderAccountBindingId &Binding() const noexcept;
        /** @brief Returns the captured live subject. @return Session-scoped capability that must be revalidated before use. */
        [[nodiscard]] PlatformSubjectHandle Subject() const noexcept;
        /** @brief Returns the captured access revision. @return Nonzero revision for valid evidence. */
        [[nodiscard]] PlatformAccessPolicyRevision AccessRevision() const noexcept;

    private:
        friend Result<PlatformSessionBindingEvidence> BuildPlatformSessionBindingEvidence(const PlatformSessionBindingIssuer &,
                                                                                          const ProviderAccountBindingId &,
                                                                                          const PlatformServiceCapabilitySnapshot &,
                                                                                          const PlatformSessionSnapshot &);
        ProviderAccountBindingId binding_;
        PlatformSubjectHandle subject_;
        PlatformAccessPolicyRevision accessRevision_;
    };

    /** @brief Failures at the protected binding-to-session handoff boundary. */
    namespace PlatformIdentityBindingErrors {
        /** @brief Broker evidence or composition is malformed. */
        extern const ErrorCodeDescriptor BindingFailed;
        /** @brief The current verified binding does not match the durable intent's expected protected partition. */
        extern const ErrorCodeDescriptor BindingMismatch;
        /** @brief A private broker cannot resolve a stable authenticated binding; no inferred fallback is permitted. */
        extern const ErrorCodeDescriptor MappingUnavailable;
    }  // namespace PlatformIdentityBindingErrors

    /**
     * @brief Builds an opaque identity from a trusted private mapping-store resolution.
     * @param issuer Private authenticated broker's issuance capability; never supplied by an ordinary consumer.
     * @param candidate Exact product/environment/provider-scoped pseudonym resolved by the identity broker.
     * @return Opaque binding, BindingFailed for incomplete evidence, or MappingUnavailable after issuer revocation.
     * @details This inert builder does not authenticate or look up an account. Only the private authenticated broker supplies
     * these inputs. The protected store retains its own canonical inputs; ordinary consumers receive no byte accessor.
     */
    [[nodiscard]] Result<ProviderAccountBindingId> BuildProviderAccountBindingId(const PlatformSessionBindingIssuer &issuer,
                                                                                 const ProviderAccountBindingCandidate &candidate);

    /**
     * @brief Captures a broker-verified binding against one already published Active session.
     * @param issuer Same private issuer that resolved binding; foreign or revoked issuers fail closed.
     * @param binding Exact binding resolved from native authentication by the private identity broker.
     * @param provider Complete selected provider capability snapshot.
     * @param session Existing published immutable session authority.
     * @return Captured evidence or typed composition/inactive/mismatch failure.
     * @details The caller must establish the native account association before this handoff. This function validates Horo
     * scope/generation consistency, never infers identity from the current account and grants no service by itself.
     */
    [[nodiscard]] Result<PlatformSessionBindingEvidence> BuildPlatformSessionBindingEvidence(
        const PlatformSessionBindingIssuer &issuer, const ProviderAccountBindingId &binding,
        const PlatformServiceCapabilitySnapshot &provider, const PlatformSessionSnapshot &session);

    /**
     * @brief Revalidates same-binding replay or commit against current provider/session/access authority.
     * @param provider Current policy-projected provider capabilities.
     * @param session Current published session snapshot; no independent session state is retained here.
     * @param evidence Fresh evidence from the broker's verified binding resolution.
     * @param expectedBinding Protected binding explicitly associated with the old durable intent by its owner.
     * @param service Exact service requiring current Granted access and provider availability.
     * @return Success only for the expected protected binding and exact live generations, otherwise a typed failure.
     * @details Old evidence remains stale after sign-out/switch/reload. Same-binding reauthentication requires new evidence
     * for the new Active subject. The semantic owner must still reauthorize profile, policy, intent and replay eligibility.
     * Live evidence/handles are never persisted in queue/save records; durable partition ownership remains with that owner.
     */
    [[nodiscard]] Result<void> ValidatePlatformSessionBindingAccess(const PlatformServiceCapabilitySnapshot &provider,
                                                                    const PlatformSessionSnapshot &session,
                                                                    const PlatformSessionBindingEvidence &evidence,
                                                                    const ProviderAccountBindingId &expectedBinding,
                                                                    PlatformServiceKind service);
}  // namespace Horo::PlatformServices
