#pragma once

/**
 * @file BehaviorTree.h
 * @brief Versioned, editor-independent behavior-tree asset schema.
 */

#include "Horo/AI/AIIdentity.h"
#include "Horo/Foundation/Result.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Horo::AI {
    struct BehaviorTreeGraphIdentityTag;
    struct BehaviorTreeNodeIdentityTag;
    struct BehaviorTreeEdgeIdentityTag;
    struct BehaviorTreePinIdentityTag;
    struct BehaviorTreePropertyIdentityTag;
    struct BehaviorTreeNodeTypeIdentityTag;

    /** @brief Stable authored identity of one behavior-tree asset. */
    using BehaviorTreeGraphId = Foundation::Detail::NonZeroId64<BehaviorTreeGraphIdentityTag, AIErrors::IdentityInvalid>;
    /** @brief Stable authored identity of one behavior-tree node. */
    using BehaviorTreeNodeId = Foundation::Detail::NonZeroId64<BehaviorTreeNodeIdentityTag, AIErrors::IdentityInvalid>;
    /** @brief Stable authored identity of one behavior-tree edge. */
    using BehaviorTreeEdgeId = Foundation::Detail::NonZeroId64<BehaviorTreeEdgeIdentityTag, AIErrors::IdentityInvalid>;
    /** @brief Stable authored identity of one node pin. */
    using BehaviorTreePinId = Foundation::Detail::NonZeroId64<BehaviorTreePinIdentityTag, AIErrors::IdentityInvalid>;
    /** @brief Stable authored identity of one node property. */
    using BehaviorTreePropertyId = Foundation::Detail::NonZeroId64<BehaviorTreePropertyIdentityTag, AIErrors::IdentityInvalid>;
    /** @brief Stable semantic node-type identity resolved by the owning catalog. */
    using BehaviorTreeNodeTypeId = Foundation::Detail::NonZeroId64<BehaviorTreeNodeTypeIdentityTag, AIErrors::IdentityInvalid>;

    /** @brief Persisted behavior-tree schema version. */
    struct BehaviorTreeSchemaVersion final {
        std::uint16_t major{1};
        std::uint16_t minor{};

        [[nodiscard]] constexpr auto operator<=>(const BehaviorTreeSchemaVersion &) const noexcept = default;
    };

    /** @brief Current schema accepted by the immutable asset boundary. */
    inline constexpr BehaviorTreeSchemaVersion CurrentBehaviorTreeSchemaVersion{1, 0};

    /** @brief Compile-time ceilings that authored or untrusted graph data cannot raise. */
    struct BehaviorTreeAssetHardLimits final {
        static constexpr std::size_t Nodes = 1'024;
        static constexpr std::size_t Edges = 2'048;
        static constexpr std::size_t Pins = 4'096;
        static constexpr std::size_t PinsPerNode = 128;
        static constexpr std::size_t PropertiesPerNode = 64;
        static constexpr std::size_t PropertyBytes = 4'096;
        static constexpr std::size_t DisplayNameBytes = 256;
        static constexpr std::size_t ChildrenPerNode = 256;
    };

    /** @brief Project-lowerable limits applied before asset publication. */
    struct BehaviorTreeAssetLimits final {
        std::size_t maximumNodes{BehaviorTreeAssetHardLimits::Nodes};
        std::size_t maximumEdges{BehaviorTreeAssetHardLimits::Edges};
        std::size_t maximumPins{BehaviorTreeAssetHardLimits::Pins};
        std::size_t maximumPinsPerNode{BehaviorTreeAssetHardLimits::PinsPerNode};
        std::size_t maximumPropertiesPerNode{BehaviorTreeAssetHardLimits::PropertiesPerNode};
        std::size_t maximumPropertyBytes{BehaviorTreeAssetHardLimits::PropertyBytes};
        std::size_t maximumDisplayNameBytes{BehaviorTreeAssetHardLimits::DisplayNameBytes};
        std::size_t maximumChildrenPerNode{BehaviorTreeAssetHardLimits::ChildrenPerNode};

        [[nodiscard]] constexpr auto operator<=>(const BehaviorTreeAssetLimits &) const noexcept = default;
    };

    /** @brief Semantic role of one behavior-tree node. */
    enum class BehaviorTreeNodeKind : std::uint8_t {
        Root,
        Composite,
        Decorator,
        Service,
        Task,
        Unknown,
        Count,
    };

    /** @brief Direction of a stable node pin. */
    enum class BehaviorTreePinDirection : std::uint8_t {
        Input,
        Output,
        Count,
    };

    /** @brief Incoming cardinality allowed by a pin. */
    enum class BehaviorTreePinCardinality : std::uint8_t {
        Single,
        Multiple,
        Count,
    };

    /** @brief Structural meaning of a behavior-tree edge. */
    enum class BehaviorTreeEdgeKind : std::uint8_t {
        Child,
        ServiceAttachment,
        Count,
    };

    /** @brief Persisted property representation, including an opaque unknown form. */
    enum class BehaviorTreePropertyKind : std::uint8_t {
        Boolean,
        SignedInteger,
        Scalar,
        String,
        NodeReference,
        Opaque,
        Count,
    };

    /** @brief Versioned bytes preserved when a property type is unavailable locally. */
    struct BehaviorTreeOpaqueProperty final {
        std::uint32_t contractVersion{1};
        std::vector<std::uint8_t> bytes;

        [[nodiscard]] bool operator==(const BehaviorTreeOpaqueProperty &) const = default;
    };

    /** @brief Closed scalar/opaque value carrier for one semantic node property. */
    using BehaviorTreePropertyValue =
        std::variant<std::monostate, bool, std::int64_t, double, std::string, BehaviorTreeNodeId, BehaviorTreeOpaqueProperty>;

    /** @brief Stable typed property whose display name is never used as identity. */
    struct BehaviorTreeProperty final {
        BehaviorTreePropertyId id;
        std::string displayName;
        BehaviorTreePropertyKind kind{BehaviorTreePropertyKind::Boolean};
        BehaviorTreePropertyValue value{};

        [[nodiscard]] bool operator==(const BehaviorTreeProperty &) const noexcept = default;
    };

    /** @brief Stable endpoint identity used by topology edges. */
    struct BehaviorTreePin final {
        BehaviorTreePinId id;
        BehaviorTreePinDirection direction{BehaviorTreePinDirection::Input};
        BehaviorTreePinCardinality cardinality{BehaviorTreePinCardinality::Single};

        [[nodiscard]] constexpr auto operator<=>(const BehaviorTreePin &) const noexcept = default;
    };

    /** @brief Versioned reference to another behavior-tree asset used as a nested subtree. */
    struct BehaviorTreeSubtreeReference final {
        BehaviorTreeGraphId graph;
        BehaviorTreeSchemaVersion requiredSchemaVersion{CurrentBehaviorTreeSchemaVersion};

        [[nodiscard]] constexpr auto operator<=>(const BehaviorTreeSubtreeReference &) const noexcept = default;
    };

    /** @brief Stable authored node independent of graph layout, display name, or array order. */
    struct BehaviorTreeNode final {
        BehaviorTreeNodeId id;
        BehaviorTreeNodeKind kind{BehaviorTreeNodeKind::Unknown};
        BehaviorTreeNodeTypeId type;
        BehaviorTreeSchemaVersion typeVersion{1, 0};
        std::string displayName;
        std::size_t minimumChildren{};
        std::size_t maximumChildren{};
        std::vector<BehaviorTreePin> pins;
        std::vector<BehaviorTreeProperty> properties;
        std::optional<BehaviorTreeSubtreeReference> nestedSubtree;

        [[nodiscard]] bool operator==(const BehaviorTreeNode &) const noexcept = default;
    };

    /** @brief Stable directed topology edge; order is semantic child/service order, not layout order. */
    struct BehaviorTreeEdge final {
        BehaviorTreeEdgeId id;
        BehaviorTreeEdgeKind kind{BehaviorTreeEdgeKind::Child};
        BehaviorTreeNodeId sourceNode;
        BehaviorTreePinId sourcePin;
        BehaviorTreeNodeId targetNode;
        BehaviorTreePinId targetPin;
        std::size_t order{};

        [[nodiscard]] constexpr auto operator<=>(const BehaviorTreeEdge &) const noexcept = default;
    };

    /** @brief Borrowed mutable source data captured into an immutable behavior-tree asset. */
    struct BehaviorTreeAssetDescriptor final {
        BehaviorTreeSchemaVersion version{CurrentBehaviorTreeSchemaVersion};
        BehaviorTreeGraphId graph;
        std::string displayName;
        BehaviorTreeNodeId root;
        std::vector<BehaviorTreeNode> nodes;
        std::vector<BehaviorTreeEdge> edges;
    };

    /**
     * @brief Immutable, editor-independent behavior-tree asset.
     * @details Nodes and edges use stable authored identities. No editor layout or UI type is part of this contract.
     */
    class BehaviorTreeAsset final {
    public:
        BehaviorTreeAsset() = delete;

        /**
         * @brief Validates, canonicalizes, and owns one bounded behavior-tree source.
         * @param candidate Borrowed-by-value source whose vectors are consumed only on success.
         * @param limits Project-lowerable finite validation limits.
         * @return Immutable asset or a typed schema, identity, topology, cycle, capacity, or storage failure.
         * @post Failure publishes no partial asset and does not mutate caller-owned source storage.
         */
        [[nodiscard]] static Result<BehaviorTreeAsset> Capture(BehaviorTreeAssetDescriptor candidate,
                                                               const BehaviorTreeAssetLimits &limits = {});

        /** @brief Returns the stable graph identity. @return Authored graph identity. */
        [[nodiscard]] constexpr BehaviorTreeGraphId Graph() const noexcept {
            return data_.graph;
        }

        /** @brief Returns the persisted schema version. @return Captured schema version. */
        [[nodiscard]] constexpr BehaviorTreeSchemaVersion Version() const noexcept {
            return data_.version;
        }

        /** @brief Returns the stable root node identity. @return Root identity. */
        [[nodiscard]] constexpr BehaviorTreeNodeId Root() const noexcept {
            return data_.root;
        }

        /** @brief Returns the canonical owned source. @return Immutable descriptor view. */
        [[nodiscard]] const BehaviorTreeAssetDescriptor &Data() const noexcept {
            return data_;
        }

        /** @brief Returns nodes sorted by stable identity. @return Borrowed immutable node view. */
        [[nodiscard]] std::span<const BehaviorTreeNode> Nodes() const noexcept;
        /** @brief Returns edges in stable semantic source/kind/order sequence. @return Borrowed immutable edge view. */
        [[nodiscard]] std::span<const BehaviorTreeEdge> Edges() const noexcept;

        /** @brief Returns the number of explicitly preserved unknown node types. @return Unknown node count. */
        [[nodiscard]] std::size_t UnknownNodeCount() const noexcept {
            return unknownNodeCount_;
        }

        /** @brief Returns the number of opaque properties preserved for degraded inspection. @return Unknown property count. */
        [[nodiscard]] std::size_t UnknownPropertyCount() const noexcept {
            return unknownPropertyCount_;
        }

        /** @brief Finds one node by stable identity. @param id Node identity. @return Node or null when absent. */
        [[nodiscard]] const BehaviorTreeNode *FindNode(BehaviorTreeNodeId id) const noexcept;

        BehaviorTreeAsset(const BehaviorTreeAsset &) = delete;
        BehaviorTreeAsset(BehaviorTreeAsset &&) noexcept = default;
        BehaviorTreeAsset &operator=(const BehaviorTreeAsset &) = delete;
        BehaviorTreeAsset &operator=(BehaviorTreeAsset &&) = delete;

    private:
        BehaviorTreeAsset(BehaviorTreeAssetDescriptor data, std::size_t unknownNodes, std::size_t unknownProperties) noexcept;

        BehaviorTreeAssetDescriptor data_;
        std::size_t unknownNodeCount_{};
        std::size_t unknownPropertyCount_{};
    };
}  // namespace Horo::AI
