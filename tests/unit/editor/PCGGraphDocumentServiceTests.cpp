#include "Horo/Editor/SourceFileOpenService.h"
#include "PCGDocumentTestSupport.h"

namespace Horo::Editor {
    using namespace PCGDocumentTestSupport;
    using Reason = PCGDocumentCommandFailureReason;

    TEST_CASE("PCG workspace service focuses one writable asset without replacing unsaved intent", "[unit][editor][pcg][service]") {
        Project project;
        PCGGraphDocumentService service(project.root, Asset(9), Catalog(), &project.mutations, &project.files);
        auto first = service.Open(Asset(), Identity(), project.path);
        REQUIRE(first.HasValue());
        REQUIRE(service.Execute(Identity().instance, Edit(first.Value(), 99)).HasValue());
        project.Write(Capture(Graph(8, 77)));
        auto second = service.Open(Asset(), Identity(2), project.path);
        REQUIRE(second.HasValue());
        CHECK(second.Value().identity.instance == Identity().instance);
        CHECK(second.Value().session == first.Value().session);
        CHECK(second.Value().source->Data().deterministicSeed == 99);
        CHECK(second.Value().dirty);
        CHECK(service.Snapshots().Value().size() == 1);
        REQUIRE(service.Close(Identity().instance, true).HasValue());
        auto reopened = service.Open(Asset(), Identity(3), project.path);
        REQUIRE(reopened.HasValue());
        CHECK(reopened.Value().session.generation > first.Value().session.generation);
        ErrorIs(service.Execute(Identity(3).instance, Edit(second.Value(), 10)), PCGDocumentError(Reason::Stale));
        CHECK(first.Value().source->Data().deterministicSeed == 11);
    }

    TEST_CASE("PCG shared recovery is read-only until a fenced semantic accept and separate source save", "[unit][editor][pcg][recovery]") {
        Project project;
        {
            PCGGraphDocumentService crashed(project.root, Asset(9), Catalog(), &project.mutations, &project.files);
            auto opened = crashed.Open(Asset(), Identity(), project.path);
            REQUIRE(opened.HasValue());
            REQUIRE(crashed.Execute(Identity().instance, Edit(opened.Value(), 42)).HasValue());
            REQUIRE(crashed.Autosave(Identity().instance, Fence(crashed.Snapshot(Identity().instance).Value())).HasValue());
            CHECK(crashed.Snapshot(Identity().instance).Value().dirty);
            CHECK(Assets::CaptureAssetSource(project.root, project.path, 1024 * 1024).Value().bytes == Capture().bytes);
        }
        PCGGraphDocumentService restarted(project.root, Asset(10), Catalog(), &project.mutations, &project.files);
        auto opened = restarted.Open(Asset(), Identity(4), project.path);
        REQUIRE(opened.HasValue());
        auto candidate = restarted.InspectRecovery(Identity(4).instance);
        REQUIRE(candidate.HasValue());
        REQUIRE(candidate.Value().has_value());
        CHECK(candidate.Value()->source->Data().deterministicSeed == 42);
        CHECK_FALSE(restarted.Snapshot(Identity(4).instance).Value().dirty);
        CHECK(restarted.Snapshot(Identity(4).instance).Value().source->Data().deterministicSeed == 11);
        REQUIRE(restarted.AcceptRecovery(Identity(4).instance, Fence(opened.Value()), *candidate.Value()).HasValue());
        auto restored = restarted.Snapshot(Identity(4).instance).Value();
        CHECK(restored.dirty);
        CHECK(restored.source->Data().deterministicSeed == 42);
        CHECK(Assets::CaptureAssetSource(project.root, project.path, 1024 * 1024).Value().bytes == Capture().bytes);
        project.files.failWrite = true;
        REQUIRE(restarted.Autosave(Identity(4).instance, Fence(restored)).HasError());
        CHECK(restarted.InspectRecovery(Identity(4).instance).Value()->record == candidate.Value()->record);
        project.files.failWrite = false;
        REQUIRE(restarted.Autosave(Identity(4).instance, Fence(restored)).HasValue());
        CHECK(restarted.InspectRecovery(Identity(4).instance).Value()->record.sessionOwner == Asset(10));
        REQUIRE(restarted.Replay(Identity(4).instance, Fence(restored), false).HasValue());
        CHECK_FALSE(restarted.Snapshot(Identity(4).instance).Value().dirty);
        REQUIRE(restarted.Replay(Identity(4).instance, Fence(restarted.Snapshot(Identity(4).instance).Value()), true).HasValue());
        REQUIRE(restarted.Save(Identity(4).instance, Fence(restarted.Snapshot(Identity(4).instance).Value())).HasValue());
        CHECK_FALSE(restarted.Snapshot(Identity(4).instance).Value().dirty);
        REQUIRE(restarted.Shutdown(true).HasValue());
        DocumentRecoveryService recovery(project.root, project.mutations, project.files);
        CHECK_FALSE(recovery.Inspect(Asset(), Identity().key).Value().has_value());
    }

