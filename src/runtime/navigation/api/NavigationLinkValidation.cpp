#include "Horo/Navigation/NavigationLinkValidation.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <tuple>
#include <utility>

namespace Horo::Navigation {
    namespace {
        /** @brief Uses double intermediates so finite float endpoints cannot overflow distance arithmetic. */
        [[nodiscard]] double Distance(const Math::Vec3 a, const Math::Vec3 b) noexcept {
            return std::hypot(static_cast<double>(a.x) - b.x, static_cast<double>(a.y) - b.y, static_cast<double>(a.z) - b.z);
        }

        /** @brief Rejects non-finite endpoints before comparisons, sorting or provider dispatch. */
        [[nodiscard]] bool ValidEndpoint(const NavigationBakeLinkEndpoint &endpoint) noexcept {
            return endpoint.surface.IsValid() && Math::IsFinite(endpoint.position) && std::isfinite(endpoint.connectionRadiusMeters) &&
                   endpoint.connectionRadiusMeters > 0.0F;
        }

        /** @brief Closed vocabulary validation shared by authored and generated candidates. */
        [[nodiscard]] bool ValidSemantics(const NavigationLinkKind kind, const NavigationLinkDirection direction) noexcept {
            return kind >= NavigationLinkKind::Jump && kind < NavigationLinkKind::Count &&
                   direction >= NavigationLinkDirection::StartToEnd && direction < NavigationLinkDirection::Count;
        }

        /** @brief Canonical finite endpoint key; ordered endpoint pairs never reverse authored direction. */
        [[nodiscard]] auto EndpointKey(const NavigationBakeLinkEndpoint &endpoint) noexcept {
            return std::tuple{endpoint.surface, endpoint.position.x, endpoint.position.y, endpoint.position.z,
                              endpoint.connectionRadiusMeters};
        }

        /** @brief Exact evidence key, independent of collision-owner container order. */
        [[nodiscard]] auto ClearanceKey(const NavigationLinkClearanceEvidence &evidence) noexcept {
            return std::tuple{evidence.profile, EndpointKey(evidence.start), EndpointKey(evidence.end)};
        }

        /** @brief Validates caller-lowered ceilings before any candidate-dependent allocation. */
        [[nodiscard]] bool ValidLimits(const NavigationLinkValidationLimits &limits) noexcept {
            return limits.maximumAuthoredLinks > 0 && limits.maximumAuthoredLinks <= NavigationLinkValidationLimits::MaximumAuthoredLinks &&
                   limits.maximumAnchors > 0 && limits.maximumAnchors <= NavigationLinkValidationLimits::MaximumAnchors &&
                   limits.maximumPairAttempts > 0 && limits.maximumPairAttempts <= NavigationLinkValidationLimits::MaximumPairAttempts &&
                   limits.maximumSuggestions > 0 && limits.maximumSuggestions <= NavigationLinkValidationLimits::MaximumSuggestions &&
                   limits.maximumClearanceRows > 0 && limits.maximumClearanceRows <= NavigationLinkValidationLimits::MaximumClearanceRows &&
                   limits.maximumWorkUnits > 0 && limits.maximumWorkUnits <= NavigationLinkValidationLimits::MaximumWorkUnits &&
                   limits.maximumOwnedBytes > 0 && limits.maximumOwnedBytes <= NavigationLinkValidationLimits::MaximumOwnedBytes;
        }

        /** @brief Charges deterministic work before execution and observes cancellation at every charge. */
        struct WorkBudget final {
            const CancellationToken &cancellation;
            std::uint64_t remaining;
            std::optional<Error> failure;

            [[nodiscard]] bool Charge(const std::uint64_t units = 1) {
                if (failure)
                    return false;
                if (cancellation.IsCancellationRequested())
                    failure = MakeError(NavigationErrors::BakeInputCancelled);
                else if (units > remaining)
                    failure = MakeError(NavigationErrors::BakeInputCapacityExceeded);
                else
                    remaining -= units;
                return !failure;
            }
        };

