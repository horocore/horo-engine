#include "Horo/Runtime/Render/ShaderReflectionErrors.h"
#include "RegisterRasterReflectionInternal.h"
#include "VfxGpuReflection.h"

#include <algorithm>

namespace Horo::Render::VfxGpuCookDetail {
    namespace {
        using Foundation::Json;

        std::uint32_t Unsigned(const Json &value) {
            if (!value.is_number_unsigned() || value.get<std::uint64_t>() > 0xffffffffULL)
                throw InvalidEvidence{};
            return value.get<std::uint32_t>();
        }

        const Json &Rows(const Json &value, const char *key, std::size_t limit) {
            static const Json empty = Json::array();
            const auto &rows = value.contains(key) ? value.at(key) : empty;
            if (!rows.is_array() || rows.size() > limit)
                throw InvalidEvidence{};
            return rows;
        }

        Json Parse(std::string_view source) {
            if (source.empty() || source.size() > 1024 * 1024)
                throw InvalidEvidence{};
            Foundation::JsonParseGuard guard{24};
            auto json = guard.Parse(source.begin(), source.end());
            if (guard.HasDuplicate() || guard.IsTooDeep() || json.is_discarded() || !json.is_object())
                throw InvalidEvidence{};
            return json;
        }

        void RootKeys(const Json &json) {
            constexpr std::array<std::string_view, 7>
                allowed{"entryPoints", "types", "ssbos", "ubos", "inputs", "outputs", "specialization_constants"};
            for (auto it = json.begin(); it != json.end(); ++it)
                if (std::ranges::find(allowed, it.key()) == allowed.end())
                    throw InvalidEvidence{};
            if (!Rows(json, "specialization_constants", 0).empty())
                throw InvalidEvidence{};
            if (json.contains("types") && (!json.at("types").is_object() || json.at("types").size() > 32))
                throw InvalidEvidence{};
        }

        void Specialization(const Json &entry) {
            for (const auto key : {"workgroup_size_is_spec_constant", "workgroup_size_is_spec_constant_id"})
                for (const auto &value : Rows(entry, key, 3))
                    if (value != false)
                        throw InvalidEvidence{};
        }

        void Workgroup(const Json &entry, ShaderStage stage) {
            const auto &group = Rows(entry, "workgroup_size", 3);
            if (stage != ShaderStage::Compute) {
                if (!group.empty())
                    throw InvalidEvidence{};
                return;
            }
            if (group.size() != 3 || Unsigned(group[0]) != 64 || Unsigned(group[1]) != 1 || Unsigned(group[2]) != 1)
                throw InvalidEvidence{};
        }

        void Entry(const Json &json, VfxGpuProgramRole role, ShaderStage stage) {
            const auto &entries = Rows(json, "entryPoints", 1);
            if (const auto mode = stage == ShaderStage::Compute  ? "comp"
                                  : stage == ShaderStage::Vertex ? "vert"
                                                                 : "frag";
                entries.size() != 1 || entries[0].at("name").get_ref<const std::string &>() != EntryName(role, stage) ||
                entries[0].at("mode") != mode)
                throw InvalidEvidence{};
            Specialization(entries[0]);
            Workgroup(entries[0], stage);
        }

        const Json &Bound(const Json &rows, std::uint32_t binding) {
            const Json *found = nullptr;
            for (const auto &entry : rows) {
                if (Unsigned(entry.at("binding")) != binding)
                    continue;
                if (found)
                    throw InvalidEvidence{};
                found = &entry;
            }
            if (!found)
                throw InvalidEvidence{};
            return *found;
        }

        const Buffer &NativeBuffer(const NativeEvidence &native, std::string_view name, bool uniform) {
            const auto found = std::ranges::find_if(native.buffers, [&](const auto &buffer) {
                return uniform ? buffer.constant : buffer.name == name;
            });
            if (found == native.buffers.end())
                throw InvalidEvidence{};
            return *found;
        }

