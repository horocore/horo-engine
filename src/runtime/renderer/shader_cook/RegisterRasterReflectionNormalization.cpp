#include "Horo/Runtime/Render/ShaderReflectionErrors.h"
#include "RegisterRasterReflectionInternal.h"

#include <algorithm>
#include <tuple>

namespace Horo::Render {
    namespace {
        using namespace RegisterRasterCookDetail;
        using Foundation::Json;

        std::uint32_t Unsigned(const Json &value) {
            if (!value.is_number_unsigned() || value.get<std::uint64_t>() > 0xffffffffULL)
                throw InvalidEvidence{};
            return value.get<std::uint32_t>();
        }

        const Json &Rows(const Json &json, const char *key, std::size_t maximum) {
            static const Json empty = Json::array();
            const auto &rows = json.contains(key) ? json.at(key) : empty;
            if (!rows.is_array() || rows.size() > maximum)
                throw InvalidEvidence{};
            return rows;
        }

        Json ParseJson(std::string_view source) {
            if (source.empty() || source.size() > ShaderReflectionCookDetail::MaximumMetadataBytes)
                throw InvalidEvidence{};
            Foundation::JsonParseGuard guard{24};
            auto json = guard.Parse(source.begin(), source.end());
            if (guard.HasDuplicate() || guard.IsTooDeep() || json.is_discarded() || !json.is_object())
                throw InvalidEvidence{};
            return json;
        }

        Json ParseStage(const SpirvReflectionStage &stage, const ShaderEntryPoint &entry) {
            if (stage.stage != entry.stage || stage.entryPoint != entry.name)
                throw InvalidEvidence{};
            auto json = ParseJson(stage.json);
            const auto &entries = Rows(json, "entryPoints", 1);
            if (entries.size() != 1 || entries[0].at("name") != entry.name ||
                entries[0].at("mode") != (entry.stage == ShaderStage::Vertex ? "vert" : "frag"))
                throw InvalidEvidence{};
            return json;
        }

        void RejectUnsupported(const Json &json) {
            for (const auto key : {"textures", "images", "push_constants", "subpass_inputs", "counters", "acceleration_structures",
                                   "tensors", "specialization_constants"})
                if (!Rows(json, key, 0).empty())
                    throw InvalidEvidence{};
        }

        void Array(const Json &member, const NativeMember &native) {
            if (native.arrayStride == 0) {
                if (member.contains("array") || member.contains("array_stride") || member.contains("array_size_is_literal"))
                    throw InvalidEvidence{};
                return;
            }
            const auto &array = Rows(member, "array", 1), &literal = Rows(member, "array_size_is_literal", 1);
            if (array.size() != 1 || literal.size() != 1 || literal[0] != true || Unsigned(array[0]) != native.arrayCount ||
                Unsigned(member.at("array_stride")) != native.arrayStride)
                throw InvalidEvidence{};
        }

        void Matrix(const Json &member, const NativeMember &native) {
            if (native.rows == 4) {
                if (Unsigned(member.at("matrix_stride")) != native.matrixStride || member.value("row_major", false))
                    throw InvalidEvidence{};
            } else if (member.contains("matrix_stride") || member.contains("row_major")) {
                throw InvalidEvidence{};
            }
        }

        ShaderReflectedParameter Parameter(const Json &member, const NativeMember &native, const ShaderParameter &declared,
                                           const std::string &name) {
            if (member.at("name") != name || native.name != name || member.at("type") != (native.rows == 4 ? "mat4" : "vec4") ||
                member.contains("physical_pointer"))
                throw InvalidEvidence{};
            if (native.rows != declared.rows || native.columns != declared.columns || native.arrayCount != declared.arrayCount ||
                Unsigned(member.at("offset")) != native.byteOffset)
                throw InvalidEvidence{};
            Array(member, native);
            Matrix(member, native);
            return {declared.id,
                    declared.binding,
                    declared.type,
                    declared.rows,
                    declared.columns,
                    declared.arrayCount,
                    native.byteOffset,
                    native.arrayStride,
                    native.matrixStride,
                    true,
                    true};
        }

        bool SamePacking(const ShaderReflectedParameter &first, const ShaderReflectedParameter &second) noexcept {
            return first.byteOffset == second.byteOffset && first.arrayStride == second.arrayStride &&
                   first.matrixStride == second.matrixStride && first.columnMajor == second.columnMajor;
        }

        bool SameInterface(const ShaderReflectedInterfaceVariable &native, ShaderStage stage, ShaderInterfaceDirection direction,
                           std::uint32_t location, std::uint8_t columns) noexcept {
            return native.stage == stage && native.direction == direction && native.location == location &&
                   native.type == ShaderValueType::Float32 && native.rows == 1 && native.columns == columns && native.arrayCount == 1;
        }

        std::uint8_t InterfaceColumns(const Json &value) {
            const auto type = value.at("type").get<std::string>();
            if (value.contains("array"))
                throw InvalidEvidence{};
            if (type == "float")
                return 1;
            if (type.size() != 4 || !type.starts_with("vec") || type[3] < '2' || type[3] > '4')
                throw InvalidEvidence{};
            return static_cast<std::uint8_t>(type[3] - '0');
        }

