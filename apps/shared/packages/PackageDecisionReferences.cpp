#include "PackageBehaviorReferences.h"

#include "Horo/AI/DecisionPlanCodec.h"
#include "Horo/AI/EnvironmentQueryGenerators.h"
#include "Horo/AI/EnvironmentQuerySourceCodec.h"
#include "Horo/Assets/AssetRegistry.h"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <set>

namespace Horo::Application::Internal {
    namespace {
        using Owners = std::map<std::uint64_t, std::set<Packages::HoroPackageId>>;
        using ReferenceScan = Result<std::vector<Diagnostic>>;
        constexpr auto MaximumReferences = 16'384U;
        constexpr std::array DecisionTypes{AI::DecisionAssetType(AI::DecisionPlanKind::BehaviorTree),
                                          AI::DecisionAssetType(AI::DecisionPlanKind::StateMachine),
                                          AI::DecisionAssetType(AI::DecisionPlanKind::Utility)};

        struct ProviderOwners final {
            Owners decisions;
            std::set<Packages::HoroPackageId> customSenses;
        };

        /** @brief Preserves the failing saved path or locked package identity. */
        Error ReferenceError(const char *code, std::string_view identity = {}) {
            return {ErrorCode{code}, ErrorDomainId{"packages.host"}, ErrorSeverity::Error, std::string{identity}};
        }

        /** @brief Indexes only departing providers with evidence from the exact locked artifact. */
        Result<ProviderOwners> IndexProviderOwners(const PackageRemovalSources &sources, const Packages::ValidatedPackageLockfileV1 &lock,
                                                   const Packages::PackageRemovalGraphPreview &removal) {
            ProviderOwners owners;
            for (const auto &role : sources.registrations) {
                if ((role.decisionNodes.empty() && !role.hasCustomSenses) ||
                    std::ranges::find(removal.removed, role.package, &Packages::LockedPackageReference::package) == removal.removed.end())
                    continue;
                const auto entry = std::ranges::find(lock.Packages(), role.package, &Packages::LockedPackage::package);
                if (entry == lock.Packages().end() || entry->version != role.version || entry->artifactDigest != role.artifact)
                    return Result<ProviderOwners>::Failure(ReferenceError("packages.host.decision_ownership_stale", role.package.Value()));
                for (const auto type : role.decisionNodes)
                    owners.decisions[type.Value()].insert(role.package);
                if (role.hasCustomSenses)
                    owners.customSenses.insert(role.package);
            }
            return Result<ProviderOwners>::Success(std::move(owners));
        }

        /** @brief Reports each departing sense owner when the source may depend on its Perception facts. */
        Result<void> CollectSenseReview(const ProviderOwners &owners, std::string_view path, std::vector<Diagnostic> &diagnostics,
                                         const Error *readFailure = nullptr) {
            if (owners.customSenses.size() > MaximumReferences - diagnostics.size())
                return Result<void>::Failure(ReferenceError("packages.host.perception_reference_limit", path));
            const auto *code = readFailure ? "packages.host.perception_reference_unreadable" : "packages.host.perception_reference_review";
            for (const auto &package : owners.customSenses)
                diagnostics.emplace_back(DiagnosticCode{code}, DiagnosticSeverity::Error,
                                         readFailure ? package.Value() + " / " + readFailure->code.Value() : package.Value(),
                                         SourceLocation{std::string{path}}, "");
            return Result<void>::Success();
        }

        /** @brief Reads a bounded authored query and proves independence only against engine-owned point descriptors. */
        Result<bool> IsIndependentPointQuery(const PackageRemovalSources &sources, std::string_view path,
                                             const AI::QuerySchemaRegistry &pointSchema) {
            using Independence = Result<bool>;
            const auto read = sources.files.SourceText(path, sources.context,
                                                       {.maximumFileBytes = AI::EnvironmentQuerySourceCodec::MaximumBytes});
            if (read.HasError())
                return Independence::Failure(read.ErrorValue());
            const auto &bytes = *read.Value().text;
            const auto query = AI::EnvironmentQuerySourceCodec::Decode(
                {reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()});
            if (query.HasError())
                return Independence::Failure(query.ErrorValue());
            return Independence::Success(AI::ReadBuiltinPointQuerySettings(query.Value(), pointSchema).HasValue());
        }