        void ResourceShape(const Json &entry, std::uint32_t binding, std::uint32_t bytes) {
            if (Unsigned(entry.at("set")) != 0 || Unsigned(entry.at("binding")) != binding || Unsigned(entry.at("block_size")) != bytes)
                throw InvalidEvidence{};
            constexpr std::array forbidden{"array", "physical_pointer", "buffer_reference", "array_stride"};
            for (const auto key : forbidden)
                if (entry.contains(key))
                    throw InvalidEvidence{};
        }

        void Access(const Json &entry, const Json &member, bool readonly) {
            if (entry.value("readonly", false) != readonly || (readonly && entry.value("writeonly", false)))
                throw InvalidEvidence{};
            if (readonly && member.value("writeonly", false))
                throw InvalidEvidence{};
            if (member.contains("readonly") && member.at("readonly") != readonly)
                throw InvalidEvidence{};
        }

        void RuntimeArray(const Json &field) {
            const auto &array = Rows(field, "array", 1);
            const auto &literal = Rows(field, "array_size_is_literal", 1);
            if (array.size() != 1 || literal.size() != 1 || literal[0] != true || Unsigned(array[0]) != 0)
                throw InvalidEvidence{};
        }

        void RuntimeType(const Json &field, const Buffer &native) {
            if (field.at("type") != "uint" || Unsigned(field.at("offset")) != 0 || Unsigned(field.at("array_stride")) != 4 ||
                native.bytes != 4)
                throw InvalidEvidence{};
            if (field.contains("matrix_stride") || field.contains("row_major") || field.contains("physical_pointer"))
                throw InvalidEvidence{};
        }

        const Json &Runtime(const Json &json, const Json &entry, const Buffer &native) {
            const auto &members = Rows(json.at("types").at(entry.at("type").get<std::string>()), "members", 1);
            if (members.size() != 1)
                throw InvalidEvidence{};
            const auto &field = members[0];
            RuntimeArray(field);
            RuntimeType(field, native);
            if (!native.wrapperMember.empty() && field.at("name") != native.wrapperMember)
                throw InvalidEvidence{};
            return field;
        }

        void Storage(const Json &json, const Json &entry, const Buffer &native, std::size_t index, VfxGpuProgramRole role) {
            if (const auto names = ResourceNames(role);
                entry.at("name").get_ref<const std::string &>() != names[index] || native.name != names[index] || native.constant)
                throw InvalidEvidence{};
            ResourceShape(entry, static_cast<std::uint32_t>(index), 0);
            const bool readonly = !Compute(role) || index == 0 || index == 2;
            Access(entry, Runtime(json, entry, native), readonly);
            if (native.readonly != readonly)
                throw InvalidEvidence{};
        }

        void UniformName(const Json &json, const Json &entry, std::string_view canonical, std::string_view binary) {
            if (entry.at("name").get_ref<const std::string &>() == canonical)
                return;
            const auto &type = json.at("types").at(entry.at("type").get<std::string>());
            if (binary.empty() || entry.at("name").get_ref<const std::string &>() != binary ||
                type.at("name").get_ref<const std::string &>() != binary)
                throw InvalidEvidence{};
        }

        void PlainMember(const Json &field) {
            constexpr std::array forbidden{"array", "array_stride", "array_size_is_literal", "physical_pointer"};
            for (const auto key : forbidden)
                if (field.contains(key))
                    throw InvalidEvidence{};
            if (field.value("writeonly", false))
                throw InvalidEvidence{};
        }

        void UniformMember(const Json &field, const Field &native, bool matrix, bool rowMajor) {
            if (field.at("name") != native.name || field.at("type") != native.type || Unsigned(field.at("offset")) != native.offset)
                throw InvalidEvidence{};
            PlainMember(field);
            if (matrix) {
                if (Unsigned(field.at("matrix_stride")) != native.matrixStride || field.value("row_major", false) != rowMajor)
                    throw InvalidEvidence{};
            } else if (field.contains("matrix_stride") || field.contains("row_major"))
                throw InvalidEvidence{};
        }

