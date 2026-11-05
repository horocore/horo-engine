#include "ExposureGpuReflectionInternal.h"
#include "Horo/Runtime/Render/ShaderReflectionErrors.h"

#include <algorithm>

namespace Horo::Render::ExposureGpuCookDetail {
    namespace {
        using RegisterRasterCookDetail::Identifier;
        using RegisterRasterCookDetail::Literal;
        using RegisterRasterCookDetail::StructureBody;

        struct Primitive {
            std::string_view json;
            std::uint32_t bytes, alignment;
        };

        /** @brief Recognizes only primitive scalar and vector ABI elements. */
        std::optional<Primitive> Type(std::string_view type) {
            if (type == "float")
                return Primitive{"float", 4, 4};
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

        /** @brief Computes alignment inside the finite admitted native layouts. */
        std::uint32_t Align(std::uint32_t value, std::uint32_t alignment) {
            return (value + alignment - 1) / alignment * alignment;
        }

        /** @brief Reads a complete fixed-size native member array declaration. */
        std::uint32_t FieldCount(Tokens declaration) {
            if (declaration.size() == 3)
                return 1;
            if (declaration[2] != "[" || declaration[4] != "]")
                throw EvidenceFailure{__func__, __LINE__};
            const auto count = Literal(declaration[3], 6);
            if (!count)
                throw EvidenceFailure{__func__, __LINE__};
            return count;
        }

        /** @brief Captures the actual declaration type and aligned member offset. */
        Field ReadField(Tokens declaration, Layout &layout) {
            if (declaration.size() != 3 && declaration.size() != 6)
                throw EvidenceFailure{__func__, __LINE__};
            const auto type = Type(declaration[0]);
            if (!type || !Identifier(declaration[1]) || declaration.back() != ";")
                throw EvidenceFailure{__func__, __LINE__};
            const auto count = FieldCount(declaration);
            layout.alignment = std::max(layout.alignment, type->alignment);
            const auto offset = Align(layout.bytes, type->alignment);
            const auto stride = Align(type->bytes, type->alignment);
            layout.bytes = offset + stride * count;
            return {std::string{declaration[1]}, std::string{type->json}, offset, count, count > 1 ? stride : 0};
        }

        /** @brief Computes actual native element layout from exact declared fields. */
        Layout Element(Tokens tokens, std::string_view name) {
            if (const auto type = Type(name))
                return {type->bytes, type->alignment, {}, std::string{type->json}};
            const auto body = StructureBody(tokens, name);
            Layout layout;
            for (std::size_t at = 0; at < body.size();) {
                const auto end = std::ranges::find(body.subspan(at), ";");
                if (end == body.end() || layout.fields.size() == 8)
                    throw EvidenceFailure{__func__, __LINE__};
                const auto count = static_cast<std::size_t>(end - body.begin()) + 1 - at;
                auto field = ReadField(body.subspan(at, count), layout);
                if (std::ranges::find(layout.fields, field.name, &Field::name) != layout.fields.end())
                    throw EvidenceFailure{__func__, __LINE__};
                layout.fields.push_back(std::move(field));
                at += count;
            }
            if (layout.fields.empty())
                throw EvidenceFailure{__func__, __LINE__};
            layout.bytes = Align(layout.bytes, layout.alignment);
            return layout;
        }

        /** @brief Unwraps one actual structured-buffer runtime-array wrapper. */
        void LayoutFor(Tokens tokens, std::string_view name, bool reference, Buffer &buffer) {
            if (reference && !Type(name)) {
                const auto body = StructureBody(tokens, name);
                const bool wrapper = body.size() == 6 && body[2] == "[" && body[3] == "1" && body[4] == "]" && body[5] == ";";
                if (wrapper) {
                    if (!Identifier(body[1]))
                        throw EvidenceFailure{__func__, __LINE__};
                    buffer.wrapperMember = body[1];
                    buffer.layout = Element(tokens, body[0]);
                    return;
                }
            }
            buffer.layout = Element(tokens, name);
        }

        /** @brief Finds the balanced selected-entry parameter boundary. */
        std::size_t Close(Tokens tokens, std::size_t open) {
            std::size_t depth = 1;
            for (std::size_t at = open + 1; at < tokens.size(); ++at) {
                if (tokens[at] == "(")
                    ++depth;
                if (tokens[at] == ")" && --depth == 0)
                    return at;
            }
            throw EvidenceFailure{__func__, __LINE__};
        }

        /** @brief Selects exactly one real canonical kernel definition. */
        Tokens Entry(Tokens tokens, ExposureGpuStage stage) {
            std::optional<Tokens> entry;
            for (std::size_t at = 0; at + 3 < tokens.size(); ++at) {
                if (tokens[at] != "kernel" || tokens[at + 1] != "void" || tokens[at + 2] != EntryName(stage) || tokens[at + 3] != "(")
                    continue;
                const auto end = Close(tokens, at + 3);
                if (entry || end + 1 == tokens.size() || tokens[end + 1] != "{")
                    throw EvidenceFailure{__func__, __LINE__};
                entry = tokens.subspan(at + 4, end - at - 4);
            }
            if (!entry)
                throw EvidenceFailure{__func__, __LINE__};
            return *entry;
        }

        /** @brief Locates the complete argument attribute boundary. */
        std::size_t Attribute(Tokens argument) {
            for (std::size_t at = 0; at + 1 < argument.size(); ++at)
                if (argument[at] == "[" && argument[at + 1] == "[")
                    return at;
            throw EvidenceFailure{__func__, __LINE__};
        }

        /** @brief Accepts only the closed invocation builtins emitted for these compute entries. */
        bool Builtin(Tokens argument, std::size_t attribute) {
            const auto name = argument[attribute + 2];
            const bool grid = name == "thread_position_in_grid";
            if (!grid && name != "thread_index_in_threadgroup")
                return false;
            if (attribute != 2 || !Identifier(argument[1]) || argument[0] != (grid ? "uint3" : "uint"))
                throw EvidenceFailure{__func__, __LINE__};
            if (argument.size() != 7 || argument[5] != "]" || argument[6] != "]")
                throw EvidenceFailure{__func__, __LINE__};
            return true;
        }

        /** @brief Reads the exact explicit native buffer register. */
        std::uint32_t BufferSlot(Tokens argument, std::size_t attribute) {
            if (argument.size() != attribute + 8 || argument[attribute + 2] != "buffer")
                throw EvidenceFailure{__func__, __LINE__};
            if (argument[attribute + 3] != "(" || argument[attribute + 5] != ")" || argument[attribute + 6] != "]" ||
                argument[attribute + 7] != "]")
                throw EvidenceFailure{__func__, __LINE__};
            return Literal(argument[attribute + 4], 30);
        }

        /** @brief Captures actual native address space, access and pointee layout. */
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
                throw EvidenceFailure{__func__, __LINE__};
            buffer.readonly = buffer.readonly || buffer.constant;
            if (attribute != at + 4 || !Identifier(argument[at + 1]) || !Identifier(argument[at + 3]))
                throw EvidenceFailure{__func__, __LINE__};
            if (argument[at + 2] != "*" && argument[at + 2] != "&")
                throw EvidenceFailure{__func__, __LINE__};
            buffer.name = argument[at + 3];
            LayoutFor(tokens, argument[at + 1], argument[at + 2] == "&", buffer);
            return buffer;
        }