        void Interfaces(const Json &json, ShaderStage stage, const NativeStage &native, ShaderReflectionCandidate &candidate) {
            std::size_t count{};
            for (const auto direction : {ShaderInterfaceDirection::Input, ShaderInterfaceDirection::Output}) {
                for (const auto &value : Rows(json, direction == ShaderInterfaceDirection::Input ? "inputs" : "outputs", 64)) {
                    const auto columns = InterfaceColumns(value);
                    const auto location = Unsigned(value.at("location"));
                    const auto matches = [&](const auto &entry) {
                        return SameInterface(entry, stage, direction, location, columns);
                    };
                    if (std::ranges::count_if(native.interfaces, matches) != 1)
                        throw InvalidEvidence{};
                    candidate.stageInterface.push_back(*std::ranges::find_if(native.interfaces, matches));
                    ++count;
                }
            }
            if (count != native.interfaces.size())
                throw InvalidEvidence{};
        }

        void OpenGLLinks(const ShaderReflectionCandidate &candidate) {
            if (candidate.backend != ShaderTargetBackend::OpenGL)
                return;
            for (const auto &input : candidate.stageInterface) {
                if (input.stage != ShaderStage::Fragment || input.direction != ShaderInterfaceDirection::Input)
                    continue;
                const auto producer = [&](const auto &output) {
                    return SameInterface(output, ShaderStage::Vertex, ShaderInterfaceDirection::Output, input.location, input.columns) &&
                           output.nativeName == input.nativeName;
                };
                if (std::ranges::count_if(candidate.stageInterface, producer) != 1)
                    throw InvalidEvidence{};
            }
        }

        class Normalizer final {
        public:
            Normalizer(const ShaderManifest &manifest, const ShaderTargetRequirement &target,
                       std::span<const ShaderReflectionBindingName> names)
                : manifest_(manifest), names_(names) {
                candidate_.backend = target.backend;
                candidate_.interfaceSchemaVersion = target.interfaceSchemaVersion;
                for (const auto &binding : manifest.bindings) {
                    candidate_.bindings.push_back({binding.id, binding.kind, binding.access, binding.arrayCount, binding.stages, false});
                    candidate_.targetBindings.push_back({.id = binding.id, .active = false});
                }
                for (const auto &parameter : manifest.parameters)
                    candidate_.parameters.push_back({parameter.id, parameter.binding, parameter.type, parameter.rows, parameter.columns,
                                                     parameter.arrayCount, 0, 0, 0, true, false});
            }

            void Stage(const SpirvReflectionStage &stage, const NativeStage &native, const ShaderEntryPoint &entry) {
                const auto json = ParseStage(stage, entry);
                RejectUnsupported(json);
                const auto visibility =
                    stage.stage == ShaderStage::Vertex ? ShaderStageVisibility::Vertex : ShaderStageVisibility::Fragment;
                Uniform(json, native, visibility);
                Sample(json, native.texture, ShaderResourceKind::SampledTexture, visibility);
                Sample(json, native.sampler, ShaderResourceKind::Sampler, visibility);
                NormalizeRegisterStorage(json, native, manifest_, names_, visibility, candidate_);
                Interfaces(json, stage.stage, native, candidate_);
            }

            Result<NormalizedShaderReflection> Finish(const ShaderTargetRequirement &target) {
                OpenGLLinks(candidate_);
                std::ranges::sort(candidate_.stageInterface, {}, [](const auto &entry) {
                    return std::tuple{entry.stage, entry.direction, entry.location};
                });
                return NormalizeShaderReflection(manifest_, target, candidate_);
            }

        private:
            std::size_t Role(ShaderResourceKind kind) const {
                const auto found = std::ranges::find(manifest_.bindings, kind, &ShaderResourceBinding::kind);
                if (found == manifest_.bindings.end())
                    throw InvalidEvidence{};
                return static_cast<std::size_t>(found - manifest_.bindings.begin());
            }

            void Activate(std::size_t role, const NativeResource &native, ShaderStageVisibility visibility) {
                auto &binding = candidate_.bindings[role];
                auto &mapping = candidate_.targetBindings[role];
                if (mapping.active && (mapping.nativeName != native.name || mapping.nativeBinding != native.binding))
                    throw InvalidEvidence{};
                const auto previous = binding.active ? binding.stages : ShaderStageVisibility::None;
                binding.active = true;
                binding.stages = static_cast<ShaderStageVisibility>(static_cast<unsigned>(previous) | static_cast<unsigned>(visibility));
                mapping = {.id = binding.id, .nativeBinding = native.binding, .nativeName = native.name};
                if (binding.kind == ShaderResourceKind::SampledTexture)
                    mapping.pairedSampler = manifest_.bindings[Role(ShaderResourceKind::Sampler)].id;
            }

