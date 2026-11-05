#pragma once

/** @file VfxSpawnPreparation.h
 * @brief Cold CPU effect recipes and one explicit scene-wide spawn reservation.
 */

#include "Horo/Runtime/Render/StandardPbrMaterial.h"
#include "Horo/Vfx/CpuParticleSimulator.h"
#include "Horo/Vfx/EffectInstancePool.h"
#include "Horo/Vfx/VfxSpawnRequest.h"

#include <memory>

namespace Horo::Vfx {
    /** @brief Explicit finite cold reservation, shared by all prepared recipes rather than multiplied per asset. */
    struct VfxSpawnBudget final {
        std::uint32_t maximumPending{64};
        std::uint32_t maximumResults{64};
        std::uint32_t maximumOwners{16};
        std::uint32_t maximumProducers{16};
        std::uint32_t maximumInstances{64};
        std::uint32_t maximumParticles{16'384};
        std::uint64_t maximumBytes{64U * 1024U * 1024U};
        float maximumDeltaSeconds{1.0F};
    };

    /** @brief Scalar schema entry copied from the genuine prepared CPU GameplayInput declaration. */
    struct VfxSpawnParameter final {
        std::uint16_t channel{};
        float minimum{};
        float maximum{};
    };

    /** @brief One asset's finite prepared CPU recipe; no graph compilation or native GPU resource admission is implied.
     * @details The canonical simulator copies all supported kernel inputs during preparation. Collision adapters and arbitrary
     * stage callbacks are unsupported here; renderer material/mesh handles remain host-admitted values.
     */
    struct VfxSpawnAssetPreparation final {
        Assets::AssetId asset;
        std::shared_ptr<const ParticleSystemDescriptor> particles;
        CpuParticleSimulatorCreateInfo simulation;
        std::uint32_t maximumInstances{1};
        std::uint32_t requiredReserve{};
        std::uint32_t initialBurst{1};
        float durationSeconds{1.0F}; /**< Explicit effect lifetime, after which all particles retire; no unbounded tail. */
        std::uint64_t parameterSchema{};
        Render::MaterialRuntimeId material;
        Render::MaterialAlphaMode materialBlend{Render::MaterialAlphaMode::Additive};
    };

    /** @brief Immutable copied recipe availability; sufficient for cold mapping validation without exposing storage. */
    struct VfxSpawnAssetInfo final {
        Assets::AssetId asset;
        std::uint64_t parameterSchema{};
        std::array<VfxSpawnParameter, VfxSpawnHardLimits::Parameters> parameters{};
        std::uint32_t parameterCount{};
        std::uint32_t maximumInstances{};
        std::uint32_t requiredReserve{};
        bool requiredGameplay{};
    };

    /** @brief Bounded owner phase report; detailed correlated outcomes remain in the result prefix. */
    struct VfxSpawnUpdate final {
        VfxSpawnStatus status{VfxSpawnStatus::Empty};
        std::uint32_t consumed{};
        std::uint32_t live{};
        std::uint32_t retired{};
        VfxSpawnOutcome firstFailure;
    };
}  // namespace Horo::Vfx
