#pragma once

/** @file EditorPlaySaveRetirement.h
 * @brief Cold-bounded owner-lane retirement of the existing Play unit through actual save settlement.
 */
#include "EditorPlaySaveSession.h"
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Result.h"

#include <memory>
#include <thread>
#include <vector>

namespace Horo::Gameplay {
    class BehaviorRuntime;
    class BehaviorRuntimeSceneOwner;
}  // namespace Horo::Gameplay

namespace Horo::Editor {
    class EditorPlaySceneHost;
    class ProjectGameplayRegistry;

    /** @brief Shared owner-lane lookup slot; Scene callbacks never retain a controller address. */
    struct EditorPlayRuntimeBinding final {
        Gameplay::BehaviorRuntime *runtime{};
        std::weak_ptr<Gameplay::BehaviorRuntimeSceneOwner> owner; /**< Scene-owned lookup, never a strong runtime cycle. */
        EditorPlaySaveSession *save{};              /**< Owner-lane borrow revoked before Scene transfer or controller destruction. */
        std::optional<PlaySaveCommand> saveCommand; /**< One bounded UI intent; controller drains at its owner safe boundary. */
        std::optional<Error> saveError;
        bool revoked{};
    };

    /** @brief Actual moved Play ownership; field teardown keeps borrowed gameplay registry alive after behavior destruction. */
    struct EditorPlayRetiredUnit final {
        EditorPlayRetiredUnit();
        ~EditorPlayRetiredUnit();
        EditorPlayRetiredUnit(EditorPlayRetiredUnit &&) noexcept;
        EditorPlayRetiredUnit &operator=(EditorPlayRetiredUnit &&) noexcept;
        EditorPlayRetiredUnit(const EditorPlayRetiredUnit &) = delete;
        EditorPlayRetiredUnit &operator=(const EditorPlayRetiredUnit &) = delete;

        std::unique_ptr<ProjectGameplayRegistry> registry;
        std::unique_ptr<EditorPlaySceneHost> scene;
        std::shared_ptr<Gameplay::BehaviorRuntimeSceneOwner> behaviorOwner;
        std::shared_ptr<EditorPlayRuntimeBinding> binding;
        CancellationToken cancellation;
        std::uint64_t frame{};

        /** @brief Pumps only cancelled Scene/save ownership without running gameplay or waiting. @return Actual settlement. */
        [[nodiscard]] Result<bool> Poll();

    private:
        /** @brief Keeps exact Scene/registry owners until actual native retirement finishes. */
        [[nodiscard]] Result<bool> PollNativeRetirement();
        bool sceneClosed_{};
    };

    /** @brief Explicit application-owned deferred lifetime storage, not another Scene or save-operation authority.
     * @details Capacity and physical slots are allocated cold. A reservation is acquired before Play activation; transferring
     * the exact unit during leave is allocation-free and cannot fail. Poll/Close run only on the creating thread. Close is
     * called after the actual application scheduler joins, before module retirement/providers disappear. No jobs capture this queue.
     */
    class EditorPlaySaveRetirement final : public std::enable_shared_from_this<EditorPlaySaveRetirement> {
    public:
        /** @brief Owns one already-reserved physical slot; destruction releases unused admission without touching a retired unit. */
        class Reservation final {
        public:
            Reservation() = default;
            ~Reservation();
            Reservation(Reservation &&other) noexcept;
            Reservation &operator=(Reservation &&other) noexcept;
            Reservation(const Reservation &) = delete;
            Reservation &operator=(const Reservation &) = delete;
            /** @brief Reports whether the owner reserved actual bounded retirement capacity. */
            [[nodiscard]] bool IsValid() const noexcept;
            /** @brief Moves the real revoked unit into its preallocated slot without allocation or dropping ownership. */
            void Retain(EditorPlayRetiredUnit unit) noexcept;

        private:
            friend class EditorPlaySaveRetirement;
            Reservation(std::shared_ptr<EditorPlaySaveRetirement> owner, std::size_t index) noexcept;
            std::shared_ptr<EditorPlaySaveRetirement> owner_;
            std::size_t index_{};
        };

        /** @brief Allocates the fixed four-slot application default cold. @throws std::bad_alloc on allocation failure. */
        EditorPlaySaveRetirement();
        /** @brief Allocates finite owner storage cold. @param capacity Positive ceiling, at most eight retired/reserved units.
         * @return Actual queue or typed invalid/capacity/allocation failure. */
        [[nodiscard]] static Result<std::shared_ptr<EditorPlaySaveRetirement>> Create(std::size_t capacity = 4);
        /** @brief Reserves physical space before activation/navigation. @return Owner reservation or typed pressure/closed error. */
        [[nodiscard]] Result<Reservation> Reserve();
        /** @brief Pumps each occupied slot once without waiting. @return Number actually reclaimed or original failure. */
        [[nodiscard]] Result<std::size_t> Poll();
        /** @brief Closes admission and polls already-joined work; pending ownership remains retained on failure. */
        [[nodiscard]] Result<void> Close();
        /** @brief Reports real deferred units, for shutdown ordering only; does not manufacture worker completion. */
        [[nodiscard]] bool HasPending() const noexcept;
        ~EditorPlaySaveRetirement();

    private:
        /** @brief Constructs physical slots only after factory bounds validation. */
        explicit EditorPlaySaveRetirement(std::size_t capacity);
        /** @brief Physical admission state; closing admission never invalidates an already reserved transfer. */
        enum class SlotState {
            Free,
            Reserved,
            Retiring
        };

        /** @brief Preallocated storage whose retiring unit cannot be reused until actual settlement. */
        struct Slot final {
            SlotState state{SlotState::Free};
            EditorPlayRetiredUnit unit;
        };

        /** @brief Releases only unused owner-lane admission, preserving any actual retained unit. */
        void Release(std::size_t index) noexcept;
        std::thread::id owner_{std::this_thread::get_id()};
        std::vector<Slot> slots_;
        bool closed_{};
    };

    /** @brief Drains already-joined Scene cancellation in bounded owner phases before module providers disappear.
     * @param owner Explicit application queue or nullptr. @details Never joins or declares a worker complete. A genuine
     * unsettled/error outcome exhausts this fail-closed process teardown boundary instead of unloading borrowed code. */
    void ClosePlaySaveRetirementAfterJobs(EditorPlaySaveRetirement *owner) noexcept;
}  // namespace Horo::Editor