        /** @brief Examines EQS with its typed codec; other authored Perception surfaces still require owner review. */
        Result<void> ReviewSenseUse(const PackageRemovalSources &sources, const ProviderOwners &owners,
                                    const Assets::AssetRecord &record, const AI::QuerySchemaRegistry *pointSchema, const bool decision,
                                    std::vector<Diagnostic> &diagnostics) {
            if (owners.customSenses.empty())
                return Result<void>::Success();
            const auto type = record.type.Value();
            const auto path = record.sourcePath.String();
            if (type == AI::EnvironmentQuerySourceAssetType && pointSchema) {
                const auto independent = IsIndependentPointQuery(sources, path, *pointSchema);
                if (independent.HasError())
                    return CollectSenseReview(owners, path, diagnostics, &independent.ErrorValue());
                if (independent.Value())
                    return Result<void>::Success();
            }
            if (!decision && type != "core.scene" && type != "core.prefab" && type != AI::EnvironmentQuerySourceAssetType)
                return Result<void>::Success();
            return CollectSenseReview(owners, path, diagnostics);
        }

        /** @brief Records each authored use, including its exact node or action phase. */
        struct DecisionReferences final {
            const Owners &owners;
            std::string_view assetPath;
            std::vector<Diagnostic> &diagnostics;

            Result<void> Collect(const std::uint64_t type, const std::uint64_t node, const std::string_view phase = {}) const {
                const auto found = owners.find(type);
                if (found == owners.end())
                    return Result<void>::Success();
                if (found->second.size() > MaximumReferences - diagnostics.size())
                    return Result<void>::Failure(ReferenceError("packages.host.decision_reference_limit", assetPath));
                auto identity = std::to_string(node);
                identity.append(phase);
                for (const auto &package : found->second)
                    diagnostics.emplace_back(DiagnosticCode{"packages.host.decision_node_in_use"}, DiagnosticSeverity::Error,
                                             package.Value() + " / " + std::to_string(type), SourceLocation{std::string{assetPath}},
                                             identity);
                return Result<void>::Success();
            }

            Result<void> operator()(const std::shared_ptr<const AI::BehaviorTreeAsset> &tree) const {
                for (const auto &node : tree->Data().nodes)
                    if (auto collected = Collect(node.type.Value(), node.id.Value()); collected.HasError())
                        return collected;
                return Result<void>::Success();
            }

            Result<void> operator()(const AI::StateMachineAssetDescriptor &machine) const {
                for (const auto &state : machine.states) {
                    const std::array phases{std::pair{&state.entryProvider, "/entry"}, std::pair{&state.updateProvider, "/update"},
                                            std::pair{&state.exitProvider, "/exit"}};
                    for (const auto &[action, phase] : phases) {
                        if (!*action)
                            continue;
                        if (auto collected = Collect((*action)->binding.type.Value(), state.id.Value(), phase); collected.HasError())
                            return collected;
                    }
                }
                return Result<void>::Success();
            }

            Result<void> operator()(const AI::UtilityDecisionSource &utility) const {
                for (const auto &option : utility.options)
                    if (auto collected = Collect(option.provider.type.Value(), option.provider.id.Value()); collected.HasError())
                        return collected;
                return Result<void>::Success();
            }
        };

