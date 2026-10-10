#include "Horo/Runtime/Save/SaveCommands.h"
#include "Horo/Runtime/Save/SaveErrors.h"
#include "unit/runtime/save/SaveTestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <thread>
#include <vector>

namespace Horo::Runtime {
    namespace {
        using namespace Test;

        CookedSaveProjectPolicy Policy(const SaveConfirmationPolicy confirmation = SaveConfirmationPolicy::OnOverwrite,
                                       const std::uint64_t cooldown = 0, const bool quick = true) {
            SaveProjectPolicy project;
            project.modes[static_cast<std::size_t>(SavePolicyMode::Manual)] = {.enabled = true,
                                                                               .eligibility = SaveModeEligibility::ActiveGameplay,
                                                                               .rotation = {.maximumRetainedSlots = 2},
                                                                               .cooldown = {.minimumIntervalMilliseconds = cooldown},
                                                                               .presentation = {.confirmation = confirmation}};
            if (quick)
                project.modes[static_cast<std::size_t>(SavePolicyMode::Quick)] = {.enabled = true,
                                                                                  .rotation = {.strategy =
                                                                                                   SaveRotationStrategy::ReplaceSingle,
                                                                                               .maximumRetainedSlots = 1},
                                                                                  .presentation = {.confirmation = confirmation}};
            project.modes[static_cast<std::size_t>(SavePolicyMode::Load)] = {.enabled = true};
            return CookedSaveProjectPolicy::Create(project, SaveRuntimeCapabilities::AllSupported()).Value();
        }

        struct Fixture final {
            std::vector<SaveManagerSlotAssessment> assessments{
                {Id<SaveGameSlotId>(1), Id<SlotGenerationId>(10), SaveManagerCompatibility::Direct, SaveManagerIntegrity::Verified}};
            SaveCommandHostState host{.binding = {.active = Address().namespaceAccess.expected,
                                                  .state = SaveNamespaceBindingState::Available,
                                                  .revision = 7},
                                      .catalog = {.revision = 11, .entries = {Entry(1, 10)}},
                                      .assessments = assessments,
                                      .quickSlot = Id<SaveGameSlotId>(2),
                                      .authority = SaveCommandAuthority::Authorized,
                                      .runtime = SaveCommandRuntimeState::ActiveGameplay,
                                      .runtimeRevision = 13,
                                      .monotonicMilliseconds = 100};
            SaveOperationArbiter arbiter = CreateSaveOperationArbiter({8}).Value();
            SaveCommands commands;

            explicit Fixture(CookedSaveProjectPolicy policy = Policy()) : commands(std::move(policy), host, arbiter) {}

            SaveCommandRequest Request(const SaveCommandKind kind = SaveCommandKind::ManualSave, const std::uint8_t slot = 1) const {
                const bool quick = kind == SaveCommandKind::QuickLoad || kind == SaveCommandKind::QuickSave;
                return {.kind = kind,
                        .access = {.expected = *host.binding.active, .expectedRevision = host.binding.revision},
                        .catalogRevision = host.catalog.revision,
                        .runtimeRevision = host.runtimeRevision,
                        .slot = quick ? std::nullopt : std::optional{Id<SaveGameSlotId>(slot)},
                        .expectedGeneration = !quick && slot == 1 ? std::optional{Id<SlotGenerationId>(10)} : std::nullopt};
            }

            Result<SaveCommandSubmission> Submit(const SaveCommandRequest &request, const OperationId operation = 1) {
                const bool load = request.kind == SaveCommandKind::QuickLoad || request.kind == SaveCommandKind::LoadSlot;
                return commands.Submit(request, {.operation = operation,
                                                 .kind = load ? SaveOperationKind::Load : SaveOperationKind::Save,
                                                 .maximumCompletionCallbacks = 4});
            }

            void AddQuick() {
                auto quick = Entry(2, 20);
                quick.publication.kind = SaveSlotKind::Quick;
                host.catalog.entries.push_back(quick);
                assessments.push_back(
                    {Id<SaveGameSlotId>(2), Id<SlotGenerationId>(20), SaveManagerCompatibility::Direct, SaveManagerIntegrity::Verified});
                host.assessments = assessments;
            }
        };