        /** @brief Requires a real float2D readonly declaration, including Metal's default sample access. */
        void TextureType(Tokens type) {
            constexpr std::array<std::string_view, 4> simple{"texture2d", "<", "float", ">"};
            if (std::ranges::equal(type, simple))
                return;
            if (type.size() != 9)
                throw EvidenceFailure{__func__, __LINE__};
            constexpr std::array<std::string_view, 7> prefix{"texture2d", "<", "float", ",", "access", ":", ":"};
            if (!std::ranges::equal(type.first(7), prefix) || type[8] != ">")
                throw EvidenceFailure{__func__, __LINE__};
            if (type[7] != "read" && type[7] != "sample")
                throw EvidenceFailure{__func__, __LINE__};
        }

        /** @brief Parses an entire explicit native texture attribute without accepting a declaration suffix. */
        std::uint32_t TextureSlot(Tokens attribute) {
            if (attribute.size() != 8 || attribute[0] != "[" || attribute[1] != "[" || attribute[2] != "texture")
                throw EvidenceFailure{__func__, __LINE__};
            if (attribute[3] != "(" || attribute[5] != ")" || attribute[6] != "]" || attribute[7] != "]")
                throw EvidenceFailure{__func__, __LINE__};
            return Literal(attribute[4], 30);
        }