    TEST_CASE("PCG recovery conflicts and stale candidates change neither working nor canonical source",
              "[unit][editor][pcg][recovery][conflict]") {
        Project project;
        PCGGraphDocumentService service(project.root, Asset(9), Catalog(), &project.mutations, &project.files);
        REQUIRE(service.Open(Asset(), Identity(), project.path).HasValue());
        REQUIRE(service.Execute(Identity().instance, Edit(service.Snapshot(Identity().instance).Value(), 42)).HasValue());
        REQUIRE(service.Autosave(Identity().instance, Fence(service.Snapshot(Identity().instance).Value())).HasValue());
        auto candidate = service.InspectRecovery(Identity().instance);
        REQUIRE(candidate.HasValue());
        REQUIRE(candidate.Value());
        const auto before = service.Snapshot(Identity().instance).Value();
        SECTION("newer external durable state") {
            project.Write(Capture(Graph(8, 88)));
            ErrorIs(service.AcceptRecovery(Identity().instance, Fence(before), *candidate.Value()), PCGDocumentError(Reason::Conflict));
            CHECK(Assets::CaptureAssetSource(project.root, project.path, 1024 * 1024).Value().bytes == Capture(Graph(8, 88)).bytes);
        }
        SECTION("stale live revision") {
            REQUIRE(service.Execute(Identity().instance, Edit(before, 45)).HasValue());
            ErrorIs(service.AcceptRecovery(Identity().instance, Fence(before), *candidate.Value()), PCGDocumentError(Reason::Stale));
        }
        SECTION("candidate payload substitution") {
            candidate.Value()->record.payload[10] ^= std::byte{1};
            ErrorIs(service.AcceptRecovery(Identity().instance, Fence(before), *candidate.Value()), PCGDocumentError(Reason::Invalid));
        }
        CHECK(service.Snapshot(Identity().instance).Value().dirty);
    }

    TEST_CASE("PCG autosave coalesces exact states and rejects late or different-session recovery writes",
              "[unit][editor][pcg][recovery][boundary]") {
        Project project;
        PCGGraphDocumentService service(project.root, Asset(9), Catalog(), &project.mutations, &project.files);
        REQUIRE(service.Open(Asset(), Identity(), project.path).HasValue());
        REQUIRE(service.Execute(Identity().instance, Edit(service.Snapshot(Identity().instance).Value(), 42)).HasValue());
        REQUIRE(service.Autosave(Identity().instance, Fence(service.Snapshot(Identity().instance).Value())).HasValue());
        auto first = service.InspectRecovery(Identity().instance);
        REQUIRE(first.HasValue());
        REQUIRE(first.Value());
        const auto writes = project.files.writes;
        REQUIRE(service.Autosave(Identity().instance, Fence(service.Snapshot(Identity().instance).Value())).HasValue());
        CHECK(project.files.writes == writes);
        REQUIRE(service.Execute(Identity().instance, Edit(service.Snapshot(Identity().instance).Value(), 43)).HasValue());
        REQUIRE(service.Autosave(Identity().instance, Fence(service.Snapshot(Identity().instance).Value())).HasValue());
        DocumentRecoveryService recovery(project.root, project.mutations, project.files);
        ErrorIs(recovery.Write(first.Value()->record), Assets::AssetSourceErrors::Conflict);
        auto different = recovery.Inspect(Asset(), Identity().key).Value().value();
        ++different.sessionGeneration;
        ErrorIs(recovery.Write(different), Assets::AssetSourceErrors::Conflict);
        CHECK(recovery.Inspect(Asset(), Identity().key).Value()->revision > first.Value()->record.revision);
        ErrorIs(recovery.Discard(first.Value()->record), Assets::AssetSourceErrors::Conflict);
    }

