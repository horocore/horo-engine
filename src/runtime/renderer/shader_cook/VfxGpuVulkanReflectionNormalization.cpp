#include "Horo/Runtime/Render/ShaderArtifact.h"
#include "Horo/Runtime/Render/ShaderReflectionErrors.h"
#include "ShaderReflectionCookJson.h"
#include "VfxGpuVulkanReflection.h"

#include <algorithm>
#include <array>

namespace Horo::Render {
    namespace {
        using Foundation::Json;
        using namespace VfxGpuCookDetail;

        std::uint32_t Unsigned(const Json &value) {
            if (!value.is_number_unsigned() || value.get<std::uint64_t>() > 0xffffffffULL)
                throw InvalidEvidence{};
            return value.get<std::uint32_t>();
        }

        const Json &Rows(const Json &object, const char *key, std::size_t maximum) {
            static const auto empty = Json::array();
            const auto &rows = object.contains(key) ? object.at(key) : empty;
            if (!rows.is_array() || rows.size() > maximum)
                throw InvalidEvidence{};
            return rows;
        }

        void Forbidden(const Json &object, std::span<const char *const> keys) {
            for (const auto key : keys)
                if (object.contains(key))
                    throw InvalidEvidence{};
        }

        Json Parse(std::string_view text) {
            if (text.empty() || text.size() > 1024U * 1024U)
                throw InvalidEvidence{};
            Foundation::JsonParseGuard guard{24};
            auto json = guard.Parse(text.begin(), text.end());
            if (guard.HasDuplicate() || guard.IsTooDeep() || json.is_discarded() || !json.is_object())
                throw InvalidEvidence{};
            constexpr std::array<std::string_view, 7>
                allowed{"entryPoints", "types", "ssbos", "ubos", "inputs", "outputs", "specialization_constants"};
            for (auto it = json.begin(); it != json.end(); ++it)
                if (std::ranges::find(allowed, it.key()) == allowed.end())
                    throw InvalidEvidence{};
            if (!Rows(json, "specialization_constants", 0).empty())
                throw InvalidEvidence{};
            return json;
        }

        void Workgroup(const Json &entry, ShaderStage stage) {
            for (const auto key : {"workgroup_size_is_spec_constant", "workgroup_size_is_spec_constant_id"})
                for (const auto &value : Rows(entry, key, 3))
                    if (value != false)
                        throw InvalidEvidence{};
            const auto &group = Rows(entry, "workgroup_size", 3);
            if (stage == ShaderStage::Compute) {
                if (group != Json::array({64, 1, 1}))
                    throw InvalidEvidence{};
            } else if (!group.empty())
                throw InvalidEvidence{};
        }

        void Entry(const Json &json, const SpirvReflectionStage &stage, VfxGpuProgramRole role) {
            const auto &rows = Rows(json, "entryPoints", 1);
            const auto mode = stage.stage == ShaderStage::Compute ? "comp" : stage.stage == ShaderStage::Vertex ? "vert" : "frag";
            if (rows.size() != 1 || rows[0].at("name").get_ref<const std::string &>() != EntryName(role, stage.stage) ||
                rows[0].at("mode") != mode)
                throw InvalidEvidence{};
            Workgroup(rows[0], stage.stage);
        }

        void Counts(const Json &json, VfxGpuProgramRole role, ShaderStage stage) {
            const bool fragment = stage == ShaderStage::Fragment;
            const auto storage = fragment ? 0u : Compute(role) ? 4u : 1u;
            const auto uniform = fragment ? 0u : 1u;
            if (Rows(json, "ssbos", 4).size() != storage || Rows(json, "ubos", 1).size() != uniform)
                throw InvalidEvidence{};
            if (json.contains("types") && (!json.at("types").is_object() || json.at("types").size() > 32))
                throw InvalidEvidence{};
        }

        const Json &Bound(const Json &json, bool uniform, std::uint32_t binding) {
            const Json *found = nullptr;
            for (const auto &row : Rows(json, uniform ? "ubos" : "ssbos", 5)) {
                if (Unsigned(row.at("binding")) != binding)
                    continue;
                if (found)
                    throw InvalidEvidence{};
                found = &row;
            }
            if (!found)
                throw InvalidEvidence{};
            return *found;
        }

        const Json &Members(const Json &json, const Json &resource, std::size_t count) {
            const auto &type = json.at("types").at(resource.at("type").get<std::string>());
            const auto &members = Rows(type, "members", 6);
            if (members.size() != count)
                throw InvalidEvidence{};
            return members;
        }

        void Placement(const Json &resource, std::size_t binding, std::size_t bytes) {
            if (Unsigned(resource.at("set")) != 0 || Unsigned(resource.at("binding")) != binding ||
                Unsigned(resource.at("block_size")) != bytes)
                throw InvalidEvidence{};
            constexpr std::array keys{"array", "physical_pointer", "buffer_reference", "array_stride"};
            Forbidden(resource, keys);
        }

