#pragma once

/**
 * @file PCGGraphValidation.h
 * @brief Deterministic pre-compile PCG graph validation and ordering contract.
 */

#include "Horo/Foundation/Sha256.h"
#include "Horo/PCG/PCGGraphAsset.h"
#include "Horo/PCG/PCGRegistry.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Horo::PCG {
    /** @brief Lifecycle gate captured before graph validation begins. */
    enum class PCGGraphValidationAdmission : std::uint8_t {
        Accepting,
        CancellationRequested,
        ShuttingDown,
        Count
    };

    /** @brief Bounded work and diagnostic policy for one validation pass. */
    struct PCGGraphValidationLimits final {
        static constexpr std::size_t HardMaximumNodes = 1'024;       /**< Compiled node work ceiling. */
        static constexpr std::size_t HardMaximumEdges = 2'048;       /**< Compiled edge work ceiling. */
        static constexpr std::size_t HardMaximumDiagnostics = 1'024; /**< Compiled retained-finding ceiling. */

        std::size_t maximumNodes{HardMaximumNodes}; /**< Maximum nodes inspected and ordered. */
        std::size_t maximumEdges{HardMaximumEdges}; /**< Maximum edges inspected. */
        std::size_t maximumDiagnostics{1'024};      /**< Maximum node-specific findings retained. */
    };

    /** @brief One validated node paired with its exact runtime-registry generation handle. */
    struct PCGValidatedNode final {
        NodeId node{};                  /**< Stable authored node identity. */
        NodeTypeId type{};              /**< Exact semantic type validated for this node. */
        PCGNodeRuntimeHandle runtime{}; /**< Runtime handle fenced to the captured registry snapshot. */
        [[nodiscard]] constexpr auto operator<=>(const PCGValidatedNode &) const noexcept = default;
    };

    /** @brief Immutable graph validation output with exact source digest safe to hand to the canonical compiler. */
    class PCGValidatedGraph final {
    public:
        /** @brief Returns the exact graph source generation. @return Durable graph generation. */
        [[nodiscard]] GraphGeneration Generation() const noexcept;
        /** @brief Returns the digest of exact canonical source bytes used by validation. @return Source digest. */
        [[nodiscard]] Sha256Digest SourceDigest() const noexcept;
        /** @brief Returns the registry generation that supplied capabilities and runtimes. @return Registry generation. */
        [[nodiscard]] std::uint64_t RegistryGeneration() const noexcept;
        /** @brief Returns the exact graph-registry handle validated for compilation. @return Generation-safe graph handle. */
        [[nodiscard]] PCGGraphHandle RegistryGraph() const noexcept;
        /** @brief Returns nodes in deterministic dependency-first order. @return Borrowed immutable node sequence. */
        [[nodiscard]] std::span<const PCGValidatedNode> Nodes() const noexcept;

    private:
        PCGValidatedGraph(GraphGeneration generation, const Sha256Digest &sourceDigest, std::uint64_t registryGeneration,
                          const PCGGraphHandle &registryGraph, std::vector<PCGValidatedNode> nodes) noexcept;

        GraphGeneration generation_{};
        Sha256Digest sourceDigest_{};
        std::uint64_t registryGeneration_{};
        PCGGraphHandle registryGraph_{};
        std::vector<PCGValidatedNode> nodes_;

        friend Result<PCGValidatedGraph> ValidatePCGGraph(const PCGGraphAsset &, const PCGRegistrySnapshot &, PCGCapabilitySet,
                                                          const PCGGraphValidationLimits &, PCGGraphValidationAdmission);
    };

    /**
     * @brief Validates one immutable graph against exact host capabilities and runtimes before compilation.
     * @param graph Canonical graph source; malformed raw source must first pass PCGGraphAsset::Create.
     * @param registry Immutable host registry snapshot retained by the caller through compilation.
     * @param requiredCapabilities Exact compiler/product capabilities required in addition to graph requirements.
     * @param limits Finite validation work and diagnostic bounds.
     * @param admission Captured cancellation/shutdown gate.
     * @return Dependency-first validated graph or GraphValidationFailed/GraphValidationCapacityExceeded/GraphLifecycleUnavailable.
     * @post Failure produces no executable plan and does not mutate graph or registry state.
     */
    [[nodiscard]] Result<PCGValidatedGraph> ValidatePCGGraph(
        const PCGGraphAsset &graph, const PCGRegistrySnapshot &registry, PCGCapabilitySet requiredCapabilities,
        const PCGGraphValidationLimits &limits = {}, PCGGraphValidationAdmission admission = PCGGraphValidationAdmission::Accepting);
}  // namespace Horo::PCG
