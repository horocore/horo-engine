#include "UiElementAuthoringCodec.h"

#include "JsonUtils.h"
#include "UiDocumentSerializationInternal.h"

#include <nlohmann/json.hpp>
#include <stdexcept>

namespace Horo::Runtime::Ui::PresentationCodec {
    namespace {
        using Json = nlohmann::json;
        using Out = nlohmann::ordered_json;

        /** @brief Reject unknown or missing authored fields before extraction. */
        void Fields(const Json &value, const std::initializer_list<std::string_view> keys) {
            if (!Foundation::HasAllowedFields(value, keys) || value.size() != keys.size())
                throw std::invalid_argument("authored UI fields");
        }

        /** @brief Read a closed integer without truncating unknown high bits or accepting floats. */
        std::uint32_t Index(const Json &value, const std::uint32_t maximum) {
            if (!value.is_number_integer())
                throw std::invalid_argument("authored UI enum");
            if (value.is_number_unsigned()) {
                const auto number = value.get<std::uint64_t>();
                if (number > maximum)
                    throw std::invalid_argument("authored UI enum");
                return static_cast<std::uint32_t>(number);
            }
            const auto number = value.get<std::int64_t>();
            if (number < 0 || static_cast<std::uint64_t>(number) > maximum)
                throw std::invalid_argument("authored UI enum");
            return static_cast<std::uint32_t>(number);
        }

        /** @brief Read an actual boolean, never truthy input. */
        bool Boolean(const Json &value) {
            if (!value.is_boolean())
                throw std::invalid_argument("authored UI boolean");
            return value.get<bool>();
        }

        /** @brief Encode the closed initial-value vocabulary without transient runtime state. */
        Out State(const UiElementAuthoredState &state) {
            Out initial = nullptr;
            if (const auto *value = std::get_if<bool>(&state.initial))
                initial = *value;
            if (const auto *value = std::get_if<UiControlScalarValues>(&state.initial))
                initial = Out::array({value->minimum, value->maximum, value->step, value->value});
            if (const auto *value = std::get_if<std::string>(&state.initial))
                initial = *value;
            return Out{{"visible", state.visible},
                       {"enabled", state.enabled},
                       {"focusable", state.focusable},
                       {"kind", state.initial.index()},
                       {"initial", std::move(initial)}};
        }

        /** @brief Decode an initial value before canonical state and element-kind validation. */
        UiElementAuthoredState State(const Json &value) {
            Fields(value, {"visible", "enabled", "focusable", "kind", "initial"});
            UiElementAuthoredState state{Boolean(value.at("visible")), Boolean(value.at("enabled")), Boolean(value.at("focusable")), {}};
            const auto kind = Index(value.at("kind"), 3);
            const auto &initial = value.at("initial");
            if (kind == 0 && !initial.is_null())
                throw std::invalid_argument("absent initial value");
            if (kind == 1)
                state.initial = Boolean(initial);
            if (kind == 2) {
                if (!initial.is_array() || initial.size() != 4)
                    throw std::invalid_argument("scalar initial value");
                for (const auto &number : initial)
                    if (!number.is_number())
                        throw std::invalid_argument("scalar number");
                state.initial = UiControlScalarValues{initial[0].get<double>(), initial[1].get<double>(), initial[2].get<double>(),
                                                      initial[3].get<double>()};
            }
            if (kind == 3) {
                if (!initial.is_string() || initial.get_ref<const std::string &>().size() > MaximumUiActionTextBytes)
                    throw std::invalid_argument("initial text");
                state.initial = initial.get<std::string>();
            }
            if (!state.IsValid())
                throw std::invalid_argument("initial state validation");
            return state;
        }

        /** @brief Encode immutable message references through their sole existing source codec. */
        Out Message(const std::optional<UiLocalizedText> &message) {
            return message ? SerializationInternal::EncodeLocalizedText(*message) : Out(nullptr);
        }

        /** @brief Decode optional messages while preserving the Localization typed error. */
        Result<std::optional<UiLocalizedText>> Message(const Json &value) {
            using R = Result<std::optional<UiLocalizedText>>;
            if (value.is_null())
                return R::Success(std::nullopt);
            auto message = SerializationInternal::DecodeLocalizedText(value, {});
            if (message.HasError())
                return R::Failure(message.ErrorValue());
            return R::Success(std::move(message).Value());
        }

        /** @brief Persist only authored accessibility semantics; live bounds/actions/value remain excluded. */
        Out Accessibility(const UiElementAccessibility &value) {
            return Out{{"role", static_cast<std::uint8_t>(value.role)},
                       {"name", Message(value.name)},
                       {"description", Message(value.description)},
                       {"hidden", value.hidden},
                       {"readOnly", value.readOnly},
                       {"required", value.required}};
        }
    }  // namespace

    /** @copydoc WriteAuthoring */
    void WriteAuthoring(nlohmann::ordered_json &output, const UiElementPresentation &presentation) {
        output["state"] = State(presentation.state);
        output["localizedText"] = Message(presentation.localizedText);
        output["accessibility"] = presentation.accessibility ? Accessibility(*presentation.accessibility) : Out(nullptr);
    }

    /** @copydoc ReadAuthoring */
    Result<void> ReadAuthoring(const nlohmann::json &input, UiElementPresentation &presentation) {
        presentation.state = State(input.at("state"));
        auto localized = Message(input.at("localizedText"));
        if (localized.HasError())
            return Result<void>::Failure(localized.ErrorValue());
        presentation.localizedText = std::move(localized).Value();
        const auto &access = input.at("accessibility");
        if (access.is_null())
            return Result<void>::Success();
        Fields(access, {"role", "name", "description", "hidden", "readOnly", "required"});
        auto name = Message(access.at("name")), description = Message(access.at("description"));
        if (name.HasError())
            return Result<void>::Failure(name.ErrorValue());
        if (description.HasError())
            return Result<void>::Failure(description.ErrorValue());
        presentation.accessibility =
            UiElementAccessibility{static_cast<UiAccessibilityRole>(
                                       Index(access.at("role"), static_cast<std::uint32_t>(UiAccessibilityRole::ScrollView))),
                                   std::move(name).Value(),
                                   std::move(description).Value(),
                                   Boolean(access.at("hidden")),
                                   Boolean(access.at("readOnly")),
                                   Boolean(access.at("required"))};
        return Result<void>::Success();
    }
}  // namespace Horo::Runtime::Ui::PresentationCodec
