#include "GameplayVfxBindingInternal.h"
#include "Horo/Foundation/Utf8.h"

namespace Horo::Gameplay::VfxBindingDetail {
    namespace {
        template <typename Range, typename Projection> [[nodiscard]] bool StrictOrder(const Range &values, const Projection projection) {
            std::optional<std::uint64_t> previous;
            for (const auto &value : values) {
                const auto next = static_cast<std::uint64_t>(projection(value));
                if (previous && next <= *previous)
                    return false;
                previous = next;
            }
            return true;
        }

        [[nodiscard]] bool ValidInputs(const GameplayVfxBinding &binding) noexcept {
            if ((binding.payloadSchema == 0) != binding.inputs.empty())
                return false;
            if (!StrictOrder(binding.inputs, [](const auto &input) {
                return input.channel;
            }))
                return false;
            for (const auto &input : binding.inputs)
                if (!std::isfinite(input.minimum) || !std::isfinite(input.maximum) || input.minimum > input.maximum)
                    return false;
            return true;
        }

        [[nodiscard]] bool ValidConstants(const Vfx::VfxParameterBlock &block) noexcept {
            if ((block.schema == 0) != (block.count == 0))
                return false;
            if (!StrictOrder(block.Values(), [](const auto &value) {
                return value.channel;
            }))
                return false;
            return std::ranges::all_of(block.Values(), [](const auto &value) {
                return std::isfinite(value.value);
            });
        }

        [[nodiscard]] bool ValidRoutes(const GameplayVfxBinding &binding, const GameplayVfxLayer &layer) noexcept {
            if (!StrictOrder(layer.routes, [](const auto &route) {
                return route.target;
            }))
                return false;
            for (const auto &route : layer.routes) {
                if (std::ranges::find(binding.inputs, route.source, &Vfx::VfxSpawnParameter::channel) == binding.inputs.end())
                    return false;
                if (std::ranges::find(layer.constants.Values(), route.target, &Vfx::VfxParameterValue::channel) !=
                    layer.constants.Values().end())
                    return false;
            }
            return true;
        }

        [[nodiscard]] bool WithinLayerLimits(const GameplayVfxLayer &layer) noexcept {
            return layer.constants.count <= Vfx::VfxSpawnHardLimits::Parameters &&
                   layer.routes.size() <= Vfx::VfxSpawnHardLimits::Parameters &&
                   layer.constants.count + layer.routes.size() <= Vfx::VfxSpawnHardLimits::Parameters;
        }

        [[nodiscard]] Result<void> ValidateLayer(const GameplayVfxBinding &binding, const GameplayVfxLayer &layer) {
            if (!WithinLayerLimits(layer))
                return Reject<void>(GameplayVfxErrors::LimitExceeded);
            if (!layer.effectAsset.IsValid() || !std::isfinite(layer.scale) || layer.scale <= 0.0F)
                return Reject<void>(GameplayVfxErrors::BindingInvalid);
            if (!ValidConstants(layer.constants) || !ValidRoutes(binding, layer))
                return Reject<void>(GameplayVfxErrors::ParameterMismatch);
            return Result<void>::Success();
        }

        [[nodiscard]] bool ValidSelector(const GameplayVfxBinding &binding) noexcept {
            const bool event = binding.event.IsValid();
            if (event == binding.tag.IsValid())
                return false;
            return !event || ValidText(binding.event.Value());
        }

        [[nodiscard]] bool ValidBindingShape(const GameplayVfxBinding &binding) noexcept {
            return binding.destination != 0 && binding.schemaVersion != 0 && !binding.layers.empty() &&
                   binding.requirement < Vfx::VfxRequirementClass::Count;
        }

        [[nodiscard]] Result<void> ValidateBinding(const GameplayVfxBinding &binding) {
            if (binding.inputs.size() > Vfx::VfxSpawnHardLimits::Parameters || binding.layers.size() > Vfx::VfxSpawnHardLimits::Layers)
                return Reject<void>(GameplayVfxErrors::LimitExceeded);
            if (!ValidBindingShape(binding) || !ValidSelector(binding))
                return Reject<void>(GameplayVfxErrors::BindingInvalid);
            if (!ValidInputs(binding))
                return Reject<void>(GameplayVfxErrors::ParameterMismatch);
            for (const auto &layer : binding.layers)
                if (auto valid = ValidateLayer(binding, layer); valid.HasError())
                    return valid;
            return Result<void>::Success();
        }

        [[nodiscard]] bool UniqueSelector(const GameplayVfxBindingDocument &document, const std::size_t index) noexcept {
            const auto &binding = document.bindings[index];
            for (std::size_t previous = 0; previous < index; ++previous) {
                const auto &other = document.bindings[previous];
                if (binding.event.IsValid() && other.event == binding.event)
                    return false;
                if (binding.tag.IsValid() && other.tag == binding.tag)
                    return false;
            }
            return true;
        }
    }  // namespace

    bool ValidText(const std::string_view text) noexcept {
        if (text.empty() || text.size() > GameplayVfxBindingLimits::TextBytes || !IsValidUtf8ScalarSequence(text))
            return false;
        return std::ranges::none_of(text, [](const unsigned char value) {
            return value < 0x20 || value == 0x7f;
        });
    }

    Result<void> ValidateDocument(const GameplayVfxBindingDocument &document) {
        if (document.version != 1)
            return Reject<void>(GameplayVfxErrors::VersionUnsupported);
        if (document.generation == 0 || document.bindings.empty())
            return Reject<void>(GameplayVfxErrors::BindingInvalid);
        if (document.bindings.size() > GameplayVfxBindingLimits::Bindings)
            return Reject<void>(GameplayVfxErrors::LimitExceeded);
        std::uint64_t previous{};
        for (std::size_t index = 0; index < document.bindings.size(); ++index) {
            const auto &binding = document.bindings[index];
            if (binding.destination <= previous || !UniqueSelector(document, index))
                return Reject<void>(GameplayVfxErrors::BindingInvalid);
            if (auto valid = ValidateBinding(binding); valid.HasError())
                return valid;
            previous = binding.destination;
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Gameplay::VfxBindingDetail
