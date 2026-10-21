#include "Horo/Editor/SourceFileOpenService.h"
#include "PrefabHierarchyTestSupport.h"
#include "editor/screens/workspace/prefab/PrefabDocumentSession.h"

#include <thread>

namespace Horo::Editor {
    using namespace PrefabHierarchyTestSupport;

    namespace {
        struct StageProbe final {
            bool hold{true};
            std::size_t shutdowns{}, fixedUpdates{};
        };

        /** @brief Real Scene service behind an explicitly owned deferred-preparation test host. */
        class StageHost final : public EditorPlaySceneHost {
        public:
            StageHost(Runtime::SceneRuntimeId id, std::shared_ptr<StageProbe> probe) : scenes_(id), probe_(std::move(probe)) {}

            Runtime::RuntimeSceneService &Scenes() noexcept override {
                return scenes_;
            }

            Result<void> CommitSceneChanges(const Runtime::FrameContext &frame) override {
                return probe_->hold ? Result<void>::Success() : EditorPlaySceneHost::CommitSceneChanges(frame);
            }

            Result<void> FixedUpdate(Gameplay::FixedDeltaTime) override {
                ++probe_->fixedUpdates;
                return Result<void>::Success();
            }

            void Shutdown() noexcept override {
                ++probe_->shutdowns;
                scenes_.Shutdown();
            }

        private:
            Runtime::RuntimeSceneService scenes_;
            std::shared_ptr<StageProbe> probe_;
        };

        /** @brief Owns real canonical files, shared identity registry and one source authority. */
        struct Fixture final {
            Project project;
            DocumentIdentityRegistry identities;
            Prefab::PrefabLimitProfile limits{Prefab::PrefabLimitProfile::Create({}).Value()};
            std::unique_ptr<PrefabDocumentSession> document;

            Fixture() {
                auto key = DocumentOpenKey{DocumentKind::Prefab, SourceDocumentId::Parse(project.record.sourcePath.String()).Value()};
                auto opened = identities.Open(key);
                REQUIRE(opened.HasValue());
                auto session = PrefabDocumentSession::Open(project.directory.path, project.source, project.asset, opened.Value().identity,
                                                           PrefabAsset(9).Asset(), limits);
                REQUIRE(session.HasValue());
                document = std::move(session).Value();
            }

            void Rename(const std::string &name) {
                REQUIRE(document->Execute(document->Snapshot().fence, RenamePrefabObject{{0}, name}).HasValue());
            }

            auto Capture() {
                const std::array roots{project.asset};
                auto captured = project.Capture(project.registry.Snapshot(), roots, limits, {});
                REQUIRE(captured.HasValue());
                return std::move(captured).Value();
            }
        };
    }  // namespace

    TEST_CASE("Prefab source routing retains a distinct persistent shared registry identity", "[editor][prefab][session]") {
        Fixture fixture;
        SourceFileOpenService open(fixture.project.directory.path, fixture.identities);
        auto first = open.Open({.path = fixture.project.source});
        REQUIRE(first.HasValue());
        CHECK(first.Value().classification.kind == SourceFileKind::Prefab);
        CHECK(first.Value().document->identity == fixture.document->Snapshot().identity);
        CHECK(first.Value().document->disposition == DocumentOpenDisposition::FocusExisting);
        CHECK_FALSE(first.Value().sourceSnapshot.has_value());
        auto encoded = SerializeDocumentOpenKey(first.Value().document->identity.key);
        REQUIRE(encoded.HasValue());
        CHECK(ParseDocumentKind("prefab").Value() == DocumentKind::Prefab);
    }

    TEST_CASE("Prefab semantic history separates command revisions from canonical saved state", "[editor][prefab][session]") {
        Fixture fixture;
        const auto initial = fixture.document->Snapshot();
        fixture.Rename("Changed");
        auto changed = fixture.document->Snapshot();
        CHECK(changed.dirty);
        CHECK(changed.fence.revision != initial.fence.revision);
        CHECK(changed.sourceRevision != initial.sourceRevision);
        CHECK(changed.savedState == initial.savedState);
        CHECK(changed.recoveryRevision == 0);
        CHECK(changed.previewRevision == 0);
        CHECK(initial.source->Data().objects.front().name == "Source root");
        REQUIRE(fixture.document->Replay(changed.fence, false).HasValue());
        CHECK_FALSE(fixture.document->Snapshot().dirty);
        CHECK(fixture.document->Snapshot().sourceRevision == initial.sourceRevision);
        REQUIRE(fixture.document->Replay(fixture.document->Snapshot().fence, true).HasValue());
        CHECK(fixture.document->Snapshot().dirty);
        CHECK_FALSE(fixture.document->Execute(initial.fence, RenamePrefabObject{{0}, "Stale"}).HasValue());
    }

