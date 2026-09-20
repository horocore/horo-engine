#include "Horo/AI/DecisionAssetValidation.h"

#include "Horo/AI/AIErrors.h"

#include <algorithm>
#include <format>
#include <iterator>
#include <new>
#include <ranges>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace Horo::AI {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        template <typename Enum> [[nodiscard]] constexpr bool IsKnown(const Enum value, const Enum count) noexcept {
            return static_cast<std::underlying_type_t<Enum>>(value) < static_cast<std::underlying_type_t<Enum>>(count);
        }

        [[nodiscard]] bool ValidSource(const SourceLocation &source) noexcept {
            return !source.source.empty() && (source.line != 0 || source.column == 0);
        }

        [[nodiscard]] SourceLocation DiagnosticSource(const SourceLocation &preferred, const SourceLocation &fallback) {
            if (ValidSource(preferred))
                return preferred;
            if (ValidSource(fallback))
                return fallback;
            return SourceLocation{"<decision-asset>", 1, 1};
        }

        [[nodiscard]] bool ValidOrigin(const DecisionDescriptorOrigin &origin) noexcept {
            return IsKnown(origin.kind, DecisionDescriptorSourceKind::Count) && origin.provider.IsValid() && origin.version != 0;
        }

        [[nodiscard]] bool ValidRequirement(const DecisionBlackboardRequirement &requirement) noexcept {
            return requirement.key.IsValid() && IsKnown(requirement.kind, BlackboardValueKind::Count) &&
                   IsKnown(requirement.cardinality, BlackboardValueCardinality::Count) &&
                   IsKnown(requirement.access, BlackboardKeyAccess::Count) && IsKnown(requirement.presence, BlackboardKeyPresence::Count) &&
                   ValidSource(requirement.source);
        }

        [[nodiscard]] bool SameRequirementContract(const DecisionBlackboardRequirement &left,
                                                   const DecisionBlackboardRequirement &right) noexcept {
            return left.key == right.key && left.kind == right.kind && left.cardinality == right.cardinality &&
                   left.access == right.access && left.presence == right.presence && left.requireDefault == right.requireDefault;
        }

        [[nodiscard]] bool ValidLimits(const DecisionAssetValidationLimits &limits) noexcept {
            return limits.maximumAssets > 0 && limits.maximumAssets <= DecisionAssetValidationHardLimits::Assets &&
                   limits.maximumNodeDescriptors > 0 &&
                   limits.maximumNodeDescriptors <= DecisionAssetValidationHardLimits::NodeDescriptors && limits.maximumSchemas > 0 &&
                   limits.maximumSchemas <= DecisionAssetValidationHardLimits::Schemas && limits.maximumNodesPerAsset > 0 &&
                   limits.maximumNodesPerAsset <= DecisionAssetValidationHardLimits::NodesPerAsset && limits.maximumSubtreesPerAsset > 0 &&
                   limits.maximumSubtreesPerAsset <= DecisionAssetValidationHardLimits::SubtreesPerAsset &&
                   limits.maximumRequirementsPerNode > 0 &&
                   limits.maximumRequirementsPerNode <= DecisionAssetValidationHardLimits::RequirementsPerNode &&
                   limits.maximumTotalRequirements > 0 &&
                   limits.maximumTotalRequirements <= DecisionAssetValidationHardLimits::TotalRequirements &&
                   limits.maximumDependencyDepth > 0 &&
                   limits.maximumDependencyDepth <= DecisionAssetValidationHardLimits::DependencyDepth && limits.maximumDiagnostics > 0 &&
                   limits.maximumDiagnostics <= DecisionAssetValidationHardLimits::Diagnostics;
        }

        [[nodiscard]] bool SameDiagnostic(const DecisionAssetValidationDiagnostic &left,
                                          const DecisionAssetValidationDiagnostic &right) noexcept {
            return left.code.Value() == right.code.Value() && left.severity == right.severity && left.message == right.message &&
                   left.source.source == right.source.source && left.source.line == right.source.line &&
                   left.source.column == right.source.column && left.path == right.path;
        }

        void CanonicalizeDiagnostics(std::vector<DecisionAssetValidationDiagnostic> &diagnostics) {
            std::ranges::sort(diagnostics,
                              [](const DecisionAssetValidationDiagnostic &left, const DecisionAssetValidationDiagnostic &right) {
                return std::tie(left.source.source, left.source.line, left.source.column, left.path, left.code.Value(), left.message) <
                       std::tie(right.source.source, right.source.line, right.source.column, right.path, right.code.Value(), right.message);
            });
            const auto uniqueEnd = std::ranges::unique(diagnostics, SameDiagnostic).begin();
            diagnostics.erase(uniqueEnd, diagnostics.end());
        }

        class Validator final {
        public:
            Validator(const DecisionAssetDescriptor &root, const std::span<const DecisionAssetDescriptor> assetCatalog,
                      const std::span<const DecisionNodeDescriptor> nodeDescriptors,
                      const std::span<const std::shared_ptr<const BlackboardSchema>> schemas, const DecisionAssetValidationLimits &limits)
                : root_(root), assetCatalog_(assetCatalog), nodeDescriptors_(nodeDescriptors), schemas_(schemas), limits_(limits) {}

            void ValidateRoot() {
                ValidateAsset(root_, 0, true);
            }

            [[nodiscard]] std::vector<DecisionAssetValidationDiagnostic> TakeDiagnostics() && {
                CanonicalizeDiagnostics(diagnostics_);
                return std::move(diagnostics_);
            }

            [[nodiscard]] bool DiagnosticsTruncated() const noexcept {
                return diagnosticsTruncated_;
            }

            [[nodiscard]] std::shared_ptr<const BlackboardSchema> RootSchema() && noexcept {
                return std::move(rootSchema_);
            }

            [[nodiscard]] std::vector<DecisionPlanNode> TakePlanNodes() && {
                return std::move(planNodes_);
            }

            [[nodiscard]] std::vector<DecisionPlanBlackboardBinding> TakePlanBindings() && {
                return std::move(planBindings_);
            }

            [[nodiscard]] std::vector<DecisionPlanDependency> TakePlanDependencies() && {
                std::ranges::sort(planDependencies_, [](const DecisionPlanDependency &left, const DecisionPlanDependency &right) {
                    return std::tie(left.asset, left.kind, left.schemaVersion) < std::tie(right.asset, right.kind, right.schemaVersion);
                });
                const auto uniqueEnd =
                    std::ranges::unique(planDependencies_, [](const DecisionPlanDependency &left, const DecisionPlanDependency &right) {
                    return left.asset == right.asset && left.kind == right.kind && left.schemaVersion == right.schemaVersion;
                }).begin();
                planDependencies_.erase(uniqueEnd, planDependencies_.end());
                return std::move(planDependencies_);
            }

        private:
            struct NodeDescriptorLookup final {
                const DecisionNodeDescriptor *descriptor{};
                std::size_t typeMatches{};
                std::size_t compatibleMatches{};
            };

            void Add(const ErrorCodeDescriptor &descriptor, std::string message, const SourceLocation &preferred,
                     const SourceLocation &fallback, std::string path) {
                if (diagnostics_.size() >= limits_.maximumDiagnostics) {
                    diagnosticsTruncated_ = true;
                    return;
                }
                const auto severity = DiagnosticSeverityForError(descriptor.defaultSeverity).value_or(DiagnosticSeverity::Error);
                diagnostics_.push_back({.code = descriptor.code,
                                        .severity = severity,
                                        .message = message.empty() ? std::string{descriptor.summary} : std::move(message),
                                        .source = DiagnosticSource(preferred, fallback),
                                        .path = std::move(path)});
            }

            [[nodiscard]] std::shared_ptr<const BlackboardSchema> ResolveSchema(const DecisionAssetDescriptor &asset,
                                                                                const std::string_view path) {
                std::size_t identityMatches{};
                std::size_t compatibleMatches{};
                std::shared_ptr<const BlackboardSchema> resolved;
                for (const auto &schema : schemas_) {
                    if (schema == nullptr || schema->Identity() != asset.blackboardSchema)
                        continue;
                    ++identityMatches;
                    if (!asset.requiredBlackboardSchemaVersion.Contains(schema->Version()))
                        continue;
                    ++compatibleMatches;
                    resolved = schema;
                }
                if (identityMatches == 0) {
                    Add(AIErrors::DecisionAssetSchemaMissing, {}, asset.source, root_.source, std::string{path});
                    return {};
                }
                if (compatibleMatches == 0) {
                    Add(AIErrors::DecisionAssetSchemaIncompatible, {}, asset.source, root_.source, std::string{path});
                    return {};
                }
                if (compatibleMatches > 1) {
                    Add(AIErrors::DecisionAssetSchemaAmbiguous, {}, asset.source, root_.source, std::string{path});
                    return {};
                }
                return resolved;
            }

            [[nodiscard]] NodeDescriptorLookup ResolveNodeDescriptor(const DecisionAssetNode &node, const SourceLocation &fallback,
                                                                     const std::string_view path) {
                NodeDescriptorLookup lookup;
                for (const auto &descriptor : nodeDescriptors_) {
                    if (descriptor.type != node.type)
                        continue;
                    ++lookup.typeMatches;
                    if (!ValidOrigin(descriptor.origin) || !ValidSource(descriptor.source)) {
                        Add(AIErrors::DecisionAssetSchemaInvalid, {}, descriptor.source, fallback, std::string{path});
                        continue;
                    }
                    if (!node.descriptorVersion.Contains(descriptor.origin.version))
                        continue;
                    ++lookup.compatibleMatches;
                    lookup.descriptor = &descriptor;
                }
                if (lookup.typeMatches == 0) {
                    Add(AIErrors::DecisionAssetDescriptorMissing, {}, node.source, fallback, std::string{path});
                } else if (lookup.compatibleMatches == 0) {
                    Add(AIErrors::DecisionAssetDescriptorIncompatible, {}, node.source, fallback, std::string{path});
                } else if (lookup.compatibleMatches > 1) {
                    Add(AIErrors::DecisionAssetDescriptorAmbiguous, {}, node.source, fallback, std::string{path});
                    lookup.descriptor = nullptr;
                }
                return lookup;
            }

            [[nodiscard]] const DecisionAssetDescriptor *ResolveSubtree(const DecisionSubtreeReference &reference,
                                                                        const std::string_view path) {
                std::vector<const DecisionAssetDescriptor *> matches;
                matches.reserve(2);
                const bool catalogContainsIdentity = std::ranges::any_of(assetCatalog_, [&reference](const auto &candidate) {
                    return candidate.asset == reference.asset;
                });
                const auto consider = [&matches, &reference](const DecisionAssetDescriptor *candidate) {
                    if (candidate != nullptr && candidate->asset == reference.asset &&
                        std::ranges::find(matches, candidate) == matches.end())
                        matches.push_back(candidate);
                };
                if (!catalogContainsIdentity)
                    consider(&root_);
                for (const auto &candidate : assetCatalog_)
                    consider(&candidate);

                if (matches.empty()) {
                    Add(AIErrors::DecisionAssetSubtreeMissing, {}, reference.source, root_.source, std::string{path});
                    return nullptr;
                }
                if (matches.size() > 1) {
                    Add(AIErrors::DecisionAssetSubtreeAmbiguous, {}, reference.source, root_.source, std::string{path});
                    return nullptr;
                }
                const auto *candidate = matches.front();
                if (candidate->kind != reference.expectedKind || !reference.requiredAssetVersion.Contains(candidate->schemaVersion)) {
                    Add(AIErrors::DecisionAssetSubtreeIncompatible, {}, reference.source, candidate->source, std::string{path});
                    return nullptr;
                }
                return candidate;
            }

            [[nodiscard]] const BlackboardKeyDescriptor *FindKey(const BlackboardSchema &schema, const BlackboardKeyId key,
                                                                 std::size_t &index) const noexcept {
                const auto keys = schema.Keys();
                const auto found = std::ranges::lower_bound(keys, key, {}, &BlackboardKeyDescriptor::key);
                if (found == keys.end() || found->key != key)
                    return nullptr;
                index = static_cast<std::size_t>(found - keys.begin());
                return std::to_address(found);
            }

            [[nodiscard]] bool ResolveRequirement(const DecisionBlackboardRequirement &requirement,
                                                  const std::shared_ptr<const BlackboardSchema> &schema, const SourceLocation &fallback,
                                                  const std::string_view path,
                                                  std::vector<DecisionPlanBlackboardBinding> &resolvedBindings) {
                if (!ValidRequirement(requirement)) {
                    Add(AIErrors::DecisionAssetSchemaInvalid, {}, requirement.source, fallback, std::string{path});
                    return false;
                }
                if (schema == nullptr)
                    return false;

                std::size_t schemaIndex{};
                const auto *key = FindKey(*schema, requirement.key, schemaIndex);
                if (key == nullptr) {
                    Add(AIErrors::DecisionAssetBindingMissing, {}, requirement.source, fallback, std::string{path});
                    return false;
                }
                bool valid = true;
                if (key->kind != requirement.kind || key->cardinality != requirement.cardinality) {
                    Add(AIErrors::DecisionAssetBindingTypeMismatch, {}, requirement.source, fallback, std::string{path});
                    valid = false;
                }
                if (requirement.access == BlackboardKeyAccess::ReadWrite && key->access != BlackboardKeyAccess::ReadWrite) {
                    Add(AIErrors::DecisionAssetBindingAccessMismatch, {}, requirement.source, fallback, std::string{path});
                    valid = false;
                }
                if (requirement.presence == BlackboardKeyPresence::Required && key->presence != BlackboardKeyPresence::Required) {
                    Add(AIErrors::DecisionAssetBindingPresenceMismatch, {}, requirement.source, fallback, std::string{path});
                    valid = false;
                }
                if (requirement.requireDefault && !key->defaultValue.has_value()) {
                    Add(AIErrors::DecisionAssetBindingDefaultMissing, {}, requirement.source, fallback, std::string{path});
                    valid = false;
                }
                if (valid)
                    resolvedBindings.push_back({.key = key->key,
                                                .schemaIndex = schemaIndex,
                                                .kind = key->kind,
                                                .cardinality = key->cardinality,
                                                .access = key->access,
                                                .presence = key->presence,
                                                .defaultValue = key->defaultValue});
                return valid;
            }

            [[nodiscard]] bool ValidateNodeRequirements(const DecisionAssetNode &node, const NodeDescriptorLookup &lookup,
                                                        const std::shared_ptr<const BlackboardSchema> &schema,
                                                        const SourceLocation &fallback, const std::string &path,
                                                        std::vector<DecisionPlanBlackboardBinding> &resolvedBindings) {
                std::vector<const DecisionBlackboardRequirement *> requirements;
                requirements.reserve(node.requirements.size() +
                                     (lookup.descriptor == nullptr ? 0U : lookup.descriptor->requirements.size()));
                if (lookup.descriptor != nullptr)
                    for (const auto &requirement : lookup.descriptor->requirements)
                        requirements.push_back(&requirement);
                for (const auto &requirement : node.requirements)
                    requirements.push_back(&requirement);

                std::vector<const DecisionBlackboardRequirement *> uniqueRequirements;
                uniqueRequirements.reserve(requirements.size());
                bool nodeValid = lookup.descriptor != nullptr;
                for (const auto *requirement : requirements) {
                    if (!ValidRequirement(*requirement)) {
                        Add(AIErrors::DecisionAssetSchemaInvalid, {}, requirement->source, fallback, path + ".blackboard");
                        nodeValid = false;
                        continue;
                    }
                    if (const auto duplicate = std::ranges::find_if(uniqueRequirements,
                                                                    [requirement](const auto *existing) {
                        return existing->key == requirement->key;
                    });
                        duplicate != uniqueRequirements.end()) {
                        if (!SameRequirementContract(**duplicate, *requirement)) {
                            Add(AIErrors::DecisionAssetBindingAmbiguous, {}, requirement->source, node.source, path + ".blackboard");
                            nodeValid = false;
                        }
                        continue;
                    }
                    uniqueRequirements.push_back(requirement);
                    const bool requirementValid =
                        ResolveRequirement(*requirement, schema, fallback, path + ".blackboard", resolvedBindings);
                    nodeValid = nodeValid && requirementValid;
                }
                return nodeValid;
            }

            void ValidateNode(const DecisionAssetDescriptor &asset, const DecisionAssetNode &node,
                              const std::shared_ptr<const BlackboardSchema> &schema, const bool rootAsset) {
                const std::string path = std::format("asset.nodes[{}]", node.id.Value());
                if (!node.id.IsValid() || !node.type.IsValid() || !node.descriptorVersion.IsValid() || !ValidSource(node.source)) {
                    Add(AIErrors::DecisionAssetSchemaInvalid, {}, node.source, asset.source, path);
                    return;
                }
                if (node.requirements.size() > limits_.maximumRequirementsPerNode) {
                    Add(AIErrors::DecisionAssetLimitExceeded, {}, node.source, asset.source, path);
                    return;
                }
                totalRequirements_ += node.requirements.size();
                if (totalRequirements_ > limits_.maximumTotalRequirements) {
                    Add(AIErrors::DecisionAssetLimitExceeded, {}, node.source, asset.source, path);
                    return;
                }

                const auto descriptorPath = path + ".descriptor";
                const auto lookup = ResolveNodeDescriptor(node, asset.source, descriptorPath);
                if (lookup.descriptor != nullptr) {
                    if (lookup.descriptor->requirements.size() > limits_.maximumRequirementsPerNode) {
                        Add(AIErrors::DecisionAssetLimitExceeded, {}, lookup.descriptor->source, node.source, descriptorPath);
                        return;
                    }
                    totalRequirements_ += lookup.descriptor->requirements.size();
                    if (totalRequirements_ > limits_.maximumTotalRequirements) {
                        Add(AIErrors::DecisionAssetLimitExceeded, {}, lookup.descriptor->source, node.source, descriptorPath);
                        return;
                    }
                }

                std::vector<DecisionPlanBlackboardBinding> resolvedBindings;
                if (!ValidateNodeRequirements(node, lookup, schema, node.source, path, resolvedBindings) || !rootAsset)
                    return;

                std::ranges::sort(resolvedBindings, {}, &DecisionPlanBlackboardBinding::key);
                const std::size_t firstBinding = planBindings_.size();
                const std::size_t bindingCount = resolvedBindings.size();
                planBindings_.insert(planBindings_.end(), std::make_move_iterator(resolvedBindings.begin()),
                                     std::make_move_iterator(resolvedBindings.end()));
                planNodes_.push_back({.id = node.id,
                                      .type = node.type,
                                      .origin = lookup.descriptor->origin,
                                      .firstBlackboardBinding = firstBinding,
                                      .blackboardBindingCount = bindingCount});
            }

            void ValidateAssetNodes(const DecisionAssetDescriptor &asset, const std::shared_ptr<const BlackboardSchema> &schema,
                                    const bool rootAsset, const std::string &assetPath) {
                std::vector<std::uint64_t> nodeIdentities;
                nodeIdentities.reserve(asset.nodes.size());
                for (const auto &node : asset.nodes)
                    nodeIdentities.push_back(node.id.Value());
                std::ranges::sort(nodeIdentities);
                if (std::ranges::adjacent_find(nodeIdentities) != nodeIdentities.end())
                    Add(AIErrors::DecisionAssetSchemaInvalid, "Decision nodes must use distinct stable identities.", asset.source,
                        root_.source, assetPath + ".nodes");

                std::vector<const DecisionAssetNode *> orderedNodes;
                orderedNodes.reserve(asset.nodes.size());
                for (const auto &node : asset.nodes)
                    orderedNodes.push_back(&node);
                std::ranges::sort(orderedNodes, {}, [](const auto *node) {
                    return node->id;
                });
                for (const auto *node : orderedNodes)
                    ValidateNode(asset, *node, schema, rootAsset);
            }

            void ValidateSubtrees(const DecisionAssetDescriptor &asset, const std::size_t depth, const std::string &assetPath) {
                for (const auto &reference : asset.subtrees) {
                    const std::string path = std::format("{}.subtrees[{}]", assetPath, reference.asset.Value());
                    if (!reference.asset.IsValid() || !IsKnown(reference.expectedKind, DecisionPlanKind::Count) ||
                        !reference.requiredAssetVersion.IsValid() || !ValidSource(reference.source)) {
                        Add(AIErrors::DecisionAssetSchemaInvalid, {}, reference.source, asset.source, path);
                        continue;
                    }
                    if (reference.ownerNode.IsValid() && std::ranges::none_of(asset.nodes, [&reference](const auto &node) {
                        return node.id == reference.ownerNode;
                    })) {
                        Add(AIErrors::DecisionAssetSchemaInvalid, {}, reference.source, asset.source, path);
                        continue;
                    }
                    const auto *candidate = ResolveSubtree(reference, path);
                    if (candidate == nullptr)
                        continue;
                    planDependencies_.push_back(
                        {.asset = candidate->asset, .kind = candidate->kind, .schemaVersion = candidate->schemaVersion});
                    ValidateAsset(*candidate, depth + 1, false);
                }
            }

            void ValidateAsset(const DecisionAssetDescriptor &asset, const std::size_t depth, const bool rootAsset) {
                const std::string assetPath = std::format("asset[{}]", asset.asset.Value());
                if (!asset.asset.IsValid() || !IsKnown(asset.kind, DecisionPlanKind::Count) || asset.schemaVersion == 0 ||
                    !asset.blackboardSchema.IsValid() || !asset.requiredBlackboardSchemaVersion.IsValid() || !ValidSource(asset.source)) {
                    Add(AIErrors::DecisionAssetSchemaInvalid, {}, asset.source, root_.source, assetPath);
                    return;
                }
                if (asset.nodes.empty()) {
                    Add(AIErrors::DecisionAssetSchemaInvalid, "A decision asset must contain at least one node.", asset.source,
                        root_.source, assetPath + ".nodes");
                    return;
                }
                if (asset.nodes.size() > limits_.maximumNodesPerAsset || asset.subtrees.size() > limits_.maximumSubtreesPerAsset) {
                    Add(AIErrors::DecisionAssetLimitExceeded, {}, asset.source, root_.source, assetPath);
                    return;
                }
                if (depth > limits_.maximumDependencyDepth) {
                    Add(AIErrors::DecisionAssetLimitExceeded, {}, asset.source, root_.source, assetPath);
                    return;
                }
                if (std::ranges::find(activeAssets_, asset.asset) != activeAssets_.end()) {
                    Add(AIErrors::DecisionAssetDependencyCycle, {}, asset.source, root_.source, assetPath);
                    return;
                }
                if (std::ranges::find(validatedAssets_, asset.asset) != validatedAssets_.end())
                    return;
                if (validatedAssets_.size() + activeAssets_.size() >= limits_.maximumAssets) {
                    Add(AIErrors::DecisionAssetLimitExceeded, {}, asset.source, root_.source, assetPath);
                    return;
                }

                activeAssets_.push_back(asset.asset);
                if (rootAsset)
                    rootSchema_ = ResolveSchema(asset, assetPath + ".blackboardSchema");

                const auto schema = rootAsset ? rootSchema_ : ResolveSchema(asset, assetPath + ".blackboardSchema");
                ValidateAssetNodes(asset, schema, rootAsset, assetPath);
                ValidateSubtrees(asset, depth, assetPath);

                activeAssets_.pop_back();
                validatedAssets_.push_back(asset.asset);
            }

            const DecisionAssetDescriptor &root_;
            std::span<const DecisionAssetDescriptor> assetCatalog_;
            std::span<const DecisionNodeDescriptor> nodeDescriptors_;
            std::span<const std::shared_ptr<const BlackboardSchema>> schemas_;
            const DecisionAssetValidationLimits &limits_;
            std::vector<DecisionAssetValidationDiagnostic> diagnostics_;
            std::vector<DecisionGraphAssetId> activeAssets_;
            std::vector<DecisionGraphAssetId> validatedAssets_;
            std::size_t totalRequirements_{};
            bool diagnosticsTruncated_{};
            std::shared_ptr<const BlackboardSchema> rootSchema_;
            std::vector<DecisionPlanNode> planNodes_;
            std::vector<DecisionPlanBlackboardBinding> planBindings_;
            std::vector<DecisionPlanDependency> planDependencies_;
        };
    }  // namespace

    /** @copydoc DecisionAssetCompiler::Compile */
    Result<DecisionAssetCompilation> DecisionAssetCompiler::Compile(const DecisionAssetDescriptor &asset,
                                                                    const std::span<const DecisionAssetDescriptor> assetCatalog,
                                                                    const std::span<const DecisionNodeDescriptor> nodeDescriptors,
                                                                    const std::span<const std::shared_ptr<const BlackboardSchema>> schemas,
                                                                    const DecisionAssetValidationLimits &limits) {
        if (!ValidLimits(limits))
            return Failure<DecisionAssetCompilation>(AIErrors::DecisionAssetSchemaInvalid);
        if (assetCatalog.size() > limits.maximumAssets || nodeDescriptors.size() > limits.maximumNodeDescriptors ||
            schemas.size() > limits.maximumSchemas)
            return Failure<DecisionAssetCompilation>(AIErrors::DecisionAssetLimitExceeded);

        try {
            Validator validator{asset, assetCatalog, nodeDescriptors, schemas, limits};
            validator.ValidateRoot();
            if (validator.DiagnosticsTruncated())
                return Failure<DecisionAssetCompilation>(AIErrors::DecisionAssetLimitExceeded);

            auto diagnostics = std::move(validator).TakeDiagnostics();
            DecisionAssetValidationReport report{std::move(diagnostics)};
            if (report.HasErrors())
                return Result<DecisionAssetCompilation>::Success({.validation = std::move(report), .plan = {}});

            auto schema = std::move(validator).RootSchema();
            auto nodes = std::move(validator).TakePlanNodes();
            auto bindings = std::move(validator).TakePlanBindings();
            auto dependencies = std::move(validator).TakePlanDependencies();
            std::ranges::sort(nodes, {}, &DecisionPlanNode::id);
            const auto plan = std::make_shared<const DecisionAssetPlan>(DecisionAssetPlan::ConstructionData{
                .token = DecisionAssetPlan::ConstructionToken{},
                .asset = asset.asset,
                .kind = asset.kind,
                .schemaVersion = asset.schemaVersion,
                .blackboardSchema = std::move(schema),
                .nodes = std::move(nodes),
                .bindings = std::move(bindings),
                .dependencies = std::move(dependencies),
            });
            return Result<DecisionAssetCompilation>::Success({.validation = std::move(report), .plan = plan});
        } catch (const std::bad_alloc &) {
            return Failure<DecisionAssetCompilation>(AIErrors::DecisionAssetStorageUnavailable);
        }
    }

}  // namespace Horo::AI