        template <typename T> void ErrorIs(const Result<T> &result, const ErrorCodeDescriptor &error) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().domain.Value() == error.domain.Value());
            CHECK(result.ErrorValue().code.Value() == error.code.Value());
        }

        TEST_CASE("User save commands validate trusted authority, cooked eligibility and exact revisions", "[unit][save][commands]") {
            Fixture fixture;
            const auto request = fixture.Request();
            fixture.host.authority = SaveCommandAuthority::Denied;
            ErrorIs(fixture.Submit(request), SaveErrors::CommandDenied);
            fixture.host.authority = SaveCommandAuthority::Authorized;
            fixture.host.runtime = SaveCommandRuntimeState::PausedOrMenu;
            ErrorIs(fixture.Submit(request), SaveErrors::CommandIneligible);
            fixture.host.runtime = SaveCommandRuntimeState::Loading;
            ErrorIs(fixture.Submit(request), SaveErrors::CommandIneligible);
            fixture.host.runtime = SaveCommandRuntimeState::ActiveGameplay;
            ++fixture.host.runtimeRevision;
            ErrorIs(fixture.Submit(request), SaveErrors::CommandStale);
            --fixture.host.runtimeRevision;
            ++fixture.host.catalog.revision;
            ErrorIs(fixture.Submit(request), SaveErrors::CommandStale);
            --fixture.host.catalog.revision;
            ++fixture.host.binding.revision;
            REQUIRE(fixture.Submit(request).HasError());
            CHECK(fixture.arbiter.QueuedCount() == 0);
        }

        TEST_CASE("Overwrite prompts are revision and generation bound and never enqueue work", "[unit][save][commands]") {
            Fixture fixture;
            auto request = fixture.Request();
            const auto prompt = fixture.Submit(request).Value();
            CHECK(prompt.disposition == SaveCommandDisposition::ConfirmationRequired);
            CHECK_FALSE(prompt.operation.Snapshot());
            CHECK(prompt.target.generation == Id<SlotGenerationId>(10));
            CHECK(fixture.arbiter.QueuedCount() == 0);
            request.confirmation = SaveCommandConfirmation::Confirmed;
            fixture.host.catalog.entries[0].publication.generation = Id<SlotGenerationId>(11);
            ErrorIs(fixture.Submit(request), SaveErrors::CommandStale);
            fixture.host.catalog.entries[0].publication.generation = Id<SlotGenerationId>(10);
            const auto accepted = fixture.Submit(request).Value();
            CHECK(accepted.disposition == SaveCommandDisposition::Admitted);
            CHECK(accepted.operation.Snapshot()->state == SaveOperationState::Queued);
            CHECK(fixture.arbiter.QueuedCount() == 1);
            const auto started = fixture.arbiter.StartNext();
            REQUIRE(started);
            CHECK(started->priority == SaveArbiterPriority::UserBlocking);
            CHECK(fixture.commands.Revalidate(1).Value().generation == Id<SlotGenerationId>(10));
        }

        TEST_CASE("New manual saves honor always-confirm policy and reject malformed command shapes", "[unit][save][commands]") {
            Fixture fixture{Policy(SaveConfirmationPolicy::Always)};
            auto request = fixture.Request(SaveCommandKind::ManualSave, 3);
            const auto prompt = fixture.Submit(request).Value();
            CHECK(prompt.disposition == SaveCommandDisposition::ConfirmationRequired);
            CHECK_FALSE(prompt.target.generation);
            request.confirmation = SaveCommandConfirmation::Confirmed;
            const auto accepted = fixture.Submit(request).Value();
            CHECK(accepted.disposition == SaveCommandDisposition::Admitted);
            CHECK_FALSE(accepted.target.generation);
            request.kind = static_cast<SaveCommandKind>(255);
            ErrorIs(fixture.Submit(request, 2), SaveErrors::CommandInvalid);
            request = fixture.Request(SaveCommandKind::QuickSave);
            request.slot = Id<SaveGameSlotId>(2);
            ErrorIs(fixture.Submit(request, 2), SaveErrors::CommandInvalid);
            request = fixture.Request();
            request.slot.reset();
            ErrorIs(fixture.Submit(request, 2), SaveErrors::CommandInvalid);
            request = fixture.Request();
            request.expectedGeneration.reset();
            ErrorIs(fixture.Submit(request, 2), SaveErrors::CommandStale);
            request = fixture.Request();
            ErrorIs(fixture.commands.Submit(request, {.operation = 2, .kind = SaveOperationKind::Load, .maximumCompletionCallbacks = 4}),
                    SaveErrors::CommandInvalid);
            CHECK(fixture.arbiter.QueuedCount() == 1);
        }

        TEST_CASE("Manual saves respect reserved slot kinds and finite capacity", "[unit][save][commands]") {
            Fixture fixture;
            ErrorIs(fixture.Submit(fixture.Request(SaveCommandKind::ManualSave, 2)), SaveErrors::CommandTargetUnavailable);
            auto automatic = Entry(3, 30);
            automatic.publication.kind = SaveSlotKind::Auto;
            fixture.host.catalog.entries.push_back(automatic);
            auto request = fixture.Request(SaveCommandKind::ManualSave, 3);
            request.expectedGeneration = Id<SlotGenerationId>(30);
            ErrorIs(fixture.Submit(request), SaveErrors::CommandTargetUnavailable);
            fixture.host.catalog.entries.push_back(Entry(4, 40));
            ErrorIs(fixture.Submit(fixture.Request(SaveCommandKind::ManualSave, 5)), SaveErrors::CommandTargetUnavailable);
            request = fixture.Request();
            request.confirmation = SaveCommandConfirmation::Confirmed;
            REQUIRE(fixture.Submit(request).HasValue());
        }

        TEST_CASE("Quick commands use the single cooked product target deterministically", "[unit][save][commands]") {
            Fixture fixture;
            ErrorIs(fixture.Submit(fixture.Request(SaveCommandKind::QuickLoad)), SaveErrors::CommandTargetUnavailable);
            fixture.AddQuick();
            fixture.host.catalog.entries[0].publication.savedAtUnixMilliseconds = 9'999'999'999'999ULL;
            const auto load = fixture.Submit(fixture.Request(SaveCommandKind::QuickLoad)).Value();
            CHECK(load.target.slot == Id<SaveGameSlotId>(2));
            CHECK(load.target.generation == Id<SlotGenerationId>(20));
            CHECK(fixture.commands.Revalidate(1).Value().slot == Id<SaveGameSlotId>(2));
            fixture.host.quickSlot = Id<SaveGameSlotId>(3);
            REQUIRE(fixture.commands.Revalidate(1).HasError());

            Fixture empty;
            const auto save = empty.Submit(empty.Request(SaveCommandKind::QuickSave)).Value();
            CHECK(save.disposition == SaveCommandDisposition::Admitted);
            CHECK(save.target.slot == Id<SaveGameSlotId>(2));
            CHECK_FALSE(save.target.generation);

            Fixture disabled{Policy(SaveConfirmationPolicy::None, 0, false)};
            ErrorIs(disabled.Submit(disabled.Request(SaveCommandKind::QuickSave)), SaveErrors::CommandDenied);
            ErrorIs(disabled.Submit(disabled.Request(SaveCommandKind::QuickLoad)), SaveErrors::CommandDenied);
        }

        TEST_CASE("Quick overwrite confirmation cannot follow a different publication", "[unit][save][commands]") {
            Fixture fixture;
            fixture.AddQuick();
            auto request = fixture.Request(SaveCommandKind::QuickSave);
            const auto prompt = fixture.Submit(request).Value();
            CHECK(prompt.disposition == SaveCommandDisposition::ConfirmationRequired);
            request.confirmation = SaveCommandConfirmation::Confirmed;
            ErrorIs(fixture.Submit(request), SaveErrors::CommandStale);
            request.expectedGeneration = prompt.target.generation;
            CHECK(fixture.Submit(request).Value().disposition == SaveCommandDisposition::Admitted);
        }

        TEST_CASE("Load commands reject unknown incompatible corrupt and stale assessments", "[unit][save][commands]") {
            Fixture fixture;
            const auto request = fixture.Request(SaveCommandKind::LoadSlot);
            for (const auto compatibility :
                 {SaveManagerCompatibility::Unknown, SaveManagerCompatibility::Unsupported, SaveManagerCompatibility::UnsupportedNewer}) {
                fixture.assessments[0].compatibility = compatibility;
                ErrorIs(fixture.Submit(request), SaveErrors::CommandIncompatible);
            }
            fixture.assessments[0].compatibility = SaveManagerCompatibility::Direct;
            fixture.assessments[0].integrity = SaveManagerIntegrity::Failed;
            ErrorIs(fixture.Submit(request), SaveErrors::CommandIncompatible);
            fixture.assessments[0].integrity = SaveManagerIntegrity::Unknown;
            ErrorIs(fixture.Submit(request), SaveErrors::CommandIncompatible);
            fixture.assessments[0].integrity = SaveManagerIntegrity::VerificationRequired;
            fixture.assessments[0].generation = Id<SlotGenerationId>(11);
            ErrorIs(fixture.Submit(request), SaveErrors::CommandStale);
            fixture.assessments[0].generation = Id<SlotGenerationId>(10);
            fixture.assessments[0].compatibility = SaveManagerCompatibility::MigrationAvailable;
            REQUIRE(fixture.Submit(request).HasValue());
            fixture.assessments[0].integrity = SaveManagerIntegrity::Failed;
            ErrorIs(fixture.commands.Revalidate(1), SaveErrors::CommandIncompatible);
        }

        TEST_CASE("Repeated input retains one command and one operation without an unbounded queue", "[unit][save][commands]") {
            Fixture fixture{Policy(SaveConfirmationPolicy::None)};
            const auto request = fixture.Request();
            REQUIRE(fixture.Submit(request).HasValue());
            for (OperationId id = 2; id < 1'002; ++id) {
                const auto repeated = fixture.Submit(request, id).Value();
                CHECK(repeated.disposition == SaveCommandDisposition::Coalesced);
                CHECK(repeated.operation.Snapshot()->operation == 1);
            }
            CHECK(fixture.arbiter.QueuedCount() == 1);
            ErrorIs(fixture.Submit(fixture.Request(SaveCommandKind::LoadSlot), 1'002), SaveErrors::OperationInProgress);
            REQUIRE(fixture.arbiter.StartNext());
            CHECK(fixture.Submit(request, 1'003).Value().disposition == SaveCommandDisposition::Coalesced);
            CHECK(fixture.arbiter.Cancel(1) == SaveCancellationRequestResult::Requested);
            REQUIRE(fixture.arbiter.ObserveCancellation(1).HasValue());
            REQUIRE(fixture.Submit(request, 1'004).HasValue());
        }

        TEST_CASE("Other arbiter producers and product cooldown prevent new user work", "[unit][save][commands]") {
            Fixture fixture{Policy(SaveConfirmationPolicy::None, 50)};
            const auto request = fixture.Request();
            REQUIRE(fixture.Submit(request).HasValue());
            CHECK(fixture.arbiter.Cancel(1) == SaveCancellationRequestResult::Requested);
            ErrorIs(fixture.Submit(request, 2), SaveErrors::CommandCooldown);
            fixture.host.monotonicMilliseconds = 99;
            ErrorIs(fixture.Submit(request, 2), SaveErrors::CommandCooldown);
            fixture.host.monotonicMilliseconds = 150;
            REQUIRE(fixture.Submit(request, 2).HasValue());
            CHECK(fixture.arbiter.Cancel(2) == SaveCancellationRequestResult::Requested);
            REQUIRE(fixture.arbiter
                        .Admit({.operation = {.operation = 3, .kind = SaveOperationKind::Save, .maximumCompletionCallbacks = 1},
                                .mode = SavePolicyMode::Auto,
                                .address = SaveArbiterAddress{*fixture.host.binding.active, Id<SaveGameSlotId>(9)}})
                        .HasValue());
            fixture.host.monotonicMilliseconds = 200;
            ErrorIs(fixture.Submit(request, 4), SaveErrors::OperationInProgress);
            CHECK(fixture.arbiter.QueuedCount() == 1);
        }

        TEST_CASE("Command dispatch fences owner thread shutdown and changed runtime authority", "[unit][save][commands]") {
            Fixture fixture{Policy(SaveConfirmationPolicy::None)};
            REQUIRE(fixture.Submit(fixture.Request()).HasValue());
            ++fixture.host.runtimeRevision;
            ErrorIs(fixture.commands.Revalidate(1), SaveErrors::CommandStale);
            --fixture.host.runtimeRevision;
            fixture.host.authority = SaveCommandAuthority::Denied;
            ErrorIs(fixture.commands.Revalidate(1), SaveErrors::CommandDenied);
            fixture.host.authority = SaveCommandAuthority::Authorized;
            bool wrongThread = false;
            const auto request = fixture.Request();
            std::thread worker([&] {
                const auto result = fixture.Submit(request, 2);
                wrongThread = result.HasError() && result.ErrorValue().code.Value() == SaveErrors::ThreadAffinityViolation.code.Value();
            });
            worker.join();
            CHECK(wrongThread);
            REQUIRE(fixture.commands.BeginShutdown().HasValue());
            REQUIRE(fixture.commands.BeginShutdown().HasValue());
            ErrorIs(fixture.Submit(request, 2), SaveErrors::LifecycleUnavailable);
            ErrorIs(fixture.commands.Revalidate(1), SaveErrors::LifecycleUnavailable);
            CHECK_FALSE(fixture.arbiter.Snapshot(1)->operation.IsTerminal());
        }
    }  // namespace
}  // namespace Horo::Runtime

namespace Horo::Runtime {
    TEST_CASE("Real manual command admission proceeds ahead of deferred background storage retry", "[unit][save][commands][retry]") {
        Fixture fixture(Policy(SaveConfirmationPolicy::None));
        const SaveArbiterRetryDescriptor retry{.policy = {2, 10, 20, 100},
                                               .preconditions = {.access = {*fixture.host.binding.active, fixture.host.binding.revision},
                                                                 .runtime = {13, 1, 1},
                                                                 .catalogRevision = fixture.host.catalog.revision,
                                                                 .slot = Id<SaveGameSlotId>(3),
                                                                 .publicationGeneration = Id<SlotGenerationId>(99)}};
        const auto autosave = fixture.arbiter.Admit({.operation = {.operation = 91, .maximumCompletionCallbacks = 1},
                                                     .mode = SavePolicyMode::Auto,
                                                     .address = SaveArbiterAddress{*fixture.host.binding.active, Id<SaveGameSlotId>(3)},
                                                     .priority = SaveArbiterPriority::Background,
                                                     .retry = retry});
        REQUIRE(autosave.HasValue());
        REQUIRE(fixture.arbiter.StartNext());
        REQUIRE(fixture.arbiter.Advance(91, SaveArbiterState::WaitingForSafePoint).HasValue());
        ErrorIs(fixture.Submit(fixture.Request()), SaveErrors::OperationInProgress);
        REQUIRE(fixture.arbiter.Advance(91, SaveArbiterState::Capturing).HasValue());
        REQUIRE(fixture.arbiter.Advance(91, SaveArbiterState::Encoding).HasValue());
        REQUIRE(fixture.arbiter
                    .DeferStorageRetry(91,
                                       {.category = SaveStorageFailureCategory::TransientIo,
                                        .nativeCause = MakeError(SaveErrors::StorageTransientIo)},
                                       0)
                    .Value());
        const auto manual = fixture.Submit(fixture.Request());
        REQUIRE(manual.HasValue());
        CHECK(manual.Value().disposition == SaveCommandDisposition::Admitted);
        CHECK_FALSE(fixture.arbiter.ResumeStorageRetry(91, retry.preconditions, 10).Value());
        CHECK(fixture.arbiter.StartNext()->operation.operation == manual.Value().operation.Id());
        CHECK_FALSE(autosave.Value().handle.Snapshot()->IsTerminal());
    }
}  // namespace Horo::Runtime
