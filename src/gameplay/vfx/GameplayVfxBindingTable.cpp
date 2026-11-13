#include "GameplayVfxBindingInternal.h"

#include <new>

namespace Horo::Gameplay {
    namespace {
        [[nodiscard]] GameplayVfxLookup Lookup(const Detail::GameplayVfxCompiledBindings &state,
                                               const Detail::GameplayVfxCompiledBinding &binding,
                                               const std::uint32_t schemaVersion) noexcept {
            if (binding.schemaVersion != schemaVersion)
                return {.status = Vfx::VfxSpawnStatus::ParameterSchemaMismatch};
            return {Vfx::VfxSpawnStatus::Accepted, {state.generation, binding.destination}, binding.payloadSchema};
        }

        [[nodiscard]] bool ValidPayload(const Detail::GameplayVfxCompiledBinding &binding,
                                        const Vfx::VfxParameterBlock &parameters) noexcept {
            if (parameters.schema != binding.payloadSchema || parameters.count != binding.inputCount)
                return false;
            for (std::size_t index = 0; index < binding.inputCount; ++index) {
                const auto &value = parameters.values[index];
                const auto &expected = binding.inputs[index];
                if (value.channel != expected.channel || !std::isfinite(value.value) || value.value < expected.minimum ||
                    value.value > expected.maximum)
                    return false;
            }
            return true;
        }

        /** @brief Projects precompiled source ordinals/constants without field parsing, schema guesses or container growth. */
        [[nodiscard]] Vfx::VfxParameterBlock Parameters(const Detail::GameplayVfxCompiledLayer &layer,
                                                        const Vfx::VfxParameterBlock &source) noexcept {
            Vfx::VfxParameterBlock output;
            output.count = layer.parameterCount;
            output.schema = output.count == 0 ? 0 : layer.schema;
            for (std::uint32_t index = 0; index < layer.parameterCount; ++index) {
                const auto &parameter = layer.parameters[index];
                const auto value = parameter.fromInput ? source.values[parameter.sourceOrdinal].value : parameter.constant;
                output.values[index] = {parameter.target, value};
            }
            return output;
        }

        [[nodiscard]] GameplayVfxResolveOutcome ResolveLayers(const Detail::GameplayVfxCompiledBindings &state,
                                                              const Detail::GameplayVfxCompiledBinding &binding,
                                                              const GameplayVfxEventPayload &payload, Vfx::VfxSpawnRequest basis,
                                                              const std::span<Vfx::VfxSpawnRequest> output) noexcept {
            if (output.size() < binding.layerCount)
                return {Vfx::VfxSpawnStatus::ResourceBudgetExceeded, 0};
            if (!ValidPayload(binding, payload.parameters))
                return {Vfx::VfxSpawnStatus::ParameterSchemaMismatch, 0};
            std::array<Vfx::VfxSpawnRequest, Vfx::VfxSpawnHardLimits::Layers> candidate;
            basis.bindingGeneration = state.generation;
            basis.destination = binding.destination;
            basis.requirement = binding.requirement;
            basis.transform = payload.transform;
            basis.impactNormal = payload.impactNormal;
            for (std::uint32_t index = 0; index < binding.layerCount; ++index) {
                const auto &layer = binding.layers[index];
                auto &request = candidate[index];
                request = basis;
                request.layerOrdinal = index;
                request.effectAsset = layer.asset;
                request.scale = payload.scale * layer.scale;
                request.parameters = Parameters(layer, payload.parameters);
                if (const auto valid = Vfx::ValidateVfxSpawnRequest(request); valid != Vfx::VfxSpawnStatus::Accepted)
                    return {valid, 0};
            }
            std::copy_n(candidate.begin(), binding.layerCount, output.begin());
            return {Vfx::VfxSpawnStatus::Accepted, binding.layerCount};
        }
    }  // namespace

    GameplayVfxBindingTable::GameplayVfxBindingTable(std::unique_ptr<Detail::GameplayVfxCompiledBindings> state)
        : state_(std::move(state)) {}

    /** @copydoc GameplayVfxBindingTable::~GameplayVfxBindingTable */
    GameplayVfxBindingTable::~GameplayVfxBindingTable() = default;

    /** @copydoc GameplayVfxBindingTable::Cook */
    Result<std::shared_ptr<const GameplayVfxBindingTable>> GameplayVfxBindingTable::Cook(
        GameplayVfxBindingDocument document, Assets::AssetRegistrySnapshot registry, const Assets::IAssetProvider &provider,
        const std::span<const Vfx::VfxSpawnAssetInfo> prepared, const CancellationToken &cancellation) {
        using Output = Result<std::shared_ptr<const GameplayVfxBindingTable>>;
        try {
            if (auto valid = VfxBindingDetail::ValidateDocument(document); valid.HasError())
                return Output::Failure(valid.ErrorValue());
            auto compiled = VfxBindingDetail::Compile(std::move(document), std::move(registry), provider, prepared, cancellation);
            if (compiled.HasError())
                return Output::Failure(compiled.ErrorValue());
            if (cancellation.IsCancellationRequested())
                return VfxBindingDetail::Reject<std::shared_ptr<const GameplayVfxBindingTable>>(GameplayVfxErrors::Cancelled);
            return Output::Success(
                std::shared_ptr<const GameplayVfxBindingTable>{new GameplayVfxBindingTable{std::move(compiled).Value()}});
        } catch (const std::bad_alloc &) {
            return VfxBindingDetail::Reject<std::shared_ptr<const GameplayVfxBindingTable>>(GameplayVfxErrors::AllocationFailed);
        } catch (...) {
            return VfxBindingDetail::Reject<std::shared_ptr<const GameplayVfxBindingTable>>(GameplayVfxErrors::AssetUnavailable);
        }
    }

    /** @copydoc GameplayVfxBindingTable::Generation */
    std::uint64_t GameplayVfxBindingTable::Generation() const noexcept {
        return state_->generation;
    }

    /** @copydoc GameplayVfxBindingTable::FindEvent */
    GameplayVfxLookup GameplayVfxBindingTable::FindEvent(const std::string_view event, const std::uint32_t schemaVersion) const noexcept {
        for (const auto &binding : state_->bindings)
            if (binding.event.IsValid() && binding.event.Value() == event)
                return Lookup(*state_, binding, schemaVersion);
        return {.status = Vfx::VfxSpawnStatus::UnknownEvent};
    }

    /** @copydoc GameplayVfxBindingTable::FindTag */
    GameplayVfxLookup GameplayVfxBindingTable::FindTag(const GameplayVfxEffectTag tag, const std::uint32_t schemaVersion) const noexcept {
        for (const auto &binding : state_->bindings)
            if (tag.IsValid() && binding.tag == tag)
                return Lookup(*state_, binding, schemaVersion);
        return {.status = Vfx::VfxSpawnStatus::UnknownTag};
    }

    /** @copydoc GameplayVfxBindingTable::Resolve */
    GameplayVfxResolveOutcome GameplayVfxBindingTable::Resolve(const GameplayVfxEventKey key, const GameplayVfxEventPayload &payload,
                                                               const Vfx::VfxSpawnRequest basis,
                                                               const std::span<Vfx::VfxSpawnRequest> output) const noexcept {
        if (key.generation != state_->generation)
            return {Vfx::VfxSpawnStatus::StaleBinding, 0};
        const auto binding = VfxBindingDetail::FindDestination(*state_, key.destination);
        if (!binding)
            return {Vfx::VfxSpawnStatus::UnknownEvent, 0};
        return ResolveLayers(*state_, *binding, payload, basis, output);
    }
}  // namespace Horo::Gameplay
