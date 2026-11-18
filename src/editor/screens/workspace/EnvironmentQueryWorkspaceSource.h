#pragma once

/** @file EnvironmentQueryWorkspaceSource.h @brief Private saved Query source admission used by the editor document and preview. */

#include "Horo/Assets/AssetCookInputSnapshot.h"

#include <cstdint>

namespace Horo::AI {
    class EnvironmentQueryAsset;
    class QuerySchemaRegistry;
}  // namespace Horo::AI

namespace Horo::Editor {
    enum class QueryDescriptorAvailability : std::uint8_t;
    struct EnvironmentQueryWorkspaceDocument;

    namespace EnvironmentQueryWorkspaceSource {
        /** @brief Captures a registered saved source and its validated identity sidecar. */
        [[nodiscard]] Result<Assets::AssetCookInputSnapshot> CaptureQuerySource(const std::filesystem::path &root,
                                                                                const Assets::AssetRegistrySnapshot &registry,
                                                                                Assets::AssetId source);
        /** @brief Checks the document's exact stable asset, registered path and source type. */
        [[nodiscard]] bool RegisteredQueryDocument(const EnvironmentQueryWorkspaceDocument &document,
                                                   const Assets::AssetRegistrySnapshot &registry);
        /** @brief Checks inert source descriptors and preserves the authored source on failure. */
        [[nodiscard]] QueryDescriptorAvailability DescriptorAvailability(const AI::EnvironmentQueryAsset &query,
                                                                         const AI::QuerySchemaRegistry *registry);
    }  // namespace EnvironmentQueryWorkspaceSource
}  // namespace Horo::Editor
