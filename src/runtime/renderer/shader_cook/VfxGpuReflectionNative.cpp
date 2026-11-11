#include "Horo/Runtime/Render/ShaderReflectionErrors.h"
#include "MetalReflectionEvidence.h"
#include "RegisterRasterReflectionInternal.h"
#include "VfxGpuReflection.h"

#include <algorithm>
#include <optional>

namespace Horo::Render::VfxGpuCookDetail {
    namespace {
        using Tokens = RegisterRasterCookDetail::TokenView;
        using RegisterRasterCookDetail::Identifier;
        using RegisterRasterCookDetail::Literal;
        using RegisterRasterCookDetail::StructureBody;

        struct Entry {
            std::string_view result;
            Tokens parameters;
        };

        std::size_t Closing(Tokens tokens, std::size_t open) {
            std::size_t depth = 1;
            for (std::size_t at = open + 1; at < tokens.size(); ++at) {
                if (tokens[at] == "(")
                    ++depth;
                if (tokens[at] == ")" && --depth == 0)
                    return at;
            }
            throw InvalidEvidence{};
        }

        bool EntryPrefix(Tokens tokens, std::size_t at, std::string_view keyword, std::string_view name) {
            return tokens[at] == keyword && tokens[at + 2] == name && tokens[at + 3] == "(";
        }

        Entry Selected(Tokens tokens, VfxGpuProgramRole role, ShaderStage stage) {
            const auto name = EntryName(role, stage);
            const auto keyword = stage == ShaderStage::Compute ? "kernel" : stage == ShaderStage::Vertex ? "vertex" : "fragment";
            std::optional<Entry> selected;
            for (std::size_t at = 0; at + 3 < tokens.size(); ++at) {
                if (!EntryPrefix(tokens, at, keyword, name))
                    continue;
                const auto end = Closing(tokens, at + 3);
                if (selected || end + 1 >= tokens.size() || tokens[end + 1] != "{")
                    throw InvalidEvidence{};
                selected = Entry{tokens[at + 1], tokens.subspan(at + 4, end - at - 4)};
            }
            if (!selected)
                throw InvalidEvidence{};
            return *selected;
        }

        Field Primitive(std::string_view type, std::string_view name, std::uint32_t offset) {
            if (type == "uint4")
                return {std::string{name}, "uvec4", offset, 16, 0};
            if (type == "float4")
                return {std::string{name}, "vec4", offset, 16, 0};
            if (type == "float4x4")
                return {std::string{name}, "mat4", offset, 64, 16};
            throw InvalidEvidence{};
        }

        void Uniform(Tokens tokens, std::string_view type, Buffer &buffer, VfxGpuProgramRole role) {
            const auto body = StructureBody(tokens, type);
            const auto names = UniformNames(role);
            if (body.size() != names.size() * 3)
                throw InvalidEvidence{};
            for (std::size_t i = 0; i < names.size(); ++i) {
                const auto field = body.subspan(i * 3, 3);
                if (field[1] != names[i] || field[2] != ";")
                    throw InvalidEvidence{};
                if (const auto expected = Compute(role) || i == 4 ? "uint4" : i == 0 ? "float4x4" : "float4"; field[0] != expected)
                    throw InvalidEvidence{};
                auto value = Primitive(field[0], field[1], buffer.bytes);
                buffer.bytes += value.bytes;
                buffer.fields.push_back(std::move(value));
            }
        }

        bool WordWrapper(Tokens body) {
            if (body.size() != 6 || body[0] != "uint" || !Identifier(body[1]))
                return false;
            constexpr std::array<std::string_view, 4> shape{"[", "1", "]", ";"};
            return std::ranges::equal(body.subspan(2), shape);
        }

        void Storage(Tokens tokens, std::string_view type, bool reference, Buffer &buffer) {
            if (!reference && type == "uint") {
                buffer.bytes = 4;
                return;
            }
            if (!reference)
                throw InvalidEvidence{};
            const auto body = StructureBody(tokens, type);
            if (!WordWrapper(body))
                throw InvalidEvidence{};
            buffer.wrapperMember = body[1];
            buffer.bytes = 4;
        }

        std::size_t Attribute(Tokens argument) {
            const auto first = std::ranges::find(argument, "[");
            const auto at = static_cast<std::size_t>(first - argument.begin());
            if (at + 4 >= argument.size() || argument[at + 1] != "[")
                throw InvalidEvidence{};
            return at;
        }

        std::uint32_t Slot(Tokens argument, std::size_t at) {
            if (argument.size() != at + 8 || argument[at + 2] != "buffer" || argument[at + 3] != "(" || argument[at + 5] != ")" ||
                argument[at + 6] != "]" || argument[at + 7] != "]")
                throw InvalidEvidence{};
            return Literal(argument[at + 4], 30);
        }

