#include "Horo/Runtime/Render/NativeReconstructionKernel.h"
#include "Horo/Runtime/Render/ShaderReflectionErrors.h"
#include "ShaderReflectionCookJson.h"

#include <algorithm>
#include <array>
#include <limits>
#include <tuple>
#include <utility>

namespace Horo::Render {
    namespace {
        using Foundation::Json;

        struct InvalidEvidence {};

        struct UnknownUniformBlock {};

        struct UnknownSampledResource {};

        /** @brief Reads an exact unsigned field without narrowing negative or oversized evidence. */
        std::uint32_t Unsigned(const Json &value) {
            if (!value.is_number_unsigned() || value.get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max())
                throw InvalidEvidence{};
            return value.get<std::uint32_t>();
        }

        struct Shape {
            ShaderValueType type;
            std::uint8_t rows;
            std::uint8_t columns;
        };

        /** @brief Admits the explicit float viewport shapes and unsigned multiview layer scalar; other types fail closed. */
        Shape ValueShape(const std::string &type) {
            if (type == "float")
                return {ShaderValueType::Float32, 1, 1};
            if (type == "uint")
                return {ShaderValueType::Uint32, 1, 1};
            if (type.size() == 4 && type.starts_with("vec") && type[3] >= '2' && type[3] <= '4')
                return {ShaderValueType::Float32, 1, static_cast<std::uint8_t>(type[3] - '0')};
            if (type.size() == 4 && type.starts_with("mat") && type[3] >= '2' && type[3] <= '4')
                return {ShaderValueType::Float32, static_cast<std::uint8_t>(type[3] - '0'), static_cast<std::uint8_t>(type[3] - '0')};
            throw InvalidEvidence{};
        }

        /** @brief Requires one complete literal element for the initial bounded viewport profile. */
        void NoArray(const Json &value) {
            if (value.contains("array") || value.contains("physical_pointer"))
                throw InvalidEvidence{};
        }

        /** @brief Admits only one fixed literal array dimension; runtime/spec-constant and nested arrays fail closed. */
        std::pair<std::uint32_t, std::uint32_t> MemberArray(const Json &member) {
            if (member.contains("physical_pointer"))
                throw InvalidEvidence{};
            if (!member.contains("array"))
                return {1, 0};
            const auto &dimensions = member.at("array");
            const auto &literal = member.at("array_size_is_literal");
            if (!dimensions.is_array() || dimensions.size() != 1 || !literal.is_array() || literal.size() != 1 ||
                !literal[0].is_boolean() || !literal[0].get<bool>())
                throw InvalidEvidence{};
            const auto count = Unsigned(dimensions[0]);
            const auto stride = Unsigned(member.at("array_stride"));
            if (count == 0 || count > 512 || stride == 0)
                throw InvalidEvidence{};
            return {count, stride};
        }

        /** @brief Preserves exact final member packing on every stage sharing a uniform block. */
        bool SamePacking(const ShaderReflectedParameter &a, const ShaderReflectedParameter &b) {
            return a.id == b.id && a.binding == b.binding && a.type == b.type && a.rows == b.rows && a.columns == b.columns &&
                   a.arrayCount == b.arrayCount && a.byteOffset == b.byteOffset && a.arrayStride == b.arrayStride &&
                   a.matrixStride == b.matrixStride && a.columnMajor == b.columnMajor;
        }

        /** @brief Admits the finite graphics envelope independently of resource parsing and native placement. */
        bool AdmittedVulkanEnvelope(const ShaderManifest &manifest, const ShaderTargetRequirement &target,
                                    const std::span<const SpirvReflectionStage> stages,
                                    const std::span<const ShaderReflectionBindingName> names) {
            return target.backend == ShaderTargetBackend::Vulkan && target.payloadFormat == ShaderPayloadFormat::SpirV16 &&
                   stages.size() == manifest.entryPoints.size() && stages.size() == 2 && names.size() <= 128 &&
                   manifest.parameters.size() <= 512 && ValidateShaderManifest(manifest).HasValue();
        }

