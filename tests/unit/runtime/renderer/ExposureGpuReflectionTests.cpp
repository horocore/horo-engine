#include "ExposureGpuReflection.h"
#include "Horo/Runtime/Render/ShaderReflectionErrors.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <utility>

#ifndef HORO_EXPOSURE_GPU_REFLECTION_FIXTURE_PATH
#error "Supply the portable genuine exposure SPIR-V/JSON/MSL fixture directory."
#endif

namespace {
    using namespace Horo;
    using namespace Horo::Render;

    /** @brief Reads one finite immutable captured input; no shader tools or native backend run in this suite. */
    std::vector<std::uint8_t> Read(std::string_view name) {
        std::ifstream input(std::filesystem::path{HORO_EXPOSURE_GPU_REFLECTION_FIXTURE_PATH} / name, std::ios::binary | std::ios::ate);
        REQUIRE(input.good());
        const auto size = input.tellg();
        REQUIRE(size > 0);
        REQUIRE(size <= 1024 * 1024);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        input.seekg(0);
        input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        REQUIRE(input.good());
        return bytes;
    }

    /** @brief Borrows captured text without reconstructing or modifying shader evidence. */
    std::string Text(std::string_view name) {
        const auto bytes = Read(name);
        return {bytes.begin(), bytes.end()};
    }

    /** @brief Selects the admitted pinned Metal compute/storage target. */
    ShaderTargetRequirement Target() {
        return {ShaderTargetBackend::Metal, ShaderPayloadFormat::MetalLibrary24, 1, 1, 32, 128, true, true};
    }

    /** @brief Resolves the three fixture basenames in canonical stage order. */
    std::string_view Entry(ExposureGpuStage stage) {
        constexpr std::array entries{"ClearExposureHistogram", "BuildExposureHistogram", "AdaptExposure"};
        return entries[static_cast<std::size_t>(stage)];
    }

    /** @brief Applies one deliberate contradiction to a copy of actual captured evidence. */
    void Replace(std::string &value, std::string_view from, std::string_view to) {
        const auto at = value.find(from);
        REQUIRE(at != std::string::npos);
        value.replace(at, from.size(), to);
    }

    /** @brief Owns genuine binary and text evidence plus the production-qualified native argument. */
    struct Capture {
        ExposureGpuStage stage;
        ShaderManifest manifest;
        std::vector<ShaderReflectionBindingName> names;
        std::array<SpirvReflectionStage, 1> reflection;
        std::array<std::string, 1> native;
        std::vector<std::uint32_t> words;

        /** @brief Loads original inputs and invokes the actual readonly qualification before any negative mutation. */
        explicit Capture(ExposureGpuStage selected)
            : stage(selected), manifest(MakeExposureGpuShaderManifest(selected, Target())),
              names(ExposureGpuMetalReflectionNames(selected)) {
            const std::string entry{Entry(stage)};
            const auto binary = Read(entry + ".spv");
            REQUIRE(binary.size() % 4 == 0);
            words.resize(binary.size() / 4);
            std::memcpy(words.data(), binary.data(), binary.size());
            reflection[0] = {ShaderStage::Compute, entry, Text(entry + ".json")};
            const auto qualified = QualifyExposureGpuMetalInputs(manifest, Text(entry + ".metal"), Bytes());
            INFO((qualified.HasError() ? qualified.ErrorValue().message : std::string{"captured qualification"}));
            REQUIRE(qualified.HasValue());
            native[0] = qualified.Value();
        }

        /** @brief Borrows the exact current binary, including only deliberate negative-test mutations. */
        std::span<const std::uint8_t> Bytes() const {
            return {reinterpret_cast<const std::uint8_t *>(words.data()), words.size() * sizeof(std::uint32_t)};
        }

        /** @brief Exercises the production final proof and canonical normalization together. */
        Result<NormalizedShaderReflection> Normalize() const {
            return NormalizeExposureGpuMetalReflection(manifest, Target(), reflection, native, names, Bytes());
        }

