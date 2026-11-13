#pragma once

#include "Horo/Runtime/Render/ColorOutputProgram.h"
#include "Horo/Runtime/Render/ColorPipeline.h"
#include "PostProcessTestSupport.h"

#include <bit>

namespace Horo::Render::Test {
    /** @brief Parses a checked-in closed-profile digest without treating it as authentication evidence. */
    inline Sha256Digest PinnedColorDigest(std::string_view text) {
        const auto digest = ParseSha256(text);
        REQUIRE(digest.HasValue());
        return digest.Value();
    }

    /** @brief Writes one float as portable little-endian bytes without aliasing native storage. */
    inline void WriteColorFloat(std::vector<std::byte> &bytes, std::size_t offset, float value) {
        const auto bits = std::bit_cast<std::uint32_t>(value);
        for (std::size_t i = 0; i < 4; ++i)
            bytes[offset + i] = std::byte((bits >> (8U * i)) & 255U);
    }

    /** @brief Integrity-only synthetic program; this is not an official processor, cooked shader or native receipt. */
    struct ColorOutputProgramFixture {
        std::string source{"float4 HoroColorOutput(float4 value) { return value; }\n"};
        ColorOutputProgramDescriptor descriptor;
        std::array<ColorOutputTableDescriptor, 2> tables;
        std::array<std::vector<std::byte>, 2> original, upload;

        /** @brief Builds the exact closed table shape with deterministic finite synthetic samples. */
        ColorOutputProgramFixture() {
            descriptor.configurationRelease = 40000;
            descriptor.ocioVersion = 20500;
            descriptor.configurationDigest = PinnedColorDigest("sha256:9e3ec773a7fbc0bb6666e428dedd8ccec4f59b8b6d40506c825cc7a8c02ff2ff");
            descriptor.optionsDigest = PinnedColorDigest("sha256:5bbeb4a92e3119c005779de978ed5826a0b23774184a273f9b6fe259ad4d2772");
            descriptor.referenceDigest.bytes[0] = 3;
            InitializeTable(0, "horo_color_reach_m_table_0", 1, RenderTextureFormat::R32Float);
            InitializeTable(1, "horo_color_gamut_cusp_table_0", 3, RenderTextureFormat::Rgba32Float);
            Rehash();
        }

        /** @brief Prepares one bounded original/upload pair with lossless RGB and explicit alpha-one padding. */
        void InitializeTable(std::size_t index, std::string name, std::uint32_t channels, RenderTextureFormat format) {
            tables[index].ordinal = static_cast<std::uint32_t>(index);
            tables[index].textureName = std::move(name);
            tables[index].samplerName = tables[index].textureName + "Sampler";
            tables[index].extent = {362, 1};
            tables[index].sourceChannels = channels;
            tables[index].format = format;
            original[index].resize(362 * channels * 4);
            const std::size_t uploadChannels = index == 0 ? 1 : 4;
            upload[index].resize(362 * uploadChannels * 4);
            FillTable(index, channels, uploadChannels);
        }

        /** @brief Populates portable samples using the same source values in every admitted upload channel. */
        void FillTable(std::size_t index, std::size_t channels, std::size_t uploadChannels) {
            for (std::size_t pixel = 0; pixel < 362; ++pixel) {
                for (std::size_t channel = 0; channel < channels; ++channel) {
                    const float value = static_cast<float>(channel) - 0.25F + static_cast<float>(pixel) / 64;
                    WriteColorFloat(original[index], (pixel * channels + channel) * 4, value);
                    WriteColorFloat(upload[index], (pixel * uploadChannels + channel) * 4, value);
                }
                if (uploadChannels == 4)
                    WriteColorFloat(upload[index], (pixel * uploadChannels + 3) * 4, 1);
            }
        }

        /** @brief Recomputes integrity metadata after an intentional test mutation, without asserting provenance. */
        void Rehash() {
            descriptor.sourceDigest = ComputeSha256(std::as_bytes(std::span{source}));
            for (std::size_t i = 0; i < tables.size(); ++i) {
                tables[i].sourceDigest = ComputeSha256(original[i]);
                tables[i].uploadDigest = ComputeSha256(upload[i]);
            }
        }

        /** @brief Borrows current exact input pairs only for the synchronous factory call. */
        std::array<ColorOutputTableInput, 2> Inputs() const {
            return {{{tables[0], original[0], upload[0]}, {tables[1], original[1], upload[1]}}};
        }

        /** @brief Creates an owned immutable integrity candidate using this fixture's current metadata. */
        Result<std::shared_ptr<const CookedColorOutputProgram>> Create() const {
            const auto inputs = Inputs();
            return CookedColorOutputProgram::Create(descriptor, source, inputs);
        }
    };

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
            descriptor.configurationRelease = 40000;
            descriptor.role = ColorTransformRole::CreativeLook;
            descriptor.configurationDigest = PinnedColorDigest("sha256:9e3ec773a7fbc0bb6666e428dedd8ccec4f59b8b6d40506c825cc7a8c02ff2ff");
            descriptor.referenceDigest.bytes[0] = 2;
            descriptor.payloadDigest = ColorPayloadDigest(rgb);
            descriptor.dimension = 2;
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
            ColorOutputProgramFixture program;
            auto transform = program.Create();
            REQUIRE(transform.HasValue());
            ColorPipelineRequest request;
            request.generation = source.compatibility.colorGeneration;
            request.settingsGeneration = 7;
            request.submission = 1;
            request.sceneColor = source.inputs.front();
            request.compatibility = source.compatibility;
            request.exposure = State();
            request.output.active.matching = RenderOutputColorMatching::ColorSpace;
            request.output.requested.color = request.output.active;
            request.output.revision = 1;
            request.output.display.revision = 1;
            request.output.display.attached = true;
            request.outputTransform = std::move(transform).Value();
            return request;
        }
    };
}  // namespace Horo::Render::Test