        /** @brief Selects the bounded compute profile by its canonical manifest and authored name identities. */
        bool AdmittedReconstructionEnvelope(const ShaderManifest &manifest, const ShaderTargetRequirement &target,
                                            std::span<const SpirvReflectionStage> stages,
                                            std::span<const ShaderReflectionBindingName> names) {
            if (stages.size() != 1 || stages.front().stage != ShaderStage::Compute || manifest.entryPoints.size() != 1)
                return false;
            for (const auto kind : {NativeReconstructionEntry::Spatial, NativeReconstructionEntry::TemporalSeed,
                                    NativeReconstructionEntry::TemporalHistory}) {
                const auto canonical = DescribeNativeReconstructionShader(kind, target);
                if (canonical.HasError())
                    return false;
                const auto &expected = canonical.Value();
                if (manifest.sourceIdentity != expected.manifest.sourceIdentity ||
                    manifest.sourceRevision != expected.manifest.sourceRevision || names.size() != expected.reflectionNames.size() ||
                    manifest.entryPoints.front().name != expected.manifest.entryPoints.front().name)
                    continue;
                const auto actualId = ComputeShaderInterfaceCompatibilityId(manifest);
                const auto canonicalId = ComputeShaderInterfaceCompatibilityId(expected.manifest);
                if (actualId.HasError() || canonicalId.HasError() || actualId.Value() != canonicalId.Value())
                    return false;
                return std::ranges::is_permutation(names, expected.reflectionNames, [](const auto &a, const auto &b) {
                    return a.id == b.id && a.name == b.name &&
                           std::ranges::is_permutation(a.parameters, b.parameters, [](const auto &first, const auto &second) {
                        return first.id == second.id && first.name == second.name;
                    });
                });
            }
            return false;
        }

        /** @brief Maps the admitted shader stages to the visibility published in reflected bindings. */
        constexpr ShaderStageVisibility Visibility(ShaderStage stage) noexcept {
            if (stage == ShaderStage::Compute)
                return ShaderStageVisibility::Compute;
            return stage == ShaderStage::Vertex ? ShaderStageVisibility::Vertex : ShaderStageVisibility::Fragment;
        }

        class Normalizer final {
        public:
            Normalizer(const ShaderManifest &manifest, const ShaderTargetRequirement &target,
                       std::span<const ShaderReflectionBindingName> names, bool reconstruction)
                : manifest_(manifest), names_(names), reconstruction_(reconstruction) {
                candidate_.backend = target.backend;
                candidate_.interfaceSchemaVersion = target.interfaceSchemaVersion;
            }

            /** @brief Builds inactive declarations; only exact stage evidence activates their visibility. */
            void Initialize() {
                if (names_.size() != manifest_.bindings.size())
                    throw InvalidEvidence{};
                for (const auto &binding : manifest_.bindings)
                    InitializeBinding(binding);
                for (const auto &parameter : manifest_.parameters)
                    candidate_.parameters.push_back({parameter.id, parameter.binding, parameter.type, parameter.rows, parameter.columns,
                                                     parameter.arrayCount, 0, 0, 0, true, false});
            }

            /** @brief Validates one actual stage before retaining resource or interface evidence. */
            void Stage(const SpirvReflectionStage &stage) {
                const auto json = ParseStage(stage);
                RejectUnsupportedResources(json);
                Interfaces(json, stage.stage, "inputs", ShaderInterfaceDirection::Input);
                Interfaces(json, stage.stage, "outputs", ShaderInterfaceDirection::Output);
                std::array<bool, 128> seen{};
                Uniforms(json, stage.stage, seen);
                SampledResources(json, stage.stage, "separate_images", ShaderResourceKind::SampledTexture, seen);
                SampledResources(json, stage.stage, "separate_samplers", ShaderResourceKind::Sampler, seen);
                if (reconstruction_) {
                    StorageImages(json, stage.stage, seen);
                    if (!std::ranges::all_of(candidate_.bindings, &ShaderReflectedBinding::active))
                        throw InvalidEvidence{};
                }
            }