        /** @brief Finds one actual captured instruction by opcode without rebuilding a substitute module. */
        std::size_t Find(std::uint32_t opcode, std::size_t occurrence = 0) const {
            for (std::size_t at = 5; at < words.size(); at += words[at] >> 16)
                if ((words[at] & 65535) == opcode && occurrence-- == 0)
                    return at;
            FAIL("Required instruction is absent from the genuine capture");
            return 0;
        }

        /** @brief Finds one actual decoration with an exact declared operand value. */
        std::size_t Decoration(std::uint32_t opcode, std::size_t member, std::uint32_t value) const {
            for (std::size_t at = 5; at < words.size(); at += words[at] >> 16) {
                if ((words[at] & 65535) != opcode || (words[at] >> 16) <= member)
                    continue;
                if (words[at + member] == value)
                    return at;
            }
            FAIL("Required decoration is absent from the genuine capture");
            return 0;
        }

        /** @brief Redirects an actual output access chain to the same-layout readonly predecessor resource. */
        void RedirectOutputWrite() {
            const auto previous = Decoration(71, 3, 2);
            const auto output = Decoration(71, 3, 3);
            REQUIRE(words[previous + 2] == 33);
            REQUIRE(words[output + 2] == 33);
            for (std::size_t at = 5; at < words.size(); at += words[at] >> 16) {
                if ((words[at] & 65535) != 65 || words[at + 3] != words[output + 1])
                    continue;
                words[at + 3] = words[previous + 1];
                return;
            }
            FAIL("Genuine output access chain is absent");
        }
    };

    /** @brief Requires a typed rejection without accepting an unrelated backend or tool failure. */
    void Rejected(const Capture &capture) {
        const auto result = capture.Normalize();
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == ShaderReflectionErrors::InvalidReflection.code.Value());
    }
}  // namespace

TEST_CASE("Exposure reflection admits all three actual strict-route captures", "[renderer][exposure][reflection]") {
    for (std::size_t i = 0; i < 3; ++i) {
        Capture capture{static_cast<ExposureGpuStage>(i)};
        INFO(Entry(capture.stage));
        const auto result = capture.Normalize();
        INFO((result.HasError() ? result.ErrorValue().message : std::string{"captured final reflection"}));
        REQUIRE(result.HasValue());
        REQUIRE(result.Value().bindings.size() == capture.manifest.bindings.size());
        REQUIRE(result.Value().parameters.size() == 6);
        for (const auto &binding : result.Value().targetBindings)
            CHECK(binding.nativeBinding == binding.id.value - 1);
        for (std::size_t j = 0; j < 6; ++j)
            CHECK(result.Value().parameters[j].byteOffset == j * 16);
    }
}

TEST_CASE("Exposure evidence diagnostics identify the rejected predicate call site", "[renderer][exposure][reflection]") {
    Capture capture{ExposureGpuStage::ClearHistogram};
    capture.reflection[0].entryPoint = "ForeignExposure";
    const auto result = capture.Normalize();
    REQUIRE(result.HasError());
    CHECK(result.ErrorValue().code.Value() == ShaderReflectionErrors::InvalidReflection.code.Value());
    const auto &message = result.ErrorValue().message;
    CHECK(message.starts_with("Exposure evidence rejected in "));
    CHECK(message.find("NormalizeExposureGpuMetalReflection") != std::string::npos);
    CHECK(message.find("EvidenceFailure") == std::string::npos);
}

TEST_CASE("Exposure readonly qualification preserves every original byte except the proven input qualifier",
          "[renderer][exposure][reflection]") {
    Capture capture{ExposureGpuStage::Adapt};
    auto expected = Text("AdaptExposure.metal");
    Replace(expected, "device type_StructuredBuffer_ExposureGpuState& PreviousExposure",
            "const device type_StructuredBuffer_ExposureGpuState& PreviousExposure");
    CHECK(capture.native[0] == expected);
    Capture clear{ExposureGpuStage::ClearHistogram};
    Capture build{ExposureGpuStage::BuildHistogram};
    CHECK(clear.native[0] == Text("ClearExposureHistogram.metal"));
    CHECK(build.native[0] == Text("BuildExposureHistogram.metal"));
}