        /** @brief Local owned capture state; borrows never escape the synchronous validation call. */
        struct Validation final {
            const NavigationBakeInputSnapshot &input;
            const NavigationAreaRegistry &areas;
            const NavigationLinkProjectionContext &context;
            const INavigationQueryBackend &backend;
            const NavigationLinkValidationLimits &limits;
            WorkBudget work;
            std::uint64_t remainingOwnedBytes{};
            const NavigationResolvedBakeProfile *profile{};
            std::array<const NavigationTraversalDescriptor *, static_cast<std::size_t>(NavigationLinkKind::Count)> descriptors{};
            std::vector<const NavigationLinkClearanceEvidence *> clearance;
            std::vector<NavigationValidatedLink> authored;
            std::vector<NavigationValidatedLink> suggestions;
            std::vector<NavigationLinkDiagnostic> diagnostics;

            /** @brief Preserves cancellation precedence when owned diagnostic storage cannot be admitted. */
            [[nodiscard]] bool StorageFailure() {
                if (!work.failure)
                    work.failure = MakeError(NavigationErrors::BakeInputCapacityExceeded);
                return false;
            }

            /** @brief Bounds retained provider error text, diagnostics and cause chains before copying their owned storage. */
            [[nodiscard]] bool AdmitError(const Error &error) {
                std::uint64_t bytes{};
                const auto add = [&](const std::uint64_t count) {
                    if (count > remainingOwnedBytes / 2 - bytes)
                        return false;
                    bytes += count;
                    return true;
                };
                for (const Error *node = &error; node; node = node->cause.Get()) {
                    if (!work.Charge() || !add(sizeof(Error)) || !add(node->code.Value().size() + 1) ||
                        !add(node->domain.Value().size() + 1) || !add(node->message.size() + 1) ||
                        !add(node->diagnostics.size() * sizeof(Diagnostic))) {
                        return StorageFailure();
                    }
                    for (const auto &diagnostic : node->diagnostics) {
                        if (!work.Charge() || !add(diagnostic.code.Value().size() + 1) || !add(diagnostic.message.size() + 1) ||
                            !add(diagnostic.location.source.size() + 1) || !add(diagnostic.path.size() + 1)) {
                            return StorageFailure();
                        }
                    }
                }
                remainingOwnedBytes -= bytes * 2;
                return true;
            }

            /** @brief Resolves an exact surface/profile binding without accepting a provider's nearest foreign surface. */
            [[nodiscard]] const NavigationTileBuildPartition *Partition(const SurfaceId surface) {
                const auto partitions = input.Partitions();
                if (!work.Charge(1 + std::bit_width(partitions.size())))
                    return nullptr;
                const auto key = std::tuple{context.profile, surface};
                const auto found = std::ranges::lower_bound(partitions, key, {}, [](const auto &partition) {
                    return std::tuple{partition.profile, partition.surface};
                });
                return found != partitions.end() && found->profile == context.profile && found->surface == surface ? std::to_address(found)
                                                                                                                   : nullptr;
            }

            /** @brief Retains both endpoints and the original provider error for one explicit failing rule. */
            void Reject(const NavigationBakeLinkInput &link, const NavigationLinkAnchorId startAnchor,
                        const NavigationLinkAnchorId endAnchor, const NavigationLinkValidationRule rule, std::optional<Error> cause = {}) {
                diagnostics.push_back({.authoredLink = link.id,
                                       .startAnchor = startAnchor,
                                       .endAnchor = endAnchor,
                                       .start = link.start,
                                       .end = link.end,
                                       .rule = rule,
                                       .cause = std::move(cause)});
            }

            /** @brief Explicit ordered projected corridor, including reverse traversal when declared. */
            struct OrderedEndpoints final {
                NavigationBakeLinkEndpoint start;
                NavigationBakeLinkEndpoint end;
            };