            void Members(const Json &members, const NativeUniform &native, const ShaderReflectionBindingName &names) {
                if (!members.is_array() || members.size() != native.members.size() || members.size() != names.parameters.size())
                    throw InvalidEvidence{};
                for (std::size_t index = 0; index < members.size(); ++index) {
                    const auto name = members[index].at("name").get<std::string>();
                    const auto authored = std::ranges::find(names.parameters, name, &ShaderReflectionParameterName::name);
                    if (authored == names.parameters.end())
                        throw InvalidEvidence{};
                    const auto declared = std::ranges::find(manifest_.parameters, authored->id, &ShaderParameter::id);
                    const auto parameter = Parameter(members[index], native.members[index], *declared, name);
                    auto &retained = *std::ranges::find(candidate_.parameters, authored->id, &ShaderReflectedParameter::id);
                    if (retained.active && !SamePacking(retained, parameter))
                        throw InvalidEvidence{};
                    retained = parameter;
                }
            }

            void Uniform(const Json &json, const NativeStage &native, ShaderStageVisibility visibility) {
                const auto &blocks = Rows(json, "ubos", 1);
                if (blocks.empty() != !native.uniform)
                    throw InvalidEvidence{};
                if (blocks.empty())
                    return;
                const auto role = Role(ShaderResourceKind::UniformBuffer);
                const auto &name = *FindName(names_, manifest_.bindings[role].id);
                const auto &block = blocks[0];
                if (block.at("name") != name.name || Unsigned(block.at("set")) != 0 || block.contains("array") ||
                    Unsigned(block.at("block_size")) != native.uniform->byteSize)
                    throw InvalidEvidence{};
                static_cast<void>(Unsigned(block.at("binding")));
                const auto &type = json.at("types").at(block.at("type").get<std::string>());
                Members(type.at("members"), *native.uniform, name);
                Activate(role, {native.uniform->name, native.uniform->binding}, visibility);
            }

            void Sample(const Json &json, const std::optional<NativeResource> &native, ShaderResourceKind kind,
                        ShaderStageVisibility visibility) {
                const auto &entries = Rows(json, kind == ShaderResourceKind::Sampler ? "separate_samplers" : "separate_images", 1);
                if (entries.empty() != !native)
                    throw InvalidEvidence{};
                if (entries.empty())
                    return;
                const auto role = Role(kind);
                const auto &entry = entries[0];
                const auto &name = *FindName(names_, manifest_.bindings[role].id);
                if (entry.at("name") != name.name || entry.at("type") != (kind == ShaderResourceKind::Sampler ? "sampler" : "texture2D") ||
                    Unsigned(entry.at("set")) != 0 || entry.contains("array"))
                    throw InvalidEvidence{};
                static_cast<void>(Unsigned(entry.at("binding")));
                Activate(role, *native, visibility);
            }

            const ShaderManifest &manifest_;
            std::span<const ShaderReflectionBindingName> names_;
            ShaderReflectionCandidate candidate_;
        };
    }  // namespace

    Result<NormalizedShaderReflection> NormalizeRegisterRasterReflection(const ShaderManifest &manifest,
                                                                         const ShaderTargetRequirement &target,
                                                                         std::span<const SpirvReflectionStage> stages,
                                                                         std::span<const std::string> translations,
                                                                         std::span<const ShaderReflectionBindingName> names) {
        using Outcome = Result<NormalizedShaderReflection>;
        try {
            if (ValidateRegisterRasterReflection(manifest, target, names).HasError() || stages.size() != 2 || translations.size() != 2)
                throw InvalidEvidence{};
            Normalizer normalizer{manifest, target, names};
            for (std::size_t index = 0; index < stages.size(); ++index) {
                const auto &entry = manifest.entryPoints[index];
                const auto native = ParseRegisterNativeStage(translations[index], entry.stage, target.backend, entry.name);
                normalizer.Stage(stages[index], native, entry);
            }
            return normalizer.Finish(target);
        } catch (const Json::exception &) {
            return Outcome::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
        } catch (const InvalidEvidence &) {
            return Outcome::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(ShaderReflectionErrors::AllocationFailed));
        }
    }

    Result<std::vector<std::uint32_t>> RegisterRasterVaryingLocations(const SpirvReflectionStage &stage) {
        using Outcome = Result<std::vector<std::uint32_t>>;
        try {
            if (stage.stage != ShaderStage::Vertex && stage.stage != ShaderStage::Fragment)
                throw InvalidEvidence{};
            const auto json = ParseStage(stage, {stage.stage, stage.entryPoint});
            std::vector<std::uint32_t> locations;
            for (const auto &value : Rows(json, stage.stage == ShaderStage::Vertex ? "outputs" : "inputs", 256)) {
                static_cast<void>(InterfaceColumns(value));
                const auto location = Unsigned(value.at("location"));
                if (location > 255)
                    throw InvalidEvidence{};
                locations.push_back(location);
            }
            std::ranges::sort(locations);
            if (std::adjacent_find(locations.begin(), locations.end()) != locations.end())
                throw InvalidEvidence{};
            return Outcome::Success(std::move(locations));
        } catch (const Json::exception &) {
            return Outcome::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
        } catch (const InvalidEvidence &) {
            return Outcome::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(ShaderReflectionErrors::AllocationFailed));
        }
    }
}  // namespace Horo::Render