    TEST_CASE("Prefab source accepts its full UTF8 byte boundary and keeps no-op edits out of history", "[editor][prefab][session]") {
        Fixture fixture;
        std::string boundary;
        while (boundary.size() + 2 <= Prefab::MaximumPrefabObjectNameBytes)
            boundary += "\xc5\x9f";
        boundary.append(Prefab::MaximumPrefabObjectNameBytes - boundary.size(), 'x');
        fixture.Rename(boundary);
        const auto accepted = fixture.document->Snapshot();
        CHECK(accepted.source->Data().objects.front().name.size() == Prefab::MaximumPrefabObjectNameBytes);
        fixture.Rename(accepted.source->Data().objects.front().name);
        CHECK(fixture.document->Snapshot().fence.revision == accepted.fence.revision);
        auto saved = fixture.document->Save(accepted.fence, fixture.project.mutations, fixture.project.files);
        REQUIRE(saved.HasValue());
        REQUIRE(fixture.document->Reload(fixture.document->Snapshot().fence, false).HasValue());
        CHECK(fixture.document->Snapshot().source->Data().objects.front().name == boundary);
    }

    TEST_CASE("Prefab malformed and foreign edits publish no state or history", "[editor][prefab][session]") {
        Fixture fixture;
        const auto before = fixture.document->Snapshot();
        SECTION("missing local object") {
            CHECK(fixture.document->Execute(before.fence, RenamePrefabObject{{999}, "Missing"}).HasError());
        }
        SECTION("invalid UTF8 name") {
            CHECK(fixture.document->Execute(before.fence, RenamePrefabObject{{0}, "bad\xc0\xaf"}).HasError());
        }
        SECTION("oversized name") {
            CHECK(
                fixture.document->Execute(before.fence, RenamePrefabObject{{0}, std::string(Prefab::MaximumPrefabObjectNameBytes + 1, 'x')})
                    .HasError());
        }
        SECTION("nonfinite transform") {
            Math::Transform transform;
            transform.translation.x = std::numeric_limits<float>::infinity();
            CHECK(fixture.document->Execute(before.fence, TransformPrefabObject{{0}, transform}).HasError());
        }
        SECTION("foreign workspace incarnation") {
            auto foreign = before.fence;
            foreign.owner = PrefabAsset(8).Asset();
            CHECK(fixture.document->Execute(foreign, RenamePrefabObject{{0}, "Foreign"}).HasError());
        }
        SECTION("wrong owner thread") {
            bool rejected{};
            std::thread other([&] {
                rejected = fixture.document->Execute(before.fence, RenamePrefabObject{{0}, "Thread"}).HasError();
            });
            other.join();
            CHECK(rejected);
        }
        CHECK(fixture.document->Snapshot().fence.revision == before.fence.revision);
        CHECK_FALSE(fixture.document->Snapshot().dirty);
        CHECK_FALSE(fixture.document->Snapshot().canUndo);
    }

    TEST_CASE("Prefab runtime stage contains actual isolated entities and releases without source mutation", "[editor][prefab][session]") {
        Fixture fixture;
        const auto initial = fixture.document->Snapshot();
        auto sources = fixture.Capture();
        REQUIRE(fixture.document->ActivateStage(fixture.project.registry.Snapshot(), sources.sources.resolver, {41}).HasValue());
        REQUIRE(fixture.document->Stage() != nullptr);
        CHECK(fixture.document->Stage()->View().SlotCount() == 2);
        const auto preview = fixture.document->Snapshot();
        CHECK(preview.previewRevision == 1);
        CHECK(preview.sourceRevision == initial.sourceRevision);
        CHECK(preview.fence.revision == initial.fence.revision);
        CHECK_FALSE(preview.dirty);
        fixture.document->ReleaseStage();
        CHECK(fixture.document->Stage() == nullptr);
        CHECK(fixture.document->Snapshot().previewRevision == preview.previewRevision);
        REQUIRE(fixture.document->ActivateStage(fixture.project.registry.Snapshot(), sources.sources.resolver, {42}).HasValue());
        CHECK(fixture.document->Snapshot().previewRevision == 2);
        CHECK(fixture.document->Snapshot().sourceRevision == initial.sourceRevision);
        fixture.Rename("Unsaved source");
        CHECK(fixture.document->Stage() == nullptr);
        REQUIRE(fixture.document->ActivateStage(fixture.project.registry.Snapshot(), sources.sources.resolver, {43}).HasValue());
        CHECK(fixture.document->Snapshot().dirty);
        CHECK(fixture.document->Snapshot().previewRevision == 3);
    }

