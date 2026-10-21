#include "Horo/Editor/PCGGraphDocument.h"
#include "Horo/PCG/PCGErrors.h"

#include <algorithm>
#include <type_traits>

namespace Horo::Editor {
    namespace {
        /** @brief Rejects structural changes while retaining only allowed pin-default intent. */
        bool PropertyIdentityMatches(const PCG::PCGGraphNode &node, const PCG::PCGGraphNode &before, const PCGNodePropertyDraft &property) {
            if (node.id != before.id || node.type != before.type || node.version != before.version || node.payload != before.payload ||
                property.node != before.id || node.pins.size() != before.pins.size())
                return false;
            for (std::size_t pin = 0; pin < node.pins.size(); ++pin) {
                auto unchanged = node.pins[pin];
                unchanged.defaultValue = before.pins[pin].defaultValue;
                if (unchanged != before.pins[pin])
                    return false;
            }
            return true;
        }

        /** @brief Proves explicit generation counts before codec allocation; checked products cannot overflow. */
        bool WithinTier(const PCG::PCGNodeAuthoringSettings &settings, const PCG::PCGTierLimits &tier) {
            return std::visit([&tier]<class T>(const T &value) {
                if constexpr (std::is_same_v<T, PCG::PCGPoissonGenerationSettings>)
                    return value.count <= tier.maximumPointsPerNodeOutput;
                else if constexpr (std::is_same_v<T, PCG::PCGStratifiedGenerationSettings>) {
                    std::size_t count = 1;
                    for (const auto dimension : value.strata) {
                        if (dimension == 0 || count > tier.maximumPointsPerNodeOutput / dimension)
                            return false;
                        count *= dimension;
                    }
                    return true;
                } else
                    return true;
            }, settings);
        }

        /** @brief Encodes one supported property candidate; unavailable node bytes stay exact. */
        Result<PCG::PCGGraphNode> PrepareNode(const PCG::PCGGraphNode &node, const PCG::PCGGraphNode &before,
                                              const PCGNodePropertyDraft &property, const PCG::PCGNodeCatalogSnapshot &catalog,
                                              const PCG::PCGTierLimits &tier) {
            using enum PCGDocumentCommandFailureReason;
            if (!PropertyIdentityMatches(node, before, property))
                return Result<PCG::PCGGraphNode>::Failure(MakeError(PCGDocumentError(Invalid)));
            if (property.settings) {
                if (!WithinTier(*property.settings, tier))
                    return Result<PCG::PCGGraphNode>::Failure(MakeError(PCGDocumentError(Limit)));
                return catalog.EncodeSettings(node, *property.settings);
            }
            if (node != before)
                return Result<PCG::PCGGraphNode>::Failure(MakeError(PCGDocumentError(Invalid)));
            return Result<PCG::PCGGraphNode>::Success(node);
        }

        /** @brief Maps a persisted mode without selecting a fallback or granting execution. */
        Result<PCG::PCGCapability> ModeCapability(PCG::PCGGenerationMode mode) {
            using Return = Result<PCG::PCGCapability>;
            using enum PCG::PCGGenerationMode;
            using enum PCG::PCGCapability;
            switch (mode) {
                case Offline:
                    return Return::Success(OfflineBake);
                case Runtime:
                    return Return::Success(RuntimeEvaluation);
                case Hybrid:
                    return Return::Success(HybridEvaluation);
                default:
                    return Return::Failure(MakeError(PCGDocumentError(PCGDocumentCommandFailureReason::Invalid)));
            }
        }

        /** @brief Validates the complete detached property/input candidate before building semantic operations. */
        Result<PCG::PCGGraphAsset> PrepareCandidate(const PCGGraphAuthoringDraft &draft, const PCG::PCGNodeCatalogSnapshot &catalog) {
            auto candidate = draft.values;
            const auto &original = draft.baseline.source->Data();
            // This gesture cannot smuggle topology, identity or tier changes into property operations.
            if (candidate.generation != original.generation || candidate.tier != original.tier || candidate.version != original.version ||
                candidate.edges != original.edges || candidate.nodes.size() != original.nodes.size() ||
                draft.properties.size() != original.nodes.size())
                return Result<PCG::PCGGraphAsset>::Failure(MakeError(PCGDocumentError(PCGDocumentCommandFailureReason::Invalid)));
            const auto tier = PCG::LimitsForTier(candidate.tier);
            if (tier.HasError())
                return Result<PCG::PCGGraphAsset>::Failure(tier.ErrorValue());
            for (std::size_t index = 0; index < candidate.nodes.size(); ++index) {
                auto prepared = PrepareNode(candidate.nodes[index], original.nodes[index], draft.properties[index], catalog, tier.Value());
                if (prepared.HasError())
                    return Result<PCG::PCGGraphAsset>::Failure(prepared.ErrorValue());
                candidate.nodes[index] = std::move(prepared).Value();
            }
            std::vector<PCG::PCGNodeTypeSupport> types;
            for (const auto &type : catalog.Nodes())
                types.push_back({type.type, type.version, type.version});
            return PCG::PCGGraphAsset::Create(std::move(candidate), {.tier = draft.values.tier,
                                                                     .unknownNodePolicy = PCG::PCGUnknownNodePolicy::PreserveInert,
                                                                     .supportedNodeTypes = types});
        }

