#pragma once

/**
 * @file UiBindingStore.h
 * @brief Owner-thread versioned binding publication and exact retained-target invalidation.
 */

#include "Horo/Runtime/Ui/UiBinding.h"
#include "Horo/Runtime/Ui/UiLayout.h"

#include <memory>

namespace Horo::Runtime::Ui {
    struct UiBindingProviderInstanceTag;
    struct UiBindingSnapshotRevisionTag;
    struct UiBindingUpdateRevisionTag;
    /** @brief Host-issued exact provider incarnation; never serialized or reused within its owner generation. */
    using UiBindingProviderInstanceId = UiRuntimeHandle<UiBindingProviderInstanceTag>;
    /** @brief Monotonic committed provider snapshot revision. */
    using UiBindingSnapshotRevision = UiRevision<UiBindingSnapshotRevisionTag>;
    /** @brief Monotonic publication revision of one retained binding store. */
    using UiBindingUpdateRevision = UiRevision<UiBindingUpdateRevisionTag>;

    inline constexpr std::size_t MaximumUiBindingProviders = 64;
    inline constexpr std::size_t MaximumUiBindingChanges = 1'024;
    inline constexpr std::size_t MaximumUiBindingStorageBytes = 4U << 20U;
    inline constexpr std::size_t MaximumUiBindingChangeBytes = 1U << 20U;

    /** @brief One owned typed property value borrowed only during synchronous publication. */
    struct UiBindingPropertyUpdate final {
        std::uint16_t property{}; /**< Zero-based slot in the exact identity-sorted provider schema. */
        UiBindingValue value;
    };

    /** @brief Exact owner-admitted provider with copied metadata and an initial coherent snapshot. */
    struct UiBindingProviderRegistration final {
        UiBindingProviderInstanceId instance;
        UiBindingProviderScopeKind scope{UiBindingProviderScopeKind::GameInstance};
        const UiBindingProviderSchema *schema{}; /**< Borrowed only by Create; no contributor pointer survives. */
        UiBindingSnapshotRevision revision;
        std::span<const UiBindingPropertyUpdate> values; /**< Strictly increasing slots; absent optional targets use fallback. */
    };

    /** @brief Host-resolved binding; no nearest-provider lookup or scope inference occurs. */
    struct UiResolvedBindingDescriptor final {
        UiBindingProviderInstanceId provider;
        UiBindingDescriptor binding;
    };

    /** @brief Delta from one coherent provider revision to a newer revision. */
    struct UiBindingChangeBatch final {
        UiBindingProviderInstanceId provider;
        UiBindingSchemaVersion schema;
        UiBindingSnapshotRevision expected;
        UiBindingSnapshotRevision revision;
        std::span<const UiBindingPropertyUpdate> changes; /**< Strictly increasing slots; borrowed until Apply returns. */
    };

    /** @brief Independent downstream work categories for one exact semantic target. */
    enum class UiBindingDirty : std::uint8_t {
        None = 0,
        Layout = 1U << 0U,
        Paint = 1U << 1U,
        Accessibility = 1U << 2U,
        Actions = 1U << 3U,
    };

    /**
     * @brief Combines independent downstream work.
     * @param left First set of dirty categories.
     * @param right Additional dirty categories.
     * @return Union of both category sets.
     */
    [[nodiscard]] constexpr UiBindingDirty operator|(const UiBindingDirty left, const UiBindingDirty right) noexcept {
        return static_cast<UiBindingDirty>(static_cast<unsigned>(left) | static_cast<unsigned>(right));
    }

    /**
     * @brief Tests downstream work without changing target state.
     * @param flags Dirty categories to inspect.
     * @param flag Category mask to test.
     * @return True when any category in flag is present in flags.
     */
    [[nodiscard]] constexpr bool HasFlag(const UiBindingDirty flags, const UiBindingDirty flag) noexcept {
        return (static_cast<unsigned>(flags) & static_cast<unsigned>(flag)) != 0;
    }

    /** @brief Explicit source availability; an unavailable required binding has no readable stale value. */
    enum class UiBindingValueOrigin : std::uint8_t {
        Provider,
        Fallback,
        Unavailable
    };

    /** @brief One retained typed target, borrowed only until the next store mutation or destruction. */
    struct UiBoundTarget final {
        UiBindingId binding;
        UiElementHandle element;
        UiBindingTargetProperty property{};
        UiBindingValue value;
        UiBindingValueOrigin origin{UiBindingValueOrigin::Unavailable};
    };

    /** @brief Exact downstream notification; copied before layout/render/action consumers run. */
    struct UiBindingTargetDirty final {
        UiBindingId binding;
        UiElementHandle element;
        UiBindingTargetProperty property{};
        UiBindingDirty dirty{UiBindingDirty::None};
        UiBindingUpdateRevision revision;
        UiRuntimeTreeRevision tree;
    };

    /** @brief Explicit load-time and per-update capacity limits. */
    struct UiBindingStoreLimits final {
        std::size_t providers{MaximumUiBindingProviders};
        std::size_t bindings{MaximumUiBindingDescriptors};
        std::size_t changes{MaximumUiBindingChanges};
        std::size_t valueBytes{MaximumUiBindingStorageBytes}; /**< Aggregate target and fallback text reservation budget. */
        std::size_t changeBytes{MaximumUiBindingChangeBytes}; /**< Aggregate variable-sized input bytes per Apply. */
    };

