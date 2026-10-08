#pragma once

/**
 * @file UiElementTree.h
 * @brief Bounded retained Runtime UI tree storage and deferred structural commands.
 */

#include "Horo/Runtime/Ui/UiIdentity.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <variant>
#include <vector>

namespace Horo::Runtime::Ui {
    inline constexpr std::uint32_t MaximumUiTreeElements = 4'096;
    inline constexpr std::uint32_t MaximumUiTreeDepth = 64;
    inline constexpr std::uint32_t MaximumUiStructuralCommands = 1'024;

    /** @brief One authored element and its optional stable parent; input order defines sibling order. */
    struct UiElementDescriptor final {
        UiElementId id;     /**< Stable authored identity. */
        UiElementId parent; /**< Invalid only for the one root descriptor. */
    };

    /** @brief Caller-selected bounds qualified by repository hard ceilings. */
    struct UiElementTreeLimits final {
        std::uint32_t elements{}; /**< Maximum simultaneously resident elements. */
        std::uint32_t depth{};    /**< Maximum root-inclusive depth. */
        std::uint32_t commands{}; /**< Maximum commands in one atomic transaction. */

        /** @brief Checks positive limits against hard ceilings. @return True when every dimension is supported. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Opaque owner-issued non-reusable element-slot reservation for one exact retained tree. */
    class UiElementSlotRange final {
    public:
        /** @brief Returns the first owner-wide element slot. @return Non-zero slot. */
        [[nodiscard]] std::uint32_t FirstSlot() const noexcept;
        /** @brief Returns the number of slots reserved for the tree. @return Positive capacity. */
        [[nodiscard]] std::uint32_t SlotCount() const noexcept;

    private:
        friend class UiElementSlotAllocator;
        friend class UiElementTree;
        struct Authority;
        UiElementSlotRange(std::uint32_t firstSlot, std::uint32_t slotCount, std::shared_ptr<const Authority> authority) noexcept;
        std::uint32_t firstSlot_{};
        std::uint32_t slotCount_{};
        std::shared_ptr<const Authority> authority_;
    };

    /**
     * @brief Move-only owner-wide allocator that monotonically reserves disjoint retained-tree element slots.
     * @details The Runtime UI service owns exactly one allocator per UiOwnershipGeneration. Reserved ranges are burned even when
     *          subsequent tree preparation fails and are never reused before that ownership generation ends.
     */
    class UiElementSlotAllocator final {
    public:
        /**
         * @brief Creates the sole element-slot allocator for one exact owner generation.
         * @param ownership Active owner.
         * @param previousIssuedSlot Owner-retained EVER-reserved high-water mark; zero starts a new ownership namespace.
         * The service must transfer its entire burned history when reinitializing an allocator, never just live slots.
         * @return Allocator or invalid-owner/capacity error; its immutable issuer identity is allocated once at load time.
         * @pre Exactly one active allocator per owner; recreation transfers the authoritative complete issued high-water mark.
         */
        [[nodiscard]] static Result<UiElementSlotAllocator> Create(UiOwnershipGeneration ownership, std::uint32_t previousIssuedSlot = 0);
        /** @brief Transfers the remaining owner-wide slot namespace. @param other Allocator to invalidate and transfer. */
        UiElementSlotAllocator(UiElementSlotAllocator &&other) noexcept;
        UiElementSlotAllocator &operator=(UiElementSlotAllocator &&other) = delete;
        UiElementSlotAllocator(const UiElementSlotAllocator &) = delete;
        UiElementSlotAllocator &operator=(const UiElementSlotAllocator &) = delete;

        /** @brief Returns the exact owner generation. @return Invalid only after move. */
        [[nodiscard]] UiOwnershipGeneration Ownership() const noexcept;

