#include "Horo/PCG/PCGErrors.h"
#include "PCGDocumentTestSupport.h"

#include <limits>
#include <thread>

namespace Horo::Editor {
    using namespace PCGDocumentTestSupport;
    using Reason = PCGDocumentCommandFailureReason;

    TEST_CASE("PCG coalesced edits retain exact element deltas and independent dirty identity", "[unit][editor][pcg][history]") {
        Project project;
        auto document = project.Open();
        const auto original = document.Snapshot().Value();
        auto command = Fence(original);
        auto node = original.source->Data().nodes[0];
        node.payload = {8, 9};
        auto input = original.source->Data().exposedInputs[0];
        input.defaultValue = 5.0;
        command.operations = {PutPCGNode{node}, PutPCGInput{input}, RemovePCGEdge{Id<PCG::EdgeId>(1)}, RemovePCGNode{Id<PCG::NodeId>(2)},
                              SetPCGGraphPolicy{PCG::PCGGenerationMode::Offline, 77}};
        REQUIRE(document.Execute(command).HasValue());
        const auto edited = document.Snapshot().Value();
        CHECK(edited.dirty);
        CHECK(edited.source->Data().mode == PCG::PCGGenerationMode::Offline);
        CHECK(edited.source->Data().deterministicSeed == 77);
        CHECK(edited.revision == original.revision + 1);
        CHECK(document.UndoCount() == 1);
        REQUIRE(document.Replay(Fence(edited), false).HasValue());
        const auto undone = document.Snapshot().Value();
        CHECK(undone.source->Data() == original.source->Data());
        CHECK(undone.state == original.state);
        CHECK_FALSE(undone.dirty);
        REQUIRE(document.Replay(Fence(undone), true).HasValue());
        CHECK(document.Snapshot().Value().source->Data() == edited.source->Data());
        CHECK(original.source->Data().deterministicSeed == 11);
    }

    TEST_CASE("PCG stale session revision and catalog fences preserve all owners", "[unit][editor][pcg][boundary]") {
        Project project;
        auto document = project.Open();
        const auto original = document.Snapshot().Value();
        SECTION("stale session") {
            auto command = Edit(original, 12);
            ++command.session.generation;
            ErrorIs(document.Execute(command), PCGDocumentError(Reason::Stale));
        }
        SECTION("stale revision") {
            auto command = Edit(original, 12);
            ++command.expectedRevision;
            ErrorIs(document.Execute(command), PCGDocumentError(Reason::Stale));
        }
        SECTION("stale catalog") {
            auto command = Edit(original, 12);
            ++command.catalogRevision;
            ErrorIs(document.Execute(command), PCGDocumentError(Reason::Stale));
        }
        CHECK(document.Snapshot().Value().source == original.source);
        CHECK(document.Snapshot().Value().revision == original.revision);
        CHECK_FALSE(document.Snapshot().Value().dirty);
        CHECK(document.UndoCount() == 0);
        CHECK(document.RedoCount() == 0);
    }

    TEST_CASE("PCG unauthorized and cancelled edits preserve all owners", "[unit][editor][pcg][boundary]") {
        Project project;
        auto document = project.Open();
        const auto original = document.Snapshot().Value();
        SECTION("unauthorized") {
            auto command = Edit(original, 12);
            command.canEdit = false;
            ErrorIs(document.Execute(command), PCGDocumentError(Reason::Unauthorized));
        }
        SECTION("cancelled gesture") {
            CancellationSource cancel;
            cancel.RequestCancellation();
            auto command = Edit(original, 12);
            command.cancellation = cancel.Token();
            ErrorIs(document.Execute(command), PCGDocumentError(Reason::Cancelled));
        }
        CHECK(document.Snapshot().Value().source == original.source);
        CHECK(document.Snapshot().Value().revision == original.revision);
        CHECK_FALSE(document.Snapshot().Value().dirty);
        CHECK(document.UndoCount() == 0);
        CHECK(document.RedoCount() == 0);
    }

    TEST_CASE("PCG finite transaction and payload budgets preserve all owners", "[unit][editor][pcg][boundary]") {
        Project project;
        auto document = project.Open();
        const auto original = document.Snapshot().Value();
        SECTION("empty") {
            ErrorIs(document.Execute(Fence(original)), PCGDocumentError(Reason::Limit));
        }
        SECTION("complete transaction cost") {
            auto command = Edit(original, 12);
            command.operations.resize(257);
            ErrorIs(document.Execute(command), PCGDocumentError(Reason::Limit));
        }
        SECTION("oversized semantic payload") {
            auto node = original.source->Data().nodes[0];
            node.payload.resize(PCG::PCGGraphSourceHardLimits::NodePayloadBytes + 1);
            auto command = Fence(original);
            command.operations = {PutPCGNode{node}};
            ErrorIs(document.Execute(command), PCGDocumentError(Reason::Limit));
        }
        CHECK(document.Snapshot().Value().source == original.source);
        CHECK(document.Snapshot().Value().revision == original.revision);
        CHECK_FALSE(document.Snapshot().Value().dirty);
        CHECK(document.UndoCount() == 0);
        CHECK(document.RedoCount() == 0);
    }