            /** @brief Requires exact collision-owner evidence for each admitted traversal direction. */
            [[nodiscard]] bool HasClearance(const OrderedEndpoints &traversal) {
                if (!work.Charge(1 + std::bit_width(clearance.size())))
                    return false;
                const auto key = std::tuple{context.profile, EndpointKey(traversal.start), EndpointKey(traversal.end)};
                const auto found = std::ranges::lower_bound(clearance, key, {}, [](const auto *evidence) {
                    return ClearanceKey(*evidence);
                });
                return found != clearance.end() && ClearanceKey(**found) == key &&
                       (*found)->radiusMeters >= profile->buildGeometry.radiusMeters &&
                       (*found)->heightMeters >= profile->buildGeometry.heightMeters;
            }

            /** @brief Projects and verifies finite, exact-generation, exact-surface, filter-compatible endpoint evidence. */
            [[nodiscard]] Result<NavigationSurfaceHit> Project(const NavigationBakeLinkEndpoint &endpoint,
                                                               const NavigationTileBuildPartition &partition) {
                if (!work.Charge(context.requirement.limits.maximumNodeExpansions))
                    return Result<NavigationSurfaceHit>::Failure(*work.failure);
                const float radius = endpoint.connectionRadiusMeters;
                auto result = backend.ProjectPoint({.world = context.world,
                                                    .topology = context.topology,
                                                    .point = endpoint.position,
                                                    .halfExtents = {radius, radius, radius},
                                                    .requirement = context.requirement},
                                                   work.cancellation);
                if (!work.Charge())
                    return Result<NavigationSurfaceHit>::Failure(*work.failure);
                if (result.HasError())
                    return Result<NavigationSurfaceHit>::Failure(std::move(result).ErrorValue());
                const auto &hit = result.Value().hit;
                if (!Math::IsFinite(hit.position) || !Math::IsFinite(hit.normal) || !std::isfinite(hit.distanceMeters) ||
                    hit.distanceMeters < 0.0F || hit.surface != endpoint.surface || hit.provenance.surface != endpoint.surface ||
                    hit.provenance.world != context.world || hit.provenance.topology != context.topology ||
                    hit.provenance.polygonIndex == NavigationPathNoPolygon || !Math::NearlyEqual(Math::Length(hit.normal), 1.0F, 0.001F) ||
                    Distance(endpoint.position, hit.position) > radius)
                    return Result<NavigationSurfaceHit>::Failure(MakeError(NavigationErrors::StaleSnapshot));
                const auto traversal = areas.ResolveTraversal(partition.filter, hit.area);
                if (traversal.HasError())
                    return Result<NavigationSurfaceHit>::Failure(traversal.ErrorValue());
                if (!traversal.Value().traversable)
                    return Result<NavigationSurfaceHit>::Failure(MakeError(NavigationErrors::OperationUnsupported));
                return Result<NavigationSurfaceHit>::Success(hit);
            }

            /** @brief Detects overlapping directed traversals while keeping opposite one-way transitions distinct. */
            [[nodiscard]] bool Duplicates(const NavigationValidatedLink &candidate,
                                          const NavigationValidatedLink &existing) const noexcept {
                const double radius = std::min(candidate.radiusMeters, existing.radiusMeters);
                const auto sameEndpoint = [radius](const NavigationSurfaceHit &left, const NavigationSurfaceHit &right) {
                    return left.surface == right.surface && left.provenance.polygonIndex == right.provenance.polygonIndex &&
                           Distance(left.position, right.position) <= radius;
                };
                if (sameEndpoint(candidate.start, existing.start) && sameEndpoint(candidate.end, existing.end))
                    return true;
                return (candidate.direction == NavigationLinkDirection::Bidirectional ||
                        existing.direction == NavigationLinkDirection::Bidirectional) &&
                       sameEndpoint(candidate.start, existing.end) && sameEndpoint(candidate.end, existing.start);
            }

