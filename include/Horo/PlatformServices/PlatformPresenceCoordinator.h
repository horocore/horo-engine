#pragma once

/**
 * @file PlatformPresenceCoordinator.h
 * @brief Registry-mapped, session-fenced and coalesced platform presence publication.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/PlatformServices/PlatformDefinitionRegistries.h"
#include "Horo/PlatformServices/PlatformUserSession.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace Horo::PlatformServices {
    /** @brief Hard maximum for one authored presence detail payload in UTF-8 bytes. */
    inline constexpr std::size_t MaximumPlatformPresenceDetailBytes = 4'096;
    /** @brief Hard maximum delay between successful presence publications. */
    inline constexpr std::chrono::milliseconds MaximumPlatformPresenceInterval{3'600'000};

    /** @brief One provider-neutral desired presence operation. */
    enum class PlatformPresenceOperation : std::uint8_t {
        Set,
        Clear
    };

    /** @brief A typed presence set intent captured against one access-policy revision. */
    struct PlatformPresenceSetRequest final {
        PlatformSubjectHandle subject;
        PlatformAccessPolicyRevision accessRevision;
        PresenceStatusId status;
        std::string detail;
    };

    /** @brief A typed presence clear intent captured against one access-policy revision. */
    struct PlatformPresenceClearRequest final {
        PlatformSubjectHandle subject;
        PlatformAccessPolicyRevision accessRevision;
    };

    /** @brief Immutable latest-wins presence intent ready for a provider adapter. */
    struct PlatformPresenceIntent final {
        PlatformPresenceOperation operation{PlatformPresenceOperation::Clear};
        PlatformSubjectHandle subject;
        PlatformSessionGeneration sessionGeneration;
        PlatformAccessPolicyRevision accessRevision;
        std::optional<PresenceStatusId> status;
        std::string detail;
        std::uint64_t sequence{};
    };

    /** @brief One admitted publication token; the adapter acknowledges it after the provider call completes. */
    struct PlatformPresencePublication final {
        PlatformPresenceIntent intent;

        /** @brief Reports whether this publication represents a clear operation. @return True for Clear. */
        [[nodiscard]] bool IsClear() const noexcept {
            return intent.operation == PlatformPresenceOperation::Clear;
        }
    };

    /** @brief Admission result for a desired-state update. */
    enum class PlatformPresenceAdmission : std::uint8_t {
        Queued,
        Coalesced,
        IgnoredDuplicate
    };

    /** @brief Result of replacing the session authority used by the coordinator. */
    enum class PlatformPresenceSessionUpdate : std::uint8_t {
        Unchanged,
        Replaced,
        Invalidated
    };

    /** @brief Normalized provider outcome supplied when an adapter finishes one publication. */
    enum class PlatformPresencePublicationOutcome : std::uint8_t {
        Succeeded,
        Failed,
        Cancelled
    };

    /** @brief Stable failures owned by the presence mapping/coalescing boundary. */
    namespace PresenceCoordinatorErrors {
        /** @brief Coordinator configuration or registry ownership is invalid. */
        extern const ErrorCodeDescriptor InvalidConfiguration;
        /** @brief The request contains an invalid subject, access revision or status representation. */
        extern const ErrorCodeDescriptor InvalidRequest;
        /** @brief The status is not present in the immutable project registry. */
        extern const ErrorCodeDescriptor StatusNotRegistered;
        /** @brief The registered status forbids free detail text. */
        extern const ErrorCodeDescriptor DetailForbidden;
        /** @brief The detail is not a complete valid UTF-8 scalar sequence. */
        extern const ErrorCodeDescriptor DetailInvalidUtf8;
        /** @brief The detail exceeds the project or coordinator bound. */
        extern const ErrorCodeDescriptor DetailTooLarge;
        /** @brief The coordinator has closed admission. */
        extern const ErrorCodeDescriptor Closed;
        /** @brief A publication token no longer belongs to the current session state. */
        extern const ErrorCodeDescriptor StalePublication;
    }  // namespace PresenceCoordinatorErrors

    /** @brief Finite policy for one session-scoped presence coordinator. */
    struct PlatformPresenceCoordinatorConfig final {
        std::size_t maximumDetailBytes{1'024};
        std::chrono::milliseconds minimumInterval{250};
    };

    /**
     * @brief Validates registry-backed presence and coalesces one session's desired state.
     * @details The coordinator has one owner lane, retains at most one pending intent, and never contains provider strings or
     * native state. A provider adapter takes a publication token, performs its asynchronous call, and acknowledges the token
     * later. Session replacement, sign-out, access-policy changes and close discard pending work and stale any in-flight token.
     */
    class PlatformPresenceCoordinator final {
    public:
        /**
         * @brief Creates a coordinator over an immutable presence registry and session authority.
         * @param registry Complete project-owned presence definitions; status IDs are resolved only through this snapshot.
         * @param session Current immutable session/access snapshot.
         * @param config Finite detail and publication-rate policy.
         * @return Coordinator or InvalidConfiguration.
         */
        [[nodiscard]] static Result<PlatformPresenceCoordinator> Create(std::shared_ptr<const PresenceDefinitionRegistry> registry,
                                                                        PlatformSessionSnapshot session,
                                                                        PlatformPresenceCoordinatorConfig config = {});

        PlatformPresenceCoordinator() = delete;
        ~PlatformPresenceCoordinator();
        PlatformPresenceCoordinator(const PlatformPresenceCoordinator &) = delete;
        PlatformPresenceCoordinator &operator=(const PlatformPresenceCoordinator &) = delete;
        PlatformPresenceCoordinator(PlatformPresenceCoordinator &&other) noexcept;
        PlatformPresenceCoordinator &operator=(PlatformPresenceCoordinator &&other) noexcept;

        /**
         * @brief Validates and latest-wins coalesces one presence set intent.
         * @param request Session/access-fenced status and bounded presentation detail.
         * @return Queued, Coalesced, IgnoredDuplicate, or typed validation failure.
         */
        [[nodiscard]] Result<PlatformPresenceAdmission> SubmitSet(PlatformPresenceSetRequest request);
        /**
         * @brief Validates and latest-wins coalesces one presence clear intent.
         * @param request Current subject and access-policy evidence.
         * @return Queued, Coalesced, IgnoredDuplicate, or typed validation failure.
         */
        [[nodiscard]] Result<PlatformPresenceAdmission> SubmitClear(const PlatformPresenceClearRequest &request);

        /**
         * @brief Takes the latest pending intent when no publication is in flight and rate policy permits it.
         * @param now Monotonic owner-lane time used for rate limiting.
         * @return A publication token, an empty value when not ready, or Closed.
         */
        [[nodiscard]] Result<std::optional<PlatformPresencePublication>> TakeReady(std::chrono::steady_clock::time_point now);

        /**
         * @brief Acknowledges one provider completion and starts the next rate-limit interval.
         * @param publication Token returned by TakeReady.
         * @param outcome Normalized provider outcome; the coordinator does not retry implicitly.
         * @param now Monotonic owner-lane completion time.
         * @return Success or StalePublication/Closed.
         */
        [[nodiscard]] Result<void> Complete(const PlatformPresencePublication &publication, PlatformPresencePublicationOutcome outcome,
                                            std::chrono::steady_clock::time_point now);

        /**
         * @brief Replaces session/access authority and invalidates old desired state.
         * @param session New validated immutable snapshot, including explicit sign-out states.
         * @return Unchanged, Replaced, Invalidated, or a stale/closed failure.
         */
        [[nodiscard]] Result<PlatformPresenceSessionUpdate> UpdateSession(PlatformSessionSnapshot session);

        /** @brief Closes admission and invalidates all pending/in-flight state. @return Idempotent success. */
        [[nodiscard]] Result<void> Close() noexcept;
        /** @brief Reports whether the coordinator is closed. @return True after Close. */
        [[nodiscard]] bool IsClosed() const noexcept;
        /** @brief Reports whether one desired state is waiting for publication. @return True when pending. */
        [[nodiscard]] bool HasPending() const noexcept;
        /** @brief Reports whether a provider publication is awaiting acknowledgement. @return True when in flight. */
        [[nodiscard]] bool HasInFlight() const noexcept;
        /** @brief Returns the next monotonic intent sequence. @return Zero before first accepted intent. */
        [[nodiscard]] std::uint64_t LastSequence() const noexcept;

    private:
        PlatformPresenceCoordinator(std::shared_ptr<const PresenceDefinitionRegistry> registry, PlatformSessionSnapshot session,
                                    PlatformPresenceCoordinatorConfig config) noexcept;

        [[nodiscard]] Result<void> ValidateSet(const PlatformPresenceSetRequest &request) const;
        [[nodiscard]] Result<void> ValidateClear(const PlatformPresenceClearRequest &request) const;
        [[nodiscard]] PlatformPresenceIntent MakeSetIntent(PlatformPresenceSetRequest request) const;
        [[nodiscard]] PlatformPresenceIntent MakeClearIntent(const PlatformPresenceClearRequest &request) const;
        [[nodiscard]] static bool SameIntent(const PlatformPresenceIntent &left, const PlatformPresenceIntent &right) noexcept;

        std::shared_ptr<const PresenceDefinitionRegistry> registry_;
        PlatformSessionSnapshot session_;
        PlatformPresenceCoordinatorConfig config_;
        std::optional<PlatformPresenceIntent> pending_;
        std::optional<PlatformPresencePublication> inFlight_;
        std::optional<std::chrono::steady_clock::time_point> lastCompletedAt_;
        std::uint64_t nextSequence_{1};
        bool closed_{};
    };
}  // namespace Horo::PlatformServices
