#pragma once

#include "Horo/Vfx/VfxWorld.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace Horo::Vfx::Detail {
    /** @brief One reusable canonical kernel; no simulation pointer or borrowed view escapes the World. */
    struct VfxSpawnInstance final {
        VfxSpawnInstance(CpuParticleSimulator value, EffectSystemId identity) : simulation(std::move(value)), instance(identity) {}

        CpuParticleSimulator simulation;
        EffectSystemId instance;
        VfxSpawnRequest request;
        float elapsed{};
        bool live{};
        bool firstStep{};
    };

    /** @brief Prepared pool and its exact disjoint same-Scene kernel range. */
    struct VfxSpawnAsset final {
        explicit VfxSpawnAsset(EffectInstancePool value) : pool(std::move(value)) {}

        EffectInstancePool pool;
        VfxSpawnAssetInfo info;
        EmitterId emitter;
        Render::MaterialRuntimeId material;
        Render::RenderMeshHandle mesh;
        Render::VfxParticlePass blend{Render::VfxParticlePass::Additive};
        Render::VfxPrimitiveTopology topology{Render::VfxPrimitiveTopology::CameraFacingBillboard};
        std::vector<VfxSpawnInstance> instances;
        std::uint32_t firstSlot{};
        std::uint32_t initialBurst{};
        float durationSeconds{};
        bool requiredGameplay{};
    };

    struct VfxSpawnOwner final {
        VfxIdentityScope owner;
        std::uint64_t generation{};
        bool active{};
    };

    /** @brief Per-producer monotonic dedup watermark, retained after result acknowledgement and owner revocation. */
    struct VfxSpawnProducer final {
        VfxIdentityScope owner;
        std::uint64_t producer{};
        std::uint64_t sequence{};
    };

    struct VfxWorldSpawnState final {
        VfxSpawnBudget budget;
        std::vector<VfxSpawnAsset> assets;
        std::vector<VfxSpawnRequest> pending;
        std::vector<VfxSpawnOutcome> results;
        std::vector<VfxSpawnOwner> owners;
        std::vector<VfxSpawnProducer> producers;
        std::uint32_t pendingCount{};
        std::uint32_t resultCount{};
        std::uint64_t tick{};
        std::uint64_t preparedBytes{};
        bool busy{};
    };
}  // namespace Horo::Vfx::Detail

namespace Horo::Vfx::SpawnDetail {
    /** @brief Finds one asset in a bounded cold-prepared palette. */
    [[nodiscard]] inline Detail::VfxSpawnAsset *FindAsset(Detail::VfxWorldSpawnState &state, const Assets::AssetId asset) noexcept {
        for (auto &entry : state.assets)
            if (entry.info.asset == asset)
                return &entry;
        return nullptr;
    }

    /** @brief Checks scalar arguments against genuine cooked GameplayInput declarations. */
    [[nodiscard]] inline bool ParametersMatch(const VfxParameterBlock &block, const VfxSpawnAssetInfo &asset) noexcept {
        if (block.count == 0)
            return block.schema == 0;
        if (block.schema != asset.parameterSchema)
            return false;
        const auto channels = std::span{asset.parameters.data(), asset.parameterCount};
        for (const auto &value : block.Values()) {
            const auto found = std::ranges::find(channels, value.channel, &VfxSpawnParameter::channel);
            if (found == channels.end() || value.value < found->minimum || value.value > found->maximum)
                return false;
        }
        return true;
    }

    [[nodiscard]] Result<std::unique_ptr<Detail::VfxWorldSpawnState>> Prepare(VfxIdentityScope scene,
                                                                              std::span<const VfxSpawnAssetPreparation> assets,
                                                                              VfxSpawnBudget budget);
    [[nodiscard]] VfxSpawnStatus ValidateBudget(const VfxSpawnBudget &budget) noexcept;
    [[nodiscard]] VfxSpawnOutcome Enqueue(Detail::VfxWorldSpawnState &state, VfxIdentityScope scene,
                                          std::span<const VfxSpawnRequest> requests) noexcept;
    [[nodiscard]] VfxSpawnUpdate Update(Detail::VfxWorldSpawnState &state, std::uint64_t tick, float deltaSeconds) noexcept;
    void Retire(Detail::VfxSpawnAsset &asset, Detail::VfxSpawnInstance &instance) noexcept;
}  // namespace Horo::Vfx::SpawnDetail
