#include "GameplayVfxBindingInternal.h"

#include <nlohmann/json.hpp>

namespace Horo::Gameplay {
    namespace {
        using Json = nlohmann::json;

        [[nodiscard]] Json Constants(const Vfx::VfxParameterBlock &block) {
            auto values = Json::array();
            for (const auto &value : block.Values())
                values.push_back({{"channel", value.channel}, {"value", value.value}});
            return {{"schema", block.schema}, {"values", std::move(values)}};
        }

        [[nodiscard]] Json Layer(const GameplayVfxLayer &layer) {
            auto routes = Json::array();
            for (const auto &route : layer.routes)
                routes.push_back({{"source", route.source}, {"target", route.target}});
            return {{"effectAsset", layer.effectAsset.ToString()},
                    {"scale", layer.scale},
                    {"constants", Constants(layer.constants)},
                    {"routes", std::move(routes)}};
        }

        [[nodiscard]] Json Binding(const GameplayVfxBinding &binding) {
            auto inputs = Json::array();
            for (const auto &input : binding.inputs)
                inputs.push_back({{"channel", input.channel}, {"minimum", input.minimum}, {"maximum", input.maximum}});
            auto layers = Json::array();
            for (const auto &layer : binding.layers)
                layers.push_back(Layer(layer));
            Json output{{"destination", binding.destination},
                        {"schemaVersion", binding.schemaVersion},
                        {"payloadSchema", binding.payloadSchema},
                        {"inputs", std::move(inputs)},
                        {"requirement", binding.requirement == Vfx::VfxRequirementClass::Cosmetic ? "cosmetic" : "required"},
                        {"layers", std::move(layers)}};
            if (binding.event.IsValid())
                output["event"] = binding.event.Value();
            else
                output["tag"] = binding.tag.value;
            return output;
        }

        [[nodiscard]] std::string Encode(const GameplayVfxBindingDocument &document) {
            auto bindings = Json::array();
            for (const auto &binding : document.bindings)
                bindings.push_back(Binding(binding));
            const Json output{{"version", document.version}, {"generation", document.generation}, {"bindings", std::move(bindings)}};
            return output.dump();
        }
    }  // namespace

    /** @copydoc EncodeGameplayVfxBindings */
    Result<std::string> EncodeGameplayVfxBindings(const GameplayVfxBindingDocument &document) {
        try {
            if (auto valid = VfxBindingDetail::ValidateDocument(document); valid.HasError())
                return Result<std::string>::Failure(valid.ErrorValue());
            auto bytes = Encode(document);
            if (bytes.size() > GameplayVfxBindingLimits::SourceBytes)
                return VfxBindingDetail::Reject<std::string>(GameplayVfxErrors::LimitExceeded);
            return Result<std::string>::Success(std::move(bytes));
        } catch (const std::bad_alloc &) {
            return VfxBindingDetail::Reject<std::string>(GameplayVfxErrors::AllocationFailed);
        } catch (const Json::exception &) {
            return VfxBindingDetail::Reject<std::string>(GameplayVfxErrors::Malformed);
        }
    }
}  // namespace Horo::Gameplay