        /** @brief Captures exact default/sample/read float2D resources; storage textures remain inadmissible. */
        Buffer TextureArgument(Tokens argument, std::size_t attribute) {
            if (!attribute || !Identifier(argument[attribute - 1]))
                throw EvidenceFailure{__func__, __LINE__};
            TextureType(argument.first(attribute - 1));
            Buffer buffer;
            buffer.texture = true;
            buffer.readonly = true;
            buffer.name = argument[attribute - 1];
            buffer.slot = TextureSlot(argument.subspan(attribute));
            return buffer;
        }

        /** @brief Rejects unrecognized arguments rather than treating them as inactive resources. */
        std::optional<Buffer> Argument(Tokens tokens, Tokens argument) {
            const auto attribute = Attribute(argument);
            if (attribute + 4 >= argument.size())
                throw EvidenceFailure{__func__, __LINE__};
            if (Builtin(argument, attribute))
                return std::nullopt;
            if (argument[attribute + 2] == "texture")
                return TextureArgument(argument, attribute);
            return Resource(tokens, argument, attribute, BufferSlot(argument, attribute));
        }

        /** @brief Rejects duplicate argument names and resource slots. */
        void Retain(std::vector<Buffer> &buffers, std::optional<Buffer> value) {
            if (!value)
                return;
            if (buffers.size() == 5)
                throw EvidenceFailure{__func__, __LINE__};
            if (std::ranges::any_of(buffers, [&](const auto &buffer) {
                return (buffer.texture == value->texture && buffer.slot == value->slot) || buffer.name == value->name;
            }))
                throw EvidenceFailure{__func__, __LINE__};
            buffers.push_back(std::move(*value));
        }

        /** @brief Requires every actual member to match the closed native ABI. */
        void RequireLayout(const Layout &layout, std::uint32_t bytes, std::initializer_list<Field> fields) {
            if (layout.bytes != bytes || layout.fields.size() != fields.size())
                throw EvidenceFailure{__func__, __LINE__};
            std::size_t index = 0;
            for (const auto &field : fields) {
                const auto &actual = layout.fields[index++];
                if (actual.name != field.name || actual.jsonType != field.jsonType || actual.offset != field.offset ||
                    actual.count != field.count || actual.stride != field.stride)
                    throw EvidenceFailure{__func__, __LINE__};
            }
        }

        /** @brief Requires the exact scalar structured-buffer element. */
        void RequireScalar(const Layout &layout, std::uint32_t bytes, std::string_view type) {
            if (layout.bytes != bytes || layout.scalarType != type || !layout.fields.empty())
                throw EvidenceFailure{__func__, __LINE__};
        }

        /** @brief Proves actual storage element and constant-buffer fields before association. */
        void RequireCanonicalLayout(const Buffer &buffer, std::size_t index) {
            if (index == 0)
                return;
            if (index == 1) {
                RequireScalar(buffer.layout, 4, "uint");
                return;
            }
            if (index == 4) {
                RequireLayout(buffer.layout, 96,
                              {{"Region", "uvec4", 0},
                               {"Limits", "vec4", 16},
                               {"Adaptation", "vec4", 32},
                               {"Timing", "vec4", 48},
                               {"Controls", "uvec4", 64},
                               {"Identity", "uvec4", 80}});
                return;
            }
            RequireLayout(buffer.layout, 32,
                          {{"exposureEv", "float", 0},
                           {"exposureScale", "float", 4},
                           {"validSamples", "uint", 8},
                           {"reason", "uint", 12},
                           {"generation", "uvec2", 16},
                           {"sourceSubmission", "uvec2", 24}});
        }

        /** @brief Locates only selected predecessor argument address qualifiers. */
        std::vector<std::size_t> InputQualifierOffsets(std::string_view source, Tokens parameters) {
            std::vector<std::size_t> offsets;
            for (std::size_t at = 0; at + 3 < parameters.size(); ++at) {
                if (parameters[at] != "device")
                    continue;
                if (parameters[at + 3] != "PreviousExposure")
                    continue;
                if (at && parameters[at - 1] == "const")
                    continue;
                offsets.push_back(static_cast<std::size_t>(parameters[at].data() - source.data()));
            }
            if (offsets.size() > 1)
                throw EvidenceFailure{__func__, __LINE__};
            return offsets;
        }