            /** @brief Stack-only state for one ordered candidate, never retained by the snapshot. */
            struct CandidateState final {
                const NavigationBakeLinkInput &link;
                NavigationLinkAnchorId startAnchor;
                NavigationLinkAnchorId endAnchor;
                float maximumDistance;
                const NavigationTileBuildPartition *startPartition{};
                const NavigationTileBuildPartition *endPartition{};
                const NavigationTraversalDescriptor *descriptor{};
                NavigationSurfaceHit start;
                NavigationSurfaceHit end;
            };

            /** @brief Records a candidate rejection without changing its authored or generated identity. */
            void Reject(const CandidateState &candidate, const NavigationLinkValidationRule rule) {
                Reject(candidate.link, candidate.startAnchor, candidate.endAnchor, rule);
            }

            /** @brief Resolves payload, profile and available traversal semantics before querying either endpoint. */
            [[nodiscard]] bool PrepareCandidate(CandidateState &candidate) {
                using enum NavigationLinkValidationRule;
                const auto &link = candidate.link;
                if (!ValidEndpoint(link.start) || !ValidEndpoint(link.end) || !ValidSemantics(link.kind, link.direction) ||
                    !std::isfinite(link.traversalCost) || link.traversalCost < 0.0F) {
                    Reject(candidate, Malformed);
                    return false;
                }
                candidate.startPartition = Partition(link.start.surface);
                candidate.endPartition = Partition(link.end.surface);
                if (link.profile != context.profile || !candidate.startPartition || !candidate.endPartition) {
                    Reject(candidate, ProfileMismatch);
                    return false;
                }
                candidate.descriptor = descriptors[static_cast<std::size_t>(link.kind)];
                if (!candidate.descriptor) {
                    Reject(candidate, DescriptorUnavailable);
                    return false;
                }
                const auto startArea = areas.ResolveTraversal(candidate.startPartition->filter, candidate.descriptor->area);
                const auto endArea = areas.ResolveTraversal(candidate.endPartition->filter, candidate.descriptor->area);
                if (startArea.HasError() || endArea.HasError() || !startArea.Value().traversable || !endArea.Value().traversable) {
                    Reject(candidate, DescriptorUnavailable);
                    return false;
                }
                if (startArea.Value().traversalCost != link.traversalCost || endArea.Value().traversalCost != link.traversalCost) {
                    Reject(candidate, TraversalCost);
                    return false;
                }
                if (link.direction == NavigationLinkDirection::Bidirectional && !candidate.descriptor->supportsBidirectional) {
                    Reject(candidate, Direction);
                    return false;
                }
                return true;
            }

            /** @brief Projects both endpoints and retains each failed endpoint's exact typed error. */
            [[nodiscard]] bool ProjectCandidate(CandidateState &candidate) {
                const auto start = Project(candidate.link.start, *candidate.startPartition);
                const auto end = Project(candidate.link.end, *candidate.endPartition);
                if (work.failure || (start.HasError() && !AdmitError(start.ErrorValue())) ||
                    (end.HasError() && !AdmitError(end.ErrorValue())))
                    return false;
                if (start.HasError())
                    Reject(candidate.link, candidate.startAnchor, candidate.endAnchor, NavigationLinkValidationRule::StartProjection,
                           start.ErrorValue());
                if (end.HasError())
                    Reject(candidate.link, candidate.startAnchor, candidate.endAnchor, NavigationLinkValidationRule::EndProjection,
                           end.ErrorValue());
                if (start.HasError() || end.HasError())
                    return false;
                candidate.start = start.Value();
                candidate.end = end.Value();
                return true;
            }

