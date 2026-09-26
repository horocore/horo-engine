#pragma once

/**
 * @file PCGCookedPlan.h
 * @brief Canonical, owned PCG graph compilation output and bounded cook boundary.
 */

#include "Horo/PCG/PCGGraphValidation.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Horo::PCG {
    /** @brief Version of the portable cooked-plan byte contract. */
    struct PCGCookedPlanVersion final {
        std::uint16_t major{1};
        std::uint16_t minor{0};
        [[nodiscard]] constexpr auto operator<=>(const PCGCookedPlanVersion &) const noexcept = default;
    };

    inline constexpr PCGCookedPlanVersion CurrentPCGCookedPlanVersion{1, 0};
    inline constexpr std::uint32_t CurrentPCGCompilerVersion = 1;

    /** @brief Detached pin schema; input defaults are lowered into Constants(). */
    struct PCGCookedPin final {
        PinId id{};                      /**< Stable authored pin identity. */
        PCGPinDirection direction{};     /**< Closed direction. */
        PCGPinType type{};               /**< Closed value type. */
        PCGPinCardinality cardinality{}; /**< Input edge cardinality. */
        [[nodiscard]] constexpr auto operator<=>(const PCGCookedPin &) const noexcept = default;
    };

    /** @brief Detached executable node description in dependency-first plan order. */
    struct PCGCookedNode final {
        NodeId id{};                             /**< Stable authored identity. */
        NodeTypeId type{};                       /**< Exact semantic node type. */
        PCGNodeTypeVersion version{};            /**< Exact authored node schema. */
        std::uint32_t runtimeContractVersion{};  /**< Exact provider semantic contract. */
        PCGNodeDeterminism determinism{};        /**< Provider's declared determinism. */
        PCGCapabilitySet requiredCapabilities{}; /**< Exact provider capability requirements. */
        std::vector<PCGCookedPin> pins{};        /**< Owned exact pin schema. */
        std::vector<std::uint8_t> payload{};     /**< Owned bounded semantic node payload. */
        [[nodiscard]] bool operator==(const PCGCookedNode &) const noexcept = default;
    };

    /** @brief Dense plan-local routing edge with stable pin endpoints. */
    struct PCGCookedRoute final {
        EdgeId id{};                /**< Stable edge identity. */
        std::uint32_t sourceNode{}; /**< Source index in Nodes(). */
        PinId sourcePin{};          /**< Source output pin. */
        std::uint32_t targetNode{}; /**< Target index in Nodes(). */
        PinId targetPin{};          /**< Target input pin. */
        [[nodiscard]] constexpr auto operator<=>(const PCGCookedRoute &) const noexcept = default;
    };

    /** @brief Owned unconnected input default used by evaluation. */
    struct PCGCookedConstant final {
        std::uint32_t node{};  /**< Owner index in Nodes(). */
        PinId pin{};           /**< Exact input pin. */
        PCGPinType type{};     /**< Closed value type. */
        PCGGraphValue value{}; /**< Owned finite value. */
        [[nodiscard]] bool operator==(const PCGCookedConstant &) const noexcept = default;
    };

    /** @brief Owned externally bound input schema and fallback value. */
    struct PCGCookedExposedInput final {
        ExposedInputId id{};          /**< Stable external binding identity. */
        std::string key{};            /**< Canonical external key. */
        std::uint32_t node{};         /**< Owner index in Nodes(). */
        PinId pin{};                  /**< Exact input pin. */
        PCGPinType type{};            /**< Closed value type. */
        PCGGraphValue defaultValue{}; /**< Owned finite fallback. */
        [[nodiscard]] bool operator==(const PCGCookedExposedInput &) const noexcept = default;
    };

    /** @brief Immutable, backend-neutral plan; every view borrows only plan-owned storage. */
    class PCGCookedPlan final {
    public:
        /** @brief Returns exact graph identity and durable source revision. @return Graph generation. */
        [[nodiscard]] GraphGeneration Generation() const noexcept;
        /** @brief Returns the operational tier captured from the graph source. @return Exact tier. */
        [[nodiscard]] PCGOperationalTier Tier() const noexcept;
        /** @brief Returns source schema bound into this plan. @return Exact graph schema. */
        [[nodiscard]] PCGGraphSchemaVersion SourceSchema() const noexcept;
        /** @brief Returns digest of exact canonical source bytes. @return Source content digest. */
        [[nodiscard]] Sha256Digest SourceDigest() const noexcept;
        /** @brief Returns the exact plan byte schema. @return Cooked-plan version. */
        [[nodiscard]] PCGCookedPlanVersion Version() const noexcept;
        /** @brief Returns exact compiler algorithm version. @return Compiler version. */
        [[nodiscard]] std::uint32_t CompilerVersion() const noexcept;
        /** @brief Returns dependency-first owned node descriptions. @return Plan-owned nodes. */
        [[nodiscard]] std::span<const PCGCookedNode> Nodes() const noexcept;
        /** @brief Returns canonical plan-local pin routing. @return Plan-owned routes. */
        [[nodiscard]] std::span<const PCGCookedRoute> Routes() const noexcept;
        /** @brief Returns unconnected input defaults. @return Plan-owned constants. */
        [[nodiscard]] std::span<const PCGCookedConstant> Constants() const noexcept;
        /** @brief Returns external input bindings. @return Plan-owned schemas. */
        [[nodiscard]] std::span<const PCGCookedExposedInput> ExposedInputs() const noexcept;
        /** @brief Returns exact union of graph, caller and node-runtime requirements. @return Required capabilities. */
        [[nodiscard]] PCGCapabilitySet RequiredCapabilities() const noexcept;
        /** @brief Returns bounded canonical network-order plan bytes. @return Plan-owned bytes. */
        [[nodiscard]] std::span<const std::uint8_t> CanonicalBytes() const noexcept;

    private:
        struct Data final {
            GraphGeneration generation{};
            PCGOperationalTier tier{};
            PCGGraphSchemaVersion sourceSchema{};
            Sha256Digest sourceDigest{};
            PCGCapabilitySet requiredCapabilities{};
            std::vector<PCGCookedNode> nodes;
            std::vector<PCGCookedRoute> routes;
            std::vector<PCGCookedConstant> constants;
            std::vector<PCGCookedExposedInput> exposedInputs;
            std::vector<std::uint8_t> bytes;
        };

        explicit PCGCookedPlan(Data data) noexcept;

        Data data_;

        friend Result<PCGCookedPlan> CompilePCGGraph(const PCGGraphAsset &, const PCGValidatedGraph &, const PCGRegistrySnapshot &,
                                                     PCGCapabilitySet, std::size_t);
    };

    /**
     * @brief Lowers one validated graph and exact retained registry snapshot to a portable owned plan.
     * @param graph Canonical immutable authored source validated by ValidatePCGGraph.
     * @param validated Dependency-first validated graph from the same source and snapshot.
     * @param registry Exact retained snapshot used for validation; no reference is kept.
     * @param requiredCapabilities Caller/product requirements to bind into the plan.
     * @param maximumPlanBytes Finite caller-lowered canonical byte ceiling.
     * @return Complete plan or typed stale, invalid, capability or capacity failure; no partial plan is published.
     */
    [[nodiscard]] Result<PCGCookedPlan> CompilePCGGraph(const PCGGraphAsset &graph, const PCGValidatedGraph &validated,
                                                        const PCGRegistrySnapshot &registry,
                                                        PCGCapabilitySet requiredCapabilities = PCGCapabilitySet::Empty(),
                                                        std::size_t maximumPlanBytes = PCGGraphSourceHardLimits::SourceBytes);
}  // namespace Horo::PCG
