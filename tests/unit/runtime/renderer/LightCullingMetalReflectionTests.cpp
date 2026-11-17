#include "Horo/Runtime/Render/LightCullingKernel.h"
#include "Horo/Runtime/Render/ShaderArtifact.h"
#include "Horo/Runtime/Render/ShaderReflectionErrors.h"
#include "LightCullingMetalReflection.h"
#include "support/LightCullingMetalCapturedSpirv.h"
#include "support/TypedIdentityTestSupport.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstring>

namespace {
    using namespace Horo;
    using namespace Horo::Render;

    // Scripted parser evidence only: no native compiler output or GPU execution is claimed by these fixtures.
    struct Evidence {
        ShaderTargetRequirement target{ShaderTargetBackend::Metal, ShaderPayloadFormat::MetalLibrary24, 1, 1, 32, 128, true, true};
        ShaderManifest manifest = MakeLightCullingShaderManifest(target);
        std::vector<ShaderReflectionBindingName> names = LightCullingMetalReflectionNames();
        std::array<SpirvReflectionStage, 1> stages{{{ShaderStage::Compute, "CullLights", R"({
          "entryPoints":[{"name":"CullLights","mode":"comp","workgroup_size":[64,1,1],"workgroup_size_is_spec_constant":[false,false,false]}],
          "types":{
            "light":{"members":[{"name":"positionRange","type":"vec4","offset":0},{"name":"direction","type":"vec3","offset":16},{"name":"kind","type":"uint","offset":28},{"name":"colorIntensity","type":"vec4","offset":32},{"name":"cones","type":"vec2","offset":48},{"name":"identity","type":"uvec2","offset":56}]},
            "cluster":{"members":[{"name":"planes","type":"vec4","offset":0,"array":[6],"array_size_is_literal":[true],"array_stride":16}]},
            "lights":{"members":[{"name":"_m0","type":"light","offset":0,"array":[0],"array_size_is_literal":[true],"array_stride":64}]},
            "clusters":{"members":[{"name":"_m0","type":"cluster","offset":0,"array":[0],"array_size_is_literal":[true],"array_stride":96}]},
            "membership":{"members":[{"name":"_m0","type":"uvec4","offset":0,"array":[0],"array_size_is_literal":[true],"array_stride":16}]},
            "references":{"members":[{"name":"_m0","type":"uint","offset":0,"array":[0],"array_size_is_literal":[true],"array_stride":4}]},
            "dispatch":{"members":[{"name":"LightCount","type":"uint","offset":0},{"name":"ClusterCount","type":"uint","offset":4},{"name":"ReferencesPerCluster","type":"uint","offset":8},{"name":"Reserved","type":"uint","offset":12}]}},
          "ssbos":[{"name":"Lights","type":"lights","readonly":true,"block_size":0,"set":0,"binding":0},{"name":"Clusters","type":"clusters","readonly":true,"block_size":0,"set":0,"binding":1},{"name":"Membership","type":"membership","block_size":0,"set":0,"binding":2},{"name":"References","type":"references","block_size":0,"set":0,"binding":3}],
          "ubos":[{"name":"type.Dispatch","type":"dispatch","block_size":16,"set":0,"binding":4}]})"}}};
        std::array<std::string, 1> native{{R"(
          struct Light { float4 positionRange; packed_float3 direction; uint kind; float4 colorIntensity; float2 cones; uint2 identity; };
          struct Cluster { float4 planes[6]; };
          struct L { Light _m0[1]; }; struct C { Cluster _m0[1]; };
          struct M { uint4 _m0[1]; }; struct R { uint _m0[1]; };
          struct D { uint LightCount; uint ClusterCount; uint ReferencesPerCluster; uint Reserved; };
          // An unrelated declaration cannot establish a resource argument for the selected kernel.
          kernel void Other(device uint* ignored [[buffer(29)]]) {}
          kernel void CullLights(const device L& Lights [[buffer(11)]], const device C& Clusters [[buffer(3)]],
            device M& Membership [[buffer(19)]], device R& References [[buffer(5)]],
            constant D& Dispatch [[buffer(7)]], uint3 id [[thread_position_in_grid]]) {}
        )"}};

        Result<NormalizedShaderReflection> Normalize(std::span<const std::uint8_t> spirv = {}) const {
            return NormalizeLightCullingMetalReflection(manifest, target, stages, native, names, spirv);
        }

        void JsonReplace(std::string_view from, std::string_view to) {
            Replace(stages[0].json, from, to);
        }

        void NativeReplace(std::string_view from, std::string_view to) {
            Replace(native[0], from, to);
        }

        static void Replace(std::string &text, std::string_view from, std::string_view to) {
            const auto at = text.find(from);
            REQUIRE(at != std::string::npos);
            text.replace(at, from.size(), to);
        }
    };

    struct BinaryEvidence {
        std::vector<std::uint32_t> words{Tests::LightCullingCapturedSpirv.begin(), Tests::LightCullingCapturedSpirv.end()};

        std::span<const std::uint8_t> Bytes() const {
            return {reinterpret_cast<const std::uint8_t *>(words.data()), words.size() * sizeof(std::uint32_t)};
        }

        std::size_t Find(std::uint32_t opcode, std::size_t occurrence = 0) const {
            for (std::size_t at = 5; at < words.size(); at += words[at] >> 16) {
                if ((words[at] & 65535) == opcode && occurrence-- == 0)
                    return at;
            }
            FAIL("Missing genuine instruction");
            return 0;
        }

        std::size_t InputChain() const {
            const auto variable = Find(59);
            const auto root = words[variable + 2];
            for (std::size_t at = 5; at < words.size(); at += words[at] >> 16)
                if ((words[at] & 65535) == 65 && words[at + 3] == root)
                    return at;
            FAIL("Missing genuine input access chain");
            return 0;
        }

        std::uint32_t InputPointer() const {
            return words[InputChain() + 2];
        }

        void StoreThroughInputAlias() {
            const auto chain = InputChain();
            const auto type = words[chain + 1];
            const auto input = words[chain + 2];
            const auto alias = words[3]++;
            const auto store = Find(62);
            const std::array copy{(4u << 16) | 83u, type, alias, input};
            words.insert(words.begin() + store, copy.begin(), copy.end());
            words[store + copy.size() + 1] = alias;
        }
    };

}  // namespace

TEST_CASE("Light compute reflection proves actual selected-entry slots and canonical packed layouts",
          "[renderer][shader-cook][light-compute]") {
    Evidence fixture;
    const auto result = fixture.Normalize();
    REQUIRE(result.HasValue());
    const std::array<std::uint32_t, 5> slots{11, 3, 19, 5, 7};
    REQUIRE(result.Value().targetBindings.size() == slots.size());
    for (std::size_t index = 0; index < slots.size(); ++index)
        CHECK(result.Value().targetBindings[index].nativeBinding == slots[index]);
    REQUIRE(result.Value().parameters.size() == 4);
    CHECK(result.Value().parameters[3].byteOffset == 12);
    const std::array stages{ShaderArtifactStage{ShaderStage::Compute, "CullLights", {'s', 'c', 'r', 'i', 'p', 't', 'e', 'd'}}};
    auto packaged = PackageShaderArtifactStages(fixture.target.backend, fixture.target.payloadFormat, stages, 1024);
    REQUIRE(packaged.HasValue());
    const auto &value = result.Value();
    ShaderReflectionCandidate candidate{value.backend,        value.interfaceSchemaVersion, value.bindings, value.parameters,
                                        value.stageInterface, value.targetBindings,         value.sourceMap};
    CompiledShaderArtifact artifact{.backend = fixture.target.backend,
                                    .payloadFormat = fixture.target.payloadFormat,
                                    .payload = std::move(packaged).Value()};
    artifact.artifactKey.bytes[0] = 1;
    CHECK(ValidateCookedLightCullingKernel({fixture.manifest, fixture.target, std::move(artifact), std::move(candidate)},
                                           ShaderTargetBackend::Metal)
              .HasValue());
}

TEST_CASE("Light compute reflection rejects altered access packing slots and selected entry evidence",
          "[renderer][shader-cook][light-compute]") {
    Evidence fixture;
    SECTION("writable light input") {
        fixture.NativeReplace("const device L&", "device L&");
    }
    SECTION("actual spec constant id spelling") {
        fixture.JsonReplace("workgroup_size_is_spec_constant", "workgroup_size_is_spec_constant_id");
        fixture.JsonReplace("[false,false,false]", "[true,false,false]");
    }
    SECTION("readonly membership output") {
        fixture.NativeReplace("device M&", "const device M&");
    }
    SECTION("wrong packed direction alignment") {
        fixture.NativeReplace("packed_float3 direction", "float3 direction");
    }
    SECTION("wrong light stride") {
        fixture.JsonReplace("\"array_stride\":64", "\"array_stride\":80");
    }
    SECTION("wrong cluster extent") {
        fixture.NativeReplace("planes[6]", "planes[5]");
    }
    SECTION("wrong output element") {
        fixture.NativeReplace("uint4 _m0", "uint2 _m0");
    }
    SECTION("aliased native slot") {
        fixture.NativeReplace("buffer(19)", "buffer(11)");
    }
    SECTION("wrong dispatch member") {
        fixture.NativeReplace("uint Reserved", "float2 Reserved");
    }
    SECTION("absent selected kernel") {
        fixture.NativeReplace("kernel void CullLights", "kernel void Renamed");
    }
    SECTION("wrong dispatch group") {
        fixture.JsonReplace("[64,1,1]", "[32,1,1]");
    }
    SECTION("readonly output JSON") {
        fixture.JsonReplace("\"Membership\",\"type\":\"membership\"", "\"Membership\",\"type\":\"membership\",\"readonly\":true");
    }
    SECTION("foreign stage") {
        fixture.stages[0].stage = ShaderStage::Fragment;
    }
    Tests::RequireActionableError(fixture.Normalize(), ShaderReflectionErrors::InvalidReflection);
}

TEST_CASE("Light compute profile rejects alternate authored interfaces without weakening raster admission",
          "[renderer][shader-cook][light-compute]") {
    Evidence fixture;
    REQUIRE(ValidateLightCullingMetalReflection(fixture.manifest, fixture.target, fixture.names).HasValue());
    SECTION("alternate source") {
        fixture.manifest.sourceIdentity = "other.hlsl";
    }
    SECTION("alternate compute entry") {
        fixture.manifest.entryPoints[0].name = "Other";
    }
    SECTION("alternate resource policy") {
        fixture.manifest.bindings[2].access = ShaderResourceAccess::ReadOnly;
    }
    SECTION("alternate native target") {
        fixture.target.payloadFormat = ShaderPayloadFormat::SpirV16;
    }
    SECTION("alternate authored parameter") {
        fixture.names[4].parameters[3].name = "Other";
    }
    Tests::RequireActionableError(ValidateLightCullingMetalReflection(fixture.manifest, fixture.target, fixture.names),
                                  ShaderReflectionErrors::InvalidReflection);
}

TEST_CASE("Mutable Metal compute declarations require independent genuine SPIRV readonly proof", "[renderer][shader-cook][light-compute]") {
    Evidence fixture;
    BinaryEvidence binary;
    fixture.NativeReplace("const device L&", "device L&");
    fixture.NativeReplace("const device C&", "device C&");
    Tests::RequireActionableError(fixture.Normalize(), ShaderReflectionErrors::InvalidReflection);
    REQUIRE(fixture.Normalize(binary.Bytes()).HasValue());
}

TEST_CASE("Compute binary proof rejects writable derived roots and pointer escapes", "[renderer][shader-cook][light-compute]") {
    Evidence fixture;
    BinaryEvidence binary;
    fixture.NativeReplace("const device L&", "device L&");
    SECTION("derived input store") {
        binary.words[binary.Find(62) + 1] = binary.InputPointer();
    }
    SECTION("copy-object input alias store") {
        binary.StoreThroughInputAlias();
    }
    SECTION("pointer value store") {
        binary.words[binary.Find(62) + 2] = binary.InputPointer();
    }
    SECTION("copy memory") {
        const auto at = binary.Find(62);
        binary.words[at] = (3u << 16) | 63u;
        binary.words[at + 1] = binary.InputPointer();
    }
    SECTION("atomic write") {
        const auto at = binary.Find(62);
        binary.words[at] = (3u << 16) | 228u;
    }
    SECTION("function escape") {
        const auto at = binary.Find(65);
        binary.words[at] = (binary.words[at] & 0xffff0000u) | 57u;
    }
    SECTION("pointer bitcast") {
        const auto at = binary.Find(65);
        binary.words[at] = (binary.words[at] & 0xffff0000u) | 124u;
    }
    SECTION("pointer phi") {
        const auto at = binary.Find(65);
        binary.words[at] = (binary.words[at] & 0xffff0000u) | 245u;
    }
    SECTION("pointer select") {
        const auto at = binary.Find(65);
        binary.words[at] = (binary.words[at] & 0xffff0000u) | 169u;
    }
    Tests::RequireActionableError(fixture.Normalize(binary.Bytes()), ShaderReflectionErrors::InvalidReflection);
}

TEST_CASE("Compute binary proof rejects malformed metadata and conflicting resources", "[renderer][shader-cook][light-compute]") {
    Evidence fixture;
    BinaryEvidence binary;
    SECTION("unknown extension instruction") {
        binary.words[binary.Find(12) + 4] = 999;
    }
    SECTION("truncated instruction") {
        binary.words.pop_back();
    }
    SECTION("duplicate function end") {
        binary.words.push_back((1u << 16) | 56u);
    }
    SECTION("function end before function start") {
        binary.words.insert(binary.words.begin() + 5, (1u << 16) | 56u);
    }
    SECTION("nested function start") {
        const auto at = binary.Find(54);
        const std::vector<std::uint32_t> start(binary.words.begin() + at, binary.words.begin() + at + 5);
        binary.words.insert(binary.words.begin() + at, start.begin(), start.end());
    }
    SECTION("oversized bound") {
        binary.words[3] = 65537;
    }
    SECTION("zero word count") {
        binary.words[5] &= 65535;
    }
    SECTION("unknown instruction") {
        binary.words[5] = (binary.words[5] & 0xffff0000u) | 60000;
    }
    SECTION("missing NonWritable") {
        for (std::size_t at = 5; at < binary.words.size(); at += binary.words[at] >> 16)
            if ((binary.words[at] & 65535) == 72 && binary.words[at + 3] == 24)
                binary.words[at + 3] = 25;
    }
    SECTION("wrong workgroup") {
        binary.words[binary.Find(16) + 3] = 32;
    }
    SECTION("duplicate resource binding") {
        bool first = true;
        for (std::size_t at = 5; at < binary.words.size(); at += binary.words[at] >> 16) {
            if ((binary.words[at] & 65535) != 71 || binary.words[at + 2] != 33)
                continue;
            if (first) {
                first = false;
                continue;
            }
            binary.words[at + 3] = 0;
            break;
        }
    }
    Tests::RequireActionableError(fixture.Normalize(binary.Bytes()), ShaderReflectionErrors::InvalidReflection);
}

TEST_CASE("Captured genuine compute JSON MSL and SPIRV establish readonly input evidence together",
          "[renderer][shader-cook][light-compute]") {
    Evidence fixture;
    BinaryEvidence binary;
    fixture.native[0] = Tests::LightCullingCapturedMetal;
    fixture.stages[0].json = Tests::LightCullingCapturedJson;
    Tests::RequireActionableError(fixture.Normalize(), ShaderReflectionErrors::InvalidReflection);
    REQUIRE(fixture.Normalize(binary.Bytes()).HasValue());
}

TEST_CASE("Compute input qualification changes only proven selected argument constness before native compilation",
          "[renderer][shader-cook][light-compute]") {
    BinaryEvidence binary;
    std::string original{Tests::LightCullingCapturedMetal};
    original += "\nkernel void Unrelated(device uint* Lights [[buffer(0)]]) {}\n";
    auto expected = original;
    Evidence::Replace(expected, "device type_StructuredBuffer_PackedLight& Lights",
                      "const device type_StructuredBuffer_PackedLight& Lights");
    Evidence::Replace(expected, "device type_StructuredBuffer_Cluster& Clusters", "const device type_StructuredBuffer_Cluster& Clusters");
    const auto qualified = QualifyLightCullingMetalInputs(original, binary.Bytes());
    REQUIRE(qualified.HasValue());
    CHECK(qualified.Value() == expected);
    const auto repeated = QualifyLightCullingMetalInputs(qualified.Value(), binary.Bytes());
    REQUIRE(repeated.HasValue());
    CHECK(repeated.Value() == qualified.Value());
    Evidence fixture;
    fixture.native[0] = qualified.Value();
    fixture.stages[0].json = Tests::LightCullingCapturedJson;
    CHECK(fixture.Normalize(binary.Bytes()).HasValue());
}

TEST_CASE("Compute input qualification fails before mutation without exact binary and native ABI proof",
          "[renderer][shader-cook][light-compute]") {
    BinaryEvidence binary;
    std::string source{Tests::LightCullingCapturedMetal};
    SECTION("missing binary") {
        binary.words.clear();
    }
    SECTION("input store") {
        binary.words[binary.Find(62) + 1] = binary.InputPointer();
    }
    SECTION("wrong packed input") {
        Evidence::Replace(source, "packed_float3 direction", "float3 direction");
    }
    SECTION("readonly output") {
        Evidence::Replace(source, "device type_RWStructuredBuffer_v4uint& Membership",
                          "const device type_RWStructuredBuffer_v4uint& Membership");
    }
    SECTION("wrong input name") {
        Evidence::Replace(source, "& Lights [[", "& Other [[");
    }
    SECTION("missing selected entry") {
        Evidence::Replace(source, "kernel void CullLights", "kernel void Other");
    }
    SECTION("duplicate selected entry") {
        source += std::string{Tests::LightCullingCapturedMetal};
    }
    const auto unchanged = source;
    Tests::RequireActionableError(QualifyLightCullingMetalInputs(source, binary.Bytes()), ShaderReflectionErrors::InvalidReflection);
    CHECK(source == unchanged);
}
