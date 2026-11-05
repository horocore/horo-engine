#pragma once

#include "Horo/Runtime/Render/ColorPipeline.h"
#include "PostProcessTestSupport.h"

#include <bit>

namespace Horo::Render::Test {
    /** @brief Produces portable little-endian RGB32F fixture bytes, independently of native host endianness. */
    inline Sha256Digest ColorPayloadDigest(std::span<const float> values) {
        Sha256Builder digest;
        for (float value : values) {
            const auto bits = std::bit_cast<std::uint32_t>(value);
            const std::array bytes{std::byte(bits & 255U), std::byte((bits >> 8U) & 255U), std::byte((bits >> 16U) & 255U),
                                   std::byte(bits >> 24U)};
            REQUIRE(digest.Update(bytes));
        }
        return digest.Finalize();
    }

    /** @brief Cold admission fixture, deliberately not an ACES conformance reference or native GPU receipt. */
    struct ColorPipelineFixture {
        PostProcessFixture source;
        std::array<float, 24> rgb{0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0, 0, 0, 1, 1, 0, 1, 0, 1, 1, 1, 1, 1};

        [[nodiscard]] ColorTransformDescriptor Descriptor() const {
            ColorTransformDescriptor descriptor;
            descriptor.configurationRelease = 20001;
            descriptor.configurationDigest.bytes[0] = 1;
            descriptor.referenceDigest.bytes[0] = 2;
            descriptor.payloadDigest = ColorPayloadDigest(rgb);
            descriptor.dimension = 2;
            descriptor.output.matching = RenderOutputColorMatching::ColorSpace;
            descriptor.referenceWhiteNits = 100;
            descriptor.peakNits = 100;
            return descriptor;
        }

        [[nodiscard]] ExposureState State() const {
            return {.view = source.compatibility.view,
                    .generation = source.compatibility.exposureGeneration,
                    .settingsGeneration = 7,
                    .deviceGeneration = source.compatibility.deviceGeneration,
                    .consumeSubmission = 1,
                    .diagnosticEv = 0};
        }

        [[nodiscard]] ColorPipelineRequest Request() const {
            auto transform = CookedColorTransform::Create(Descriptor(), rgb);
            REQUIRE(transform.HasValue());
            ColorPipelineRequest request;
            request.generation = source.compatibility.colorGeneration;
            request.settingsGeneration = 7;
            request.submission = 1;
            request.sceneColor = source.inputs.front();
            request.compatibility = source.compatibility;
            request.exposure = State();
            request.output.active = Descriptor().output;
            request.output.requested.color = request.output.active;
            request.output.revision = 1;
            request.output.display.revision = 1;
            request.output.display.attached = true;
            request.outputTransform = std::move(transform).Value();
            return request;
        }
    };
}  // namespace Horo::Render::Test