    TEST_CASE("Prefab stage projects an actual primitive and extracted leases survive retirement", "[editor][prefab][session]") {
        Fixture fixture;
        auto data = fixture.document->Snapshot().source->Data();
        Prefab::RawComponentPayload raw{.instance = Prefab::PrefabComponentInstanceId::Create(1).Value(),
                                        .component = {.typeId = Gameplay::ComponentTypeId::Parse("game.horo.primitive_mesh").Value(),
                                                      .schemaVersion = 1,
                                                      .encoding = Gameplay::ComponentPayloadEncoding::CanonicalJson}};
        constexpr std::string_view payload = R"({"id":"primitive.mesh.box","version":1,"parameters":{"size":[1,1,1]}})";
        for (const char byte : payload)
            raw.component.payload.push_back(static_cast<std::byte>(byte));
        data.objects.front().components.push_back(raw);
        auto source = Prefab::PrefabDocument::Create(std::move(data), fixture.limits);
        REQUIRE(source.HasValue());
        auto encoded = source.Value().SerializeCanonical();
        REQUIRE(encoded.HasValue());
        std::ofstream(fixture.project.source, std::ios::trunc) << encoded.Value();
        REQUIRE(fixture.document->Reload(fixture.document->Snapshot().fence, false).HasValue());
        auto sources = fixture.Capture();
        REQUIRE(fixture.document->ActivateStage(fixture.project.registry.Snapshot(), sources.sources.resolver, {61}).HasValue());
        Runtime::PrimitiveMeshCache meshes;
        auto extracted = ExtractEditorViewportScene(fixture.document->Stage()->View(), {1}, {}, meshes);
        REQUIRE(extracted.HasValue());
        REQUIRE(extracted.Value().instances.size() == 1);
        REQUIRE(extracted.Value().meshLeases.size() == 1);
        fixture.document->ReleaseStage();
        CHECK(extracted.Value().meshLeases.front().Data().vertices.size() > 0);
        CHECK_FALSE(fixture.document->Snapshot().dirty);
    }

    TEST_CASE("Prefab hosted stage advances separately and cancels without gameplay or source mutation", "[editor][prefab][session]") {
        Fixture fixture;
        auto sources = fixture.Capture();
        auto probe = std::make_shared<StageProbe>();
        EditorPlaySceneFactory factory = [probe](Runtime::SceneRuntimeId id,
                                                 const Runtime::RuntimeSceneDefinition &) -> Result<std::unique_ptr<EditorPlaySceneHost>> {
            return Result<std::unique_ptr<EditorPlaySceneHost>>::Success(std::make_unique<StageHost>(id, probe));
        };
        const auto initial = fixture.document->Snapshot();
        REQUIRE(fixture.document->ActivateStage(fixture.project.registry.Snapshot(), sources.sources.resolver, {81}, factory).HasValue());
        CHECK(fixture.document->HasStage());
        CHECK_FALSE(fixture.document->Snapshot().stageActive);
        REQUIRE_FALSE(fixture.document->AdvanceStage().Value());
        CHECK(fixture.document->Snapshot().previewRevision == 0);
        fixture.document->ReleaseStage();
        CHECK(probe->shutdowns == 1);
        probe->hold = false;
        REQUIRE(fixture.document->ActivateStage(fixture.project.registry.Snapshot(), sources.sources.resolver, {82}, factory).HasValue());
        for (int frame = 0; frame < 16 && !fixture.document->Snapshot().stageActive; ++frame)
            REQUIRE(fixture.document->AdvanceStage().HasValue());
        REQUIRE(fixture.document->Snapshot().stageActive);
        REQUIRE(fixture.document->StageView().has_value());
        CHECK(fixture.document->StageView()->SlotCount() == 2);
        CHECK(fixture.document->Snapshot().previewRevision == 1);
        CHECK(fixture.document->Snapshot().sourceRevision == initial.sourceRevision);
        CHECK(fixture.document->Snapshot().fence.revision == initial.fence.revision);
        CHECK(probe->fixedUpdates == 0);
        REQUIRE(fixture.document->Close(fixture.document->Snapshot().fence, false).HasValue());
        CHECK(probe->shutdowns == 2);
    }

