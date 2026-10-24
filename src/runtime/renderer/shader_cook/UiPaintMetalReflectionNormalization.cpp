#include "Horo/Runtime/Render/ShaderReflectionErrors.h"
#include "Horo/Runtime/Render/UiPaintKernel.h"
#include "ShaderReflectionCookJson.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <tuple>

namespace Horo::Render {
    namespace {
        using Foundation::Json;

        struct InvalidEvidence {};

        struct Member {
            std::string_view name;
            std::string_view type;
            std::uint32_t offset;
        };

        constexpr std::array Members{Member{"Extent", "vec2", 0},         Member{"Opacity", "float", 8},
                                     Member{"Sampling", "uint", 12},      Member{"Flags", "uint", 16},
                                     Member{"Reserved0", "uint", 20},     Member{"Reserved1", "uint", 24},
                                     Member{"Reserved2", "uint", 28},     Member{"MaskExtent", "vec2", 32},
                                     Member{"MaskReserved", "uvec2", 40}, Member{"CornerRadii", "vec4", 48}};

        /** @brief Rejects signed, narrowing and non-literal placement evidence. */
        std::uint32_t Unsigned(const Json &value) {
            if (!value.is_number_unsigned() || value.get<std::uint64_t>() > 0xffffffffULL)
                throw InvalidEvidence{};
            return value.get<std::uint32_t>();
        }

        /** @brief Tokenizes bounded generated MSL while excluding comments and string literals from binding evidence. */
        std::vector<std::string_view> Tokens(const std::string_view text) {
            if (text.empty() || text.size() > 1024U * 1024U)
                throw InvalidEvidence{};
            std::vector<std::string_view> result;
            for (std::size_t cursor = 0; cursor < text.size();) {
                const auto start = cursor;
                const auto ch = text[cursor];
                if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') {
                    ++cursor;
                    continue;
                }
                if (text.substr(cursor, 2) == "//") {
                    const auto end = text.find('\n', cursor + 2);
                    cursor = end == text.npos ? text.size() : end;
                    continue;
                }
                if (text.substr(cursor, 2) == "/*") {
                    const auto end = text.find("*/", cursor + 2);
                    if (end == text.npos)
                        throw InvalidEvidence{};
                    cursor = end + 2;
                    continue;
                }
                if (ch == '"' || ch == '\'') {
                    ++cursor;
                    while (cursor < text.size() && text[cursor] != ch) {
                        if (text[cursor] == '\\')
                            ++cursor;
                        ++cursor;
                    }
                    if (cursor >= text.size())
                        throw InvalidEvidence{};
                    ++cursor;
                    continue;
                }
                const auto word = [](const char value) {
                    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') ||
                           value == '_';
                };
                if (word(ch))
                    while (cursor < text.size() && word(text[cursor]))
                        ++cursor;
                else
                    ++cursor;
                if (result.size() == 131072)
                    throw InvalidEvidence{};
                result.push_back(text.substr(start, cursor - start));
            }
            return result;
        }

        /** @brief Proves the generated constant struct uses the exact closed scalar/vector packing, not SPIR-V offsets alone. */
        void StructPacking(const std::vector<std::string_view> &tokens, const std::string_view type) {
            std::size_t matches = 0;
            for (std::size_t index = 0; index + 2 < tokens.size(); ++index) {
                if (tokens[index] != "struct" || tokens[index + 1] != type || tokens[index + 2] != "{")
                    continue;
                ++matches;
                std::size_t cursor = index + 3;
                std::uint32_t offset = 0;
                for (const auto &member : Members) {
                    const auto native = member.type == "vec2"    ? "float2"
                                        : member.type == "vec4"  ? "float4"
                                        : member.type == "uvec2" ? "uint2"
                                                                 : member.type;
                    const std::uint32_t width = member.type == "vec4" ? 16U : member.type == "vec2" || member.type == "uvec2" ? 8U : 4U;
                    offset = (offset + width - 1) / width * width;
                    if (cursor + 2 >= tokens.size() || tokens[cursor] != native || tokens[cursor + 1] != member.name ||
                        tokens[cursor + 2] != ";" || offset != member.offset)
                        throw InvalidEvidence{};
                    offset += width;
                    cursor += 3;
                }
                if (cursor >= tokens.size() || tokens[cursor] != "}" || offset != 64)
                    throw InvalidEvidence{};
            }
            if (matches != 1)
                throw InvalidEvidence{};
        }

