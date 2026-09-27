#include "Horo/Security/AgentPolicy.h"

#include <algorithm>
#include <limits>
#include <span>
#include <string_view>
#include <unordered_set>

namespace Horo::Security {
    using enum AgentDataClass;
    using enum AgentToolRisk;
    using enum AgentPolicyReason;

    namespace {
        /** @brief Rejects unknown provider values before any local/cloud branch. */
        [[nodiscard]] bool ValidProvider(const AgentProviderResidence provider) noexcept {
            return provider == AgentProviderResidence::Local || provider == AgentProviderResidence::Cloud;
        }

        /** @brief Rejects unknown data classes instead of admitting an unclassified value. */
        [[nodiscard]] bool ValidDataClass(const AgentDataClass dataClass) noexcept {
            switch (dataClass) {
                case EditorContext:
                case SceneData:
                case ProjectFile:
                case AssetMetadata:
                case ToolResult:
                case ProposedChange:
                case Transcript:
                case SensitiveProjectData:
                case Credential:
                case RawSensorData:
                    return true;
            }
            return false;
        }

        /** @brief Rejects unknown tool risks rather than treating them as reads. */
        [[nodiscard]] bool ValidToolRisk(const AgentToolRisk risk) noexcept {
            switch (risk) {
                case ReadOnly:
                case Mutation:
                case Destructive:
                case Execution:
                case CredentialAccess:
                case NetworkAccess:
                    return true;
            }
            return false;
        }

        /** @brief Classifies data that must never enter an ordinary agent context. */
        [[nodiscard]] bool IsExcluded(const AgentDataClass dataClass) noexcept {
            return dataClass == Credential || dataClass == RawSensorData;
        }

        /** @brief Requires the host to supply an already-redacted value before admission. */
        [[nodiscard]] bool NeedsRedaction(const AgentDataClass dataClass) noexcept {
            return dataClass == ProjectFile || dataClass == ToolResult || dataClass == ProposedChange || dataClass == Transcript ||
                   dataClass == SensitiveProjectData;
        }

        /** @brief Identifies operations that are never admitted for an untrusted project. */
        [[nodiscard]] bool NeedsProjectTrust(const AgentToolRisk risk) noexcept {
            return risk == Mutation || risk == Destructive || risk == Execution || risk == CredentialAccess || risk == NetworkAccess;
        }

        /** @brief Hashes preview bytes without retaining their content in the consent grant. */
        [[nodiscard]] Sha256Digest DigestText(const std::string_view text) noexcept {
            return ComputeSha256(std::as_bytes(std::span{text.data(), text.size()}));
        }
    }  // namespace

    /** @copydoc AgentPolicy::AgentPolicy */
    AgentPolicy::AgentPolicy(const AgentPolicyLimits &limits) noexcept : limits_(limits), valid_(ValidLimits()) {
        if (!valid_)
            limits_ = {};
    }

    /** @copydoc AgentPolicy::SetProjectTrust */
    bool AgentPolicy::SetProjectTrust(const bool trusted, const std::uint64_t revision) noexcept {
        if (!valid_ || revision == 0 || revision <= revision_)
            return false;
        trusted_ = trusted;
        revision_ = revision;
        contextGrant_.reset();
        dispatchGrant_.reset();
        toolGrant_.reset();
        return true;
    }

    /** @copydoc AgentPolicy::GrantContextConsent */
    AgentPolicyDecision AgentPolicy::GrantContextConsent(const AgentProviderResidence provider, const std::uint64_t requestId,
                                                         const std::vector<AgentContextCandidate> &visibleItems,
                                                         const std::uint64_t revision) {
        if (!Current(revision))
            return Decision(provider, StaleAuthority);
        if (!ValidProvider(provider))
            return Decision(provider, InvalidRequest);
        if (provider == AgentProviderResidence::Cloud && !trusted_)
            return Decision(provider, ProjectUntrusted);
        if (requestId == 0 || visibleItems.empty() || visibleItems.size() > limits_.maximumContextItems || contextGrant_ || dispatchGrant_)
            return Decision(provider, InvalidRequest);
        std::unordered_set<std::uint64_t> distinct;
        ContextGrant grant;
        grant.provider = provider;
        grant.requestId = requestId;
        grant.revision = revision;
        std::size_t totalBytes{};
        for (const AgentContextCandidate &item : visibleItems) {
            if (item.itemId == 0 || !distinct.insert(item.itemId).second || !item.visibleToUser || !ValidDataClass(item.dataClass))
                return Decision(provider, InvalidRequest);
            if (IsExcluded(item.dataClass))
                return Decision(provider, SensitiveDataExcluded, item.dataClass);
            if (NeedsRedaction(item.dataClass) && !item.redacted)
                return Decision(provider, ContextNotRedacted, item.dataClass);
            if (item.text.size() > limits_.maximumItemBytes || item.text.size() > limits_.maximumContextBytes - totalBytes)
                return Decision(provider, ContextLimitExceeded, item.dataClass);
            totalBytes += item.text.size();
            grant.items.emplace_back(item.itemId, item.dataClass, DigestText(item.text), item.redacted);
        }
        contextGrant_ = std::move(grant);
        return Decision(provider, Allowed, std::nullopt, std::nullopt, requestId);
    }

