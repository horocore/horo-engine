#include "Horo/Foundation/Sha256.h"
#include "Horo/Runtime/Render/ShaderReflectionErrors.h"
#include "VfxGpuReflection.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#ifndef HORO_VFX_GPU_REFLECTION_FIXTURE_PATH
#error "Supply the dedicated actual captured GPU VFX fixture directory."
#endif

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    constexpr auto Role = VfxGpuProgramRole::Simulation;
    const ShaderEntryPoint Entry{ShaderStage::Compute, "SimulateParticles"};

    /** @brief Loads only bounded immutable real capture bytes; test mutations never write fixture files. */
    std::vector<std::uint8_t> Read(std::string_view name) {
        const auto path = std::filesystem::path{HORO_VFX_GPU_REFLECTION_FIXTURE_PATH} / name;
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        REQUIRE(input.good());
        const auto bytes = input.tellg();
        REQUIRE(bytes > 0);
        REQUIRE(bytes <= 1024 * 1024);
        std::vector<std::uint8_t> result(static_cast<std::size_t>(bytes));
        input.seekg(0);
        input.read(reinterpret_cast<char *>(result.data()), static_cast<std::streamsize>(result.size()));
        REQUIRE(input.good());
        return result;
    }

    std::string Text(std::string_view name) {
        const auto bytes = Read(name);
        return {bytes.begin(), bytes.end()};
    }

    /** @brief Reproduces the recorded actual producer target value; manifest/name semantics come from the production API. */
    ShaderTargetRequirement Target() {
        return {.backend = ShaderTargetBackend::Metal,
                .payloadFormat = ShaderPayloadFormat::MetalLibrary24,
                .descriptorVersion = 1,
                .interfaceSchemaVersion = 1,
                .maximumBindings = 32,
                .maximumInlineConstantBytes = 128,
                .supportsCompute = true,
                .supportsStorageResources = true};
    }

    struct Capture {
        std::vector<std::uint8_t> spirv{Read("compute.spv")};
        std::string native{Text("compute.metal")}, json{Text("compute.json")};
        ShaderManifest manifest{MakeVfxGpuShaderManifest(Role, Target())};
    };

    std::vector<std::uint32_t> Words(std::span<const std::uint8_t> bytes) {
        REQUIRE(bytes.size() % 4 == 0);
        std::vector<std::uint32_t> words(bytes.size() / 4);
        std::memcpy(words.data(), bytes.data(), bytes.size());
        return words;
    }

    std::vector<std::uint8_t> Bytes(std::span<const std::uint32_t> words) {
        std::vector<std::uint8_t> bytes(words.size_bytes());
        std::memcpy(bytes.data(), words.data(), bytes.size());
        return bytes;
    }

    /** @brief Bounds the genuine instruction stream before locating a mutation target. */
    std::size_t Width(std::span<const std::uint32_t> words, std::size_t at) {
        REQUIRE(at < words.size());
        const auto count = words[at] >> 16;
        REQUIRE(count > 0);
        REQUIRE(count <= words.size() - at);
        return count;
    }

    template <typename Match> std::size_t Find(std::span<const std::uint32_t> words, Match match, std::size_t first = 5) {
        for (auto at = first; at < words.size(); at += Width(words, at)) {
            const auto count = Width(words, at);
            if (match(words.subspan(at, count)))
                return at;
        }
        FAIL("Actual captured instruction needed by regression is absent.");
        return 0;
    }

    std::size_t Minimum(std::span<const std::uint32_t> words) {
        return Find(words, [](auto instruction) {
            return (instruction[0] & 65535) == 12 && instruction.size() == 7 && instruction[4] == 79;
        });
    }

    std::uint32_t UintType(std::span<const std::uint32_t> words) {
        const auto at = Find(words, [](auto i) {
            return (i[0] & 65535) == 21 && i.size() == 4 && i[2] == 32 && i[3] == 0;
        });
        return words[at + 1];
    }

    std::uint32_t SourceVariable(std::span<const std::uint32_t> words) {
        const auto at = Find(words, [](auto i) {
            return (i[0] & 65535) == 71 && i.size() == 4 && i[2] == 33 && i[3] == 0;
        });
        return words[at + 1];
    }

    void Rejected(const Capture &capture, std::span<const std::uint32_t> words) {
        const auto bytes = Bytes(words);
        const auto result = QualifyVfxGpuMetalInputs(capture.manifest, Entry, capture.native, bytes);
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == ShaderReflectionErrors::InvalidReflection.code.Value());
    }

    std::string Qualified(const Capture &capture) {
        auto result = QualifyVfxGpuMetalInputs(capture.manifest, Entry, capture.native, capture.spirv);
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    std::string ReplaceFirst(std::string text, std::string_view from, std::string_view to) {
        const auto at = text.find(from);
        REQUIRE(at != std::string::npos);
        text.replace(at, from.size(), to);
        return text;
    }

    Result<NormalizedShaderReflection> Normalize(const Capture &capture, std::string json, std::string native) {
        const std::array<SpirvReflectionStage, 1> stages{{{ShaderStage::Compute, "SimulateParticles", std::move(json)}}};
        const std::array translations{std::move(native)};
        return NormalizeVfxGpuMetalReflection(capture.manifest, Target(), stages, translations, VfxGpuShaderReflectionNames(Role),
                                              capture.spirv);
    }
}  // namespace

