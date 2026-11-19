#pragma once

/** @file NetworkAuthoredScenePage.h @brief Bounded wire pages and ordered assembly for authored Scene identities. */

#include "Horo/Network/ReplicationWorldLifecycle.h"

namespace Horo::Network {
    inline constexpr std::size_t MaximumNetworkAuthoredSceneObjects = ReplicationWorldLimits{}.maximumObjects;
    inline constexpr std::size_t MaximumNetworkAuthoredScenePageObjects = 32;
    inline constexpr std::size_t MinimumNetworkAuthoredScenePageBytes = 26;
    inline constexpr std::size_t NetworkAuthoredSceneRowBytes = 20;
    inline constexpr std::size_t MaximumNetworkAuthoredScenePageBytes =
        MinimumNetworkAuthoredScenePageBytes + NetworkAuthoredSceneRowBytes * MaximumNetworkAuthoredScenePageObjects;

    /** @brief One contiguous portion of the authority's canonical authored inventory. */
    struct NetworkAuthoredScenePage final {
        NetworkSessionGeneration session;
        ReplicationAuthorityEpoch authority;
        std::size_t totalObjects{};
        std::size_t firstIndex{}; /**< Zero-based index in the full inventory. */
        std::vector<ReplicationAuthoredObjectBinding> objects;

        bool operator==(const NetworkAuthoredScenePage &) const noexcept = default;
    };

    /**
     * @brief Copies a canonical page for a sender-selected payload capacity.
     * @param inventory Complete authored inventory captured from the authority world.
     * @param firstIndex First unsent index. Use zero for the single empty-inventory page.
     * @param maximumObjects Maximum rows allowed by the selected transport, within the protocol page limit.
     * @return One owned page or a typed framing or capacity error.
     */
    [[nodiscard]] Result<NetworkAuthoredScenePage> MakeNetworkAuthoredScenePage(
        const ReplicationAuthoredSceneInventory &inventory, std::size_t firstIndex,
        std::size_t maximumObjects = MaximumNetworkAuthoredScenePageObjects);

    /**
     * @brief Encodes one complete big-endian authored inventory page.
     * @param page Exact scoped page with monotonically ordered Scene provenance.
     * @return Canonical bytes or a typed framing or capacity error.
     */
    [[nodiscard]] Result<std::vector<std::byte>> EncodeNetworkAuthoredScenePage(const NetworkAuthoredScenePage &page);

    /**
     * @brief Reads one complete page without accepting trailing bytes.
     * @param bytes Exact envelope payload borrowed for the duration of decoding.
     * @return Owned page or a typed framing or capacity error.
     */
    [[nodiscard]] Result<NetworkAuthoredScenePage> DecodeNetworkAuthoredScenePage(std::span<const std::byte> bytes);

    /** @brief Single-owner, one-shot assembly scoped to an already authenticated world handoff. */
    class NetworkAuthoredSceneAssembly final {
    public:
        /**
         * @brief Starts an empty assembly for an authority world selected by the caller.
         * @param session Exact bound world session.
         * @param authority Exact bound authority epoch.
         */
        NetworkAuthoredSceneAssembly(NetworkSessionGeneration session, ReplicationAuthorityEpoch authority) noexcept
            : inventory_{session, authority, {}} {}

        NetworkAuthoredSceneAssembly(const NetworkAuthoredSceneAssembly &) = delete;
        NetworkAuthoredSceneAssembly &operator=(const NetworkAuthoredSceneAssembly &) = delete;

        /**
         * @brief Admits exactly the next page, preserving existing staged rows on failure.
         * @param page Decoded page from the authenticated authority control route.
         * @return Success or a typed duplicate, ordering, stale-world or malformed error.
         */
        [[nodiscard]] Result<void> Append(const NetworkAuthoredScenePage &page);

        /** @brief Reports whether the complete inventory is ready for transfer. */
        [[nodiscard]] bool IsComplete() const noexcept;

        /** @brief Transfers the complete inventory once, or returns a typed incomplete-assembly error. */
        [[nodiscard]] Result<ReplicationAuthoredSceneInventory> Take();

    private:
        ReplicationAuthoredSceneInventory inventory_;
        std::optional<std::size_t> expectedRows_;
    };
}  // namespace Horo::Network