    private:
        friend class UiElementTree;
        friend class UiAnimationOwner;
        explicit UiElementSlotAllocator(UiOwnershipGeneration ownership,
                                        std::shared_ptr<const UiElementSlotRange::Authority> authority) noexcept;
        /** @brief Burns one contiguous range or reports non-wrapping generation exhaustion. */
        [[nodiscard]] Result<UiElementSlotRange> Reserve(std::uint32_t slotCount);
        UiOwnershipGeneration ownership_;
        std::uint64_t nextSlot_{1};
        std::shared_ptr<const UiElementSlotRange::Authority> authority_;
    };

    /** @brief Exact immutable ownership and source-document evidence for one runtime canvas tree. */
    struct UiElementTreeDescriptor final {
        RuntimeUiInstanceId instance;        /**< Exact mutable runtime document instance. */
        UiCanvasInstanceId canvas;           /**< Exact runtime canvas incarnation. */
        UiDocumentId document;               /**< Stable source document identity. */
        UiDocumentRevision documentRevision; /**< Exact source document revision. */
        UiRuntimeTreeRevision treeRevision;  /**< Initial published tree revision. */
        UiElementTreeLimits limits;          /**< Fixed lifetime bounds. */
    };

    /** @brief Typed insertion at an explicit stable child position. */
    struct UiInsertElementCommand final {
        UiElementId id;             /**< New stable authored identity. */
        UiElementHandle parent;     /**< Current exact parent handle. */
        std::uint32_t childIndex{}; /**< Position in [0, current child count]. */
    };

    /** @brief Typed recursive removal of a non-root element. */
    struct UiRemoveElementCommand final {
        UiElementHandle element; /**< Current exact subtree root handle. */
    };

    /** @brief Typed move/reorder of a non-root element. */
    struct UiReparentElementCommand final {
        UiElementHandle element;    /**< Current exact element handle. */
        UiElementHandle parent;     /**< Current exact destination parent. */
        std::uint32_t childIndex{}; /**< Position after detaching from the old parent. */
    };

    /** @brief Closed structural mutation vocabulary evaluated in transaction order. */
    using UiStructuralCommand = std::variant<UiInsertElementCommand, UiRemoveElementCommand, UiReparentElementCommand>;

    /** @brief Owner safe points at which a complete candidate may replace the live tree. */
    enum class UiStructuralCommitPoint : std::uint8_t {
        ApplyQueuedOwnerThreadCommands,
        CommitDeferredLifecycleChanges,
    };

    /** @brief Explicit retained-tree lifecycle; retirement closes mutation admission. */
    enum class UiElementTreeState : std::uint8_t {
        Active,
        Retiring,
        Stopped,
    };

    /** @brief Owned bounded transaction prepared without mutating the retained tree. */
    class UiStructuralCommandBuffer final {
    public:
        /**
         * @brief Prepares an empty exact-owner transaction and reserves its bounded storage.
         * @param instance Exact target runtime instance.
         * @param canvas Exact target runtime canvas.
         * @param documentRevision Exact source document revision expected by the caller.
         * @param treeRevision Exact live tree revision expected by the caller.
         * @param capacity Positive transaction capacity within the hard ceiling.
         * @return Prepared buffer or a typed identity/revision/capacity failure.
         */
        [[nodiscard]] static Result<UiStructuralCommandBuffer> Create(RuntimeUiInstanceId instance, UiCanvasInstanceId canvas,
                                                                      UiDocumentRevision documentRevision,
                                                                      UiRuntimeTreeRevision treeRevision, std::uint32_t capacity);