        /** @brief Checks actual access against the closed resource role. */
        bool AccessMatches(const Buffer &buffer, std::size_t index, bool proof) {
            if (buffer.constant != (index == 4))
                return false;
            if (proof && index == 2)
                return true;
            return buffer.readonly == (index == 0 || index == 2 || index == 4);
        }

        /** @brief Splits exact arguments while respecting annotation and texture-template nesting. */
        std::vector<Buffer> ReadArguments(Tokens tokens, Tokens parameters) {
            std::vector<Buffer> buffers;
            std::size_t start = 0, depth = 0;
            for (std::size_t at = 0; at < parameters.size(); ++at) {
                if (parameters[at] == "(" || parameters[at] == "<")
                    ++depth;
                if (parameters[at] == ")" || parameters[at] == ">") {
                    if (!depth)
                        throw EvidenceFailure{__func__, __LINE__};
                    --depth;
                }
                if (parameters[at] != "," || depth != 0)
                    continue;
                Retain(buffers, Argument(tokens, parameters.subspan(start, at - start)));
                start = at + 1;
            }
            if (depth)
                throw EvidenceFailure{__func__, __LINE__};
            Retain(buffers, Argument(tokens, parameters.subspan(start)));
            return buffers;
        }
    }  // namespace

    /** @copydoc ParseNative */
    std::array<std::optional<Buffer>, 5> ParseNative(std::string_view source, ExposureGpuStage stage, bool provenReadonlyInputs) {
        const auto owned = MetalReflectionCookDetail::Tokens(source);
        const auto buffers = ReadArguments(owned, Entry(owned, stage));
        const auto names = ExposureGpuMetalReflectionNames(stage);
        if (buffers.size() != names.size())
            throw EvidenceFailure{__func__, __LINE__};
        std::array<std::optional<Buffer>, 5> result;
        for (const auto &name : names) {
            const auto index = name.id.value - 1;
            const auto found = std::ranges::find_if(buffers, [&](const Buffer &buffer) {
                return index == 4 ? buffer.constant : buffer.name == name.name;
            });
            if (found == buffers.end() || !AccessMatches(*found, index, provenReadonlyInputs) || found->texture != (index == 0))
                throw EvidenceFailure{__func__, __LINE__};
            if (found->slot != index)
                throw EvidenceFailure{__func__, __LINE__};
            RequireCanonicalLayout(*found, index);
            result[index] = *found;
        }
        return result;
    }
}  // namespace Horo::Render::ExposureGpuCookDetail

namespace Horo::Render {
    /** @copydoc QualifyExposureGpuMetalInputs */
    Result<std::string> QualifyExposureGpuMetalInputs(const ShaderManifest &manifest, std::string_view source,
                                                      std::span<const std::uint8_t> validatedSpirv) {
        using namespace ExposureGpuCookDetail;
        try {
            if (source.empty() || source.size() > 1024 * 1024)
                throw EvidenceFailure{__func__, __LINE__};
            const auto stage = Stage(manifest);
            (void)ParseNative(source, stage, true);
            // Qualification proves access only; final admission separately requires actual Cross JSON as well.
            ProveSpirv(validatedSpirv, stage);
            const auto tokens = MetalReflectionCookDetail::Tokens(source);
            auto offsets = InputQualifierOffsets(source, Entry(tokens, stage));
            std::ranges::sort(offsets, std::greater<>{});
            std::string qualified{source};
            for (const auto offset : offsets)
                qualified.insert(offset, "const ");
            (void)ParseNative(qualified, stage);
            return Result<std::string>::Success(std::move(qualified));
        } catch (const EvidenceFailure &failure) {
            return Result<std::string>::Failure(MakeError(ShaderReflectionErrors::InvalidReflection, EvidenceMessage(failure)));
        } catch (const InvalidEvidence &) {
            return Result<std::string>::Failure(
                MakeError(ShaderReflectionErrors::InvalidReflection, "Exposure shared structure/layout parser rejected evidence."));
        } catch (const MetalReflectionCookDetail::InvalidEvidence &) {
            return Result<std::string>::Failure(
                MakeError(ShaderReflectionErrors::InvalidReflection, "Exposure native lexical tokenizer rejected evidence."));
        } catch (const std::bad_alloc &) {
            return Result<std::string>::Failure(MakeError(ShaderReflectionErrors::AllocationFailed));
        }
    }
}  // namespace Horo::Render
