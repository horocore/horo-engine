#pragma once

/** @file ConsoleSceneOverlayInternal.h @brief Non-installed bounded display projection over existing weak Scene views. */
#include "Horo/Debug/ConsoleBuiltins.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"

#include <array>
#include <chrono>

namespace Horo::Debug::Internal {
    /** @brief Copied public identities only; no entity/component/storage borrow survives refresh. */
    struct ConsoleSceneOverlayRow final {
        Runtime::EntityRef entity;
        std::optional<Runtime::EntityRef> parent;
        bool operator==(const ConsoleSceneOverlayRow &) const = default;
    };

    /** @brief Owner-thread, four-Hz read-only display projection; hidden instances never query the supplied Scene.
     * Fixed storage holds at most 256 copied entity/parent identities. The authoritative Scene alone owns topology and lifetime.
     */
    class ConsoleSceneOverlay final {
    public:
        /** @brief Creates inert bounded inspection state; acquires no Scene view and starts no service.
         * @param profile Actual composed product profile. @param principal Explicit existing host grants.
         * @param limits At most 4096 visited slots and 256 displayed entities, both positive.
         * @return Projection or typed profile/permission/limit failure. Only Editor/Development Developer inspection is admitted.
         */
        [[nodiscard]] static Result<ConsoleSceneOverlay> Create(ProductProfile profile, const DebugCommandContext &principal,
                                                                ConsoleSceneInspectionLimits limits = {});
        /** @brief Copies an existing weak view without querying Scene storage; null clears presentation immediately.
         * @param view Explicit current host publication, or null when absent. @return Success or wrong-owner failure before mutation.
         */
        [[nodiscard]] Result<void> Publish(const Runtime::RuntimeSceneView *view);
        /** @brief Changes actual panel visibility; hiding drops rows, identity and refresh cadence without querying Scene.
         * @param visible Actual admitted presentation, including suspension/focus gates. @return Success or wrong-owner failure.
         */
        [[nodiscard]] Result<void> SetVisible(bool visible);
        /** @brief Refreshes visible rows at most four Hz, with bounded slot scanning and no heap allocation.
         * @param now Host monotonic time. @return Whether copied presentation changed, or wrong-owner failure before querying Scene.
         * Absent, retired, foreign-thread or structurally stale views publish unavailable state, never retained component pointers.
         */
        [[nodiscard]] Result<bool> Refresh(std::chrono::steady_clock::time_point now);
        /** @brief Borrows copied display identities. @return At most 256 rows, invalidated by publication/refresh/hide/destruction.
         * @pre Query on this projection's owner thread.
         */
        [[nodiscard]] std::span<const ConsoleSceneOverlayRow> Rows() const noexcept;
        /** @brief Reports whether the last admitted refresh observed a current Scene, including an empty Scene. */
        [[nodiscard]] bool Available() const noexcept;
        /** @brief Reports whether the slot/row budget omitted possible entities from the last available refresh. */
        [[nodiscard]] bool Truncated() const noexcept;
        /** @brief Returns copied runtime identity, or zero while unavailable/hidden. */
        [[nodiscard]] Runtime::SceneRuntimeId RuntimeId() const noexcept;
        /** @brief Returns a mutation identity for native selection invalidation; unchanged refreshes preserve it. */
        [[nodiscard]] std::uint64_t Revision() const noexcept;

    private:
        /** @brief Captures owner identity without consulting Scene state. */
        explicit ConsoleSceneOverlay(ConsoleSceneInspectionLimits limits);
        /** @brief Drops presentation immediately without accessing the retained weak view. @return Whether presentation changed. */
        bool Clear() noexcept;
        ConsoleSceneInspectionLimits limits_;
        std::thread::id owner_;
        std::optional<Runtime::RuntimeSceneView> view_;
        std::array<ConsoleSceneOverlayRow, 256> rows_{};
        std::size_t count_{};
        Runtime::SceneRuntimeId runtime_{};
        std::uint64_t structuralRevision_{}, revision_{};
        std::optional<std::chrono::steady_clock::time_point> nextRefresh_;
        bool visible_{}, available_{}, truncated_{};
    };
}  // namespace Horo::Debug::Internal