    TEST_CASE("PCG recovery lifecycle preserves roots on failure and closes only after explicit policy",
              "[unit][editor][pcg][recovery][lifecycle]") {
        Project project;
        PCGGraphDocumentService service(project.root, Asset(9), Catalog(), &project.mutations, &project.files);
        REQUIRE(service.Open(Asset(), Identity(), project.path).HasValue());
        REQUIRE(service.Execute(Identity().instance, Edit(service.Snapshot(Identity().instance).Value(), 42)).HasValue());
        const auto reader = service.Snapshot(Identity().instance).Value();
        ErrorIs(service.Close(Identity().instance), PCGDocumentError(Reason::Dirty));
        ErrorIs(service.Shutdown(false), PCGDocumentError(Reason::Dirty));
        SECTION("failed recovery retains retry ownership") {
            project.files.failWrite = true;
            REQUIRE(service.Shutdown(true).HasError());
            CHECK(service.Snapshot(Identity().instance).HasValue());
            project.files.failWrite = false;
            REQUIRE(service.Shutdown(true).HasValue());
        }
        SECTION("autosave refuses lifecycle replacement during staging while later edits remain dirty") {
            project.files.duringWrite = [&] {
                ErrorIs(service.Close(Identity().instance, true), PCGDocumentError(Reason::Busy));
                ErrorIs(service.Shutdown(false, true), PCGDocumentError(Reason::Busy));
                ErrorIs(service.ReplaceCatalog(Catalog(2)), PCGDocumentError(Reason::Busy));
                REQUIRE(service.Execute(Identity().instance, Edit(service.Snapshot(Identity().instance).Value(), 44)).HasValue());
            };
            REQUIRE(service.Autosave(Identity().instance, Fence(reader)).HasValue());
            CHECK(service.Snapshot(Identity().instance).Value().source->Data().deterministicSeed == 44);
            CHECK(service.InspectRecovery(Identity().instance).Value()->source->Data().deterministicSeed == 42);
            REQUIRE(service.Shutdown(true).HasValue());
        }
        SECTION("explicit discard removes only own recovery and preserves source") {
            REQUIRE(service.Autosave(Identity().instance, Fence(reader)).HasValue());
            DocumentRecoveryService recovery(project.root, project.mutations, project.files);
            const auto candidate = recovery.Inspect(Asset(), Identity().key);
            REQUIRE(candidate.HasValue());
            REQUIRE(candidate.Value());
            const auto recoveryPath = recovery.Path(Asset(), Identity().key);
            REQUIRE(recoveryPath.HasValue());
            const auto lockPath = Assets::AssetSourcePublicationLockPath(project.root, recoveryPath.Value());
            REQUIRE(lockPath.HasValue());
            project.files.duringRemove = [&] {
                CHECK(project.files.TryAcquireExclusive(lockPath.Value(), "competing-recovery-writer").HasError());
                CHECK(project.mutations.TryAcquire({project.root, ProjectMutationOwner::Autosave, "competing-discard"}).HasError());
            };
            REQUIRE(service.Shutdown(false, true).HasValue());
            CHECK_FALSE(recovery.Inspect(Asset(), Identity().key).Value());
            CHECK(project.files.TryAcquireExclusive(lockPath.Value(), "released-recovery-lease").HasValue());
        }
        SECTION("shutdown retains edits made after its recovery snapshot") {
            project.files.duringWrite = [&] {
                REQUIRE(service.Execute(Identity().instance, Edit(service.Snapshot(Identity().instance).Value(), 44)).HasValue());
            };
            ErrorIs(service.Shutdown(true), PCGDocumentError(Reason::Stale));
            CHECK(service.Snapshot(Identity().instance).Value().source->Data().deterministicSeed == 44);
            REQUIRE(service.Shutdown(true).HasValue());
        }
        ErrorIs(service.Snapshot(Identity().instance), PCGDocumentError(Reason::Closed));
        CHECK(reader.source->Data().deterministicSeed == 42);
        CHECK(Assets::CaptureAssetSource(project.root, project.path, 1024 * 1024).Value().bytes == Capture().bytes);
    }