            /** @brief Checks final projected distance and asymmetric rise/drop in every declared direction. */
            [[nodiscard]] bool CheckTraversal(const CandidateState &candidate) {
                const auto &descriptor = *candidate.descriptor;
                const double distance = Distance(candidate.start.position, candidate.end.position);
                const double rise = static_cast<double>(candidate.end.position.y) - candidate.start.position.y;
                if (distance <= Math::DefaultEpsilon) {
                    Reject(candidate, NavigationLinkValidationRule::CoincidentEndpoints);
                    return false;
                }
                const bool forward = distance <= std::min(descriptor.maximumDistanceMeters, candidate.maximumDistance) &&
                                     rise <= descriptor.maximumRiseMeters && -rise <= descriptor.maximumDropMeters;
                if (const bool reverse = candidate.link.direction != NavigationLinkDirection::Bidirectional ||
                                         (-rise <= descriptor.maximumRiseMeters && rise <= descriptor.maximumDropMeters);
                    !forward || !reverse) {
                    Reject(candidate, NavigationLinkValidationRule::Direction);
                    return false;
                }
                return true;
            }

            /** @brief Requires measured free clearance for the final projected forward and optional reverse corridor. */
            [[nodiscard]] bool CheckClearance(const CandidateState &candidate) {
                auto start = candidate.link.start;
                auto end = candidate.link.end;
                start.position = candidate.start.position;
                end.position = candidate.end.position;
                if (start.connectionRadiusMeters < profile->buildGeometry.radiusMeters ||
                    end.connectionRadiusMeters < profile->buildGeometry.radiusMeters || !HasClearance({.start = start, .end = end}) ||
                    (candidate.link.direction == NavigationLinkDirection::Bidirectional && !HasClearance({.start = end, .end = start}))) {
                    Reject(candidate, NavigationLinkValidationRule::Clearance);
                    return false;
                }
                return true;
            }

            /** @brief Checks admitted directed duplicates against both independently retained acceptance tables. */
            [[nodiscard]] bool IsDuplicate(const NavigationValidatedLink &candidate) {
                for (const auto rows :
                     {std::span<const NavigationValidatedLink>{authored}, std::span<const NavigationValidatedLink>{suggestions}}) {
                    for (const auto &existing : rows) {
                        if (!work.Charge())
                            return false;
                        if (Duplicates(candidate, existing))
                            return true;
                    }
                }
                return false;
            }

            /** @brief Validates one complete candidate while keeping suggestion ownership separate from authored acceptance. */
            void Candidate(const NavigationBakeLinkInput &link, const NavigationLinkAnchorId startAnchor = {},
                           const NavigationLinkAnchorId endAnchor = {}, const float maximumDistance = std::numeric_limits<float>::max()) {
                if (!work.Charge())
                    return;
                CandidateState candidate{.link = link,
                                         .startAnchor = startAnchor,
                                         .endAnchor = endAnchor,
                                         .maximumDistance = maximumDistance};
                if (!PrepareCandidate(candidate) || !ProjectCandidate(candidate) || !CheckTraversal(candidate) ||
                    !CheckClearance(candidate))
                    return;
                const NavigationValidatedLink accepted{.authoredLink = link.id,
                                                       .startAnchor = startAnchor,
                                                       .endAnchor = endAnchor,
                                                       .start = candidate.start,
                                                       .end = candidate.end,
                                                       .kind = link.kind,
                                                       .direction = link.direction,
                                                       .area = candidate.descriptor->area,
                                                       .radiusMeters =
                                                           std::min(link.start.connectionRadiusMeters, link.end.connectionRadiusMeters)};
                if (IsDuplicate(accepted)) {
                    Reject(candidate, NavigationLinkValidationRule::DuplicateTraversal);
                    return;
                }
                if (work.failure)
                    return;
                if (link.id.IsValid())
                    authored.push_back(accepted);
                else if (suggestions.size() < limits.maximumSuggestions)
                    suggestions.push_back(accepted);
                else
                    work.failure = MakeError(NavigationErrors::BakeInputCapacityExceeded);
            }
        };

