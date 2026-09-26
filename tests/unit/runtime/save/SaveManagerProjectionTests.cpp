#include "Horo/Runtime/Save/SaveErrors.h"
#include "Horo/Runtime/Save/SaveManagerProjection.h"
#include "SaveTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

namespace Horo::Runtime {
    namespace {
        using namespace Test;

        struct ProjectionFixture final {
            SaveNamespaceBindingSnapshot binding{.active = Address().namespaceAccess.expected,
                                                 .state = SaveNamespaceBindingState::Available,
                                                 .revision = 7};
            SaveSlotIndex catalog{.revision = 11, .entries = {Entry(1, 10), Entry(2, 20), Entry(3, 30)}};
            std::vector<SaveManagerProfileSource> profiles{{*binding.active, true}};
            std::vector<SaveManagerSlotAssessment> assessments{{Id<SaveGameSlotId>(1), Id<SlotGenerationId>(10),
                                                                SaveManagerCompatibility::Direct, SaveManagerIntegrity::Verified},
                                                               {Id<SaveGameSlotId>(3), Id<SlotGenerationId>(30),
                                                                SaveManagerCompatibility::Unsupported,
                                                                SaveManagerIntegrity::VerificationRequired}};
            std::vector<SaveManagerOperationSource> operations;
            std::vector<SaveManagerDiagnostic> diagnostics;
            std::uint64_t publicationRevision{13};

            [[nodiscard]] SaveManagerProjectionInput Input() const {
                return {binding, catalog, publicationRevision, profiles, assessments, operations, diagnostics};
            }
        };

        TEST_CASE("Manager projection owns detached values without retaining source mutations or raw errors",
                  "[unit][save][manager-projection]") {
            ProjectionFixture source;
            source.catalog.entries[0].display.displayName = "Alpha";
            source.catalog.entries[1].display.displayName = "München";
            source.operations.push_back({.snapshot = {.operation = 42,
                                                      .kind = SaveOperationKind::Load,
                                                      .state = SaveOperationState::Failed,
                                                      .stage = SaveOperationStage::VerifyingArchive,
                                                      .terminalError = MakeError(SaveErrors::StorageOperationInvalid)},
                                         .slot = Id<SaveGameSlotId>(1),
                                         .generation = Id<SlotGenerationId>(10),
                                         .failureCategory = SaveManagerDiagnosticKind::Storage});
            source.diagnostics.push_back({.kind = SaveManagerDiagnosticKind::Corrupt, .slot = Id<SaveGameSlotId>(3)});
            auto projection = SaveManagerProjection::Create(source.Input()).Value();
            auto retained = projection;
            source.catalog.entries[0].display.displayName = "Changed";
            source.catalog.entries.clear();
            source.operations.clear();

            REQUIRE(retained.Profiles().size() == 1);
            REQUIRE(retained.Profiles()[0].active);
            REQUIRE(retained.Operations().size() == 1);
            REQUIRE(retained.Operations()[0].failureCategory == SaveManagerDiagnosticKind::Storage);
            REQUIRE(retained.Diagnostics().size() == 1);
            REQUIRE(retained.Detail(Id<SaveGameSlotId>(1))->displayName == "Alpha");
            REQUIRE(retained.Detail(Id<SaveGameSlotId>(2))->displayName == "München");
            REQUIRE_FALSE(retained.Detail(Id<SaveGameSlotId>(9)));
        }

        TEST_CASE("Manager pages are bounded and cursors bind exact publication and filter", "[unit][save][manager-projection]") {
            ProjectionFixture source;
            source.catalog.entries[0].display.displayName = "slot-a";
            source.catalog.entries[1].display.displayName = "other";
            source.catalog.entries[2].display.displayName = "slot-c";
            const auto projection = SaveManagerProjection::Create(source.Input()).Value();
            SaveManagerPageRequest request{.filter = {.nameContains = "slot"}, .maximumRows = 1};
            const auto first = projection.Page(request).Value();
            REQUIRE(first.totalMatches == 2);
            REQUIRE(first.rows.size() == 1);
            REQUIRE(first.rows[0].slot == Id<SaveGameSlotId>(1));
            REQUIRE(first.next);
            request.cursor = first.next;
            const auto second = projection.Page(request).Value();
            REQUIRE(second.rows.size() == 1);
            REQUIRE(second.rows[0].slot == Id<SaveGameSlotId>(3));
            REQUIRE_FALSE(second.next);

            request.filter.nameContains = "other";
            REQUIRE(projection.Page(request).ErrorValue().code.Value() == SaveErrors::ManagerProjectionStale.code.Value());
            request.filter.nameContains = "slot";
            ++source.publicationRevision;
            const auto refreshed = SaveManagerProjection::Create(source.Input()).Value();
            REQUIRE(refreshed.Page(request).ErrorValue().code.Value() == SaveErrors::ManagerProjectionStale.code.Value());
            request.cursor->nextMatch = 99;
            REQUIRE(projection.Page(request).ErrorValue().code.Value() == SaveErrors::ManagerProjectionInvalid.code.Value());
        }

