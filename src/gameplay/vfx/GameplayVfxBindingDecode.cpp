#include "GameplayVfxBindingInternal.h"

#include <limits>
#include <nlohmann/json.hpp>
#include <set>

namespace Horo::Gameplay {
    namespace {
        using Json = nlohmann::json;

        struct DecodeFailure final {
            const ErrorCodeDescriptor *descriptor;
        };

        [[noreturn]] void Fail(const ErrorCodeDescriptor &descriptor = GameplayVfxErrors::Malformed) {
            throw DecodeFailure{&descriptor};
        }

        void Object(const Json &value, const std::initializer_list<std::string_view> allowed,
                    const std::initializer_list<std::string_view> required) {
            if (!value.is_object())
                Fail();
            for (const auto &[key, child] : value.items()) {
                static_cast<void>(child);
                if (std::ranges::find(allowed, key) == allowed.end())
                    Fail();
            }
            for (const auto key : required)
                if (!value.contains(std::string{key}))
                    Fail();
        }

        void Array(const Json &value, const std::size_t maximum) {
            if (!value.is_array())
                Fail();
            if (value.size() > maximum)
                Fail(GameplayVfxErrors::LimitExceeded);
        }

        [[nodiscard]] std::uint64_t Unsigned(const Json &value, const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max()) {
            if (!value.is_number_unsigned())
                Fail();
            const auto number = value.get<std::uint64_t>();
            if (number > maximum)
                Fail(GameplayVfxErrors::LimitExceeded);
            return number;
        }

        [[nodiscard]] std::uint16_t Channel(const Json &value) {
            return static_cast<std::uint16_t>(Unsigned(value, std::numeric_limits<std::uint16_t>::max()));
        }

        [[nodiscard]] float Scalar(const Json &value) {
            if (!value.is_number())
                Fail();
            const auto number = value.get<double>();
            if (!std::isfinite(number) || std::abs(number) > std::numeric_limits<float>::max())
                Fail();
            return static_cast<float>(number);
        }

        [[nodiscard]] std::string Text(const Json &value, const std::size_t maximum = GameplayVfxBindingLimits::TextBytes) {
            if (!value.is_string())
                Fail();
            auto text = value.get<std::string>();
            if (text.size() > maximum)
                Fail(GameplayVfxErrors::LimitExceeded);
            return text;
        }

        [[nodiscard]] Vfx::VfxSpawnParameter Input(const Json &value) {
            Object(value, {"channel", "minimum", "maximum"}, {"channel", "minimum", "maximum"});
            return {Channel(value.at("channel")), Scalar(value.at("minimum")), Scalar(value.at("maximum"))};
        }

        [[nodiscard]] Vfx::VfxParameterValue Constant(const Json &value) {
            Object(value, {"channel", "value"}, {"channel", "value"});
            return {Channel(value.at("channel")), Scalar(value.at("value"))};
        }

        [[nodiscard]] Vfx::VfxParameterBlock Constants(const Json &value) {
            Object(value, {"schema", "values"}, {"schema", "values"});
            const auto &values = value.at("values");
            Array(values, Vfx::VfxSpawnHardLimits::Parameters);
            Vfx::VfxParameterBlock output;
            output.schema = Unsigned(value.at("schema"));
            for (const auto &item : values)
                output.values[output.count++] = Constant(item);
            return output;
        }

        [[nodiscard]] GameplayVfxParameterRoute Route(const Json &value) {
            Object(value, {"source", "target"}, {"source", "target"});
            return {Channel(value.at("source")), Channel(value.at("target"))};
        }

        [[nodiscard]] GameplayVfxLayer Layer(const Json &value) {
            Object(value, {"effectAsset", "scale", "constants", "routes"}, {"effectAsset", "scale", "constants", "routes"});
            auto asset = Assets::AssetId::Parse(Text(value.at("effectAsset"), 36));
            if (asset.HasError())
                Fail();
            GameplayVfxLayer output{.effectAsset = asset.Value(),
                                    .scale = Scalar(value.at("scale")),
                                    .constants = Constants(value.at("constants"))};
            const auto &routes = value.at("routes");
            Array(routes, Vfx::VfxSpawnHardLimits::Parameters);
            output.routes.reserve(routes.size());
            for (const auto &route : routes)
                output.routes.push_back(Route(route));
            return output;
        }

        [[nodiscard]] Vfx::VfxRequirementClass Requirement(const Json &value) {
            const auto text = Text(value);
            if (text == "cosmetic")
                return Vfx::VfxRequirementClass::Cosmetic;
            if (text == "required")
                return Vfx::VfxRequirementClass::GameplayRequired;
            Fail();
        }

