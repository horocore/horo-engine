#pragma once

/** @file OverlayLayout.h @brief Inert development-panel registration and bounded presentation layout/focus state. */
#include "Horo/Debug/CommandRegistry.h"

#include <array>
#include <thread>

namespace Horo::Debug {
    /** @brief The owning snapshot family; registration does not start a sampler or query a service. */
    enum class OverlaySource : std::uint8_t {
        Logs,
        Frame,
        Cpu,
        Memory,
        Jobs,
        Renderer,
        Assets,
        Scene,
        Input,
        Network
    };

    /** @brief Finite logical-viewport fractions; typography/style remain runtime UI responsibilities. */
    struct OverlayBounds final {
        float x{}, y{}, width{0.3F}, height{0.3F};
    };

    /** @brief Inert host contribution. Optional sources are contributed only when their real capability exists. */
    struct OverlayDescriptor final {
        std::uint64_t id{};
        std::string_view name;
        OverlaySource source{};
        CommandAvailability availability{CommandAvailability::NonShipping};
        CommandPermission permission{CommandPermission::Developer};
    };

    /** @brief Bounded paint/focus projection; owns no metric, log, operation or runtime state. */
    struct OverlayPanelSnapshot final {
        std::uint64_t id{};
        OverlaySource source{};
        OverlayBounds bounds;
        std::uint8_t focusOrder{};
    };

    /** @brief Fixed-capacity owner-thread presentation state, independent of subsystem snapshot production. */
    class OverlayLayout final {
    public:
        OverlayLayout(const OverlayLayout &) = delete;
        OverlayLayout &operator=(const OverlayLayout &) = delete;
        OverlayLayout(OverlayLayout &&) noexcept = default;
        OverlayLayout &operator=(OverlayLayout &&) noexcept = default;
        /** @brief Atomically validates registration before publishing any panel.
         * @param descriptors Explicit available sources; names and IDs must be unique.
         * @param policy Fixed product profile; Shipping and Server reject visual panels.
         * @return Inert bounded layout or typed composition failure. No source callback is invoked.
         */
        [[nodiscard]] static Result<OverlayLayout> Create(std::span<const OverlayDescriptor> descriptors, RegistryPolicy policy);
        /** @brief Applies one panel's normalized layout and visibility without allocation.
         * @param id Registered identity. @param bounds Finite contained viewport fractions.
         * @param visible Requested visibility. @param principal Current host principal.
         * @return Success or typed bounds/permission/identity failure without mutation.
         */
        [[nodiscard]] Result<void> Configure(std::uint64_t id, OverlayBounds bounds, bool visible, const DebugCommandContext &principal);
        /** @brief Brings a visible allowed panel to the front with stable relative order for peers.
         * @param id Registered identity. @param principal Current host principal. @return Success or typed failure.
         */
        [[nodiscard]] Result<void> Focus(std::uint64_t id, const DebugCommandContext &principal);
        /** @brief Copies only allowed visible projections into caller-owned frame storage.
         * @param principal Current host principal. @param output Caller-provided finite storage.
         * @return Written panel count or typed capacity failure; no partial output is published on failure.
         */
        [[nodiscard]] Result<std::size_t> Snapshot(const DebugCommandContext &principal, std::span<OverlayPanelSnapshot> output) const;

    private:
        OverlayLayout() = default;

        struct Panel final {
            std::uint64_t id{};
            std::string name;
            OverlaySource source{};
            CommandPermission permission{};
            OverlayBounds bounds;
            bool visible{};
            std::uint8_t order{};
        };

        std::array<Panel, 32> panels_;
        std::size_t count_{};
        ProductProfile profile_{};
        std::thread::id owner_{std::this_thread::get_id()};
    };
}  // namespace Horo::Debug
