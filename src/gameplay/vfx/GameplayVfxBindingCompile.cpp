#include "GameplayVfxBindingInternal.h"

namespace Horo::Gameplay::VfxBindingDetail {
    namespace {
        struct CompileContext final {
            const Assets::AssetRegistrySnapshot &registry;
            const Assets::IAssetProvider &provider;
            std::span<const Vfx::VfxSpawnAssetInfo> prepared;
            const CancellationToken &cancellation;
            std::array<Assets::AssetId, Vfx::VfxSpawnHardLimits::Effects> checked{};
            std::uint32_t checkedCount{};
        };

        [[nodiscard]] Result<const Vfx::VfxSpawnAssetInfo *> CheckAsset(CompileContext &context, const Assets::AssetId asset) {
            const auto *record = context.registry.Find(asset);
            if (!record)
                return Reject<const Vfx::VfxSpawnAssetInfo *>(GameplayVfxErrors::MissingAsset);
            if (record->type.Value() != "core.vfx.effect")
                return Reject<const Vfx::VfxSpawnAssetInfo *>(GameplayVfxErrors::AssetTypeMismatch);
            const auto info = std::ranges::find(context.prepared, asset, &Vfx::VfxSpawnAssetInfo::asset);
            if (info == context.prepared.end())
                return Reject<const Vfx::VfxSpawnAssetInfo *>(GameplayVfxErrors::AssetUnavailable);
            if (const auto checked = std::span{context.checked.data(), context.checkedCount};
                std::ranges::find(checked, asset) != checked.end())
                return Result<const Vfx::VfxSpawnAssetInfo *>::Success(&*info);
            auto exists = context.provider.Exists(asset, context.cancellation);
            if (exists.HasError())
                return Result<const Vfx::VfxSpawnAssetInfo *>::Failure(WrapError(GameplayVfxErrors::AssetUnavailable, exists.ErrorValue()));
            if (!exists.Value())
                return Reject<const Vfx::VfxSpawnAssetInfo *>(GameplayVfxErrors::AssetUnavailable);
            if (context.checkedCount == context.checked.size())
                return Reject<const Vfx::VfxSpawnAssetInfo *>(GameplayVfxErrors::LimitExceeded);
            context.checked[context.checkedCount++] = asset;
            return Result<const Vfx::VfxSpawnAssetInfo *>::Success(&*info);
        }

        [[nodiscard]] const Vfx::VfxSpawnParameter *FindChannel(const Vfx::VfxSpawnAssetInfo &asset, const std::uint16_t channel) noexcept {
            const auto channels = std::span{asset.parameters.data(), asset.parameterCount};
            const auto found = std::ranges::find(channels, channel, &Vfx::VfxSpawnParameter::channel);
            return found == channels.end() ? nullptr : &*found;
        }

        [[nodiscard]] bool FitsRange(const Vfx::VfxSpawnParameter *target, const float minimum, const float maximum) noexcept {
            return target && minimum >= target->minimum && maximum <= target->maximum;
        }

