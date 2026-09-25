#pragma once

/**
 * @file PlatformAchievementCoordinator.h
 * @brief Registry-backed achievement mutation idempotency and state-query boundary.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/PlatformServices/AchievementDefinitionRegistry.h"
#include "Horo/PlatformServices/PlatformUserSession.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <vector>

namespace Horo::PlatformServices {
    /** @brief Hard upper bound for one coordinator's retained mutation identities. */
    inline constexpr std::size_t MaximumPlatformAchievementMutationLedger = 4'096;
    /** @brief Hard upper bound for queued achievement publications. */
    inline constexpr std::size_t MaximumPlatformAchievementPendingMutations = 1'024;

    /** @brief Typed non-secret 128-bit identity for one logical achievement occurrence. */
    struct PlatformAchievementMutationId final {
        std::array<std::byte, 16> bytes{};

        /** @brief Rejects the reserved all-zero identity. @return True when any byte is nonzero. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const PlatformAchievementMutationId &) const noexcept = default;
    };

    /** @brief One typed achievement mutation captured against session and authority evidence. */
    struct PlatformAchievementMutationRequest final {
        PlatformSubjectHandle subject;
        PlatformAccessPolicyRevision accessRevision;
        AchievementId achievement;
        ProgressionAuthorityMode authority{ProgressionAuthorityMode::LocalProduct};
        AchievementProgressKind kind{AchievementProgressKind::UnlockOnce};
        std::uint32_t progress{1};
        PlatformAchievementMutationId mutation;
    };

    /** @brief One typed state query captured against the current subject/access policy. */
    struct PlatformAchievementStateQueryRequest final {
        PlatformSubjectHandle subject;
        PlatformAccessPolicyRevision accessRevision;
        AchievementId achievement;
    };

    /** @brief Provider-neutral achievement state returned at one captured generation. */
    struct PlatformAchievementStateSnapshot final {
        PlatformSubjectHandle subject; /**< Opaque subject captured by the provider result. */
        AchievementId achievement;
        PlatformProviderGeneration providerGeneration;
        PlatformSessionGeneration sessionGeneration;
        PlatformAccessPolicyRevision accessRevision;
        std::uint64_t providerRevision{};
        std::uint32_t progress{};
        bool unlocked{};
    };

    /** @brief Provider-neutral mutation token handed to the private provider adapter. */
    struct PlatformAchievementMutationPublication final {
        PlatformAchievementMutationRequest request;
        PlatformSessionGeneration sessionGeneration;
        std::uint64_t sequence{};
    };

    /** @brief Provider-neutral query token handed to the private provider adapter. */
    struct PlatformAchievementQueryIntent final {
        PlatformAchievementStateQueryRequest request;
        PlatformProviderGeneration providerGeneration;
        PlatformSessionGeneration sessionGeneration;
        std::uint64_t sequence{};
    };

    /** @brief Result of admitting one mutation into the bounded idempotency ledger. */
    enum class PlatformAchievementMutationAdmission : std::uint8_t {
        Queued,
        IgnoredDuplicate
    };

    /** @brief Normalized provider outcome for one admitted mutation. */
    enum class PlatformAchievementPublicationOutcome : std::uint8_t {
        Succeeded,
        Failed,
        Cancelled
    };

    /** @brief Stable errors owned by the achievement semantic coordinator. */
    namespace AchievementCoordinatorErrors {
        /** @brief Coordinator configuration or registry ownership is invalid. */
        extern const ErrorCodeDescriptor InvalidConfiguration;
        /** @brief Mutation/query identity or value evidence is malformed. */
        extern const ErrorCodeDescriptor InvalidRequest;
        /** @brief No active immutable definition exists for the requested achievement. */
        extern const ErrorCodeDescriptor UnknownAchievement;
        /** @brief The request authority does not match the registered definition. */
        extern const ErrorCodeDescriptor AuthorityDenied;
        /** @brief The mutation kind or progress value contradicts the registered schema. */
        extern const ErrorCodeDescriptor InvalidProgress;
        /** @brief A mutation ID was reused with a different logical envelope. */
        extern const ErrorCodeDescriptor IdempotencyConflict;
        /** @brief The bounded mutation ledger or publication queue is full. */
        extern const ErrorCodeDescriptor CapacityExceeded;
        /** @brief The coordinator has closed admission. */
        extern const ErrorCodeDescriptor Closed;
        /** @brief A completion token no longer belongs to the current session. */
        extern const ErrorCodeDescriptor StalePublication;
        /** @brief A provider state query result is malformed or violates its captured request. */
        extern const ErrorCodeDescriptor InvalidState;
        /** @brief A provider state result belongs to an older session/access generation. */
        extern const ErrorCodeDescriptor StaleState;
    }  // namespace AchievementCoordinatorErrors

    /** @brief Finite semantic coordinator limits. */
    struct PlatformAchievementCoordinatorConfig final {
        std::size_t maximumPendingMutations{64};
        std::size_t maximumLedgerEntries{256};
    };

    /**
     * @brief Owns registry validation, authority/progress checks and bounded achievement idempotency.
     * @details The coordinator never calls a provider and never stores provider/native identity. Exact duplicate mutation IDs
     * are ignored, conflicting reuse is rejected, and session replacement clears old pending/ledger state. Query results are
     * validated against the exact captured subject, provider/session/access generations and registered progress schema.
     */
    class PlatformAchievementCoordinator final {
    public:
        /**
         * @brief Creates one semantic coordinator for an immutable achievement registry and session.
         * @param registry Complete project-owned achievement definitions.
         * @param session Current immutable session/access snapshot.
         * @param config Finite pending and idempotency capacities.
         * @return Coordinator or InvalidConfiguration.
         */
        [[nodiscard]] static Result<PlatformAchievementCoordinator> Create(std::shared_ptr<const AchievementDefinitionRegistry> registry,
                                                                           PlatformSessionSnapshot session,
                                                                           PlatformAchievementCoordinatorConfig config = {});

        PlatformAchievementCoordinator() = delete;
        ~PlatformAchievementCoordinator();
        PlatformAchievementCoordinator(const PlatformAchievementCoordinator &) = delete;
        PlatformAchievementCoordinator &operator=(const PlatformAchievementCoordinator &) = delete;
        PlatformAchievementCoordinator(PlatformAchievementCoordinator &&other) noexcept;
        PlatformAchievementCoordinator &operator=(PlatformAchievementCoordinator &&other) noexcept;

        /**
         * @brief Validates and records one logical mutation.
         * @param request Registered achievement mutation and caller-owned idempotency identity.
         * @return Queued, IgnoredDuplicate, or a typed validation/conflict/capacity failure.
         */
        [[nodiscard]] Result<PlatformAchievementMutationAdmission> SubmitMutation(const PlatformAchievementMutationRequest &request);

        /**
         * @brief Takes the next mutation for provider publication.
         * @return One token, empty while another token is in flight/no work exists, or Closed.
         */
        [[nodiscard]] Result<std::optional<PlatformAchievementMutationPublication>> TakeNextMutation();

        /**
         * @brief Records one provider completion without retrying or changing its semantic identity.
         * @param publication Token returned by TakeNextMutation.
         * @param outcome Normalized provider outcome.
         * @return Success or StalePublication/Closed.
         */
        [[nodiscard]] Result<void> CompleteMutation(const PlatformAchievementMutationPublication &publication,
                                                    PlatformAchievementPublicationOutcome outcome);

        /**
         * @brief Validates and creates one provider-neutral state query token.
         * @param request Current subject/access and registered achievement ID.
         * @return Query token or typed validation failure.
         */
        [[nodiscard]] Result<PlatformAchievementQueryIntent> MakeStateQuery(const PlatformAchievementStateQueryRequest &request);

        /**
         * @brief Validates one provider state response before it becomes observable.
         * @param query The exact query token used for the provider request.
         * @param state Copied provider-neutral state response.
         * @return Success or malformed/stale state failure.
         */
        [[nodiscard]] Result<void> ValidateStateResult(const PlatformAchievementQueryIntent &query,
                                                       const PlatformAchievementStateSnapshot &state) const;

        /**
         * @brief Replaces session/access authority and discards old mutations/tokens.
         * @param session New immutable session snapshot.
         * @return Success with the new session accepted or a stale/closed failure.
         */
        [[nodiscard]] Result<void> UpdateSession(PlatformSessionSnapshot session);

        /** @brief Closes admission and invalidates all pending/in-flight state. @return Idempotent success. */
        [[nodiscard]] Result<void> Close() noexcept;
        /** @brief Reports whether admission is closed. @return True after Close. */
        [[nodiscard]] bool IsClosed() const noexcept;
        /** @brief Returns the current pending publication count. @return Bounded count. */
        [[nodiscard]] std::size_t PendingCount() const noexcept;
        /** @brief Reports whether one mutation is awaiting provider completion. @return True when in flight. */
        [[nodiscard]] bool HasInFlight() const noexcept;

    private:
        struct LedgerEntry;

        PlatformAchievementCoordinator(std::shared_ptr<const AchievementDefinitionRegistry> registry, PlatformSessionSnapshot session,
                                       PlatformAchievementCoordinatorConfig config);

        [[nodiscard]] Result<const AchievementDefinition *> FindDefinition(AchievementId id) const;
        [[nodiscard]] Result<void> ValidateRequest(const PlatformAchievementMutationRequest &request) const;
        [[nodiscard]] Result<void> ValidateQuery(const PlatformAchievementStateQueryRequest &request) const;
        [[nodiscard]] static bool SameMutation(const PlatformAchievementMutationRequest &left,
                                               const PlatformAchievementMutationRequest &right) noexcept;
        [[nodiscard]] static bool SameSessionAuthority(const PlatformSessionSnapshot &left, const PlatformSessionSnapshot &right) noexcept;
        [[nodiscard]] LedgerEntry *FindLedger(PlatformAchievementMutationId id) noexcept;

        std::shared_ptr<const AchievementDefinitionRegistry> registry_;
        PlatformSessionSnapshot session_;
        PlatformAchievementCoordinatorConfig config_;
        std::vector<LedgerEntry> ledger_;
        std::deque<PlatformAchievementMutationPublication> pending_;
        std::optional<PlatformAchievementMutationPublication> inFlight_;
        std::uint64_t nextSequence_{1};
        std::uint64_t nextQuerySequence_{1};
        bool closed_{};
    };
}  // namespace Horo::PlatformServices
