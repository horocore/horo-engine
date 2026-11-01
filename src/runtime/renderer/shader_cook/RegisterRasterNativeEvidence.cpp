#include "RegisterRasterReflectionInternal.h"

#include <algorithm>
#include <charconv>

namespace Horo::Render::RegisterRasterCookDetail {
    /** @copydoc Literal */
    std::uint32_t Literal(const std::string_view token, const std::uint32_t maximum) {
        std::uint32_t result{};
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), result);
        if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() || result > maximum)
            throw InvalidEvidence{};
        return result;
    }

    /** @copydoc RequireToken */
    void RequireToken(const TokenView tokens, std::size_t &cursor, const std::string_view expected) {
        if (cursor == tokens.size() || tokens[cursor] != expected)
            throw InvalidEvidence{};
        ++cursor;
    }

    /** @copydoc StructureBody */
    TokenView StructureBody(const TokenView tokens, const std::string_view name) {
        std::optional<TokenView> result;
        for (std::size_t at = 0; at + 2 < tokens.size(); ++at) {
            if (tokens[at] != "struct" || tokens[at + 1] != name || tokens[at + 2] != "{")
                continue;
            if (result)
                throw InvalidEvidence{};
            const auto tail = tokens.subspan(at + 3);
            const auto end = std::ranges::find(tail, "}");
            if (end == tail.end())
                throw InvalidEvidence{};
            result = tail.first(static_cast<std::size_t>(end - tail.begin()));
        }
        if (!result)
            throw InvalidEvidence{};
        return *result;
    }

    namespace {
        /** @brief Admits native vec4/mat4 only; vectors with hidden padding are unsupported. */
        NativeMember MemberType(const std::string_view type, const bool metal) {
            NativeMember member;
            if (type == (metal ? "float4" : "vec4"))
                return member;
            if (type != (metal ? "float4x4" : "mat4"))
                throw InvalidEvidence{};
            member.rows = 4;
            member.matrixStride = 16;
            return member;
        }

        /** @brief Requires one positive literal native array extent. */
        void MemberArray(const TokenView body, std::size_t &cursor, NativeMember &member) {
            if (cursor == body.size() || body[cursor] != "[")
                return;
            ++cursor;
            if (cursor == body.size())
                throw InvalidEvidence{};
            member.arrayCount = Literal(body[cursor++], MaximumArrayElements);
            if (member.arrayCount == 0)
                throw InvalidEvidence{};
            RequireToken(body, cursor, "]");
            member.arrayStride = member.rows * 16U;
        }

        /** @brief Captures one complete native field, including the bytes actually implied by its native type. */
        NativeMember ReadMember(const TokenView body, std::size_t &cursor, const bool metal, const std::uint32_t offset) {
            auto member = MemberType(body[cursor++], metal);
            if (cursor == body.size() || !Identifier(body[cursor]))
                throw InvalidEvidence{};
            member.name = body[cursor++];
            member.byteOffset = offset;
            MemberArray(body, cursor, member);
            RequireToken(body, cursor, ";");
            return member;
        }
    }  // namespace

    /** @copydoc ReadUniformMembers */
    NativeUniform ReadUniformMembers(const TokenView body, const bool metal) {
        NativeUniform uniform;
        for (std::size_t cursor = 0; cursor < body.size();) {
            if (uniform.members.size() == MaximumMembers)
                throw InvalidEvidence{};
            auto member = ReadMember(body, cursor, metal, uniform.byteSize);
            if (std::ranges::find(uniform.members, member.name, &NativeMember::name) != uniform.members.end())
                throw InvalidEvidence{};
            uniform.byteSize += member.rows * 16U * member.arrayCount;
            uniform.members.push_back(std::move(member));
        }
        if (uniform.members.empty())
            throw InvalidEvidence{};
        return uniform;
    }

    /** @copydoc ParseRegisterNativeStage */
    NativeStage ParseRegisterNativeStage(const std::string_view source, const ShaderStage stage, const ShaderTargetBackend backend,
                                         const std::string_view entryPoint) {
        try {
            const auto tokens = MetalReflectionCookDetail::Tokens(source);
            if (backend == ShaderTargetBackend::OpenGL)
                return ReadOpenGLNativeStage(tokens, stage);
            if (backend == ShaderTargetBackend::Metal)
                return ReadMetalNativeStage(tokens, stage, entryPoint);
            throw InvalidEvidence{};
        } catch (const MetalReflectionCookDetail::InvalidEvidence &) {
            throw InvalidEvidence{};
        }
    }
}  // namespace Horo::Render::RegisterRasterCookDetail
