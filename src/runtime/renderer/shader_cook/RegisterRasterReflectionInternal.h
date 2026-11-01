#pragma once

/** @file RegisterRasterReflectionInternal.h
 * @brief Bounded native evidence for a portable float-register raster interface.
 */
#include "MetalReflectionEvidence.h"
#include "ShaderReflectionCookJson.h"

#include <array>
#include <optional>

namespace Horo::Render::RegisterRasterCookDetail {
    struct InvalidEvidence {};

    /** @brief One field actually declared by the translator, before any logical-name association. */
    struct NativeMember final {
        std::string name;
        std::uint8_t rows{1};
        std::uint8_t columns{4};
        std::uint32_t arrayCount{1};
        std::uint32_t byteOffset{};
        std::uint32_t arrayStride{};
        std::uint32_t matrixStride{};
    };

    struct NativeUniform final {
        std::string name;
        std::uint32_t binding{};
        std::uint32_t byteSize{};
        std::vector<NativeMember> members;
    };

    struct NativeResource final {
        std::string name;
        std::uint32_t binding{};
    };

    /** @brief Native declarations, never a map copied from another target's reflection. */
    struct NativeStage final {
        std::optional<NativeUniform> uniform;
        std::optional<NativeResource> texture;
        std::optional<NativeResource> sampler;
        std::vector<ShaderReflectedInterfaceVariable> interfaces;
    };

    constexpr std::size_t MaximumMembers = 128;
    constexpr std::uint32_t MaximumArrayElements = 512;
    constexpr std::size_t MaximumNameBytes = 256;

    /** @brief Admits an exact ASCII shader symbol; arbitrary expressions are not names. */
    [[nodiscard]] bool Identifier(std::string_view value) noexcept;
    /** @brief Resolves one admitted authored binding without creating a second logical registry. */
    [[nodiscard]] const ShaderReflectionBindingName *FindName(std::span<const ShaderReflectionBindingName> names,
                                                              ShaderBindingId id) noexcept;

    using TokenView = std::span<const std::string_view>;
    /** @brief Reads one complete bounded unsigned token without narrowing. */
    [[nodiscard]] std::uint32_t Literal(std::string_view token, std::uint32_t maximum);
    /** @brief Requires the exact next token; malformed declarations never become partial evidence. */
    void RequireToken(TokenView tokens, std::size_t &cursor, std::string_view expected);
    /** @brief Finds one exact named native structure body, excluding comments and quoted text. */
    [[nodiscard]] TokenView StructureBody(TokenView tokens, std::string_view name);
    /** @brief Computes packing only for the admitted native float-register declarations. */
    [[nodiscard]] NativeUniform ReadUniformMembers(TokenView body, bool metal);
    /** @brief Captures actual GLSL410 declarations and explicit finite runtime installation slots. */
    [[nodiscard]] NativeStage ReadOpenGLNativeStage(TokenView tokens, ShaderStage stage);
    /** @brief Captures only resources and interface structs attached to the actual selected Metal entry. */
    [[nodiscard]] NativeStage ReadMetalNativeStage(TokenView tokens, ShaderStage stage, std::string_view entryPoint);
    /** @brief Parses actual translated source; rejects unsupported, ambiguous or incomplete declarations. */
    [[nodiscard]] NativeStage ParseRegisterNativeStage(std::string_view source, ShaderStage stage, ShaderTargetBackend backend,
                                                       std::string_view entryPoint);
}  // namespace Horo::Render::RegisterRasterCookDetail
