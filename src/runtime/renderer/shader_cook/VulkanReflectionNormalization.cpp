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

        /** @brief Admits the explicit float scalar/vector/matrix viewport ABI; unsupported types fail closed. */
        Shape ValueShape(const std::string &type) {
            if (type == "float")
                return {ShaderValueType::Float32, 1, 1};
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

        /** @brief Preserves exact final member packing on every stage sharing a uniform block. */
        bool SamePacking(const ShaderReflectedParameter &a, const ShaderReflectedParameter &b) {
            return a.id == b.id && a.binding == b.binding && a.type == b.type && a.rows == b.rows && a.columns == b.columns &&
                   a.arrayCount == b.arrayCount && a.byteOffset == b.byteOffset && a.arrayStride == b.arrayStride &&
                   a.matrixStride == b.matrixStride && a.columnMajor == b.columnMajor;
        }

        class Normalizer final {
        public:
            Normalizer(const ShaderManifest &manifest, const ShaderTargetRequirement &target,
                       std::span<const ShaderReflectionBindingName> names)
                : manifest_(manifest), names_(names) {
                candidate_.backend = target.backend;
                candidate_.interfaceSchemaVersion = target.interfaceSchemaVersion;
            }

            /** @brief Builds inactive declarations first; only actual final evidence can activate a resource. */
            void Initialize() {
                if (names_.size() != manifest_.bindings.size())
                    throw InvalidEvidence{};
                for (const auto &binding : manifest_.bindings) {
                    if (binding.kind != ShaderResourceKind::UniformBuffer || binding.arrayCount != 1 ||
                        binding.access != ShaderResourceAccess::ReadOnly)
                        throw InvalidEvidence{};
                    const auto name = std::ranges::find(names_, binding.id, &ShaderReflectionBindingName::id);
                    if (name == names_.end() || name->name.empty() || name->name.size() > 256)
                        throw InvalidEvidence{};
                    if (std::ranges::count(names_, name->name, &ShaderReflectionBindingName::name) != 1 ||
                        name->parameters.size() != std::ranges::count(manifest_.parameters, binding.id, &ShaderParameter::binding))
                        throw InvalidEvidence{};
                    for (const auto &parameter : name->parameters)
                        if (parameter.name.empty() || parameter.name.size() > 256 ||
                            std::ranges::count(name->parameters, parameter.name, &ShaderReflectionParameterName::name) != 1 ||
                            std::ranges::count(name->parameters, parameter.id, &ShaderReflectionParameterName::id) != 1)
                            throw InvalidEvidence{};
                    candidate_.bindings.push_back({binding.id, binding.kind, binding.access, binding.arrayCount, binding.stages, false});
                    candidate_.targetBindings.push_back({.id = binding.id, .active = false});
                }
                for (const auto &parameter : manifest_.parameters)
                    candidate_.parameters.push_back({parameter.id, parameter.binding, parameter.type, parameter.rows, parameter.columns,
                                                     parameter.arrayCount, 0, 0, 0, true, false});
            }

            /** @brief Validates one official final-stage JSON document before retaining any native ABI evidence. */
            void Stage(const SpirvReflectionStage &stage) {
                if (stage.json.empty() || stage.json.size() > ShaderReflectionCookDetail::MaximumMetadataBytes)
                    throw InvalidEvidence{};
                Foundation::JsonParseGuard guard{24};
                const auto json = guard.Parse(stage.json.begin(), stage.json.end());
                if (guard.HasDuplicate() || guard.IsTooDeep() || json.is_discarded() || !json.is_object())
                    throw InvalidEvidence{};
                const auto entry = std::ranges::find(manifest_.entryPoints, stage.stage, &ShaderEntryPoint::stage);
                if (entry == manifest_.entryPoints.end() || entry->name != stage.entryPoint)
                    throw InvalidEvidence{};
                const auto mode = stage.stage == ShaderStage::Vertex ? "vert" : "frag";
                const auto &entries = json.at("entryPoints");
                if (!entries.is_array() || entries.size() != 1 || entries[0].at("name") != stage.entryPoint ||
                    entries[0].at("mode") != mode)
                    throw InvalidEvidence{};
                for (const auto key : {"textures", "separate_images", "separate_samplers", "images", "ssbos", "push_constants",
                                       "subpass_inputs", "counters", "acceleration_structures", "tensors", "specialization_constants"})
                    if (json.contains(key) && !json.at(key).empty())
                        throw InvalidEvidence{};
                Interfaces(json, stage.stage, "inputs", ShaderInterfaceDirection::Input);
                Interfaces(json, stage.stage, "outputs", ShaderInterfaceDirection::Output);
                if (json.contains("ubos")) {
                    if (!json.at("ubos").is_array() || json.at("ubos").size() > 128)
                        throw InvalidEvidence{};
                    std::array<bool, 128> seen{};
                    for (const auto &ubo : json.at("ubos"))
                        Uniform(json, ubo, stage.stage, seen);
                }
            }

            ShaderReflectionCandidate candidate_;

        private:
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
                NoArray(ubo);
                const auto name = std::ranges::find(names_, ubo.at("name").get<std::string>(), &ShaderReflectionBindingName::name);
                if (name == names_.end())
                    throw InvalidEvidence{};
                const auto resource = std::ranges::find(candidate_.bindings, name->id, &ShaderReflectedBinding::id);
                if (resource == candidate_.bindings.end() ||
                    std::exchange(seen[static_cast<std::size_t>(resource - candidate_.bindings.begin())], true))
                    throw InvalidEvidence{};
                const auto visibility = stage == ShaderStage::Vertex ? ShaderStageVisibility::Vertex : ShaderStageVisibility::Fragment;
                if ((static_cast<unsigned>(resource->stages) & static_cast<unsigned>(visibility)) == 0)
                    throw InvalidEvidence{};
                auto &mapping = *std::ranges::find(candidate_.targetBindings, name->id, &ShaderTargetBindingMapEntry::id);
                const auto space = Unsigned(ubo.at("set")), binding = Unsigned(ubo.at("binding"));
                if (mapping.active && (mapping.nativeSpace != space || mapping.nativeBinding != binding))
                    throw InvalidEvidence{};
                mapping = {name->id, 0, space, binding, name->name, {}, true};
                resource->active = true;
                const auto &members = json.at("types").at(ubo.at("type").get<std::string>()).at("members");
                if (!members.is_array() || members.size() != name->parameters.size() || members.size() > 512)
                    throw InvalidEvidence{};
                const auto blockBytes = Unsigned(ubo.at("block_size"));
                if (blockBytes == 0 || blockBytes > 16U * 1024U * 1024U)
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
                NoArray(member);
                const auto name =
                    std::ranges::find(binding.parameters, member.at("name").get<std::string>(), &ShaderReflectionParameterName::name);
                if (name == binding.parameters.end())
                    throw InvalidEvidence{};
                const auto declared = std::ranges::find(candidate_.parameters, name->id, &ShaderReflectedParameter::id);
                if (declared == candidate_.parameters.end() || declared->binding != binding.id)
                    throw InvalidEvidence{};
                const auto shape = ValueShape(member.at("type").get<std::string>());
                const auto stride = member.contains("matrix_stride") ? Unsigned(member.at("matrix_stride")) : 0;
                const auto offset = Unsigned(member.at("offset"));
                const bool columnMajor = !member.value("row_major", false);
                const std::uint64_t size =
                    stride ? stride * std::uint64_t{columnMajor ? shape.columns : shape.rows} : 4U * shape.rows * shape.columns;
                if (size == 0 || offset > blockBytes || size > blockBytes - offset)
                    throw InvalidEvidence{};
                const ShaderReflectedParameter value{name->id, binding.id, shape.type, shape.rows,  shape.columns, 1,
                                                     offset,   0,          stride,     columnMajor, true};
                if (declared->active && !SamePacking(*declared, value))
                    throw InvalidEvidence{};
                *declared = value;
            }

            const ShaderManifest &manifest_;
            std::span<const ShaderReflectionBindingName> names_;
        };
    }  // namespace

    /** @copydoc NormalizeVulkanReflection */
    Result<NormalizedShaderReflection> NormalizeVulkanReflection(const ShaderManifest &manifest, const ShaderTargetRequirement &target,
                                                                 const std::span<const SpirvReflectionStage> stages,
                                                                 const std::span<const ShaderReflectionBindingName> names) {
        if (target.backend != ShaderTargetBackend::Vulkan || target.payloadFormat != ShaderPayloadFormat::SpirV16 ||
            stages.size() != manifest.entryPoints.size() || stages.size() != 2 || names.size() > 128 || manifest.parameters.size() > 512 ||
            ValidateShaderManifest(manifest).HasError())
            return Result<NormalizedShaderReflection>::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
        try {
            Normalizer normalizer(manifest, target, names);
            normalizer.Initialize();
            std::array<bool, 2> seen{};
            for (const auto &stage : stages) {
                if (stage.stage > ShaderStage::Fragment || std::exchange(seen[static_cast<std::size_t>(stage.stage)], true))
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
        } catch (const InvalidEvidence &) {
            return Result<NormalizedShaderReflection>::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
        } catch (const std::bad_alloc &) {
            return Result<NormalizedShaderReflection>::Failure(MakeError(ShaderReflectionErrors::AllocationFailed));
        }
    }
}  // namespace Horo::Render