        /** @brief Adds one owned command without touching the tree. @param command Typed command. @return Success or capacity failure. */
        [[nodiscard]] Result<void> Add(UiStructuralCommand command);
        /** @brief Returns the exact target instance. @return Immutable runtime instance handle. */
        [[nodiscard]] RuntimeUiInstanceId Instance() const noexcept;
        /** @brief Returns the exact target canvas. @return Immutable runtime canvas handle. */
        [[nodiscard]] UiCanvasInstanceId Canvas() const noexcept;
        /** @brief Returns the expected document revision. @return Immutable source revision. */
        [[nodiscard]] UiDocumentRevision DocumentRevision() const noexcept;
        /** @brief Returns the expected live tree revision. @return Immutable tree revision. */
        [[nodiscard]] UiRuntimeTreeRevision TreeRevision() const noexcept;
        /** @brief Returns commands in evaluation order. @return Borrowed immutable bounded commands. */
        [[nodiscard]] std::span<const UiStructuralCommand> Commands() const noexcept;

    private:
        UiStructuralCommandBuffer(RuntimeUiInstanceId instance, UiCanvasInstanceId canvas, UiDocumentRevision documentRevision,
                                  UiRuntimeTreeRevision treeRevision, std::uint32_t capacity);

        RuntimeUiInstanceId instance_;
        UiCanvasInstanceId canvas_;
        UiDocumentRevision documentRevision_;
        UiRuntimeTreeRevision treeRevision_;
        std::uint32_t capacity_{};
        std::vector<UiStructuralCommand> commands_;
    };

    /** @brief One immutable element query result copied from the current tree revision. */
    struct UiElementRecord final {
        UiElementHandle handle; /**< Exact transient runtime handle. */
        UiElementId id;         /**< Stable authored identity. */
        UiElementHandle parent; /**< Invalid only for the root. */
    };

    /** @brief Summary of one atomic safe-point commit. */
    struct UiStructuralCommitResult final {
        UiRuntimeTreeRevision revision;  /**< Current revision; unchanged for an empty transaction. */
        std::uint32_t commandsApplied{}; /**< Number of commands committed atomically. */
        std::uint32_t elementsRemoved{}; /**< Recursive removal count. */
    };

    /**
     * @brief Sole owner of one bounded acyclic retained element tree.
     * @details Query and traversal methods allocate nothing. Structural commands are prepared separately and only a successful
     *          non-empty owner-safe-point commit publishes a new revision. Failed candidates leave all live handles and order intact.
     */
    class UiElementTree final {
    public:
        /**
         * @brief Builds a complete private tree candidate from authored descriptors.
         * @param slotAllocator Exact-owner allocator that burns a disjoint element-slot range for this tree.
         * @param descriptor Exact runtime/canvas/document identity, initial revision and lifetime limits.
         * @param elements Non-empty descriptors; exactly one root and authored sibling order are required.
         * @return Owned active tree or a typed malformed/conflict/capacity failure.
         */
        [[nodiscard]] static Result<UiElementTree> Create(UiElementSlotAllocator &slotAllocator, const UiElementTreeDescriptor &descriptor,
                                                          std::span<const UiElementDescriptor> elements);
        ~UiElementTree();
        UiElementTree(UiElementTree &&) noexcept;
        UiElementTree &operator=(UiElementTree &&) noexcept;
        UiElementTree(const UiElementTree &) = delete;
        UiElementTree &operator=(const UiElementTree &) = delete;

