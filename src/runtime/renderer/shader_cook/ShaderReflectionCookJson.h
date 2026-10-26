#pragma once
#include "JsonUtils.h"
#include "ShaderReflectionCook.h"

namespace Horo::Render::ShaderReflectionCookDetail {
    using Foundation::Json;
    constexpr std::size_t MaximumMetadataBytes = 1024U * 1024U;
}  // namespace Horo::Render::ShaderReflectionCookDetail
