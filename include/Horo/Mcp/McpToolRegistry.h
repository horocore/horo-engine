#pragma once

/**
 * @file McpToolRegistry.h
 * @brief Immutable MCP tool metadata, capability-filtered discovery, and bounded schema admission.
 */

#include "Horo/Mcp/McpSession.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <vector>

namespace Horo::Mcp {
    /** @brief Stable, transport-neutral identity for one application tool. */
    struct McpToolId final {
        std::string value;
        [[nodiscard]] bool operator==(const McpToolId &) const = default;
    };

    /** @brief Independent version of a tool's input and output contract. */
    struct McpToolVersion final {
        std::uint16_t major{1};
        std::uint16_t minor{};
        std::uint16_t patch{};
        [[nodiscard]] auto operator<=>(const McpToolVersion &) const = default;
    };

    /** @brief Closed effect category; authentication and approval are later host policy. */
    enum class McpToolEffect : std::uint8_t {
        Query,
        PresentationSideEffect,
        Mutation
    };

    /** @brief Finite per-tool budgets within the enclosing MCP session limits. */
    struct McpToolBounds final {
        std::size_t maximumInputBytes{64U << 10U};
        std::size_t maximumResultBytes{64U << 10U};
        std::size_t maximumDepth{24};
        std::size_t maximumNodes{2048};
    };

    /** @brief Owned, inert descriptor with no provider or transport dependencies. */
    struct McpToolDescriptor final {
        McpToolId id;
        McpToolVersion version;
        std::string description;
        nlohmann::json inputSchema;
        nlohmann::json outputSchema;
        McpToolEffect effect{McpToolEffect::Query};
        std::vector<std::string> requiredCapabilities;
        McpToolBounds bounds;
    };

    /** @brief Injected application adapter; later owner-thread policy may implement this seam. */
    class IMcpToolAdapter {
    public:
        virtual ~IMcpToolAdapter() = default;
        /** @brief Runs an already admitted tool call. @param arguments Schema-validated input. @param context Session authority and stop
         * view.
         * @return Application outcome, without protocol serialization. */
        [[nodiscard]] virtual Result<nlohmann::json> Invoke(const nlohmann::json &arguments, const McpRequestContext &context) = 0;

        /** @brief Starts a tool without retaining an owner thread for slow application work.
         * @param arguments Schema-validated input. @param context Operation authority and progress callback.
         * @param complete Exactly-once completion callback, safe to invoke from a host worker.
         * @note Default implementation invokes the synchronous adapter on the owner thread.
         * Async implementations retain complete, a copy of context, and their application-owner lease until work
         * finishes. They must observe context.IsStopRequested() at bounded cooperative checkpoints and before committing
         * deferred work; the cancellation token alone does not observe credential expiry. Invoke complete exactly once, including after
         * cancellation. A bounded controller drain timeout does not terminate application-owned jobs or processes. */
        virtual void InvokeAsync(const nlohmann::json &arguments, const McpRequestContext &context,
                                 const std::function<void(Result<nlohmann::json>)> &complete);
    };

    /** @brief Host execution context that owns an adapter's application capability. */
    enum class McpOwnerContext : std::uint8_t {
        Editor,
        Runtime,
        Background,
        Build,
        Unspecified
    };

    /** @brief One host-authored descriptor and its lifetime-owned application adapter. */
    struct McpToolRegistration final {
        McpToolDescriptor descriptor;
        std::shared_ptr<IMcpToolAdapter> adapter;
        McpOwnerContext owner{McpOwnerContext::Unspecified};
    };

    /** @brief Immutable published registry generation retained by active readers. */
    class McpToolSnapshot final {
        struct ConstructionKey final {
        private:
            friend class McpToolRegistry;
            ConstructionKey() = default;

        public:
            ConstructionKey(const ConstructionKey &) = default;
        };

