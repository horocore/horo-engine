#include "Horo/Runtime/Render/ShaderReflectionErrors.h"
#include "Horo/Runtime/Render/VfxShaderParameters.h"
#include "MetalReflectionEvidence.h"
#include "ShaderReflectionCookJson.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <tuple>

namespace Horo::Render {
    namespace {
        using Foundation::Json;
        using InvalidVfxEvidence = MetalReflectionCookDetail::InvalidEvidence;
        using MetalReflectionCookDetail::AttributeIndex;
        using MetalReflectionCookDetail::Tokens;

        struct NativeStageEvidence {
            std::array<std::uint32_t, 4> bindings{0xffffffffU, 0xffffffffU, 0xffffffffU, 0xffffffffU};
            std::array<std::string, 4> names;
            std::vector<ShaderReflectedInterfaceVariable> interfaces;
            std::string instanceObject;
        };

        /** @brief Admits bounded generated ASCII identifiers for actual native lookup. */
        bool Identifier(const std::string_view value) {
            const auto letter = [](const char ch) {
                return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
            };
            return !value.empty() && value.size() <= 256 && letter(value.front()) && std::ranges::all_of(value, [&](const char ch) {
                return letter(ch) || (ch >= '0' && ch <= '9');
            });
        }

        /** @brief Requires one whole token sequence; comments, literals and repeated proof sequences cannot supply evidence. */
        void UniqueTokens(const std::vector<std::string_view> &tokens, const std::string_view expected) {
            const auto pattern = Tokens(expected);
            std::size_t matches{};
            for (auto at = tokens.begin(); at != tokens.end();) {
                const auto found = std::search(at, tokens.end(), pattern.begin(), pattern.end());
                if (found == tokens.end())
                    break;
                ++matches;
                at = found + 1;
            }
            if (matches != 1)
                throw InvalidVfxEvidence{};
        }

        /** @brief Rejects overloaded or duplicate lowering definitions even if one contains the expected body. */
        void UniqueFunctionDefinition(const std::vector<std::string_view> &tokens, const std::string_view name) {
            std::size_t definitions{};
            for (std::size_t at = 0; at + 1 < tokens.size(); ++at) {
                if (tokens[at] != name || tokens[at + 1] != "(")
                    continue;
                std::size_t depth = 1, end = at + 2;
                for (; end < tokens.size() && depth != 0; ++end) {
                    if (tokens[end] == "(")
                        ++depth;
                    if (tokens[end] == ")")
                        --depth;
                }
                if (depth != 0)
                    throw InvalidVfxEvidence{};
                if (end < tokens.size() && tokens[end] == "{")
                    ++definitions;
            }
            if (definitions != 1)
                throw InvalidVfxEvidence{};
        }

        /** @brief Proves the generated helper's complete column-matrix/vector accumulation and its actual position call. */
        void MetalMatrixLowering(const std::vector<std::string_view> &tokens) {
            UniqueFunctionDefinition(tokens, "spvFMulMatrixVector");
            UniqueTokens(tokens, R"(
                template<typename T, int Cols, int Rows>
                [[clang::optnone]] vec<T, Rows> spvFMulMatrixVector(matrix<T, Cols, Rows> m, vec<T, Cols> v) {
                    vec<T, Rows> res = vec<T, Rows>(0);
                    for (uint i = Cols; i > 0; --i) { res = fma(m[i - 1], vec<T, Rows>(v[i - 1]), res); }
                    return res;
                })");
            UniqueTokens(tokens, R"(out.gl_Position = spvFMulMatrixVector(
                VfxInstances.ClipFromLocal[gl_InstanceIndex], float4(in.in_var_POSITION, 1.0));)");
        }

        /** @brief Reads finite literal placement evidence without signed or narrowing conversions. */
        std::uint32_t Unsigned(const Json &value) {
            if (!value.is_number_unsigned() || value.get<std::uint64_t>() > 0xffffffffULL)
                throw InvalidVfxEvidence{};
            return value.get<std::uint32_t>();
        }