        std::size_t ResourceAddress(Tokens argument, std::size_t attribute, Buffer &buffer) {
            const std::size_t start = argument[0] == "const" ? 1 : 0;
            buffer.constant = argument[start] == "constant";
            buffer.readonly = start != 0 || buffer.constant;
            if (!buffer.constant && argument[start] != "device")
                throw InvalidEvidence{};
            if (attribute != start + 4 || !Identifier(argument[start + 1]) || !Identifier(argument[start + 3]))
                throw InvalidEvidence{};
            return start;
        }

        Buffer Resource(Tokens tokens, Tokens argument, std::size_t attribute, VfxGpuProgramRole role) {
            Buffer buffer;
            buffer.slot = Slot(argument, attribute);
            const auto start = ResourceAddress(argument, attribute, buffer);
            const auto reference = argument[start + 2] == "&";
            if (!reference && argument[start + 2] != "*")
                throw InvalidEvidence{};
            buffer.name = argument[start + 3];
            if (buffer.constant) {
                if (!reference)
                    throw InvalidEvidence{};
                Uniform(tokens, argument[start + 1], buffer, role);
            } else
                Storage(tokens, argument[start + 1], reference, buffer);
            return buffer;
        }

        bool PositionField(Tokens field) {
            if (field.size() != 8 || !Identifier(field[1]))
                return false;
            const std::array<std::string_view, 8> expected{"float4", field[1], "[", "[", "position", "]", "]", ";"};
            return std::ranges::equal(field, expected);
        }

        void AttributedField(Tokens field) {
            if (field.size() != 11 || field[0] != "float4" || !Identifier(field[1]))
                throw InvalidEvidence{};
            constexpr std::array<std::size_t, 7> indices{2, 3, 5, 7, 8, 9, 10};
            constexpr std::array<std::string_view, 7> expected{"[", "[", "(", ")", "]", "]", ";"};
            for (std::size_t i = 0; i < indices.size(); ++i)
                if (field[indices[i]] != expected[i])
                    throw InvalidEvidence{};
        }

        bool InterfaceKind(Tokens field, ShaderStage stage, ShaderInterfaceDirection direction) {
            const bool user = field[4] == "user" && field[6] == "locn0";
            const bool color =
                stage == ShaderStage::Fragment && direction == ShaderInterfaceDirection::Output && field[4] == "color" && field[6] == "0";
            return user || color;
        }

        void Interface(Tokens field, ShaderStage stage, ShaderInterfaceDirection direction, NativeEvidence &result) {
            if (PositionField(field))
                return;
            AttributedField(field);
            if (!InterfaceKind(field, stage, direction))
                throw InvalidEvidence{};
            result.interfaces.push_back({stage, direction, 0, ShaderValueType::Float32, 1, 4, 1, std::string{field[1]}});
        }

        void Interfaces(Tokens tokens, std::string_view type, ShaderStage stage, ShaderInterfaceDirection direction,
                        NativeEvidence &result) {
            if (type == "void")
                return;
            const auto body = StructureBody(tokens, type);
            for (std::size_t at = 0; at < body.size();) {
                const auto remaining = body.subspan(at);
                const auto end = std::ranges::find(remaining, ";");
                if (end == remaining.end())
                    throw InvalidEvidence{};
                const auto count = static_cast<std::size_t>(end - remaining.begin()) + 1;
                Interface(body.subspan(at, count), stage, direction, result);
                if (result.interfaces.size() > 3)
                    throw InvalidEvidence{};
                at += count;
            }
        }

        bool BuiltinShape(Tokens argument, std::size_t at) {
            return at == 2 && argument.size() == 7 && Identifier(argument[1]) && argument[5] == "]" && argument[6] == "]";
        }

        void Builtin(Tokens argument, std::size_t at, ShaderStage stage) {
            if (!BuiltinShape(argument, at))
                throw InvalidEvidence{};
            if (stage == ShaderStage::Compute && argument[0] == "uint3" && argument[4] == "thread_position_in_grid")
                return;
            if (stage != ShaderStage::Vertex || argument[0] != "uint")
                throw InvalidEvidence{};
            constexpr std::array names{"vertex_id", "instance_id", "base_instance"};
            if (std::ranges::find(names, argument[4]) == names.end())
                throw InvalidEvidence{};
        }

        bool Readonly(const Buffer &buffer, VfxGpuProgramRole role) {
            if (buffer.constant)
                return true;
            const auto names = ResourceNames(role);
            const auto storage = names.first(names.size() - 1);
            const auto found = std::ranges::find(storage, buffer.name);
            if (found == storage.end())
                throw InvalidEvidence{};
            const auto index = static_cast<std::size_t>(found - storage.begin());
            return !Compute(role) || index == 0 || index == 2;
        }

        void Unique(const Buffer &buffer, const NativeEvidence &result) {
            if (std::ranges::any_of(result.buffers, [&](const auto &old) {
                return old.name == buffer.name || old.slot == buffer.slot || (old.constant && buffer.constant);
            }))
                throw InvalidEvidence{};
        }

