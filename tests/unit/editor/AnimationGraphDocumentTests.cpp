#include "../runtime/animation/AnimationGraphTestFixtures.h"
#include "AllocationProbe.h"
#include "Horo/Editor/AnimationGraphDocument.h"

#include <limits>
#include <thread>

namespace Horo::Editor {
    namespace {
        using namespace Animation;
        using Animation::Test::Graph::Blended;
        using Animation::Test::Id;

        DocumentIdentity Identity() {
            auto source = SourceDocumentId::Parse("animations/locomotion.animgraph");
            REQUIRE(source.HasValue());
            return {{DocumentKind::Asset, source.Value()}, Id<DocumentInstanceId>(1)};
        }

        std::unique_ptr<AnimationGraphDocument> Document(const AnimationGraphDocumentLimits &limits = {}) {
            auto opened = AnimationGraphDocument::Open(Identity(), Blended(), limits);
            REQUIRE(opened.HasValue());
            return std::move(opened).Value();
        }

        AnimationGraphDocumentSnapshot Capture(const AnimationGraphDocument &document) {
            auto captured = document.Snapshot();
            REQUIRE(captured.HasValue());
            return std::move(captured).Value();
        }

        PutAnimationGraphParameter Weight(const float value) {
            return {{Id<GraphParameterId>(1), "weight", GraphValueType::Float, value}};
        }

        Result<void> Edit(AnimationGraphDocument &document, const float weight) {
            const std::array<AnimationGraphDocumentCommand, 1> commands{Weight(weight)};
            return document.Execute(Capture(document).Revision(), commands);
        }

        template <typename T> void ErrorIs(const Result<T> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == expected.code.Value());
        }

