#include "Horo/Editor/SourceFileOpenService.h"
#include "PrefabHierarchyTestSupport.h"
#include "editor/screens/workspace/prefab/PrefabDocumentSession.h"

#include <thread>

namespace Horo::Editor {
    using namespace PrefabHierarchyTestSupport;

    namespace {
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
            REQUIRE(fixture.document->Autosave(fixture.project.mutations, fixture.project.files).HasValue());
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
