#include "Horo/AI/DecisionPlanCodec.h"
#include "Horo/Assets/AssetRegistry.h"
#include "Horo/Editor/BlackboardSchemaDocument.h"
#include "Horo/Editor/ProjectMutation.h"
#include "Horo/Editor/SourceFileOpenService.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <fstream>
#include <limits>
#include <thread>

namespace {
    using namespace Horo;
    using namespace Horo::AI;
    using namespace Horo::Editor;

    BlackboardKeyId Key(const std::uint64_t value) {
        return BlackboardKeyId::Create(value).Value();
    }

    BlackboardSchemaRow BooleanRow(const BlackboardKeyId key, std::string name) {
        return {{.key = key, .defaultValue = BlackboardScalarValue{true}}, std::move(name)};
    }

    BlackboardSchemaSource Source() {
        return {.asset = Assets::AssetId::FromBytes({1}),
                .schema = BlackboardSchemaId::Create(17).Value(),
                .version = 2,
                .rows = {BooleanRow(Key(8), "Has Target"), BooleanRow(Key(3), "Can Move")}};
    }

    DocumentIdentity Document() {
        return {{DocumentKind::Asset, SourceDocumentId::Parse("assets/guard.horo_bb").Value()}, DocumentInstanceId::Create(2).Value()};
    }