            ShaderReflectionCandidate candidate_;

        private:
            /** @brief Restricts native resource classes to the implemented read-only raster contract. */
            bool SupportedBinding(const ShaderResourceBinding &binding) const noexcept {
                using enum Horo::Render::ShaderResourceKind;
                if (reconstruction_ && binding.kind == StorageTexture)
                    return binding.arrayCount == 1 && binding.access == ShaderResourceAccess::ReadWrite;
                const bool kind = binding.kind == UniformBuffer || binding.kind == SampledTexture || binding.kind == Sampler;
                return kind && binding.arrayCount == 1 && binding.access == ShaderResourceAccess::ReadOnly;
            }

            /** @brief Requires unique complete member names before associating any compiler offsets. */
            static void ValidateMemberNames(const ShaderReflectionBindingName &binding) {
                for (const auto &parameter : binding.parameters)
                    if (parameter.name.empty() || parameter.name.size() > 256 ||
                        std::ranges::count(binding.parameters, parameter.name, &ShaderReflectionParameterName::name) != 1 ||
                        std::ranges::count(binding.parameters, parameter.id, &ShaderReflectionParameterName::id) != 1)
                        throw InvalidEvidence{};
            }

            /** @brief Installs inert logical declarations, never guessed native placements. */
            void InitializeBinding(const ShaderResourceBinding &binding) {
                if (!SupportedBinding(binding))
                    throw InvalidEvidence{};
                const auto name = std::ranges::find(names_, binding.id, &ShaderReflectionBindingName::id);
                if (name == names_.end() || name->name.empty() || name->name.size() > 256)
                    throw InvalidEvidence{};
                if (std::ranges::count(names_, name->name, &ShaderReflectionBindingName::name) != 1 ||
                    name->parameters.size() != std::ranges::count(manifest_.parameters, binding.id, &ShaderParameter::binding))
                    throw InvalidEvidence{};
                ValidateMemberNames(*name);
                candidate_.bindings.push_back({binding.id, binding.kind, binding.access, binding.arrayCount, binding.stages, false});
                candidate_.targetBindings.push_back({.id = binding.id, .active = false});
            }

            /** @brief Bounds JSON parsing and requires the selected actual entry definition. */
            Json ParseStage(const SpirvReflectionStage &stage) const {
                if (stage.json.empty() || stage.json.size() > ShaderReflectionCookDetail::MaximumMetadataBytes)
                    throw InvalidEvidence{};
                Foundation::JsonParseGuard guard{24};
                auto json = guard.Parse(stage.json.begin(), stage.json.end());
                if (guard.HasDuplicate() || guard.IsTooDeep() || json.is_discarded() || !json.is_object())
                    throw InvalidEvidence{};
                const auto entry = std::ranges::find(manifest_.entryPoints, stage.stage, &ShaderEntryPoint::stage);
                if (entry == manifest_.entryPoints.end() || entry->name != stage.entryPoint)
                    throw InvalidEvidence{};
                ValidateEntry(json, stage);
                return json;
            }

            /** @brief Rejects foreign or ambiguous compiler entry metadata. */
            void ValidateEntry(const Json &json, const SpirvReflectionStage &stage) const {
                const auto &entries = json.at("entryPoints");
                const auto mode = reconstruction_ ? "comp" : (stage.stage == ShaderStage::Vertex ? "vert" : "frag");
                if (!entries.is_array() || entries.size() != 1 || entries[0].at("name") != stage.entryPoint ||
                    entries[0].at("mode") != mode)
                    throw InvalidEvidence{};
                if (reconstruction_) {
                    constexpr auto extent = NativeReconstructionDispatch::GroupExtent;
                    const auto &size = entries[0].at("workgroup_size");
                    if (!size.is_array() || size.size() != 3 || Unsigned(size[0]) != extent || Unsigned(size[1]) != extent ||
                        Unsigned(size[2]) != 1)
                        throw InvalidEvidence{};
                    for (const auto key : {"workgroup_size_is_spec_constant", "workgroup_size_is_spec_constant_id"})
                        if (entries[0].contains(key) && entries[0].at(key) != Json::array({false, false, false}))
                            throw InvalidEvidence{};
                }
            }

