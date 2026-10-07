#include "Horo/Mcp/McpAuthorization.h"

#include "Horo/Foundation/Sha256.h"
#include "Horo/Mcp/McpErrors.h"
#include "McpJsonBounds.h"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <mutex>

namespace Horo::Mcp {
    namespace {
        using Clock = std::chrono::steady_clock;

        /** @brief Classifies ASCII letters and digits without locale-dependent authority interpretation. */
        bool Alphanumeric(const unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        }

        /** @brief Restricts authority identities to bounded opaque tokens, never paths or free text. */
        bool Identity(const std::string &value) {
            return !value.empty() && value.size() <= 256 && std::ranges::all_of(value, [](const unsigned char c) {
                return Alphanumeric(c) || c == '.' || c == '_' || c == '-';
            });
        }

        /** @brief Validates the complete bounded host grant before entropy or metadata retention. */
        bool Admission(const McpSessionAdmission &value) {
            if (!Identity(value.clientIdentity) || value.authorizationRevision == 0 || value.registryRevision == 0 ||
                value.capabilities.size() > 64 || (value.projectIdentity && !Identity(*value.projectIdentity)))
                return false;
            for (std::size_t i = 0; i < value.capabilities.size(); ++i) {
                if (!Identity(value.capabilities[i]) || std::find(value.capabilities.begin(), value.capabilities.begin() + i,
                                                                  value.capabilities[i]) != value.capabilities.begin() + i)
                    return false;
            }
            return true;
        }

        /** @brief Limits credential and challenge lifetime to a finite host policy window. */
        bool Lifetime(const std::chrono::milliseconds value) {
            return value > std::chrono::milliseconds::zero() && value <= std::chrono::hours{24};
        }

        /** @brief Rejects substitution of any field of a previously issued credential scope. */
        bool SameAdmission(const McpSessionAdmission &a, const McpSessionAdmission &b) {
            return a.clientIdentity == b.clientIdentity && a.capabilities == b.capabilities && a.projectIdentity == b.projectIdentity &&
                   a.authorizationRevision == b.authorizationRevision && a.registryRevision == b.registryRevision;
        }

        /** @brief Checks call fields only after structural budgets and field presence have been established. */
        bool CallFields(const McpRequest &request) {
            return request.params["name"].is_string() && Identity(request.params["name"].get<std::string>()) &&
                   request.params["arguments"].is_object() && (request.id.is_string() || request.id.is_number_integer());
        }

        /** @brief Requires exactly the tool identity and argument object, not caller-supplied policy fields. */
        bool CallShape(const McpRequest &request) {
            if (request.method != "tools/call" || !request.params.is_object() || request.params.size() != 2 ||
                !request.params.contains("name") || !request.params.contains("arguments"))
                return false;
            return CallFields(request);
        }

        /** @brief Hashes exact canonical decoded input without retaining request payloads. */
        Result<Sha256Digest> Fingerprint(const McpRequest &request) {
            const auto validated = McpAuthorization::ValidateCallRequest(request);
            if (validated.HasError())
                return Result<Sha256Digest>::Failure(validated.ErrorValue());
            const auto bytes = nlohmann::json{{"id", request.id}, {"method", request.method}, {"params", request.params}}.dump();
            if (bytes.size() > (256U << 10U))
                return Result<Sha256Digest>::Failure(MakeError(McpErrors::InputCapacityExceeded));
            return Result<Sha256Digest>::Success(ComputeSha256(std::as_bytes(std::span{bytes.data(), bytes.size()})));
        }

        /** @brief Clears temporary entropy on success, failure and stack unwinding. */
        struct SecretScratch final {
            std::array<std::byte, 32> bytes{};

            ~SecretScratch() {
                Security::SecureZero(bytes);
            }
        };
    }  // namespace

    struct McpAuthority::Record final {
        std::weak_ptr<void> issuer;
        McpSessionAdmission admission;
        Clock::time_point expires;
        bool revoked{};
        CancellationSource cancellation;
    };

    struct McpAuthorization::State final {
        struct Trust final {
            std::uint64_t revision{};
            bool trusted{};
        };

