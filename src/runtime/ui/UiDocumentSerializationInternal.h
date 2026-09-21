#pragma once

#include "Horo/Runtime/Ui/UiDocumentSerialization.h"

#include <nlohmann/json.hpp>

namespace Horo::Runtime::Ui::SerializationInternal {
    using Json = nlohmann::json;
    using OrderedJson = nlohmann::ordered_json;

    [[nodiscard]] bool IsSupportedVersion(UiDocumentSchemaVersion version) noexcept;

    [[nodiscard]] std::string EncodeUiId(const SerializedUiId &bytes);

    template <typename Integer> [[nodiscard]] Result<Integer> ReadUnsigned(const Json &value);

    template <typename Id> [[nodiscard]] Result<Id> DecodeUiId(const Json &value);

    template <typename Id> [[nodiscard]] OrderedJson EncodeUiId(const Id id) {
        return EncodeUiId(id.Bytes());
    }

    [[nodiscard]] OrderedJson EncodeCanvas(const UiCanvasDescriptor &canvas);
    [[nodiscard]] OrderedJson EncodeElement(const UiDocumentElement &element);
    [[nodiscard]] OrderedJson EncodeDependency(const UiAssetDependency &dependency);
    [[nodiscard]] OrderedJson EncodeRoute(const UiRouteMetadata &route);

    [[nodiscard]] Result<UiCanvasDescriptor> DecodeCanvas(const Json &value, const UiDocumentSerializationLimits &limits);
    [[nodiscard]] Result<UiDocumentElement> DecodeElement(const Json &value, const UiDocumentSerializationLimits &limits);
    [[nodiscard]] Result<UiAssetDependency> DecodeDependency(const Json &value, const UiDocumentSerializationLimits &limits);
    [[nodiscard]] Result<UiRouteMetadata> DecodeRoute(const Json &value, const UiDocumentSerializationLimits &limits);
    [[nodiscard]] Result<Json> ParseJson(std::string_view source, const UiDocumentSerializationLimits &limits);
}  // namespace Horo::Runtime::Ui::SerializationInternal