        /** @brief Uses the bounded codec for the registered source, without loading gameplay factories. */
        Result<void> InspectGraph(const PackageRemovalSources &sources, const Assets::AssetRecord &record,
                                  const DecisionReferences &references) {
            const auto captured = sources.files.SourceText(record.sourcePath.String(), sources.context, {});
            if (captured.HasError())
                return Result<void>::Failure(
                    WithCause(ReferenceError("packages.host.decision_reference_unreadable", references.assetPath), captured.ErrorValue()));
            const auto &bytes = *captured.Value().text;
            AI::DecisionGraphAssetId root;
            const auto decoded = AI::DecisionPlanCodec::DecodeSource(
                {reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()}, root);
            if (decoded.HasError())
                return Result<void>::Failure(
                    WithCause(ReferenceError("packages.host.decision_reference_unreadable", references.assetPath), decoded.ErrorValue()));
            const auto &graphs = decoded.Value();
            if (graphs.size() != 1 || graphs.front().asset != root || DecisionTypes[graphs.front().body.index()] != record.type.Value())
                return Result<void>::Failure(ReferenceError("packages.host.decision_reference_identity_mismatch", references.assetPath));
            return std::visit(references, graphs.front().body);
        }

        /** @brief Reviews the provider surfaces relevant to this one registered asset. */
        Result<void> InspectProviderSource(const PackageRemovalSources &sources, const ProviderOwners &owners,
                                           const Assets::AssetRecord &record, const AI::QuerySchemaRegistry *pointSchema,
                                           std::vector<Diagnostic> &diagnostics) {
            const bool decision = std::ranges::find(DecisionTypes, record.type.Value()) != DecisionTypes.end();
            if (auto reviewed = ReviewSenseUse(sources, owners, record, pointSchema, decision, diagnostics); reviewed.HasError())
                return reviewed;
            if (!decision || owners.decisions.empty())
                return Result<void>::Success();
            return InspectGraph(sources, record, {owners.decisions, record.sourcePath.String(), diagnostics});
        }

        /** @brief Uses one immutable schema for the scan and keeps the original command authority for all reads. */
        ReferenceScan ScanProviderSources(const PackageRemovalSources &sources, const ProviderOwners &owners,
                                          std::span<const Assets::AssetRecord> records) {
            std::optional<AI::QuerySchemaRegistry> pointSchema;
            if (!owners.customSenses.empty()) {
                auto schema = AI::CaptureBuiltinQuerySchema();
                if (schema.HasError())
                    return ReferenceScan::Failure(schema.ErrorValue());
                pointSchema.emplace(std::move(schema).Value());
            }
            const auto *schema = pointSchema ? &*pointSchema : nullptr;
            std::vector<Diagnostic> diagnostics;
            for (const auto &record : records)
                if (auto inspected = InspectProviderSource(sources, owners, record, schema, diagnostics); inspected.HasError())
                    return ReferenceScan::Failure(inspected.ErrorValue());
            if (auto admitted = Platform::CheckProjectReadContext(sources.context); admitted.HasError())
                return ReferenceScan::Failure(admitted.ErrorValue());
            return ReferenceScan::Success(std::move(diagnostics));
        }
    }  // namespace

    /** @copydoc InspectPackageDecisionReferences */
    Result<std::vector<Diagnostic>> InspectPackageDecisionReferences(const PackageRemovalSources &sources,
                                                                   const Packages::ValidatedPackageLockfileV1 &lock,
                                                                   const Packages::PackageRemovalGraphPreview &removal) {
        const auto owners = IndexProviderOwners(sources, lock, removal);
        if (owners.HasError())
            return ReferenceScan::Failure(owners.ErrorValue());
        if (owners.Value().decisions.empty() && owners.Value().customSenses.empty())
            return ReferenceScan::Success({});
        const auto candidate = Assets::PrepareAssetRegistryCandidate(sources.assets);
        if (candidate.HasError())
            return ReferenceScan::Failure(candidate.ErrorValue());
        if (candidate.Value().status != Assets::AssetRegistryBuildStatus::Complete)
            return ReferenceScan::Failure(ReferenceError("packages.host.asset_inventory_incomplete"));
        return ScanProviderSources(sources, owners.Value(), candidate.Value().records);
    }
}  // namespace Horo::Application::Internal