        void Selector(const Json &value, GameplayVfxBinding &output) {
            if (value.contains("event") == value.contains("tag"))
                Fail();
            if (value.contains("event"))
                output.event = GameplayEventTypeId{Text(value.at("event"))};
            else
                output.tag = {Unsigned(value.at("tag"))};
        }

        [[nodiscard]] GameplayVfxBinding Binding(const Json &value) {
            Object(value, {"destination", "event", "tag", "schemaVersion", "payloadSchema", "inputs", "requirement", "layers"},
                   {"destination", "schemaVersion", "payloadSchema", "inputs", "requirement", "layers"});
            GameplayVfxBinding output;
            output.destination = Unsigned(value.at("destination"));
            Selector(value, output);
            output.schemaVersion =
                static_cast<std::uint32_t>(Unsigned(value.at("schemaVersion"), std::numeric_limits<std::uint32_t>::max()));
            output.payloadSchema = Unsigned(value.at("payloadSchema"));
            output.requirement = Requirement(value.at("requirement"));
            const auto &inputs = value.at("inputs");
            Array(inputs, Vfx::VfxSpawnHardLimits::Parameters);
            output.inputs.reserve(inputs.size());
            for (const auto &input : inputs)
                output.inputs.push_back(Input(input));
            const auto &layers = value.at("layers");
            Array(layers, Vfx::VfxSpawnHardLimits::Layers);
            output.layers.reserve(layers.size());
            for (const auto &layer : layers)
                output.layers.push_back(Layer(layer));
            return output;
        }

        /** @brief Bounds total parse work and catches escape-equivalent duplicate keys before DOM overwrite. */
        struct ParseBudget final {
            std::size_t nodes{};
            std::vector<std::set<std::string>> fields;

            bool Visit(const int depth, const Json::parse_event_t event, const Json &value) {
                if (++nodes > GameplayVfxBindingLimits::JsonNodes || depth > static_cast<int>(GameplayVfxBindingLimits::JsonDepth))
                    Fail(GameplayVfxErrors::LimitExceeded);
                if ((event == Json::parse_event_t::object_start || event == Json::parse_event_t::array_start) &&
                    depth >= static_cast<int>(GameplayVfxBindingLimits::JsonDepth))
                    Fail(GameplayVfxErrors::LimitExceeded);
                if (event == Json::parse_event_t::object_start)
                    fields.emplace_back();
                if (event == Json::parse_event_t::object_end)
                    fields.pop_back();
                if (event == Json::parse_event_t::key && !fields.back().insert(value.get<std::string>()).second)
                    Fail(GameplayVfxErrors::DuplicateField);
                return true;
            }
        };

        [[nodiscard]] GameplayVfxBindingDocument Document(const Json &value) {
            Object(value, {"version", "generation", "bindings"}, {"version", "generation", "bindings"});
            if (Unsigned(value.at("version")) != 1)
                Fail(GameplayVfxErrors::VersionUnsupported);
            GameplayVfxBindingDocument output{.generation = Unsigned(value.at("generation"))};
            const auto &bindings = value.at("bindings");
            Array(bindings, GameplayVfxBindingLimits::Bindings);
            output.bindings.reserve(bindings.size());
            for (const auto &binding : bindings)
                output.bindings.push_back(Binding(binding));
            return output;
        }
    }  // namespace

    /** @copydoc DecodeGameplayVfxBindings */
    Result<GameplayVfxBindingDocument> DecodeGameplayVfxBindings(const std::string_view bytes) {
        if (bytes.size() > GameplayVfxBindingLimits::SourceBytes)
            return VfxBindingDetail::Reject<GameplayVfxBindingDocument>(GameplayVfxErrors::LimitExceeded);
        try {
            ParseBudget budget;
            auto parsed = Json::parse(bytes, [&budget](int depth, Json::parse_event_t event, const Json &value) {
                return budget.Visit(depth, event, value);
            });
            auto document = Document(parsed);
            if (auto valid = VfxBindingDetail::ValidateDocument(document); valid.HasError())
                return Result<GameplayVfxBindingDocument>::Failure(valid.ErrorValue());
            return Result<GameplayVfxBindingDocument>::Success(std::move(document));
        } catch (const DecodeFailure &failure) {
            return VfxBindingDetail::Reject<GameplayVfxBindingDocument>(*failure.descriptor);
        } catch (const std::bad_alloc &) {
            return VfxBindingDetail::Reject<GameplayVfxBindingDocument>(GameplayVfxErrors::AllocationFailed);
        } catch (const Json::exception &) {
            return VfxBindingDetail::Reject<GameplayVfxBindingDocument>(GameplayVfxErrors::Malformed);
        }
    }
}  // namespace Horo::Gameplay
