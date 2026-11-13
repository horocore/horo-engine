#pragma once

/** @file AndroidWindowHost.h @brief Noninstalled native-window ownership shared only by Android host composition and its selected renderer.
 */
#include "Horo/Platform/AndroidLifecycle.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace Horo::Platform::Android {
    class AndroidLifecycleAdapter;
    class AndroidWindowHost;
    struct AndroidWindowHostState;

    /** @brief Explicit host profile; headless never admits a native presentation lease. */
    enum class AndroidWindowMode : std::uint8_t {
        Headless,
        Interactive
    };

    /** @brief Committed native pixel extent; zero dimensions describe a nondrawable window. */
    struct AndroidWindowExtent final {
        std::uint32_t width{};
        std::uint32_t height{};
        [[nodiscard]] constexpr bool operator==(const AndroidWindowExtent &) const noexcept = default;
    };

    /** @brief Derived native facts; Activity/window identities are borrowed from the sole lifecycle authority. */
    struct AndroidWindowSnapshot final {
        AndroidActivityGeneration activity;
        AndroidWindowGeneration window;
        AndroidWindowExtent extent;
        std::uint64_t revision{}; /**< Nonwrapping native-facts revision; never a presentation-success receipt. */
        bool available{};         /**< Exact occurrence is committed, not lost/closed; zero extent can still be nondrawable. */
    };

    /** @brief Trusted native operations; default interactive composition selects actual NDK operations in the implementation. */
    struct AndroidWindowNativeOperations final {
        void *context{};
        void (*acquire)(void *context, void *window) noexcept {};
        void (*release)(void *context, void *window) noexcept {};
        AndroidWindowExtent (*extent)(void *context, void *window) noexcept {};
    };

    /** @brief Typed private window admission and lifetime failures, independent of renderer errors. */
    namespace AndroidWindowErrors {
        extern const ErrorCodeDescriptor InvalidInput;
        extern const ErrorCodeDescriptor WrongThread;
        extern const ErrorCodeDescriptor Unsupported;
        extern const ErrorCodeDescriptor Headless;
        extern const ErrorCodeDescriptor Closed;
        extern const ErrorCodeDescriptor Pending;
        extern const ErrorCodeDescriptor StaleGeneration;
        extern const ErrorCodeDescriptor CapacityExceeded;
        extern const ErrorCodeDescriptor RevisionExhausted;
        extern const ErrorCodeDescriptor AllocationFailed;
    }  // namespace AndroidWindowErrors

    /** @brief Move-only allocation-free retention of one genuine native-window occurrence and facts revision.
     * @details Host, lease and renderer retirement run on the same serialized owner lane. The host outlives every lease.
     * Logical loss/replacement immediately makes IsCurrent false, but the retained native reference survives until the
     * renderer releases the lease after its genuine GPU/presentation retirement proof. No destructor waits for a GPU.
     */
    class AndroidWindowLease final {
    public:
        /** @brief Creates an empty lease, containing no native handle. */
        AndroidWindowLease() noexcept = default;
        /** @brief Releases this exact occurrence's reference count on its owner lane. */
        ~AndroidWindowLease();
        AndroidWindowLease(const AndroidWindowLease &) = delete;
        AndroidWindowLease &operator=(const AndroidWindowLease &) = delete;
        /** @brief Transfers retained ownership, leaving the source empty. @param other Exact retained occurrence. */
        AndroidWindowLease(AndroidWindowLease &&other) noexcept;
        /** @brief Releases this lease then transfers ownership. @param other Exact retained occurrence. @return This lease. */
        AndroidWindowLease &operator=(AndroidWindowLease &&other) noexcept;
        /** @brief Returns facts captured at admission. @return Immutable copied extent and authentic lifecycle generations. */
        [[nodiscard]] AndroidWindowSnapshot Snapshot() const noexcept;
        /** @brief Checks exact current occurrence/revision on the owner lane. @return False for empty, lost, resized, replaced or closed.
         */
        [[nodiscard]] bool IsCurrent() const noexcept;
        /** @brief Borrows retained opaque NDK storage, even during retirement. @return Native handle or null for an empty lease.
         * @details Only Android renderer implementation may interpret this pointer; new surface creation additionally requires IsCurrent.
         */
        [[nodiscard]] void *NativeHandle() const noexcept;

    private:
        friend class AndroidWindowHost;
        AndroidWindowLease(AndroidWindowHostState &state, std::size_t slot, AndroidWindowSnapshot snapshot) noexcept;
        void Reset() noexcept;
        AndroidWindowHostState *state_{};
        std::size_t slot_{};
        AndroidWindowSnapshot snapshot_{};
    };

    /** @brief Bounded owner of actual NDK references; existing AndroidLifecycleController remains the identity/state authority.
     * @details Create performs two bounded cold allocations. Observations, refresh, snapshots and lease moves perform no heap allocation
     * on success. At most eight retained native occurrences and 64 simultaneous leases per occurrence are admitted.
     * Duplicate availability for the same live pointer/Activity is idempotent. Loss then availability of the same pointer
     * creates a fresh lifecycle generation, never reactivates the old lease. Call Close and retire all leases before destruction.
     */
    class AndroidWindowHost final {
    public:
        static constexpr std::size_t MaximumOccurrences = 8;
        static constexpr std::size_t MaximumLeasesPerOccurrence = 64;
        /** @brief Creates one private window owner.
         * @param lifecycle Existing lifecycle authority, retained by composition for the owner's complete lifetime.
         * @param adapter Its exact generation-issuing adapter, retained for the same lifetime.
         * @param mode Explicit interactive/headless profile.
         * @param operations Trusted native/test operations; empty selects NDK on Android, otherwise typed unsupported.
         * @return Owner or typed invalid, affinity or unavailable-native failure. No renderer is selected here.
         */
        [[nodiscard]] static Result<std::unique_ptr<AndroidWindowHost>> Create(AndroidLifecycleController &lifecycle,
                                                                               AndroidLifecycleAdapter &adapter, AndroidWindowMode mode,
                                                                               AndroidWindowNativeOperations operations = {});
        /** @brief Releases unleased native references; live leases at destruction violate the explicit ownership contract. */
        ~AndroidWindowHost();
        AndroidWindowHost(const AndroidWindowHost &) = delete;
        AndroidWindowHost &operator=(const AndroidWindowHost &) = delete;
        /** @brief Retains a callback's actual native occurrence and queues its authentic lifecycle identity.
         * @param activity Already committed current Activity generation.
         * @param nativeWindow Actual callback-borrowed native window, valid until this call returns.
         * @return Assigned or duplicate generation, or typed failure. Failed current-Activity replacement denies old-window acquisition;
         * stale Activity observations cannot revoke a replacement.
         * @post A new occurrence cannot be leased until lifecycle.Drain and Refresh observe its actual committed generation.
         */
        [[nodiscard]] Result<AndroidWindowGeneration> ObserveAvailable(AndroidActivityGeneration activity, void *nativeWindow);
        /** @brief Stops new acquisition and queues loss of exactly one occurrence. @param window Captured occurrence identity.
         * @return Success or typed stale/closed/affinity failure; a stale loss cannot retire a replacement.
         */
        [[nodiscard]] Result<void> ObserveLost(AndroidWindowGeneration window);
        /** @brief Refreshes native extent/currentness from the authentic lifecycle snapshot after its owner-thread Drain.
         * @return Success or typed affinity/revision failure; no presentation success is inferred.
         */
        [[nodiscard]] Result<void> Refresh();
        /** @brief Reads derived facts from the existing lifecycle publication. @return Copied current/unavailable facts or affinity error.
         */
        [[nodiscard]] Result<AndroidWindowSnapshot> Snapshot() const;
        /** @brief Retains only a committed, drawable, exact current occurrence. @param window Expected genuine lifecycle generation.
         * @return Move-only lease or typed headless, pending, stale, capacity or closed failure.
         */
        [[nodiscard]] Result<AndroidWindowLease> Acquire(AndroidWindowGeneration window);
        /** @brief Closes acquisition and retires unleased references without waiting. @return True only when every renderer lease has
         * retired. */
        [[nodiscard]] Result<bool> Close();

    private:
        explicit AndroidWindowHost(std::unique_ptr<AndroidWindowHostState> state) noexcept;
        std::unique_ptr<AndroidWindowHostState> state_;
    };
}  // namespace Horo::Platform::Android