    /** @copydoc AgentPolicy::DenyContext */
    void AgentPolicy::DenyContext(const std::uint64_t requestId) {
        if (requestId == 0)
            return;
        AgentProviderResidence provider = AgentProviderResidence::Local;
        if (contextGrant_ && contextGrant_->requestId == requestId) {
            provider = contextGrant_->provider;
            contextGrant_.reset();
        }
        if (dispatchGrant_ && dispatchGrant_->requestId == requestId) {
            provider = dispatchGrant_->provider;
            dispatchGrant_.reset();
        }
        static_cast<void>(Decision(provider, Denied, std::nullopt, std::nullopt, requestId));
    }

    /** @copydoc AgentPolicy::BuildContext */
    std::pair<AgentContextEnvelope, AgentPolicyDecision> AgentPolicy::BuildContext(const AgentProviderResidence provider,
                                                                                   const std::uint64_t requestId,
                                                                                   const std::vector<AgentContextCandidate> &candidates,
                                                                                   const std::uint64_t revision) {
        AgentContextEnvelope empty;
        if (!Current(revision))
            return {std::move(empty), Decision(provider, StaleAuthority)};
        if (!ValidProvider(provider))
            return {std::move(empty), Decision(provider, InvalidRequest)};
        if (provider == AgentProviderResidence::Cloud && !trusted_)
            return {std::move(empty), Decision(provider, ProjectUntrusted)};
        if (dispatchGrant_)
            return {std::move(empty), Decision(provider, InvalidRequest)};
        if (requestId == 0 || !contextGrant_ || contextGrant_->provider != provider || contextGrant_->requestId != requestId ||
            contextGrant_->revision != revision)
            return {std::move(empty), Decision(provider, ConsentRequired)};

        ContextGrant grant = std::move(*contextGrant_);
        contextGrant_.reset();
        return AssembleAdmittedContext(std::move(grant), candidates);
    }

    /** @copydoc AgentPolicy::AssembleAdmittedContext */
    std::pair<AgentContextEnvelope, AgentPolicyDecision> AgentPolicy::AssembleAdmittedContext(
        ContextGrant grant, const std::vector<AgentContextCandidate> &candidates) {
        AgentContextEnvelope empty;
        const AgentProviderResidence provider = grant.provider;
        const std::uint64_t requestId = grant.requestId;
        if (candidates.size() > limits_.maximumContextItems)
            return {std::move(empty), Decision(provider, ContextLimitExceeded)};

        AgentContextEnvelope result;
        result.provider = provider;
        result.requestId = requestId;
        result.projectRevision = grant.revision;
        std::unordered_set<std::uint64_t> seen;
        for (const AgentContextCandidate &candidate : candidates) {
            if (candidate.itemId == 0 || !seen.insert(candidate.itemId).second || !ValidDataClass(candidate.dataClass))
                return {std::move(empty), Decision(provider, InvalidRequest)};
            const auto approved = std::ranges::find_if(grant.items, [&candidate](const ContextFingerprint &item) {
                return item.itemId == candidate.itemId;
            });
            if (approved == grant.items.end()) {
                static_cast<void>(Decision(provider, ContextNotAdmitted, candidate.dataClass));
                continue;
            }
            if (!candidate.visibleToUser)
                return {std::move(empty), Decision(provider, ContextNotVisible, candidate.dataClass)};
            if (IsExcluded(candidate.dataClass))
                return {std::move(empty), Decision(provider, SensitiveDataExcluded, candidate.dataClass)};
            if (NeedsRedaction(candidate.dataClass) && !candidate.redacted)
                return {std::move(empty), Decision(provider, ContextNotRedacted, candidate.dataClass)};
            if (candidate.dataClass != approved->dataClass || candidate.redacted != approved->redacted ||
                DigestText(candidate.text) != approved->textDigest)
                return {std::move(empty), Decision(provider, StaleAuthority, candidate.dataClass)};
            if (candidate.text.size() > limits_.maximumItemBytes || candidate.text.size() > limits_.maximumContextBytes - result.totalBytes)
                return {std::move(empty), Decision(provider, ContextLimitExceeded, candidate.dataClass)};
            result.totalBytes += candidate.text.size();
            result.items.push_back(candidate);
        }
        if (result.items.empty() || result.items.size() != grant.items.size())
            return {std::move(empty), Decision(provider, ContextNotAdmitted)};
        dispatchGrant_ = std::move(grant);
        return {std::move(result), Decision(provider, Allowed, std::nullopt, std::nullopt, requestId)};
    }

