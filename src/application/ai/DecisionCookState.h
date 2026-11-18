#pragma once
#include "Horo/AI/DecisionPlanCodec.h"
#include "Horo/Application/DecisionAssetCookHost.h"

namespace Horo::Application::DecisionCookDetail {
    struct Input final {
        Assets::AssetId asset;
        Assets::AssetTypeId type;
        Sha256Digest sourceDigest;
        AI::DecisionGraphSource graph;
    };

    struct Prepared final {
        Assets::AssetId asset;
        Assets::AssetTypeId type;
        Sha256Digest sourceDigest;
        std::vector<std::uint8_t> payload;
        std::vector<Assets::AssetId> dependencies;
    };

    /** @brief Associates a semantic schema version with the real project artifact providing it. */
    struct SchemaArtifact final {
        AI::BlackboardSchemaId identity;
        std::uint32_t version{};
        Assets::AssetId asset;
    };

    struct State final {
        std::shared_ptr<const Assets::AssetCookInputSnapshot> files;
        AI::DecisionCompilationEnvironment environment;
        AI::DecisionCompilationLimits limits;
        AssetCookTargetId target;
        std::uint64_t maximumPreparedBytes{};
        std::uint64_t preparedSchemaBytes{};
        std::vector<Input> inputs;
        std::vector<SchemaArtifact> schemas;
        std::vector<Prepared> prepared;
        Sha256Digest settings;
    };

    /** @brief Decodes one registered graph source and rebases authored locations to its captured project path. */
    [[nodiscard]] Result<AI::DecisionGraphSource> DecodeRegisteredDecisionGraphSource(const Assets::AssetCookPinnedSource &file,
                                                                                      AI::DecisionCompilationLimits limits);

    [[nodiscard]] Result<void> Verify(const State &state, const CancellationToken &cancellation);
    [[nodiscard]] Result<void> Capture(State &state, const Assets::AssetRegistrySnapshot &registry, const CancellationToken &cancellation);
    [[nodiscard]] Result<void> PrepareRoots(State &state, const CancellationToken &cancellation);
    [[nodiscard]] Result<std::shared_ptr<const Assets::CookerCatalogSnapshot>> Catalog(
        std::shared_ptr<const State> state, std::span<const Assets::CookerContribution> additional);
}  // namespace Horo::Application::DecisionCookDetail