    std::unique_ptr<BlackboardSchemaDocument> Open() {
        auto result = BlackboardSchemaDocument::Open(Document(), Source());
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    TEST_CASE("Blackboard display edits preserve stable key IDs and reuse the runtime schema", "[editor][blackboard]") {
        auto document = Open();
        const auto begin = document->Snapshot().Value();
        REQUIRE(begin.source->rows.front().descriptor.key == Key(8));
        REQUIRE(begin.schema->Keys().front().key == Key(3));

        REQUIRE(document->Execute(begin.revision, RenameBlackboardKey{Key(8), "Current Target"}).HasValue());
        const auto renamed = document->Snapshot().Value();
        CHECK(renamed.schema == begin.schema);
        CHECK(renamed.source->rows[0].descriptor.key == Key(8));
        CHECK(renamed.source->rows[0].displayName == "Current Target");

        REQUIRE(document->Execute(renamed.revision, MoveBlackboardKey{Key(8), 1}).HasValue());
        const auto moved = document->Snapshot().Value();
        CHECK(moved.schema == begin.schema);
        CHECK(moved.source->rows[1].descriptor.key == Key(8));
        CHECK(moved.source->asset == begin.source->asset);
        CHECK(moved.schema->Version() == 2);
        REQUIRE(document->Undo(moved.revision).HasValue());
        const auto undone = document->Snapshot().Value();
        CHECK(undone.source->rows[0].descriptor.key == Key(8));
        REQUIRE(document->Redo(undone.revision).HasValue());
        CHECK(document->Snapshot().Value().source->rows[1].displayName == "Current Target");
        CHECK(begin.source->rows[0].displayName == "Has Target");
    }

    TEST_CASE("Blackboard draft rows admit only canonical typed defaults and remain undoable", "[editor][blackboard]") {
        auto document = Open();
        const auto id = document->NextKeyId().Value();
        CHECK(id == Key(9));
        const auto begin = document->Snapshot().Value();
        REQUIRE(document->Execute(begin.revision, AddBlackboardKey{BooleanRow(id, "Local"), 1}).HasValue());
        const auto added = document->Snapshot().Value();
        REQUIRE(added.schema->Keys().size() == 3);
        REQUIRE(added.source->rows[1].descriptor.key == id);

        auto descriptor = added.source->rows[1].descriptor;
        descriptor.kind = BlackboardValueKind::SignedInteger;
        descriptor.access = BlackboardKeyAccess::ReadOnly;
        const auto wrongDefault = document->Execute(added.revision, EditBlackboardKey{descriptor});
        REQUIRE(wrongDefault.HasError());
        CHECK(document->Snapshot().Value().revision == added.revision);
        CHECK(document->Snapshot().Value().source == added.source);
        CHECK(document->Snapshot().Value().schema == added.schema);

        descriptor.defaultValue = BlackboardScalarValue{std::int64_t{7}};
        REQUIRE(document->Execute(added.revision, EditBlackboardKey{descriptor}).HasValue());
        const auto edited = document->Snapshot().Value();
        CHECK(edited.schema->Keys().back() == descriptor);
        REQUIRE(document->Execute(edited.revision, RemoveBlackboardKey{id}).HasValue());
        const auto removed = document->Snapshot().Value();
        CHECK(removed.schema->Keys().size() == 2);
        REQUIRE(document->Undo(removed.revision).HasValue());
        CHECK(document->Snapshot().Value().schema->Keys().back() == descriptor);
    }

    TEST_CASE("Blackboard durable key edits require actual reference resolution", "[editor][blackboard]") {
        auto document = Open();
        const auto begin = document->Snapshot().Value();
        auto changed = begin.source->rows[0].descriptor;
        changed.access = BlackboardKeyAccess::ReadOnly;
        const auto attemptedEdit = document->Execute(begin.revision, EditBlackboardKey{changed});
        const auto attemptedDeletion = document->Execute(begin.revision, RemoveBlackboardKey{changed.key});
        for (const auto *outcome : {&attemptedEdit, &attemptedDeletion}) {
            REQUIRE(outcome->HasError());
            CHECK(outcome->ErrorValue().code.Value() == BlackboardDocumentErrors::ReferenceResolutionRequired.code.Value());
        }
        CHECK(document->Snapshot().Value().source == begin.source);
        CHECK(document->Undo(begin.revision).HasError());
    }

    TEST_CASE("Blackboard reference preview validates a durable edit without publishing", "[editor][blackboard]") {
        auto document = Open();
        const auto begin = document->Snapshot().Value();
        auto changed = begin.source->rows[0].descriptor;
        changed.access = BlackboardKeyAccess::ReadOnly;
        const auto proposed = document->PreviewReferenceEdit(begin.revision, EditBlackboardKey{changed});
        REQUIRE(proposed.HasValue());
        CHECK(proposed.Value().fence.instance == begin.document.instance);
        CHECK(proposed.Value().fence.revision == begin.revision);
        CHECK(proposed.Value().key == changed.key);
        CHECK(proposed.Value().source->rows.front().descriptor == changed);
        CHECK(std::ranges::find(proposed.Value().schema->Keys(), changed) != proposed.Value().schema->Keys().end());

        const auto deletion = document->PreviewReferenceEdit(begin.revision, RemoveBlackboardKey{changed.key});
        REQUIRE(deletion.HasValue());
        CHECK(deletion.Value().source->rows.size() == begin.source->rows.size() - 1);
        CHECK(deletion.Value().schema->Keys().size() == begin.schema->Keys().size() - 1);
        changed.cardinality = BlackboardValueCardinality::Collection;
        changed.maximumCollectionElements = 0;
        CHECK(document->PreviewReferenceEdit(begin.revision, EditBlackboardKey{changed}).HasError());
        CHECK(document->PreviewReferenceEdit(begin.revision + 1, RemoveBlackboardKey{changed.key}).HasError());

        const auto after = document->Snapshot().Value();
        CHECK(after.revision == begin.revision);
        CHECK(after.source == begin.source);
        CHECK(after.schema == begin.schema);
        CHECK_FALSE(after.dirty);
        CHECK_FALSE(after.canUndo);
    }

    TEST_CASE("Blackboard document rejects invalid initial source before publication", "[editor][blackboard]") {
        auto source = Source();
        SECTION("duplicate KeyId with distinct names") {
            source.rows[1].descriptor.key = source.rows[0].descriptor.key;
        }
        SECTION("reserved zero version") {
            source.version = 0;
        }
        SECTION("invalid authored display bytes") {
            source.rows[0].displayName = std::string(1, static_cast<char>(0xff));
        }
        CHECK(BlackboardSchemaDocument::Open(Document(), std::move(source)).HasError());
    }

    TEST_CASE("Blackboard history fences cancellation stale revisions and reissuing an accepted key", "[editor][blackboard]") {
        auto document = Open();
        const auto begin = document->Snapshot().Value();
        const auto key = document->NextKeyId().Value();
        const AddBlackboardKey add{BooleanRow(key, "New Key"), 0};
        CancellationSource cancelled;
        cancelled.RequestCancellation();
        const auto stopped = document->Execute(begin.revision, add, cancelled.Token());
        REQUIRE(stopped.HasError());
        CHECK(stopped.ErrorValue().code.Value() == BlackboardDocumentErrors::Cancelled.code.Value());
        CHECK(document->NextKeyId().Value() == key);
        REQUIRE(document->Execute(begin.revision, add).HasValue());
        CHECK(document->Execute(begin.revision, RemoveBlackboardKey{key}).HasError());
        const auto added = document->Snapshot().Value();
        REQUIRE(document->Undo(added.revision).HasValue());
        const auto undone = document->Snapshot().Value();
        CHECK(document->NextKeyId().Value() == Key(10));
        CHECK(document->Execute(undone.revision, add).HasError());
        REQUIRE(document->Redo(undone.revision).HasValue());
        CHECK(document->Snapshot().Value().source->rows[0].descriptor.key == key);
    }

    TEST_CASE("Registered Blackboard authoring round-trips legacy schema and exact authored order", "[editor][blackboard][source]") {
        const auto source = Source();
        const auto identity = Document();
        auto document = Open();
        const auto semantic = BlackboardSchemaCodec::Encode(*document->Snapshot().Value().schema);
        REQUIRE(semantic.HasValue());

        Assets::AssetRegistry registry;
        const auto path = ProjectPath::Parse(identity.key.source.Value()).Value();
        const auto metadata = ProjectPath::Parse(identity.key.source.Value() + ".horo").Value();
        const auto type = Assets::AssetTypeId::Parse(BlackboardSchemaAssetType).Value();
        REQUIRE(registry.Publish(std::vector<Assets::AssetRecord>{{source.asset, type, path, metadata}}).status ==
                Assets::AssetRegistryBuildStatus::Complete);
        const auto registered = registry.Snapshot();
        auto legacy = BlackboardSchemaDocument::OpenRegistered(identity, registered, source.asset, semantic.Value());
        REQUIRE(legacy.HasValue());
        const auto initial = legacy.Value()->Snapshot().Value();
        REQUIRE(initial.source->rows.size() == 2);
        CHECK(initial.source->rows.front().descriptor.key == Key(3));
        CHECK_FALSE(initial.source->rows.front().displayName);
        REQUIRE(legacy.Value()->Execute(initial.revision, RenameBlackboardKey{Key(8), "Target Exists"}).HasValue());
        const auto named = legacy.Value()->Snapshot().Value();
        REQUIRE(legacy.Value()->Execute(named.revision, MoveBlackboardKey{Key(8), 0}).HasValue());
        const auto final = legacy.Value()->Snapshot().Value();
        const auto encoded = legacy.Value()->EncodeSource(final.revision);
        REQUIRE(encoded.HasValue());
        CHECK(legacy.Value()->EncodeSource(initial.revision).HasError());
        CHECK(BlackboardSchemaCodec::Decode(encoded.Value()).HasError());

        auto reopened = BlackboardSchemaDocument::OpenRegistered(identity, registered, source.asset, encoded.Value());
        REQUIRE(reopened.HasValue());
        const auto roundTrip = reopened.Value()->Snapshot().Value();
        CHECK(roundTrip.source->rows.front().descriptor.key == Key(8));
        CHECK(roundTrip.source->rows.front().displayName == "Target Exists");
        CHECK_FALSE(roundTrip.source->rows.back().displayName);
        CHECK(BlackboardSchemaCodec::Encode(*roundTrip.schema).Value() == semantic.Value());
        CHECK(BlackboardSchemaDocument::OpenRegistered(identity, registered, {}, encoded.Value()).HasError());

        auto different = identity;
        different.key.source = SourceDocumentId::Parse("assets/other.horo_bb").Value();
        CHECK(BlackboardSchemaDocument::OpenRegistered(different, registered, source.asset, encoded.Value()).HasError());
        auto changed = encoded.Value();
        changed.push_back(0);
        CHECK(BlackboardSchemaDocument::OpenRegistered(identity, registered, source.asset, changed).HasError());
    }

    TEST_CASE("Blackboard source metadata rejects duplicate unknown or malformed key presentation", "[editor][blackboard][source]") {
        const auto document = Open();
        const auto schema = document->Snapshot().Value().schema;
        std::array rows{BlackboardSchemaAuthoringRow{Key(8), "Target"}, BlackboardSchemaAuthoringRow{Key(3), "Move"}};
        SECTION("duplicate row identity") {
            rows[1].key = rows[0].key;
        }
        SECTION("foreign row identity") {
            rows[0].key = Key(25);
        }
        SECTION("malformed label") {
            rows[0].displayName = std::string(129, 'x');
        }
        CHECK(BlackboardSchemaSourceCodec::Encode(*schema, rows).HasError());
    }

    /** @brief Actual source route, registry, project writer and no-follow durable filesystem. */
    struct FileFixture final {
        DocumentIdentityRegistry documents;
        Assets::AssetRegistry registry;
        NativeDurableFileSystem files;
        ProjectMutationCoordinator mutations{files};
        std::filesystem::path root;
        std::filesystem::path path;

        FileFixture() {
            const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
            root = std::filesystem::temp_directory_path() / ("horo-blackboard-" + std::to_string(nonce));
            std::filesystem::create_directories(root / "assets");
            root = std::filesystem::canonical(root);
            path = root / "assets/guard.horo_bb";
            auto semantic = Open();
            auto encoded = BlackboardSchemaCodec::Encode(*semantic->Snapshot().Value().schema);
            REQUIRE(encoded.HasValue());
            Write(encoded.Value());
            const auto type = Assets::AssetTypeId::Parse(BlackboardSchemaAssetType).Value();
            REQUIRE(registry
                        .Publish(std::vector<Assets::AssetRecord>{{Source().asset, type, ProjectPath::Parse("assets/guard.horo_bb").Value(),
                                                                   ProjectPath::Parse("assets/guard.horo_bb.horo").Value()}})
                        .status == Assets::AssetRegistryBuildStatus::Complete);
        }

        ~FileFixture() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }

        void Write(std::span<const std::uint8_t> bytes) const {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            REQUIRE(static_cast<bool>(file));
        }
    };

