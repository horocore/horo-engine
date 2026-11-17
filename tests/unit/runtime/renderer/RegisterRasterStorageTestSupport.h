#pragma once
#include "ShaderReflectionCook.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Render::RegisterRasterStorageTests {
    constexpr std::string_view Types = R"("types":{
        "_ubo":{"members":[{"name":"Tint","type":"vec4","offset":0}]},
        "_light":{"members":[
          {"name":"positionRange","type":"vec4","offset":0},{"name":"direction","type":"vec3","offset":16},
          {"name":"kind","type":"uint","offset":28},{"name":"colorIntensity","type":"vec4","offset":32},
          {"name":"cones","type":"vec2","offset":48},{"name":"identityLow","type":"uint","offset":56},
          {"name":"identityHigh","type":"uint","offset":60}]},
        "_membership":{"members":[{"name":"offset","type":"uint","offset":0},{"name":"count","type":"uint","offset":4},
          {"name":"omitted","type":"uint","offset":8},{"name":"reserved","type":"uint","offset":12}]},
        "_lights":{"members":[{"name":"_m0","type":"_light","array":[0],"array_size_is_literal":[true],"offset":0,"array_stride":64}]},
        "_memberships":{"members":[{"name":"_m0","type":"_membership","array":[0],"array_size_is_literal":[true],"offset":0,"array_stride":16}]},
        "_references":{"members":[{"name":"_m0","type":"uint","array":[0],"array_size_is_literal":[true],"offset":0,"array_stride":4}]}})";
    constexpr std::string_view Storages = R"("ssbos":[
        {"name":"ClusterLights","type":"_lights","set":0,"binding":3,"readonly":true,"block_size":0},
        {"name":"ClusterMembership","type":"_memberships","set":0,"binding":4,"readonly":true,"block_size":0},
        {"name":"ClusterReferences","type":"_references","set":0,"binding":5,"readonly":true,"block_size":0}])";
    constexpr std::string_view Structures = R"(
        struct FrameData { float4 Tint; };
        struct PackedLight { float4 positionRange; packed_float3 direction; uint kind; float4 colorIntensity; float2 cones; uint identityLow; uint identityHigh; };
        struct LightMembership { uint offset; uint count; uint omitted; uint reserved; };
    )";
    constexpr std::string_view Arguments = R"(const device PackedLight* ClusterLights [[buffer(3)]],
        const device LightMembership* ClusterMembership [[buffer(4)]],const device uint* ClusterReferences [[buffer(5)]])";

    inline void Replace(std::string &text, std::string_view before, std::string_view after) {
        const auto position = text.find(before);
        REQUIRE(position != text.npos);
        text.replace(position, before.size(), after);
    }

    /** @brief Scripted JSON/MSL parser evidence only; no output here is native shader or GPU qualification. */
    struct Evidence final {
        ShaderTargetRequirement target{ShaderTargetBackend::Metal, ShaderPayloadFormat::MetalLibrary24, 1, 1, 8, 0, false, true};
        ShaderManifest
            manifest{.schemaVersion = 1,
                     .sourceIdentity = "tests/storage-raster.hlsl",
                     .sourceRevision = 1,
                     .entryPoints = {{ShaderStage::Vertex, "VSMain"}, {ShaderStage::Fragment, "PSMain"}},
                     .bindings =
                         {{{1}, ShaderResourceKind::UniformBuffer, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Vertex},
                          {{4}, ShaderResourceKind::StorageBuffer, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment},
                          {{5}, ShaderResourceKind::StorageBuffer, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment},
                          {{6}, ShaderResourceKind::StorageBuffer, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment}},
                     .parameters = {{{1}, {1}, ShaderValueType::Float32, 1, 4, 1},
                                    {{100}, {4}, ShaderValueType::Float32, 1, 4, 1},
                                    {{101}, {4}, ShaderValueType::Float32, 1, 3, 1},
                                    {{102}, {4}, ShaderValueType::Uint32, 1, 1, 1},
                                    {{103}, {4}, ShaderValueType::Float32, 1, 4, 1},
                                    {{104}, {4}, ShaderValueType::Float32, 1, 2, 1},
                                    {{105}, {4}, ShaderValueType::Uint32, 1, 1, 1},
                                    {{106}, {4}, ShaderValueType::Uint32, 1, 1, 1},
                                    {{110}, {5}, ShaderValueType::Uint32, 1, 1, 1},
                                    {{111}, {5}, ShaderValueType::Uint32, 1, 1, 1},
                                    {{112}, {5}, ShaderValueType::Uint32, 1, 1, 1},
                                    {{113}, {5}, ShaderValueType::Uint32, 1, 1, 1},
                                    {{120}, {6}, ShaderValueType::Uint32, 1, 1, 1}},
                     .targets = {target}};
        std::vector<ShaderReflectionBindingName> names{{{1}, "type.FrameData", {{{1}, "Tint"}}},
                                                       {{4},
                                                        "ClusterLights",
                                                        {{{100}, "positionRange"},
                                                         {{101}, "direction"},
                                                         {{102}, "kind"},
                                                         {{103}, "colorIntensity"},
                                                         {{104}, "cones"},
                                                         {{105}, "identityLow"},
                                                         {{106}, "identityHigh"}}},
                                                       {{5},
                                                        "ClusterMembership",
                                                        {{{110}, "offset"}, {{111}, "count"}, {{112}, "omitted"}, {{113}, "reserved"}}},
                                                       {{6}, "ClusterReferences", {{{120}, "_m0"}}}};
        std::array<SpirvReflectionStage, 2> stages{
            {{ShaderStage::Vertex, "VSMain",
              std::string{R"({"entryPoints":[{"name":"VSMain","mode":"vert"}],)"} + std::string{Types} +
                  R"(,"ubos":[{"name":"type.FrameData","type":"_ubo","set":0,"binding":0,"block_size":16}]})"},
             {ShaderStage::Fragment, "PSMain",
              std::string{R"({"entryPoints":[{"name":"PSMain","mode":"frag"}],)"} + std::string{Types} + "," + std::string{Storages} +
                  "}"}}};
        std::array<std::string, 2> translations{{std::string{Structures} + "vertex void VSMain(constant FrameData& frame [[buffer(0)]]) {}",
                                                 std::string{Structures} + "fragment void PSMain(" + std::string{Arguments} + ") {}"}};

        auto Normalize() const {
            return NormalizeRegisterRasterReflection(manifest, target, stages, translations, names);
        }

        void Wrappers() {
            translations[1] = std::string{Structures} + R"(
                struct LightBuffer { PackedLight _m0[1]; }; struct MembershipBuffer { LightMembership _m0[1]; }; struct ReferenceBuffer { uint _m0[1]; };
                fragment void PSMain(const device LightBuffer& ClusterLights [[buffer(3)]],
                    const device MembershipBuffer& ClusterMembership [[buffer(4)]],const device ReferenceBuffer& ClusterReferences [[buffer(5)]]) {})";
        }

        void BothStages() {
            for (auto &binding : manifest.bindings)
                if (binding.kind == ShaderResourceKind::StorageBuffer)
                    binding.stages = ShaderStageVisibility::Vertex | ShaderStageVisibility::Fragment;
            stages[0].json.pop_back();
            stages[0].json += "," + std::string{Storages} + "}";
            translations[0] =
                std::string{Structures} + "vertex void VSMain(constant FrameData& frame [[buffer(0)]]," + std::string{Arguments} + ") {}";
        }

        void AdditionalReference(std::string_view name, std::uint32_t binding, std::uint32_t parameter, std::uint32_t slot) {
            manifest.bindings.push_back(
                {{binding}, ShaderResourceKind::StorageBuffer, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment});
            manifest.parameters.push_back({{parameter}, {binding}, ShaderValueType::Uint32, 1, 1, 1});
            names.push_back({{binding}, std::string{name}, {{{parameter}, "_m0"}}});
            const auto entry = "\"ssbos\":[{\"name\":\"" + std::string{name} +
                               "\",\"type\":\"_references\",\"set\":0,\"binding\":" + std::to_string(slot) +
                               ",\"readonly\":true,\"block_size\":0},";
            Replace(stages[1].json, "\"ssbos\":[", entry);
            const auto argument =
                "fragment void PSMain(const device uint* " + std::string{name} + " [[buffer(" + std::to_string(slot) + ")]],";
            Replace(translations[1], "fragment void PSMain(", argument);
        }
    };
}  // namespace Horo::Render::RegisterRasterStorageTests
