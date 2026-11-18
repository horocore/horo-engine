#pragma once

#include "Horo/AI/DecisionIdentity.h"
#include "Horo/Assets/AssetSourcePublication.h"
#include "editor/screens/workspace/DecisionStateMachineAuthoring.h"

#include <compare>
#include <span>
#include <vector>

namespace Horo::Editor {
    struct GraphViewSnapshot;

    /** @brief Authored placement in unscaled canvas units, addressed by stable source identity. */
    struct DecisionNodePlacement final {
        AI::DecisionNodeId node;
        float x{};
        float y{};
        auto operator<=>(const DecisionNodePlacement &) const = default;
    };

    /** @brief Bounded ordered set of position overrides separate from Decision topology. */
    using DecisionGraphLayout = std::vector<DecisionNodePlacement>;

    inline constexpr std::size_t MaximumDecisionLayoutNodes = 1024;
    inline constexpr std::size_t MaximumDecisionLayoutBytes = 128U * 1024U;
    inline constexpr float MaximumDecisionLayoutCoordinate = 1'000'000.0F;

    enum class DecisionLayoutOperation : std::uint8_t {
        Move,
        AutoLayout
    };

    /** @brief One revision-fenced drag or explicit auto-layout command. */
    struct DecisionGraphLayoutRequest final {
        DecisionGraphDocumentFence fence;
        DecisionLayoutOperation operation{DecisionLayoutOperation::Move};
        DecisionGraphLayout placements;
    };

    /** @brief Accepted canonical disk identity and immutable placement source. */
    struct DecisionGraphLayoutCapture final {
        Assets::AssetSourceIdentity accepted;
        DecisionGraphLayout layout;
    };

    /** @brief Resolves the paired canonical authoring metadata path. */
    [[nodiscard]] std::filesystem::path DecisionLayoutPath(const std::filesystem::path &source);
    /** @brief Reads bounded placement metadata pinned to both durable source identities. */
    [[nodiscard]] Result<DecisionGraphLayoutCapture> CaptureDecisionLayout(const std::filesystem::path &root,
                                                                           const std::filesystem::path &source, Assets::AssetId asset,
                                                                           AI::DecisionGraphAssetId graph);
    /** @brief Encodes one canonical layout independently of runtime graph bytes. */
    [[nodiscard]] Result<std::vector<std::byte>> EncodeDecisionLayout(const DecisionGraphLayout &layout, Assets::AssetId asset,
                                                                      AI::DecisionGraphAssetId graph);
    /** @brief Computes deterministic logical positions across reachable graph layers. */
    [[nodiscard]] DecisionGraphLayout AutomaticDecisionLayout(const GraphViewSnapshot &graph);
    /** @brief Applies only bounded positions for nodes in the current revision. */
    [[nodiscard]] Result<DecisionGraphLayout> ApplyDecisionLayout(const GraphViewSnapshot &graph, const DecisionGraphLayout &current,
                                                                  const DecisionGraphLayoutRequest &request);
    /** @brief Retains placements for surviving stable authored nodes after a semantic edit. */
    [[nodiscard]] DecisionGraphLayout PruneDecisionLayout(const DecisionGraphLayout &layout, const GraphViewSnapshot &graph);
}  // namespace Horo::Editor
