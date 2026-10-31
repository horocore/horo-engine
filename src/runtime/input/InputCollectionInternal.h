#pragma once

#include "Horo/Runtime/InputCollection.h"

#include <array>
#include <bitset>
#include <thread>

namespace Horo::Input {
    /** @brief Lifecycle-allocated weak-borrow anchor and fixed collection state; InputRouter remains its sole owner.
     * Shared ownership protects only anchor lifetime, never router access. All state access is owner-thread-only.
     * Foreign-thread destruction leaves a bounded registration/cutoff reserved instead of touching owner state.
     */
    struct InputCollectionLifetime final {
        struct Value final {
            float x{};
            float y{};
            bool active{};
        };

        struct Source final {
            std::uint64_t generation{};
            std::uint64_t context{};
            std::uint64_t configuration{};
            std::uint64_t assignment{};
            std::optional<PlayerId> player;
            std::optional<GamepadDeviceId> device;
            std::size_t count{};
            bool retiring{};
            bool neutralCommitted{};
        };

        struct Entry final {
            std::uint64_t generation{};
            std::size_t source{};
            std::size_t local{};
            std::size_t action{};
            ActionValueType type{};
            Value pending;
            Value current;
            Value previous;
            bool appended{};
            bool consumed{};
        };

        explicit InputCollectionLifetime(InputRouter &owner) : router(&owner) {}

        [[nodiscard]] Result<InputRouter *> Borrow() const;
        [[nodiscard]] Result<void> ValidateSource(std::size_t slot, std::uint64_t generation) const;
        [[nodiscard]] Result<void> ValidateFrame(std::uint64_t frame) const;
        [[nodiscard]] Result<void> Revoke(std::size_t slot, std::uint64_t generation);
        [[nodiscard]] Result<std::array<std::size_t, MaximumCollectedInputActions>> ResolveActions(
            std::uint64_t context, std::span<const ActionDescriptor> actions) const;
        [[nodiscard]] Result<std::pair<std::size_t, std::uint64_t>> Register(Source source, std::span<const ActionDescriptor> actions);
        void AbandonSource(std::size_t slot, std::uint64_t generation) noexcept;
        void AbandonFrame(std::uint64_t frame) noexcept;
        void Reconfigure() noexcept;
        void ReclaimNeutral() noexcept;
        void CommitEmpty() noexcept;
        void CommitValues(bool focused) noexcept;
        void Accumulate(std::size_t action, std::optional<PlayerId> player, ActionValue &value) noexcept;
        InputRouter *router;
        const std::thread::id thread{std::this_thread::get_id()};
        std::array<Source, MaximumCollectedInputSources> sources{};
        std::array<Entry, MaximumCollectedInputActions> entries{};
        std::uint64_t nextGeneration{1};
        std::uint64_t serial{};
        const RawInputSnapshot *candidate{};
        bool collecting{};
        bool routingAllowed{true};
    };
}  // namespace Horo::Input
