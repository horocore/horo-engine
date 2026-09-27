#pragma once

/**
 * @file McpToolRegistry.h
 * @brief Immutable MCP tool metadata, capability-filtered discovery, and bounded schema admission.
 */

#include "Horo/Mcp/McpSession.h"

#include <compare>
#include <cstddef>
#include <cstdint>
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
    };

    /** @brief One host-authored descriptor and its lifetime-owned application adapter. */
    struct McpToolRegistration final {
        McpToolDescriptor descriptor;
        std::shared_ptr<IMcpToolAdapter> adapter;
    };

    /** @brief Immutable published registry generation retained by active readers. */
    class McpToolSnapshot final {
    public:
        /** @brief Returns the monotonically increasing publication generation. */
        [[nodiscard]] std::uint64_t Generation() const noexcept;
        /** @brief Lists descriptors in stable ID order, filtered by the host-approved grant set.
         * @param capabilities Immutable session capability identities. @return Owned discovery descriptors. */
        [[nodiscard]] std::vector<McpToolDescriptor> Discover(std::span<const std::string> capabilities) const;
        /** @brief Validates grant, input bounds and schema before invoking the registered adapter.
         * @param id Stable tool identity. @param arguments Decoded input. @param context Immutable session authority.
         * @return Bounded, output-schema-validated result or typed failure. */
        [[nodiscard]] Result<nlohmann::json> Invoke(const McpToolId &id, const nlohmann::json &arguments,
                                                    const McpRequestContext &context) const;

    private:
        friend class McpToolRegistry;
        explicit McpToolSnapshot(std::uint64_t generation, std::vector<McpToolRegistration> entries);
        std::uint64_t generation_{};
        std::vector<McpToolRegistration> entries_;
    };

    /** @brief Explicit host publication point; failed candidates leave the current snapshot unchanged. */
    class McpToolRegistry final {
    public:
        McpToolRegistry();
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
    };
}  // namespace Horo::Mcp
