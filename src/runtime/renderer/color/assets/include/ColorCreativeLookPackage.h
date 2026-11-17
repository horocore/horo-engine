#pragma once
/** @file ColorCreativeLookPackage.h @brief Read-only three-file creative look decoding; no cube import, cook, math or trust qualification.
 */
#include "Horo/Runtime/Render/ColorPipeline.h"

#include <array>
#include <filesystem>

namespace Horo::Render::Internal {
    /** @brief Pins the actual schema2 transform and original manifest/payload/reference bytes as one cold generation. */
    struct ColorCreativeLookBundle final {
        std::shared_ptr<const CookedColorTransform> look;
        std::array<std::vector<std::uint8_t>, 3> files;
    };

    /** @brief Loads exactly manifest.json/look.rgb.f32le/reference.json beneath an explicit ordinary root.
     * @param root Absolute known package directory. @return Exact finite transform plus original bytes, or typed malformed/IO/factory
     * failure.
     * @details Static path checks and hashes establish integrity only; host retains directory ownership during cold loading.
     * Reference JSON is finite, duplicate-free original data, without mathematical, offline provenance or trust qualification.
     * Scene/frame admission and same-output-configuration checks remain PrepareColorTransformSelection's authority.
     */
    [[nodiscard]] Result<ColorCreativeLookBundle> LoadColorCreativeLookPackage(const std::filesystem::path &root);
}  // namespace Horo::Render::Internal