        TEST_CASE("Manager commands carry catalog and generation preconditions", "[unit][save][manager-projection]") {
            ProjectionFixture source;
            const auto oldView = SaveManagerProjection::Create(source.Input()).Value();
            const auto command = oldView.Command(SaveManagerCommandKind::Delete, Id<SaveGameSlotId>(1)).Value();
            REQUIRE(oldView.ValidateCommand(command).HasValue());
            REQUIRE(command.expectedSnapshot.catalogRevision == 11);
            REQUIRE(command.expectedSnapshot.bindingRevision == 7);
            REQUIRE(command.expectedGeneration == Id<SlotGenerationId>(10));
            REQUIRE(oldView.Command(SaveManagerCommandKind::Load, Id<SaveGameSlotId>(9)).ErrorValue().code.Value() ==
                    SaveErrors::ManagerProjectionStale.code.Value());

            source.catalog.revision = 12;
            source.catalog.entries[0] = Entry(1, 11);
            source.assessments[0].generation = Id<SlotGenerationId>(11);
            ++source.publicationRevision;
            const auto newView = SaveManagerProjection::Create(source.Input()).Value();
            REQUIRE(newView.ValidateCommand(command).ErrorValue().code.Value() == SaveErrors::ManagerProjectionStale.code.Value());
            auto forged = newView.Command(SaveManagerCommandKind::Load, Id<SaveGameSlotId>(1)).Value();
            forged.expectedGeneration = Id<SlotGenerationId>(10);
            REQUIRE(newView.ValidateCommand(forged).ErrorValue().code.Value() == SaveErrors::ManagerProjectionStale.code.Value());
            ++source.binding.revision;
            ++source.publicationRevision;
            REQUIRE(SaveManagerProjection::Create(source.Input()).Value().ValidateCommand(forged).HasError());
        }

        TEST_CASE("Manager rejects stale assessments, malformed source and excessive input before copy",
                  "[unit][save][manager-projection]") {
            ProjectionFixture source;
            source.assessments[0].generation = Id<SlotGenerationId>(99);
            REQUIRE(SaveManagerProjection::Create(source.Input()).ErrorValue().code.Value() ==
                    SaveErrors::ManagerProjectionStale.code.Value());
            source.assessments[0].generation = Id<SlotGenerationId>(10);
            source.assessments.push_back(source.assessments[0]);
            REQUIRE(SaveManagerProjection::Create(source.Input()).HasError());
            source.assessments.pop_back();
            source.catalog.entries[0].publication.slot = {};
            REQUIRE(SaveManagerProjection::Create(source.Input()).ErrorValue().code.Value() ==
                    SaveErrors::ManagerProjectionInvalid.code.Value());
            source.catalog.entries[0] = Entry(1, 10);
            REQUIRE(SaveManagerProjection::Create(source.Input(), {.maximumSlots = 2}).ErrorValue().code.Value() ==
                    SaveErrors::ManagerProjectionLimitExceeded.code.Value());
            source.binding.active.reset();
            REQUIRE(SaveManagerProjection::Create(source.Input()).HasError());
        }

        TEST_CASE("Profile rows cannot cross product scope or mark the active profile unavailable", "[unit][save][manager-projection]") {
            ProjectionFixture source;
            SaveNamespaceId second = *source.binding.active;
            second.owner = ServerWorldOwner{Id<ServerStorageOwnerId>(4)};
            source.profiles.push_back({second, true});
            const auto projection = SaveManagerProjection::Create(source.Input()).Value();
            REQUIRE(projection.Profiles().size() == 2);
            REQUIRE(projection.Profiles()[0].active);
            REQUIRE_FALSE(projection.Profiles()[1].active);
            source.profiles[0].available = false;
            REQUIRE(SaveManagerProjection::Create(source.Input()).HasError());
            source.profiles[0].available = true;
            source.profiles[1].namespaceId.product = Id<ProductStorageId>(9);
            REQUIRE(SaveManagerProjection::Create(source.Input()).HasError());
        }

        TEST_CASE("Manager page limits and typed filters are cross-platform deterministic", "[unit][save][manager-projection]") {
            ProjectionFixture source;
            source.catalog.entries[0].display.displayName = "München/Slot";
            source.catalog.entries[1].display.displayName = "München\\Slot";
            source.catalog.entries[2].publication.cloudState = SaveSlotCloudState::Conflict;
            const auto projection = SaveManagerProjection::Create(source.Input()).Value();
            SaveManagerPageRequest request{.filter = {.cloud = SaveSlotCloudState::Conflict}, .maximumRows = 1};
            REQUIRE(projection.Page(request).Value().rows[0].slot == Id<SaveGameSlotId>(3));
            request.filter = {.nameContains = "München/"};
            REQUIRE(projection.Page(request).Value().totalMatches == 1);
            request.filter.nameContains = std::string(257, 'x');
            REQUIRE(projection.Page(request).HasError());
            request.filter.nameContains = std::string(1, static_cast<char>(0xff));
            REQUIRE(projection.Page(request).HasError());
            request.filter = {};
            request.maximumRows = 129;
            REQUIRE(projection.Page(request).HasError());
            request.maximumRows = 0;
            REQUIRE(projection.Page(request).HasError());
        }
    }  // namespace
}  // namespace Horo::Runtime
