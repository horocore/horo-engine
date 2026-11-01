#include "EditorViewportScene.h"

#include <array>
#include <bit>

namespace Horo::Editor {
    namespace {
        void MixLightWord(std::uint64_t &signature, const std::uint64_t word) noexcept {
            for (unsigned int shift = 0; shift != 64; shift += 8) {
                signature ^= (word >> shift) & 0xffU;
                signature *= 1099511628211ULL;
            }
        }

        void MixLightValue(std::uint64_t &signature, const Render::RenderLight &light) noexcept {
            MixLightWord(signature, light.stableIdentity);
            MixLightWord(signature, static_cast<std::uint64_t>(light.kind));
            const std::array values{light.position.x,  light.position.y,      light.position.z,     light.direction.x, light.direction.y,
                                    light.direction.z, light.color.x,         light.color.y,        light.color.z,     light.intensity,
                                    light.range,       light.innerConeCosine, light.outerConeCosine};
            for (const float value : values)
                MixLightWord(signature, std::bit_cast<std::uint32_t>(value));
        }
    }  // namespace

    /** @copydoc ComputeEditorViewportLightRevision */
    std::uint64_t ComputeEditorViewportLightRevision(const std::span<const Render::RenderLight> lights) noexcept {
        if (lights.size() > Render::MaximumSceneLights)
            return 0;
        std::uint64_t signature = 14695981039346656037ULL;
        MixLightWord(signature, lights.size());
        for (const auto &light : lights)
            MixLightValue(signature, light);
        return signature == 0 ? 1 : signature;
    }
}  // namespace Horo::Editor