    TEST_CASE("PCG invalid semantic intent preserves all owners", "[unit][editor][pcg][boundary]") {
        Project project;
        auto document = project.Open();
        const auto original = document.Snapshot().Value();
        SECTION("dangling endpoint") {
            auto command = Fence(original);
            command.operations = {RemovePCGNode{Id<PCG::NodeId>(2)}};
            REQUIRE(document.Execute(command).HasError());
        }
        SECTION("failure after valid intermediate intent") {
            auto command = Edit(original, 12);
            command.operations.push_back(RemovePCGInput{Id<PCG::ExposedInputId>(99)});
            ErrorIs(document.Execute(command), PCGDocumentError(Reason::Invalid));
        }
        SECTION("baseline catalog rejects hybrid policy without weakening its tier contract") {
            auto command = Fence(original);
            command.operations = {SetPCGGraphPolicy{PCG::PCGGenerationMode::Hybrid, 77}};
            ErrorIs(document.Execute(command), PCG::PCGErrors::GraphSourceMalformed);
        }
        CHECK(document.Snapshot().Value().source == original.source);
        CHECK(document.Snapshot().Value().revision == original.revision);
        CHECK_FALSE(document.Snapshot().Value().dirty);
        CHECK(document.UndoCount() == 0);
        CHECK(document.RedoCount() == 0);
    }

    TEST_CASE("PCG history reserves exact reversible form and evicts only old semantic entries", "[unit][editor][pcg][history]") {
        Project project;
        SECTION("oversized reversible delta fails before mutation") {
            auto document = project.Open({2, 1});
            const auto before = document.Snapshot().Value();
            ErrorIs(document.Execute(Edit(before, 12)), PCGDocumentError(Reason::HistoryLimit));
            CHECK(document.Snapshot().Value().source == before.source);
            CHECK(document.UndoCount() == 0);
        }
        SECTION("item eviction and redo branch") {
            auto document = project.Open({2, 4096});
            for (std::uint64_t seed = 12; seed < 15; ++seed)
                REQUIRE(document.Execute(Edit(document.Snapshot().Value(), seed)).HasValue());
            CHECK(document.UndoCount() == 2);
            REQUIRE(document.Replay(Fence(document.Snapshot().Value()), false).HasValue());
            REQUIRE(document.Replay(Fence(document.Snapshot().Value()), false).HasValue());
            CHECK(document.Snapshot().Value().source->Data().deterministicSeed == 12);
            CHECK(document.Snapshot().Value().dirty);
            auto same = Edit(document.Snapshot().Value(), 12);
            REQUIRE(document.Execute(same).HasValue());
            CHECK(document.RedoCount() == 2);
            REQUIRE(document.Execute(Edit(document.Snapshot().Value(), 99)).HasValue());
            CHECK(document.RedoCount() == 0);
        }
    }

    TEST_CASE("PCG exact save captures preserve edits during publication and history", "[unit][editor][pcg][save]") {
        Project project;
        auto document = project.Open();
        REQUIRE(document.Execute(Edit(document.Snapshot().Value(), 12)).HasValue());
        auto save = document.CaptureSave(Fence(document.Snapshot().Value()));
        REQUIRE(save.HasValue());
        project.files.duringWrite = [&] {
            REQUIRE(document.Execute(Edit(document.Snapshot().Value(), 13)).HasValue());
            ErrorIs(document.Close(true), PCGDocumentError(Reason::Busy));
        };
        auto published = document.Save(save.Value(), project.root, project.path, project.mutations, project.files);
        REQUIRE(published.HasValue());
        CHECK(published.Value().status == Assets::AssetSourcePublicationStatus::Durable);
        const auto current = document.Snapshot().Value();
        CHECK(current.dirty);
        CHECK(current.source->Data().deterministicSeed == 13);
        CHECK(current.acceptedGeneration.revision == Id<PCG::GraphRevision>(2));
        CHECK(document.UndoCount() == 2);
        auto bytes = Assets::CaptureAssetSource(project.root, project.path, 1024 * 1024);
        REQUIRE(bytes.HasValue());
        CHECK(bytes.Value().bytes == std::vector<std::byte>(save.Value().Bytes().begin(), save.Value().Bytes().end()));
        REQUIRE(document.Replay(Fence(current), false).HasValue());
        CHECK_FALSE(document.Snapshot().Value().dirty);
        CHECK(document.Snapshot().Value().source->Data().deterministicSeed == 12);
    }