        void Unchanged(const AnimationGraphDocument &document, const AnimationGraphDocumentSnapshot &before) {
            const auto after = Capture(document);
            CHECK(after.Data() == before.Data());
            CHECK(after.SourceBytes() == before.SourceBytes());
            CHECK(after.Revision() == before.Revision());
            CHECK(after.State() == before.State());
        }
    }  // namespace

    TEST_CASE("Animation graph documents capture canonical source without runtime dependency lookup", "[animation][editor][document]") {
        auto document = Document();
        const auto original = Capture(*document);
        const auto decoded = DeserializeAnimationGraphSource(original.SourceBytes(), original.Data().id);
        REQUIRE(decoded.HasValue());
        CHECK(decoded.Value() == original.Data());
        CHECK_FALSE(document->IsDirty());
        CHECK_FALSE(document->CanUndo());
        CHECK_FALSE(document->CanRedo());
        REQUIRE(Edit(*document, 0.75F).HasValue());
        CHECK(std::get<float>(original.Data().parameters.front().defaultValue) == 0.5F);
        REQUIRE(document->Close(AnimationDocumentDiscardPolicy::DiscardChanges).HasValue());
        document.reset();
        CHECK(DeserializeAnimationGraphSource(original.SourceBytes(), original.Data().id).HasValue());
    }

    TEST_CASE("Animation graph semantic history restores stable content and saved state", "[animation][editor][document]") {
        auto document = Document();
        const auto original = Capture(*document);
        REQUIRE(Edit(*document, 0.75F).HasValue());
        const auto edited = Capture(*document);
        CHECK(document->IsDirty());
        CHECK(edited.Data().definitions == original.Data().definitions);
        REQUIRE(document->MarkSaved(edited).HasValue());
        CHECK_FALSE(document->IsDirty());
        REQUIRE(document->Undo(edited.Revision()).HasValue());
        CHECK(Capture(*document).SourceBytes() == original.SourceBytes());
        CHECK(Capture(*document).State() == original.State());
        CHECK(document->IsDirty());
        REQUIRE(document->Redo(Capture(*document).Revision()).HasValue());
        CHECK(Capture(*document).SourceBytes() == edited.SourceBytes());
        CHECK(Capture(*document).State() == edited.State());
        CHECK(Capture(*document).Revision() > edited.Revision());
        CHECK_FALSE(document->IsDirty());
    }

    TEST_CASE("Animation graph final transaction validates references and entry replacement together", "[animation][editor][document]") {
        auto document = Document();
        const auto original = Capture(*document);
        auto replacement = original.Data().definitions.front();
        replacement.id = Id<GraphDefinitionId>(2);
        const std::array<AnimationGraphDocumentCommand, 3> commands{RemoveAnimationGraphDefinition{original.Data().entry},
                                                                    SetAnimationGraphEntry{replacement.id},
                                                                    PutAnimationGraphDefinition{replacement}};
        REQUIRE(document->Execute(original.Revision(), commands).HasValue());
        const auto replaced = Capture(*document);
        CHECK(replaced.Data().entry == replacement.id);
        CHECK(replaced.Data().definitions.size() == 1);
        REQUIRE(document->Undo(replaced.Revision()).HasValue());
        CHECK(Capture(*document).SourceBytes() == original.SourceBytes());
        REQUIRE(document->Redo(Capture(*document).Revision()).HasValue());
        CHECK(Capture(*document).SourceBytes() == replaced.SourceBytes());
    }

    TEST_CASE("Animation graph dirty status compares canonical saved content rather than edit count", "[animation][editor][document]") {
        auto document = Document();
        const auto saved = Capture(*document);
        REQUIRE(Edit(*document, 0.75F).HasValue());
        CHECK(document->IsDirty());
        REQUIRE(Edit(*document, 0.5F).HasValue());
        CHECK_FALSE(document->IsDirty());
        CHECK(Capture(*document).State() != saved.State());
        CHECK(document->CanUndo());
        REQUIRE(document->Undo(Capture(*document).Revision()).HasValue());
        CHECK(document->IsDirty());
        REQUIRE(document->Redo(Capture(*document).Revision()).HasValue());
        CHECK_FALSE(document->IsDirty());
    }

    TEST_CASE("Animation graph invalid and no-op commands preserve redo and publication", "[animation][editor][document]") {
        auto document = Document();
        REQUIRE(Edit(*document, 0.75F).HasValue());
        REQUIRE(document->Undo(Capture(*document).Revision()).HasValue());
        const auto original = Capture(*document);
        const std::array<AnimationGraphDocumentCommand, 2> invalid{Weight(0.25F), RemoveAnimationGraphParameter{Id<GraphParameterId>(1)}};
        CHECK(document->Execute(original.Revision(), invalid).HasError());
        CHECK(Edit(*document, std::numeric_limits<float>::quiet_NaN()).HasError());
        const std::array<AnimationGraphDocumentCommand, 1> missing{RemoveAnimationGraphDefinition{Id<GraphDefinitionId>(999)}};
        ErrorIs(document->Execute(original.Revision(), missing), AnimationDocumentErrors::InvalidCommand);
        REQUIRE(Edit(*document, 0.5F).HasValue());
        Unchanged(*document, original);
        CHECK(document->CanRedo());
        REQUIRE(Edit(*document, 0.25F).HasValue());
        CHECK_FALSE(document->CanRedo());
    }

    TEST_CASE("Animation graph stale and cancelled operations preserve current intent", "[animation][editor][document]") {
        auto document = Document();
        const auto old = Capture(*document);
        REQUIRE(Edit(*document, 0.75F).HasValue());
        const auto before = Capture(*document);
        const std::array<AnimationGraphDocumentCommand, 1> commands{Weight(0.25F)};
        ErrorIs(document->Execute(old.Revision(), commands), AnimationDocumentErrors::StaleRevision);
        ErrorIs(document->Undo(old.Revision()), AnimationDocumentErrors::StaleRevision);
        ErrorIs(document->Redo(old.Revision()), AnimationDocumentErrors::StaleRevision);
        ErrorIs(document->Reload(old.Revision(), old.Data()), AnimationDocumentErrors::StaleRevision);
        ErrorIs(document->ApplyRecovery(old.Revision(), old.SourceBytes()), AnimationDocumentErrors::StaleRevision);
        CancellationSource cancelled;
        cancelled.RequestCancellation();
        ErrorIs(document->Execute(before.Revision(), commands, cancelled.Token()), AnimationErrors::GraphOperationCancelled);
        ErrorIs(document->Undo(before.Revision(), cancelled.Token()), AnimationErrors::GraphOperationCancelled);
        ErrorIs(document->Redo(before.Revision(), cancelled.Token()), AnimationErrors::GraphOperationCancelled);
        ErrorIs(document->Reload(before.Revision(), old.Data(), AnimationDocumentDiscardPolicy::DiscardChanges, cancelled.Token()),
                AnimationErrors::GraphOperationCancelled);
        ErrorIs(document->ApplyRecovery(before.Revision(), old.SourceBytes(), cancelled.Token()), AnimationErrors::GraphOperationCancelled);
        Unchanged(*document, before);
        CHECK(document->CanUndo());
    }

    TEST_CASE("Animation graph late save acknowledges only its captured semantic state and session", "[animation][editor][document]") {
        auto document = Document();
        const auto original = Capture(*document);
        REQUIRE(Edit(*document, 0.75F).HasValue());
        const auto persisted = Capture(*document);
        REQUIRE(Edit(*document, 0.25F).HasValue());
        REQUIRE(document->MarkSaved(persisted).HasValue());
        CHECK(document->IsDirty());
        REQUIRE(document->Undo(Capture(*document).Revision()).HasValue());
        CHECK_FALSE(document->IsDirty());
        auto other = Document();  // Even a duplicated host identity cannot acknowledge another owner's save.
        ErrorIs(document->MarkSaved(Capture(*other)), AnimationDocumentErrors::StaleRevision);
        CHECK_FALSE(document->IsDirty());
        REQUIRE(document->MarkSaved(original).HasValue());
        CHECK(document->IsDirty());
    }

    TEST_CASE("Animation graph external reload preserves dirty intent until explicit discard", "[animation][editor][document]") {
        auto document = Document();
        const auto original = Capture(*document);
        REQUIRE(Edit(*document, 0.75F).HasValue());
        const auto edited = Capture(*document);
        ErrorIs(document->Reload(edited.Revision(), original.Data()), AnimationDocumentErrors::DirtyDocument);
        auto malformed = original.Data();
        malformed.schemaVersion = 999;
        CHECK(document->Reload(edited.Revision(), malformed, AnimationDocumentDiscardPolicy::DiscardChanges).HasError());
        ErrorIs(document->Reload(edited.Revision(), original.Data(), static_cast<AnimationDocumentDiscardPolicy>(255)),
                AnimationDocumentErrors::InvalidCommand);
        auto foreign = original.Data();
        foreign.skeleton = Animation::Test::Asset<SkeletonId>(99);
        ErrorIs(document->Reload(edited.Revision(), foreign, AnimationDocumentDiscardPolicy::DiscardChanges),
                AnimationDocumentErrors::InvalidCommand);
        Unchanged(*document, edited);
        CHECK(document->CanUndo());
        REQUIRE(document->Reload(edited.Revision(), original.Data(), AnimationDocumentDiscardPolicy::DiscardChanges).HasValue());
        CHECK_FALSE(document->IsDirty());
        CHECK_FALSE(document->CanUndo());
        CHECK_FALSE(document->CanRedo());
        ErrorIs(document->MarkSaved(edited), AnimationDocumentErrors::StaleRevision);
        ErrorIs(document->MarkSaved(original), AnimationDocumentErrors::StaleRevision);
        CHECK(Capture(*document).SourceBytes() == original.SourceBytes());
    }

    TEST_CASE("Animation graph recovery uses the source codec and is one reversible edit", "[animation][editor][document]") {
        auto source = Document();
        REQUIRE(Edit(*source, 0.75F).HasValue());
        const auto recovered = Capture(*source);
        auto document = Document();
        const auto before = Capture(*document);
        CHECK(document->ApplyRecovery(before.Revision(), "{invalid").HasError());
        auto foreign = recovered.Data();
        foreign.skeleton = Animation::Test::Asset<SkeletonId>(99);
        auto foreignBytes = SerializeAnimationGraphSource(foreign);
        REQUIRE(foreignBytes.HasValue());
        CHECK(document->ApplyRecovery(before.Revision(), foreignBytes.Value()).HasError());
        Unchanged(*document, before);
        REQUIRE(document->ApplyRecovery(before.Revision(), recovered.SourceBytes()).HasValue());
        CHECK(document->IsDirty());
        CHECK(Capture(*document).SourceBytes() == recovered.SourceBytes());
        REQUIRE(document->Undo(Capture(*document).Revision()).HasValue());
        CHECK_FALSE(document->IsDirty());
        CHECK(Capture(*document).SourceBytes() == before.SourceBytes());
        REQUIRE(document->Redo(Capture(*document).Revision()).HasValue());
        CHECK(document->IsDirty());
    }

    TEST_CASE("Animation graph history retention and project limits remain bounded", "[animation][editor][document]") {
        SECTION("oversized transaction is rejected without losing source") {
            AnimationGraphDocumentLimits limits;
            limits.historyBytes = 1;
            auto document = Document(limits);
            const auto before = Capture(*document);
            ErrorIs(Edit(*document, 0.75F), AnimationDocumentErrors::HistoryLimit);
            Unchanged(*document, before);
        }
        SECTION("oldest history evicts only after a successful transaction") {
            AnimationGraphDocumentLimits limits;
            limits.historyEntries = 1;
            auto document = Document(limits);
            REQUIRE(Edit(*document, 0.75F).HasValue());
            const auto retained = Capture(*document);
            REQUIRE(Edit(*document, 0.25F).HasValue());
            REQUIRE(document->Undo(Capture(*document).Revision()).HasValue());
            CHECK(Capture(*document).SourceBytes() == retained.SourceBytes());
            ErrorIs(document->Undo(Capture(*document).Revision()), AnimationDocumentErrors::HistoryUnavailable);
        }
        SECTION("command counts and identity are checked before staging") {
            auto document = Document();
            const auto before = Capture(*document);
            ErrorIs(document->Execute(before.Revision(), {}), AnimationDocumentErrors::InvalidCommand);
            const std::vector<AnimationGraphDocumentCommand> excess(65, Weight(0.25F));
            ErrorIs(document->Execute(before.Revision(), excess), AnimationDocumentErrors::InvalidCommand);
            auto identity = Identity();
            identity.key.kind = DocumentKind::Source;
            CHECK(AnimationGraphDocument::Open(identity, Blended()).HasError());
            AnimationGraphDocumentLimits invalid;
            invalid.historyEntries = 65;
            CHECK(AnimationGraphDocument::Open(Identity(), Blended(), invalid).HasError());
            Unchanged(*document, before);
        }
    }

    TEST_CASE("Animation graph document allocation failures preserve semantic history and roots", "[animation][editor][document]") {
        bool completed = false;
        std::size_t failures{};
        for (std::size_t allocation = 0; allocation < 2048 && !completed; ++allocation) {
            auto document = Document();
            REQUIRE(Edit(*document, 0.75F).HasValue());
            REQUIRE(document->Undo(Capture(*document).Revision()).HasValue());
            const auto before = Capture(*document);
            const std::array<AnimationGraphDocumentCommand, 1> commands{Weight(0.25F)};
            try {
                Tests::AllocationProbe::ScopedFailure failure{allocation};
                completed = document->Execute(before.Revision(), commands).HasValue();
            } catch (const std::bad_alloc &) {
                ++failures;
            }
            if (!completed) {
                Unchanged(*document, before);
                CHECK_FALSE(document->IsDirty());
                CHECK(document->CanRedo());
            }
        }
        CHECK(completed);
        CHECK(failures > 0);
    }

    TEST_CASE("Animation graph command budgets reject nested payloads before copying them", "[animation][editor][document]") {
        auto document = Document();
        const auto before = Capture(*document);
        auto oversized = before.Data().definitions.front();
        oversized.nodes.front().pins.resize(AnimationGraphHardLimits::Pins + 1);
        const auto payloadBytes = oversized.nodes.front().pins.size() * sizeof(GraphPin);
        const std::array<AnimationGraphDocumentCommand, 1> commands{PutAnimationGraphDefinition{std::move(oversized)}};
        Tests::AllocationProbe::Measurement allocations;
        std::optional<Result<void>> result;
        {
            Tests::AllocationProbe::ScopedMeasurement measurement;
            result = document->Execute(before.Revision(), commands);
            allocations = measurement.Snapshot();
        }
        REQUIRE(result.has_value());
        ErrorIs(*result, AnimationErrors::GraphLimitExceeded);
        CHECK(allocations.largestRequest < payloadBytes);
        Unchanged(*document, before);

        AnimationGraphDocumentLimits limits;
        limits.graph.nodes = static_cast<std::uint32_t>(before.Data().definitions.front().nodes.size());
        auto bounded = Document(limits);
        auto second = before.Data().definitions.front();
        second.id = Id<GraphDefinitionId>(2);
        const std::array<AnimationGraphDocumentCommand, 1> additional{PutAnimationGraphDefinition{std::move(second)}};
        ErrorIs(bounded->Execute(Capture(*bounded).Revision(), additional), AnimationErrors::GraphLimitExceeded);
        CHECK_FALSE(bounded->CanUndo());
    }

    TEST_CASE("Animation graph recovery allocation failures preserve source and history", "[animation][editor][document]") {
        auto source = Document();
        REQUIRE(Edit(*source, 0.75F).HasValue());
        const auto recovery = Capture(*source);
        bool completed = false;
        std::size_t failures{};
        for (std::size_t allocation = 0; allocation < 8192 && !completed; ++allocation) {
            auto document = Document();
            const auto before = Capture(*document);
            try {
                Tests::AllocationProbe::ScopedFailure failure{allocation};
                completed = document->ApplyRecovery(before.Revision(), recovery.SourceBytes()).HasValue();
            } catch (const std::bad_alloc &) {
                ++failures;
            }
            if (!completed) {
                Unchanged(*document, before);
                CHECK_FALSE(document->IsDirty());
                CHECK_FALSE(document->CanUndo());
            }
        }
        CHECK(completed);
        CHECK(failures > 0);
    }

    TEST_CASE("Animation graph source migration remains dirty until a durable save acknowledgement", "[animation][editor][document]") {
        auto legacy = Animation::Test::Graph::Simple();
        legacy.schemaVersion = 1;
        legacy.contractVersion = {1, 0, 0};
        for (auto &connection : legacy.definitions.front().connections)
            connection.type = GraphValueType::Unspecified;
        auto opened = AnimationGraphDocument::Open(Identity(), legacy);
        REQUIRE(opened.HasValue());
        auto document = std::move(opened).Value();
        CHECK(document->IsDirty());
        const auto migrated = Capture(*document);
        CHECK(migrated.Data().schemaVersion == CurrentAnimationGraphSchemaVersion);
        CHECK(legacy.schemaVersion == 1);
        REQUIRE(document->MarkSaved(migrated).HasValue());
        CHECK_FALSE(document->IsDirty());
        REQUIRE(document->Reload(migrated.Revision(), legacy).HasValue());
        CHECK(document->IsDirty());
        ErrorIs(document->MarkSaved(migrated), AnimationDocumentErrors::StaleRevision);
    }

    TEST_CASE("Animation graph document rejects wrong-thread operations and closes without revoking captures",
              "[animation][editor][document]") {
        auto document = Document();
        const auto before = Capture(*document);
        bool rejected = false;
        std::thread worker{[&] {
            const auto snapshot = document->Snapshot();
            const auto close = document->Close();
            rejected = snapshot.HasError() && snapshot.ErrorValue().code.Value() == AnimationDocumentErrors::WrongThread.code.Value() &&
                       close.HasError() && close.ErrorValue().code.Value() == AnimationDocumentErrors::WrongThread.code.Value();
        }};
        worker.join();
        CHECK(rejected);
        Unchanged(*document, before);
        REQUIRE(Edit(*document, 0.75F).HasValue());
        ErrorIs(document->Close(), AnimationDocumentErrors::DirtyDocument);
        REQUIRE(document->Close(AnimationDocumentDiscardPolicy::DiscardChanges).HasValue());
        REQUIRE(document->Close().HasValue());
        ErrorIs(document->Snapshot(), AnimationDocumentErrors::Closed);
        ErrorIs(document->MarkSaved(before), AnimationDocumentErrors::Closed);
        CHECK_FALSE(document->IsDirty());
        CHECK_FALSE(document->CanUndo());
        CHECK_FALSE(document->CanRedo());
        CHECK(DeserializeAnimationGraphSource(before.SourceBytes(), before.Data().id).HasValue());
    }
}  // namespace Horo::Editor