            /** @brief Other resource classes need their own typed native admission rather than silent omission. */
            void RejectUnsupportedResources(const Json &json) const {
                if (!reconstruction_ && json.contains("images") && !json.at("images").empty())
                    throw InvalidEvidence{};
                if (reconstruction_)
                    for (const auto key : {"inputs", "outputs", "separate_samplers"})
                        if (json.contains(key) && !json.at(key).empty())
                            throw InvalidEvidence{};
                for (const auto key : {"textures", "ssbos", "push_constants", "subpass_inputs", "counters", "acceleration_structures",
                                       "tensors", "specialization_constants"})
                    if (json.contains(key) && !json.at(key).empty())
                        throw InvalidEvidence{};
            }

            /** @brief Every named resource must occur once in its actual class and declared stage. */
            std::size_t Resource(const Json &evidence, const ShaderResourceKind kind, const ShaderStage stage,
                                 std::array<bool, 128> &seen) const {
                NoArray(evidence);
                const auto name = std::ranges::find(names_, evidence.at("name").get<std::string>(), &ShaderReflectionBindingName::name);
                if (name == names_.end()) {
                    if (kind == ShaderResourceKind::UniformBuffer)
                        throw UnknownUniformBlock{};
                    throw UnknownSampledResource{};
                }
                const auto resource = std::ranges::find(manifest_.bindings, name->id, &ShaderResourceBinding::id);
                if (resource == manifest_.bindings.end() || resource->kind != kind)
                    throw InvalidEvidence{};
                const auto index = static_cast<std::size_t>(resource - manifest_.bindings.begin());
                if (const auto visibility = Visibility(stage);
                    std::exchange(seen[index], true) || (static_cast<unsigned>(resource->stages) & static_cast<unsigned>(visibility)) == 0)
                    throw InvalidEvidence{};
                return index;
            }

            /** @brief Retains exact compiler set/binding and the union of observed stages only. */
            void Activate(const Json &evidence, const std::size_t index, const ShaderStage stage) {
                auto &resource = candidate_.bindings[index];
                auto &mapping = candidate_.targetBindings[index];
                const auto space = Unsigned(evidence.at("set"));
                const auto binding = Unsigned(evidence.at("binding"));
                if ((reconstruction_ && (space != 0 || binding != resource.id.value - 1)) ||
                    (mapping.active && (mapping.nativeSpace != space || mapping.nativeBinding != binding)))
                    throw InvalidEvidence{};
                const auto visibility = Visibility(stage);
                resource.stages = resource.active ? resource.stages | visibility : visibility;
                resource.active = true;
                mapping = {resource.id, 0, space, binding, evidence.at("name").get<std::string>(), {}, true};
            }

            /** @brief Admits only ordinary two-dimensional sampled images and independent sampler objects. */
            void SampledResources(const Json &json, const ShaderStage stage, const char *key, const ShaderResourceKind kind,
                                  std::array<bool, 128> &seen) {
                if (!json.contains(key))
                    return;
                const auto &rows = json.at(key);
                if (!rows.is_array() || rows.size() > 128)
                    throw InvalidEvidence{};
                for (const auto &row : rows) {
                    const auto index = Resource(row, kind, stage, seen);
                    if (row.at("type") != (kind == ShaderResourceKind::Sampler ? "sampler" : "texture2D"))
                        throw InvalidEvidence{};
                    Activate(row, index, stage);
                }
            }

