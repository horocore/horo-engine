#pragma once

#include "Horo/Gameplay/GameplayVfxBindingTable.h"

#include <algorithm>
#include <cmath>

namespace Horo::Gameplay::Detail {
    struct GameplayVfxCompiledParameter final {
        std::uint16_t target{};
        std::uint16_t sourceOrdinal{};
        float constant{};
        bool fromInput{};
    };

    struct GameplayVfxCompiledLayer final {
        Assets::AssetId asset;
        float scale{1.0F};
        std::uint64_t schema{};
        std::array<GameplayVfxCompiledParameter, Vfx::VfxSpawnHardLimits::Parameters> parameters{};
        std::uint32_t parameterCount{};
    };

    struct GameplayVfxCompiledBinding final {
        std::uint64_t destination{};
        GameplayEventTypeId event;
        GameplayVfxEffectTag tag;
        std::uint32_t schemaVersion{};
        std::uint64_t payloadSchema{};
        std::array<Vfx::VfxSpawnParameter, Vfx::VfxSpawnHardLimits::Parameters> inputs{};
        std::uint32_t inputCount{};
        Vfx::VfxRequirementClass requirement{};
        std::array<GameplayVfxCompiledLayer, Vfx::VfxSpawnHardLimits::Layers> layers{};
        std::uint32_t layerCount{};
    };

    struct GameplayVfxCompiledBindings final {
        std::uint64_t generation{};
        Assets::AssetRegistrySnapshot registry;
        std::vector<GameplayVfxCompiledBinding> bindings;
    };

    struct GameplayVfxBindingAccess final {
        [[nodiscard]] static const GameplayVfxCompiledBindings &State(const GameplayVfxBindingTable &table) noexcept {
            return *table.state_;
        }
    };
}  // namespace Horo::Gameplay::Detail

namespace Horo::Gameplay::VfxBindingDetail {
    template <typename Value> [[nodiscard]] inline Result<Value> Reject(const ErrorCodeDescriptor &descriptor) {
        return Result<Value>::Failure(MakeError(descriptor));
    }

    [[nodiscard]] Result<void> ValidateDocument(const GameplayVfxBindingDocument &document);
    [[nodiscard]] Result<std::unique_ptr<Detail::GameplayVfxCompiledBindings>> Compile(GameplayVfxBindingDocument document,
                                                                                       Assets::AssetRegistrySnapshot registry,
                                                                                       const Assets::IAssetProvider &provider,
                                                                                       std::span<const Vfx::VfxSpawnAssetInfo> prepared,
                                                                                       const CancellationToken &cancellation);
    [[nodiscard]] bool ValidText(std::string_view text) noexcept;

    [[nodiscard]] inline const Detail::GameplayVfxCompiledBinding *FindDestination(const Detail::GameplayVfxCompiledBindings &state,
                                                                                   const std::uint64_t destination) noexcept {
        const auto found = std::ranges::lower_bound(state.bindings, destination, {}, &Detail::GameplayVfxCompiledBinding::destination);
        return found != state.bindings.end() && found->destination == destination ? &*found : nullptr;
    }
}  // namespace Horo::Gameplay::VfxBindingDetail
