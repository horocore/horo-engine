#include "FractureDocumentFixture.h"

namespace Horo::Editor {
    using namespace FractureTest;
    using namespace Destruction;

    TEST_CASE("Fracture open validates source lifetime and explicit history envelopes", "[unit][editor][fracture][open]") {
        ErrorIs(FractureAssetDocument::Open(Source(), {}, Id<FractureDocumentSession>(12)), FractureDocumentErrors::LimitExceeded);
        ErrorIs(FractureAssetDocument::Open(Source(), Id<FractureSourceRevision>(11), {}), FractureDocumentErrors::LimitExceeded);
        ErrorIs(FractureAssetDocument::Open(Source(), Id<FractureSourceRevision>(11), Id<FractureDocumentSession>(12), {0, 1024}),
                FractureDocumentErrors::LimitExceeded);
        ErrorIs(FractureAssetDocument::Open(Source(), Id<FractureSourceRevision>(11), Id<FractureDocumentSession>(12), {129, 1024}),
                FractureDocumentErrors::LimitExceeded);
        ErrorIs(FractureAssetDocument::Open(Source(), Id<FractureSourceRevision>(11), Id<FractureDocumentSession>(12), {128, 0}),
                FractureDocumentErrors::LimitExceeded);
        ErrorIs(FractureAssetDocument::Open(Source(), Id<FractureSourceRevision>(11), Id<FractureDocumentSession>(12),
                                            {128, 16 * MaximumFractureSourceBytes + 1}),
                FractureDocumentErrors::LimitExceeded);
        auto invalid = Source();
        invalid.chunks.clear();
        ErrorIs(FractureAssetDocument::Open(std::move(invalid), Id<FractureSourceRevision>(11), Id<FractureDocumentSession>(12)),
                FractureDocumentErrors::InvalidSource);
        auto document = Document();
        const auto before = document.Snapshot();
        CHECK_FALSE(document.IsClosed());
        CHECK(document.Undo(Context(document)).Value() == FractureDocumentChange::Unchanged);
        CHECK(document.Redo(Context(document)).Value() == FractureDocumentChange::Unchanged);
        CHECK(document.Apply({}, Context(document)).Value() == FractureDocumentChange::Unchanged);
        CHECK(document.Snapshot().revision == before.revision);
    }

    TEST_CASE("Fracture keyed overwrites preserve exact inverse values in compound transactions", "[unit][editor][fracture][overwrite]") {
        auto document = Document();
        const auto before = document.Snapshot();
        auto chunk = before.source->chunks.back();
        chunk.required = false;
        auto contact = before.source->contacts.front();
        contact.weight = 3;
        auto material = before.source->materials.front();
        material.digest = Digest(99);
        REQUIRE(document.Apply({{PutFractureChunk{chunk}, PutFractureContact{contact}, PutFractureMaterial{material}}}, Context(document))
                    .HasValue());
        const auto edited = document.Snapshot();
        REQUIRE(document.Undo(Context(document)).HasValue());
        CHECK(*document.Snapshot().source == *before.source);
        REQUIRE(document.Redo(Context(document)).HasValue());
        CHECK(*document.Snapshot().source == *edited.source);
        const auto revision = document.Snapshot().revision;
        CHECK(document
                  .Apply({{RemoveFractureChunk{Id<DestructionChunkId>(999)},
                           RemoveFractureContact{Id<DestructionChunkId>(30), Id<DestructionChunkId>(40)}, RemoveFractureMaterial{99}}},
                         Context(document))
                  .Value() == FractureDocumentChange::Unchanged);
        CHECK(document.Snapshot().revision == revision);
    }

    TEST_CASE("Fracture edits commit one semantic transaction and undo exact source values", "[unit][editor][fracture][history]") {
        auto document = Document();
        const auto original = document.Snapshot();
        auto settings = original.source->settings;
        settings.seed = 0;
        auto damage = original.source->damage;
        damage.health.maximumHealth = 200;
        const FractureSourcePatch patch{{SetFractureSettings{settings}, SetFractureDamage{damage},
                                         PutFractureMaterial{{1, Asset(15), Digest(16)}},
                                         PutFractureChunk{{Id<DestructionChunkId>(30), {}, 1}},
                                         PutFractureContact{{Id<DestructionChunkId>(20), Id<DestructionChunkId>(30), 2.5}}}};
        REQUIRE(document.Apply(patch, Context(document)).HasValue());
        CHECK(document.IsDirty());
        CHECK(document.UndoCount() == 1);
        CHECK(document.Snapshot().revision.Value() == 2);
        CHECK(original.source->chunks.size() == 2);
        const auto edited = document.Snapshot();
        REQUIRE(document.Undo(Context(document)).HasValue());
        CHECK(*document.Snapshot().source == *original.source);
        CHECK(document.Snapshot().state == original.state);
        CHECK(document.Snapshot().revision.Value() == 3);
        CHECK_FALSE(document.IsDirty());
        REQUIRE(document.Redo(Context(document)).HasValue());
        CHECK(*document.Snapshot().source == *edited.source);
        CHECK(document.Snapshot().state == edited.state);
        CHECK(document.IsDirty());
    }