        struct Invitation final {
            McpSessionAdmission admission;
            Clock::time_point expires;
        };

        struct Approval final {
            std::weak_ptr<const McpAuthority> authority;
            Sha256Digest fingerprint;
            Clock::time_point expires;
            bool approved{};
        };

        std::mutex mutex;
        std::shared_ptr<Security::SecureRandomSource> random;
        std::map<std::optional<std::string>, Trust> trust;
        std::map<Sha256Digest, Invitation> invitations;
        std::map<std::uint64_t, Approval> approvals;
        std::vector<std::weak_ptr<McpAuthority::Record>> principals;
        std::uint64_t nextChallenge{1};

        bool Trusted(const McpSessionAdmission &admission) const {
            const auto found = trust.find(admission.projectIdentity);
            return found != trust.end() && found->second.trusted && found->second.revision == admission.authorizationRevision;
        }

        bool Current(const McpRequestContext &context) const {
            if (!context.authority)
                return false;
            const auto &record = *context.authority->record_;
            const auto issuer = record.issuer.lock();
            const auto &admission = record.admission;
            return issuer.get() == this && !record.revoked && Clock::now() < record.expires && Trusted(admission) &&
                   context.clientIdentity == admission.clientIdentity && context.capabilities == admission.capabilities &&
                   context.projectIdentity == admission.projectIdentity &&
                   context.authorizationRevision == admission.authorizationRevision &&
                   context.registryRevision == admission.registryRevision;
        }

        /** @brief Invalidates every principal, invitation and approval in a replaced project scope under the policy lock. */
        void RevokeProject(const std::optional<std::string> &project) {
            for (const auto &weak : principals) {
                if (const auto record = weak.lock(); record && record->admission.projectIdentity == project) {
                    record->revoked = true;
                    record->cancellation.RequestCancellation();
                }
            }
            std::erase_if(invitations, [&project](const auto &entry) {
                return entry.second.admission.projectIdentity == project;
            });
            std::erase_if(approvals, [&project](const auto &entry) {
                const auto authority = entry.second.authority.lock();
                return !authority || authority->record_->admission.projectIdentity == project;
            });
        }

        /** @brief Reclaims expired metadata without weakening replay denial or authority revisions. */
        void Prune() {
            const auto now = Clock::now();
            std::erase_if(invitations, [now](const auto &entry) {
                return entry.second.expires <= now;
            });
            std::erase_if(approvals, [now](const auto &entry) {
                return entry.second.expires <= now || entry.second.authority.expired();
            });
            std::erase_if(principals, [](const auto &record) {
                return record.expired();
            });
        }
    };

    McpAuthority::McpAuthority(std::shared_ptr<Record> record) : record_(std::move(record)) {}

    McpAuthority::~McpAuthority() = default;

    /** @copydoc McpAuthority::Cancellation */
    CancellationToken McpAuthority::Cancellation() const noexcept {
        return record_->cancellation.Token();
    }

    /** @copydoc McpAuthority::ExpiresAt */
    std::chrono::steady_clock::time_point McpAuthority::ExpiresAt() const noexcept {
        return record_->expires;
    }

    /** @copydoc McpAuthority::IsStopRequested */
    bool McpAuthority::IsStopRequested() const noexcept {
        return record_->issuer.expired() || Clock::now() >= record_->expires || record_->cancellation.Token().IsCancellationRequested();
    }

    /** @copydoc McpAuthorization::McpAuthorization */
    McpAuthorization::McpAuthorization(std::shared_ptr<Security::SecureRandomSource> random) : state_(std::make_shared<State>()) {
        state_->random = std::move(random);
    }

    McpAuthorization::~McpAuthorization() {
        std::lock_guard lock{state_->mutex};
        for (const auto &weak : state_->principals)
            if (const auto record = weak.lock())
                record->cancellation.RequestCancellation();
    }

