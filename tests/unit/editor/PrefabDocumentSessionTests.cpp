#include "../../support/NativePublicationFiles.h"
#include "Horo/Editor/SourceDocumentService.h"
#include "Horo/Editor/SourceFileOpenService.h"
#include "Horo/Foundation/JobSystem.h"
#include "PrefabDocumentSessionTestSupport.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace Horo::Editor {
    using namespace PrefabDocumentSessionTestSupport;

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

        /** @brief Exercises reentrant owner admission while native source publication is inside the busy guard. */
        struct ReentrantFiles final : Horo::TestSupport::NativePublicationFiles {
            std::function<void()> duringWrite;

            Result<void> WritePrivateDurable(const std::filesystem::path &path, std::span<const std::byte> bytes, bool &created,
                                             std::filesystem::perms permissions) override {
                if (duringWrite)
                    duringWrite();
                return native.WritePrivateDurable(path, bytes, created, permissions);
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
        INFO("save code: " << (saved.HasError() ? saved.ErrorValue().code.Value() : std::string_view{}));
        INFO("save detail: " << (saved.HasError() ? saved.ErrorValue().message : std::string{}));
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

    TEST_CASE("Prefab pending stage retires at edit reload and close fences before deferred completion",
              "[editor][prefab][session][qualification]") {
        Fixture fixture;
        const auto initial = fixture.document->Snapshot();
        auto sources = fixture.Capture();
        auto probe = std::make_shared<StageProbe>();
        EditorPlaySceneFactory factory = [probe](Runtime::SceneRuntimeId id,
                                                 const Runtime::RuntimeSceneDefinition &) -> Result<std::unique_ptr<EditorPlaySceneHost>> {
            return Result<std::unique_ptr<EditorPlaySceneHost>>::Success(std::make_unique<StageHost>(id, probe));
        };
        REQUIRE(fixture.document->ActivateStage(fixture.project.registry.Snapshot(), sources.sources.resolver, {91}, factory).HasValue());
        REQUIRE_FALSE(fixture.document->AdvanceStage().Value());
        REQUIRE(fixture.document->HasStage());
        bool closed{};
        SECTION("local edit retires the pending source revision") {
            fixture.Rename("Local intent");
            CHECK(fixture.document->Snapshot().dirty);
            CHECK(fixture.document->Snapshot().source->Data().objects.front().name == "Local intent");
        }
        SECTION("external reload replaces the pending source revision") {
            fixture.project.WriteSource("External replacement");
            REQUIRE(fixture.document->Reload(initial.fence, false).HasValue());
            CHECK(fixture.document->Snapshot().source->Data().objects.front().name == "External replacement");
            CHECK_FALSE(fixture.document->Snapshot().dirty);
        }
        SECTION("close revokes the pending session") {
            REQUIRE(fixture.document->Close(initial.fence, false).HasValue());
            closed = true;
            CHECK(fixture.document->Execute(initial.fence, RenamePrefabObject{{0}, "Late edit"}).HasError());
        }
        CHECK(probe->shutdowns == 1);
        CHECK_FALSE(fixture.document->HasStage());
        probe->hold = false;
        const auto advanced = fixture.document->AdvanceStage();
        CHECK(advanced.HasError() == closed);
        if (advanced.HasValue())
            CHECK_FALSE(advanced.Value());
        CHECK_FALSE(fixture.document->StageView().has_value());
        CHECK(fixture.document->Snapshot().previewRevision == initial.previewRevision);
        CHECK(probe->fixedUpdates == 0);
        CHECK(probe->shutdowns == 1);
    }

    TEST_CASE("Prefab save publishes canonical source and external conflicts retain local history", "[editor][prefab][session]") {
        Fixture fixture;
        fixture.Rename("Saved");
        auto saved = fixture.document->Save(fixture.document->Snapshot().fence, fixture.project.mutations, fixture.project.files);
        INFO("save code: " << (saved.HasError() ? saved.ErrorValue().code.Value() : std::string_view{}));
        INFO("save detail: " << (saved.HasError() ? saved.ErrorValue().message : std::string{}));
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

    TEST_CASE("Prefab sessions preserve the losing edit when another workspace publishes the same source",
              "[editor][prefab][session][qualification][conflict]") {
        Fixture fixture;
        DocumentIdentityRegistry otherIdentities;
        const auto identity = otherIdentities.Open(fixture.document->Snapshot().identity.key);
        REQUIRE(identity.HasValue());
        auto other = PrefabDocumentSession::Open(fixture.project.directory.path, fixture.project.source, fixture.project.asset,
                                                 identity.Value().identity, PrefabAsset(10).Asset(), fixture.limits);
        REQUIRE(other.HasValue());
        fixture.Rename("First workspace intent");
        REQUIRE(other.Value()->Execute(other.Value()->Snapshot().fence, RenamePrefabObject{{0}, "Second workspace intent"}).HasValue());
        auto *winner = fixture.document.get();
        auto *loser = other.Value().get();
        SECTION("first workspace publishes first") {}
        SECTION("second workspace publishes first") {
            std::swap(winner, loser);
        }
        const auto pending = loser->Snapshot();
        const auto published = winner->Save(winner->Snapshot().fence, fixture.project.mutations, fixture.project.files);
        REQUIRE(published.HasValue());
        REQUIRE(published.Value().status == Assets::AssetSourcePublicationStatus::Durable);
        const auto rejected = loser->Save(pending.fence, fixture.project.mutations, fixture.project.files);
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == Assets::AssetSourceErrors::Conflict.code.Value());
        const auto retained = loser->Snapshot();
        CHECK(retained.externalConflict);
        CHECK(retained.dirty);
        CHECK(retained.canUndo);
        CHECK(retained.fence.revision == pending.fence.revision);
        CHECK(retained.sourceRevision == pending.sourceRevision);
        CHECK(retained.acceptedSource == pending.acceptedSource);
        CHECK(retained.source == pending.source);
        const auto native = Assets::CaptureAssetSource(fixture.project.directory.path, fixture.project.source,
                                                       Prefab::PrefabHardLimits::SourceDocumentBytes);
        REQUIRE(native.HasValue());
        CHECK(native.Value().identity == published.Value().identity);
        CHECK(loser->Reload(retained.fence, false).HasError());
        REQUIRE(loser->Reload(retained.fence, true).HasValue());
        CHECK(loser->Snapshot().sourceRevision == winner->Snapshot().sourceRevision);
        CHECK(loser->Snapshot().fence.instance == pending.fence.instance);
        CHECK_FALSE(loser->Snapshot().dirty);
        const auto stale = loser->Execute(pending.fence, RenamePrefabObject{{0}, "Stale completion"});
        REQUIRE(stale.HasError());
        CHECK(stale.ErrorValue().code.Value() == SourceDocumentErrors::Stale.code.Value());
    }

    TEST_CASE("Prefab recovery is separate durable intent and cannot overwrite an external source", "[editor][prefab][session]") {
        Fixture fixture;
        const auto original = fixture.document->Snapshot();
        fixture.Rename("Recovered intent");
        fixture.Autosave();
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

    TEST_CASE("Prefab comparison keeps the exact local draft without accepting an external save base", "[editor][prefab][session]") {
        Fixture fixture;
        fixture.Rename("Draft");
        const auto before = fixture.document->Snapshot();
        const auto firstWatch = fixture.document->CaptureExternalInspection();
        REQUIRE(ComparePrefabExternal(firstWatch).HasValue());
        CHECK_FALSE(ComparePrefabExternal(firstWatch).Value());
        fixture.project.WriteSource("Disk version");
        const auto observed = ComparePrefabExternal(firstWatch);
        REQUIRE(observed.HasValue());
        REQUIRE(observed.Value());
        REQUIRE(observed.Value()->members.size() == 1);
        CHECK(observed.Value()->members.front().draftName == "Draft");
        CHECK(observed.Value()->members.front().diskName == "Disk version");

        fixture.Rename("Current draft");
        CHECK_FALSE(fixture.document->ObserveExternal(firstWatch, observed.Value()));
        const auto current = fixture.document->Snapshot();
        const auto currentWatch = fixture.document->CaptureExternalInspection();
        const auto comparison = ComparePrefabExternal(currentWatch);
        REQUIRE(comparison.HasValue());
        REQUIRE(comparison.Value());
        REQUIRE(fixture.document->ObserveExternal(currentWatch, comparison.Value()));
        CHECK(fixture.document->KeepExternalDraft(current.fence, *observed.Value()).HasError());
        REQUIRE(fixture.document->KeepExternalDraft(current.fence, *comparison.Value()).HasValue());
        const auto kept = fixture.document->Snapshot();
        CHECK(kept.source == current.source);
        CHECK(kept.savedState == before.savedState);
        CHECK(kept.acceptedSource == before.acceptedSource);
        CHECK(kept.keepExternalDraft);
        CHECK(kept.externalConflict);
        CHECK(kept.dirty);
        CHECK(kept.canUndo);
        CHECK(fixture.document->Save(kept.fence, fixture.project.mutations, fixture.project.files).HasError());
        const auto persisted = Assets::CaptureAssetSource(fixture.project.directory.path, fixture.project.source,
                                                          Prefab::PrefabHardLimits::SourceDocumentBytes);
        REQUIRE(persisted.HasValue());
        CHECK(persisted.Value().identity == comparison.Value()->observed);
        fixture.project.WriteSource("Unseen disk revision");
        CHECK(fixture.document->Reload(kept.fence, true, comparison.Value()->observed).HasError());
        CHECK(fixture.document->Snapshot().source == kept.source);
        fixture.project.WriteSource("Disk version");
        REQUIRE(fixture.document->Reload(kept.fence, true, comparison.Value()->observed).HasValue());
        const auto reloaded = fixture.document->Snapshot();
        CHECK_FALSE(reloaded.externalConflict);
        CHECK_FALSE(reloaded.keepExternalDraft);
        CHECK_FALSE(reloaded.externalComparison);
        CHECK(reloaded.source->Data().objects.front().name == "Disk version");
    }

    TEST_CASE("Malformed external prefab comparison retains the local draft and reports disk rejection", "[editor][prefab][session]") {
        Fixture fixture;
        fixture.Rename("Retained draft");
        const auto snapshot = fixture.document->Snapshot();
        std::ofstream(fixture.project.source, std::ios::trunc) << "{malformed";
        const auto inspection = fixture.document->CaptureExternalInspection();
        const auto compared = ComparePrefabExternal(inspection);
        REQUIRE(compared.HasValue());
        REQUIRE(compared.Value());
        CHECK(compared.Value()->diskError.has_value());
        REQUIRE(fixture.document->ObserveExternal(inspection, compared.Value()));
        CHECK(fixture.document->Snapshot().source == snapshot.source);
        CHECK(fixture.document->Snapshot().dirty);
        CHECK(fixture.document->Reload(snapshot.fence, true).HasError());
        CHECK(fixture.document->Snapshot().externalConflict);
    }

    TEST_CASE("Detached prefab recovery checkpoints retain the captured state while the owner continues editing", "[editor][prefab][session]") {
        Fixture fixture;
        fixture.Rename("Scheduled intent");
        const auto original = fixture.document->Snapshot();
        const auto checkpoint = fixture.document->CaptureRecovery();
        REQUIRE(checkpoint.HasValue());
        REQUIRE(checkpoint.Value().has_value());
        DocumentRecoveryService recovery(fixture.project.directory.path, fixture.project.mutations, fixture.project.files);
        CHECK_FALSE(recovery.Inspect(fixture.project.asset, original.identity.key).Value().has_value());

        CancellationSource cancelled;
        cancelled.RequestCancellation();
        const auto cancelledWrite = WritePrefabRecovery(*checkpoint.Value(), recovery, cancelled.Token());
        REQUIRE(cancelledWrite.HasError());
        CHECK(IsJobCancelled(cancelledWrite.ErrorValue()));
        CHECK_FALSE(recovery.Inspect(fixture.project.asset, original.identity.key).Value().has_value());

        fixture.Rename("Later intent");
        JobSystem jobs{JobSystemConfig{.workerCount = 1, .ioWorkerCount = 1}};
        auto submitted = jobs.SubmitResult({.resource = JobResource::Io},
                                          [&checkpoint, &recovery](const CancellationToken &token) {
            const auto written = WritePrefabRecovery(*checkpoint.Value(), recovery, token);
            return written.HasError() ? Result<void>::Failure(written.ErrorValue()) : Result<void>::Success();
        });
        REQUIRE(submitted.HasValue());
        REQUIRE(submitted.Value().Wait().HasValue());
        fixture.document->AcknowledgeRecovery(*checkpoint.Value());
        const auto newer = fixture.document->Snapshot();
        CHECK(newer.source->Data().objects.front().name == "Later intent");
        CHECK(newer.recoveryRevision == original.fence.revision);
        CHECK(newer.dirty);
        const auto persisted = recovery.Inspect(fixture.project.asset, original.identity.key);
        REQUIRE(persisted.HasValue());
        REQUIRE(persisted.Value().has_value());
        CHECK(persisted.Value()->revision == original.fence.revision);
        CHECK(persisted.Value()->acceptedSource == original.acceptedSource);
        fixture.Autosave();
        fixture.document->AcknowledgeRecovery(*checkpoint.Value());
        CHECK(fixture.document->Snapshot().recoveryRevision == newer.fence.revision);
        CHECK(Assets::CaptureAssetSource(fixture.project.directory.path, fixture.project.source,
                                         Prefab::PrefabHardLimits::SourceDocumentBytes)
                  .Value()
                  .identity == original.acceptedSource);
    }

    TEST_CASE("Explicit prefab recovery discard preserves edits and refuses a replaced candidate", "[editor][prefab][session]") {
        Fixture fixture;
        const auto original = fixture.document->Snapshot();
        fixture.Rename("Old recovered intent");
        fixture.Autosave();
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
        fixture.Autosave();
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

    TEST_CASE("Prefab source opening rejects foreign identities malformed bytes and missing native source", "[editor][prefab][session]") {
        Fixture fixture;
        const auto original = fixture.document->Snapshot();
        const auto open = [&](DocumentIdentity identity, Assets::AssetId asset) {
            return PrefabDocumentSession::Open(fixture.project.directory.path, fixture.project.source, asset, std::move(identity),
                                               original.fence.owner, fixture.limits);
        };
        SECTION("invalid shared identity") {
            CHECK(open({}, fixture.project.asset).HasError());
        }
        SECTION("foreign asset") {
            CHECK(open(original.identity, PrefabAsset(44).Asset()).HasError());
        }
        SECTION("wrong source kind") {
            auto identity = original.identity;
            identity.key.kind = DocumentKind::Source;
            CHECK(open(identity, fixture.project.asset).HasError());
        }
        SECTION("malformed source") {
            std::ofstream(fixture.project.source, std::ios::trunc) << "{broken";
            CHECK(open(original.identity, fixture.project.asset).HasError());
            REQUIRE(fixture.document->InspectExternal().HasValue());
            CHECK(fixture.document->Snapshot().externalConflict);
            CHECK(fixture.document->Execute(original.fence, RenamePrefabObject{{0}, "Rejected"}).HasError());
            CHECK(fixture.document->Replay(original.fence, false).HasError());
        }
        SECTION("missing source") {
            REQUIRE(std::filesystem::remove(fixture.project.source));
            CHECK(open(original.identity, fixture.project.asset).HasError());
            REQUIRE(fixture.document->InspectExternal().HasValue());
            CHECK(fixture.document->Snapshot().externalConflict);
        }
        CHECK(fixture.document->Snapshot().source == original.source);
        CHECK_FALSE(fixture.document->Snapshot().dirty);
    }

    TEST_CASE("Prefab publication rejects reentrant source commands and releases admission after save", "[editor][prefab][session]") {
        Fixture fixture;
        fixture.Rename("Published once");
        const auto fence = fixture.document->Snapshot().fence;
        ReentrantFiles files;
        ProjectMutationCoordinator mutations{files};
        std::size_t rejected{};
        files.duringWrite = [&] {
            const auto result = fixture.document->Execute(fence, RenamePrefabObject{{0}, "Reentrant"});
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == SourceDocumentErrors::Busy.code.Value());
            const auto reloaded = fixture.document->Reload(fence, true);
            REQUIRE(reloaded.HasError());
            CHECK(reloaded.ErrorValue().code.Value() == SourceDocumentErrors::Busy.code.Value());
            const auto closed = fixture.document->Close(fence, true);
            REQUIRE(closed.HasError());
            CHECK(closed.ErrorValue().code.Value() == SourceDocumentErrors::Busy.code.Value());
            const auto savedAgain = fixture.document->Save(fence, mutations, files);
            REQUIRE(savedAgain.HasError());
            CHECK(savedAgain.ErrorValue().code.Value() == SourceDocumentErrors::Busy.code.Value());
            ++rejected;
        };
        auto saved = fixture.document->Save(fence, mutations, files);
        REQUIRE(saved.HasValue());
        CHECK(rejected == 1);
        CHECK_FALSE(fixture.document->Snapshot().dirty);
        CHECK(fixture.document->Snapshot().source->Data().objects.front().name == "Published once");
        fixture.Rename("Allowed after publication");
        CHECK(fixture.document->Snapshot().dirty);
    }

}  // namespace Horo::Editor