    TEST_CASE("Fracture invalid graph transaction rolls back source history and revision", "[unit][editor][fracture][rollback]") {
        auto document = Document();
        const auto before = document.Snapshot();
        auto settings = before.source->settings;
        settings.seed = 42;
        ErrorIs(document.Apply({{SetFractureSettings{settings}, RemoveFractureChunk{Id<DestructionChunkId>(10)}}}, Context(document)),
                FractureDocumentErrors::InvalidSource);
        CHECK(document.Snapshot().source == before.source);
        CHECK(document.Snapshot().revision == before.revision);
        CHECK(document.UndoCount() == 0);
        CHECK_FALSE(document.IsDirty());
        const FractureSourcePatch removal{{RemoveFractureContact{Id<DestructionChunkId>(10), Id<DestructionChunkId>(20)},
                                           RemoveFractureChunk{Id<DestructionChunkId>(20)}}};
        REQUIRE(document.Apply(removal, Context(document)).HasValue());
        REQUIRE(document.Undo(Context(document)).HasValue());
        CHECK(*document.Snapshot().source == *before.source);
    }

    TEST_CASE("Fracture no-op edits preserve redo while a new edit branches history", "[unit][editor][fracture][noop]") {
        auto document = Document();
        REQUIRE(document.Apply(SeedPatch(document, 42), Context(document)).HasValue());
        REQUIRE(document.Undo(Context(document)).HasValue());
        const auto before = document.Snapshot();
        const auto noChange = document.Apply(SeedPatch(document, 7), Context(document));
        REQUIRE(noChange.HasValue());
        CHECK(noChange.Value() == FractureDocumentChange::Unchanged);
        CHECK(document.RedoCount() == 1);
        CHECK(document.Snapshot().revision == before.revision);
        REQUIRE(document.Apply(SeedPatch(document, 100), Context(document)).HasValue());
        CHECK(document.RedoCount() == 0);
        CHECK(document.UndoCount() == 1);
        REQUIRE(document.Undo(Context(document)).HasValue());
        CHECK_FALSE(document.IsDirty());
    }

    TEST_CASE("Fracture permission revision cancellation and close fence late operations", "[unit][editor][fracture][lifecycle]") {
        auto document = Document();
        const auto original = document.Snapshot();
        auto context = Context(document);
        context.canEdit = false;
        ErrorIs(document.Apply(SeedPatch(document, 42), context), FractureDocumentErrors::AuthorityDenied);
        context = Context(document);
        context.session = Id<FractureDocumentSession>(13);
        ErrorIs(document.Apply(SeedPatch(document, 42), context), FractureDocumentErrors::WrongDocument);
        context = Context(document);
        REQUIRE(document.Apply(SeedPatch(document, 42), context).HasValue());
        ErrorIs(document.Undo(context), FractureDocumentErrors::StaleRevision);
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        context = Context(document);
        context.cancellation = cancellation.Token();
        ErrorIs(document.Undo(context), FractureDocumentErrors::Cancelled);
        const auto save = document.CaptureSave();
        REQUIRE(save.HasValue());
        document.Close();
        document.Close();
        ErrorIs(document.Apply({}, Context(document)), FractureDocumentErrors::Closed);
        ErrorIs(document.Undo(Context(document)), FractureDocumentErrors::Closed);
        ErrorIs(document.Redo(Context(document)), FractureDocumentErrors::Closed);
        ErrorIs(document.CaptureSave(), FractureDocumentErrors::Closed);
        ErrorIs(document.AcknowledgeSave(save.Value(), Id<FractureSourceRevision>(12)), FractureDocumentErrors::Closed);
        CHECK(original.source->settings.seed == 7);
        CHECK(DecodeFractureAssetSource(save.Value().Bytes()).HasValue());
    }