        /** @brief Validates all descriptors without default selection or duplicate-kind precedence. */
        [[nodiscard]] Result<void> CaptureDescriptors(Validation &validation,
                                                      const std::span<const NavigationTraversalDescriptor> descriptors) {
            for (const auto &descriptor : descriptors) {
                if (!ValidSemantics(descriptor.kind, NavigationLinkDirection::StartToEnd) ||
                    descriptor.profile != validation.context.profile || !descriptor.area.IsValid() ||
                    !std::isfinite(descriptor.maximumDistanceMeters) || descriptor.maximumDistanceMeters <= 0.0F ||
                    !std::isfinite(descriptor.maximumRiseMeters) || descriptor.maximumRiseMeters < 0.0F ||
                    !std::isfinite(descriptor.maximumDropMeters) || descriptor.maximumDropMeters < 0.0F ||
                    validation.areas.ResolveArea(descriptor.area).HasError())
                    return Result<void>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
                auto &entry = validation.descriptors[static_cast<std::size_t>(descriptor.kind)];
                if (entry)
                    return Result<void>::Failure(MakeError(NavigationErrors::DescriptorConflict));
                entry = &descriptor;
            }
            return Result<void>::Success();
        }

        /** @brief Captures exact clearance keys, rejecting stale or ambiguous collision observations. */
        [[nodiscard]] Result<void> CaptureClearance(Validation &validation,
                                                    const std::span<const NavigationLinkClearanceEvidence> evidence) {
            for (const auto &row : evidence) {
                if (!ValidEndpoint(row.start) || !ValidEndpoint(row.end) || !row.profile.IsValid() || !std::isfinite(row.radiusMeters) ||
                    row.radiusMeters < 0.0F || !std::isfinite(row.heightMeters) || row.heightMeters < 0.0F)
                    return Result<void>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
                if (row.bakeFingerprint != validation.input.Fingerprint())
                    return Result<void>::Failure(MakeError(NavigationErrors::BakeInputStale));
                validation.clearance.push_back(&row);
            }
            std::ranges::sort(validation.clearance, {}, [](const auto *row) {
                return ClearanceKey(*row);
            });
            if (std::ranges::adjacent_find(validation.clearance, [](const auto *left, const auto *right) {
                return ClearanceKey(*left) == ClearanceKey(*right);
            }) != validation.clearance.end())
                return Result<void>::Failure(MakeError(NavigationErrors::DescriptorConflict));
            return Result<void>::Success();
        }

        /** @brief Admits one ordered stable anchor pair before shared candidate validation. */
        [[nodiscard]] Result<void> ProposePair(Validation &validation, const NavigationLinkGenerationPolicy &policy,
                                               const NavigationLinkGenerationAnchor &start, const NavigationLinkGenerationAnchor &end,
                                               std::uint32_t &attempts, const float traversalCost) {
            if (start.id == end.id || (policy.direction == NavigationLinkDirection::Bidirectional && end.id < start.id))
                return Result<void>::Success();
            if (!validation.work.Charge())
                return Result<void>::Failure(*validation.work.failure);
            if (++attempts > validation.limits.maximumPairAttempts)
                return Result<void>::Failure(MakeError(NavigationErrors::BakeInputCapacityExceeded));
            if (start.endpoint.surface == end.endpoint.surface ||
                Distance(start.endpoint.position, end.endpoint.position) > policy.maximumDistanceMeters)
                return Result<void>::Success();
            validation.Candidate({.profile = validation.context.profile,
                                  .start = start.endpoint,
                                  .end = end.endpoint,
                                  .kind = policy.kind,
                                  .direction = policy.direction,
                                  .traversalCost = traversalCost},
                                 start.id, end.id, policy.maximumDistanceMeters);
            return validation.work.failure ? Result<void>::Failure(*validation.work.failure) : Result<void>::Success();
        }