    TEST_CASE("Blackboard file route saves typed edits and fences durable keys on reopen", "[editor][blackboard][source]") {
        FileFixture fixture;
        SourceFileOpenService service{fixture.root, fixture.documents};
        const auto open = service.Open({.path = fixture.path, .origin = SourceOpenOrigin::AssetActivation});
        REQUIRE(open.HasValue());
        REQUIRE(open.Value().document);
        CHECK(open.Value().classification.kind == SourceFileKind::BlackboardSchema);
        CHECK(open.Value().document->identity.key.kind == DocumentKind::BlackboardSchema);
        CHECK(ParseDocumentKind("blackboard_schema").Value() == DocumentKind::BlackboardSchema);
        CHECK_FALSE(open.Value().sourceSnapshot);
        auto candidate = BlackboardSchemaDocument::OpenFile(open.Value().document->identity, fixture.registry.Snapshot(), fixture.root,
                                                            open.Value().location.absolutePath);
        REQUIRE(candidate.HasValue());
        auto &document = *candidate.Value();
        const auto start = document.Snapshot().Value();
        CHECK_FALSE(start.dirty);
        REQUIRE(start.nextKey);
        REQUIRE(document.Execute(start.revision, AddBlackboardKey{BooleanRow(*start.nextKey, "Visible"), 0}).HasValue());
        const auto draft = document.Snapshot().Value();
        CHECK(draft.dirty);
        CHECK(draft.canUndo);
        REQUIRE(document.Save(draft.revision, fixture.registry.Snapshot(), fixture.mutations, fixture.files).HasValue());
        const auto saved = document.Snapshot().Value();
        CHECK_FALSE(saved.dirty);
        CHECK_FALSE(saved.canUndo);
        CHECK(document.Undo(saved.revision).HasError());
        const auto again = service.Open({.path = fixture.path});
        REQUIRE(again.HasValue());
        REQUIRE(again.Value().document);
        CHECK(again.Value().document->disposition == DocumentOpenDisposition::FocusExisting);
        CHECK(again.Value().document->identity == open.Value().document->identity);
        auto reopened = BlackboardSchemaDocument::OpenFile(again.Value().document->identity, fixture.registry.Snapshot(), fixture.root,
                                                           again.Value().location.absolutePath);
        REQUIRE(reopened.HasValue());
        const auto durable = reopened.Value()->Snapshot().Value();
        CHECK(durable.source->rows[0].descriptor.key == *start.nextKey);
        CHECK(durable.source->rows[0].displayName == "Visible");
        CHECK(reopened.Value()->Execute(durable.revision, RemoveBlackboardKey{*start.nextKey}).HasError());
    }

