#pragma once

#include "PhysicsStateSnapshotFields.h"
#include "PhysicsStateSolverStorage.h"

namespace Horo::Physics::StateDetail {
    /** @brief Private parsed values, never a capability or native publication
     * authority. */
    struct ParsedSnapshotEnvelope final {
        PhysicsStateCompatibility compatibility;
        PhysicsStateSolverView solver;
        PhysicsStateWorldView world;
        std::span<const std::byte> construction;
    };

    /** @brief Structurally checked slices only; no solver/construction/native/asset authority is granted.
     * Cold receiving owner checks actual build/content/settings, then fully decodes both sections before native effects.
     */
    struct SnapshotEnvelopeSections final {
        SnapshotHeader header;
        std::span<const std::byte> solver;
        std::span<const std::byte> construction;
        std::span<const std::byte> world;
    };

    [[nodiscard]] Result<SnapshotEnvelopeSections> InspectSnapshotEnvelope(std::span<const std::byte>);

    /** @brief Writes the closed envelope around an independently validated
     * canonical solver and construction sections.
     * Construction is already fully validated by its Physics-owned manifest codec; this envelope is not another native admission authority.
     * @details The owner validates/rebinds full projection metadata against
     * retained actual resources separately. All sizing and alias checks precede
     * output writes. No storage grows.
     */
    [[nodiscard]] Result<std::size_t> EncodeSnapshotEnvelope(const PhysicsStateCompatibility &compatibility,
                                                             std::span<const std::byte> solver, const PhysicsStateWorldView &world,
                                                             std::span<std::byte> output, std::span<std::uint8_t> eventOrdinalScratch,
                                                             std::span<const std::byte> validatedConstruction);

    /** @brief Parses only into private unleased staging; full owner
     * compatibility/projection admission follows before publication.
     * @details Unknown sections, truncation, trailing bytes and noncanonical
     * optional representations reject. Failure may change this unpublished scratch,
     * never a leased previous capture or live native world. The returned construction bytes require full manifest
     * validation or exact comparison with an already validated owner manifest before any publication/native effect.
     */
    [[nodiscard]] Result<ParsedSnapshotEnvelope> DecodeSnapshotEnvelope(std::span<const std::byte> input, SolverStorage &solver,
                                                                        std::span<PhysicsStateEventPair> pairs,
                                                                        std::span<PhysicsStatePublishedEvent> events,
                                                                        std::span<std::uint8_t> eventOrdinalScratch);
}  // namespace Horo::Physics::StateDetail