    /** @brief Result of atomic binding publication, including required source loss on unregister. */
    struct UiBindingApplyResult final {
        UiBindingUpdateRevision revision;
        UiLayoutContentRevision content;
        std::size_t targetsChanged{};
        std::size_t requiredUnavailable{};
    };

    /**
     * @brief Sole mutable binding owner for one exact retained canvas generation.
     * @details Create is load-time and copies schema, descriptors and initial values. Apply and Unregister run only on the Runtime UI
     * owner thread at the VariableUpdate snapshot cutoff, before layout/input/extraction consumers. They validate the entire transaction,
     * then atomically enqueue precise layout invalidations and copy target values into preallocated storage. No provider callback, I/O,
     * polling, lock or allocation occurs on successful frame-hot calls. Empty updates do no target work.
     * Target borrows cannot escape a synchronous owner phase; immutable text/layout/render snapshots own their derived copies.
     * Reload/structural replacement prepares a new store against the replacement tree; stale batches never reconcile by slot alone.
     * Direct read and TwoWay read projections are supported. TargetToSource and converters require separate write/conversion capability
     * and fail explicitly at preparation; descriptor metadata never grants execution authority.
     */
    class UiBindingStore final {
    public:
        /**
         * @brief Prepares complete target state against one active retained tree.
         * @param tree Exact active canvas; no tree pointer is retained.
         * @param providers Exact host-resolved provider registrations; all spans are copied or consumed before return.
         * @param bindings Conflict-free resolved descriptors; required initial values must be present.
         * @param limits Hard-bounded lifetime capacities.
         * @return Complete private candidate or typed identity, schema, availability, value or capacity failure.
         */
        [[nodiscard]] static Result<UiBindingStore> Create(const UiElementTree &tree,
                                                           std::span<const UiBindingProviderRegistration> providers,
                                                           std::span<const UiResolvedBindingDescriptor> bindings,
                                                           const UiBindingStoreLimits &limits = {});
        /** @brief Releases all Horo-owned metadata and target storage without calling external owners. */
        ~UiBindingStore();
        /** @brief Transfers unique binding ownership and invalidates other. @param other Store to transfer. */
        UiBindingStore(UiBindingStore &&other) noexcept;
        /** @brief Replaces this owner with other. @param other Store to transfer. @return This store. */
        UiBindingStore &operator=(UiBindingStore &&other) noexcept;
        UiBindingStore(const UiBindingStore &) = delete;
        UiBindingStore &operator=(const UiBindingStore &) = delete;

        /**
         * @brief Atomically consumes bounded deltas and queues exact measure invalidations in the existing layout owner.
         * @param tree Exact tree captured at creation; replacement revisions reject old batches.
         * @param batches At most one batch per provider, strictly increasing provider identity, with increasing property slots.
         * @param layout Exact canvas layout owner. Capacity/lifecycle failure leaves values, revisions and queued invalidations unchanged.
         * @return Publication summary or typed stale/schema/conflict/value/capacity/lifecycle failure.
         * @post Equal values advance provider evidence but do not dirty or rebuild targets. Content advances only for layout work.
         */
        [[nodiscard]] Result<UiBindingApplyResult> Apply(const UiElementTree &tree, std::span<const UiBindingChangeBatch> batches,
                                                         UiLayoutEngine &layout);

        /**
         * @brief Atomically revokes a provider, removes readable required values and publishes optional fallbacks.
         * @param tree Exact live tree.
         * @param provider Exact provider incarnation to close permanently in this store.
         * @param layout Exact layout owner receiving affected-target invalidations.
         * @return Publication summary; repeated unregister succeeds without work. Failure leaves the registration intact for retry.
         * @pre The host stops provider admission/producers before unregister and retires required UI on requiredUnavailable.
         */
        [[nodiscard]] Result<UiBindingApplyResult> Unregister(const UiElementTree &tree, UiBindingProviderInstanceId provider,
                                                              UiLayoutEngine &layout);

        /**
         * @brief Reads retained target state without calling the provider.
         * @param tree Exact active tree; structural replacement, reload and retirement invalidate borrows.
         * @param binding Stable binding identity.
         * @return Synchronous borrow, or nullptr for absent/required-unavailable targets or after retirement.
         */
        [[nodiscard]] const UiBoundTarget *Find(const UiElementTree &tree, UiBindingId binding) const noexcept;
        /**
         * @brief Copies and acknowledges all accumulated downstream dirties in descriptor order, without allocation.
         * @param output Caller-owned storage; insufficient capacity leaves output and pending work unchanged.
         * @return Copied count or capacity/lifecycle failure. Layout work was already queued by publication.
         */
        [[nodiscard]] Result<std::size_t> DrainDirty(std::span<UiBindingTargetDirty> output);
        /** @brief Returns current publication/content evidence without polling providers. @return Latest committed summary. */
        [[nodiscard]] UiBindingApplyResult Current() const noexcept;
        /** @brief Closes admission and target borrows; existing immutable downstream snapshots remain owned by their consumers. */
        void BeginRetirement() noexcept;
        /** @brief Idempotently releases owned metadata and target storage; no provider or downstream owner is called. */
        void Shutdown() noexcept;

    private:
        struct Storage;
        /** @brief Adopts a fully prepared private binding candidate. @param storage Unique candidate ownership. */
        explicit UiBindingStore(std::unique_ptr<Storage> storage) noexcept;
        std::unique_ptr<Storage> storage_;
    };
}  // namespace Horo::Runtime::Ui