        /** @brief Requires the complete native struct, including literal array extents and member order. */
        void NativeStruct(const std::vector<std::string_view> &tokens, const std::string_view type, const bool instances) {
            const std::array<std::string_view, 14>
                instanceBody{"float4x4", "ClipFromLocal", "[", "128", "]", ";", "float4", "InstanceColor", "[", "128", "]", ";", "}", ";"};
            const std::array<std::string_view, 8> materialBody{"float4", "Tint", ";", "float", "AlphaCutoff", ";", "}", ";"};
            const auto expected =
                instances ? std::span<const std::string_view>{instanceBody} : std::span<const std::string_view>{materialBody};
            std::size_t matches{};
            for (std::size_t at = 0; at + 2 < tokens.size(); ++at) {
                if (tokens[at] != "struct" || tokens[at + 1] != type || tokens[at + 2] != "{")
                    continue;
                ++matches;
                if (tokens.size() - at - 3 < expected.size() ||
                    !std::ranges::equal(std::span{tokens}.subspan(at + 3, expected.size()), expected))
                    throw InvalidVfxEvidence{};
            }
            if (matches != 1)
                throw InvalidVfxEvidence{};
        }

        /** @brief Takes each native slot from its actual argument attribute and proves uniform native packing. */
        std::array<std::uint32_t, 4> NativePlacements(const std::string &source, const bool vertex) {
            constexpr auto Missing = 0xffffffffU;
            std::array<std::uint32_t, 4> result{Missing, Missing, Missing, Missing};
            const auto tokens = Tokens(source);
            for (std::size_t at = 1; at + 7 < tokens.size(); ++at) {
                if (tokens[at] != "[" || tokens[at + 1] != "[")
                    continue;
                const auto kind = tokens[at + 2];
                if (kind != "buffer" && kind != "texture" && kind != "sampler")
                    continue;
                const auto index = AttributeIndex(tokens, at);
                std::size_t role = 4;
                if (kind == "buffer") {
                    if (at < 4 || tokens[at - 2] != "&" || tokens[at - 4] != "constant")
                        throw InvalidVfxEvidence{};
                    const auto type = tokens[at - 3];
                    if (type == "VfxInstances" || type == "type_VfxInstances")
                        role = 0;
                    if (type == "VfxMaterial" || type == "type_VfxMaterial")
                        role = 1;
                    if (role < 2)
                        NativeStruct(tokens, type, role == 0);
                } else if (kind == "texture" && tokens[at - 1] == "BaseColor") {
                    if (at < 5 || tokens[at - 5] != "texture2d" || tokens[at - 4] != "<" || tokens[at - 3] != "float" ||
                        tokens[at - 2] != ">")
                        throw InvalidVfxEvidence{};
                    role = 2;
                } else if (kind == "sampler" && tokens[at - 1] == "BaseColorSampler") {
                    if (at < 2 || tokens[at - 2] != "sampler")
                        throw InvalidVfxEvidence{};
                    role = 3;
                }
                if (role == 4 || result[role] != Missing)
                    throw InvalidVfxEvidence{};
                result[role] = index;
            }
            if (result[0] != Missing && result[0] == result[1])
                throw InvalidVfxEvidence{};
            if (vertex)
                MetalMatrixLowering(tokens);
            return result;
        }

        /** @brief Proves one complete std140 block from generated GL4.1 source, retaining its actual link name. */
        std::size_t OpenGLBlock(const std::vector<std::string_view> &tokens, const std::size_t at, NativeStageEvidence &result) {
            if (at < 4 || tokens[at - 4] != "layout" || tokens[at - 3] != "(" || tokens[at - 2] != "std140" || tokens[at - 1] != ")" ||
                at + 3 >= tokens.size() || tokens[at + 2] != "{")
                throw InvalidVfxEvidence{};
            constexpr std::array<std::string_view, 12> instances{"mat4", "ClipFromLocal", "[", "128", "]", ";",
                                                                 "vec4", "InstanceColor", "[", "128", "]", ";"};
            constexpr std::array<std::string_view, 6> material{"vec4", "Tint", ";", "float", "AlphaCutoff", ";"};
            const auto role = tokens[at + 3] == "mat4" ? std::size_t{0} : std::size_t{1};
            const auto fields = role == 0 ? std::span<const std::string_view>{instances} : std::span<const std::string_view>{material};
            const auto begin = at + 3;
            if (tokens.size() - begin <= fields.size() || !std::ranges::equal(std::span{tokens}.subspan(begin, fields.size()), fields) ||
                tokens[begin + fields.size()] != "}" || result.bindings[role] != 0xffffffffU || !Identifier(tokens[at + 1]))
                throw InvalidVfxEvidence{};
            auto end = begin + fields.size() + 1;
            if (end < tokens.size() && tokens[end] != ";") {
                if (!Identifier(tokens[end]))
                    throw InvalidVfxEvidence{};
                if (role == 0)
                    result.instanceObject = tokens[end];
                ++end;  // Optional generated instance name; the program API resolves the block type name.
            }
            if (end >= tokens.size() || tokens[end] != ";")
                throw InvalidVfxEvidence{};
            result.names[role] = tokens[at + 1];
            // GL410 has no required explicit resource binding syntax. These finite Horo slots are
            // installed on the candidate program by glUniformBlockBinding, after actual link/limit validation.
            result.bindings[role] = static_cast<std::uint32_t>(role);
            return end;
        }