        /** @brief Generates all eligible ordered pairs with explicit fail-closed attempt/output/work bounds. */
        [[nodiscard]] Result<void> Generate(Validation &validation, const NavigationLinkGenerationPolicy &policy) {
            if (!ValidSemantics(policy.kind, policy.direction) || !std::isfinite(policy.maximumDistanceMeters) ||
                policy.maximumDistanceMeters <= 0.0F)
                return Result<void>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
            std::vector<NavigationLinkGenerationAnchor> anchors{policy.anchors.begin(), policy.anchors.end()};
            for (const auto &anchor : anchors) {
                if (!anchor.id.IsValid() || !ValidEndpoint(anchor.endpoint))
                    return Result<void>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
            }
            std::ranges::sort(anchors, {}, &NavigationLinkGenerationAnchor::id);
            if (std::ranges::adjacent_find(anchors, {}, &NavigationLinkGenerationAnchor::id) != anchors.end())
                return Result<void>::Failure(MakeError(NavigationErrors::DescriptorConflict));
            const auto *descriptor = validation.descriptors[static_cast<std::size_t>(policy.kind)];
            const float traversalCost = descriptor ? validation.areas.ResolveArea(descriptor->area).Value().traversalCost : 1.0F;
            std::uint32_t attempts{};
            for (const auto &start : anchors) {
                for (const auto &end : anchors) {
                    if (const auto proposed = ProposePair(validation, policy, start, end, attempts, traversalCost); proposed.HasError())
                        return proposed;
                }
            }
            return Result<void>::Success();
        }

        /** @brief Precharges bounded sorting, scans, capture and output capacities before allocating scratch/results. */
        [[nodiscard]] Result<void> AdmitStorage(Validation &validation, const std::size_t authoredCount, const std::size_t descriptorCount,
                                                const std::size_t clearanceCount, const std::size_t anchorCount) {
            const auto &limits = validation.limits;
            if (authoredCount > limits.maximumAuthoredLinks || descriptorCount > validation.descriptors.size() ||
                clearanceCount > limits.maximumClearanceRows || anchorCount > limits.maximumAnchors)
                return Result<void>::Failure(MakeError(NavigationErrors::BakeInputCapacityExceeded));
            const std::uint64_t pairs = anchorCount * (anchorCount > 0 ? anchorCount - 1 : 0);
            const std::uint64_t candidates = authoredCount + std::min<std::uint64_t>(pairs, limits.maximumPairAttempts);
            const std::uint64_t ownedBytes =
                authoredCount * (sizeof(NavigationBakeLinkInput) + sizeof(NavigationValidatedLink)) +
                limits.maximumSuggestions * sizeof(NavigationValidatedLink) + 2 * candidates * sizeof(NavigationLinkDiagnostic) +
                clearanceCount * sizeof(const NavigationLinkClearanceEvidence *) + anchorCount * sizeof(NavigationLinkGenerationAnchor);
            if (ownedBytes > limits.maximumOwnedBytes)
                return Result<void>::Failure(MakeError(NavigationErrors::BakeInputCapacityExceeded));
            validation.remainingOwnedBytes = limits.maximumOwnedBytes - ownedBytes;
            if (const std::uint64_t sortingWork =
                    authoredCount * (2 + std::bit_width(authoredCount)) + clearanceCount * (2 + std::bit_width(clearanceCount)) +
                    anchorCount * (2 + std::bit_width(anchorCount)) + descriptorCount + validation.input.Profiles().size();
                !validation.work.Charge(sortingWork))
                return Result<void>::Failure(*validation.work.failure);
            validation.authored.reserve(authoredCount);
            validation.suggestions.reserve(limits.maximumSuggestions);
            validation.diagnostics.reserve(2 * candidates);
            validation.clearance.reserve(clearanceCount);
            return Result<void>::Success();
        }