    /** @copydoc AgentPolicy::AuthorizeContextDispatch */
    AgentPolicyDecision AgentPolicy::AuthorizeContextDispatch(const AgentContextEnvelope &envelope) {
        if (!Current(envelope.projectRevision))
            return Decision(envelope.provider, StaleAuthority);
        if (!ValidProvider(envelope.provider))
            return Decision(envelope.provider, InvalidRequest);
        if (envelope.provider == AgentProviderResidence::Cloud && !trusted_)
            return Decision(envelope.provider, ProjectUntrusted);
        if (!dispatchGrant_ || dispatchGrant_->provider != envelope.provider || dispatchGrant_->requestId != envelope.requestId ||
            dispatchGrant_->revision != envelope.projectRevision)
            return Decision(envelope.provider, ConsentRequired);
        const ContextGrant grant = std::move(*dispatchGrant_);
        dispatchGrant_.reset();
        if (envelope.items.size() != grant.items.size() || envelope.items.size() > limits_.maximumContextItems)
            return Decision(envelope.provider, StaleAuthority);
        std::size_t totalBytes{};
        std::unordered_set<std::uint64_t> seen;
        for (const AgentContextCandidate &item : envelope.items) {
            if (const auto approved = std::ranges::find_if(grant.items,
                                                           [&item](const ContextFingerprint &fingerprint) {
                return fingerprint.itemId == item.itemId;
            });
                approved == grant.items.end() || !seen.insert(item.itemId).second || !ValidDataClass(item.dataClass) ||
                !item.visibleToUser || IsExcluded(item.dataClass) || (NeedsRedaction(item.dataClass) && !item.redacted) ||
                item.dataClass != approved->dataClass || item.redacted != approved->redacted ||
                DigestText(item.text) != approved->textDigest || item.text.size() > limits_.maximumItemBytes ||
                item.text.size() > limits_.maximumContextBytes - totalBytes)
                return Decision(envelope.provider, StaleAuthority, item.dataClass);
            totalBytes += item.text.size();
        }
        if (totalBytes != envelope.totalBytes)
            return Decision(envelope.provider, StaleAuthority);
        return Decision(envelope.provider, Allowed, std::nullopt, std::nullopt, envelope.requestId);
    }

    /** @copydoc AgentPolicy::EvaluateTool */
    AgentPolicyDecision AgentPolicy::EvaluateTool(const AgentProviderResidence provider, const AgentToolRequest request,
                                                  const std::uint64_t revision) {
        if (!Current(revision))
            return Decision(provider, StaleAuthority, std::nullopt, request.risk, request.operationId, request.proposalRevision);
        if (!ValidProvider(provider) || !ValidToolRisk(request.risk) || request.operationId == 0 || request.proposalRevision == 0)
            return Decision(provider, InvalidRequest, std::nullopt, request.risk, request.operationId, request.proposalRevision);
        if (!trusted_ && (provider == AgentProviderResidence::Cloud || NeedsProjectTrust(request.risk)))
            return Decision(provider, ProjectUntrusted, std::nullopt, request.risk, request.operationId, request.proposalRevision);
        return Decision(provider, request.risk == ReadOnly ? Allowed : ApprovalRequired, std::nullopt, request.risk, request.operationId,
                        request.proposalRevision);
    }