    TEST_CASE("Blackboard source conflict preserves dirty edits until explicit revision-fenced reload", "[editor][blackboard][source]") {
        FileFixture fixture;
        SourceFileOpenService service{fixture.root, fixture.documents};
        const auto opened = service.Open({.path = fixture.path});
        REQUIRE(opened.HasValue());
        REQUIRE(opened.Value().document);
        auto loaded = BlackboardSchemaDocument::OpenFile(opened.Value().document->identity, fixture.registry.Snapshot(), fixture.root,
                                                         opened.Value().location.absolutePath);
        REQUIRE(loaded.HasValue());
        auto &document = *loaded.Value();
        const auto start = document.Snapshot().Value();
        REQUIRE(document.Execute(start.revision, RenameBlackboardKey{Key(8), "Local Target"}).HasValue());
        const auto dirty = document.Snapshot().Value();
        std::array labels{BlackboardSchemaAuthoringRow{Key(8), "External Target"}, BlackboardSchemaAuthoringRow{Key(3), std::nullopt}};
        const auto external = BlackboardSchemaSourceCodec::Encode(*start.schema, labels);
        REQUIRE(external.HasValue());
        fixture.Write(external.Value());

        const auto conflict = document.Save(dirty.revision, fixture.registry.Snapshot(), fixture.mutations, fixture.files);
        REQUIRE(conflict.HasError());
        CHECK(document.Snapshot().Value().conflict);
        CHECK(document.Snapshot().Value().source == dirty.source);
        CHECK(document.Reload(dirty.revision, fixture.registry.Snapshot()).HasError());
        REQUIRE(document.Reload(dirty.revision, fixture.registry.Snapshot(), true).HasValue());
        const auto refreshed = document.Snapshot().Value();
        CHECK_FALSE(refreshed.conflict);
        CHECK_FALSE(refreshed.dirty);
        CHECK_FALSE(refreshed.canUndo);
        CHECK(refreshed.source->rows[0].displayName == "External Target");
        CHECK(refreshed.revision > dirty.revision);
        CHECK(document.Execute(dirty.revision, RenameBlackboardKey{Key(8), "Stale"}).HasError());
    }

    TEST_CASE("Blackboard owner rejects foreign threads and leaves exhausted identities unchanged", "[editor][blackboard]") {
        auto source = Source();
        source.rows[0].descriptor.key = Key(std::numeric_limits<std::uint64_t>::max());
        auto opened = BlackboardSchemaDocument::Open(Document(), std::move(source));
        REQUIRE(opened.HasValue());
        auto document = std::move(opened).Value();
        std::optional<ErrorCode> foreign;
        {
            std::jthread thread{[&] {
                const auto snapshot = document->Snapshot();
                if (snapshot.HasError())
                    foreign = snapshot.ErrorValue().code;
            }};
        }
        REQUIRE(foreign);
        CHECK(foreign->Value() == BlackboardDocumentErrors::WrongThread.code.Value());
        const auto next = document->NextKeyId();
        REQUIRE(next.HasError());
        CHECK(next.ErrorValue().code.Value() == BlackboardDocumentErrors::IdentityExhausted.code.Value());
    }
}  // namespace
