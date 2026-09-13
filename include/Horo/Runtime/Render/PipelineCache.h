#pragma once

/**
 * @file PipelineCache.h
 * @brief Backend-neutral native pipeline-cache identity and safe serialized-blob admission.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/Runtime/Render/RenderAdapter.h"
#include "Horo/Runtime/Render/RenderBackend.h"
#include "Horo/Runtime/Render/ShaderManifest.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Horo::Render {
    /** @brief Complete non-native identity controlling reuse of one backend pipeline-cache blob. */
    struct PipelineCacheCompatibility final {
        std::uint32_t schemaVersion{0};                                           /**< Version of this compatibility contract. */
        RenderBackendId backend;                                                  /**< Exact Horo backend implementation identity. */
        std::uint64_t backendModuleVersion{0};                                    /**< Non-zero backend implementation/cache ABI version. */
        RenderAdapterId adapter;                                                  /**< Stable backend-scoped device identity. */
        Sha256Digest deviceCompatibilityDigest;                                   /**< Backend-derived native cache compatibility facts. */
        std::string driverIdentity;                                               /**< Canonical backend-reported driver identity. */
        std::string driverVersion;                                                /**< Canonical backend-reported driver version. */
        ShaderTargetBackend shaderBackend{ShaderTargetBackend::Null};             /**< Exact cooked shader target. */
        ShaderPayloadFormat shaderFormat{ShaderPayloadFormat::ValidationFixture}; /**< Exact cooked payload format. */
        Sha256Digest shaderArtifactKey;                                           /**< Complete cooked shader artifact identity. */
        ShaderInterfaceCompatibilityId shaderInterface;                           /**< Logical shader interface identity. */
        Sha256Digest pipelineDescriptorDigest; /**< Canonical fixed-function/layout descriptor identity. */

        [[nodiscard]] auto operator<=>(const PipelineCacheCompatibility &) const noexcept = default;
    };

    /** @brief Canonical digest used to address and validate a native pipeline-cache blob. */
    struct PipelineCacheKey final {
        Sha256Digest digest;
        [[nodiscard]] auto operator<=>(const PipelineCacheKey &) const noexcept = default;
    };

    /** @brief Finite serialized cache bounds enforced before allocation or hashing. */
    struct PipelineCacheLimits final {
        static constexpr std::size_t HardMaximumIdentityBytes = 1'024;
        static constexpr std::size_t HardMaximumPayloadBytes = 512U * 1024U * 1024U;

        std::size_t maximumIdentityBytes{256};
        std::size_t maximumPayloadBytes{128U * 1024U * 1024U};

        /** @brief Reports whether both limits are non-zero and within hard maxima. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return maximumIdentityBytes > 0 && maximumIdentityBytes <= HardMaximumIdentityBytes && maximumPayloadBytes > 0 &&
                   maximumPayloadBytes <= HardMaximumPayloadBytes;
        }
    };

    /**
     * @brief Computes the complete native cache compatibility key.
     * @param compatibility Exact backend, device, driver, shader, and pipeline descriptor identity.
     * @param limits Finite identity and payload bounds.
     * @return Canonical key, or a typed validation/allocation failure.
     */
    [[nodiscard]] Result<PipelineCacheKey> ComputePipelineCacheKey(const PipelineCacheCompatibility &compatibility,
                                                                   const PipelineCacheLimits &limits = {});

    /**
     * @brief Serializes one optional backend-native acceleration blob without exposing a native handle.
     * @param key Complete compatibility key produced by ComputePipelineCacheKey.
     * @param payload Opaque backend-owned bytes; never the only recoverable shader source.
     * @param limits Finite identity and payload bounds.
     * @return Versioned integrity-protected bytes, or a typed validation/allocation failure.
     */
    [[nodiscard]] Result<std::vector<std::uint8_t>> SerializePipelineCacheBlob(const PipelineCacheKey &key,
                                                                               std::span<const std::uint8_t> payload,
                                                                               const PipelineCacheLimits &limits = {});

    /**
     * @brief Validates and loads a cache blob only for the exact current compatibility key.
     * @details A mismatch or malformed blob fails closed. The caller may rebuild from the cooked shader artifact at a permitted
     * preparation boundary; this function never creates a pipeline, changes backend state, or silently substitutes another cache.
     * @param expectedKey Key for the active backend/device/driver/shader/pipeline combination.
     * @param serialized Untrusted serialized cache bytes.
     * @param limits Finite identity and payload bounds.
     * @return Owned opaque native payload, or a typed stale/corrupt/version/allocation failure.
     */
    [[nodiscard]] Result<std::vector<std::uint8_t>> LoadPipelineCacheBlob(const PipelineCacheKey &expectedKey,
                                                                          std::span<const std::uint8_t> serialized,
                                                                          const PipelineCacheLimits &limits = {});
}  // namespace Horo::Render