        void Access(const Json &resource, const Json &member, bool readonly) {
            if (resource.value("readonly", false) != readonly || resource.value("writeonly", false) || member.value("writeonly", false))
                throw InvalidEvidence{};
            if (member.contains("readonly") && member.at("readonly") != readonly)
                throw InvalidEvidence{};
        }

        bool Readonly(VfxGpuProgramRole role, std::size_t index) {
            return !Compute(role) || index == 0 || index == 2;
        }

        /** @brief Cross names the proven single unnamed runtime member by its actual ordinal; authored binary names remain exact. */
        std::string_view RuntimeMemberName(const BinaryEvidence &binary, std::size_t index) {
            const auto &names = binary.memberNames.at(index);
            if (names.empty())
                return "_m0";
            if (names.size() != 1)
                throw InvalidEvidence{};
            return names[0].empty() ? std::string_view{"_m0"} : std::string_view{names[0]};
        }

        void Storage(const Json &json, const Json &resource, const BinaryEvidence &binary, std::size_t index, VfxGpuProgramRole role) {
            Placement(resource, index, 0);
            if (resource.at("name").get_ref<const std::string &>() != ResourceNames(role)[index])
                throw InvalidEvidence{};
            const auto &member = Members(json, resource, 1)[0];
            if (member.at("name").get_ref<const std::string &>() != RuntimeMemberName(binary, index) || member.at("type") != "uint" ||
                Unsigned(member.at("offset")) != 0 || Unsigned(member.at("array_stride")) != 4)
                throw InvalidEvidence{};
            if (Rows(member, "array", 1) != Json::array({0}) || Rows(member, "array_size_is_literal", 1) != Json::array({true}))
                throw InvalidEvidence{};
            constexpr std::array keys{"matrix_stride", "row_major", "physical_pointer", "buffer_reference"};
            Forbidden(member, keys);
            Access(resource, member, Readonly(role, index));
        }

        void UniformName(const Json &json, const Json &resource, const BinaryEvidence &binary, VfxGpuProgramRole role) {
            const auto index = ResourceNames(role).size() - 1;
            const auto &name = binary.blockNames.at(index);
            const auto &type = json.at("types").at(resource.at("type").get<std::string>());
            if (resource.at("name") != name || type.at("name") != name)
                throw InvalidEvidence{};
        }

        void Plain(const Json &field) {
            constexpr std::array keys{"array", "array_stride", "array_size_is_literal", "physical_pointer", "buffer_reference"};
            Forbidden(field, keys);
            if (field.value("writeonly", false))
                throw InvalidEvidence{};
        }

        std::uint32_t UniformOffset(VfxGpuProgramRole role, std::size_t index) {
            if (Compute(role))
                return static_cast<std::uint32_t>(index * 16);
            return index == 0 ? 0u : static_cast<std::uint32_t>(64 + (index - 1) * 16);
        }

        const char *UniformType(VfxGpuProgramRole role, std::size_t index) {
            if (Compute(role) || index == 4)
                return "uvec4";
            return index == 0 ? "mat4" : "vec4";
        }

        void MatrixPacking(const Json &field, bool matrix, bool rowMajor) {
            if (matrix) {
                if (Unsigned(field.at("matrix_stride")) != 16 || field.value("row_major", false) != rowMajor)
                    throw InvalidEvidence{};
            } else {
                constexpr std::array keys{"matrix_stride", "row_major"};
                Forbidden(field, keys);
            }
        }

        ShaderReflectedParameter UniformField(const Json &field, std::size_t index, VfxGpuProgramRole role, bool rowMajor) {
            const bool matrix = !Compute(role) && index == 0;
            const bool integer = Compute(role) || index == 4;
            const auto offset = UniformOffset(role, index);
            if (field.at("name").get_ref<const std::string &>() != UniformNames(role)[index] ||
                field.at("type") != UniformType(role, index) || Unsigned(field.at("offset")) != offset)
                throw InvalidEvidence{};
            Plain(field);
            MatrixPacking(field, matrix, rowMajor);
            return {{static_cast<std::uint32_t>(index + 1)},
                    {static_cast<std::uint32_t>(ResourceNames(role).size())},
                    integer ? ShaderValueType::Uint32 : ShaderValueType::Float32,
                    static_cast<std::uint8_t>(matrix ? 4 : 1),
                    4,
                    1,
                    offset,
                    0,
                    matrix ? 16u : 0u,
                    matrix ? !rowMajor : true,
                    true};
        }

        void Uniform(const Json &json, const Json &resource, const BinaryEvidence &binary, VfxGpuProgramRole role,
                     ShaderReflectionCandidate &candidate) {
            Placement(resource, ResourceNames(role).size() - 1, Compute(role) ? 96 : 128);
            UniformName(json, resource, binary, role);
            if (resource.value("writeonly", false))
                throw InvalidEvidence{};
            const auto &fields = Members(json, resource, UniformNames(role).size());
            for (std::size_t i = 0; i < fields.size(); ++i)
                candidate.parameters.push_back(UniformField(fields[i], i, role, binary.rowMajor));
        }