        /** @brief Reads only complete generated resource attributes, never inferred default resource indices. */
        std::array<std::uint32_t, 4> Placements(const std::string &source, const bool vertex) {
            constexpr auto Missing = 0xffffffffU;
            std::array<std::uint32_t, 4> placements{Missing, Missing, Missing, Missing};
            const auto tokens = Tokens(source);
            for (std::size_t index = 1; index + 7 < tokens.size(); ++index) {
                if (tokens[index] != "[" || tokens[index + 1] != "[")
                    continue;
                const auto kind = tokens[index + 2];
                if (kind != "buffer" && kind != "texture" && kind != "sampler")
                    continue;
                if (tokens[index + 3] != "(" || tokens[index + 5] != ")" || tokens[index + 6] != "]" || tokens[index + 7] != "]")
                    throw InvalidEvidence{};
                std::uint32_t value{};
                const auto number = tokens[index + 4];
                const auto parsed = std::from_chars(number.data(), number.data() + number.size(), value);
                if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size() || value > 30)
                    throw InvalidEvidence{};
                std::size_t slot = 4;
                if (kind == "buffer") {
                    // The one reflected fixed block must be a constant reference, not a device or writable argument.
                    if (index < 4 || tokens[index - 2] != "&" || tokens[index - 4] != "constant")
                        throw InvalidEvidence{};
                    StructPacking(tokens, tokens[index - 3]);
                    slot = 0;
                } else if (!vertex && kind == "texture" && tokens[index - 1] == "PaintTexture")
                    slot = 1;
                else if (!vertex && kind == "texture" && tokens[index - 1] == "CoverageTexture")
                    slot = 2;
                else if (!vertex && kind == "sampler" && tokens[index - 1] == "PaintSampler")
                    slot = 3;
                if (slot == 4 || placements[slot] != Missing)
                    throw InvalidEvidence{};
                placements[slot] = value;
            }
            if (placements[0] == Missing || (!vertex && (placements[1] == Missing || placements[2] == Missing || placements[3] == Missing ||
                                                         placements[1] == placements[2])))
                throw InvalidEvidence{};
            return placements;
        }

        /** @brief Checks every actual compiler-produced fixed member, including reserved packing, before publishing a map. */
        void Uniform(const Json &json) {
            const auto &blocks = json.at("ubos");
            if (!blocks.is_array() || blocks.size() != 1)
                throw InvalidEvidence{};
            const auto &block = blocks[0];
            if (Unsigned(block.at("block_size")) != 64 || Unsigned(block.at("set")) != 0 || Unsigned(block.at("binding")) != 0 ||
                block.contains("array"))
                throw InvalidEvidence{};
            const auto &members = json.at("types").at(block.at("type").get<std::string>()).at("members");
            if (!members.is_array() || members.size() != Members.size())
                throw InvalidEvidence{};
            for (std::size_t index = 0; index < Members.size(); ++index) {
                const auto &member = members[index];
                if (member.at("name") != Members[index].name || member.at("type") != Members[index].type ||
                    Unsigned(member.at("offset")) != Members[index].offset || member.contains("array") ||
                    member.contains("matrix_stride") || member.contains("physical_pointer"))
                    throw InvalidEvidence{};
            }
        }

        /** @brief Admits only the complete actual texture/sampler set for the selected fixed stage. */
        void Resources(const Json &json, const bool vertex) {
            for (const auto key : {"textures", "images", "ssbos", "push_constants", "subpass_inputs", "counters", "acceleration_structures",
                                   "tensors", "specialization_constants"})
                if (json.contains(key) && !json.at(key).empty())
                    throw InvalidEvidence{};
            for (const auto key : {"separate_images", "separate_samplers"}) {
                const auto expected = vertex ? 0U : (std::string_view{key} == "separate_images" ? 2U : 1U);
                if (!json.contains(key)) {
                    if (expected != 0)
                        throw InvalidEvidence{};
                    continue;
                }
                const auto &entries = json.at(key);
                if (!entries.is_array() || entries.size() != expected)
                    throw InvalidEvidence{};
                std::array<bool, 3> seen{};
                for (const auto &entry : entries) {
                    const auto name = entry.at("name").get<std::string>();
                    const auto slot = name == "PaintTexture" ? 0U : name == "CoverageTexture" ? 1U : name == "PaintSampler" ? 2U : 3U;
                    if (slot == 3 || seen[slot] || (std::string_view{key} == "separate_images") != (slot < 2) ||
                        Unsigned(entry.at("set")) != 0 || Unsigned(entry.at("binding")) != slot + 1 || entry.contains("array") ||
                        (slot < 2 && entry.at("type") != "texture2D"))
                        throw InvalidEvidence{};
                    seen[slot] = true;
                }
            }
        }

        /** @brief Validates exact stage entry and finite user IO against the canonical raster ABI. */
        void Interfaces(const Json &json, const ShaderStage stage, ShaderReflectionCandidate &candidate) {
            const bool vertex = stage == ShaderStage::Vertex;
            const auto &entries = json.at("entryPoints");
            if (!entries.is_array() || entries.size() != 1 || entries[0].at("name") != (vertex ? "PaintVertex" : "PaintFragment") ||
                entries[0].at("mode") != (vertex ? "vert" : "frag"))
                throw InvalidEvidence{};
            for (const auto direction : {ShaderInterfaceDirection::Input, ShaderInterfaceDirection::Output}) {
                const auto &variables = json.at(direction == ShaderInterfaceDirection::Input ? "inputs" : "outputs");
                const auto count = !vertex && direction == ShaderInterfaceDirection::Output ? 1U : 3U;
                if (!variables.is_array() || variables.size() != count)
                    throw InvalidEvidence{};
                std::array<bool, 3> seen{};
                for (const auto &variable : variables) {
                    const auto location = Unsigned(variable.at("location"));
                    const auto width = count == 1 ? 4U
                                                  : ((vertex && direction == ShaderInterfaceDirection::Input) ? (location == 2 ? 4U : 2U)
                                                                                                              : (location == 1 ? 4U : 2U));
                    if (location >= count || seen[location] || variable.contains("array") ||
                        variable.at("type") != (width == 4 ? "vec4" : "vec2"))
                        throw InvalidEvidence{};
                    seen[location] = true;
                    candidate.stageInterface.push_back({stage, direction, location, ShaderValueType::Float32, 1,
                                                        static_cast<std::uint8_t>(width), 1, variable.at("name").get<std::string>()});
                }
            }
        }

        /** @brief Validates one complete compiler stage before retaining its exact generated native placements. */
        std::array<std::uint32_t, 4> StageEvidence(const SpirvReflectionStage &stage, const std::string &translation, const bool vertex,
                                                   ShaderReflectionCandidate &candidate) {
            if (stage.stage != (vertex ? ShaderStage::Vertex : ShaderStage::Fragment) ||
                stage.entryPoint != (vertex ? "PaintVertex" : "PaintFragment") || stage.json.empty() || stage.json.size() > 1024U * 1024U)
                throw InvalidEvidence{};
            Foundation::JsonParseGuard guard{24};
            const auto json = guard.Parse(stage.json.begin(), stage.json.end());
            if (guard.HasDuplicate() || guard.IsTooDeep() || json.is_discarded() || !json.is_object())
                throw InvalidEvidence{};
            Uniform(json);
            Resources(json, vertex);
            Interfaces(json, stage.stage, candidate);
            return Placements(translation, vertex);
        }
    }  // namespace

    /** @copydoc NormalizeUiPaintMetalReflection */
    Result<NormalizedShaderReflection> NormalizeUiPaintMetalReflection(const ShaderManifest &manifest,
                                                                       const ShaderTargetRequirement &target,
                                                                       std::span<const SpirvReflectionStage> stages,
                                                                       std::span<const std::string> translations) {
        using Outcome = Result<NormalizedShaderReflection>;
        try {
            const auto expected = ComputeShaderInterfaceCompatibilityId(MakeUiPaintShaderManifest(target));
            const auto actual = ComputeShaderInterfaceCompatibilityId(manifest);
            if (target.backend != ShaderTargetBackend::Metal || stages.size() != 2 || translations.size() != 2 || expected.HasError() ||
                actual.HasError() || expected.Value() != actual.Value())
                throw InvalidEvidence{};
            ShaderReflectionCandidate candidate{.backend = target.backend, .interfaceSchemaVersion = target.interfaceSchemaVersion};
            for (const auto &binding : manifest.bindings)
                candidate.bindings.push_back({binding.id, binding.kind, binding.access, binding.arrayCount, binding.stages, true});
            std::array<std::uint32_t, 4> fragment{};
            std::uint32_t vertexBuffer{};
            for (std::size_t index = 0; index < stages.size(); ++index) {
                const bool vertex = index == 0;
                const auto placements = StageEvidence(stages[index], translations[index], vertex, candidate);
                if (vertex)
                    vertexBuffer = placements[0];
                else
                    fragment = placements;
            }
            if (vertexBuffer != fragment[0])
                throw InvalidEvidence{};
            for (std::uint32_t index = 0; index < fragment.size(); ++index)
                candidate.targetBindings.push_back({.id = {index + 1},
                                                    .nativeBinding = fragment[index],
                                                    .pairedSampler = index == 1 || index == 2 ? ShaderBindingId{4} : ShaderBindingId{}});
            std::ranges::sort(candidate.stageInterface, {}, [](const auto &variable) {
                return std::tuple{variable.stage, variable.direction, variable.location};
            });
            return NormalizeShaderReflection(manifest, target, candidate);
        } catch (const Json::exception &) {
            return Outcome::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
        } catch (const InvalidEvidence &) {
            return Outcome::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(ShaderReflectionErrors::AllocationFailed));
        }
    }
}  // namespace Horo::Render
