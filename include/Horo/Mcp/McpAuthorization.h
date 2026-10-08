#pragma once

/** @file McpAuthorization.h
 * @brief Host-owned local authentication, revocable project trust and exact-request approval.
 */

#include "Horo/Mcp/McpSession.h"
#include "Horo/Security/SecureMemory.h"

#include <span>

namespace Horo::Mcp {
    class McpAuthorization;

    /** @brief Non-forgeable authenticated principal; contains no credential material. */
    class McpAuthority final {
        struct Record;

        /** @brief Authentication-only capability preserving non-forgeable principal issuance. */
        class ConstructionKey final {
            friend class McpAuthorization;
            ConstructionKey() = default;
        };

    public:
        /** @brief Adopts an authenticated record through the standard shared allocation factory.
         * @param key Private capability issued only by McpAuthorization::Authenticate.
         * @param record Complete non-null authenticated record.
         */
        explicit McpAuthority(ConstructionKey key, std::shared_ptr<Record> record);
        ~McpAuthority();
        /** @brief Observes explicit revocation and issuer shutdown; this token has no expiry timer.
         * @return Stop token for derived work.
         * @note Jobs must retain the request context and observe IsStopRequested, not use this token as an authority lease. */
        [[nodiscard]] CancellationToken Cancellation() const noexcept;
        /** @brief Returns the immutable credential expiry on the monotonic clock.
         * @return Latest permissible cooperative execution deadline. */
        [[nodiscard]] std::chrono::steady_clock::time_point ExpiresAt() const noexcept;
        /** @brief Observes expiry, issuer destruction and cooperative revocation. @return True when authority ended. */
        [[nodiscard]] bool IsStopRequested() const noexcept;
        McpAuthority(const McpAuthority &) = delete;
        McpAuthority &operator=(const McpAuthority &) = delete;

    private:
        friend class McpAuthorization;
        std::shared_ptr<Record> record_;
    };

    /**
     * @brief Concurrent, bounded host authority shared by session and controller composition.
     * @details Only the local host receives this mutable interface. Clients receive a one-use
     * credential through an approved private channel, never protocol arguments or project files.
     * The policy retains credential digests and request fingerprints, not secrets or arguments.
     * Its mutex protects trust, credential consumption, revocation and single-use approvals.
     */
    class McpAuthorization final {
    public:
        /** @brief Composes an OS entropy source; missing entropy fails closed.
         * @param random Host-owned secure entropy provider. */
        explicit McpAuthorization(std::shared_ptr<Security::SecureRandomSource> random);
        ~McpAuthorization();
        McpAuthorization(const McpAuthorization &) = delete;
        McpAuthorization &operator=(const McpAuthorization &) = delete;

        /** @brief Advances project trust and invalidates old credentials, principals and approvals.
         * @param project Exact host identity; no filesystem path is inferred.
         * @param revision Strictly increasing, nonzero authority revision.
         * @param trusted Explicit local host decision. @return Typed outcome. */
        [[nodiscard]] Result<void> SetTrust(std::optional<std::string> project, std::uint64_t revision, bool trusted) const;

        /** @brief Issues one bounded, one-use local credential for an exact host admission.
         * @param admission Host-approved identity, capabilities, project and revisions.
         * @param lifetime Positive lifetime, at most 24 hours. @return Zeroizing secret or typed failure.
         * @note Returned bytes must be delivered privately and destroyed after authentication. */
        [[nodiscard]] Result<Security::SecureBytes> IssueCredential(const McpSessionAdmission &admission,
                                                                    std::chrono::milliseconds lifetime) const;

        /** @brief Consumes a credential on its first matching attempt; replay always fails.
         * @param admission Exact host-approved admission, not caller-supplied authority.
         * @param credential Move-only proof, consumed and zeroized on every path.
         * @return Immutable principal or typed denial. */
        [[nodiscard]] Result<std::shared_ptr<const McpAuthority>> Authenticate(const McpSessionAdmission &admission,
                                                                               Security::SecureBytes credential) const;

        /** @brief Validates principal provenance, expiry, revocation and all context fields.
         * @param context Immutable request authority. @return Typed outcome. */
        [[nodiscard]] Result<void> Validate(const McpRequestContext &context) const;

        /** @brief Checks live authority and the complete cooperative stop view at an execution boundary.
         * @details Shared by session dispatch and registry invocation, which are separate owning targets.
         * Authority denial takes precedence over deadline/cancellation; no approval is consumed and no adapter is invoked.
         * @param context Retained operation context. @return Authority denial or request cancellation, otherwise success.
         * @note A deadline is reported as RequestCancelled here; session/controller boundaries own timeout presentation. */
        [[nodiscard]] Result<void> ValidateActive(const McpRequestContext &context) const;

        /** @brief Validates the bounded exact tools/call envelope shared by admission and approval hashing.
         * @details Used by the controller target and session-owned approval fingerprinting. Requires a bounded request ID,
         * exactly name/arguments, an opaque tool identity and object arguments. This structural check grants no authority.
         * @param request Immutable request. @return Typed malformed-input or capacity failure, otherwise success. */
        [[nodiscard]] static Result<void> ValidateCallRequest(const McpRequest &request);

        /** @brief Creates a content-free challenge bound to exact arguments, ID and registry revision.
         * @param context Current authenticated authority. @param request Exact tools/call envelope.
         * @param lifetime Positive lifetime, at most 24 hours. @return Host-local challenge identity. */
        [[nodiscard]] Result<std::uint64_t> Challenge(const McpRequestContext &context, const McpRequest &request,
                                                      std::chrono::milliseconds lifetime) const;

        /** @brief Applies an explicit local decision; denial erases the challenge permanently.
         * @param challenge Host-local challenge identity. @param approved Visible host decision.
         * @return Typed outcome. */
        [[nodiscard]] Result<void> Decide(std::uint64_t challenge, bool approved) const;

        /** @brief Rechecks authority and exact approval at admission and again at owner execution.
         * @details Successful consuming authorization is the execution-admission linearization point.
         * Revocation ordered before admission denies it; later revocation signals cooperative cancellation.
         * No policy lock is held across adapter code, and already committed application effects are not rolled back.
         * @param context Current authenticated authority. @param request Exact immutable request.
         * @param requiresApproval Whether the registered effect is not Query.
         * @param consume True only immediately before application invocation.
         * @return Typed outcome; consumed approvals cannot be replayed. */
        [[nodiscard]] Result<void> Authorize(const McpRequestContext &context, const McpRequest &request, bool requiresApproval,
                                             bool consume) const;

        /** @brief Revokes one authenticated principal and all its pending approvals.
         * @param authority Principal emitted by this policy; foreign principals are ignored. */
        void Revoke(const std::shared_ptr<const McpAuthority> &authority) const;

    private:
        struct State;
        std::shared_ptr<State> state_;
    };
}  // namespace Horo::Mcp