        /** @brief Proves the unique combined sampler emitted from the closed single image/sampler pair. */
        std::size_t OpenGLSample(const std::vector<std::string_view> &tokens, const std::size_t at, NativeStageEvidence &result) {
            if (at + 3 >= tokens.size() || tokens[at + 3] != ";" || !Identifier(tokens[at + 2]) || result.bindings[2] != 0xffffffffU)
                throw InvalidVfxEvidence{};
            // There is exactly one authored image and one sampler, independently checked against Cross JSON.
            // Preserve its generated name; never guess SPIRV-Cross's combined-name spelling or default unit.
            result.names[2] = tokens[at + 2];
            result.names[3] = tokens[at + 2];
            result.bindings[2] = 0;
            result.bindings[3] = 0;  // Same Horo-selected texture unit; glProgramUniform1iv installs it after linking.
            return at + 3;
        }

        /** @brief Reads final GL link names only from complete explicit location/direction/vector declarations. */
        void OpenGLInterface(const std::vector<std::string_view> &tokens, const std::size_t at, const ShaderStage stage,
                             NativeStageEvidence &result) {
            if (at + 9 >= tokens.size() || tokens[at + 1] != "(" || tokens[at + 3] != "=" || tokens[at + 5] != ")" || tokens[at + 9] != ";")
                throw InvalidVfxEvidence{};
            const auto direction = tokens[at + 6], type = tokens[at + 7];
            std::uint32_t location{};
            const auto number = tokens[at + 4];
            const auto parsed = std::from_chars(number.data(), number.data() + number.size(), location);
            if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size() || location > 2 ||
                (direction != "in" && direction != "out") || type.size() != 4 || !type.starts_with("vec") || type[3] < '2' ||
                type[3] > '4' || !Identifier(tokens[at + 8]))
                throw InvalidVfxEvidence{};
            result.interfaces.push_back({stage, direction == "in" ? ShaderInterfaceDirection::Input : ShaderInterfaceDirection::Output,
                                         location, ShaderValueType::Float32, 1, static_cast<std::uint8_t>(type[3] - '0'), 1,
                                         std::string{tokens[at + 8]}});
        }

        /** @brief Proves zero-base matrix-vector position evaluation and its final assignment structurally. */
        void OpenGLMatrixLowering(const std::vector<std::string_view> &tokens, const NativeStageEvidence &native) {
            const auto position = [](const auto &entry) {
                return entry.direction == ShaderInterfaceDirection::Input && entry.location == 0 && entry.columns == 3;
            };
            if (native.instanceObject.empty() || std::ranges::count_if(native.interfaces, position) != 1)
                throw InvalidVfxEvidence{};
            const auto &name = std::ranges::find_if(native.interfaces, position)->nativeName;
            std::size_t matches{};
            for (std::size_t at = 0; at + 3 < tokens.size(); ++at) {
                if (tokens[at] != "precise" || tokens[at + 1] != "vec4" || !Identifier(tokens[at + 2]) || tokens[at + 3] != "=")
                    continue;
                const auto temporary = std::string{tokens[at + 2]};
                const auto expression = "precise vec4 " + temporary + " = " + native.instanceObject +
                                        ".ClipFromLocal[uint(gl_InstanceID)] * vec4(" + name + ", 1.0);";
                const auto expected = Tokens(expression);
                if (tokens.size() - at >= expected.size() && std::ranges::equal(std::span{tokens}.subspan(at, expected.size()), expected)) {
                    UniqueTokens(tokens, "gl_Position = " + temporary + ";");
                    ++matches;
                }
            }
            if (matches != 1)
                throw InvalidVfxEvidence{};
        }

        /** @brief Reads backend-specific GLSL410 std140 and combined-sampler evidence, without borrowing another target map. */
        NativeStageEvidence OpenGLPlacements(const std::string &source, const ShaderStage stage) {
            const auto tokens = Tokens(source);
            if (tokens.size() < 3 || tokens[0] != "#" || tokens[1] != "version" || tokens[2] != "410")
                throw InvalidVfxEvidence{};
            NativeStageEvidence result;
            for (std::size_t at = 3; at < tokens.size(); ++at) {
                if (tokens[at] == "layout" && at + 2 < tokens.size() && tokens[at + 2] == "location")
                    OpenGLInterface(tokens, at, stage, result);
                if (tokens[at] != "uniform")
                    continue;
                if (at + 1 >= tokens.size())
                    throw InvalidVfxEvidence{};
                at = tokens[at + 1] == "sampler2D" ? OpenGLSample(tokens, at, result) : OpenGLBlock(tokens, at, result);
            }
            if (stage == ShaderStage::Vertex)
                OpenGLMatrixLowering(tokens, result);
            return result;
        }

        /** @brief Validates an actual reflected member against the proven closed native array/matrix layout. */
        ShaderReflectedParameter Parameter(const Json &member, const bool instances, const std::size_t index) {
            using namespace VfxShaderParameterIds;
            constexpr std::uint32_t instanceArrayCount{MaximumVfxDrawInstances};
            constexpr std::array<std::string_view, 4> names{"ClipFromLocal", "InstanceColor", "Tint", "AlphaCutoff"};
            const auto role = instances ? index : index + 2;
            const bool matrix = role == 0;
            const std::uint32_t offset = role == 1 ? 8192U : role == 3 ? 16U : 0U;
            if (member.at("name") != names[role] ||
                member.at("type") != (matrix      ? "mat4"
                                      : role == 3 ? "float"
                                                  : "vec4") ||
                Unsigned(member.at("offset")) != offset || member.contains("physical_pointer"))
                throw InvalidVfxEvidence{};
            if (instances) {
                const auto &array = member.at("array"), &literal = member.at("array_size_is_literal");
                if (!array.is_array() || array.size() != 1 || Unsigned(array[0]) != MaximumVfxDrawInstances || !literal.is_array() ||
                    literal.size() != 1 || literal[0] != true || Unsigned(member.at("array_stride")) != (matrix ? 64U : 16U))
                    throw InvalidVfxEvidence{};
            } else if (member.contains("array") || member.contains("array_stride")) {
                throw InvalidVfxEvidence{};
            }
            // Native float4x4 arrays prove column-vector storage only. A different translator lowering
            // requires its own evidence/qualification; copying SPIR-V row-major packing would be unsafe.
            if (matrix ? Unsigned(member.at("matrix_stride")) != 16 || member.value("row_major", false)
                       : member.contains("matrix_stride") || member.contains("row_major"))
                throw InvalidVfxEvidence{};
            return {{static_cast<std::uint32_t>(role + 1)},
                    instances ? Instances : Material,
                    ShaderValueType::Float32,
                    static_cast<std::uint8_t>(matrix ? 4 : 1),
                    static_cast<std::uint8_t>(role == 3 ? 1 : 4),
                    instances ? instanceArrayCount : 1U,
                    offset,
                    instances ? (matrix ? 64U : 16U) : 0,
                    matrix ? 16U : 0,
                    true,
                    true};
        }

        /** @brief Reconciles JSON semantics with actual backend names; GL fragment-output lookup cannot use SPIR-V names. */
        void Interfaces(const Json &json, const ShaderStage stage, const NativeStageEvidence &native,
                        ShaderReflectionCandidate &candidate) {
            std::size_t count{};
            for (const auto direction : {ShaderInterfaceDirection::Input, ShaderInterfaceDirection::Output}) {
                const auto &values = json.at(direction == ShaderInterfaceDirection::Input ? "inputs" : "outputs");
                if (!values.is_array() || values.size() > 3)
                    throw InvalidVfxEvidence{};
                for (const auto &value : values) {
                    const auto type = value.at("type").get<std::string>();
                    if (type.size() != 4 || !type.starts_with("vec") || type[3] < '2' || type[3] > '4' || value.contains("array"))
                        throw InvalidVfxEvidence{};
                    const auto location = Unsigned(value.at("location"));
                    std::string name = value.at("name").get<std::string>();
                    if (candidate.backend == ShaderTargetBackend::OpenGL) {
                        const auto matches = [&](const auto &entry) {
                            return entry.stage == stage && entry.direction == direction && entry.location == location &&
                                   entry.columns == static_cast<std::uint8_t>(type[3] - '0');
                        };
                        if (std::ranges::count_if(native.interfaces, matches) != 1)
                            throw InvalidVfxEvidence{};
                        name = std::ranges::find_if(native.interfaces, matches)->nativeName;
                    }
                    candidate.stageInterface.push_back({stage, direction, location, ShaderValueType::Float32, 1,
                                                        static_cast<std::uint8_t>(type[3] - '0'), 1, std::move(name)});
                    ++count;
                }
            }
            if (candidate.backend == ShaderTargetBackend::OpenGL && count != native.interfaces.size())
                throw InvalidVfxEvidence{};
        }

        /** @brief Merges a proven per-stage resource without inventing cross-stage native placement. */
        void Activate(ShaderReflectionCandidate &candidate, const std::size_t role, const std::uint32_t native, const std::string &name,
                      const ShaderStageVisibility stage) {
            auto &binding = candidate.bindings[role];
            auto &mapping = candidate.targetBindings[role];
            if (mapping.active && (mapping.nativeBinding != native || mapping.nativeName != name))
                throw InvalidVfxEvidence{};
            binding.active = true;
            binding.stages = static_cast<ShaderStageVisibility>(static_cast<unsigned>(binding.stages) | static_cast<unsigned>(stage));
            mapping = {.id = binding.id,
                       .nativeBinding = native,
                       .nativeName = name,
                       .pairedSampler = role == 2 ? VfxShaderParameterIds::BaseColorSampler : ShaderBindingId{}};
        }

        /** @brief Validates the actual intermediate block and the same translation's native structure/slot together. */
        void Uniforms(const Json &json, const bool vertex, const NativeStageEvidence &native, ShaderReflectionCandidate &candidate) {
            std::array<bool, 2> seen{};
            if (json.contains("ubos")) {
                const auto &blocks = json.at("ubos");
                if (!blocks.is_array() || blocks.size() > 2)
                    throw InvalidVfxEvidence{};
                for (const auto &block : blocks) {
                    const auto name = block.at("name").get<std::string>();
                    const std::size_t role = name == "VfxInstances" || name == "type.VfxInstances" ? 0
                                             : name == "VfxMaterial" || name == "type.VfxMaterial" ? 1
                                                                                                   : 2;
                    if (role == 2 || seen[role] || (!vertex && role == 0) || native.bindings[role] == 0xffffffffU ||
                        Unsigned(block.at("set")) != 0 || Unsigned(block.at("binding")) != role || block.contains("array"))
                        throw InvalidVfxEvidence{};
                    seen[role] = true;
                    const auto bytes = Unsigned(block.at("block_size"));
                    if (role == 0 ? bytes != 10240 : bytes != 20 && bytes != 32)
                        throw InvalidVfxEvidence{};
                    const auto &members = json.at("types").at(block.at("type").get<std::string>()).at("members");
                    if (!members.is_array() || members.size() != 2)
                        throw InvalidVfxEvidence{};
                    for (std::size_t index = 0; index < 2; ++index) {
                        const auto parameter = Parameter(members[index], role == 0, index);
                        auto &retained = candidate.parameters[role * 2 + index];
                        // Both stages must describe exactly the same member layout.
                        if (retained.active &&
                            (retained.byteOffset != parameter.byteOffset || retained.arrayStride != parameter.arrayStride ||
                             retained.matrixStride != parameter.matrixStride || retained.columnMajor != parameter.columnMajor))
                            throw InvalidVfxEvidence{};
                        retained = parameter;
                    }
                    Activate(candidate, role, native.bindings[role], native.names[role],
                             vertex ? ShaderStageVisibility::Vertex : ShaderStageVisibility::Fragment);
                }
            }
            for (std::size_t role = 0; role < seen.size(); ++role)
                if (seen[role] != (native.bindings[role] != 0xffffffffU) || (vertex && !seen[role]))
                    throw InvalidVfxEvidence{};
        }

        /** @brief Requires the fixed image/sampler pair only in its actual fragment stage. */
        void Samples(const Json &json, const bool vertex, const NativeStageEvidence &native, ShaderReflectionCandidate &candidate) {
            for (const auto role : {std::size_t{2}, std::size_t{3}}) {
                const auto key = role == 2 ? "separate_images" : "separate_samplers";
                const auto name = role == 2 ? "BaseColor" : "BaseColorSampler";
                const auto type = role == 2 ? "texture2D" : "sampler";
                if (vertex) {
                    if ((json.contains(key) && !json.at(key).empty()) || native.bindings[role] != 0xffffffffU)
                        throw InvalidVfxEvidence{};
                    continue;
                }
                const auto &entries = json.at(key);
                if (!entries.is_array() || entries.size() != 1 || native.bindings[role] == 0xffffffffU)
                    throw InvalidVfxEvidence{};
                const auto &entry = entries[0];
                if (entry.at("name") != name || entry.at("type") != type || Unsigned(entry.at("set")) != 0 ||
                    Unsigned(entry.at("binding")) != role || entry.contains("array"))
                    throw InvalidVfxEvidence{};
                Activate(candidate, role, native.bindings[role], native.names[role], ShaderStageVisibility::Fragment);
            }
        }

        /** @brief Normalizes one same-operation intermediate/native evidence pair after bounded duplicate-safe parsing. */
        void Stage(const SpirvReflectionStage &stage, const NativeStageEvidence &native, const bool vertex,
                   ShaderReflectionCandidate &candidate) {
            if (stage.stage != (vertex ? ShaderStage::Vertex : ShaderStage::Fragment) ||
                stage.entryPoint != (vertex ? "VSMain" : "PSMain") || stage.json.empty() ||
                stage.json.size() > ShaderReflectionCookDetail::MaximumMetadataBytes)
                throw InvalidVfxEvidence{};
            Foundation::JsonParseGuard guard{24};
            const auto json = guard.Parse(stage.json.begin(), stage.json.end());
            if (guard.HasDuplicate() || guard.IsTooDeep() || json.is_discarded() || !json.is_object())
                throw InvalidVfxEvidence{};
            const auto &entries = json.at("entryPoints");
            if (!entries.is_array() || entries.size() != 1 || entries[0].at("name") != stage.entryPoint ||
                entries[0].at("mode") != (vertex ? "vert" : "frag"))
                throw InvalidVfxEvidence{};
            for (const auto key : {"textures", "images", "ssbos", "push_constants", "subpass_inputs", "counters", "acceleration_structures",
                                   "tensors", "specialization_constants"})
                if (json.contains(key) && !json.at(key).empty())
                    throw InvalidVfxEvidence{};
            Uniforms(json, vertex, native, candidate);
            Samples(json, vertex, native, candidate);
            Interfaces(json, stage.stage, native, candidate);
        }
    }  // namespace

    /** @brief Binds closed backend-specific source evidence to the shared logical interface validation. */
    static Result<NormalizedShaderReflection> NormalizeVfxNativeReflection(const ShaderManifest &manifest,
                                                                           const ShaderTargetRequirement &target,
                                                                           const std::span<const SpirvReflectionStage> stages,
                                                                           const std::span<const NativeStageEvidence> native) {
        using Outcome = Result<NormalizedShaderReflection>;
        try {
            const auto canonical = MakeVfxShaderManifest(target);
            const auto expected = ComputeShaderInterfaceCompatibilityId(canonical);
            const auto actual = ComputeShaderInterfaceCompatibilityId(manifest);
            if ((target.backend != ShaderTargetBackend::Metal && target.backend != ShaderTargetBackend::OpenGL) || stages.size() != 2 ||
                native.size() != 2 || manifest.sourceRevision != canonical.sourceRevision || expected.HasError() || actual.HasError() ||
                expected.Value() != actual.Value())
                throw InvalidVfxEvidence{};
            ShaderReflectionCandidate candidate{.backend = target.backend, .interfaceSchemaVersion = target.interfaceSchemaVersion};
            for (const auto &binding : canonical.bindings) {
                candidate.bindings.push_back(
                    {binding.id, binding.kind, binding.access, binding.arrayCount, ShaderStageVisibility::None, false});
                candidate.targetBindings.push_back({.id = binding.id, .active = false});
            }
            for (const auto &parameter : canonical.parameters)
                candidate.parameters.push_back({parameter.id, parameter.binding, parameter.type, parameter.rows, parameter.columns,
                                                parameter.arrayCount, 0, 0, 0, true, false});
            Stage(stages[0], native[0], true, candidate);
            Stage(stages[1], native[1], false, candidate);
            std::ranges::sort(candidate.stageInterface, {}, [](const auto &entry) {
                return std::tuple{entry.stage, entry.direction, entry.location};
            });
            return NormalizeShaderReflection(manifest, target, candidate);
        } catch (const Json::exception &) {
            return Outcome::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
        } catch (const InvalidVfxEvidence &) {
            return Outcome::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(ShaderReflectionErrors::AllocationFailed));
        }
    }

    /** @brief Parses only the selected backend's actual translation before sharing logical evidence validation. */
    static Result<NormalizedShaderReflection> NormalizeVfxTranslations(const ShaderManifest &manifest,
                                                                       const ShaderTargetRequirement &target,
                                                                       const std::span<const SpirvReflectionStage> stages,
                                                                       const std::span<const std::string> translations,
                                                                       const ShaderTargetBackend backend) {
        using Outcome = Result<NormalizedShaderReflection>;
        if (target.backend != backend || translations.size() != 2)
            return Outcome::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
        try {
            std::array<NativeStageEvidence, 2> native;
            for (std::size_t index = 0; index < native.size(); ++index) {
                if (backend == ShaderTargetBackend::Metal)
                    native[index].bindings = NativePlacements(translations[index], index == 0);
                else
                    native[index] = OpenGLPlacements(translations[index], index == 0 ? ShaderStage::Vertex : ShaderStage::Fragment);
            }
            if (backend == ShaderTargetBackend::OpenGL) {
                for (const auto &input : native[1].interfaces) {
                    if (input.direction != ShaderInterfaceDirection::Input)
                        continue;
                    const auto matches = [&](const auto &output) {
                        return output.direction == ShaderInterfaceDirection::Output && output.location == input.location &&
                               output.nativeName == input.nativeName && output.type == input.type && output.rows == input.rows &&
                               output.columns == input.columns && output.arrayCount == input.arrayCount;
                    };
                    if (std::ranges::count_if(native[0].interfaces, matches) != 1)
                        throw InvalidVfxEvidence{};
                }
            }
            return NormalizeVfxNativeReflection(manifest, target, stages, native);
        } catch (const InvalidVfxEvidence &) {
            return Outcome::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(ShaderReflectionErrors::AllocationFailed));
        }
    }

    /** @copydoc NormalizeVfxMetalReflection */
    Result<NormalizedShaderReflection> NormalizeVfxMetalReflection(const ShaderManifest &manifest, const ShaderTargetRequirement &target,
                                                                   const std::span<const SpirvReflectionStage> stages,
                                                                   const std::span<const std::string> translations) {
        return NormalizeVfxTranslations(manifest, target, stages, translations, ShaderTargetBackend::Metal);
    }

    /** @copydoc NormalizeVfxOpenGLReflection */
    Result<NormalizedShaderReflection> NormalizeVfxOpenGLReflection(const ShaderManifest &manifest, const ShaderTargetRequirement &target,
                                                                    const std::span<const SpirvReflectionStage> stages,
                                                                    const std::span<const std::string> translations) {
        return NormalizeVfxTranslations(manifest, target, stages, translations, ShaderTargetBackend::OpenGL);
    }
}  // namespace Horo::Render
