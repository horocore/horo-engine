#pragma once
/** @file UiStyleValueCodec.h
 * @brief Shared target-private style literal/reference codec for presentation and style assets.
 */
#include "Horo/Runtime/Ui/UiStyle.h"

#include <nlohmann/json_fwd.hpp>

namespace Horo::Runtime::Ui::StyleCodec {
    /** @brief Encode a canonical literal. @param value Typed literal. @return Owned JSON. */
    [[nodiscard]] nlohmann::ordered_json EncodeLiteral(const UiStyleValue &value);
    /** @brief Decode a bounded literal at the enclosing exception boundary. @param value JSON. @return Typed literal. */
    [[nodiscard]] UiStyleValue DecodeLiteral(const nlohmann::json &value);
    /** @brief Encode a token/literal source. @param value Typed source. @return Owned JSON. */
    [[nodiscard]] nlohmann::ordered_json EncodeSource(const UiStyleValueSource &value);
    /** @brief Decode one strict token/literal source. @param value JSON. @return Typed source; throws on malformed fields. */
    [[nodiscard]] UiStyleValueSource DecodeSource(const nlohmann::json &value);
}  // namespace Horo::Runtime::Ui::StyleCodec