        void Retain(Buffer buffer, Tokens argument, std::string_view source, bool proven, VfxGpuProgramRole role, NativeEvidence &result) {
            const bool readonly = Readonly(buffer, role);
            if (buffer.readonly != readonly && !(readonly && proven))
                throw InvalidEvidence{};
            if (readonly && !buffer.readonly)
                result.qualificationOffsets.push_back(static_cast<std::size_t>(argument[0].data() - source.data()));
            Unique(buffer, result);
            if (result.buffers.size() == ResourceNames(role).size())
                throw InvalidEvidence{};
            result.buffers.push_back(std::move(buffer));
        }

        void Argument(Tokens tokens, Tokens argument, std::string_view source, VfxGpuProgramRole role, ShaderStage stage, bool proven,
                      NativeEvidence &result) {
            const auto at = Attribute(argument);
            if (argument[at + 2] == "buffer") {
                if (stage == ShaderStage::Fragment)
                    throw InvalidEvidence{};
                Retain(Resource(tokens, argument, at, role), argument, source, proven, role, result);
                return;
            }
            if (argument[at + 2] == "stage_in") {
                if (stage != ShaderStage::Fragment || argument.size() != 7 || at != 2 || argument[5] != "]" || argument[6] != "]")
                    throw InvalidEvidence{};
                Interfaces(tokens, argument[0], stage, ShaderInterfaceDirection::Input, result);
                return;
            }
            Builtin(argument, at, stage);
        }

        void Arguments(Tokens tokens, Tokens parameters, std::string_view source, VfxGpuProgramRole role, ShaderStage stage, bool proven,
                       NativeEvidence &result) {
            std::size_t start = 0;
            std::size_t depth = 0;
            for (std::size_t at = 0; at < parameters.size(); ++at) {
                if (parameters[at] == "(")
                    ++depth;
                if (parameters[at] == ")") {
                    if (!depth)
                        throw InvalidEvidence{};
                    --depth;
                }
                if (parameters[at] != "," || depth)
                    continue;
                Argument(tokens, parameters.subspan(start, at - start), source, role, stage, proven, result);
                start = at + 1;
            }
            if (depth)
                throw InvalidEvidence{};
            if (start < parameters.size())
                Argument(tokens, parameters.subspan(start), source, role, stage, proven, result);
        }
    }  // namespace

    NativeEvidence ParseNative(std::string_view source, VfxGpuProgramRole role, ShaderStage stage, bool provenReadonly) {
        const auto tokens = MetalReflectionCookDetail::Tokens(source);
        const auto entry = Selected(tokens, role, stage);
        NativeEvidence evidence;
        Interfaces(tokens, entry.result, stage, ShaderInterfaceDirection::Output, evidence);
        Arguments(tokens, entry.parameters, source, role, stage, provenReadonly, evidence);
        if (const auto expected = stage == ShaderStage::Fragment ? 0 : ResourceNames(role).size(); evidence.buffers.size() != expected)
            throw InvalidEvidence{};
        return evidence;
    }
}  // namespace Horo::Render::VfxGpuCookDetail

namespace Horo::Render {
    namespace {
        VfxGpuProgramRole QualificationRole(const ShaderManifest &manifest) {
            const auto role = VfxGpuCookDetail::Role(manifest);
            const auto target = std::ranges::find(manifest.targets, ShaderTargetBackend::Metal, &ShaderTargetRequirement::backend);
            if (target == manifest.targets.end())
                throw VfxGpuCookDetail::InvalidEvidence{};
            if (ValidateVfxGpuMetalReflection(manifest, *target, VfxGpuShaderReflectionNames(role)).HasError())
                throw VfxGpuCookDetail::InvalidEvidence{};
            return role;
        }
    }  // namespace

    Result<std::string> QualifyVfxGpuMetalInputs(const ShaderManifest &manifest, const ShaderEntryPoint &entry, std::string_view source,
                                                 std::span<const std::uint8_t> validatedSpirv) {
        using namespace VfxGpuCookDetail;
        try {
            const auto role = QualificationRole(manifest);
            if (entry.name != EntryName(role, entry.stage))
                throw InvalidEvidence{};
            if (entry.stage == ShaderStage::Fragment) {
                (void)ParseNative(source, role, entry.stage, false);
                return Result<std::string>::Success(std::string{source});
            }
            (void)ProveSpirv(validatedSpirv, role);
            auto evidence = ParseNative(source, role, entry.stage, true);
            std::ranges::sort(evidence.qualificationOffsets, std::greater<>{});
            std::string qualified{source};
            for (const auto offset : evidence.qualificationOffsets)
                qualified.insert(offset, "const ");
            (void)ParseNative(qualified, role, entry.stage, false);
            return Result<std::string>::Success(std::move(qualified));
        } catch (const InvalidEvidence &) {
            return Result<std::string>::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
        } catch (const RegisterRasterCookDetail::InvalidEvidence &) {
            return Result<std::string>::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
        } catch (const MetalReflectionCookDetail::InvalidEvidence &) {
            return Result<std::string>::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
        } catch (const std::bad_alloc &) {
            return Result<std::string>::Failure(MakeError(ShaderReflectionErrors::AllocationFailed));
        }
    }
}  // namespace Horo::Render
