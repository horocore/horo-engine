#include "Horo/Editor/InspectorBehaviorBinding.h"
#include "Horo/Scene/SceneSource.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace Horo::Editor::Tests {
    namespace {
        /** @brief Owns the actual headless Scene/history and two distinct matching attachment instances. */
        struct Fixture final {
            Gameplay::BehaviorTypeId type{Gameplay::BehaviorTypeId::Parse("game.fixture.controller").Value()};
            Gameplay::BehaviorDescriptor descriptor{.typeId = type, .fields = {{"count", std::int64_t{1}}}};
            std::array<InspectorBehaviorTarget, 2> targets{{{{1}, {11}}, {{2}, {12}}}};
            SceneDocumentTransactions owner;

            /** @brief Opens two actual authored objects with independent exact typed values. */
            Fixture() {
                SceneSource::SceneObjectSnapshot first{.id = {1}, .name = "first"};
                first.components.behaviors.push_back(
                    {{11}, type, 1, true, {{"count", std::int64_t{9007199254740993LL}}, {"future", std::string{"preserve"}}}});
                auto second = first;
                second.id = {2};
                second.name = "second";
                second.components.behaviors.front().instanceId = {12};
                second.components.behaviors.front().fields.front().value = std::int64_t{2};
                auto created =
                    SceneDocumentTransactions::Create({Assets::AssetId::FromBytes({1}), 1}, {std::move(first), std::move(second)});
                REQUIRE(created.HasValue());
                owner = std::move(created).Value();
            }

            /** @brief Captures the current canonical owner, with no synthetic document shadow. */
            SceneTransactionSnapshot Snapshot() const {
                auto value = owner.Snapshot();
                REQUIRE(value.HasValue());
                return std::move(value).Value();
            }

            /** @brief Binds explicit owner/session permission to the exact current state. */
            SceneTransactionContext Context() const {
                const auto value = Snapshot();
                return {value.session, value.revision, value.state, true, {}};
            }

            /** @brief Captures the real selected attachments using the registered schema. */
            InspectorBehaviorDraft Capture() const {
                auto value = CaptureInspectorBehavior(Snapshot(), targets, descriptor, 7);
                REQUIRE(value.HasValue());
                return std::move(value).Value();
            }
        };

        /** @brief Serializes the actual complete authoring owner for rollback/roundtrip comparison. */
        std::string Bytes(const SceneTransactionSnapshot &snapshot) {
            return SceneSource::EncodeSceneSource({snapshot.objects, snapshot.prefabInstances});
        }
    }  // namespace

    TEST_CASE("Inspector mixed behavior edit commits one real undo step and preserves unknown fields", "[editor][inspector][behavior]") {
        Fixture f;
        const auto before = f.Snapshot();
        const auto draft = f.Capture();
        REQUIRE(draft.fields.size() == 1);
        CHECK(draft.fields.front().mixed);
        auto changes = PrepareInspectorBehaviorEdit(draft, "count", std::int64_t{INT64_MIN}, 7);
        REQUIRE(changes.HasValue());
        REQUIRE(f.owner.Execute(f.Context(), changes.Value()).HasValue());
        const auto after = f.Snapshot();
        CHECK(after.revision == before.revision + 1);
        for (const auto &object : after.objects) {
            const auto &fields = object.components.behaviors.front().fields;
            CHECK(std::get<std::int64_t>(fields.front().value) == INT64_MIN);
            CHECK(std::get<std::string>(fields.back().value) == "preserve");
        }
        auto decoded = SceneSource::DecodeSceneSource(Bytes(after));
        REQUIRE(decoded.HasValue());
        REQUIRE(f.owner.Replay(f.Context(), false).HasValue());
        CHECK(Bytes(f.Snapshot()) == Bytes(before));
        REQUIRE(f.owner.Replay(f.Context(), true).HasValue());
        CHECK(Bytes(f.Snapshot()) == Bytes(after));
    }

    TEST_CASE("Inspector stale selection unknown field and wrong type cannot prepare a document edit", "[editor][inspector][behavior]") {
        Fixture f;
        const auto draft = f.Capture();
        CHECK(PrepareInspectorBehaviorEdit(draft, "count", std::int64_t{3}, 8).HasError());
        CHECK(PrepareInspectorBehaviorEdit(draft, "future", std::string{"overwrite"}, 7).HasError());
        CHECK(PrepareInspectorBehaviorEdit(draft, "count", 3.0, 7).HasError());
        auto duplicate = f.targets;
        duplicate[1] = duplicate[0];
        CHECK(CaptureInspectorBehavior(f.Snapshot(), duplicate, f.descriptor, 7).HasError());
        auto schema = f.descriptor;
        ++schema.schemaVersion;
        CHECK(CaptureInspectorBehavior(f.Snapshot(), f.targets, schema, 7).HasError());
    }

    TEST_CASE("Inspector second stale candidate preserves the entire current world and history", "[editor][inspector][rollback]") {
        Fixture f;
        auto changes = PrepareInspectorBehaviorEdit(f.Capture(), "count", std::int64_t{4}, 7);
        REQUIRE(changes.HasValue());
        auto malformed = std::move(changes).Value();
        malformed[1].priorContent = {};
        const auto before = f.Snapshot();
        CHECK(f.owner.Execute(f.Context(), malformed).HasError());
        CHECK(Bytes(f.Snapshot()) == Bytes(before));
        CHECK(f.Snapshot().revision == before.revision);
        CHECK(f.owner.Replay(f.Context(), false).HasError());
    }

    TEST_CASE("Inspector document reload and shutdown revoke captured edit admission", "[editor][inspector][lifecycle]") {
        Fixture f;
        const auto context = f.Context();
        auto changes = PrepareInspectorBehaviorEdit(f.Capture(), "count", std::int64_t{5}, 7);
        REQUIRE(changes.HasValue());
        auto denied = context;
        denied.canEdit = false;
        CHECK(f.owner.Execute(denied, changes.Value()).HasError());
        auto stale = context;
        ++stale.session.generation;
        CHECK(f.owner.Execute(stale, changes.Value()).HasError());
        f.owner.Shutdown();
        CHECK(f.owner.Execute(context, changes.Value()).HasError());
    }
}  // namespace Horo::Editor::Tests