        /** @brief Builds only source deltas; admission and semantic validation have already succeeded. */
        PCGDocumentCommand BuildOperations(const PCGDocumentSnapshot &current, const PCG::PCGGraphSourceData &candidate,
                                           const PCG::PCGGraphSourceData &original, bool canEdit) {
            PCGDocumentCommand command{.session = current.session,
                                       .expectedRevision = current.revision,
                                       .catalogRevision = current.catalogRevision,
                                       .canEdit = canEdit};
            if (candidate.mode != original.mode || candidate.deterministicSeed != original.deterministicSeed)
                command.operations.emplace_back(SetPCGGraphPolicy{candidate.mode, candidate.deterministicSeed});
            for (std::size_t index = 0; index < candidate.nodes.size(); ++index)
                if (candidate.nodes[index] != original.nodes[index])
                    command.operations.emplace_back(PutPCGNode{candidate.nodes[index]});
            for (const auto &input : candidate.exposedInputs) {
                const auto before = std::ranges::find(original.exposedInputs, input.id, &PCG::PCGExposedInput::id);
                if (before == original.exposedInputs.end() || input != *before)
                    command.operations.emplace_back(PutPCGInput{input});
            }
            for (const auto &input : original.exposedInputs)
                if (std::ranges::find(candidate.exposedInputs, input.id, &PCG::PCGExposedInput::id) == candidate.exposedInputs.end())
                    command.operations.emplace_back(RemovePCGInput{input.id});
            return command;
        }
    }  // namespace

    /** @copydoc PCGGraphAuthoringDraft::Capture */
    Result<PCGGraphAuthoringDraft> PCGGraphAuthoringDraft::Capture(const PCGDocumentSnapshot &snapshot,
                                                                   const PCG::PCGNodeCatalogSnapshot &catalog) {
        if (!snapshot.source || !snapshot.session.IsValid() || !catalog.IsValid())
            return Result<PCGGraphAuthoringDraft>::Failure(MakeError(PCGDocumentError(PCGDocumentCommandFailureReason::Invalid)));
        PCGGraphAuthoringDraft draft{.baseline = snapshot,
                                     .executableCatalogGeneration = catalog.Generation(),
                                     .values = snapshot.source->Data()};
        for (const auto &node : draft.values.nodes) {
            auto decoded = catalog.DecodeSettings(node);
            PCGNodePropertyDraft property{.node = node.id};
            if (decoded.HasValue())
                property.settings = std::move(decoded).Value();
            else
                property.diagnostic = decoded.ErrorValue();
            draft.properties.push_back(std::move(property));
        }
        return Result<PCGGraphAuthoringDraft>::Success(std::move(draft));
    }

    /** @copydoc PCGGraphAuthoringDraft::Matches */
    bool PCGGraphAuthoringDraft::Matches(const PCGDocumentSnapshot &snapshot, const PCG::PCGNodeCatalogSnapshot &catalog) const noexcept {
        return baseline.source && snapshot.source && snapshot.asset == baseline.asset && snapshot.session == baseline.session &&
               snapshot.identity == baseline.identity && snapshot.revision == baseline.revision &&
               snapshot.catalogRevision == baseline.catalogRevision && snapshot.state == baseline.state && catalog.IsValid() &&
               catalog.Generation() == executableCatalogGeneration;
    }

    /** @copydoc PCGGraphAuthoringDraft::Command */
    Result<PCGDocumentCommand> PCGGraphAuthoringDraft::Command(const PCGDocumentSnapshot &current,
                                                               const PCG::PCGNodeCatalogSnapshot &catalog, bool canEdit) const {
        const auto reject = [](PCGDocumentCommandFailureReason reason) {
            return Result<PCGDocumentCommand>::Failure(MakeError(PCGDocumentError(reason)));
        };
        if (!canEdit)
            return reject(PCGDocumentCommandFailureReason::Unauthorized);
        if (!Matches(current, catalog))
            return reject(PCGDocumentCommandFailureReason::Stale);
        const auto capability = ModeCapability(values.mode);
        if (capability.HasError())
            return Result<PCGDocumentCommand>::Failure(capability.ErrorValue());
        if (values.mode != baseline.source->Data().mode && !catalog.Capabilities().granted.Contains(capability.Value()))
            return Result<PCGDocumentCommand>::Failure(MakeError(PCG::PCGErrors::UnsupportedCapability));
        auto prepared = PrepareCandidate(*this, catalog);
        if (prepared.HasError())
            return Result<PCGDocumentCommand>::Failure(prepared.ErrorValue());
        const auto &candidate = prepared.Value().Data();
        const auto &original = baseline.source->Data();
        auto command = BuildOperations(current, candidate, original, canEdit);
        if (command.operations.size() > 256)
            return reject(PCGDocumentCommandFailureReason::Limit);
        if (command.operations.empty())
            return reject(PCGDocumentCommandFailureReason::Invalid);
        return Result<PCGDocumentCommand>::Success(std::move(command));
    }
}  // namespace Horo::Editor