TEST_CASE("Captured VFX extension names require a terminator inside their own instruction", "[unit][renderer][shader][vfx][capture]") {
    const Capture capture;
    REQUIRE_FALSE(Qualified(capture).empty());
    auto words = Words(capture.spirv);
    const auto imported = Find(words, [](auto instruction) {
        return (instruction[0] & 65535) == 11;
    });
    const auto count = Width(words, imported);
    REQUIRE(count > 2);
    for (auto at = imported + 2; at < imported + count; ++at)
        words[at] = 0x41414141U;
    Rejected(capture, words);
}

TEST_CASE("Actual GPU VFX capture retains validated producer bytes", "[unit][renderer][shader][vfx][capture]") {
    const Capture capture;
    CHECK(FormatSha256(ComputeSha256(std::as_bytes(std::span{capture.spirv}))) ==
          "sha256:4dce7b44c885daa016ea8f9a6d0f6d47a65e52640e85419eba4ebccb43e9fc09");
    CHECK(FormatSha256(ComputeSha256(std::as_bytes(std::span{capture.native}))) ==
          "sha256:4e3906e38cfca7968cab72585aa0938ff83ac3e8945cb8d1e3943256d3ce9d0c");
    CHECK(FormatSha256(ComputeSha256(std::as_bytes(std::span{capture.json}))) ==
          "sha256:1894785cb9e885011db938750a726cf042a03053d0cc2492fa804f5db3d5e8e2");
}

TEST_CASE("Actual strict NMin compute capture completes binary native and JSON evidence", "[unit][renderer][shader][vfx][capture]") {
    const Capture capture;
    const auto qualified = Qualified(capture);  // Complete binary proof and native parsing, not only an opcode unit test.
    auto expected = ReplaceFirst(capture.native, "device type_ByteAddressBuffer& SourceParticles [[buffer(0)]]",
                                 "const device type_ByteAddressBuffer& SourceParticles [[buffer(0)]]");
    expected = ReplaceFirst(std::move(expected), "device type_ByteAddressBuffer& ForceWords [[buffer(2)]]",
                            "const device type_ByteAddressBuffer& ForceWords [[buffer(2)]]");
    CHECK(qualified == expected);  // Every body, cast, output and unrelated signature byte is preserved.
    const auto again = QualifyVfxGpuMetalInputs(capture.manifest, Entry, qualified, capture.spirv);
    REQUIRE(again.HasValue());
    CHECK(again.Value() == qualified);
    const auto reflection = Normalize(capture, capture.json, qualified);
    REQUIRE(reflection.HasValue());
    REQUIRE(reflection.Value().bindings.size() == 5);
    REQUIRE(reflection.Value().parameters.size() == 6);
    REQUIRE(reflection.Value().targetBindings.size() == 5);
    CHECK(reflection.Value().stageInterface.empty());
    for (std::size_t i = 0; i < 6; ++i) {
        const auto &parameter = reflection.Value().parameters[i];
        CHECK(parameter.id.value == i + 1);
        CHECK(parameter.binding.value == 5);
        CHECK(parameter.type == ShaderValueType::Uint32);
        CHECK(parameter.byteOffset == i * 16);
    }
    for (std::size_t i = 0; i < 5; ++i)
        CHECK(reflection.Value().targetBindings[i].nativeBinding == i);
}

