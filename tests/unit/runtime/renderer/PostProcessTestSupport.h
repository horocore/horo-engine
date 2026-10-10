#pragma once

#include "Horo/Runtime/Render/PostProcessErrors.h"
#include "Horo/Runtime/Render/PostProcessGraph.h"
#include "Horo/Runtime/Render/PostProcessVolumes.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::Render::Test {
    /** @brief Complete synchronous scene/recipe fixture using Horo identities only. */
    struct PostProcessFixture {
        TemporalHistoryCompatibility compatibility{.view = {1},
                                                   .provider = {2},
                                                   .mode = {3},
                                                   .renderExtent = {16, 16},
                                                   .targetExtent = {16, 16},
                                                   .providerGeneration = 1,
                                                   .modeGeneration = 1,
                                                   .surfaceGeneration = 1,
                                                   .rasterGeneration = 1,
                                                   .colorGeneration = 4,
                                                   .exposureGeneration = 5,
                                                   .inputSchemaGeneration = 1,
                                                   .deviceGeneration = 9,
                                                   .projectionGeneration = 1,
                                                   .jitterGeneration = 1,
                                                   .sceneOriginGeneration = 1,
                                                   .motionGeneration = 1,
                                                   .recipeGeneration = 1};
        std::vector<PostProcessTextureInput> inputs;
        std::vector<PostProcessRecipe> recipes;
        PostProcessSettings settings;
        RenderCapabilitySnapshot capabilities;

        PostProcessFixture() {
            capabilities.deviceIncarnation = 9;
            capabilities.capabilityRevision = 1;
            capabilities.features.bits = RenderCapabilitySet::KnownBits;
            capabilities.queues = {true, true, true, false};
            capabilities.limits.maxTextureDimension2D = 16'384;
            capabilities.limits.maxFramesInFlight = 2;
            capabilities.formats.usages.fill(RenderTextureUsage::Sampled | RenderTextureUsage::Storage |
                                             RenderTextureUsage::RenderAttachment);
            capabilities.formats.sampleCountMask = std::uint64_t{1} << 1U;
            using enum RenderTextureFormat;
            const std::array formats{Rgba16Float, R32Float, Rgba16Float, R8Unorm, Rg16Float, Rgba16Float};
            for (std::size_t i = 0; i < formats.size(); ++i) {
                const bool color = i == 0 || i == 5;
                inputs.push_back({static_cast<PostProcessSemantic>(i),
                                  {{1}, static_cast<std::uint32_t>(i + 1), 1},
                                  {.extent = {16, 16}, .format = formats[i], .usage = RenderTextureUsage::Sampled},
                                  color ? PostProcessRepresentation::UnexposedAcesCg : PostProcessRepresentation::Data,
                                  compatibility});
            }
            for (std::size_t i = 0; i < 8; ++i)
                recipes.push_back({static_cast<PostProcessEffect>(i), i + 1, {}, RenderPassKind::Graphics, 1'000'000, 4096});
        }

        void EnableAll() {
            settings.bloom.emplace();
            settings.depthOfField.emplace();
            settings.motionBlur.emplace();
            settings.ambientOcclusion.emplace();
            settings.reflections.emplace();
            settings.vignette.emplace();
            settings.chromaticAberration.emplace();
            settings.filmGrain.emplace();
        }

        [[nodiscard]] PostProcessGraphRequest Request() const {
            return {.settingsGeneration = 7,
                    .settings = settings,
                    .compatibility = compatibility,
                    .capabilities = capabilities,
                    .inputs = inputs,
                    .exposure = PostProcessExposureInput{{{1}, 8, 1}, compatibility.view, compatibility.exposureGeneration},
                    .recipes = recipes,
                    .maximumWorkItems = 1'000'000,
                    .maximumReservedBytes = 1'000'000};
        }
    };
}  // namespace Horo::Render::Test
