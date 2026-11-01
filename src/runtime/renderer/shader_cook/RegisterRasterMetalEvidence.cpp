#include "RegisterRasterReflectionInternal.h"

#include <algorithm>

namespace Horo::Render::RegisterRasterCookDetail {
    namespace {
        struct Entry final {
            std::string_view resultType;
            TokenView parameters;
        };

        /** @brief Bounds the actual function signature, including nested native attributes. */
        std::size_t ClosingParenthesis(const TokenView tokens, const std::size_t open) {
            std::size_t depth{1};
            for (std::size_t at = open + 1; at < tokens.size(); ++at) {
                if (tokens[at] == "(")
                    ++depth;
                else if (tokens[at] == ")" && --depth == 0)
                    return at;
            }
            throw InvalidEvidence{};
        }

        /** @brief Identifies one complete selected entry definition; helper arguments cannot supply its evidence. */
        Entry FindEntry(const TokenView tokens, const ShaderStage stage, const std::string_view name) {
            std::optional<Entry> result;
            const auto keyword = stage == ShaderStage::Vertex ? "vertex" : "fragment";
            for (std::size_t at = 0; at + 3 < tokens.size(); ++at) {
                if (tokens[at] != keyword || tokens[at + 2] != name || tokens[at + 3] != "(")
                    continue;
                const auto end = ClosingParenthesis(tokens, at + 3);
                if (result || end + 1 == tokens.size() || tokens[end + 1] != "{")
                    throw InvalidEvidence{};
                result = Entry{tokens[at + 1], tokens.subspan(at + 4, end - at - 4)};
            }
            if (!result)
                throw InvalidEvidence{};
            return *result;
        }

        /** @brief Reads only exact float scalar/vector interface shapes. */
        std::uint8_t VectorWidth(const std::string_view type) {
            if (type == "float")
                return 1;
            if (type.size() != 6 || !type.starts_with("float") || type[5] < '2' || type[5] > '4')
                throw InvalidEvidence{};
            return static_cast<std::uint8_t>(type[5] - '0');
        }

        /** @brief Admits only actual attribute/user/color locations; position is an excluded native builtin. */
        std::optional<std::uint32_t> InterfaceAttribute(const TokenView field) {
            if (field.size() == 8 && field[4] == "position")
                return std::nullopt;
            if (field.size() != 11 || field[5] != "(" || field[7] != ")")
                throw InvalidEvidence{};
            if (field[4] == "attribute" || field[4] == "color")
                return Literal(field[6], 255);
            if (field[4] != "user" || !field[6].starts_with("locn"))
                throw InvalidEvidence{};
            return Literal(field[6].substr(4), 255);
        }

        /** @brief Requires the full native attribute field, not a name occurring inside an unrelated expression. */
        void InterfaceField(const TokenView field, const ShaderStage stage, const ShaderInterfaceDirection direction, NativeStage &result) {
            if (field.size() < 8 || !Identifier(field[1]) || field[2] != "[" || field[3] != "[")
                throw InvalidEvidence{};
            const auto size = field.size();
            if (field[size - 3] != "]" || field[size - 2] != "]" || field[size - 1] != ";")
                throw InvalidEvidence{};
            const auto location = InterfaceAttribute(field);
            if (!location)
                return;
            if (result.interfaces.size() == 256)
                throw InvalidEvidence{};
            result.interfaces.push_back(
                {stage, direction, *location, ShaderValueType::Float32, 1, VectorWidth(field[0]), 1, std::string{field[1]}});
        }

        /** @brief Inspects only the structure attached to the selected stage entry. */
        void Interfaces(const TokenView tokens, const std::string_view type, const ShaderStage stage,
                        const ShaderInterfaceDirection direction, NativeStage &result) {
            if (type == "void")
                return;
            const auto body = StructureBody(tokens, type);
            for (std::size_t at = 0; at < body.size();) {
                const auto remaining = body.subspan(at);
                const auto end = std::ranges::find(remaining, ";");
                if (end == remaining.end())
                    throw InvalidEvidence{};
                const auto count = static_cast<std::size_t>(end - remaining.begin()) + 1;
                InterfaceField(remaining.first(count), stage, direction, result);
                at += count;
            }
        }