    TEST_CASE("Prefab save publishes canonical source and external conflicts retain local history", "[editor][prefab][session]") {
        Fixture fixture;
        fixture.Rename("Saved");
        auto saved = fixture.document->Save(fixture.document->Snapshot().fence, fixture.project.mutations, fixture.project.files);
        REQUIRE(saved.HasValue());
        CHECK(saved.Value().status == Assets::AssetSourcePublicationStatus::Durable);
        CHECK_FALSE(fixture.document->Snapshot().dirty);
        fixture.Rename("Local");
        fixture.project.WriteSource("External");
        auto rejected = fixture.document->Save(fixture.document->Snapshot().fence, fixture.project.mutations, fixture.project.files);
        CHECK(rejected.HasError());
        CHECK(fixture.document->Snapshot().dirty);
        CHECK(fixture.document->Snapshot().externalConflict);
        CHECK(fixture.document->Snapshot().canUndo);
        CHECK(fixture.document->Snapshot().source->Data().objects.front().name == "Local");
        CHECK(fixture.document->Reload(fixture.document->Snapshot().fence, false).HasError());
        REQUIRE(fixture.document->Reload(fixture.document->Snapshot().fence, true).HasValue());
        CHECK(fixture.document->Snapshot().source->Data().objects.front().name == "External");
        CHECK_FALSE(fixture.document->Snapshot().dirty);
        CHECK_FALSE(fixture.document->Snapshot().canUndo);
    }

    TEST_CASE("Prefab recovery is separate durable intent and cannot overwrite an external source", "[editor][prefab][session]") {
        Fixture fixture;
        const auto original = fixture.document->Snapshot();
        fixture.Rename("Recovered intent");
        REQUIRE(fixture.document->Autosave(fixture.project.mutations, fixture.project.files).HasValue());
        auto captured = fixture.document->Snapshot();
        CHECK(captured.dirty);
        CHECK(captured.acceptedSource == original.acceptedSource);
        CHECK(captured.recoveryRevision == captured.fence.revision);
        DocumentRecoveryService recovery(fixture.project.directory.path, fixture.project.mutations, fixture.project.files);
        auto record = recovery.Inspect(fixture.project.asset, original.identity.key);
        REQUIRE(record.HasValue());
        REQUIRE(record.Value().has_value());
        fixture.document.reset();
        auto reopened = PrefabDocumentSession::Open(fixture.project.directory.path, fixture.project.source, fixture.project.asset,
                                                    original.identity, PrefabAsset(10).Asset(), fixture.limits);
        REQUIRE(reopened.HasValue());
        fixture.document = std::move(reopened).Value();
        SECTION("unchanged source") {
            REQUIRE(fixture.document
                        ->Recover(fixture.document->Snapshot().fence, *record.Value(), fixture.project.mutations, fixture.project.files)
                        .HasValue());
            CHECK(fixture.document->Snapshot().source->Data().objects.front().name == "Recovered intent");
            CHECK(fixture.document->Snapshot().dirty);
            REQUIRE(fixture.document->DiscardOwnedRecovery(fixture.project.mutations, fixture.project.files).HasValue());
            CHECK_FALSE(recovery.Inspect(fixture.project.asset, original.identity.key).Value().has_value());
        }
        SECTION("external source replacement") {
            fixture.project.WriteSource("External");
            CHECK(fixture.document
                      ->Recover(fixture.document->Snapshot().fence, *record.Value(), fixture.project.mutations, fixture.project.files)
                      .HasError());
            CHECK_FALSE(fixture.document->Snapshot().dirty);
        }
        CHECK(Assets::CaptureAssetSource(fixture.project.directory.path, fixture.project.source,
                                         Prefab::PrefabHardLimits::SourceDocumentBytes)
                  .Value()
                  .identity != Assets::AssetSourceIdentity{});
    }