    public:
        /** @brief Internal construction seam used only by the registry's atomic publication. */
        explicit McpToolSnapshot(ConstructionKey, std::uint64_t generation, std::vector<McpToolRegistration> entries,
                                 std::shared_ptr<McpAuthorization> authorization);
        /** @brief Returns the monotonically increasing publication generation. */
        [[nodiscard]] std::uint64_t Generation() const noexcept;
        /** @brief Lists descriptors in stable ID order, filtered by the host-approved grant set.
         * @param capabilities Immutable session capability identities. @return Owned discovery descriptors. */
        [[nodiscard]] std::vector<McpToolDescriptor> Discover(std::span<const std::string> capabilities) const;
        /** @brief Validates grant, input bounds and schema before invoking the registered adapter.
         * @details Requires this snapshot's host policy and an authenticated principal from that issuer.
         * Admission consumes exact approval atomically; subsequent revocation cancels cooperatively and
         * prevents publication of late results, rather than undoing committed application effects.
         * @param id Stable tool identity. @param arguments Decoded input. @param context Immutable session authority.
         * @return Bounded, output-schema-validated result or typed failure. */
        [[nodiscard]] Result<nlohmann::json> Invoke(const McpToolId &id, const nlohmann::json &arguments,
                                                    const McpRequestContext &context) const;
        /** @brief Validates and starts one adapter on its owner; completion may arrive later.
         * @details Enforces the same issuer and admission contract as Invoke. Free-text progress is omitted;
         * terminal results are revalidated against live authority before the exactly-once callback.
         * @param id Tool identity. @param arguments Decoded input. @param context Session authority.
         * @param complete Exactly-once terminal callback. */
        void InvokeAsync(const McpToolId &id, const nlohmann::json &arguments, const McpRequestContext &context,
                         std::function<void(Result<nlohmann::json>)> complete) const;
        /** @brief Resolves the host-declared owner after checking session grants.
         * @param id Tool identity. @param capabilities Host-approved session grants.
         * @return Owner or typed absence/denial. */
        [[nodiscard]] Result<McpOwnerContext> Owner(const McpToolId &id, std::span<const std::string> capabilities) const;
        /** @brief Resolves effect only after checking capability grants. @param id Tool identity.
         * @param capabilities Host-approved grants. @return Exact immutable effect or typed denial. */
        [[nodiscard]] Result<McpToolEffect> Effect(const McpToolId &id, std::span<const std::string> capabilities) const;

    private:
        friend class McpToolRegistry;
        std::uint64_t generation_{};
        std::vector<McpToolRegistration> entries_;
        std::shared_ptr<McpAuthorization> authorization_;
    };

    /** @brief Explicit host publication point; failed candidates leave the current snapshot unchanged. */
    class McpToolRegistry final {
    public:
        /** @brief Binds every published snapshot to one immutable host policy identity.
         * @param authorization Host policy; a metadata-only registry without one cannot invoke adapters. */
        explicit McpToolRegistry(std::shared_ptr<McpAuthorization> authorization = {});
        /** @brief Checks composition policy identity without exposing mutable policy access.
         * @param authorization Proposed controller policy. @return True only for this registry's non-null policy. */
        [[nodiscard]] bool UsesAuthorization(const std::shared_ptr<McpAuthorization> &authorization) const noexcept;
        /** @brief Validates and atomically publishes a complete replacement table.
         * @param entries Host-owned candidate registrations.
         * @param availableCapabilities Capabilities the host can supply in this composition.
         * @return New generation or a typed rejection. */
        [[nodiscard]] Result<std::uint64_t> Publish(std::vector<McpToolRegistration> entries,
                                                    std::span<const std::string> availableCapabilities = {});
        /** @brief Retains one immutable generation across later replacement or shutdown. */
        [[nodiscard]] std::shared_ptr<const McpToolSnapshot> Read() const;

    private:
        mutable std::mutex mutex_;
        std::shared_ptr<const McpToolSnapshot> current_;
        const std::shared_ptr<McpAuthorization> authorization_;
    };
}  // namespace Horo::Mcp