        /** @brief Takes a resource index exclusively from the selected entry's exact native argument annotation. */
        std::uint32_t ResourceIndex(const TokenView parameter, std::size_t at) {
            RequireToken(parameter, at, "(");
            if (at == parameter.size())
                throw InvalidEvidence{};
            const auto index = Literal(parameter[at++], 30);
            RequireToken(parameter, at, ")");
            RequireToken(parameter, at, "]");
            RequireToken(parameter, at, "]");
            if (at != parameter.size())
                throw InvalidEvidence{};
            return index;
        }

        /** @brief Proves one exact constant reference and the complete referenced native structure. */
        void Uniform(const TokenView tokens, const TokenView argument, const std::size_t attribute, NativeStage &result) {
            if (result.uniform || attribute != 4 || argument[0] != "constant" || argument[2] != "&")
                throw InvalidEvidence{};
            auto uniform = ReadUniformMembers(StructureBody(tokens, argument[1]), true);
            uniform.name = argument[3];
            uniform.binding = ResourceIndex(argument, attribute + 3);
            result.uniform = std::move(uniform);
        }

        /** @brief Captures an ordinary sampled 2D float image or independent sampler argument. */
        void SampledResource(const TokenView argument, const std::size_t attribute, const bool texture, NativeStage &result) {
            auto &resource = texture ? result.texture : result.sampler;
            if (resource || attribute == 0 || !Identifier(argument[attribute - 1]))
                throw InvalidEvidence{};
            constexpr std::array<std::string_view, 4> image{"texture2d", "<", "float", ">"};
            const bool validType =
                texture ? attribute == 5 && std::ranges::equal(argument.first(4), image) : attribute == 2 && argument[0] == "sampler";
            if (!validType)
                throw InvalidEvidence{};
            resource = NativeResource{std::string{argument[attribute - 1]}, ResourceIndex(argument, attribute + 3)};
        }

        /** @brief Requires the entire stage-in attribute instead of accepting a matching prefix. */
        bool StageInput(const TokenView argument, const std::size_t attribute) {
            return argument.size() == 7 && attribute == 2 && argument[5] == "]" && argument[6] == "]";
        }

        /** @brief Routes one whole actual selected-entry parameter; unknown native roles fail closed. */
        void Parameter(const TokenView tokens, const TokenView argument, const ShaderStage stage, NativeStage &result) {
            const auto bracket = std::ranges::find(argument, "[");
            const auto at = static_cast<std::size_t>(bracket - argument.begin());
            if (at + 3 >= argument.size() || argument[at + 1] != "[")
                throw InvalidEvidence{};
            const auto kind = argument[at + 2];
            if (kind == "buffer")
                Uniform(tokens, argument, at, result);
            else if (kind == "texture" || kind == "sampler")
                SampledResource(argument, at, kind == "texture", result);
            else if (kind == "stage_in" && StageInput(argument, at))
                Interfaces(tokens, argument[0], stage, ShaderInterfaceDirection::Input, result);
            else
                throw InvalidEvidence{};
        }

        /** @brief Splits finite resource arguments without treating commas inside annotations as separators. */
        void Parameters(const TokenView tokens, const TokenView parameters, const ShaderStage stage, NativeStage &result) {
            std::size_t start{}, depth{};
            for (std::size_t at = 0; at < parameters.size(); ++at) {
                if (parameters[at] == "(")
                    ++depth;
                else if (parameters[at] == ")")
                    --depth;
                else if (parameters[at] == "," && depth == 0) {
                    Parameter(tokens, parameters.subspan(start, at - start), stage, result);
                    start = at + 1;
                }
            }
            if (start < parameters.size())
                Parameter(tokens, parameters.subspan(start), stage, result);
        }
    }  // namespace

    /** @copydoc ReadMetalNativeStage */
    NativeStage ReadMetalNativeStage(const TokenView tokens, const ShaderStage stage, const std::string_view entryPoint) {
        NativeStage result;
        const auto entry = FindEntry(tokens, stage, entryPoint);
        Interfaces(tokens, entry.resultType, stage, ShaderInterfaceDirection::Output, result);
        Parameters(tokens, entry.parameters, stage, result);
        return result;
    }
}  // namespace Horo::Render::RegisterRasterCookDetail
