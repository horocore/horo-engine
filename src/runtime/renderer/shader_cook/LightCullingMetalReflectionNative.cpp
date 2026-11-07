#include "Horo/Runtime/Render/ShaderReflectionErrors.h"
#include "LightCullingMetalReflectionInternal.h"

#include <algorithm>

namespace Horo::Render::LightCullingMetalCookDetail {
    namespace {
        using RegisterRasterCookDetail::Identifier;
        using RegisterRasterCookDetail::Literal;
        using RegisterRasterCookDetail::StructureBody;

        struct Primitive {
            std::string_view json;
            std::uint32_t bytes, alignment;
        };

        std::optional<Primitive> Type(std::string_view type) {
            if (type == "uint")
                return Primitive{"uint", 4, 4};
            if (type == "uint2")
                return Primitive{"uvec2", 8, 8};
            if (type == "uint4")
                return Primitive{"uvec4", 16, 16};
            if (type == "float2")
                return Primitive{"vec2", 8, 8};
            if (type == "float3")
                return Primitive{"vec3", 16, 16};
            if (type == "packed_float3")
                return Primitive{"vec3", 12, 4};
            if (type == "float4")
                return Primitive{"vec4", 16, 16};
            return std::nullopt;
        }

        std::uint32_t Align(std::uint32_t value, std::uint32_t alignment) {
            return (value + alignment - 1) / alignment * alignment;
        }

        std::uint32_t FieldCount(Tokens declaration) {
            if (declaration.size() == 3)
                return 1;
            if (declaration[2] != "[" || declaration[4] != "]")
                throw InvalidEvidence{};
            const auto count = Literal(declaration[3], 6);
            if (!count)
                throw InvalidEvidence{};
            return count;
        }

        Field ReadField(Tokens declaration, Layout &layout) {
            if (declaration.size() != 3 && declaration.size() != 6)
                throw InvalidEvidence{};
            const auto type = Type(declaration[0]);
            if (!type || !Identifier(declaration[1]) || declaration.back() != ";")
                throw InvalidEvidence{};
            const auto count = FieldCount(declaration);
            layout.alignment = std::max(layout.alignment, type->alignment);
            const auto offset = Align(layout.bytes, type->alignment);
            const auto stride = Align(type->bytes, type->alignment);
            layout.bytes = offset + stride * count;
            return {std::string{declaration[1]}, std::string{type->json}, offset, count, count > 1 ? stride : 0};
        }

        Layout Element(Tokens tokens, std::string_view name) {
            if (const auto type = Type(name))
                return {type->bytes, type->alignment, {}, std::string{type->json}};
            const auto body = StructureBody(tokens, name);
            Layout layout;
            for (std::size_t at = 0; at < body.size();) {
                const auto remaining = body.subspan(at);
                const auto end = std::ranges::find(remaining, ";");
                if (end == remaining.end() || layout.fields.size() == 8)
                    throw InvalidEvidence{};
                const auto count = static_cast<std::size_t>(end - remaining.begin()) + 1;
                auto field = ReadField(body.subspan(at, count), layout);
                if (std::ranges::find(layout.fields, field.name, &Field::name) != layout.fields.end())
                    throw InvalidEvidence{};
                layout.fields.push_back(std::move(field));
                at += count;
            }
            if (layout.fields.empty())
                throw InvalidEvidence{};
            layout.bytes = Align(layout.bytes, layout.alignment);
            return layout;
        }

        void LayoutFor(Tokens tokens, std::string_view name, bool reference, Buffer &buffer) {
            if (reference && !Type(name)) {
                const auto body = StructureBody(tokens, name);
                const bool wrapper = body.size() == 6 && body[2] == "[" && body[3] == "1" && body[4] == "]" && body[5] == ";";
                if (wrapper) {
                    if (!Identifier(body[1]))
                        throw InvalidEvidence{};
                    buffer.wrapperMember = body[1];
                    buffer.layout = Element(tokens, body[0]);
                    return;
                }
            }
            buffer.layout = Element(tokens, name);
        }

        std::size_t Close(Tokens tokens, std::size_t open) {
            std::size_t depth = 1;
            for (std::size_t at = open + 1; at < tokens.size(); ++at) {
                if (tokens[at] == "(")
                    ++depth;
                if (tokens[at] == ")" && --depth == 0)
                    return at;
            }
            throw InvalidEvidence{};
        }

