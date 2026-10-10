#pragma once
/** @file PCGNodeCatalog.h
 * @brief Explicit host-owned built-in node schemas and immutable executable catalog generations. */
#include "Horo/PCG/PCGGraphAsset.h"
#include "Horo/PCG/PCGRegistry.h"

#include <array>
#include <memory>
#include <thread>

namespace Horo::PCG {
    struct PCGCookedNode;

    namespace detail {
        struct NodeExecutionContext;
        Result<void> ExecuteNode(const NodeExecutionContext &, std::uint32_t);
    }  // namespace detail

    /** @brief Closed repository-built semantics; no content-supplied callbacks are admitted. */
    enum class PCGCpuNodeKind : std::uint8_t {
        SnapshotGrid = 0,
        DensityFilter = 1,
        Merge = 2,
        Forward = 3
    };
    /** @brief Returns a stable semantic type ID for one supported built-in node.
     * @param kind Exact closed built-in operation.
     * @return Stable type identity or typed unsupported-kind failure. */
    [[nodiscard]] Result<NodeTypeId> PCGCpuNodeType(PCGCpuNodeKind kind);

    /** @brief Exact schema of the opaque authored settings payload. */
    enum class PCGNodeSettingsSchema : std::uint8_t {
        None,
        SpatialGridIdentity
    };

    /** @brief One semantic pin slot; authored pin identities remain graph-owned. */
    struct PCGNodePinSchema final {
        PCGPinDirection direction{};
        PCGPinType type{};
        PCGPinCardinality cardinality{};
        auto operator<=>(const PCGNodePinSchema &) const = default;
    };

    /** @brief Inert complete declaration; execution functions are catalog-private and never serialized. */
    struct PCGBuiltInNodeDescriptor final {
        NodeTypeId type{};
        PCGCpuNodeKind kind{};
        PCGNodeTypeVersion version{1, 0};
        std::uint32_t runtimeContractVersion{1};
        std::uint32_t migrationVersion{1}; /**< Exact settings migration policy; version 1 requires exact source schema. */
        PCGNodeDeterminism determinism{};
        PCGCapabilitySet requiredCapabilities{};
        std::array<PCGNodePinSchema, 3> pins{};
        std::size_t pinCount{};
        PCGNodeSettingsSchema settings{};
        std::uint32_t workPerPoint{}; /**< Complete bounded point visits, including count/copy phases. */
        std::uint32_t fixedWork{1};
        std::uint32_t workPerSnapshotElement{}; /**< Grid identity lookup visits; zero for nodes that do not search spatial inputs. */
    };

    /** @brief Exact declared worst-case execution work; point storage is separately reserved by the workspace. */
    struct PCGNodeCost final {
        std::size_t pointVisits{};
        std::size_t snapshotElementVisits{};
    };

    /** @brief Immutable owning node catalog, safe through replacement and owner shutdown. */
    class PCGNodeCatalogSnapshot final {
    public:
        struct State;
        PCGNodeCatalogSnapshot() = default;
        /** @brief Reports whether a host issued this root. @return False for an inert snapshot. */
        [[nodiscard]] bool IsValid() const noexcept;
        /** @brief Returns publication generation. @return Zero for an inert snapshot. */
        [[nodiscard]] std::uint64_t Generation() const noexcept;
        /** @brief Returns the complete retained catalog charge. @return Zero for an inert root. */
        [[nodiscard]] std::size_t ResidentBytes() const noexcept;
        /** @brief Returns the exact product grant. @return Immutable host projection. */
        [[nodiscard]] const PCGCapabilityProjection &Capabilities() const noexcept;
        /** @brief Returns descriptors in stable type order. @return Snapshot-owned declarations. */
        [[nodiscard]] std::span<const PCGBuiltInNodeDescriptor> Nodes() const noexcept;
        /** @brief Resolves one repository-built type. @param type Stable type. @return Descriptor or unavailable failure.
         * @pre This snapshot outlives every borrowed descriptor pointer. */
        [[nodiscard]] Result<const PCGBuiltInNodeDescriptor *> Find(NodeTypeId type) const;
        /** @brief Validates exact cooked schema, version and product requirements. @param node Detached node.
         * @return Success or a stable node-located diagnostic; never invokes executable code. */
        [[nodiscard]] Result<void> Validate(const PCGCookedNode &node) const;
        /** @brief Computes checked declared work before execution. @param type Stable type.
         * @param maximumPoints Admitted input/output point ceiling.
         * @param maximumSnapshotElements Exact admitted spatial grid count for lookup. @return Bounded work or overflow failure. */
        [[nodiscard]] Result<PCGNodeCost> Cost(NodeTypeId type, std::size_t maximumPoints, std::size_t maximumSnapshotElements) const;

    private:
        friend class PCGNodeCatalog;
        friend Result<void> detail::ExecuteNode(const detail::NodeExecutionContext &, std::uint32_t);

        explicit PCGNodeCatalogSnapshot(std::shared_ptr<const State> state) noexcept : state_(std::move(state)) {}

        std::shared_ptr<const State> state_;
    };

    /** @brief Explicit composition owner; registration only publishes inert declarations and private built-in functions.
     * @details All owner operations and moves stay on the creating thread. Snapshots alone cross threads. */
    class PCGNodeCatalog final {
    public:
        PCGNodeCatalog(PCGNodeCatalog &&) noexcept = default;
        PCGNodeCatalog &operator=(PCGNodeCatalog &&) noexcept = default;
        PCGNodeCatalog(const PCGNodeCatalog &) = delete;
        PCGNodeCatalog &operator=(const PCGNodeCatalog &) = delete;
        /** @brief Creates an empty explicit product composition. @param capabilities Validated exact host grant.
         * @param maximumNodes Lowered built-in capacity, 1 through 4. @return Owner or typed invalid failure. */
        [[nodiscard]] static Result<PCGNodeCatalog> Create(PCGCapabilityProjection capabilities, std::size_t maximumNodes = 4);
        /** @brief Registers or explicitly replaces one built-in. @param kind Repository-built implementation.
         * @param replace True only when replacing an existing entry. @return New generation or typed duplicate/unavailable failure. */
        [[nodiscard]] Result<std::uint64_t> Register(PCGCpuNodeKind kind, bool replace = false);
        /** @brief Withdraws one type from future roots. @param type Stable type. @return New generation or unavailable failure. */
        [[nodiscard]] Result<std::uint64_t> Remove(NodeTypeId type);
        /** @brief Captures current immutable declarations/functions. @return Root or closed/wrong-thread failure. */
        [[nodiscard]] Result<PCGNodeCatalogSnapshot> Snapshot() const;
        /** @brief Closes owner admission; old roots remain executable historical capabilities.
         * @return Success or wrong-thread failure; repeated close is idempotent. */
        [[nodiscard]] Result<void> Close();

    private:
        PCGNodeCatalog(PCGCapabilityProjection capabilities, std::size_t maximumNodes,
                       std::shared_ptr<const PCGNodeCatalogSnapshot::State> state) noexcept;
        [[nodiscard]] Result<void> Check() const;
        PCGCapabilityProjection capabilities_;
        std::size_t maximumNodes_{};
        std::thread::id owner_;
        std::shared_ptr<const PCGNodeCatalogSnapshot::State> state_;
        bool closed_{};
    };
}  // namespace Horo::PCG