    TEST_CASE("Explicit prefab recovery discard preserves edits and refuses a replaced candidate", "[editor][prefab][session]") {
        Fixture fixture;
        const auto original = fixture.document->Snapshot();
        fixture.Rename("Old recovered intent");
        REQUIRE(fixture.document->Autosave(fixture.project.mutations, fixture.project.files).HasValue());
        DocumentRecoveryService recovery(fixture.project.directory.path, fixture.project.mutations, fixture.project.files);
        auto inspected = recovery.Inspect(fixture.project.asset, original.identity.key);
        REQUIRE(inspected.HasValue());
        REQUIRE(inspected.Value().has_value());
        const auto oldRecord = *inspected.Value();
        fixture.document.reset();
        auto reopened = PrefabDocumentSession::Open(fixture.project.directory.path, fixture.project.source, fixture.project.asset,
                                                    original.identity, PrefabAsset(10).Asset(), fixture.limits);
        REQUIRE(reopened.HasValue());
        fixture.document = std::move(reopened).Value();
        fixture.Rename("Current local intent");
        const auto edited = fixture.document->Snapshot();
        REQUIRE(fixture.document->Autosave(fixture.project.mutations, fixture.project.files).HasError());
        REQUIRE(fixture.document->DiscardRecoveryCandidate(edited.fence, oldRecord, fixture.project.mutations, fixture.project.files)
                    .HasValue());
        const auto discarded = fixture.document->Snapshot();
        CHECK(discarded.source == edited.source);
        CHECK(discarded.fence.revision == edited.fence.revision);
        CHECK(discarded.acceptedSource == edited.acceptedSource);
        CHECK(discarded.savedState == edited.savedState);
        CHECK(discarded.dirty);
        CHECK(discarded.canUndo);
        REQUIRE(fixture.document->Autosave(fixture.project.mutations, fixture.project.files).HasValue());
        auto current = recovery.Inspect(fixture.project.asset, original.identity.key);
        REQUIRE(current.HasValue());
        REQUIRE(current.Value().has_value());
        CHECK(current.Value()->sessionOwner != oldRecord.sessionOwner);
        CHECK(fixture.document->DiscardRecoveryCandidate(edited.fence, oldRecord, fixture.project.mutations, fixture.project.files)
                  .HasError());
        auto retained = recovery.Inspect(fixture.project.asset, original.identity.key);
        REQUIRE(retained.HasValue());
        CHECK(retained.Value() == current.Value());
        CHECK(fixture.document->Snapshot().recoveryRevision == edited.fence.revision);
        auto canonical = Assets::CaptureAssetSource(fixture.project.directory.path, fixture.project.source,
                                                    Prefab::PrefabHardLimits::SourceDocumentBytes);
        REQUIRE(canonical.HasValue());
        CHECK(canonical.Value().identity == original.acceptedSource);
        REQUIRE(fixture.document->Replay(edited.fence, false).HasValue());
        CHECK_FALSE(fixture.document->Snapshot().dirty);
    }

    TEST_CASE("Prefab reload validates a complete replacement before retiring source and runtime", "[editor][prefab][session]") {
        Fixture fixture;
        auto sources = fixture.Capture();
        REQUIRE(fixture.document->ActivateStage(fixture.project.registry.Snapshot(), sources.sources.resolver, {71}).HasValue());
        const auto original = fixture.document->Snapshot();
        std::ofstream(fixture.project.source, std::ios::trunc) << "{malformed";
        CHECK(fixture.document->Reload(original.fence, true).HasError());
        CHECK(fixture.document->Stage() != nullptr);
        CHECK(fixture.document->Snapshot().source == original.source);
        CHECK(fixture.document->Snapshot().fence.revision == original.fence.revision);
    }

    TEST_CASE("Prefab dirty close is explicit and old source fences cannot enter closed sessions", "[editor][prefab][session]") {
        Fixture fixture;
        fixture.Rename("Unsaved");
        const auto before = fixture.document->Snapshot();
        REQUIRE(fixture.document->Close(before.fence, false).HasError());
        CHECK(fixture.document->Snapshot().canUndo);
        REQUIRE(fixture.document->Close(before.fence, true).HasValue());
        CHECK(fixture.document->Execute(before.fence, RenamePrefabObject{{0}, "Closed"}).HasError());
        CHECK(before.source->Data().objects.front().name == "Unsaved");
    }
}  // namespace Horo::Editor
