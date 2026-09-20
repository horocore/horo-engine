#include "Horo/WorldStreaming/WorldEntityReferenceFixup.h"
#include "Horo/WorldStreaming/WorldStreamingErrors.h"
#include "WorldStreamingTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace Horo::WorldStreaming {
    namespace {
        using TestSupport::Asset;
        using TestSupport::IdentityFrom;
        using TestSupport::RequireError;
        using TestSupport::WorldOwner;

        WorldDependencyEndpoint Endpoint(const std::uint64_t object, const std::uint64_t revision) {
            return {{Asset(7), object}, IdentityFrom<WorldAuthoringRevision>(revision)};
        }

        WorldEntityReferenceFixupRequest Request(const std::uint64_t revision = 1,
                                                 const WorldDependencyKind kind = WorldDependencyKind::Soft,
                                                 const std::uint64_t targetRevision = 20) {
            return {.reference = {Endpoint(1, 10), Endpoint(2, targetRevision), kind},
                    .revision = IdentityFrom<WorldEntityReferenceFixupRevision>(revision)};
        }

        WorldEntityReferenceBinding Binding(const std::uint64_t object, const std::uint64_t revision, const std::uint64_t runtime) {
            return {.endpoint = Endpoint(object, revision), .runtime = IdentityFrom<WorldRuntimeEntityId>(runtime)};
        }

        WorldEntityReferenceFixupLedger Ledger(const std::uint32_t capacity = 4) {
            auto result = WorldEntityReferenceFixupLedger::Create(WorldOwner(),
                                                                  {.maximumPendingReferences = capacity, .maximumActivationMappings = 8});
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        static_assert(!std::is_copy_constructible_v<WorldEntityReferenceFixupLedger>);
        static_assert(std::is_move_constructible_v<WorldEntityReferenceFixupLedger>);

        TEST_CASE("Entity fixups resolve hard references and defer soft references until target activation",
                  "[unit][world_streaming][entity_fixup]") {
            auto ledger = Ledger();
            const auto source = Binding(1, 10, 101);
            const auto target = Binding(2, 20, 202);
            const std::array active{source, target};

            auto hard = Request(1, WorldDependencyKind::Hard);
            const auto resolved = ledger.Submit(WorldOwner(), hard, active);
            REQUIRE(resolved.HasValue());
            REQUIRE(resolved.Value().disposition == WorldEntityReferenceFixupDisposition::Resolved);
            REQUIRE(resolved.Value().mutation == WorldEntityReferenceFixupMutation::Resolved);
            REQUIRE(resolved.Value().runtime == target.runtime);
            REQUIRE(ledger.Pending().empty());

            auto soft = Request(2);
            REQUIRE(ledger.Submit(WorldOwner(), soft, std::array{source}).Value().disposition ==
                    WorldEntityReferenceFixupDisposition::Deferred);
            REQUIRE(ledger.Pending().size() == 1);

            const auto activated = ledger.Activate(WorldOwner(), soft, target);
            REQUIRE(activated.HasValue());
            REQUIRE(activated.Value().disposition == WorldEntityReferenceFixupDisposition::Resolved);
            REQUIRE(activated.Value().mutation == WorldEntityReferenceFixupMutation::Activated);
            REQUIRE(activated.Value().runtime == target.runtime);
            REQUIRE(ledger.Pending().empty());
        }

        TEST_CASE("Entity fixups reject stale and unsupported activation evidence without partial state",
                  "[unit][world_streaming][entity_fixup]") {
            auto ledger = Ledger();
            const auto source = Binding(1, 10, 101);
            const auto target = Binding(2, 20, 202);
            auto hard = Request(1, WorldDependencyKind::Hard);
            const auto before = ledger.Pending().size();

            RequireError(ledger.Submit(WorldOwner(), hard, std::array{Binding(1, 99, 101), Binding(2, 20, 202)}),
                         WorldStreamingErrors::EntityFixupStale);
            REQUIRE(ledger.Pending().size() == before);

            auto unsupported = hard;
            unsupported.reference.kind = static_cast<WorldDependencyKind>(99);
            RequireError(ledger.Submit(WorldOwner(), unsupported, std::array{source}), WorldStreamingErrors::EntityFixupUnsupported);
            REQUIRE(ledger.Pending().size() == before);

            auto deferred = Request(2);
            REQUIRE(ledger.Submit(WorldOwner(), deferred, std::array{source}).HasValue());
            const auto staleTarget = Binding(2, 21, 202);
            RequireError(ledger.Activate(WorldOwner(), deferred, staleTarget), WorldStreamingErrors::EntityFixupStale);
            REQUIRE(ledger.Pending().size() == 1);

            auto staleSource = deferred;
            staleSource.reference.source.revision = IdentityFrom<WorldAuthoringRevision>(11);
            RequireError(ledger.Activate(WorldOwner(), staleSource, target), WorldStreamingErrors::EntityFixupStale);
            REQUIRE(ledger.Pending().size() == 1);

            auto hardActivation = deferred;
            hardActivation.reference.kind = WorldDependencyKind::Hard;
            RequireError(ledger.Activate(WorldOwner(), hardActivation, target), WorldStreamingErrors::EntityFixupUnsupported);
            REQUIRE(ledger.Pending().size() == 1);

            auto invalid = Request(3);
            invalid.reference.source = {};
            RequireError(ledger.Submit(WorldOwner(), invalid, std::array{source}), WorldStreamingErrors::EntityFixupInvalid);
            REQUIRE(ledger.Pending().size() == 1);

            std::array oversizedMappings{source,
                                         Binding(2, 20, 202),
                                         Binding(3, 30, 303),
                                         Binding(4, 40, 404),
                                         Binding(5, 50, 505),
                                         Binding(6, 60, 606),
                                         Binding(7, 70, 707),
                                         Binding(8, 80, 808),
                                         Binding(9, 90, 909)};
            RequireError(ledger.Submit(WorldOwner(), Request(4), oversizedMappings), WorldStreamingErrors::EntityFixupCapacityExceeded);
            REQUIRE(ledger.Pending().size() == 1);
        }

        TEST_CASE("Entity fixups enforce capacity, replacement, cancellation, failure, and shutdown fences",
                  "[unit][world_streaming][entity_fixup][lifecycle]") {
            auto ledger = Ledger(1);
            const auto source = Binding(1, 10, 101);
            auto first = Request(1);
            REQUIRE(ledger.Submit(WorldOwner(), first, std::array{source}).HasValue());

            auto second = Request(1);
            second.reference.target.address.object = 3;
            RequireError(ledger.Submit(WorldOwner(), second, std::array{source}), WorldStreamingErrors::EntityFixupCapacityExceeded);
            REQUIRE(ledger.Pending().size() == 1);

            auto replacement = first;
            replacement.revision = IdentityFrom<WorldEntityReferenceFixupRevision>(2);
            replacement.expectedRevision = first.revision;
            const auto replaced = ledger.Submit(WorldOwner(), replacement, std::array{source});
            REQUIRE(replaced.HasValue());
            REQUIRE(replaced.Value().mutation == WorldEntityReferenceFixupMutation::Replaced);
            REQUIRE(ledger.Pending().front().revision == replacement.revision);

            auto staleReplacement = replacement;
            staleReplacement.revision = IdentityFrom<WorldEntityReferenceFixupRevision>(3);
            staleReplacement.expectedRevision = first.revision;
            RequireError(ledger.Submit(WorldOwner(), staleReplacement, std::array{source}), WorldStreamingErrors::EntityFixupStale);
            REQUIRE(ledger.Pending().front().revision == replacement.revision);

            RequireError(ledger.Cancel(WorldOwner(), first), WorldStreamingErrors::EntityFixupStale);
            auto hardCancel = replacement;
            hardCancel.reference.kind = WorldDependencyKind::Hard;
            RequireError(ledger.Cancel(WorldOwner(), hardCancel), WorldStreamingErrors::EntityFixupUnsupported);
            REQUIRE(ledger.Pending().size() == 1);
            const auto failed = ledger.Fail(WorldOwner(), replacement);
            REQUIRE(failed.HasValue());
            REQUIRE(failed.Value().disposition == WorldEntityReferenceFixupDisposition::Failed);
            REQUIRE(ledger.Pending().empty());

            REQUIRE(ledger.RequestCancellation(WorldOwner()).HasValue());
            REQUIRE(ledger.BeginShutdown(WorldOwner()).HasValue());
            REQUIRE(ledger.Lifecycle() == WorldEntityReferenceFixupLedgerLifecycle::Closed);
            RequireError(ledger.Submit(WorldOwner(), Request(4), std::array{source}),
                         WorldStreamingErrors::EntityFixupLifecycleUnavailable);
            REQUIRE(ledger.BeginShutdown(WorldOwner()).HasValue());
        }

        TEST_CASE("Entity fixups keep deferred state unchanged across foreign owners and hard misses",
                  "[unit][world_streaming][entity_fixup]") {
            auto ledger = Ledger();
            const auto source = Binding(1, 10, 101);
            const auto hard = Request(1, WorldDependencyKind::Hard);
            RequireError(ledger.Submit(WorldOwner(9), hard, std::array{source}), WorldStreamingErrors::EntityFixupStale);
            RequireError(ledger.Submit(WorldOwner(), hard, std::array{source}), WorldStreamingErrors::EntityFixupTargetUnavailable);
            REQUIRE(ledger.Pending().empty());

            const auto soft = Request(2);
            REQUIRE(ledger.Submit(WorldOwner(), soft, std::array{source}).HasValue());
            REQUIRE(ledger.RequestCancellation(WorldOwner()).HasValue());
            const auto cancelled = ledger.Cancel(WorldOwner(), soft);
            REQUIRE(cancelled.HasValue());
            REQUIRE(cancelled.Value().disposition == WorldEntityReferenceFixupDisposition::Cancelled);
            REQUIRE(ledger.Lifecycle() == WorldEntityReferenceFixupLedgerLifecycle::Closed);
        }

        TEST_CASE("Entity fixups reject invalid construction and activation mappings transactionally",
                  "[unit][world_streaming][entity_fixup][invalid]") {
            RequireError(WorldEntityReferenceFixupLedger::Create({}, {.maximumPendingReferences = 1, .maximumActivationMappings = 1}),
                         WorldStreamingErrors::EntityFixupInvalid);
            RequireError(WorldEntityReferenceFixupLedger::Create(WorldOwner(),
                                                                 {.maximumPendingReferences = 0, .maximumActivationMappings = 1}),
                         WorldStreamingErrors::EntityFixupInvalid);
            RequireError(WorldEntityReferenceFixupLedger::Create(WorldOwner(),
                                                                 {.maximumPendingReferences = 1, .maximumActivationMappings = 0}),
                         WorldStreamingErrors::EntityFixupInvalid);
            RequireError(WorldEntityReferenceFixupLedger::Create(WorldOwner(),
                                                                 {.maximumPendingReferences =
                                                                      WorldEntityReferenceFixupLimits::MaximumPendingReferences + 1,
                                                                  .maximumActivationMappings = 1}),
                         WorldStreamingErrors::EntityFixupInvalid);

            auto ledger = Ledger();
            const auto source = Binding(1, 10, 101);
            const auto target = Binding(2, 20, 202);
            const auto request = Request();

            RequireError(ledger.Submit(WorldOwner(), request, std::array{source, Binding(1, 10, 303), target}),
                         WorldStreamingErrors::EntityFixupIdentityConflict);
            REQUIRE(ledger.Pending().empty());
            RequireError(ledger.Submit(WorldOwner(), request, std::array{source, Binding(3, 30, 101)}),
                         WorldStreamingErrors::EntityFixupIdentityConflict);
            REQUIRE(ledger.Pending().empty());
            RequireError(ledger.Submit(WorldOwner(), request, std::array{source, WorldEntityReferenceBinding{target.endpoint, {}}}),
                         WorldStreamingErrors::EntityFixupInvalid);
            REQUIRE(ledger.Pending().empty());
            RequireError(ledger.Submit(WorldOwner(), request, std::array{target}), WorldStreamingErrors::EntityFixupSourceUnavailable);
            REQUIRE(ledger.Pending().empty());
        }

        TEST_CASE("Entity fixups preserve lifecycle fences across cancellation, move, shutdown, and revision exhaustion",
                  "[unit][world_streaming][entity_fixup][lifecycle]") {
            auto ledger = Ledger();
            const auto source = Binding(1, 10, 101);
            const auto deferred = Request();
            REQUIRE(ledger.Submit(WorldOwner(), deferred, std::array{source}).HasValue());

            REQUIRE(ledger.RequestCancellation(WorldOwner()).HasValue());
            RequireError(ledger.Submit(WorldOwner(), Request(2), std::array{source}),
                         WorldStreamingErrors::EntityFixupLifecycleUnavailable);
            const auto cancelled = ledger.Cancel(WorldOwner(), deferred);
            REQUIRE(cancelled.HasValue());
            REQUIRE(ledger.Lifecycle() == WorldEntityReferenceFixupLedgerLifecycle::Closed);
            RequireError(ledger.Fail(WorldOwner(), deferred), WorldStreamingErrors::EntityFixupLifecycleUnavailable);
            RequireError(ledger.Cancel(WorldOwner(), deferred), WorldStreamingErrors::EntityFixupLifecycleUnavailable);

            auto moved = Ledger();
            auto movedFrom = std::move(moved);
            RequireError(moved.Submit(WorldOwner(), Request(), std::array{source}), WorldStreamingErrors::EntityFixupLifecycleUnavailable);
            REQUIRE(movedFrom.Owner() == WorldOwner());
            REQUIRE(movedFrom.Lifecycle() == WorldEntityReferenceFixupLedgerLifecycle::Active);

            REQUIRE(movedFrom.BeginShutdown(WorldOwner()).HasValue());
            REQUIRE(movedFrom.Lifecycle() == WorldEntityReferenceFixupLedgerLifecycle::Closed);
            RequireError(movedFrom.BeginShutdown(WorldOwner(9)), WorldStreamingErrors::EntityFixupStale);
            RequireError(movedFrom.Submit(WorldOwner(), Request(), std::array{source}),
                         WorldStreamingErrors::EntityFixupLifecycleUnavailable);

            RequireError(NextWorldEntityReferenceFixupRevision({}), WorldStreamingErrors::IdentityInvalid);
            const auto maximum = IdentityFrom<WorldEntityReferenceFixupRevision>(std::numeric_limits<std::uint64_t>::max());
            RequireError(NextWorldEntityReferenceFixupRevision(maximum), WorldStreamingErrors::GenerationExhausted);
        }
    }  // namespace
}  // namespace Horo::WorldStreaming