        /**
         * @brief Checks the immutable authority that actually reserved this tree's slot namespace.
         * @param allocator Live or moved-to sole owner allocator.
         * @return True only for the exact issuer; another allocator with equal ownership is rejected.
         * @details The tree retains an immutable issuer pin and never borrows the allocator or its mutable ledger.
         */
        [[nodiscard]] bool WasIssuedBy(const UiElementSlotAllocator &allocator) const noexcept;
        /** @brief Returns lifecycle state. @return Active, Retiring, or Stopped. */
        [[nodiscard]] UiElementTreeState State() const noexcept;
        /** @brief Returns exact target instance. @return Runtime instance identity. */
        [[nodiscard]] RuntimeUiInstanceId Instance() const noexcept;
        /** @brief Returns exact target canvas. @return Runtime canvas identity. */
        [[nodiscard]] UiCanvasInstanceId Canvas() const noexcept;
        /** @brief Returns the stable source document identity. @return Authored document identity. */
        [[nodiscard]] UiDocumentId SourceDocument() const noexcept;
        /** @brief Returns the exact source document revision. @return Revision captured by this tree. */
        [[nodiscard]] UiDocumentRevision SourceDocumentRevision() const noexcept;
        /** @brief Returns current published tree revision. @return Non-zero revision while owned. */
        [[nodiscard]] UiRuntimeTreeRevision Revision() const noexcept;
        /** @brief Returns resident element count. @return Bounded current count. */
        [[nodiscard]] std::uint32_t Size() const noexcept;
        /** @brief Returns the allocator-issued namespace, including every reserved unused/tombstoned slot.
         * @return Immutable range while retained, or a lifecycle failure after shutdown.
         */
        [[nodiscard]] Result<UiElementSlotRange> ReservedSlots() const;
        /** @brief Returns the exact root. @return Root record or lifecycle failure after shutdown. */
        [[nodiscard]] Result<UiElementRecord> Root() const;
        /** @brief Resolves a stable authored identity in the current generation. @param id Stable identity. @return Exact handle. */
        [[nodiscard]] Result<UiElementHandle> Find(UiElementId id) const;
        /** @brief Reads one exact resident handle. @param handle Runtime handle. @return Copied current record. */
        [[nodiscard]] Result<UiElementRecord> Get(UiElementHandle handle) const;
        /**
         * @brief Copies deterministic authored-order children without allocation.
         * @param parent Exact parent handle.
         * @param output Caller-owned capacity.
         * @return Child count or capacity/handle failure; output is unchanged on failure.
         */
        [[nodiscard]] Result<std::size_t> Children(UiElementHandle parent, std::span<UiElementHandle> output) const;
        /**
         * @brief Copies root-first depth-first order without allocation.
         * @param output Caller-owned capacity.
         * @return Element count or capacity/lifecycle failure; output is unchanged on failure.
         */
        [[nodiscard]] Result<std::size_t> Preorder(std::span<UiElementHandle> output) const;
        /**
         * @brief Atomically publishes one complete candidate at an owner safe point.
         * @param buffer Exact bounded transaction; evaluated in command order.
         * @param point Declared ADR-073 owner safe point.
         * @return Commit summary; empty buffers succeed without advancing the revision.
         */
        [[nodiscard]] Result<UiStructuralCommitResult> CommitDeferred(const UiStructuralCommandBuffer &buffer,
                                                                      UiStructuralCommitPoint point);
        /** @brief Closes structural admission while preserving read access for retirement. @return Success or lifecycle failure. */
        [[nodiscard]] Result<void> BeginRetirement();
        /** @brief Idempotently destroys retained contents after external snapshots/leases are drained. */
        void Shutdown() noexcept;

    private:
        friend class UiStyleResolver;
        friend class UiLayoutEngine;
        friend class UiLayoutClipEngine;
        struct Storage;
        /** @brief Pins the actual issuer of this burned range for same-target publication admission. */
        [[nodiscard]] std::shared_ptr<const void> IssuerPin() const noexcept;
        explicit UiElementTree(std::unique_ptr<Storage> state) noexcept;
        /** @brief Validates lifecycle, safe point, target identities, revisions, and batch bounds before candidate allocation. */
        [[nodiscard]] Result<void> ValidateCommitRequest(const UiStructuralCommandBuffer &buffer, UiStructuralCommitPoint point) const;
        /** @brief Applies a validated non-empty batch to a detached candidate and atomically publishes it. */
        [[nodiscard]] Result<UiStructuralCommitResult> PublishCandidate(const UiStructuralCommandBuffer &buffer,
                                                                        UiRuntimeTreeRevision nextRevision);
        std::unique_ptr<Storage> state_;
    };
}  // namespace Horo::Runtime::Ui
