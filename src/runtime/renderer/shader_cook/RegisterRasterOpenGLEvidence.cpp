#include "RegisterRasterReflectionInternal.h"

#include <algorithm>

namespace Horo::Render::RegisterRasterCookDetail {
    namespace {
        /** @brief Reads only finite native scalar/vector stage values supported by this profile. */
        std::uint8_t VectorWidth(const std::string_view token) {
            if (token == "float")
                return 1;
            if (token.size() != 4 || !token.starts_with("vec") || token[3] < '2' || token[3] > '4')
                throw InvalidEvidence{};
            return static_cast<std::uint8_t>(token[3] - '0');
        }

        /** @brief Takes a location from an actual GLSL qualifier, never from an authored parameter order. */
        std::optional<std::uint32_t> DeclaredLocation(const TokenView tokens, const std::size_t at) {
            if (at < 6)
                return std::nullopt;
            const auto prefix = tokens.subspan(at - 6, 6);
            if (prefix[0] != "layout" || prefix[1] != "(" || prefix[2] != "location" || prefix[3] != "=" || prefix[5] != ")")
                return std::nullopt;
            return Literal(prefix[4], 255);
        }

        /** @brief Name-based links have locations only after the translator applies our exact location rename. */
        std::uint32_t InterfaceLocation(const TokenView tokens, const std::size_t at, const std::string_view name) {
            if (const auto location = DeclaredLocation(tokens, at); location.has_value())
                return *location;
            constexpr std::string_view prefix = "horo_raster_varying_";
            if (!name.starts_with(prefix))
                throw InvalidEvidence{};
            return Literal(name.substr(prefix.size()), 255);
        }

        /** @brief Captures one complete native stage declaration and its linked name. */
        void Interface(const TokenView tokens, const std::size_t at, const ShaderStage stage, NativeStage &result) {
            if (at + 3 >= tokens.size() || !Identifier(tokens[at + 2]) || tokens[at + 3] != ";")
                throw InvalidEvidence{};
            const auto direction = tokens[at] == "in" ? ShaderInterfaceDirection::Input : ShaderInterfaceDirection::Output;
            const auto name = tokens[at + 2];
            if (result.interfaces.size() == 256)
                throw InvalidEvidence{};
            const auto location = InterfaceLocation(tokens, at, name);
            const auto width = VectorWidth(tokens[at + 1]);
            result.interfaces.emplace_back(stage, direction, location, ShaderValueType::Float32, std::uint8_t{1}, width, std::uint32_t{1},
                                           std::string{name});
        }

        /** @brief Requires the exact std140 qualifier emitted by the admitted no-420pack translator route. */
        void Std140(const TokenView tokens, const std::size_t at) {
            constexpr std::array<std::string_view, 4> expected{"layout", "(", "std140", ")"};
            if (at < expected.size() || !std::ranges::equal(tokens.subspan(at - expected.size(), expected.size()), expected))
                throw InvalidEvidence{};
        }

        /** @brief Requires the full generated block declaration, including its optional instance and terminator. */
        void UniformEnd(const TokenView tokens, std::size_t cursor) {
            if (cursor < tokens.size() && Identifier(tokens[cursor]))
                ++cursor;
            RequireToken(tokens, cursor, ";");
        }

        /** @brief Reads one whole native block and its actual GL block lookup name. */
        std::size_t Uniform(const TokenView tokens, const std::size_t at, NativeStage &result) {
            if (result.uniform || at + 2 >= tokens.size() || !Identifier(tokens[at + 1]))
                throw InvalidEvidence{};
            Std140(tokens, at);
            const auto tail = tokens.subspan(at + 3);
            const auto end = std::ranges::find(tail, "}");
            if (end == tail.end())
                throw InvalidEvidence{};
            UniformEnd(tokens, at + 4 + static_cast<std::size_t>(end - tail.begin()));
            auto uniform = ReadUniformMembers(tail.first(static_cast<std::size_t>(end - tail.begin())), false);
            uniform.name = tokens[at + 1];
            uniform.binding = 0;  // Explicit finite runtime binding installation; not a reflected driver slot.
            result.uniform = std::move(uniform);
            return at + 3 + static_cast<std::size_t>(end - tail.begin());
        }

        /** @brief One native combined sampler proves a paired logical image/sampler, not two independent units. */
        void CombinedSampler(const TokenView tokens, const std::size_t at, NativeStage &result) {
            if (!result.textures.empty() || at + 3 >= tokens.size() || tokens[at + 1] != "sampler2D")
                throw InvalidEvidence{};
            if (!Identifier(tokens[at + 2]) || tokens[at + 3] != ";")
                throw InvalidEvidence{};
            result.textures.emplace_back(std::string{tokens[at + 2]}, std::uint32_t{0});
            result.samplers = result.textures;
        }

        /** @brief Handles global declarations separately from function-body nesting. */
        void GlobalDeclaration(const TokenView tokens, std::size_t &at, const ShaderStage stage, NativeStage &result) {
            if (tokens[at] == "uniform") {
                if (at + 2 < tokens.size() && tokens[at + 2] == "{")
                    at = Uniform(tokens, at, result);
                else
                    CombinedSampler(tokens, at, result);
            } else if (tokens[at] == "in" || tokens[at] == "out") {
                Interface(tokens, at, stage, result);
            }
        }

        /** @brief Requires the admitted source version and one actual native entry definition. */
        void NativeEntry(const TokenView tokens) {
            constexpr std::array<std::string_view, 3> version{"#", "version", "410"};
            constexpr std::array<std::string_view, 5> entry{"void", "main", "(", ")", "{"};
            if (tokens.size() < version.size() || !std::ranges::equal(tokens.first(version.size()), version))
                throw InvalidEvidence{};
            const auto found = std::search(tokens.begin(), tokens.end(), entry.begin(), entry.end());
            if (found == tokens.end())
                throw InvalidEvidence{};
            if (std::search(found + 1, tokens.end(), entry.begin(), entry.end()) != tokens.end())
                throw InvalidEvidence{};
        }
    }  // namespace

    /** @copydoc ReadOpenGLNativeStage */
    NativeStage ReadOpenGLNativeStage(const TokenView tokens, const ShaderStage stage) {
        NativeEntry(tokens);
        NativeStage result;
        std::size_t depth{};
        for (std::size_t at = 0; at < tokens.size(); ++at) {
            if (tokens[at] == "{")
                ++depth;
            else if (tokens[at] == "}") {
                if (depth == 0)
                    throw InvalidEvidence{};
                --depth;
            } else if (depth == 0)
                GlobalDeclaration(tokens, at, stage, result);
        }
        if (depth != 0)
            throw InvalidEvidence{};
        return result;
    }
}  // namespace Horo::Render::RegisterRasterCookDetail
