#pragma once

/**
 * @file UiAccessibility.h
 * @brief Typed, bounded Runtime UI accessibility semantics and immutable snapshots.
 */

#include "Horo/Runtime/Ui/UiLayout.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace Horo::Runtime::Ui {
    inline constexpr std::uint32_t CurrentUiAccessibilitySchemaVersion = 1;
    inline constexpr std::uint32_t MaximumUiAccessibilityNodes = 4'096;
    inline constexpr std::uint32_t MaximumUiAccessibilityRelations = 8'192;
    inline constexpr std::uint32_t MaximumUiAccessibilityActions = 8'192;
    inline constexpr std::uint32_t MaximumUiAccessibilityTextBytes = 1'048'576;
    inline constexpr std::uint32_t MaximumUiAccessibilitySnapshotsInFlight = 64;
    inline constexpr std::uint32_t NoUiAccessibilityIndex = static_cast<std::uint32_t>(-1);

    struct UiAccessibilityNodeHandleTag;
    struct UiAccessibilitySemanticRevisionTag;
    /** @brief Generation-checked identity of one semantic node in an exact Runtime UI incarnation. */
    using UiAccessibilityNodeId = UiRuntimeHandle<UiAccessibilityNodeHandleTag>;
    /** @brief Monotonic semantic publication generation for one Runtime UI owner. */
    using UiAccessibilitySemanticRevision = UiRevision<UiAccessibilitySemanticRevisionTag>;

    /** @brief Closed role vocabulary shared by core and contributed controls. */
    enum class UiAccessibilityRole : std::uint8_t {
        Application,
        Window,
        Screen,
        Dialog,
        Alert,
        Group,
        Heading,
        StaticText,
        Button,
        Toggle,
        Checkbox,
        CheckBox = Checkbox,
        Radio,
        Slider,
        TextField,
        TextInput = TextField,
        Link,
        Image,
        Progress,
        ProgressBar = Progress,
        List,
        ListItem,
        Menu,
        MenuItem,
        Tab,
        TabItem,
        Tree,
        TreeItem,
        Table,
        Row,
        Cell,
        ScrollView,
    };

    /** @brief Closed source vocabulary distinguishing built-in and package-contributed controls. */
    enum class UiAccessibilityControlSource : std::uint8_t {
        Core,
        Contributed,
    };

    /** @brief Closed text provenance; localization is resolved before Runtime UI publication. */
    enum class UiAccessibilityTextSource : std::uint8_t {
        ResolvedMessage,
        UserContent,
    };

    /** @brief Closed exposure policy for one semantic node. */
    enum class UiAccessibilityExposure : std::uint8_t {
        Visible,
        Offscreen,
        Hidden,
        Covered,
        Suppressed,
        Suspended,
    };

    /** @brief Closed semantic state flags validated against the node role and value model. */
    enum class UiAccessibilityStateFlag : std::uint32_t {
        None = 0,
        Disabled = 1U << 0U,
        Focusable = 1U << 1U,
        Focused = 1U << 2U,
        Checked = 1U << 3U,
        Pressed = 1U << 4U,
        Selected = 1U << 5U,
        Expanded = 1U << 6U,
        ReadOnly = 1U << 7U,
        Editable = 1U << 8U,
        Required = 1U << 9U,
        Invalid = 1U << 10U,
        Busy = 1U << 11U,
        Modal = 1U << 12U,
        MultiSelectable = 1U << 13U,
        Visited = 1U << 14U,
        HasPopup = 1U << 15U,
    };
    using UiAccessibilityStateMask = std::uint32_t;

    /** @brief Combines two accessibility state flags without introducing untyped bitfields. */
    [[nodiscard]] constexpr UiAccessibilityStateMask operator|(UiAccessibilityStateFlag left, UiAccessibilityStateFlag right) noexcept {
        return static_cast<UiAccessibilityStateMask>(left) | static_cast<UiAccessibilityStateMask>(right);
    }

    /** @brief Combines an existing state mask with one accessibility state flag. */
    [[nodiscard]] constexpr UiAccessibilityStateMask operator|(UiAccessibilityStateMask left, UiAccessibilityStateFlag right) noexcept {
        return left | static_cast<UiAccessibilityStateMask>(right);
    }

    /** @brief Owned typed state mask with role-validation helpers. */
    struct UiAccessibilityState final {
        UiAccessibilityStateMask flags{};

        constexpr UiAccessibilityState() noexcept = default;

        constexpr UiAccessibilityState(const UiAccessibilityStateFlag flag) noexcept : flags(static_cast<UiAccessibilityStateMask>(flag)) {}

        constexpr UiAccessibilityState(const UiAccessibilityStateMask value) noexcept : flags(value) {}

        /** @brief Checks one state bit. @param flag Flag to inspect. @return Whether the flag is present. */
        [[nodiscard]] constexpr bool Has(const UiAccessibilityStateFlag flag) const noexcept {
            return (flags & static_cast<UiAccessibilityStateMask>(flag)) != 0;
        }

        /** @brief Checks that no future/unknown state bit is present. @return Whether the mask is schema-valid. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return (flags & ~static_cast<UiAccessibilityStateMask>(0xFFFFU)) == 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const UiAccessibilityState &) const noexcept = default;
    };

    /** @brief Bounded text supplied to a semantic projection; the snapshot copies it into owned storage. */
    struct UiAccessibilityTextInput final {
        std::string_view text;
        UiAccessibilityTextSource source{UiAccessibilityTextSource::ResolvedMessage};
    };

    /** @brief Owned text range in an immutable semantic snapshot. */
    struct UiAccessibilityTextRef final {
        std::uint32_t offset{NoUiAccessibilityIndex};
        std::uint32_t size{};
        UiAccessibilityTextSource source{UiAccessibilityTextSource::ResolvedMessage};

        /** @brief Checks whether this reference names text. @return False for an absent optional field. */
        [[nodiscard]] constexpr bool IsPresent() const noexcept {
            return offset != NoUiAccessibilityIndex;
        }

        [[nodiscard]] constexpr auto operator<=>(const UiAccessibilityTextRef &) const noexcept = default;
    };

    /** @brief Closed typed value vocabulary for node values and action arguments. */
    enum class UiAccessibilityValueKind : std::uint8_t {
        None,
        Boolean,
        Integer,
        Number,
        Text,
    };

    /** @brief Borrowed typed value used in a candidate projection or action request. */
    struct UiAccessibilityValueInput final {
        UiAccessibilityValueKind kind{UiAccessibilityValueKind::None};
        bool boolean{};
        std::int64_t integer{};
        double number{};
        UiAccessibilityTextInput text;
    };

    /** @brief Owned typed value stored in an immutable semantic snapshot. */
    struct UiAccessibilityValue final {
        UiAccessibilityValueKind kind{UiAccessibilityValueKind::None};
        bool boolean{};
        std::int64_t integer{};
        double number{};
        UiAccessibilityTextRef text;

        /** @brief Checks the scalar representation; text-reference bounds are checked by the owning snapshot. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Inclusive finite range metadata for sliders and progress controls. */
    struct UiAccessibilityRange final {
        double minimum{};
        double maximum{};
        double current{};
        double step{};

        /** @brief Validates finite ordered bounds, current value and non-negative step. @return Whether the range is valid. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Selection model and item evidence for collection, tab, tree and table controls. */
    enum class UiAccessibilitySelectionMode : std::uint8_t {
        None,
        Single,
        Multiple,
    };

    /** @brief Typed selection state; item index/count are bounded by the published snapshot. */
    struct UiAccessibilitySelection final {
        UiAccessibilitySelectionMode mode{UiAccessibilitySelectionMode::None};
        bool selected{};
        std::uint32_t index{};
        std::uint32_t count{};

        /** @brief Validates mode, selected item range and finite collection evidence. @return Whether selection is valid. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Closed validation-error vocabulary exposed to assistive technology. */
    enum class UiAccessibilityErrorKind : std::uint8_t {
        None,
        Invalid,
        Required,
        Range,
        Pattern,
        Custom,
    };

    /** @brief Borrowed validation error supplied by the control owner. */
    struct UiAccessibilityErrorInput final {
        UiAccessibilityErrorKind kind{UiAccessibilityErrorKind::None};
        UiAccessibilityTextInput message;
    };

    /** @brief Owned validation error stored in an immutable semantic snapshot. */
    struct UiAccessibilityError final {
        UiAccessibilityErrorKind kind{UiAccessibilityErrorKind::None};
        UiAccessibilityTextRef message;
    };

    /** @brief Closed relation vocabulary for label, description, control and focus projections. */
    enum class UiAccessibilityRelationKind : std::uint8_t {
        LabelledBy,
        DescribedBy,
        Controls,
        Owns,
        ActiveDescendant,
        ErrorMessage,
        FlowTo,
    };

    namespace Detail {
        /** @brief Shared representation for bounded non-zero accessibility identities. */
        template <typename Derived, typename Scalar> class UiAccessibilityNumericId {
        public:
            constexpr UiAccessibilityNumericId() noexcept = default;

            /** @brief Returns the stable numeric identity. @return Zero only for the invalid identity. */
            [[nodiscard]] constexpr Scalar Value() const noexcept {
                return value_;
            }

            /** @brief Checks representation, not registry residency. @return Whether the identity is non-zero. */
            [[nodiscard]] constexpr bool IsValid() const noexcept {
                return value_ != 0;
            }

            [[nodiscard]] friend constexpr auto operator<=>(const Derived &left, const Derived &right) noexcept {
                return left.Value() <=> right.Value();
            }

            [[nodiscard]] friend constexpr bool operator==(const Derived &left, const Derived &right) noexcept {
                return left.Value() == right.Value();
            }

        protected:
            explicit constexpr UiAccessibilityNumericId(const Scalar value) noexcept : value_(value) {}

        private:
            Scalar value_{};
        };
    }  // namespace Detail

    /** @brief Bounded stable action identity supplied by the control owner. */
    class UiAccessibilityActionId final : public Detail::UiAccessibilityNumericId<UiAccessibilityActionId, std::uint32_t> {
        using Base = Detail::UiAccessibilityNumericId<UiAccessibilityActionId, std::uint32_t>;

    public:
        /** @brief Constructs the reserved invalid identity. */
        UiAccessibilityActionId() = default;
        /** @brief Creates a non-zero action identity. @param value Stable owner-assigned action number. */
        [[nodiscard]] static Result<UiAccessibilityActionId> Create(std::uint32_t value);

        using Base::IsValid;
        using Base::Value;

    private:
        explicit constexpr UiAccessibilityActionId(const std::uint32_t value) noexcept : Base(value) {}
    };

    /** @brief Bounded stable identity of a package/module contributing a control projection. */
    class UiAccessibilityContributorId final : public Detail::UiAccessibilityNumericId<UiAccessibilityContributorId, std::uint64_t> {
        using Base = Detail::UiAccessibilityNumericId<UiAccessibilityContributorId, std::uint64_t>;

    public:
        /** @brief Constructs the reserved core/no-contributor identity. */
        UiAccessibilityContributorId() = default;
        /** @brief Creates a non-zero contributor identity. @param value Stable host-issued contributor number. */
        [[nodiscard]] static Result<UiAccessibilityContributorId> Create(std::uint64_t value);

        using Base::IsValid;
        using Base::Value;

    private:
        explicit constexpr UiAccessibilityContributorId(const std::uint64_t value) noexcept : Base(value) {}
    };

    /** @brief Closed action vocabulary that maps to typed Runtime UI commands. */
    enum class UiAccessibilityActionKind : std::uint8_t {
        Focus,
        Activate,
        Increment,
        Decrement,
        SetValue,
        SetText,
        ScrollForward,
        ScrollBackward,
        ScrollTo,
        Expand,
        Collapse,
        Select,
        ClearSelection,
        Dismiss,
    };

    /** @brief Declared argument type accepted by one semantic action. */
    enum class UiAccessibilityActionValueKind : std::uint8_t {
        None,
        Boolean,
        Integer,
        Number,
        Text,
    };

    /** @brief Borrowed typed relation target expressed in stable authored identity space. */
    struct UiAccessibilityRelationInput final {
        UiAccessibilityRelationKind kind{UiAccessibilityRelationKind::LabelledBy};
        UiElementId target;
    };

    /** @brief Public schema synonym for one borrowed relation declaration. */
    using UiAccessibilityRelationDescriptor = UiAccessibilityRelationInput;

    /** @brief Borrowed action declaration for one node. */
    struct UiAccessibilityActionInput final {
        UiAccessibilityActionId id;
        UiAccessibilityActionKind kind{UiAccessibilityActionKind::Activate};
        UiAccessibilityActionValueKind argumentKind{UiAccessibilityActionValueKind::None};
        UiAccessibilityTextInput name;
    };

    /** @brief Public schema synonym for one borrowed action declaration. */
    using UiAccessibilityActionDescriptor = UiAccessibilityActionInput;

    /** @brief Owned relation in node-ID space for one immutable semantic snapshot. */
    struct UiAccessibilityRelation final {
        UiAccessibilityRelationKind kind{UiAccessibilityRelationKind::LabelledBy};
        UiAccessibilityNodeId target;
    };

    /** @brief Owned action declaration in one immutable semantic snapshot. */
    struct UiAccessibilityAction final {
        UiAccessibilityActionId id;
        UiAccessibilityActionKind kind{UiAccessibilityActionKind::Activate};
        UiAccessibilityActionValueKind argumentKind{UiAccessibilityActionValueKind::None};
        UiAccessibilityTextRef name;
    };

    /** @brief Candidate semantic node supplied by a core or contributed control owner. */
    struct UiAccessibilityNodeInput final {
        UiElementId element;
        UiAccessibilityRole role{UiAccessibilityRole::Group};
        UiAccessibilityControlSource source{UiAccessibilityControlSource::Core};
        UiAccessibilityContributorId contributor;
        UiAccessibilityTextInput name;
        UiAccessibilityTextInput description;
        UiAccessibilityValueInput value;
        UiAccessibilityState state;
        bool hasRange{};
        UiAccessibilityRange range;
        bool hasSelection{};
        UiAccessibilitySelection selection;
        UiAccessibilityErrorInput error;
        UiAccessibilityExposure exposure{UiAccessibilityExposure::Visible};
        UiLogicalRect bounds;
        std::span<const UiAccessibilityRelationInput> relations;
        std::span<const UiAccessibilityActionInput> actions;
    };

    /** @brief Public schema synonym for one borrowed core/contributed node declaration. */
    using UiAccessibilityNodeDescriptor = UiAccessibilityNodeInput;

    /** @brief Caller-selected bounded semantic capacities. */
    struct UiAccessibilityLimits final {
        std::uint32_t nodes{};
        std::uint32_t relations{};
        std::uint32_t actions{};
        std::uint32_t textBytes{};

        /** @brief Checks every capacity against the repository hard ceilings. @return Whether storage can be reserved. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Exact immutable lineage for one accessibility publication. */
    struct UiAccessibilitySnapshotDescriptor final {
        std::uint32_t schemaVersion{CurrentUiAccessibilitySchemaVersion};
        RuntimeUiInstanceId instance;
        UiCanvasInstanceId canvas;
        UiDocumentId document;
        UiDocumentRevision documentRevision;
        UiRuntimeTreeRevision treeRevision;
        UiInteractionRevision interactionRevision;
        UiAccessibilitySemanticRevision semanticRevision;
        UiAccessibilityLimits limits;

        /** @brief Checks all owner identities, revisions and capacities. @return Whether the descriptor is publishable. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Non-owning complete candidate projection copied transactionally into one snapshot slot. */
    struct UiAccessibilityProjection final {
        std::span<const UiAccessibilityNodeInput> nodes;
    };

    /** @brief Load-time owner descriptor for a bounded immutable semantic snapshot store. */
    struct UiAccessibilityExtractorDescriptor final {
        RuntimeUiInstanceId instance;
        UiCanvasInstanceId canvas;
        UiDocumentId document;
        UiAccessibilityLimits limits;
        std::uint32_t concurrentSnapshots{};

        /** @brief Checks exact owner identities, capacities and slot count. @return Whether the store can be created. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Explicit admission state of a semantic snapshot store. */
    enum class UiAccessibilityExtractorState : std::uint8_t {
        Active,
        Closed,
    };

    /** @brief One node's stable, owned semantic record. */
    struct UiAccessibilityNode final {
        UiAccessibilityNodeId id;
        UiElementHandle element;
        UiElementId elementId; /**< Stable authored element identity used for reload-safe lookup. */
        UiAccessibilityNodeId parent;
        UiAccessibilityRole role{UiAccessibilityRole::Group};
        UiAccessibilityControlSource source{UiAccessibilityControlSource::Core};
        UiAccessibilityContributorId contributor;
        UiAccessibilityTextRef name;
        UiAccessibilityTextRef description;
        UiAccessibilityValue value;
        UiAccessibilityState state;
        bool hasRange{};
        UiAccessibilityRange range;
        bool hasSelection{};
        UiAccessibilitySelection selection;
        UiAccessibilityError error;
        UiAccessibilityExposure exposure{UiAccessibilityExposure::Visible};
        UiLogicalRect bounds;
        std::uint32_t firstRelation{};
        std::uint32_t relationCount{};
        std::uint32_t firstAction{};
        std::uint32_t actionCount{};
    };

    /** @brief Revision-checked request to invoke one declared semantic action. */
    struct UiAccessibilityActionRequest final {
        RuntimeUiInstanceId instance;
        UiCanvasInstanceId canvas;
        UiDocumentId document;
        UiDocumentRevision documentRevision;
        UiRuntimeTreeRevision treeRevision;
        UiInteractionRevision interactionRevision;
        UiAccessibilitySemanticRevision semanticRevision;
        UiAccessibilityNodeId node;
        UiAccessibilityActionId action;
        UiAccessibilityValueInput argument;
    };

    class UiAccessibilityExtractor;

    /**
     * @brief Immutable owned accessibility snapshot safe for platform, recording and headless consumers.
     * @details The snapshot retains one preallocated slot. It owns resolved text bytes and typed records, never tree pointers,
     *          renderer handles, native objects, callbacks or contributor-owned memory.
     */
    class UiAccessibilitySnapshot final {
    public:
        /** @brief Releases this snapshot's immutable storage lease. */
        ~UiAccessibilitySnapshot();
        /** @brief Retains the exact immutable snapshot generation. @param other Live snapshot to retain. */
        UiAccessibilitySnapshot(const UiAccessibilitySnapshot &other) noexcept;
        /** @brief Replaces this lease with a retained snapshot. @param other Live snapshot. @return This snapshot. */
        UiAccessibilitySnapshot &operator=(const UiAccessibilitySnapshot &other) noexcept;
        /** @brief Transfers one immutable snapshot lease. @param other Snapshot to transfer. */
        UiAccessibilitySnapshot(UiAccessibilitySnapshot &&other) noexcept;
        /** @brief Replaces this lease by transfer. @param other Snapshot to transfer. @return This snapshot. */
        UiAccessibilitySnapshot &operator=(UiAccessibilitySnapshot &&other) noexcept;

        /** @brief Returns exact source and publication evidence. @return Borrowed immutable descriptor. */
        [[nodiscard]] const UiAccessibilitySnapshotDescriptor &Descriptor() const noexcept;
        /** @brief Returns all nodes in supplied deterministic projection order. @return Borrowed immutable nodes. */
        [[nodiscard]] std::span<const UiAccessibilityNode> Nodes() const noexcept;
        /** @brief Returns all flattened relations. @return Borrowed immutable relations. */
        [[nodiscard]] std::span<const UiAccessibilityRelation> Relations() const noexcept;
        /** @brief Returns all flattened action declarations. @return Borrowed immutable actions. */
        [[nodiscard]] std::span<const UiAccessibilityAction> Actions() const noexcept;
        /** @brief Resolves an owned text reference. @param text Snapshot-owned text reference. @return Empty for absent/invalid references.
         */
        [[nodiscard]] std::string_view Text(UiAccessibilityTextRef text) const noexcept;
        /** @brief Finds one node by stable authored identity. @param element Stable element identity. @return Node ID or stale failure. */
        [[nodiscard]] Result<UiAccessibilityNodeId> Find(UiElementId element) const;
        /** @brief Copies one exact node record. @param node Generation-checked semantic node. @return Node or stale failure. */
        [[nodiscard]] Result<UiAccessibilityNode> Get(UiAccessibilityNodeId node) const;
        /** @brief Returns the action slice for one valid node record. @param node Exact node from Nodes(). @return Borrowed bounded
         * actions. */
        [[nodiscard]] std::span<const UiAccessibilityAction> Actions(const UiAccessibilityNode &node) const noexcept;
        /** @brief Returns the relation slice for one valid node record. @param node Exact node from Nodes(). @return Borrowed bounded
         * relations. */
        [[nodiscard]] std::span<const UiAccessibilityRelation> Relations(const UiAccessibilityNode &node) const noexcept;

    private:
        struct Storage;
        friend class UiAccessibilityExtractor;
        explicit UiAccessibilitySnapshot(std::shared_ptr<const Storage> storage) noexcept;
        void Retain() const noexcept;
        void Release() noexcept;
        std::shared_ptr<const Storage> storage_;
    };

    /**
     * @brief Owner-thread bounded store that publishes immutable accessibility snapshots without frame-hot fallback allocation.
     * @details Creation reserves every node, relation, action and text arena in every slot. Close prevents new publication while
     *          existing leases remain valid for reload, presentation and shutdown draining.
     */
    class UiAccessibilityExtractor final {
    public:
        /**
         * @brief Creates an active store and preallocates all snapshot slots.
         * @param descriptor Exact Runtime UI owner and fixed semantic capacities.
         * @return Active store or typed identity/capacity failure.
         */
        [[nodiscard]] static Result<UiAccessibilityExtractor> Create(const UiAccessibilityExtractorDescriptor &descriptor);
        /** @brief Closes publication admission while outstanding snapshots remain readable. */
        ~UiAccessibilityExtractor();
        /** @brief Transfers store ownership. @param other Store to invalidate and transfer. */
        UiAccessibilityExtractor(UiAccessibilityExtractor &&other) noexcept;
        /** @brief Replaces this store by transfer. @param other Store to transfer. @return This store. */
        UiAccessibilityExtractor &operator=(UiAccessibilityExtractor &&other) noexcept;
        UiAccessibilityExtractor(const UiAccessibilityExtractor &) = delete;
        UiAccessibilityExtractor &operator=(const UiAccessibilityExtractor &) = delete;

        /**
         * @brief Validates and publishes one complete candidate against the exact active retained tree.
         * @param tree Active retained tree supplying element residency and parent evidence.
         * @param descriptor Exact source, interaction and semantic revision lineage.
         * @param projection Borrowed core/contributed node records copied on success.
         * @return Immutable snapshot lease or typed validation, stale, capacity, exhaustion or lifecycle failure.
         * @pre Calls for one extractor are serialized on the Runtime UI owner thread.
         */
        [[nodiscard]] Result<UiAccessibilitySnapshot> Extract(const UiElementTree &tree,
                                                              const UiAccessibilitySnapshotDescriptor &descriptor,
                                                              const UiAccessibilityProjection &projection);
        /** @brief Closes new publication admission idempotently. */
        void Close() noexcept;
        /** @brief Reports whether all snapshot leases have drained. @return True when no slot is retained. */
        [[nodiscard]] bool IsDrained() const noexcept;
        /** @brief Returns current admission state. @return Active or Closed. */
        [[nodiscard]] UiAccessibilityExtractorState State() const noexcept;

    private:
        struct Storage;
        explicit UiAccessibilityExtractor(std::unique_ptr<Storage> storage) noexcept;
        std::unique_ptr<Storage> storage_;
    };

    /**
     * @brief Validates a revision-checked action request against one immutable snapshot.
     * @param snapshot Exact semantic generation that was presented or recorded.
     * @param request Native/input request with expected owner, revisions, node and typed argument.
     * @return Success or typed stale/rejected/invalid action failure; no command is executed.
     */
    [[nodiscard]] Result<void> ValidateUiAccessibilityActionRequest(const UiAccessibilitySnapshot &snapshot,
                                                                    const UiAccessibilityActionRequest &request);
}  // namespace Horo::Runtime::Ui
