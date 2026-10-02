#include "Horo/Navigation/NavigationLinkValidation.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
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
                        if (!work.failure)
                            work.failure = MakeError(NavigationErrors::BakeInputCapacityExceeded);
                        return false;
                    }
                    for (const auto &diagnostic : node->diagnostics) {
                        if (!work.Charge() || !add(diagnostic.code.Value().size() + 1) || !add(diagnostic.message.size() + 1) ||
                            !add(diagnostic.location.source.size() + 1) || !add(diagnostic.path.size() + 1)) {
                            if (!work.failure)
                                work.failure = MakeError(NavigationErrors::BakeInputCapacityExceeded);
                            return false;
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
                return found != partitions.end() && found->profile == context.profile && found->surface == surface ? &*found : nullptr;
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

            /** @brief Requires exact collision-owner evidence for each admitted traversal direction. */
            [[nodiscard]] bool HasClearance(const NavigationBakeLinkEndpoint &start, const NavigationBakeLinkEndpoint &end) {
                if (!work.Charge(1 + std::bit_width(clearance.size())))
                    return false;
                const auto key = std::tuple{context.profile, EndpointKey(start), EndpointKey(end)};
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

            /** @brief Validates one complete candidate; rejected suggestions never join the authored acceptance table. */
            void Candidate(const NavigationBakeLinkInput &link, const NavigationLinkAnchorId startAnchor = {},
                           const NavigationLinkAnchorId endAnchor = {}, const float maximumDistance = std::numeric_limits<float>::max()) {
                using enum NavigationLinkValidationRule;
                const auto reject = [&](const NavigationLinkValidationRule rule) {
                    Reject(link, startAnchor, endAnchor, rule);
                };
                if (!work.Charge())
                    return;
                if (!ValidEndpoint(link.start) || !ValidEndpoint(link.end) || !ValidSemantics(link.kind, link.direction) ||
                    !std::isfinite(link.traversalCost) || link.traversalCost < 0.0F) {
                    reject(Malformed);
                    return;
                }
                const auto *startPartition = Partition(link.start.surface);
                const auto *endPartition = Partition(link.end.surface);
                if (link.profile != context.profile || !startPartition || !endPartition) {
                    reject(ProfileMismatch);
                    return;
                }
                const auto *descriptor = descriptors[static_cast<std::size_t>(link.kind)];
                if (!descriptor) {
                    reject(DescriptorUnavailable);
                    return;
                }
                const auto startArea = areas.ResolveTraversal(startPartition->filter, descriptor->area);
                const auto endArea = areas.ResolveTraversal(endPartition->filter, descriptor->area);
                if (startArea.HasError() || endArea.HasError() || !startArea.Value().traversable || !endArea.Value().traversable) {
                    reject(DescriptorUnavailable);
                    return;
                }
                if (startArea.Value().traversalCost != link.traversalCost || endArea.Value().traversalCost != link.traversalCost) {
                    reject(TraversalCost);
                    return;
                }
                if (link.direction == NavigationLinkDirection::Bidirectional && !descriptor->supportsBidirectional) {
                    reject(Direction);
                    return;
                }
                const auto start = Project(link.start, *startPartition);
                const auto end = Project(link.end, *endPartition);
                if (work.failure)
                    return;
                if ((start.HasError() && !AdmitError(start.ErrorValue())) || (end.HasError() && !AdmitError(end.ErrorValue())))
                    return;
                if (start.HasError())
                    Reject(link, startAnchor, endAnchor, StartProjection, start.ErrorValue());
                if (end.HasError())
                    Reject(link, startAnchor, endAnchor, EndProjection, end.ErrorValue());
                if (start.HasError() || end.HasError())
                    return;
                const double distance = Distance(start.Value().position, end.Value().position);
                const double rise = static_cast<double>(end.Value().position.y) - start.Value().position.y;
                if (distance <= Math::DefaultEpsilon) {
                    reject(CoincidentEndpoints);
                    return;
                }
                const bool forward = distance <= std::min(descriptor->maximumDistanceMeters, maximumDistance) &&
                                     rise <= descriptor->maximumRiseMeters && -rise <= descriptor->maximumDropMeters;
                const bool reverse = link.direction != NavigationLinkDirection::Bidirectional ||
                                     (-rise <= descriptor->maximumRiseMeters && rise <= descriptor->maximumDropMeters);
                if (!forward || !reverse) {
                    reject(Direction);
                    return;
                }
                auto projectedStart = link.start;
                auto projectedEnd = link.end;
                projectedStart.position = start.Value().position;
                projectedEnd.position = end.Value().position;
                if (link.start.connectionRadiusMeters < profile->buildGeometry.radiusMeters ||
                    link.end.connectionRadiusMeters < profile->buildGeometry.radiusMeters || !HasClearance(projectedStart, projectedEnd) ||
                    (link.direction == NavigationLinkDirection::Bidirectional && !HasClearance(projectedEnd, projectedStart))) {
                    reject(Clearance);
                    return;
                }
                NavigationValidatedLink accepted{.authoredLink = link.id,
                                                 .startAnchor = startAnchor,
                                                 .endAnchor = endAnchor,
                                                 .start = start.Value(),
                                                 .end = end.Value(),
                                                 .kind = link.kind,
                                                 .direction = link.direction,
                                                 .area = descriptor->area,
                                                 .radiusMeters =
                                                     std::min(link.start.connectionRadiusMeters, link.end.connectionRadiusMeters)};
                for (const auto rows :
                     {std::span<const NavigationValidatedLink>{authored}, std::span<const NavigationValidatedLink>{suggestions}}) {
                    for (const auto &existing : rows) {
                        if (!work.Charge())
                            return;
                        if (Duplicates(accepted, existing)) {
                            reject(DuplicateTraversal);
                            return;
                        }
                    }
                }
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
            std::uint32_t attempts{};
            for (std::size_t start = 0; start < anchors.size(); ++start) {
                for (std::size_t end = 0; end < anchors.size(); ++end) {
                    if (start == end || (policy.direction == NavigationLinkDirection::Bidirectional && end < start))
                        continue;
                    if (!validation.work.Charge())
                        return Result<void>::Failure(*validation.work.failure);
                    if (++attempts > validation.limits.maximumPairAttempts)
                        return Result<void>::Failure(MakeError(NavigationErrors::BakeInputCapacityExceeded));
                    if (anchors[start].endpoint.surface == anchors[end].endpoint.surface ||
                        Distance(anchors[start].endpoint.position, anchors[end].endpoint.position) > policy.maximumDistanceMeters)
                        continue;
                    validation.Candidate({.profile = validation.context.profile,
                                          .start = anchors[start].endpoint,
                                          .end = anchors[end].endpoint,
                                          .kind = policy.kind,
                                          .direction = policy.direction,
                                          .traversalCost =
                                              validation.descriptors[static_cast<std::size_t>(policy.kind)]
                                                  ? validation.areas
                                                        .ResolveArea(validation.descriptors[static_cast<std::size_t>(policy.kind)]->area)
                                                        .Value()
                                                        .traversalCost
                                                  : 1.0F},
                                         anchors[start].id, anchors[end].id, policy.maximumDistanceMeters);
                    if (validation.work.failure)
                        return Result<void>::Failure(*validation.work.failure);
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
            const std::uint64_t sortingWork =
                authoredCount * (2 + std::bit_width(authoredCount)) + clearanceCount * (2 + std::bit_width(clearanceCount)) +
                anchorCount * (2 + std::bit_width(anchorCount)) + descriptorCount + validation.input.Profiles().size();
            if (!validation.work.Charge(sortingWork))
                return Result<void>::Failure(*validation.work.failure);
            validation.authored.reserve(authoredCount);
            validation.suggestions.reserve(limits.maximumSuggestions);
            validation.diagnostics.reserve(2 * candidates);
            validation.clearance.reserve(clearanceCount);
            return Result<void>::Success();
        }

        /** @brief Fixed-width big-endian hashing excludes runtime handles and operation generation from the cook key. */
        void HashU64(Sha256Builder &hash, const std::uint64_t value) noexcept {
            std::array<std::byte, 8> bytes{};
            for (std::size_t index = 0; index < bytes.size(); ++index)
                bytes[index] = static_cast<std::byte>((value >> ((7 - index) * 8)) & 0xFFU);
            static_cast<void>(hash.Update(bytes));
        }

        /** @brief Canonical float hashing treats signed zero as one semantic value. */
        void HashPoint(Sha256Builder &hash, const Math::Vec3 point) noexcept {
            for (const float value : {point.x, point.y, point.z})
                HashU64(hash, std::bit_cast<std::uint32_t>(value == 0.0F ? 0.0F : value));
        }

        /** @brief Adds portable link semantics and stable source identities to the cook dependency key. */
        void HashLink(Sha256Builder &hash, const NavigationValidatedLink &link) noexcept {
            HashU64(hash, link.authoredLink.Value());
            HashU64(hash, link.startAnchor.Value());
            HashU64(hash, link.endAnchor.Value());
            HashU64(hash, link.start.surface.Value());
            HashU64(hash, link.end.surface.Value());
            HashPoint(hash, link.start.position);
            HashPoint(hash, link.end.position);
            HashU64(hash, link.start.provenance.polygonIndex);
            HashU64(hash, link.end.provenance.polygonIndex);
            HashU64(hash, static_cast<std::uint8_t>(link.kind));
            HashU64(hash, static_cast<std::uint8_t>(link.direction));
            HashU64(hash, link.area.Value());
            HashU64(hash, std::bit_cast<std::uint32_t>(link.radiusMeters));
        }
    }  // namespace

    /** @copydoc NavigationLinkValidationSnapshot::Validate */
    Result<NavigationLinkValidationSnapshot> NavigationLinkValidationSnapshot::Validate(
        const NavigationBakeInputSnapshot &input, const NavigationAreaRegistry &areas, const NavigationLinkProjectionContext &context,
        const INavigationQueryBackend &backend, const std::span<const NavigationBakeLinkInput> authored,
        const std::span<const NavigationTraversalDescriptor> descriptors, const std::span<const NavigationLinkClearanceEvidence> clearance,
        const std::optional<NavigationLinkGenerationPolicy> &generation, const CancellationToken &cancellation,
        const NavigationLinkValidationLimits &limits) {
        if (!ValidLimits(limits) || !context.profile.IsValid() || !context.world.IsValid() || !context.topology.IsValid() ||
            context.requirement.query != NavigationQueryKind::NearestPoint || context.requirement.limits.maximumResultPoints != 1)
            return Result<NavigationLinkValidationSnapshot>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
        const auto capabilities = backend.Capabilities();
        const auto admitted = AdmitNavigationQuery(capabilities, capabilities.revision, context.requirement);
        if (admitted.HasError())
            return Result<NavigationLinkValidationSnapshot>::Failure(admitted.ErrorValue());
        Validation validation{.input = input,
                              .areas = areas,
                              .context = context,
                              .backend = backend,
                              .limits = limits,
                              .work = {.cancellation = cancellation, .remaining = limits.maximumWorkUnits}};
        const auto storage =
            AdmitStorage(validation, authored.size(), descriptors.size(), clearance.size(), generation ? generation->anchors.size() : 0);
        if (storage.HasError())
            return Result<NavigationLinkValidationSnapshot>::Failure(storage.ErrorValue());
        const auto profiles = input.Profiles();
        const auto profile = std::ranges::find(profiles, context.profile, &NavigationResolvedBakeProfile::id);
        if (profile == profiles.end())
            return Result<NavigationLinkValidationSnapshot>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
        validation.profile = &*profile;
        const auto descriptorCapture = CaptureDescriptors(validation, descriptors);
        if (descriptorCapture.HasError())
            return Result<NavigationLinkValidationSnapshot>::Failure(descriptorCapture.ErrorValue());
        const auto clearanceCapture = CaptureClearance(validation, clearance);
        if (clearanceCapture.HasError())
            return Result<NavigationLinkValidationSnapshot>::Failure(clearanceCapture.ErrorValue());
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
        if (validation.work.failure)
            return Result<NavigationLinkValidationSnapshot>::Failure(*validation.work.failure);
        if (generation) {
            const auto generated = Generate(validation, *generation);
            if (generated.HasError())
                return Result<NavigationLinkValidationSnapshot>::Failure(generated.ErrorValue());
        }
        if (!validation.work.Charge())
            return Result<NavigationLinkValidationSnapshot>::Failure(*validation.work.failure);
        if (backend.Capabilities().revision != capabilities.revision)
            return Result<NavigationLinkValidationSnapshot>::Failure(MakeError(NavigationErrors::CapabilityStale));
        NavigationLinkValidationSnapshot snapshot{input, context};
        snapshot.authored_ = std::move(validation.authored);
        snapshot.suggestions_ = std::move(validation.suggestions);
        snapshot.diagnostics_ = std::move(validation.diagnostics);
        return Result<NavigationLinkValidationSnapshot>::Success(std::move(snapshot));
    }

    NavigationLinkValidationSnapshot::NavigationLinkValidationSnapshot(const NavigationBakeInputSnapshot &input,
                                                                       const NavigationLinkProjectionContext &context)
        : revisions_(input.Revisions()), fingerprint_(input.Fingerprint()), context_(context) {}

    /** @copydoc NavigationLinkValidationSnapshot::NavigationLinkValidationSnapshot */
    NavigationLinkValidationSnapshot::NavigationLinkValidationSnapshot(NavigationLinkValidationSnapshot &&other) noexcept
        : valid_(std::exchange(other.valid_, false)), revisions_(other.revisions_), fingerprint_(other.fingerprint_),
          context_(other.context_), authored_(std::move(other.authored_)), suggestions_(std::move(other.suggestions_)),
          diagnostics_(std::move(other.diagnostics_)) {}

    /** @copydoc NavigationLinkValidationSnapshot::Authored */
    std::span<const NavigationValidatedLink> NavigationLinkValidationSnapshot::Authored() const noexcept {
        return authored_;
    }

    /** @copydoc NavigationLinkValidationSnapshot::Suggestions */
    std::span<const NavigationValidatedLink> NavigationLinkValidationSnapshot::Suggestions() const noexcept {
        return suggestions_;
    }

    /** @copydoc NavigationLinkValidationSnapshot::Diagnostics */
    std::span<const NavigationLinkDiagnostic> NavigationLinkValidationSnapshot::Diagnostics() const noexcept {
        return diagnostics_;
    }

    /** @copydoc NavigationLinkValidationSnapshot::PrepareCookedLinks */
    Result<NavigationCookedLinkSet> NavigationLinkValidationSnapshot::PrepareCookedLinks(
        const NavigationGeneratedLinkCookPolicy policy, const NavigationBakeInputSnapshot &input,
        const NavigationBakeInputRevisions &currentRevisions, const std::span<const NavigationSourceObservation> currentSources,
        const NavigationLinkProjectionContext &currentContext, const NavigationBakePublicationState state,
        const CancellationToken &cancellation) const {
        if (cancellation.IsCancellationRequested())
            return Result<NavigationCookedLinkSet>::Failure(MakeError(NavigationErrors::BakeInputCancelled));
        if (!valid_ || policy < NavigationGeneratedLinkCookPolicy::AuthoredOnly || policy >= NavigationGeneratedLinkCookPolicy::Count)
            return Result<NavigationCookedLinkSet>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
        if (input.Revisions() != revisions_ || input.Fingerprint() != fingerprint_ || currentContext.profile != context_.profile ||
            currentContext.world != context_.world || currentContext.topology != context_.topology)
            return Result<NavigationCookedLinkSet>::Failure(MakeError(NavigationErrors::BakeInputStale));
        const auto fresh = input.ValidatePublication(revisions_.requestGeneration, currentRevisions, currentSources, state);
        if (fresh.HasError())
            return Result<NavigationCookedLinkSet>::Failure(fresh.ErrorValue());
        if (std::ranges::any_of(diagnostics_, [](const auto &row) {
            return !row.startAnchor.IsValid();
        }))
            return Result<NavigationCookedLinkSet>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
        NavigationCookedLinkSet output;
        const bool includeSuggestions = policy == NavigationGeneratedLinkCookPolicy::IncludeValidatedSuggestions;
        output.links.reserve(authored_.size() + (includeSuggestions ? suggestions_.size() : 0));
        Sha256Builder hash;
        HashU64(hash, 0x4e41564c494e4b01ULL);
        static_cast<void>(hash.Update(std::as_bytes(std::span{fingerprint_.bytes})));
        HashU64(hash, context_.profile.Value());
        HashU64(hash, static_cast<std::uint8_t>(policy));
        HashU64(hash, authored_.size() + (includeSuggestions ? suggestions_.size() : 0));
        const auto append = [&](const std::span<const NavigationValidatedLink> rows) {
            for (const auto &row : rows) {
                output.links.push_back({.start = row.start.position,
                                        .end = row.end.position,
                                        .radiusMeters = row.radiusMeters,
                                        .startPolygon = row.start.provenance.polygonIndex,
                                        .endPolygon = row.end.provenance.polygonIndex,
                                        .area = row.area,
                                        .bidirectional = row.direction == NavigationLinkDirection::Bidirectional});
                HashLink(hash, row);
            }
        };
        append(authored_);
        if (includeSuggestions)
            append(suggestions_);
        if (cancellation.IsCancellationRequested())
            return Result<NavigationCookedLinkSet>::Failure(MakeError(NavigationErrors::BakeInputCancelled));
        output.fingerprint = hash.Finalize();
        return Result<NavigationCookedLinkSet>::Success(std::move(output));
    }
}  // namespace Horo::Navigation