    TEST_CASE("PCG shared source lease and project authority remain held through replacement", "[unit][editor][pcg][locking]") {
        Project project;
        auto document = project.Open();
        auto save = document.CaptureSave(Fence(document.Snapshot().Value()));
        REQUIRE(save.HasValue());
        ProjectMutationCoordinator another(project.files);
        project.files.duringWrite = [&] {
            CHECK(project.mutations.TryAcquire({project.root, ProjectMutationOwner::Save, "same-owner"}).HasError());
            CHECK(another.TryAcquire({project.root, ProjectMutationOwner::Asset, "other-owner"}).HasError());
            const auto path = Assets::AssetSourcePublicationLockPath(project.root, project.path);
            REQUIRE(path.HasValue());
            CHECK(project.files.TryAcquireExclusive(path.Value(), "another-source-writer").HasError());
        };
        REQUIRE(document.Save(save.Value(), project.root, project.path, project.mutations, project.files).HasValue());
        CHECK(project.mutations.TryAcquire({project.root, ProjectMutationOwner::Save, "released"}).HasValue());
    }

    TEST_CASE("PCG replacement catalog and close fence every late save while immutable readers survive", "[unit][editor][pcg][lifecycle]") {
        Project project;
        auto document = project.Open();
        const auto reader = document.Snapshot().Value();
        auto save = document.CaptureSave(Fence(reader));
        REQUIRE(save.HasValue());
        SECTION("explicit source root replacement") {
            const auto incoming = Capture(Graph(2, 20));
            REQUIRE(document.Reload(Fence(reader), incoming, false).HasValue());
            ErrorIs(document.Save(save.Value(), project.root, project.path, project.mutations, project.files),
                    PCGDocumentError(Reason::Stale));
            CHECK(document.UndoCount() == 0);
            CHECK_FALSE(document.Snapshot().Value().dirty);
        }
        SECTION("missing node semantics after catalog replacement") {
            auto catalog = Catalog(2);
            catalog.types.clear();
            REQUIRE(document.ReplaceCatalog(catalog).HasValue());
            CHECK_FALSE(document.Snapshot().Value().source->IsCookEligible());
            CHECK(document.Snapshot().Value().source->UnknownNodeCount() == 2);
            CHECK(document.Snapshot().Value().source->Data() == reader.source->Data());
            CHECK_FALSE(document.Snapshot().Value().dirty);
            ErrorIs(document.Save(save.Value(), project.root, project.path, project.mutations, project.files),
                    PCGDocumentError(Reason::Stale));
            ErrorIs(document.Execute(Edit(reader, 15)), PCGDocumentError(Reason::Stale));
        }
        SECTION("close") {
            REQUIRE(document.Close().HasValue());
            ErrorIs(document.Save(save.Value(), project.root, project.path, project.mutations, project.files),
                    PCGDocumentError(Reason::Closed));
        }
        CHECK(reader.source->Data().deterministicSeed == 11);
        CHECK(project.files.writes == 0);
    }

    TEST_CASE("PCG migration requires an explicit bounded converter and never publishes converted candidates",
              "[unit][editor][pcg][migration]") {
        auto legacy = Capture();
        legacy.bytes[7] = std::byte{0};
        legacy.identity.digest = ComputeSha256(legacy.bytes);
        SyntheticMigrator migrator;
        auto rejected = PCGGraphDocument::Open(Asset(), Identity(), {Asset(9), 1}, legacy, Catalog());
        REQUIRE(rejected.HasError());
        auto opened = PCGGraphDocument::Open(Asset(), Identity(), {Asset(9), 1}, legacy, Catalog(), {}, &migrator);
        REQUIRE(opened.HasValue());
        CHECK(opened.Value().Snapshot().Value().dirty);
        CHECK(opened.Value().Snapshot().Value().acceptedSource == legacy.identity);
        migrator.invalidOutput = true;
        REQUIRE(PCGGraphDocument::Open(Asset(), Identity(), {Asset(9), 2}, legacy, Catalog(), {}, &migrator).HasError());
        auto future = Capture();
        future.bytes[5] = std::byte{9};
        future.identity.digest = ComputeSha256(future.bytes);
        REQUIRE(PCGGraphDocument::Open(Asset(), Identity(), {Asset(9), 3}, future, Catalog(), {}, &migrator).HasError());
    }

    TEST_CASE("PCG rejects wrong-thread commands without accessing authored storage", "[unit][editor][pcg][thread]") {
        Project project;
        auto document = project.Open();
        const auto before = document.Snapshot().Value();
        bool rejected{};
        std::thread other([&] {
            auto result = document.Execute(Edit(before, 19));
            rejected = result.HasError() && result.ErrorValue().code.Value() == PCGDocumentError(Reason::WrongThread).code.Value();
        });
        other.join();
        CHECK(rejected);
        CHECK(document.Snapshot().Value().source == before.source);
    }

}  // namespace Horo::Editor