TEST_CASE("Exposure image proof rejects depth arrays multisampling and foreign image shapes", "[renderer][exposure][reflection]") {
    const std::array mutations{std::pair{3u, 3u}, std::pair{4u, 1u}, std::pair{5u, 1u},
                               std::pair{6u, 1u}, std::pair{7u, 2u}, std::pair{8u, 1u}};
    for (const auto &[operand, value] : mutations) {
        Capture capture{ExposureGpuStage::BuildHistogram};
        INFO(operand);
        const auto image = capture.Find(25);
        REQUIRE(capture.words[image + 4] == 2);  // Original DXC Depth=2 classification; never a fabricated valid input.
        capture.words[image + operand] = value;
        Rejected(capture);
    }
}

TEST_CASE("Exposure unspecified depth requires independent real JSON and native float2D evidence", "[renderer][exposure][reflection]") {
    Capture json{ExposureGpuStage::BuildHistogram};
    Replace(json.reflection[0].json, "texture2D", "textureCube");
    Rejected(json);
    Capture native{ExposureGpuStage::BuildHistogram};
    Replace(native.native[0], "texture2d<float>", "depth2d<float>");
    Rejected(native);
}

TEST_CASE("Exposure predecessor readonly decoration and native constness remain mandatory", "[renderer][exposure][reflection]") {
    Capture binary{ExposureGpuStage::Adapt};
    const auto readonly = binary.Decoration(72, 3, 24);
    binary.words[readonly + 3] = 0;  // Corrupt the actual NonWritable decoration, retaining the captured member identity.
    Rejected(binary);
    Capture native{ExposureGpuStage::Adapt};
    Replace(native.native[0], "const device type_StructuredBuffer_ExposureGpuState& PreviousExposure",
            "device type_StructuredBuffer_ExposureGpuState& PreviousExposure");
    Rejected(native);
    Capture json{ExposureGpuStage::Adapt};
    Replace(json.reflection[0].json, "\"readonly\" : true", "\"readonly\" : false");
    Rejected(json);
}

TEST_CASE("Exposure binary and Cross state strides must independently equal the native ABI", "[renderer][exposure][reflection]") {
    Capture binary{ExposureGpuStage::Adapt};
    const auto stride = binary.Decoration(71, 3, 32);
    REQUIRE(binary.words[stride + 2] == 6);
    binary.words[stride + 3] = 64;
    Rejected(binary);
    Capture json{ExposureGpuStage::Adapt};
    Replace(json.reflection[0].json, "\"array_stride\" : 32", "\"array_stride\" : 64");
    Rejected(json);
}

TEST_CASE("Exposure dispatch offsets must match across binary Cross and native declarations", "[renderer][exposure][reflection]") {
    Capture binary{ExposureGpuStage::Adapt};
    const auto identity = binary.Decoration(72, 4, 80);
    REQUIRE(binary.words[identity + 3] == 35);
    binary.words[identity + 4] = 84;
    Rejected(binary);
    Capture json{ExposureGpuStage::Adapt};
    Replace(json.reflection[0].json, "\"offset\" : 80", "\"offset\" : 84");
    Rejected(json);
    Capture native{ExposureGpuStage::Adapt};
    Replace(native.native[0], "uint4 Identity;", "uint2 Identity;");
    Rejected(native);
}

TEST_CASE("Exposure proof rejects foreign manifests aliases and duplicate Cross metadata", "[renderer][exposure][reflection]") {
    Capture revision{ExposureGpuStage::ClearHistogram};
    ++revision.manifest.sourceRevision;
    Rejected(revision);
    Capture alias{ExposureGpuStage::ClearHistogram};
    alias.names[0].name = "OtherHistogram";
    Rejected(alias);
    Capture duplicate{ExposureGpuStage::ClearHistogram};
    duplicate.reflection[0].json.insert(1, "\"types\":{},");
    Rejected(duplicate);
}

TEST_CASE("Exposure pointer proof rejects genuine output stores redirected to the readonly predecessor",
          "[renderer][exposure][reflection]") {
    Capture capture{ExposureGpuStage::Adapt};
    capture.RedirectOutputWrite();
    Rejected(capture);
}
