#pragma once
#include "Horo/Runtime/Render/LightCullingKernel.h"
#include "LightCullingMetalReflection.h"
#include "RegisterRasterReflectionInternal.h"

namespace Horo::Render::LightCullingMetalCookDetail {
    using Foundation::Json;
    using Tokens = RegisterRasterCookDetail::TokenView;
    using RegisterRasterCookDetail::InvalidEvidence;

    struct Field {
        std::string name;
        std::string jsonType;
        std::uint32_t offset{}, count{1}, stride{};
    };

    struct Layout {
        std::uint32_t bytes{}, alignment{1};
        std::vector<Field> fields;
        std::string scalarType;
    };

    struct Buffer {
        std::string name, wrapperMember;
        std::uint32_t slot{};
        bool constant{}, readonly{};
        Layout layout;
    };

    [[nodiscard]] std::array<Buffer, 5> ParseNative(std::string_view source, bool provenReadonlyInputs = false);
    void ProveLightCullingSpirv(std::span<const std::uint8_t> bytes);
    [[nodiscard]] std::uint32_t Unsigned(const Json &value);
    [[nodiscard]] const Json &Rows(const Json &value, const char *key, std::size_t maximum);
    void ValidateStorage(const Json &json, const Json &entry, const Buffer &native, std::size_t index);
    void ValidateDispatch(const Json &json, const Json &entry, const Buffer &native, ShaderReflectionCandidate &candidate,
                          std::span<const ShaderReflectionBindingName> names);
}  // namespace Horo::Render::LightCullingMetalCookDetail