        void Uniform(const Json &json, const Json &entry, const Buffer &native, const BinaryEvidence &binary, VfxGpuProgramRole role) {
            const auto names = ResourceNames(role);
            const auto index = names.size() - 1;
            ResourceShape(entry, static_cast<std::uint32_t>(index), Compute(role) ? 96 : 128);
            if (!native.constant || !native.readonly || native.bytes != (Compute(role) ? 96u : 128u))
                throw InvalidEvidence{};
            UniformName(json, entry, names[index], binary.blockNames.at(index));
            const auto &fields = Rows(json.at("types").at(entry.at("type").get<std::string>()), "members", 6);
            if (fields.size() != native.fields.size())
                throw InvalidEvidence{};
            for (std::size_t i = 0; i < fields.size(); ++i)
                UniformMember(fields[i], native.fields[i], !Compute(role) && i == 0, binary.rowMajor);
        }

        void ResourceCounts(const Json &json, VfxGpuProgramRole role, ShaderStage stage) {
            const auto storage = stage == ShaderStage::Fragment ? 0u : Compute(role) ? 4u : 1u;
            const auto uniform = stage == ShaderStage::Fragment ? 0u : 1u;
            if (Rows(json, "ssbos", storage).size() != storage || Rows(json, "ubos", uniform).size() != uniform)
                throw InvalidEvidence{};
        }

        std::size_t InterfaceCount(ShaderStage stage, bool input) {
            return stage == ShaderStage::Compute || (stage == ShaderStage::Vertex && input) ? 0u : 1u;
        }

        void InterfaceRow(const Json &row) {
            if (row.at("type") != "vec4" || Unsigned(row.at("location")) != 0 || row.contains("array"))
                throw InvalidEvidence{};
        }

        void InterfaceRows(const Json &json, const NativeEvidence &native, ShaderStage stage, bool input,
                           ShaderReflectionCandidate &candidate) {
            const auto expected = InterfaceCount(stage, input);
            const auto &rows = Rows(json, input ? "inputs" : "outputs", expected);
            if (rows.size() != expected)
                throw InvalidEvidence{};
            const auto direction = input ? ShaderInterfaceDirection::Input : ShaderInterfaceDirection::Output;
            if (const auto count = std::ranges::count(native.interfaces, direction, &ShaderReflectedInterfaceVariable::direction);
                static_cast<std::size_t>(count) != expected)
                throw InvalidEvidence{};
            if (!expected)
                return;
            InterfaceRow(rows[0]);
            const auto found = std::ranges::find(native.interfaces, direction, &ShaderReflectedInterfaceVariable::direction);
            candidate.stageInterface.push_back(*found);
        }

        void AddParameters(const ShaderManifest &manifest, const NativeEvidence &native, VfxGpuProgramRole role,
                           ShaderReflectionCandidate &candidate) {
            const auto &uniform = NativeBuffer(native, {}, true);
            const auto binding = ResourceNames(role).size();
            for (std::size_t i = 0; i < uniform.fields.size(); ++i) {
                const auto &field = uniform.fields[i];
                const bool integer = Compute(role) || i == 4;
                const bool matrix = !Compute(role) && i == 0;
                candidate.parameters.push_back({{static_cast<std::uint32_t>(i + 1)},
                                                {static_cast<std::uint32_t>(binding)},
                                                integer ? ShaderValueType::Uint32 : ShaderValueType::Float32,
                                                static_cast<std::uint8_t>(matrix ? 4 : 1),
                                                4,
                                                1,
                                                field.offset,
                                                0,
                                                field.matrixStride,
                                                true,
                                                true});
            }
            if (!Compute(role))
                candidate.parameters.push_back({{100}, {1}, ShaderValueType::Uint32, 1, 1, 1, 0, 0, 0, true, true});
            if (candidate.parameters.size() != manifest.parameters.size())
                throw InvalidEvidence{};
        }