    /** @copydoc McpAuthorization::SetTrust */
    Result<void> McpAuthorization::SetTrust(std::optional<std::string> project, const std::uint64_t revision, const bool trusted) {
        if ((project && !Identity(*project)) || revision == 0)
            return Result<void>::Failure(MakeError(McpErrors::AuthorizationDenied));
        std::lock_guard lock{state_->mutex};
        const auto found = state_->trust.find(project);
        if ((found != state_->trust.end() && revision <= found->second.revision) ||
            (found == state_->trust.end() && state_->trust.size() >= 64))
            return Result<void>::Failure(MakeError(McpErrors::AuthorizationDenied));
        state_->trust.insert_or_assign(project, State::Trust{revision, trusted});
        state_->RevokeProject(project);
        return Result<void>::Success();
    }

    /** @copydoc McpAuthorization::IssueCredential */
    Result<Security::SecureBytes> McpAuthorization::IssueCredential(const McpSessionAdmission &admission,
                                                                    const std::chrono::milliseconds lifetime) {
        if (!Admission(admission) || !Lifetime(lifetime) || admission.authority)
            return Result<Security::SecureBytes>::Failure(MakeError(McpErrors::AuthorizationDenied));
        std::lock_guard lock{state_->mutex};
        state_->Prune();
        if (!state_->random || !state_->Trusted(admission) || state_->invitations.size() >= 64)
            return Result<Security::SecureBytes>::Failure(MakeError(McpErrors::AuthorizationDenied));
        SecretScratch scratch;
        const auto entropy = state_->random->Fill(scratch.bytes);
        if (entropy.HasError())
            return Result<Security::SecureBytes>::Failure(entropy.ErrorValue());
        const auto digest = ComputeSha256(scratch.bytes);
        if (!state_->invitations.try_emplace(digest, State::Invitation{admission, Clock::now() + lifetime}).second)
            return Result<Security::SecureBytes>::Failure(MakeError(McpErrors::AuthorizationDenied));
        return Result<Security::SecureBytes>::Success(Security::SecureBytes{scratch.bytes});
    }

    /** @copydoc McpAuthorization::Authenticate */
    Result<std::shared_ptr<const McpAuthority>> McpAuthorization::Authenticate(const McpSessionAdmission &admission,
                                                                               Security::SecureBytes credential) {
        if (credential.View().size() != 32 || !Admission(admission) || admission.authority)
            return Result<std::shared_ptr<const McpAuthority>>::Failure(MakeError(McpErrors::AuthorizationDenied));
        const auto digest = ComputeSha256(credential.View());
        credential.Clear();
        std::lock_guard lock{state_->mutex};
        state_->Prune();
        const auto found = state_->invitations.find(digest);
        if (found == state_->invitations.end())
            return Result<std::shared_ptr<const McpAuthority>>::Failure(MakeError(McpErrors::AuthorizationDenied));
        auto invitation = std::move(found->second);
        state_->invitations.erase(found);  // Consume before scope checks; a substituted request cannot retry this proof.
        if (invitation.expires <= Clock::now() || !state_->Trusted(admission) || !SameAdmission(admission, invitation.admission) ||
            state_->principals.size() >= 128)
            return Result<std::shared_ptr<const McpAuthority>>::Failure(MakeError(McpErrors::AuthorizationDenied));
        auto record = std::make_shared<McpAuthority::Record>();
        record->issuer = state_;
        record->admission = std::move(invitation.admission);
        record->expires = invitation.expires;
        state_->principals.push_back(record);
        return Result<std::shared_ptr<const McpAuthority>>::Success(
            std::shared_ptr<const McpAuthority>{new McpAuthority{std::move(record)}});
    }

    /** @copydoc McpAuthorization::Validate */
    Result<void> McpAuthorization::Validate(const McpRequestContext &context) const {
        std::lock_guard lock{state_->mutex};
        return state_->Current(context) ? Result<void>::Success() : Result<void>::Failure(MakeError(McpErrors::AuthorizationDenied));
    }

    /** @copydoc McpAuthorization::ValidateActive */
    Result<void> McpAuthorization::ValidateActive(const McpRequestContext &context) const {
        const auto current = Validate(context);
        if (current.HasError())
            return current;
        return context.IsStopRequested() ? Result<void>::Failure(MakeError(McpErrors::RequestCancelled)) : Result<void>::Success();
    }