// Mutated modules below are negative parser/access-proof cases, not claims that a real validator accepted them.
TEST_CASE("Captured NMin rejects wrong operand counts", "[unit][renderer][shader][vfx][capture]") {
    const Capture capture;
    const auto original = Words(capture.spirv);
    const auto at = Minimum(original);
    auto missing = original;
    missing.erase(missing.begin() + at + 6);
    missing[at] = (6U << 16) | 12U;
    Rejected(capture, missing);
    auto extra = original;
    extra.insert(extra.begin() + at + 7, original[at + 6]);
    extra[at] = (8U << 16) | 12U;
    Rejected(capture, extra);
}

TEST_CASE("Captured NMin rejects integer and pointer types", "[unit][renderer][shader][vfx][capture]") {
    const Capture capture;
    const auto original = Words(capture.spirv);
    const auto at = Minimum(original);
    const auto integer = UintType(original);
    auto resultType = original;
    resultType[at + 1] = integer;
    Rejected(capture, resultType);
    const auto constant = Find(original, [integer](auto i) {
        return (i[0] & 65535) == 43 && i.size() == 4 && i[1] == integer;
    });
    auto operandType = original;
    operandType[at + 5] = original[constant + 2];
    Rejected(capture, operandType);
    auto pointer = original;
    pointer[at + 5] = SourceVariable(original);
    Rejected(capture, pointer);
}

TEST_CASE("Captured NMin rejects foreign sets unknown ops and pointer-output extinst", "[unit][renderer][shader][vfx][capture]") {
    const Capture capture;
    const auto original = Words(capture.spirv);
    const auto at = Minimum(original);
    auto foreign = original;
    foreign[at + 3] = UintType(original);
    Rejected(capture, foreign);
    for (const auto opcode : {35U, 51U, 65535U}) {
        auto words = original;
        words[at + 4] = opcode;
        Rejected(capture, words);
    }
}

TEST_CASE("Actual capture still rejects unsupported capabilities and readonly writes", "[unit][renderer][shader][vfx][capture]") {
    const Capture capture;
    const auto original = Words(capture.spirv);
    auto capability = original;
    const auto cap = Find(original, [](auto i) {
        return (i[0] & 65535) == 17;
    });
    capability[cap + 1] = 4442;  // VariablePointers is outside this closed profile.
    Rejected(capture, capability);
    const auto source = SourceVariable(original);
    const auto access = Find(original, [source](auto i) {
        return (i[0] & 65535) == 65 && i.size() >= 5 && i[3] == source;
    });
    const auto store = Find(original, [](auto i) {
        return (i[0] & 65535) == 62;
    }, access + Width(original, access));
    auto write = original;
    write[store + 1] = original[access + 2];
    Rejected(capture, write);
}

TEST_CASE("Actual capture rejects contradictory JSON and native buffer aliases", "[unit][renderer][shader][vfx][capture]") {
    const Capture capture;
    const auto qualified = Qualified(capture);
    const auto stride = ReplaceFirst(capture.json, "\"array_stride\" : 4", "\"array_stride\" : 8");
    CHECK(Normalize(capture, stride, qualified).HasError());
    const auto access = ReplaceFirst(capture.json, "\"readonly\" : true", "\"readonly\" : false");
    CHECK(Normalize(capture, access, qualified).HasError());
    const auto alias = ReplaceFirst(capture.native, "ForceWords [[buffer(2)]]", "ForceWords [[buffer(0)]]");
    const auto rejected = QualifyVfxGpuMetalInputs(capture.manifest, Entry, alias, capture.spirv);
    REQUIRE(rejected.HasError());
    CHECK(rejected.ErrorValue().code.Value() == ShaderReflectionErrors::InvalidReflection.code.Value());
}