    TEST_CASE("Fracture source save receipt preserves later dirty edits and reopens exact saved source", "[unit][editor][fracture][save]") {
        auto document = Document();
        REQUIRE(document.Apply(SeedPatch(document, 42), Context(document)).HasValue());
        const auto save = document.CaptureSave();
        REQUIRE(save.HasValue());
        CHECK(document.IsDirty());
        CHECK(save.Value().ExpectedSourceRevision() == Id<FractureSourceRevision>(11));
        REQUIRE(document.Apply(SeedPatch(document, 43), Context(document)).HasValue());
        ErrorIs(document.AcknowledgeSave(save.Value(), Id<FractureSourceRevision>(11)), FractureDocumentErrors::PublicationConflict);
        REQUIRE(document.AcknowledgeSave(save.Value(), Id<FractureSourceRevision>(12)).HasValue());
        REQUIRE(document.AcknowledgeSave(save.Value(), Id<FractureSourceRevision>(12)).HasValue());
        CHECK(document.IsDirty());
        REQUIRE(document.Undo(Context(document)).HasValue());
        CHECK_FALSE(document.IsDirty());
        auto decoded = DecodeFractureAssetSource(save.Value().Bytes());
        REQUIRE(decoded.HasValue());
        auto reopened =
            FractureAssetDocument::Open(std::move(decoded.Value()), Id<FractureSourceRevision>(12), Id<FractureDocumentSession>(99));
        REQUIRE(reopened.HasValue());
        CHECK_FALSE(reopened.Value().IsDirty());
        CHECK(reopened.Value().Snapshot().source->settings.seed == 42);
        ErrorIs(reopened.Value().AcknowledgeSave(save.Value(), Id<FractureSourceRevision>(13)), FractureDocumentErrors::WrongDocument);
        auto stale = document.CaptureSave();
        REQUIRE(stale.HasValue());
        auto current = document.CaptureSave();
        REQUIRE(current.HasValue());
        REQUIRE(document.AcknowledgeSave(current.Value(), Id<FractureSourceRevision>(13)).HasValue());
        ErrorIs(document.AcknowledgeSave(stale.Value(), Id<FractureSourceRevision>(14)), FractureDocumentErrors::PublicationConflict);
    }

    TEST_CASE("Fracture history budgets reject before mutation and evict oldest steps deterministically",
              "[unit][editor][fracture][budget]") {
        auto tiny = Document({1, 1});
        const auto original = tiny.Snapshot();
        ErrorIs(tiny.Apply(SeedPatch(tiny, 42), Context(tiny)), FractureDocumentErrors::HistoryBudgetExceeded);
        CHECK(tiny.Snapshot().source == original.source);
        CHECK(tiny.Snapshot().revision == original.revision);
        auto bounded = Document({2, 65536});
        for (std::uint64_t seed : {10, 11, 12})
            REQUIRE(bounded.Apply(SeedPatch(bounded, seed), Context(bounded)).HasValue());
        CHECK(bounded.UndoCount() == 2);
        REQUIRE(bounded.Undo(Context(bounded)).HasValue());
        REQUIRE(bounded.Undo(Context(bounded)).HasValue());
        CHECK(bounded.Snapshot().source->settings.seed == 10);
        CHECK(bounded.IsDirty());
        const auto unchanged = bounded.Undo(Context(bounded));
        REQUIRE(unchanged.HasValue());
        CHECK(unchanged.Value() == FractureDocumentChange::Unchanged);
    }

    TEST_CASE("Fracture accepted source checkpoint freezes mutable caller aliases and restores exact history",
              "[unit][editor][fracture][checkpoint]") {
        auto document = Document();
        const auto before = document.Snapshot();
        auto replacement = std::make_shared<FractureAssetSource>(Source());
        replacement->settings.seed = 42;
        REQUIRE(document.Apply({{ReplaceFractureSource{replacement}}}, Context(document)).HasValue());
        replacement->settings.seed = 100;
        CHECK(document.Snapshot().source->settings.seed == 42);
        REQUIRE(document.Undo(Context(document)).HasValue());
        CHECK(*document.Snapshot().source == *before.source);
        REQUIRE(document.Redo(Context(document)).HasValue());
        CHECK(document.Snapshot().source->settings.seed == 42);
        replacement->asset = FractureAssetId::Create(Asset(20)).Value();
        ErrorIs(document.Apply({{ReplaceFractureSource{replacement}}}, Context(document)), FractureDocumentErrors::WrongDocument);
        ErrorIs(document.Apply({{ReplaceFractureSource{}}}, Context(document)), FractureDocumentErrors::WrongDocument);
    }
}  // namespace Horo::Editor
