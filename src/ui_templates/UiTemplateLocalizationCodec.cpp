#include "UiTemplateCodec.h"

namespace Horo::UiTemplates::Detail::Codec {
    namespace {
        using namespace Runtime::Ui;

        /** @brief Encodes the owning Localization argument vocabulary with explicit closed alternative tags. */
        Json WriteArgumentValue(const UiLocalizedArgumentValue &value) {
            if (value.valueless_by_exception())
                Reject();
            const auto payload = std::visit([]<typename T>(const T &item) -> Json {
                if constexpr (std::is_same_v<T, UiLocalizedDateTime>)
                    return item.unixMilliseconds;
                else if constexpr (std::is_same_v<T, UiLocalizedDuration>)
                    return item.milliseconds;
                else if constexpr (std::is_same_v<T, UiLocalizedStableEnum>)
                    return Json::array({item.typeId, item.value});
                else if constexpr (std::is_same_v<T, UiLocalizedShortcut>)
                    return Json::array({item.modifiers, item.key});
                else
                    return item;
            }, value);
            return {{"kind", value.index()}, {"value", payload}};
        }

        /** @brief Restores each argument category without converting integers, display strings or enum evidence. */
        UiLocalizedArgumentValue ReadArgumentValue(const Json &value) {
            Fields(value, {"kind", "value"});
            const auto &p = value.at("value");
            switch (Integer<std::uint8_t>(value.at("kind"))) {
                case 0:
                    return Integer<std::int64_t>(p);
                case 1:
                    return Scalar<double>(p);
                case 2:
                    return Boolean(p);
                case 3:
                    return UiLocalizedDateTime{Integer<std::int64_t>(p)};
                case 4:
                    return UiLocalizedDuration{Integer<std::int64_t>(p)};
                case 5: {
                    const auto &v = Tuple(p, 2);
                    return UiLocalizedStableEnum{Integer<std::uint64_t>(v[0]), Integer<std::uint64_t>(v[1])};
                }
                case 6:
                    return Text(p);
                case 7: {
                    const auto &v = Tuple(p, 2);
                    return UiLocalizedShortcut{Integer<std::uint32_t>(v[0]), Integer<std::uint32_t>(v[1])};
                }
                default:
                    Reject();
            }
        }
    }  // namespace

    /** @copydoc WriteLocalized */
    Json WriteLocalized(const Runtime::Ui::LocalizedMessageRef &value) {
        if (!value.IsValid())
            Reject();
        Json arguments = Json::array();
        for (const auto &argument : value.Arguments())
            arguments.push_back({{"name", argument.name}, {"value", WriteArgumentValue(argument.value)}});
        return {{"namespace", value.Key().namespaceId},
                {"key", value.Key().localKey},
                {"arguments", std::move(arguments)},
                {"fallback", value.FallbackText()},
                {"failure", static_cast<std::uint8_t>(value.FailurePolicy())}};
    }

    /** @copydoc ReadLocalized */
    Runtime::Ui::LocalizedMessageRef ReadLocalized(const Json &value) {
        using namespace Runtime::Ui;
        Fields(value, {"namespace", "key", "arguments", "fallback", "failure"});
        const auto &array = Array(value.at("arguments"), MaximumUiLocalizedArguments);
        std::vector<UiLocalizedArgument> arguments;
        arguments.reserve(array.size());
        for (const auto &argument : array) {
            Fields(argument, {"name", "value"});
            arguments.push_back({Text(argument.at("name")), ReadArgumentValue(argument.at("value"))});
        }
        return Take(UiLocalizedText::Create(Take(UiMessageKey::Create(Text(value.at("namespace")), Text(value.at("key")))),
                                            std::move(arguments), Text(value.at("fallback")),
                                            Enum<UiLocalizedTextFailurePolicy>(value.at("failure"), 3)));
    }
}  // namespace Horo::UiTemplates::Detail::Codec