            /** @brief Proves the two reconstruction output image formats before publishing any native map. */
            void StorageImages(const Json &json, ShaderStage stage, std::array<bool, 128> &seen) {
                constexpr auto kind = ShaderResourceKind::StorageTexture;
                const auto &rows = json.at("images");
                if (!rows.is_array() || rows.size() > 2)
                    throw InvalidEvidence{};
                for (const auto &row : rows) {
                    const auto index = Resource(row, kind, stage, seen);
                    const auto format = candidate_.bindings[index].id.value == 2 ? "rgba32f" : "r32f";
                    if (row.at("type") != "image2D" || row.at("format") != format || row.value("readonly", false))
                        throw InvalidEvidence{};
                    Activate(row, index, stage);
                }
            }

            /** @brief Shares stage duplicate detection across uniform, image and sampler declarations. */
            void Uniforms(const Json &json, const ShaderStage stage, std::array<bool, 128> &seen) {
                if (!json.contains("ubos"))
                    return;
                const auto &rows = json.at("ubos");
                if (!rows.is_array() || rows.size() > 128)
                    throw InvalidEvidence{};
                for (const auto &row : rows)
                    Uniform(json, row, stage, seen);
            }

            /** @brief Collects actual user stage interfaces; builtins are omitted by the official reflection producer. */
            void Interfaces(const Json &json, ShaderStage stage, const char *key, ShaderInterfaceDirection direction) {
                if (!json.contains(key))
                    return;
                const auto &values = json.at(key);
                if (!values.is_array() || values.size() > 256 || candidate_.stageInterface.size() + values.size() > 256)
                    throw InvalidEvidence{};
                for (const auto &value : values) {
                    NoArray(value);
                    const auto shape = ValueShape(value.at("type").get<std::string>());
                    candidate_.stageInterface.push_back({stage, direction, Unsigned(value.at("location")), shape.type, shape.rows,
                                                         shape.columns, 1, value.at("name").get<std::string>()});
                }
            }

            /** @brief Resolves a native block by authored identity while taking binding/offsets exclusively from reflection. */
            void Uniform(const Json &json, const Json &ubo, ShaderStage stage, std::array<bool, 128> &seen) {
                const auto index = Resource(ubo, ShaderResourceKind::UniformBuffer, stage, seen);
                const auto name = std::ranges::find(names_, candidate_.bindings[index].id, &ShaderReflectionBindingName::id);
                Activate(ubo, index, stage);
                const auto &members = json.at("types").at(ubo.at("type").get<std::string>()).at("members");
                if (!members.is_array() || members.size() != name->parameters.size() || members.size() > 512)
                    throw InvalidEvidence{};
                const auto blockBytes = Unsigned(ubo.at("block_size"));
                if (blockBytes == 0 || blockBytes > 16U * 1024U * 1024U ||
                    (reconstruction_ && blockBytes != sizeof(NativeReconstructionDispatch)))
                    throw InvalidEvidence{};
                std::array<bool, 512> memberSeen{};
                for (const auto &member : members) {
                    const auto declared =
                        std::ranges::find(name->parameters, member.at("name").get<std::string>(), &ShaderReflectionParameterName::name);
                    if (declared == name->parameters.end() ||
                        std::exchange(memberSeen[static_cast<std::size_t>(declared - name->parameters.begin())], true))
                        throw InvalidEvidence{};
                    Parameter(*name, member, blockBytes);
                }
            }