    /** @copydoc AgentPolicy::GrantToolApproval */
    AgentPolicyDecision AgentPolicy::GrantToolApproval(const AgentProviderResidence provider, const AgentToolRequest request,
                                                       const std::uint64_t revision) {
        if (const AgentPolicyDecision evaluation = EvaluateTool(provider, request, revision); evaluation.reason != ApprovalRequired)
            return evaluation.reason == Allowed
                       ? Decision(provider, InvalidRequest, std::nullopt, request.risk, request.operationId, request.proposalRevision)
                       : evaluation;
        if (toolGrant_)
            return Decision(provider, InvalidRequest, std::nullopt, request.risk, request.operationId, request.proposalRevision);
        toolGrant_ = ToolGrant{provider, request, revision};
        return Decision(provider, Allowed, std::nullopt, request.risk, request.operationId, request.proposalRevision);
    }

    /** @copydoc AgentPolicy::DenyTool */
    void AgentPolicy::DenyTool(const std::uint64_t operationId) {
        if (operationId == 0)
            return;
        AgentProviderResidence provider = AgentProviderResidence::Local;
        std::optional<AgentToolRisk> risk;
        std::uint64_t proposalRevision{};
        if (toolGrant_ && toolGrant_->request.operationId == operationId) {
            risk = toolGrant_->request.risk;
            provider = toolGrant_->provider;
            proposalRevision = toolGrant_->request.proposalRevision;
            toolGrant_.reset();
        }
        static_cast<void>(Decision(provider, Denied, std::nullopt, risk, operationId, proposalRevision));
    }

    /** @copydoc AgentPolicy::AuthorizeTool */
    AgentPolicyDecision AgentPolicy::AuthorizeTool(const AgentProviderResidence provider, const AgentToolRequest request,
                                                   const std::uint64_t revision) {
        if (const AgentPolicyDecision evaluation = EvaluateTool(provider, request, revision); evaluation.reason != ApprovalRequired)
            return evaluation;
        if (!toolGrant_)
            return Decision(provider, ApprovalRequired, std::nullopt, request.risk, request.operationId, request.proposalRevision);
        if (toolGrant_->provider != provider || toolGrant_->revision != revision ||
            toolGrant_->request.operationId != request.operationId || toolGrant_->request.proposalRevision != request.proposalRevision ||
            toolGrant_->request.risk != request.risk)
            return Decision(provider, StaleAuthority, std::nullopt, request.risk, request.operationId, request.proposalRevision);
        toolGrant_.reset();
        return Decision(provider, Allowed, std::nullopt, request.risk, request.operationId, request.proposalRevision);
    }

    /** @copydoc AgentPolicy::AuditSnapshot */
    std::vector<AgentPolicyAuditEntry> AgentPolicy::AuditSnapshot() const {
        return {audit_.begin(), audit_.end()};
    }

    /** @copydoc AgentPolicy::ProjectRevision */
    std::uint64_t AgentPolicy::ProjectRevision() const noexcept {
        return revision_;
    }

    /** @brief Appends only typed metadata to the finite in-memory audit ring. */
    AgentPolicyDecision AgentPolicy::Decision(const AgentProviderResidence provider, const AgentPolicyReason reason,
                                              const std::optional<AgentDataClass> dataClass, const std::optional<AgentToolRisk> toolRisk,
                                              const std::uint64_t subjectId, const std::uint64_t proposalRevision) {
        if (sequence_ < std::numeric_limits<std::uint64_t>::max())
            ++sequence_;
        audit_.emplace_back(sequence_, std::chrono::system_clock::now(), revision_, subjectId, proposalRevision, provider, reason,
                            dataClass, toolRisk);
        if (audit_.size() > limits_.maximumAuditEntries)
            audit_.pop_front();
        return {reason, revision_};
    }

    /** @brief Requires an established, exact host project/trust revision. */
    bool AgentPolicy::Current(const std::uint64_t revision) const noexcept {
        return valid_ && revision != 0 && revision == revision_;
    }

    /** @brief Prevents zero or arbitrarily expanded host budgets. */
    bool AgentPolicy::ValidLimits() const noexcept {
        return limits_.maximumContextItems > 0 && limits_.maximumContextItems <= 128 && limits_.maximumItemBytes > 0 &&
               limits_.maximumItemBytes <= (64U << 10U) && limits_.maximumContextBytes > 0 &&
               limits_.maximumContextBytes <= (256U << 10U) && limits_.maximumAuditEntries > 0 && limits_.maximumAuditEntries <= 1024;
    }
}  // namespace Horo::Security