        /** @brief Resolves immutable validation tables after complete count/work/storage admission. */
        [[nodiscard]] Result<void> PrepareValidation(Validation &validation, const NavigationLinkValidationRequest &request) {
            if (const auto storage = AdmitStorage(validation, request.authored.size(), request.descriptors.size(), request.clearance.size(),
                                                  request.generation ? request.generation->anchors.size() : 0);
                storage.HasError())
                return storage;
            const auto profiles = request.input.Profiles();
            const auto profile = std::ranges::find(profiles, request.context.profile, &NavigationResolvedBakeProfile::id);
            if (profile == profiles.end())
                return Result<void>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
            validation.profile = std::to_address(profile);
            if (const auto descriptors = CaptureDescriptors(validation, request.descriptors); descriptors.HasError())
                return descriptors;
            return CaptureClearance(validation, request.clearance);
        }

        /** @brief Rejects every ambiguous authored identity and validates the remaining rows in stable identity order. */
        void ValidateAuthored(Validation &validation, const std::span<const NavigationBakeLinkInput> authored) {
            std::vector<NavigationBakeLinkInput> links{authored.begin(), authored.end()};
            std::ranges::sort(links, {}, &NavigationBakeLinkInput::id);
            for (std::size_t index = 0; index < links.size(); ++index) {
                const auto &link = links[index];
                if (!validation.work.Charge())
                    break;
                const bool duplicate =
                    (index > 0 && links[index - 1].id == link.id) || (index + 1 < links.size() && links[index + 1].id == link.id);
                if (!link.id.IsValid())
                    validation.Reject(link, {}, {}, NavigationLinkValidationRule::Malformed);
                else if (duplicate)
                    validation.Reject(link, {}, {}, NavigationLinkValidationRule::DuplicateIdentity);
                else
                    validation.Candidate(link);
            }
        }
    }  // namespace

    /** @copydoc NavigationLinkValidationSnapshot::Validate */
    Result<NavigationLinkValidationSnapshot> NavigationLinkValidationSnapshot::Validate(const NavigationLinkValidationRequest &request,
                                                                                        const CancellationToken &cancellation) {
        const auto &context = request.context;
        if (!ValidLimits(request.limits) || !context.profile.IsValid() || !context.world.IsValid() || !context.topology.IsValid() ||
            context.requirement.query != NavigationQueryKind::NearestPoint || context.requirement.limits.maximumResultPoints != 1)
            return Result<NavigationLinkValidationSnapshot>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
        const auto capabilities = request.backend.Capabilities();
        if (const auto admitted = AdmitNavigationQuery(capabilities, capabilities.revision, context.requirement); admitted.HasError())
            return Result<NavigationLinkValidationSnapshot>::Failure(admitted.ErrorValue());
        Validation validation{.input = request.input,
                              .areas = request.areas,
                              .context = context,
                              .backend = request.backend,
                              .limits = request.limits,
                              .work = {.cancellation = cancellation, .remaining = request.limits.maximumWorkUnits}};
        if (const auto prepared = PrepareValidation(validation, request); prepared.HasError())
            return Result<NavigationLinkValidationSnapshot>::Failure(prepared.ErrorValue());
        ValidateAuthored(validation, request.authored);
        if (validation.work.failure)
            return Result<NavigationLinkValidationSnapshot>::Failure(*validation.work.failure);
        if (request.generation) {
            if (const auto generated = Generate(validation, *request.generation); generated.HasError())
                return Result<NavigationLinkValidationSnapshot>::Failure(generated.ErrorValue());
        }
        if (!validation.work.Charge())
            return Result<NavigationLinkValidationSnapshot>::Failure(*validation.work.failure);
        if (request.backend.Capabilities().revision != capabilities.revision)
            return Result<NavigationLinkValidationSnapshot>::Failure(MakeError(NavigationErrors::CapabilityStale));
        NavigationLinkValidationSnapshot snapshot{request.input, context};
        snapshot.authored_ = std::move(validation.authored);
        snapshot.suggestions_ = std::move(validation.suggestions);
        snapshot.diagnostics_ = std::move(validation.diagnostics);
        return Result<NavigationLinkValidationSnapshot>::Success(std::move(snapshot));
    }
}  // namespace Horo::Navigation
