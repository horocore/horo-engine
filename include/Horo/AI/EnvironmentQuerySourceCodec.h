#pragma once
/** @file EnvironmentQuerySourceCodec.h
 * @brief Bounded authored EQS transport for the existing typed query model.
 */

#include "Horo/AI/EnvironmentQuerySchema.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace Horo::AI {
    /** @brief Asset Registry type identity for an authored EQS source with a .horo_eqs file extension. */
    inline constexpr std::string_view EnvironmentQuerySourceAssetType = "core.environment_query";
    inline constexpr std::string_view EnvironmentQuerySourceExtension = ".horo_eqs"; /**< Native authored query source suffix. */

    /** @brief Canonical versioned source owned independently from project sidecars and cooked plans. */
    class EnvironmentQuerySourceCodec final {
    public:
        static constexpr std::size_t MaximumBytes = 512U * 1024U;

        /** @brief Encodes an admitted source in stable execution and property identity order.
         * @param asset Typed immutable authored query, including non-executable unknown stages.
         * @return Complete versioned bytes or a typed representation/limit error.
         */
        [[nodiscard]] static Result<std::vector<std::uint8_t>> Encode(const EnvironmentQueryAsset &asset);

        /** @brief Validates an entire current source without resolving or executing stage descriptors.
         * @param bytes Complete authored source within @ref MaximumBytes.
         * @return Owned validated source or an existing query schema, version or limit error.
         */
        [[nodiscard]] static Result<EnvironmentQueryAsset> Decode(std::span<const std::uint8_t> bytes);
    };

    /** @brief Versioned execution artifact with no editor labels and an exact admitted descriptor fingerprint. */
    class EnvironmentQueryCookedCodec final {
    public:
        static constexpr std::size_t MaximumBytes = EnvironmentQuerySourceCodec::MaximumBytes + 64U;

        /** @brief Compiles and encodes the query with its provider and context assumptions.
         * @param asset Authoring source. @param registry Target host descriptor snapshot.
         * @return Immutable artifact payload or a typed admission or storage error.
         */
        [[nodiscard]] static Result<std::vector<std::uint8_t>> Encode(const EnvironmentQueryAsset &asset,
                                                                      const QuerySchemaRegistry &registry);

        /** @brief Reconstructs an executable plan and verifies the admitted descriptor fingerprint.
         * @param bytes Stored payload. @param registry Exact host descriptor snapshot for admission.
         * @return Admitted plan or a typed corrupt, unsupported, or incompatible error.
         */
        [[nodiscard]] static Result<EnvironmentQueryPlan> Decode(std::span<const std::uint8_t> bytes, const QuerySchemaRegistry &registry);
    };
}  // namespace Horo::AI