        void Resources(const ShaderManifest &manifest, const Json &json, const NativeEvidence &native, const BinaryEvidence &binary,
                       VfxGpuProgramRole role, ShaderReflectionCandidate &candidate) {
            const auto names = ResourceNames(role);
            for (std::size_t i = 0; i < names.size(); ++i) {
                const bool uniform = i + 1 == names.size();
                const auto &buffer = NativeBuffer(native, names[i], uniform);
                const auto &entry = Bound(Rows(json, uniform ? "ubos" : "ssbos", 5), static_cast<std::uint32_t>(i));
                if (uniform)
                    Uniform(json, entry, buffer, binary, role);
                else
                    Storage(json, entry, buffer, i, role);
                const auto &declared = manifest.bindings[i];
                candidate.bindings.push_back({declared.id, declared.kind, declared.access, declared.arrayCount, declared.stages, true});
                candidate.targetBindings.push_back({.id = declared.id, .nativeBinding = buffer.slot, .nativeName = buffer.name});
            }
            AddParameters(manifest, native, role, candidate);
        }

        ShaderReflectionCandidate Normalize(const ShaderManifest &manifest, std::span<const SpirvReflectionStage> stages,
                                            std::span<const std::string> translations, std::span<const std::uint8_t> spirv) {
            const auto role = Role(manifest);
            const auto binary = ProveSpirv(spirv, role);
            if (stages.size() != (Compute(role) ? 1u : 2u) || translations.size() != stages.size())
                throw InvalidEvidence{};
            ShaderReflectionCandidate candidate{.backend = ShaderTargetBackend::Metal, .interfaceSchemaVersion = 1};
            for (std::size_t i = 0; i < stages.size(); ++i) {
                const auto stage = Compute(role) ? ShaderStage::Compute : i == 0 ? ShaderStage::Vertex : ShaderStage::Fragment;
                if (stages[i].stage != stage || stages[i].entryPoint != EntryName(role, stage))
                    throw InvalidEvidence{};
                const auto json = Parse(stages[i].json);
                RootKeys(json);
                Entry(json, role, stage);
                ResourceCounts(json, role, stage);
                const auto native = ParseNative(translations[i], role, stage, false);
                InterfaceRows(json, native, stage, true, candidate);
                InterfaceRows(json, native, stage, false, candidate);
                if (stage != ShaderStage::Fragment)
                    Resources(manifest, json, native, binary, role, candidate);
            }
            return candidate;
        }
    }  // namespace
}  // namespace Horo::Render::VfxGpuCookDetail

namespace Horo::Render {
    Result<NormalizedShaderReflection> NormalizeVfxGpuMetalReflection(const ShaderManifest &manifest, const ShaderTargetRequirement &target,
                                                                      std::span<const SpirvReflectionStage> stages,
                                                                      std::span<const std::string> translations,
                                                                      std::span<const ShaderReflectionBindingName> names,
                                                                      std::span<const std::uint8_t> validatedSpirv) {
        using namespace VfxGpuCookDetail;
        try {
            if (ValidateVfxGpuMetalReflection(manifest, target, names).HasError())
                throw InvalidEvidence{};
            return NormalizeShaderReflection(manifest, target, Normalize(manifest, stages, translations, validatedSpirv));
        } catch (const Foundation::Json::exception &) {
            return Result<NormalizedShaderReflection>::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
        } catch (const InvalidEvidence &) {
            return Result<NormalizedShaderReflection>::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
        } catch (const RegisterRasterCookDetail::InvalidEvidence &) {
            return Result<NormalizedShaderReflection>::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
        } catch (const MetalReflectionCookDetail::InvalidEvidence &) {
            return Result<NormalizedShaderReflection>::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
        } catch (const std::bad_alloc &) {
            return Result<NormalizedShaderReflection>::Failure(MakeError(ShaderReflectionErrors::AllocationFailed));
        }
    }
}  // namespace Horo::Render
