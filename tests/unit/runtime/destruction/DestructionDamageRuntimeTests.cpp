#include "Horo/Destruction/DestructionDamageRuntime.h"
#include "Horo/Destruction/DestructionErrors.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>

namespace Horo::Destruction {
    namespace {
        template <typename Identity> Identity Id(const std::uint64_t value) {
            return Identity::Create(value).Value();
        }

        DestructionHandle Handle(const std::uint64_t generation = 1) {
            return {Id<DestructionWorldId>(7), Id<DestructibleId>(11), Id<DestructionGeneration>(generation)};
        }

        FractureArtifactContentIdentity Content(const std::uint8_t value = 1) {
            std::array<std::uint8_t, 16> bytes{};
            bytes.front() = value;
            const auto asset = FractureAssetId::Create(Assets::AssetId::FromBytes(bytes)).Value();
            Sha256Digest digest{};
            digest.bytes.front() = value;
            return FractureArtifactContentIdentity::Create(asset, Id<FractureContentRevision>(value), digest).Value();
        }

        DestructibleDescriptor Descriptor(const std::uint32_t cooldown = 2,
                                          const DestructionTriggerPolicy trigger = DestructionTriggerPolicy::DamageAndContact,
                                          const std::uint8_t content = 1) {
            const auto profile = GetDestructionTierProfile(DestructionFeatureTier::Baseline).Value();
            DestructibleDescriptorData data{.destructible = Id<DestructibleId>(11),
                                            .content = Content(content),
                                            .configurationRevision = Id<DestructionConfigurationRevision>(3),
                                            .features = {.required = {.bits =
                                                                          DestructionFeatureBit<DestructionFeature::PreCookedFracture>}},
                                            .limits = profile.limits,
                                            .behavior = {.trigger = trigger,
                                                         .support = DestructionSupportPolicy::Disabled,
                                                         .minimumDamageIntervalTicks = cooldown}};
            auto descriptor = DestructibleDescriptor::Create(data);
            REQUIRE(descriptor.HasValue());
            return descriptor.Value();
        }

        DestructionAuthorityGrant Authority(const std::uint64_t revision = 1) {
            return {.authority = Id<DestructionAuthorityId>(9),
                    .revision = Id<DestructionAuthorityRevision>(revision),
                    .capabilities = {.bits = DestructionCommandCapabilityBit<DestructionCommandCapability::Damage> |
                                             DestructionCommandCapabilityBit<DestructionCommandCapability::Collision> |
                                             DestructionCommandCapabilityBit<DestructionCommandCapability::Script> |
                                             DestructionCommandCapabilityBit<DestructionCommandCapability::ExplicitFracture>}};
        }

        DestructionDamageContext Context(const DestructionDamageRuntime &runtime, const std::uint64_t tick,
                                         const DestructionDamageSafePoint safePoint = DestructionDamageSafePoint::PrePhysics) {
            return {.target = runtime.Snapshot().target,
                    .content = runtime.Snapshot().content,
                    .configurationRevision = runtime.Snapshot().configurationRevision,
                    .capabilityRevision = Id<DestructionCapabilityRevision>(5),
                    .authority = Authority(),
                    .limits = {},
                    .simulationTick = tick,
                    .safePoint = safePoint};
        }

        DestructionCommandHeader Header(const DestructionDamageRuntime &runtime, const std::uint64_t value,
                                        const std::uint64_t eligibleTick) {
            return {.id = {runtime.Snapshot().target, Id<DestructionCommandValue>(value)},
                    .expectedRevision = runtime.Snapshot().revision,
                    .capabilityRevision = Id<DestructionCapabilityRevision>(5),
                    .eligibleSimulationTick = eligibleTick,
                    .authority = Authority()};
        }

        DestructionCommand Damage(const DestructionDamageRuntime &runtime, const std::uint64_t value, const std::uint64_t tick,
                                  const float amount) {
            return DestructionCommand::Damage(Header(runtime, value, tick), amount).Value();
        }

