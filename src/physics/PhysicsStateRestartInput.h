#pragma once
#include "PhysicsStateConstructionManifestOwner.h"
#include "PhysicsStateEnvelope.h"

namespace Horo::Physics::StateDetail {
    /** @brief Independent cold receiving-owner ceilings. No values are accepted from the incoming wire as a budget.
     * The caller additionally checks these ceilings against its actual World/runtime admission before construction.
     */
    struct RestartInputLimits final {
        PhysicsStateLimits solver;
        PhysicsStateConstructionManifestLimits construction;
        std::uint32_t maximumEventPairs{}, maximumEvents{};
        std::size_t maximumWireBytes{}, maximumRetainedBytes{};
    };

    /** @brief Fully owned, immutable canonical input prepared before any native reconstruction.
     * SAME Physics codecs validate every solver/constructor/event scalar into bounded private storage.
     * This object grants no World, content, Scene or publication authority. The receiving owner independently authenticates
     * content/build/image compatibility and retains its actual content/code leases before calling native construction.
     * A failed decode cannot change this already-prepared value or any live World. All allocation is cold.
     */
    class RestartInput final {
    public:
        [[nodiscard]] static Result<std::unique_ptr<RestartInput>> Prepare(std::span<const std::byte>, const RestartInputLimits &,
                                                                           PhysicsStateFpControl actualReceivingControls);

        [[nodiscard]] const ParsedSnapshotEnvelope &Envelope() const noexcept {
            return envelope_;
        }

        [[nodiscard]] const PhysicsStateConstructionManifest &Construction() const noexcept {
            return construction_->View();
        }

        [[nodiscard]] const PhysicsStateConstructionManifestLimits &ConstructionLimits() const noexcept {
            return construction_->Limits();
        }

        [[nodiscard]] std::span<const std::byte> Bytes() const noexcept {
            return bytes_;
        }

        [[nodiscard]] std::size_t RetainedPayloadBytes() const noexcept;

    private:
        RestartInput() = default;
        [[nodiscard]] Result<void> Allocate(std::span<const std::byte>, const RestartInputLimits &, const SnapshotEnvelopeSections &,
                                            const PhysicsStateConstructionManifestLimits &);
        [[nodiscard]] Result<void> Decode(PhysicsStateFpControl);
        std::unique_ptr<SolverStorage> solver_;
        std::unique_ptr<ConstructionManifestOwner> construction_;
        std::vector<std::byte> bytes_;
        std::vector<PhysicsStateEventPair> pairs_;
        std::vector<PhysicsStatePublishedEvent> events_;
        std::vector<std::uint8_t> ordinals_;
        ParsedSnapshotEnvelope envelope_;
    };
}  // namespace Horo::Physics::StateDetail
