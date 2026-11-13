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

    enum class StorageElementKind : std::uint8_t {
        PackedLight,
        Membership,
        Reference
    };

    struct StorageElementField final {
        ShaderValueType type;
        std::uint8_t columns;
        std::uint32_t byteOffset;
    };

    struct NativeStorageMember final {
        std::string name;
        ShaderValueType type;
        std::uint8_t columns{};
        std::uint32_t byteOffset{};
    };

    /** @brief Actual selected-entry const-device declaration and its proven element layout. */
    struct NativeStorage final {
        std::string name;
        std::uint32_t binding{};
        std::uint32_t elementStride{};
        bool scalarUint{};
        std::string wrapperMember; /**< Empty for a direct pointer; actual single-array member for a wrapper reference. */
        std::vector<NativeStorageMember> members;
    };

    /** @brief Native declarations, never a map copied from another target's reflection. */
    struct NativeStage final {
        std::optional<NativeUniform> uniform;
        std::vector<NativeResource> textures;
        std::vector<NativeResource> samplers;
        std::vector<ShaderReflectedInterfaceVariable> interfaces;
        std::vector<NativeStorage> storage;
        bool vertexIndex{};      /**< At most one exact selected-vertex uint [[vertex_id]] builtin; no logical input is invented. */
        bool fragmentPosition{}; /**< At most one exact selected-fragment float4 [[position]] builtin; not a logical varying. */
    };

    constexpr std::size_t MaximumMembers = 128;
    constexpr std::uint32_t MaximumArrayElements = 512;
    constexpr std::size_t MaximumNameBytes = 256;
    constexpr std::size_t MaximumStorageBuffers = 4;
    constexpr std::size_t MaximumSampledResources = 8; /**< Independent Metal images/samplers; GL retains one pair. */

    /** @brief Admits an exact ASCII shader symbol; arbitrary expressions are not names. */
    [[nodiscard]] bool Identifier(std::string_view value) noexcept;
    /** @brief Resolves one admitted authored binding without creating a second logical registry. */
    [[nodiscard]] const ShaderReflectionBindingName *FindName(std::span<const ShaderReflectionBindingName> names,
                                                              ShaderBindingId id) noexcept;
    [[nodiscard]] std::span<const StorageElementField> StorageFields(StorageElementKind kind) noexcept;
    [[nodiscard]] std::uint32_t StorageStride(StorageElementKind kind) noexcept;
    [[nodiscard]] std::optional<StorageElementKind> StorageKind(const ShaderManifest &manifest, ShaderBindingId binding) noexcept;

    using TokenView = std::span<const std::string_view>;
    /** @brief Reads one complete bounded unsigned token without narrowing. */
    [[nodiscard]] std::uint32_t Literal(std::string_view token, std::uint32_t maximum);
    /** @brief Requires the exact next token; malformed declarations never become partial evidence. */
    void RequireToken(TokenView tokens, std::size_t &cursor, std::string_view expected);
    /** @brief Finds one exact named native structure body, excluding comments and quoted text. */
    [[nodiscard]] TokenView StructureBody(TokenView tokens, std::string_view name);
    /** @brief Computes packing only for the admitted native float-register declarations. */
    [[nodiscard]] NativeUniform ReadUniformMembers(TokenView body, bool metal);
    [[nodiscard]] NativeStorage ReadMetalStorageArgument(TokenView tokens, TokenView argument, std::size_t attribute,
                                                         std::uint32_t binding);
    void NormalizeRegisterStorage(const Foundation::Json &json, const NativeStage &native, const ShaderManifest &manifest,
                                  std::span<const ShaderReflectionBindingName> names, ShaderStageVisibility visibility,
                                  ShaderReflectionCandidate &candidate);
    /** @brief Captures actual GLSL410 declarations and explicit finite runtime installation slots. */
    [[nodiscard]] NativeStage ReadOpenGLNativeStage(TokenView tokens, ShaderStage stage);
    /** @brief Captures only resources and interface structs attached to the actual selected Metal entry. */
    [[nodiscard]] NativeStage ReadMetalNativeStage(TokenView tokens, ShaderStage stage, std::string_view entryPoint);
    /** @brief Parses actual translated source; rejects unsupported, ambiguous or incomplete declarations. */
    [[nodiscard]] NativeStage ParseRegisterNativeStage(std::string_view source, ShaderStage stage, ShaderTargetBackend backend,
                                                       std::string_view entryPoint);
}  // namespace Horo::Render::RegisterRasterCookDetail