        DestructionDamageRuntime Runtime(const DestructibleDescriptor &descriptor = Descriptor()) {
            auto runtime = DestructionDamageRuntime::Create(descriptor, Handle(), Id<DestructionStateRevision>(1));
            REQUIRE(runtime.HasValue());
            return runtime.Value();
        }

        DestructionDamageRuntime CommitCommand(const DestructionDamageRuntime &runtime, const DestructionCommand &command,
                                               const std::uint64_t tick) {
            const auto context = Context(runtime, tick,
                                         command.Kind() == DestructionCommandKind::Collision ? DestructionDamageSafePoint::PostPhysics
                                                                                             : DestructionDamageSafePoint::PrePhysics);
            auto candidate = runtime.Prepare(command, context);
            REQUIRE(candidate.HasValue());
            auto committed = runtime.Commit(candidate.Value(), context);
            REQUIRE(committed.HasValue());
            return committed.Value();
        }

        template <typename T> void CheckError(const Result<T> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == expected.code.Value());
        }
    }  // namespace

    TEST_CASE("Ordered damage crosses exact thresholds and produces typed revision results", "[unit][destruction][damage]") {
        auto runtime = Runtime();
        runtime = CommitCommand(runtime, Damage(runtime, 1, 10, 24.0F), 10);
        CHECK(runtime.Snapshot().health == 76.0F);
        CHECK(runtime.Snapshot().phase == DestructionStatePhase::Intact);
        REQUIRE(runtime.LastResult().has_value());
        CHECK(runtime.LastResult()->Command().value == Id<DestructionCommandValue>(1));
        CHECK(runtime.LastResult()->SourceRevision() == Id<DestructionStateRevision>(1));
        CHECK(runtime.LastResult()->TerminalRevision() == Id<DestructionStateRevision>(2));
        CHECK(runtime.LastResult()->Outcome() == DestructionCommandOutcome::Succeeded);

        const auto second = Damage(runtime, 2, 12, 1.0F);
        CheckError(runtime.Prepare(second, Context(runtime, 11)), DestructionErrors::CommandInvalid);
        runtime = CommitCommand(runtime, second, 12);
        CHECK(runtime.Snapshot().health == 75.0F);
        CHECK(runtime.Snapshot().phase == DestructionStatePhase::Damaged);
        runtime = CommitCommand(runtime, Damage(runtime, 3, 14, 75.0F), 14);
        CHECK(runtime.Snapshot().health == 0.0F);
        CHECK(runtime.Snapshot().phase == DestructionStatePhase::Destroyed);
        CHECK(runtime.Snapshot().revision == Id<DestructionStateRevision>(4));
    }

    TEST_CASE("Cooldown is checked at boundaries without changing active state", "[unit][destruction][damage][boundary]") {
        auto runtime = Runtime();
        runtime = CommitCommand(runtime, Damage(runtime, 1, 10, 10.0F), 10);
        const auto second = Damage(runtime, 2, 11, 5.0F);
        CheckError(runtime.Prepare(second, Context(runtime, 11)), DestructionErrors::DamageCooldownActive);
        CHECK(runtime.Snapshot().health == 90.0F);
        CHECK(runtime.Snapshot().revision == Id<DestructionStateRevision>(2));
        runtime = CommitCommand(runtime, second, 12);
        CHECK(runtime.Snapshot().health == 85.0F);
        const auto olderFracture = DestructionCommand::Script(Header(runtime, 3, 11), DestructionScriptIntent::Fracture).Value();
        CheckError(runtime.Prepare(olderFracture, Context(runtime, 11)), DestructionErrors::CommandInvalid);
        CHECK(runtime.Snapshot().health == 85.0F);

        auto noCooldown = Runtime(Descriptor(0));
        noCooldown = CommitCommand(noCooldown, Damage(noCooldown, 1, 10, 1.0F), 10);
        noCooldown = CommitCommand(noCooldown, Damage(noCooldown, 2, 10, 1.0F), 10);
        CHECK(noCooldown.Snapshot().health == 98.0F);

        auto late = Runtime();
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        late = CommitCommand(late, Damage(late, 1, maximum - 1, 1.0F), maximum - 1);
        CheckError(late.Prepare(Damage(late, 2, maximum, 1.0F), Context(late, maximum)), DestructionErrors::DamageCooldownActive);
    }

    TEST_CASE("Whole-command identity rejects conflicting retries and keeps exact retry idempotent", "[unit][destruction][damage]") {
        const auto initial = Runtime();
        const auto command = Damage(initial, 1, 10, 10.0F);
        const auto candidate = initial.Prepare(command, Context(initial, 10)).Value();
        const auto committed = initial.Commit(candidate, Context(initial, 10)).Value();
        const auto retry = committed.Prepare(command, Context(committed, 10));
        REQUIRE(retry.HasValue());
        CHECK(retry.Value().StateTransition().Status() == DestructionTransitionStatus::Duplicate);
        CHECK(committed.Commit(candidate, Context(committed, 10)).Value().Snapshot() == committed.Snapshot());
        CHECK(committed.Commit(retry.Value(), Context(committed, 10)).Value().Snapshot() == committed.Snapshot());

        auto changedHeader = command.Header();
        changedHeader.eligibleSimulationTick = 11;
        const auto changed = DestructionCommand::Damage(changedHeader, 10.0F).Value();
        CheckError(committed.Prepare(changed, Context(committed, 11)), DestructionErrors::DuplicateCommand);
        CheckError(committed.Prepare(Damage(committed, 1, 12, 10.0F), Context(committed, 12)), DestructionErrors::DuplicateCommand);
    }

    TEST_CASE("Authority, generation, policy, and capability evidence are rechecked at commit", "[unit][destruction][damage]") {
        const auto runtime = Runtime();
        const auto command = Damage(runtime, 1, 10, 10.0F);
        const auto candidate = runtime.Prepare(command, Context(runtime, 10)).Value();
        auto context = Context(runtime, 10);
        context.authority = Authority(2);
        CheckError(runtime.Prepare(command, context), DestructionErrors::CommandAuthorityDenied);
        CheckError(runtime.Commit(candidate, context), DestructionErrors::CommandAuthorityDenied);
        context = Context(runtime, 10);
        context.capabilityRevision = Id<DestructionCapabilityRevision>(6);
        CheckError(runtime.Commit(candidate, context), DestructionErrors::CommandUnsupported);
        context = Context(runtime, 10);
        context.limits.maximumDamage = 5.0F;
        CheckError(runtime.Commit(candidate, context), DestructionErrors::CommandLimitExceeded);
        context = Context(runtime, 10);
        context.configurationRevision = Id<DestructionConfigurationRevision>(4);
        CheckError(runtime.Commit(candidate, context), DestructionErrors::StaleConfiguration);
        context = Context(runtime, 10);
        context.content = Content(2);
        CheckError(runtime.Commit(candidate, context), DestructionErrors::StaleContent);
        context = Context(runtime, 10);
        context.target = Handle(2);
        CheckError(runtime.Commit(candidate, context), DestructionErrors::StaleGeneration);
        context = Context(runtime, 10);
        context.simulationTick = 9;
        CheckError(runtime.Commit(candidate, context), DestructionErrors::CommandInvalid);
        context = Context(runtime, 10);
        context.safePoint = static_cast<DestructionDamageSafePoint>(255);
        CheckError(runtime.Prepare(command, context), DestructionErrors::CommandInvalid);
        CheckError(runtime.Commit(candidate, context), DestructionErrors::CommandInvalid);
        auto staleHeader = command.Header();
        staleHeader.expectedRevision = Id<DestructionStateRevision>(2);
        const auto stale = DestructionCommand::Damage(staleHeader, 10.0F).Value();
        CheckError(runtime.Prepare(stale, Context(runtime, 10)), DestructionErrors::StaleRevision);
        CHECK(runtime.Snapshot().health == 100.0F);
    }

    TEST_CASE("Post-step collision and explicit fracture use the same canonical state machine", "[unit][destruction][damage]") {
        auto runtime = Runtime();
        auto collision = DestructionCommand::Collision(Header(runtime, 1, 20), {1, 2, 3}, {0, 1, 0}, 2.0F, 10.0F).Value();
        CheckError(runtime.Prepare(collision, Context(runtime, 20)), DestructionErrors::CommandInvalid);
        const auto postStep = Context(runtime, 20, DestructionDamageSafePoint::PostPhysics);
        const auto candidate = runtime.Prepare(collision, postStep);
        REQUIRE(candidate.HasValue());
        runtime = runtime.Commit(candidate.Value(), Context(runtime, 21)).Value();
        CHECK(runtime.Snapshot().health == 90.0F);
        const auto fracture = DestructionCommand::Script(Header(runtime, 2, 21), DestructionScriptIntent::Fracture).Value();
        runtime = CommitCommand(runtime, fracture, 21);
        CHECK(runtime.Snapshot().phase == DestructionStatePhase::Destroyed);

        const auto noContact = Runtime(Descriptor(0, DestructionTriggerPolicy::AccumulatedDamage));
        collision = DestructionCommand::Collision(Header(noContact, 1, 20), {1, 2, 3}, {0, 1, 0}, 2.0F, 10.0F).Value();
        CheckError(noContact.Prepare(collision, Context(noContact, 20, DestructionDamageSafePoint::PostPhysics)),
                   DestructionErrors::CommandUnsupported);
        const auto explicitOnly = Runtime(Descriptor(0, DestructionTriggerPolicy::ExplicitOnly));
        CheckError(explicitOnly.Prepare(Damage(explicitOnly, 1, 1, 100.0F), Context(explicitOnly, 1)),
                   DestructionErrors::CommandUnsupported);
    }

    TEST_CASE("Cancellation, competing candidates, replacement, and shutdown preserve published state",
              "[unit][destruction][damage][lifecycle]") {
        const auto runtime = Runtime();
        const auto first = runtime.Prepare(Damage(runtime, 1, 10, 10.0F), Context(runtime, 10)).Value();
        const auto second = runtime.Prepare(Damage(runtime, 2, 10, 20.0F), Context(runtime, 10)).Value();
        CheckError(runtime.Commit(first.Cancel(), Context(runtime, 10)), DestructionErrors::CancelledBeforeCommit);
        const auto committed = runtime.Commit(first, Context(runtime, 10)).Value();
        CheckError(committed.Commit(second, Context(committed, 10)), DestructionErrors::StaleRevision);
        CHECK(committed.Snapshot().health == 90.0F);

        const auto replacement = committed.Replace(Handle(2), Descriptor()).Value();
        CHECK(replacement.Snapshot().revision == Id<DestructionStateRevision>(1));
        CHECK(replacement.Snapshot().health == 100.0F);
        CHECK_FALSE(replacement.LastResult().has_value());
        CheckError(replacement.Commit(first, Context(replacement, 10)), DestructionErrors::StaleGeneration);
        const auto fresh = replacement.Prepare(Damage(replacement, 1, 10, 1.0F), Context(replacement, 10));
        REQUIRE(fresh.HasValue());

        const auto closed = committed.BeginShutdown().BeginShutdown();
        CHECK_FALSE(closed.IsAdmissionOpen());
        CHECK(closed.Snapshot() == committed.Snapshot());
        CheckError(closed.Prepare(Damage(closed, 3, 12, 1.0F), Context(closed, 12)), DestructionErrors::ShutdownInProgress);
        CheckError(closed.Commit(second, Context(closed, 12)), DestructionErrors::ShutdownInProgress);
        CheckError(closed.Replace(Handle(2), Descriptor()), DestructionErrors::ShutdownInProgress);
    }
}  // namespace Horo::Destruction
