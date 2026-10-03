#pragma once

/**
 * @file AgentPolicy.h
 * @brief Host-owned, fail-closed agent context and tool authorization policy.
 */

#include "Horo/Foundation/Sha256.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Horo::Security {
    /** @brief Provider residence; local inference does not imply tool authority. */
    enum class AgentProviderResidence {
        Local,
        Cloud
    };

    /** @brief Data classes requiring separate per-request admission. */
    enum class AgentDataClass {
        EditorContext,
        SceneData,
        ProjectFile,
        AssetMetadata,
        ToolResult,
        ProposedChange,
        Transcript,
        SensitiveProjectData,
        Credential,
        RawSensorData,
    };

    /** @brief Risk of the host-resolved operation, independent of model/provider choice. */
    enum class AgentToolRisk {
        ReadOnly,
        Mutation,
        Destructive,
        Execution,
        CredentialAccess,
        NetworkAccess
    };

    /** @brief Stable, payload-free reason available to a UI or audit projection. */
    enum class AgentPolicyReason {
        Allowed,
        InvalidRequest,
        ProjectUntrusted,
        ConsentRequired,
        ApprovalRequired,
        Denied,
        StaleAuthority,
        SensitiveDataExcluded,
        ContextNotVisible,
        ContextNotAdmitted,
        ContextNotRedacted,
        ContextLimitExceeded,
    };

    /** @brief Host-classified candidate; text is never retained by the policy. */
    struct AgentContextCandidate {
        std::uint64_t itemId{};
        AgentDataClass dataClass{AgentDataClass::EditorContext};
        std::string text;
        bool visibleToUser{};
        bool redacted{};
    };

    /** @brief Ephemeral provider-bound context copied only after policy admission. */
    struct AgentContextEnvelope {
        AgentProviderResidence provider{AgentProviderResidence::Local};
        std::uint64_t requestId{};
        std::vector<AgentContextCandidate> items;
        std::size_t totalBytes{};
        std::uint64_t projectRevision{};
    };

    /** @brief Host-resolved operation identity and proposal revision. */
    struct AgentToolRequest {
        std::uint64_t operationId{};
        std::uint64_t proposalRevision{};
        AgentToolRisk risk{AgentToolRisk::ReadOnly};
    };

    /** @brief Bounded, content-free policy decision for user inspection. */
    struct AgentPolicyDecision {
        AgentPolicyReason reason{AgentPolicyReason::InvalidRequest};
        std::uint64_t projectRevision{};

        [[nodiscard]] bool Allowed() const noexcept {
            return reason == AgentPolicyReason::Allowed;
        }
    };

    /** @brief Content-free audit event; does not store context, arguments, tool names, or credentials. */
    struct AgentPolicyAuditEntry {
        std::uint64_t sequence{};
        std::chrono::system_clock::time_point timestamp;
        std::uint64_t projectRevision{};
        std::uint64_t subjectId{};
        std::uint64_t proposalRevision{};
        AgentProviderResidence provider{AgentProviderResidence::Local};
        AgentPolicyReason reason{AgentPolicyReason::InvalidRequest};
        std::optional<AgentDataClass> dataClass;
        std::optional<AgentToolRisk> toolRisk;
    };

    /** @brief Finite context and audit budgets; invalid limits keep the policy closed. */
    struct AgentPolicyLimits {
        std::size_t maximumContextItems{16};
        std::size_t maximumItemBytes{4U << 10U};
        std::size_t maximumContextBytes{16U << 10U};
        std::size_t maximumAuditEntries{128};
    };

    /**
     * @brief Single-host-thread policy boundary; host UI alone records consent and approval.
     *
     * The model and provider receive only Evaluate/Build/Authorize results, never a mutable
     * reference to this object. The host must recheck authorization immediately before
     * dispatch and must not cache an Allowed result across project trust changes.
     */
    class AgentPolicy final {
    public:
        /** @brief Creates a policy with finite limits; invalid limits fail closed. @param limits Host budgets. */
        explicit AgentPolicy(const AgentPolicyLimits &limits = {}) noexcept;
        AgentPolicy(const AgentPolicy &) = delete;
        AgentPolicy &operator=(const AgentPolicy &) = delete;
        AgentPolicy(AgentPolicy &&) = delete;
        AgentPolicy &operator=(AgentPolicy &&) = delete;

        /**
         * @brief Replaces project trust and revokes all outstanding consent and approvals.
         * @param trusted Whether the host currently trusts the active project.
         * @param revision Strictly increasing host project/trust revision.
         * @return False for stale/zero revision; state then remains unchanged.
         */
        [[nodiscard]] bool SetProjectTrust(bool trusted, std::uint64_t revision) noexcept;

        /**
         * @brief Records visible per-request context consent from the host UI.
         * @param provider Exact destination provider residence disclosed in the preview.
         * @param requestId Nonzero request identity.
         * @param visibleItems Exact selected preview values; only their hashes and classification are retained.
         * @param revision Current project/trust revision.
         * @return Decision; no context bytes are retained.
         */
        [[nodiscard]] AgentPolicyDecision GrantContextConsent(AgentProviderResidence provider, std::uint64_t requestId,
                                                              const std::vector<AgentContextCandidate> &visibleItems,
                                                              std::uint64_t revision);

        /** @brief Explicitly denies and revokes a pending context grant. @param requestId Request identity. */
        void DenyContext(std::uint64_t requestId);

        /**
         * @brief Consumes one request grant and copies only visible, classified, bounded, admitted data.
         * @param provider Destination provider residence.
         * @param requestId Nonzero request identity.
         * @param candidates Host-classified candidate values; never stored by the policy.
         * @param revision Current project/trust revision.
         * @return Accepted ephemeral envelope and decision; failure leaves an empty envelope.
         */
        [[nodiscard]] std::pair<AgentContextEnvelope, AgentPolicyDecision> BuildContext(
            AgentProviderResidence provider, std::uint64_t requestId, const std::vector<AgentContextCandidate> &candidates,
            std::uint64_t revision);

        /**
         * @brief Consumes exact context authority immediately before synchronous provider dispatch.
         * @param envelope Previously built envelope; contents must still match the visible preview.
         * @return Decision; project trust replacement or a modified envelope fails closed.
         */
        [[nodiscard]] AgentPolicyDecision AuthorizeContextDispatch(const AgentContextEnvelope &envelope);

        /**
         * @brief Evaluates tool authority without granting it.
         * @param provider Local or cloud model; both use the same approval rules.
         * @param request Host-resolved operation and risk.
         * @param revision Current project/trust revision.
         * @return Decision requiring approval for every non-read-only risk.
         */
        [[nodiscard]] AgentPolicyDecision EvaluateTool(AgentProviderResidence provider, AgentToolRequest request, std::uint64_t revision);

        /**
         * @brief Records a visible, exact-operation user approval supplied by the host UI.
         * @param provider Provider residence shown with the proposal.
         * @param request Approved operation and proposal revision.
         * @param revision Current project/trust revision.
         * @return Decision; approval is single-use and never broadens an untrusted project.
         */
        [[nodiscard]] AgentPolicyDecision GrantToolApproval(AgentProviderResidence provider, AgentToolRequest request,
                                                            std::uint64_t revision);

        /** @brief Explicitly denies and revokes an operation approval. @param operationId Operation identity. */
        void DenyTool(std::uint64_t operationId);

        /**
         * @brief Consumes exact approval immediately before tool dispatch.
         * @param provider Local or cloud model.
         * @param request Host-resolved operation and risk.
         * @param revision Current project/trust revision.
         * @return Decision; no model output can approve itself.
         */
        [[nodiscard]] AgentPolicyDecision AuthorizeTool(AgentProviderResidence provider, AgentToolRequest request, std::uint64_t revision);

        /** @brief Returns bounded content-free decision history. @return Snapshot in sequence order. */
        [[nodiscard]] std::vector<AgentPolicyAuditEntry> AuditSnapshot() const;

        /** @brief Returns current project/trust revision. @return Zero before first trust assignment. */
        [[nodiscard]] std::uint64_t ProjectRevision() const noexcept;

    private:
        struct ContextFingerprint {
            std::uint64_t itemId{};
            AgentDataClass dataClass{AgentDataClass::EditorContext};
            Sha256Digest textDigest;
            bool redacted{};
        };

        struct ContextGrant {
            AgentProviderResidence provider{AgentProviderResidence::Local};
            std::uint64_t requestId{};
            std::uint64_t revision{};
            std::vector<ContextFingerprint> items;
        };

        struct ToolGrant {
            AgentProviderResidence provider{AgentProviderResidence::Local};
            AgentToolRequest request;
            std::uint64_t revision{};
        };

        /** @brief Assembles an exact-preview dispatch envelope after one-request consent is consumed. */
        [[nodiscard]] std::pair<AgentContextEnvelope, AgentPolicyDecision> AssembleAdmittedContext(
            ContextGrant grant, const std::vector<AgentContextCandidate> &candidates);
        [[nodiscard]] AgentPolicyDecision Decision(AgentProviderResidence provider, AgentPolicyReason reason,
                                                   std::optional<AgentDataClass> dataClass = std::nullopt,
                                                   std::optional<AgentToolRisk> toolRisk = std::nullopt, std::uint64_t subjectId = 0,
                                                   std::uint64_t proposalRevision = 0);
        [[nodiscard]] bool Current(std::uint64_t revision) const noexcept;
        [[nodiscard]] bool ValidLimits() const noexcept;
        AgentPolicyLimits limits_;
        bool valid_{};
        bool trusted_{};
        std::uint64_t revision_{};
        std::uint64_t sequence_{};
        std::optional<ContextGrant> contextGrant_;
        std::optional<ContextGrant> dispatchGrant_;
        std::optional<ToolGrant> toolGrant_;
        std::deque<AgentPolicyAuditEntry> audit_;
    };
}  // namespace Horo::Security