            /** @brief Requires complete named parameter evidence and exact repeated-stage layout equality. */
            void Parameter(const ShaderReflectionBindingName &binding, const Json &member, std::uint32_t blockBytes) {
                const auto [arrayCount, arrayStride] = MemberArray(member);
                const auto name =
                    std::ranges::find(binding.parameters, member.at("name").get<std::string>(), &ShaderReflectionParameterName::name);
                if (name == binding.parameters.end())
                    throw InvalidEvidence{};
                const auto declared = std::ranges::find(candidate_.parameters, name->id, &ShaderReflectedParameter::id);
                if (declared == candidate_.parameters.end() || declared->binding != binding.id)
                    throw InvalidEvidence{};
                const auto type = member.at("type").get<std::string>();
                const auto shape = reconstruction_ && type == "uvec4" ? Shape{ShaderValueType::Uint32, 1, 4} : ValueShape(type);
                const auto stride = member.contains("matrix_stride") ? Unsigned(member.at("matrix_stride")) : 0;
                const auto offset = Unsigned(member.at("offset"));
                const bool columnMajor = !member.value("row_major", false);
                const std::uint64_t size =
                    stride ? stride * std::uint64_t{columnMajor ? shape.columns : shape.rows} : 4U * shape.rows * shape.columns;
                const auto totalSize = std::uint64_t{arrayCount - 1} * arrayStride + size;
                if (size == 0 || (arrayCount > 1 && arrayStride < size) || offset > blockBytes || totalSize > blockBytes - offset ||
                    arrayCount != declared->arrayCount ||
                    (reconstruction_ && (offset != (declared->id.value - 1) * 16 || stride != 0 || arrayStride != 0)))
                    throw InvalidEvidence{};
                const ShaderReflectedParameter value{name->id, binding.id,  shape.type, shape.rows,  shape.columns, arrayCount,
                                                     offset,   arrayStride, stride,     columnMajor, true};
                if (declared->active && !SamePacking(*declared, value))
                    throw InvalidEvidence{};
                *declared = value;
            }

            const ShaderManifest &manifest_;
            std::span<const ShaderReflectionBindingName> names_;
            const bool reconstruction_;
        };
    }  // namespace

    /** @copydoc NormalizeVulkanReflection */
    Result<NormalizedShaderReflection> NormalizeVulkanReflection(const ShaderManifest &manifest, const ShaderTargetRequirement &target,
                                                                 const std::span<const SpirvReflectionStage> stages,
                                                                 const std::span<const ShaderReflectionBindingName> names) {
        try {
            const bool reconstruction = AdmittedReconstructionEnvelope(manifest, target, stages, names);
            if (!reconstruction && !AdmittedVulkanEnvelope(manifest, target, stages, names))
                throw InvalidEvidence{};
            Normalizer normalizer(manifest, target, names, reconstruction);
            normalizer.Initialize();
            std::array<bool, 2> seen{};
            for (const auto &stage : stages) {
                if (!reconstruction &&
                    (stage.stage > ShaderStage::Fragment || std::exchange(seen[static_cast<std::size_t>(stage.stage)], true)))
                    throw InvalidEvidence{};
                normalizer.Stage(stage);
            }
            auto &candidate = normalizer.candidate_;
            std::ranges::sort(candidate.stageInterface, [](const auto &a, const auto &b) {
                return std::tuple{a.stage, a.direction, a.location} < std::tuple{b.stage, b.direction, b.location};
            });
            return NormalizeShaderReflection(manifest, target, candidate);
        } catch (const Json::exception &) {
            return Result<NormalizedShaderReflection>::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
        } catch (const UnknownUniformBlock &) {
            return Result<NormalizedShaderReflection>::Failure(
                MakeError(ShaderReflectionErrors::InvalidReflection, "Final uniform block identity is not admitted by the target map."));
        } catch (const UnknownSampledResource &) {
            return Result<NormalizedShaderReflection>::Failure(
                MakeError(ShaderReflectionErrors::InvalidReflection, "Final sampled resource identity is not admitted by the target map."));
        } catch (const InvalidEvidence &) {
            return Result<NormalizedShaderReflection>::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
        } catch (const std::bad_alloc &) {
            return Result<NormalizedShaderReflection>::Failure(MakeError(ShaderReflectionErrors::AllocationFailed));
        }
    }
}  // namespace Horo::Render