        void Resources(const ShaderManifest &manifest, const Json &json, const BinaryEvidence &binary, VfxGpuProgramRole role,
                       ShaderReflectionCandidate &candidate) {
            for (std::size_t i = 0; i < manifest.bindings.size(); ++i) {
                const bool uniform = i + 1 == manifest.bindings.size();
                const auto &resource = Bound(json, uniform, static_cast<std::uint32_t>(i));
                if (uniform)
                    Uniform(json, resource, binary, role, candidate);
                else
                    Storage(json, resource, binary, i, role);
                const auto &declared = manifest.bindings[i];
                candidate.bindings.push_back({declared.id, declared.kind, declared.access, declared.arrayCount, declared.stages, true});
                candidate.targetBindings.push_back({.id = declared.id,
                                                    .nativeSpace = Unsigned(resource.at("set")),
                                                    .nativeBinding = Unsigned(resource.at("binding")),
                                                    .nativeName = resource.at("name").get<std::string>()});
            }
            if (!Compute(role))
                candidate.parameters.push_back({{100}, {1}, ShaderValueType::Uint32, 1, 1, 1, 0, 0, 0, true, true});
        }

        bool InterfaceRequired(ShaderStage stage, bool input) {
            return stage != ShaderStage::Compute && !(stage == ShaderStage::Vertex && input);
        }

        void Interfaces(const Json &json, ShaderStage stage, bool input, ShaderReflectionCandidate &candidate) {
            const bool required = InterfaceRequired(stage, input);
            const auto &rows = Rows(json, input ? "inputs" : "outputs", 1);
            if (rows.size() != (required ? 1u : 0u))
                throw InvalidEvidence{};
            if (!required)
                return;
            const auto &row = rows[0];
            if (row.at("type") != "vec4" || Unsigned(row.at("location")) != 0 || row.contains("array"))
                throw InvalidEvidence{};
            candidate.stageInterface.push_back({stage, input ? ShaderInterfaceDirection::Input : ShaderInterfaceDirection::Output, 0,
                                                ShaderValueType::Float32, 1, 4, 1, row.at("name").get<std::string>()});
        }

        void StageIdentity(const ShaderArtifactStageView &record, const SpirvReflectionStage &json, ShaderStage stage,
                           VfxGpuProgramRole role) {
            if (record.stage != stage || record.entryPoint != EntryName(role, stage) || json.stage != stage ||
                json.entryPoint != record.entryPoint)
                throw InvalidEvidence{};
        }

        ShaderReflectionCandidate Normalize(const ShaderManifest &manifest, const ShaderTargetRequirement &target,
                                            std::span<const SpirvReflectionStage> stages, const ShaderArtifactPackageView &package) {
            const auto role = Role(manifest);
            const auto count = Compute(role) ? 1u : 2u;
            if (stages.size() != count || package.stageCount != count)
                throw InvalidEvidence{};
            ShaderReflectionCandidate candidate{.backend = ShaderTargetBackend::Vulkan,
                                                .interfaceSchemaVersion = target.interfaceSchemaVersion};
            for (std::size_t i = 0; i < count; ++i) {
                const auto stage = Compute(role) ? ShaderStage::Compute : i == 0 ? ShaderStage::Vertex : ShaderStage::Fragment;
                const auto &record = package.stages[i];
                StageIdentity(record, stages[i], stage, role);
                const auto json = Parse(stages[i].json);
                Entry(json, stages[i], role);
                Counts(json, role, stage);
                if (stage != ShaderStage::Fragment)
                    Resources(manifest, json, ProveSpirv(record.bytes, role), role, candidate);
                Interfaces(json, stage, true, candidate);
                Interfaces(json, stage, false, candidate);
            }
            return candidate;
        }
    }  // namespace

    Result<NormalizedShaderReflection> NormalizeVfxGpuVulkanReflection(const ShaderManifest &manifest,
                                                                       const ShaderTargetRequirement &target,
                                                                       std::span<const SpirvReflectionStage> stages,
                                                                       std::span<const ShaderReflectionBindingName> names,
                                                                       const CompiledShaderArtifact &compiled) {
        try {
            if (ValidateVfxGpuVulkanReflection(manifest, target, names).HasError() || compiled.backend != target.backend ||
                compiled.payloadFormat != target.payloadFormat)
                throw VfxGpuCookDetail::InvalidEvidence{};
            const auto decoded = DecodeShaderArtifactStages(compiled.payload, target.backend, target.payloadFormat, 16U * 1024U * 1024U);
            if (decoded.HasError())
                throw VfxGpuCookDetail::InvalidEvidence{};
            return NormalizeShaderReflection(manifest, target, Normalize(manifest, target, stages, decoded.Value()));
        } catch (const Foundation::Json::exception &) {
            return Result<NormalizedShaderReflection>::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
        } catch (const VfxGpuCookDetail::InvalidEvidence &) {
            return Result<NormalizedShaderReflection>::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
        } catch (const std::bad_alloc &) {
            return Result<NormalizedShaderReflection>::Failure(MakeError(ShaderReflectionErrors::AllocationFailed));
        }
    }
}  // namespace Horo::Render
