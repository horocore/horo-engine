#include "RegisterRasterReflectionInternal.h"

#include <algorithm>

namespace Horo::Render::RegisterRasterCookDetail {
    namespace {
        struct NativeType final {
            ShaderValueType type;
            std::uint8_t columns;
            std::uint32_t size;
            std::uint32_t alignment;
        };

        NativeType Type(std::string_view token) {
            using enum Horo::Render::ShaderValueType;
            if (token == "uint")
                return {Uint32, 1, 4, 4};
            if (token == "float2")
                return {Float32, 2, 8, 8};
            if (token == "float3")
                return {Float32, 3, 16, 16};
            if (token == "packed_float3")
                return {Float32, 3, 12, 4};
            if (token == "float4")
                return {Float32, 4, 16, 16};
            throw InvalidEvidence{};
        }

        std::uint32_t Align(std::uint32_t value, std::uint32_t alignment) noexcept {
            return ((value + alignment - 1U) / alignment) * alignment;
        }

        void Element(TokenView tokens, std::string_view type, NativeStorage &storage) {
            if (type == "uint") {
                storage.scalarUint = true;
                storage.elementStride = 4;
                return;
            }
            const auto body = StructureBody(tokens, type);
            std::uint32_t offset{};
            std::uint32_t alignment = 1;
            for (std::size_t at = 0; at < body.size(); at += 3) {
                if (body.size() - at < 3 || storage.members.size() == MaximumMembers)
                    throw InvalidEvidence{};
                const auto field = Type(body[at]);
                if (!Identifier(body[at + 1]) || body[at + 2] != ";")
                    throw InvalidEvidence{};
                if (std::ranges::find(storage.members, body[at + 1], &NativeStorageMember::name) != storage.members.end())
                    throw InvalidEvidence{};
                offset = Align(offset, field.alignment);
                alignment = std::max(alignment, field.alignment);
                storage.members.emplace_back(std::string{body[at + 1]}, field.type, field.columns, offset);
                offset += field.size;
            }
            if (storage.members.empty())
                throw InvalidEvidence{};
            storage.elementStride = Align(offset, alignment);
        }

        std::string_view Wrapper(TokenView tokens, std::string_view type, NativeStorage &storage) {
            const auto body = StructureBody(tokens, type);
            if (body.size() != 6 || !Identifier(body[0]) || !Identifier(body[1]))
                throw InvalidEvidence{};
            if (body[2] != "[" || body[3] != "1" || body[4] != "]" || body[5] != ";")
                throw InvalidEvidence{};
            storage.wrapperMember = body[1];
            return body[0];
        }
    }  // namespace

    NativeStorage ReadMetalStorageArgument(TokenView tokens, TokenView argument, std::size_t attribute, std::uint32_t binding) {
        if (attribute != 5 || argument[0] != "const" || argument[1] != "device")
            throw InvalidEvidence{};
        if (!Identifier(argument[2]) || !Identifier(argument[4]))
            throw InvalidEvidence{};
        NativeStorage storage{.name = std::string{argument[4]}, .binding = binding};
        std::string_view element = argument[2];
        if (argument[3] == "&")
            element = Wrapper(tokens, element, storage);
        else if (argument[3] != "*")
            throw InvalidEvidence{};
        Element(tokens, element, storage);
        return storage;
    }
}  // namespace Horo::Render::RegisterRasterCookDetail