        Tokens Entry(Tokens tokens) {
            std::optional<Tokens> entry;
            for (std::size_t at = 0; at + 3 < tokens.size(); ++at) {
                if (tokens[at] != "kernel" || tokens[at + 1] != "void" || tokens[at + 2] != "CullLights" || tokens[at + 3] != "(")
                    continue;
                const auto end = Close(tokens, at + 3);
                if (entry || end + 1 == tokens.size() || tokens[end + 1] != "{")
                    throw InvalidEvidence{};
                entry = tokens.subspan(at + 4, end - at - 4);
            }
            if (!entry)
                throw InvalidEvidence{};
            return *entry;
        }

        std::size_t Attribute(Tokens argument) {
            for (std::size_t at = 0; at + 1 < argument.size(); ++at)
                if (argument[at] == "[" && argument[at + 1] == "[")
                    return at;
            throw InvalidEvidence{};
        }

        bool Builtin(Tokens argument, std::size_t attribute) {
            if (argument[attribute + 2] != "thread_position_in_grid")
                return false;
            if (attribute != 2 || argument[0] != "uint3" || !Identifier(argument[1]))
                throw InvalidEvidence{};
            if (argument.size() != attribute + 5 || argument[attribute + 3] != "]" || argument[attribute + 4] != "]")
                throw InvalidEvidence{};
            return true;
        }

        std::uint32_t BufferSlot(Tokens argument, std::size_t attribute) {
            if (argument.size() != attribute + 8 || argument[attribute + 2] != "buffer")
                throw InvalidEvidence{};
            if (argument[attribute + 3] != "(" || argument[attribute + 5] != ")" || argument[attribute + 6] != "]" ||
                argument[attribute + 7] != "]")
                throw InvalidEvidence{};
            return Literal(argument[attribute + 4], 30);
        }

        Buffer Resource(Tokens tokens, Tokens argument, std::size_t attribute, std::uint32_t slot) {
            Buffer buffer;
            buffer.slot = slot;
            std::size_t at = 0;
            if (argument[at] == "const") {
                buffer.readonly = true;
                ++at;
            }
            buffer.constant = argument[at] == "constant";
            if (!buffer.constant && argument[at] != "device")
                throw InvalidEvidence{};
            buffer.readonly = buffer.readonly || buffer.constant;
            if (attribute != at + 4 || !Identifier(argument[at + 1]) || !Identifier(argument[at + 3]))
                throw InvalidEvidence{};
            if (argument[at + 2] != "*" && argument[at + 2] != "&")
                throw InvalidEvidence{};
            buffer.name = argument[at + 3];
            LayoutFor(tokens, argument[at + 1], argument[at + 2] == "&", buffer);
            return buffer;
        }

        std::optional<Buffer> Argument(Tokens tokens, Tokens argument) {
            const auto attribute = Attribute(argument);
            if (attribute + 4 >= argument.size())
                throw InvalidEvidence{};
            if (Builtin(argument, attribute))
                return std::nullopt;
            return Resource(tokens, argument, attribute, BufferSlot(argument, attribute));
        }

        void Retain(std::vector<Buffer> &buffers, std::optional<Buffer> value) {
            if (!value)
                return;
            if (buffers.size() == 5)
                throw InvalidEvidence{};
            if (std::ranges::any_of(buffers, [&](const auto &buffer) {
                return buffer.slot == value->slot || buffer.name == value->name;
            }))
                throw InvalidEvidence{};
            buffers.push_back(std::move(*value));
        }

        void RequireLayout(const Layout &layout, std::uint32_t bytes, std::initializer_list<Field> fields) {
            if (layout.bytes != bytes || layout.fields.size() != fields.size())
                throw InvalidEvidence{};
            std::size_t index = 0;
            for (const auto &field : fields) {
                const auto &actual = layout.fields[index++];
                if (actual.name != field.name || actual.jsonType != field.jsonType || actual.offset != field.offset ||
                    actual.count != field.count || actual.stride != field.stride)
                    throw InvalidEvidence{};
            }
        }

        void RequireScalar(const Layout &layout, std::uint32_t bytes, std::string_view type) {
            if (layout.bytes != bytes || layout.scalarType != type || !layout.fields.empty())
                throw InvalidEvidence{};
        }