    TEST_CASE("PCG clean and dirty external observations require explicit root replacement", "[unit][editor][pcg][external]") {
        Project project;
        PCGGraphDocumentService service(project.root, Asset(9), Catalog(), &project.mutations, &project.files);
        const auto opened = service.Open(Asset(), Identity(), project.path).Value();
        REQUIRE(service.Execute(Identity().instance, Edit(opened, 42)).HasValue());
        const auto edited = service.Snapshot(Identity().instance).Value();
        project.Write(Capture(Graph(2, 99)));
        auto observed = service.InspectExternal(Identity().instance);
        REQUIRE(observed.HasValue());
        CHECK(observed.Value().externalConflict);
        CHECK(observed.Value().source == edited.source);
        CHECK(observed.Value().acceptedSource == edited.acceptedSource);
        ErrorIs(service.Reload(Identity().instance, Fence(observed.Value()), false), PCGDocumentError(Reason::Dirty));
        REQUIRE(service.Reload(Identity().instance, Fence(observed.Value()), true).HasValue());
        const auto reloaded = service.Snapshot(Identity().instance).Value();
        CHECK_FALSE(reloaded.dirty);
        CHECK_FALSE(reloaded.externalConflict);
        CHECK(reloaded.source->Data().deterministicSeed == 99);
        ErrorIs(service.Replay(Identity().instance, Fence(reloaded), false), PCGDocumentError(Reason::Invalid));
    }

    TEST_CASE("PCG close and shutdown reject reentrant newer edits during recovery discard", "[unit][editor][pcg][lifecycle]") {
        Project project;
        PCGGraphDocumentService service(project.root, Asset(9), Catalog(), &project.mutations, &project.files);
        REQUIRE(service.Open(Asset(), Identity(), project.path).HasValue());
        REQUIRE(service.Execute(Identity().instance, Edit(service.Snapshot(Identity().instance).Value(), 42)).HasValue());
        REQUIRE(service.Autosave(Identity().instance, Fence(service.Snapshot(Identity().instance).Value())).HasValue());
        project.files.duringRemove = [&] {
            ErrorIs(service.Close(Identity().instance, true), PCGDocumentError(Reason::Busy));
            REQUIRE(service.Execute(Identity().instance, Edit(service.Snapshot(Identity().instance).Value(), 44)).HasValue());
        };
        SECTION("document close") {
            ErrorIs(service.Close(Identity().instance, true), PCGDocumentError(Reason::Stale));
        }
        SECTION("workspace shutdown") {
            ErrorIs(service.Shutdown(false, true), PCGDocumentError(Reason::Stale));
        }
        CHECK(service.Snapshot(Identity().instance).Value().source->Data().deterministicSeed == 44);
        REQUIRE(service.Shutdown(true).HasValue());
        DocumentRecoveryService recovery(project.root, project.mutations, project.files);
        CHECK(recovery.Inspect(Asset(), Identity().key).Value()->sessionOwner == Asset(9));
        CHECK(Assets::CaptureAssetSource(project.root, project.path, 1024 * 1024).Value().bytes == Capture().bytes);
    }

    TEST_CASE("PCG missing node catalog support preserves exact bytes and fences old commands", "[unit][editor][pcg][catalog]") {
        Project project;
        PCGGraphDocumentService service(project.root, Asset(9), Catalog(), &project.mutations, &project.files);
        const auto opened = service.Open(Asset(), Identity(), project.path).Value();
        auto catalog = Catalog(2);
        catalog.types.clear();
        REQUIRE(service.ReplaceCatalog(catalog).HasValue());
        const auto unknown = service.Snapshot(Identity().instance).Value();
        CHECK_FALSE(unknown.source->IsCookEligible());
        CHECK(unknown.source->Data() == opened.source->Data());
        ErrorIs(service.Execute(Identity().instance, Edit(opened, 15)), PCGDocumentError(Reason::Stale));
        REQUIRE(service.Execute(Identity().instance, Edit(unknown, 42)).HasValue());
        REQUIRE(service.Save(Identity().instance, Fence(service.Snapshot(Identity().instance).Value())).HasValue());
        const auto saved = Assets::CaptureAssetSource(project.root, project.path, 1024 * 1024).Value();
        PCG::PCGGraphSourceContext context{.unknownNodePolicy = PCG::PCGUnknownNodePolicy::PreserveInert};
        auto graph =
            PCG::DeserializePCGGraphAsset({reinterpret_cast<const std::uint8_t *>(saved.bytes.data()), saved.bytes.size()}, context);
        REQUIRE(graph.HasValue());
        CHECK(graph.Value().Data().nodes == opened.source->Data().nodes);
    }

