#pragma once

/**
 * @file PCGProvenance.h
 * @brief Immutable deterministic PCG execution provenance, seed, and output hash contract.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/PCG/PCGIdentity.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace Horo::PCG {
    /** @brief Stable world identity; cell coordinates alone do not identify a world. */
    using PCGWorldId = PcgStableIdentity<struct PCGWorldIdentityTag>;
    /** @brief Stable identity of one typed exposed or external input. */
    using PCGInputId = PcgStableIdentity<struct PCGInputIdentityTag>;

    /** @brief Semantic reproducibility promise, independent of operational capacity tier. */
    enum class PCGDeterminismClass : std::uint8_t {
        PortableDeterministic,
        ProfileDeterministic,
        BestEffortPreview,
        Count
    };

    /** @brief Numeric implementation required to fulfill the determinism promise. */
    enum class PCGNumericSupport : std::uint8_t {
        PortableInteger,
        CertifiedProfileFloat,
        PreviewOnly,
        Count
    };

    /** @brief Whether one input admits reproducible evaluation. */
    enum class PCGInputDeterminism : std::uint8_t {
        Deterministic,
        NonDeterministic,
        Count
    };

    /** @brief Lifecycle gate captured before accepting a provenance snapshot. */
    enum class PCGProvenanceAdmission : std::uint8_t {
        Accepting,
        CancellationRequested,
        ShuttingDown,
        Count
    };

    /** @brief Exact typed input revision and canonical value digest. */
    struct PCGInputStamp final {
        PCGInputId id{};                                                     /**< Stable typed input identity. */
        std::uint64_t revision{};                                            /**< Non-zero owner-issued revision. */
        Sha256Digest content{};                                              /**< Digest of canonical typed input bytes. */
        PCGInputDeterminism determinism{PCGInputDeterminism::Deterministic}; /**< Input eligibility. */
        [[nodiscard]] auto operator<=>(const PCGInputStamp &) const = default;
    };

    /** @brief Exact provider snapshot and source truth revision. */
    struct PCGProviderStamp final {
        SpatialProviderId provider{};                                        /**< Stable host-composed provider identity. */
        SpatialSourceId source{};                                            /**< Stable provider-owned source identity. */
        SpatialSnapshotId snapshot{};                                        /**< Captured immutable snapshot identity. */
        SpatialRevision revision{};                                          /**< Exact source semantic revision. */
        std::uint64_t originEpoch{};                                         /**< Exact coordinate-origin generation. */
        Sha256Digest content{};                                              /**< Digest of canonical captured provider values. */
        PCGInputDeterminism determinism{PCGInputDeterminism::Deterministic}; /**< Provider eligibility. */
        [[nodiscard]] auto operator<=>(const PCGProviderStamp &) const = default;
    };

    /** @brief Complete detached execution inputs; capture owns sorted copies on success. */
    struct PCGProvenanceCandidate final {
        GraphGeneration graph{};            /**< Exact graph identity and accepted source revision. */
        Sha256Digest graphContent{};        /**< Canonical graph content digest. */
        std::uint64_t graphSeed{};          /**< Authored seed, including zero. */
        PCGWorldId world{};                 /**< Exact world identity. */
        std::array<std::int64_t, 3> cell{}; /**< Signed world-cell coordinates. */
        NodeId node{};                      /**< Stable semantic node identity. */
        PCGDeterminismClass determinism{PCGDeterminismClass::PortableDeterministic}; /**< Required promise. */
        PCGNumericSupport numeric{PCGNumericSupport::PortableInteger};               /**< Declared numeric implementation. */
        std::uint64_t numericPolicyVersion{1};     /**< Non-zero seed, order, and numeric policy version. */
        Sha256Digest profile{};                    /**< Exact certified profile for profile determinism; zero otherwise. */
        std::vector<PCGInputStamp> inputs{};       /**< Bounded typed input stamps. */
        std::vector<PCGProviderStamp> providers{}; /**< Bounded provider snapshot stamps. */
    };

    /** @brief Immutable provenance root that keeps old readers valid across replacement. */
    class PCGProvenance final {
    public:
        struct State;

        /** @brief Opaque construction gate restricted to validated capture. */
        class ConstructionKey final {
            friend Result<PCGProvenance> CapturePCGProvenance(PCGProvenanceCandidate, PCGProvenanceAdmission);
            ConstructionKey() = default;
        };

        PCGProvenance() = delete;

        /** @brief Adopts a fully validated immutable state through the capture-only gate. */
        explicit PCGProvenance(ConstructionKey, std::shared_ptr<const State> state) noexcept : state_(std::move(state)) {}

        /** @brief Returns canonical provenance evidence. @return Immutable captured candidate. */
        [[nodiscard]] const PCGProvenanceCandidate &Data() const noexcept;
        /** @brief Returns the versioned canonical provenance hash. @return Exact SHA-256 digest. */
        [[nodiscard]] Sha256Digest Key() const noexcept;
        /** @brief Derives a node and sample-local random stream without global RNG state.
         * @param sample Stable sample identity, independent of traversal order.
         * @return Stable 64-bit seed within the declared numeric policy and profile.
         */
        [[nodiscard]] Result<std::uint64_t> Seed(SourceSampleId sample) const;

    private:
        std::shared_ptr<const State> state_;
    };

    /** @brief Captures and canonicalizes a bounded provenance tuple.
     * @param candidate Complete detached graph, world, node, inputs, providers, and tier evidence.
     * @param admission Current lifecycle gate; cancellation and shutdown reject new captures.
     * @return Immutable root or typed malformed, duplicate, capacity, unsupported-tier, or lifecycle failure.
     */
    [[nodiscard]] Result<PCGProvenance> CapturePCGProvenance(PCGProvenanceCandidate candidate,
                                                             PCGProvenanceAdmission admission = PCGProvenanceAdmission::Accepting);

    /** @brief Checks whether captured derived data may be reused against current authoritative truth.
     * @param captured Original immutable provenance root.
     * @param current Newly captured authoritative root for the same logical scope.
     * @return Success only for identical deterministic provenance; stale or unsupported otherwise.
     */
    [[nodiscard]] Result<void> ValidatePCGProvenanceReuse(const PCGProvenance &captured, const PCGProvenance &current);

    /** @brief Stable identity and canonical content of one output candidate. */
    struct PCGOutputStamp final {
        GeneratedOutputId id{}; /**< Exact stable output identity. */
        Sha256Digest content{}; /**< Canonical output bytes digest. */
        [[nodiscard]] auto operator<=>(const PCGOutputStamp &) const = default;
    };

    /** @brief Hashes bounded outputs in canonical identity order within a deterministic tier.
     * @param provenance Exact immutable execution provenance.
     * @param outputs Unordered output stamps; IDs must be unique and belong to the captured graph/node.
     * @return Versioned output hash or typed invalid, capacity, or non-deterministic failure.
     */
    [[nodiscard]] Result<Sha256Digest> HashPCGOutputs(const PCGProvenance &provenance, std::span<const PCGOutputStamp> outputs);
}  // namespace Horo::PCG