    /** @copydoc McpAuthorization::ValidateCallRequest */
    Result<void> McpAuthorization::ValidateCallRequest(const McpRequest &request) {
        const McpSessionLimits limits;
        if (!ValidRequestId(request.id, limits) || !JsonWithinBounds(request.params, limits, limits.maximumInputBytes))
            return Result<void>::Failure(MakeError(McpErrors::InputCapacityExceeded));
        return CallShape(request) ? Result<void>::Success() : Result<void>::Failure(MakeError(McpErrors::RequestInvalid));
    }

    /** @copydoc McpAuthorization::Challenge */
    Result<std::uint64_t> McpAuthorization::Challenge(const McpRequestContext &context, const McpRequest &request,
                                                      const std::chrono::milliseconds lifetime) {
        if (!Lifetime(lifetime))
            return Result<std::uint64_t>::Failure(MakeError(McpErrors::AuthorizationDenied));
        if (const auto authorized = Validate(context); authorized.HasError())
            return Result<std::uint64_t>::Failure(authorized.ErrorValue());
        const auto fingerprint = Fingerprint(request);
        if (fingerprint.HasError())
            return Result<std::uint64_t>::Failure(fingerprint.ErrorValue());
        std::lock_guard lock{state_->mutex};
        state_->Prune();
        if (!state_->Current(context) || state_->approvals.size() >= 128 ||
            state_->nextChallenge == std::numeric_limits<std::uint64_t>::max())
            return Result<std::uint64_t>::Failure(MakeError(McpErrors::AuthorizationDenied));
        const auto id = state_->nextChallenge++;
        state_->approvals.emplace(id, State::Approval{context.authority, fingerprint.Value(), Clock::now() + lifetime});
        return Result<std::uint64_t>::Success(id);
    }

    /** @copydoc McpAuthorization::Decide */
    Result<void> McpAuthorization::Decide(const std::uint64_t challenge, const bool approved) {
        std::lock_guard lock{state_->mutex};
        state_->Prune();
        const auto found = state_->approvals.find(challenge);
        if (found == state_->approvals.end())
            return Result<void>::Failure(MakeError(McpErrors::ApprovalRequired));
        if (!approved)
            state_->approvals.erase(found);
        else
            found->second.approved = true;
        return Result<void>::Success();
    }

    /** @copydoc McpAuthorization::Authorize */
    Result<void> McpAuthorization::Authorize(const McpRequestContext &context, const McpRequest &request, const bool requiresApproval,
                                             const bool consume) {
        std::lock_guard lock{state_->mutex};
        state_->Prune();
        if (!state_->Current(context))
            return Result<void>::Failure(MakeError(McpErrors::AuthorizationDenied));
        if (!requiresApproval)
            return Result<void>::Success();
        const auto fingerprint = Fingerprint(request);
        if (fingerprint.HasError())
            return Result<void>::Failure(fingerprint.ErrorValue());
        const auto found = std::find_if(state_->approvals.begin(), state_->approvals.end(), [&](const auto &entry) {
            return entry.second.approved && entry.second.authority.lock() == context.authority &&
                   entry.second.fingerprint == fingerprint.Value();
        });
        if (found == state_->approvals.end())
            return Result<void>::Failure(MakeError(McpErrors::ApprovalRequired));
        if (consume)
            state_->approvals.erase(found);
        return Result<void>::Success();
    }

    /** @copydoc McpAuthorization::Revoke */
    void McpAuthorization::Revoke(const std::shared_ptr<const McpAuthority> &authority) {
        if (!authority)
            return;
        std::lock_guard lock{state_->mutex};
        if (authority->record_->issuer.lock().get() != state_.get())
            return;
        authority->record_->revoked = true;
        authority->record_->cancellation.RequestCancellation();
        std::erase_if(state_->approvals, [&authority](const auto &entry) {
            return entry.second.authority.lock() == authority;
        });
    }
}  // namespace Horo::Mcp
