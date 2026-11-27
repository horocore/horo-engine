#pragma once

/** @file AgentContext.h @brief Exact, bounded editor context previews and provider dispatch admission. */

#include "Horo/Agent/ModelProvider.h"
#include "Horo/Application/ProjectCodeQuery.h"
#include "Horo/Security/AgentPolicy.h"

#include <span>
#include <variant>

namespace Horo::Agent {
    /** @brief Explicit selection of one file, byte range, or read-only editor observation. */
    enum class AgentContextKind : std::uint8_t {
        File,
        SourceRange,
        Diagnostic,
        BuildStatus,
        BuildOutput,
        Selection,
        Note,
        Report
    };

    /** @brief Capture request for a file or a range in its exact content revision. */
    struct AgentContextTextRequest final {
        AgentContextKind kind{AgentContextKind::File};
        std::string path;
        std::string revision;
        std::size_t offset{};
        std::size_t length{}; /**< Nonzero byte count for a source range. Zero for a complete file. */
    };

    /** @brief Explicit dynamic project evidence requested through the host's read-only MCP route. */
    struct AgentContextQueryRequest final {
        std::string revision;
        Application::CodeQueryRequest query;
    };

    /** @brief Classification of a host-owned text attachment. Binary and credential attachments are unsupported. */
    enum class AgentContextAttachmentKind : std::uint8_t {
        Selection,
        BuildOutput,
        BuildDiagnostics,
        Note,
        Report
    };

    /** @brief Immutable caller-selected evidence captured by a host UI owner at a known revision. */
    struct AgentContextAttachment final {
        AgentContextAttachmentKind kind{AgentContextAttachmentKind::Note};
        std::string origin;
        std::string revision;
        std::string text;
        bool truncated{}; /**< This selected projection explicitly omits source evidence. */
    };

    /** @brief One explicitly selected source, in the order displayed to the user. */
    using AgentContextSelection = std::variant<AgentContextTextRequest, AgentContextQueryRequest, AgentContextAttachment>;

    /** @brief Exact origin and producer revision for a displayed context item and its freshness lookup. */
    struct AgentContextProvenance final {
        AgentContextKind kind{};
        std::string origin;
        std::string revision;
        std::size_t offset{};
        std::size_t capturedBytes{};
        bool truncated{}; /**< The query explicitly reported omitted rows or producer retention loss. */
    };

    /** @brief Typed projection and the exact user-role bytes that would reach the selected provider. */
    struct AgentContextPreviewItem final {
        AgentContextProvenance provenance;
        Security::AgentContextCandidate candidate;
    };

    /** @brief Only explicit, synchronous read/redaction capabilities; callbacks retain no call arguments. */
    struct AgentContextReaders final {
        std::function<Result<std::optional<Platform::ProjectReadTextSnapshot>>(std::string_view, const Platform::ProjectReadContext &)>
            existingText;                           /**< Same callback as the existing-source document reader, never opens a document. */
        const Platform::IProjectReadFiles *files{}; /**< Optional bounded no-follow file authority, borrowed during capture only. */
        std::function<Result<Application::CodeQueryPage>(const Application::CodeQueryRequest &, const Application::CodeQueryContext &)>
            readOnlyQuery; /**< Selected read-only MCP projection, invoked only for explicit Diagnostics/BuildStatus queries. */
        std::function<Result<std::string>(const AgentContextProvenance &, std::string_view)> redact; /**< Redacts the complete payload. */
        std::function<bool(const AgentContextProvenance &)> isCurrent; /**< Compares live revisions without I/O or opening documents. */
    };

    /** @brief Immutable host-owned preview bound to one request, provider residence and trust revision. */
    class AgentContextPreview final {
    public:
        /** @brief Captures only selected sources and verifies finite text, path, freshness and redaction before publishing.
         * @param selection Exact explicitly chosen context in display order.
         * @param readers Trusted borrowed read and redaction capabilities for this capture.
         * @param context Project identity, cancellation, deadline and lifetime authority for the read.
         * @param provider Exact selected provider's host-confirmed residence.
         * @param requestId Nonzero host request identity.
         * @param trustRevision Current Security policy revision captured by the host.
         * @return Fully owned preview or typed rejection without any partial publication. */
        [[nodiscard]] static Result<AgentContextPreview> Capture(std::span<const AgentContextSelection> selection,
                                                                 const AgentContextReaders &readers,
                                                                 const Platform::ProjectReadContext &context,
                                                                 Security::AgentProviderResidence provider, std::uint64_t requestId,
                                                                 std::uint64_t trustRevision);

        /** @brief Observes the immutable display/dispatch bytes. @return Items in user-selected order. */
        [[nodiscard]] std::span<const AgentContextPreviewItem> Items() const noexcept {
            return items_;
        }

        /** @brief Observes the destination that must be shown at confirmation. @return Provider residence. */
        [[nodiscard]] Security::AgentProviderResidence Provider() const noexcept {
            return provider_;
        }

        /** @brief Records consent only after the host displays these exact items and receives explicit confirmation.
         * @param policy Active host trust and consent policy. @return Exact policy decision. */
        [[nodiscard]] Security::AgentPolicyDecision Grant(Security::AgentPolicy &policy) const;

        /** @brief Rechecks project/trust and each exact producer revision without source reads.
         * @param policy Active trust policy on this owner lane.
         * @param context Current project authority.
         * @param isCurrent Owner-lane revision observer with no read or mutation capability.
         * @return Whether the preview's entire provenance remains current. */
        [[nodiscard]] bool IsCurrent(const Security::AgentPolicy &policy, const Platform::ProjectReadContext &context,
                                     const std::function<bool(const AgentContextProvenance &)> &isCurrent) const;

        /** @brief Consumes that consent and inserts the exact preview bytes into one outgoing request before dispatch.
         * @param policy Active host trust and consent policy.
         * @param context Current project fence; its authority is checked without any source re-read.
         * @param isCurrent Owner-lane revision lookup for each exact origin, required for dispatch.
         * @param provider Actual selected destination residence confirmed by host composition.
         * @param requestId Exact identity of the imminent request.
         * @param request Outgoing conversation whose final message is the user's prompt.
         * @return Policy decision; any failure leaves the outgoing request intact. */
        [[nodiscard]] Security::AgentPolicyDecision InsertBeforeDispatch(
            Security::AgentPolicy &policy, const Platform::ProjectReadContext &context,
            const std::function<bool(const AgentContextProvenance &)> &isCurrent, Security::AgentProviderResidence provider,
            std::uint64_t requestId, ModelRequest &request) const;

    private:
        AgentContextPreview() = default;

        std::vector<AgentContextPreviewItem> items_;
        std::string project_;
        std::uint64_t generation_{};
        std::uint64_t requestId_{};
        std::uint64_t trustRevision_{};
        Security::AgentProviderResidence provider_{};
    };
}  // namespace Horo::Agent