        void RequireCanonicalLayouts(const std::array<Buffer, 5> &buffers) {
            RequireLayout(buffers[0].layout, 64,
                          {{"positionRange", "vec4", 0},
                           {"direction", "vec3", 16},
                           {"kind", "uint", 28},
                           {"colorIntensity", "vec4", 32},
                           {"cones", "vec2", 48},
                           {"identity", "uvec2", 56}});
            RequireLayout(buffers[1].layout, 96, {{"planes", "vec4", 0, 6, 16}});
            RequireScalar(buffers[2].layout, 16, "uvec4");
            RequireScalar(buffers[3].layout, 4, "uint");
            RequireLayout(buffers[4].layout, 16,
                          {{"LightCount", "uint", 0},
                           {"ClusterCount", "uint", 4},
                           {"ReferencesPerCluster", "uint", 8},
                           {"Reserved", "uint", 12}});
        }

        std::vector<std::size_t> InputQualifierOffsets(std::string_view source, Tokens parameters) {
            std::vector<std::size_t> offsets;
            for (std::size_t at = 0; at + 3 < parameters.size(); ++at) {
                if (parameters[at] != "device")
                    continue;
                if (parameters[at + 3] != "Lights" && parameters[at + 3] != "Clusters")
                    continue;
                if (at && parameters[at - 1] == "const")
                    continue;
                offsets.push_back(static_cast<std::size_t>(parameters[at].data() - source.data()));
            }
            if (offsets.size() > 2)
                throw InvalidEvidence{};
            return offsets;
        }

        bool AccessMatches(const Buffer &buffer, std::size_t index, bool proof) {
            if (buffer.constant != (index == 4))
                return false;
            if (proof && index < 2)
                return true;
            return buffer.readonly == (index < 2 || index == 4);
        }

        std::vector<Buffer> ReadArguments(Tokens tokens, Tokens parameters) {
            std::vector<Buffer> buffers;
            std::size_t start = 0, depth = 0;
            for (std::size_t at = 0; at < parameters.size(); ++at) {
                if (parameters[at] == "(")
                    ++depth;
                if (parameters[at] == ")") {
                    if (!depth)
                        throw InvalidEvidence{};
                    --depth;
                }
                if (parameters[at] != "," || depth != 0)
                    continue;
                Retain(buffers, Argument(tokens, parameters.subspan(start, at - start)));
                start = at + 1;
            }
            if (depth)
                throw InvalidEvidence{};
            Retain(buffers, Argument(tokens, parameters.subspan(start)));
            return buffers;
        }
    }  // namespace

    std::array<Buffer, 5> ParseNative(std::string_view source, bool provenReadonlyInputs) {
        const auto owned = MetalReflectionCookDetail::Tokens(source);
        const auto buffers = ReadArguments(owned, Entry(owned));
        if (buffers.size() != 5)
            throw InvalidEvidence{};
        std::array<Buffer, 5> result;
        constexpr std::array<std::string_view, 5> names{"Lights", "Clusters", "Membership", "References", "Dispatch"};
        for (std::size_t index = 0; index < names.size(); ++index) {
            const auto found = std::ranges::find_if(buffers, [&](const Buffer &buffer) {
                return index == 4 ? buffer.constant : buffer.name == names[index];
            });
            if (found == buffers.end())
                throw InvalidEvidence{};
            result[index] = *found;
            if (!AccessMatches(*found, index, provenReadonlyInputs))
                throw InvalidEvidence{};
        }
        return result;
    }
}  // namespace Horo::Render::LightCullingMetalCookDetail

namespace Horo::Render {
    Result<std::string> QualifyLightCullingMetalInputs(std::string_view source, std::span<const std::uint8_t> validatedSpirv) {
        using namespace LightCullingMetalCookDetail;
        try {
            if (source.empty() || source.size() > 1024 * 1024)
                throw InvalidEvidence{};
            ProveLightCullingSpirv(validatedSpirv);
            RequireCanonicalLayouts(ParseNative(source, true));
            const auto tokens = MetalReflectionCookDetail::Tokens(source);
            auto offsets = InputQualifierOffsets(source, Entry(tokens));
            std::ranges::sort(offsets, std::greater<>{});
            std::string qualified{source};
            for (const auto offset : offsets)
                qualified.insert(offset, "const ");
            (void)ParseNative(qualified);
            return Result<std::string>::Success(std::move(qualified));
        } catch (const InvalidEvidence &) {
            return Result<std::string>::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
        } catch (const MetalReflectionCookDetail::InvalidEvidence &) {
            return Result<std::string>::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
        } catch (const std::bad_alloc &) {
            return Result<std::string>::Failure(MakeError(ShaderReflectionErrors::AllocationFailed));
        }
    }
}  // namespace Horo::Render