    TEST_CASE("PCG empty workspace rejects malformed catalog replacement before later opening", "[unit][editor][pcg][catalog]") {
        Project project;
        PCGGraphDocumentService service(project.root, Asset(9), Catalog(), &project.mutations, &project.files);
        auto invalid = Catalog(2);
        invalid.types.push_back(invalid.types.front());
        REQUIRE(service.ReplaceCatalog(invalid).HasError());
        const auto opened = service.Open(Asset(), Identity(), project.path);
        REQUIRE(opened.HasValue());
        CHECK(opened.Value().catalogRevision == 1);
    }

    TEST_CASE("PCG recovery storage rejects quotas corruption and unsafe paths without source mutation", "[unit][editor][pcg][storage]") {
        Project project;
        PCGGraphDocumentService service(project.root, Asset(9), Catalog(), &project.mutations, &project.files);
        REQUIRE(service.Open(Asset(), Identity(), project.path).HasValue());
        REQUIRE(service.Execute(Identity().instance, Edit(service.Snapshot(Identity().instance).Value(), 42)).HasValue());
        REQUIRE(service.Autosave(Identity().instance, Fence(service.Snapshot(Identity().instance).Value())).HasValue());
        auto candidate = service.InspectRecovery(Identity().instance).Value().value();
        DocumentRecoveryService tiny(project.root, project.mutations, project.files, {.maximumRecordBytes = 1});
        ErrorIs(tiny.Write(candidate.record), Assets::AssetSourceErrors::Limit);
        auto second = candidate.record;
        second.asset = Asset(2);
        DocumentRecoveryService one(project.root, project.mutations, project.files, {.maximumRecords = 1});
        ErrorIs(one.Write(second), Assets::AssetSourceErrors::Limit);
        const auto recoveryRoot = project.root / ".horo/local/recovery";
        const auto recoveryPath = std::filesystem::directory_iterator(recoveryRoot)->path();
        {
            std::ofstream out(recoveryPath, std::ios::binary | std::ios::trunc);
            out << "broken";
        }
        REQUIRE(service.InspectRecovery(Identity().instance).HasError());
        CHECK(Assets::CaptureAssetSource(project.root, project.path, 1024 * 1024).Value().bytes == Capture().bytes);
        ExclusiveFileLock missing;
        ErrorIs(Assets::PublishAssetSource(project.root, project.path, Capture().identity, Capture().bytes, missing, project.files),
                Assets::AssetSourceErrors::Authority);
        ErrorIs(Assets::CaptureAssetSource(project.root, project.root.parent_path() / "outside.hpcg", 1024),
                Assets::AssetSourceErrors::InvalidPath);
    }

    TEST_CASE("Binary PCG routes bypass UTF8 source admission and persist a distinct graph key", "[unit][editor][pcg][routing]") {
        Project project;
        SourceFileOpenService routing(project.root);
        auto opened = routing.Open({.path = project.path, .mode = SourceOpenMode::EmbeddedOnly});
        REQUIRE(opened.HasValue());
        CHECK(opened.Value().classification.kind == SourceFileKind::PCGGraph);
        REQUIRE(opened.Value().document);
        CHECK(opened.Value().document->identity.key.kind == DocumentKind::PCGGraph);
        CHECK_FALSE(opened.Value().sourceSnapshot);
        CHECK(routing.Documents().Snapshot(opened.Value().document->identity.instance).HasError());
        auto serialized = SerializeDocumentOpenKey(opened.Value().document->identity.key);
        REQUIRE(serialized.HasValue());
        CHECK(DeserializeDocumentOpenKey(serialized.Value()).Value() == opened.Value().document->identity.key);
        auto again = routing.Open({.path = Identity().key.source.Value(), .mode = SourceOpenMode::EmbeddedOnly});
        REQUIRE(again.HasValue());
        CHECK(again.Value().document->identity.instance == opened.Value().document->identity.instance);
    }
}  // namespace Horo::Editor