        [[nodiscard]] Result<void> CompileConstants(const GameplayVfxLayer &layer, const Vfx::VfxSpawnAssetInfo &asset,
                                                    Detail::GameplayVfxCompiledLayer &output) {
            if (layer.constants.count != 0 && layer.constants.schema != asset.parameterSchema)
                return Reject<void>(GameplayVfxErrors::ParameterMismatch);
            for (const auto &constant : layer.constants.Values()) {
                if (!FitsRange(FindChannel(asset, constant.channel), constant.value, constant.value))
                    return Reject<void>(GameplayVfxErrors::ParameterMismatch);
                output.parameters[output.parameterCount++] = {constant.channel, 0, constant.value, false};
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> CompileRoutes(const GameplayVfxBinding &binding, const GameplayVfxLayer &layer,
                                                 const Vfx::VfxSpawnAssetInfo &asset, Detail::GameplayVfxCompiledLayer &output) {
            for (const auto &route : layer.routes) {
                const auto input = std::ranges::find(binding.inputs, route.source, &Vfx::VfxSpawnParameter::channel);
                if (input == binding.inputs.end())
                    return Reject<void>(GameplayVfxErrors::ParameterMismatch);
                if (!FitsRange(FindChannel(asset, route.target), input->minimum, input->maximum))
                    return Reject<void>(GameplayVfxErrors::ParameterMismatch);
                const auto ordinal = static_cast<std::uint16_t>(input - binding.inputs.begin());
                output.parameters[output.parameterCount++] = {route.target, ordinal, 0.0F, true};
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> CompileLayer(CompileContext &context, const GameplayVfxBinding &binding, const GameplayVfxLayer &layer,
                                                Detail::GameplayVfxCompiledLayer &output) {
            const auto available = CheckAsset(context, layer.effectAsset);
            if (available.HasError())
                return Result<void>::Failure(available.ErrorValue());
            const auto &asset = *available.Value();
            if (binding.requirement == Vfx::VfxRequirementClass::GameplayRequired &&
                (!asset.requiredGameplay || asset.requiredReserve == 0))
                return Reject<void>(GameplayVfxErrors::BindingInvalid);
            output.asset = layer.effectAsset;
            output.scale = layer.scale;
            output.schema = asset.parameterSchema;
            if (auto constants = CompileConstants(layer, asset, output); constants.HasError())
                return constants;
            if (auto routes = CompileRoutes(binding, layer, asset, output); routes.HasError())
                return routes;
            std::ranges::sort(std::span{output.parameters.data(), output.parameterCount}, {},
                              &Detail::GameplayVfxCompiledParameter::target);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<Detail::GameplayVfxCompiledBinding> CompileBinding(CompileContext &context,
                                                                                const GameplayVfxBinding &binding) {
            Detail::GameplayVfxCompiledBinding output;
            output.destination = binding.destination;
            output.event = binding.event;
            output.tag = binding.tag;
            output.schemaVersion = binding.schemaVersion;
            output.payloadSchema = binding.payloadSchema;
            output.requirement = binding.requirement;
            output.inputCount = static_cast<std::uint32_t>(binding.inputs.size());
            std::ranges::copy(binding.inputs, output.inputs.begin());
            output.layerCount = static_cast<std::uint32_t>(binding.layers.size());
            for (std::size_t index = 0; index < binding.layers.size(); ++index)
                if (auto layer = CompileLayer(context, binding, binding.layers[index], output.layers[index]); layer.HasError())
                    return Result<Detail::GameplayVfxCompiledBinding>::Failure(layer.ErrorValue());
            return Result<Detail::GameplayVfxCompiledBinding>::Success(std::move(output));
        }

        [[nodiscard]] bool ValidPreparedInfo(const Vfx::VfxSpawnAssetInfo &info) noexcept {
            if (!info.asset.IsValid() || info.maximumInstances == 0 || info.requiredReserve > info.maximumInstances)
                return false;
            if (info.parameterCount > Vfx::VfxSpawnHardLimits::Parameters || (info.parameterSchema == 0) != (info.parameterCount == 0))
                return false;
            for (const auto &parameter : std::span{info.parameters.data(), info.parameterCount})
                if (!std::isfinite(parameter.minimum) || !std::isfinite(parameter.maximum) || parameter.minimum > parameter.maximum)
                    return false;
            return true;
        }

        [[nodiscard]] bool ValidPreparedPalette(const std::span<const Vfx::VfxSpawnAssetInfo> prepared) noexcept {
            if (prepared.empty() || prepared.size() > Vfx::VfxSpawnHardLimits::Effects)
                return false;
            for (std::size_t index = 0; index < prepared.size(); ++index) {
                if (!ValidPreparedInfo(prepared[index]))
                    return false;
                for (std::size_t previous = 0; previous < index; ++previous)
                    if (prepared[index].asset == prepared[previous].asset)
                        return false;
            }
            return true;
        }
    }  // namespace

    Result<std::unique_ptr<Detail::GameplayVfxCompiledBindings>> Compile(GameplayVfxBindingDocument document,
                                                                         Assets::AssetRegistrySnapshot registry,
                                                                         const Assets::IAssetProvider &provider,
                                                                         const std::span<const Vfx::VfxSpawnAssetInfo> prepared,
                                                                         const CancellationToken &cancellation) {
        if (!ValidPreparedPalette(prepared))
            return Reject<std::unique_ptr<Detail::GameplayVfxCompiledBindings>>(GameplayVfxErrors::ScopeInvalid);
        auto output = std::make_unique<Detail::GameplayVfxCompiledBindings>();
        output->generation = document.generation;
        output->registry = std::move(registry);
        output->bindings.reserve(document.bindings.size());
        CompileContext context{output->registry, provider, prepared, cancellation};
        for (const auto &binding : document.bindings) {
            if (cancellation.IsCancellationRequested())
                return Reject<std::unique_ptr<Detail::GameplayVfxCompiledBindings>>(GameplayVfxErrors::Cancelled);
            auto compiled = CompileBinding(context, binding);
            if (compiled.HasError())
                return Result<std::unique_ptr<Detail::GameplayVfxCompiledBindings>>::Failure(compiled.ErrorValue());
            output->bindings.push_back(std::move(compiled).Value());
        }
        return Result<std::unique_ptr<Detail::GameplayVfxCompiledBindings>>::Success(std::move(output));
    }
}  // namespace Horo::Gameplay::VfxBindingDetail
