#include "Horo/Editor/WorkspacePanelHost.h"
#include "PrefabDocumentSessionTestSupport.h"

#include <fstream>
#include <ios>
#include <span>
#include <string>

namespace Horo::Editor {
    using namespace PrefabDocumentSessionTestSupport;

    namespace {
        /** @brief Checks parser diagnostics and recovery admission before canonical source replacement. */
        void CheckRecoveryAdmission(Fixture &fixture, const PrefabDocumentSnapshot &before, const std::string &text,
                                    const Result<Prefab::PrefabDocument> &expected) {
            DocumentRecoveryRecord record{};
            record.asset = fixture.project.asset;
            record.document = before.identity.key;
            record.acceptedSource = before.acceptedSource;
            const auto bytes = std::as_bytes(std::span(text));
            record.payload.assign(bytes.begin(), bytes.end());
            const auto recovered = fixture.document->Recover(before.fence, record, fixture.project.mutations, fixture.project.files);
            REQUIRE(recovered.HasError());
            if (text.empty()) {
                CHECK(recovered.ErrorValue().code.Value() == Assets::AssetSourceErrors::Conflict.code.Value());
            } else {
                CHECK(recovered.ErrorValue().code.Value() == expected.ErrorValue().code.Value());
                CHECK(recovered.ErrorValue().message == expected.ErrorValue().message);
            }
            record.asset = PrefabAsset(44).Asset();
            const auto foreign = fixture.document->Recover(before.fence, record, fixture.project.mutations, fixture.project.files);
            REQUIRE(foreign.HasError());
            CHECK(foreign.ErrorValue().code.Value() == Assets::AssetSourceErrors::Conflict.code.Value());
            record.asset = fixture.project.asset;
            fixture.Rename("Local intent");
            const auto dirty =
                fixture.document->Recover(fixture.document->Snapshot().fence, record, fixture.project.mutations, fixture.project.files);
            REQUIRE(dirty.HasError());
            CHECK(dirty.ErrorValue().code.Value() == WorkspaceDocumentErrors::DirtyDocument.code.Value());
            REQUIRE(fixture.document->Replay(fixture.document->Snapshot().fence, false).HasValue());
        }
    }  // namespace

    TEST_CASE("Prefab byte text boundaries preserve parser diagnostics and recovery admission", "[editor][prefab][session]") {
        Fixture fixture;
        const auto before = fixture.document->Snapshot();
        std::string text;
        SECTION("empty") {
            text.clear();
        }
        SECTION("embedded NUL") {
            text = "{\"name\":\"ab\"}";
            text.insert(text.find('b'), 1, '\0');
        }
        SECTION("invalid high byte") {
            text = "\xff";
        }
        SECTION("oversized before invalid UTF8") {
            text.assign(Prefab::PrefabHardLimits::SourceDocumentBytes + 1, '\xff');
        }
        const auto expected =
            Prefab::PrefabDocument::Parse(text, fixture.limits, {.expectedProjectVersion = before.source->Data().projectVersion});
        REQUIRE(expected.HasError());
        CheckRecoveryAdmission(fixture, before, text, expected);
        const auto retained = fixture.document->Snapshot();
        std::ofstream output(fixture.project.source, std::ios::binary | std::ios::trunc);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.close();
        REQUIRE(output);
        const auto opened = PrefabDocumentSession::Open(fixture.project.directory.path, fixture.project.source, fixture.project.asset,
                                                        before.identity, before.fence.owner, fixture.limits);
        REQUIRE(opened.HasError());
        const auto reloaded = fixture.document->Reload(fixture.document->Snapshot().fence, false);
        REQUIRE(reloaded.HasError());
        if (text.size() <= Prefab::PrefabHardLimits::SourceDocumentBytes) {
            CHECK(opened.ErrorValue().code.Value() == expected.ErrorValue().code.Value());
            CHECK(opened.ErrorValue().message == expected.ErrorValue().message);
            CHECK(reloaded.ErrorValue().code.Value() == expected.ErrorValue().code.Value());
            CHECK(reloaded.ErrorValue().message == expected.ErrorValue().message);
        }
        CHECK(fixture.document->Snapshot().source == retained.source);
        CHECK(fixture.document->Snapshot().acceptedSource == before.acceptedSource);
        CHECK_FALSE(fixture.document->Snapshot().dirty);
    }

    TEST_CASE("Prefab owned text retains UTF8 through reload and durable recovery", "[editor][prefab][session]") {
        Fixture fixture;
        const std::string name = "Text \xc5\x9f \xf0\x9f\x8c\x8d";
        fixture.Rename(name);
        REQUIRE(fixture.document->Save(fixture.document->Snapshot().fence, fixture.project.mutations, fixture.project.files).HasValue());
        REQUIRE(fixture.document->Reload(fixture.document->Snapshot().fence, false).HasValue());
        CHECK(fixture.document->Snapshot().source->Data().objects.front().name == name);
        const auto reopened = PrefabDocumentSession::Open(fixture.project.directory.path, fixture.project.source, fixture.project.asset,
                                                          fixture.document->Snapshot().identity, PrefabAsset(10).Asset(), fixture.limits);
        REQUIRE(reopened.HasValue());
        CHECK(reopened.Value()->Snapshot().source->Data().objects.front().name == name);
        const auto before = fixture.document->Snapshot();
        fixture.Rename(name + " recovered");
        fixture.Autosave();
        DocumentRecoveryService recovery(fixture.project.directory.path, fixture.project.mutations, fixture.project.files);
        const auto inspected = recovery.Inspect(fixture.project.asset, before.identity.key);
        REQUIRE(inspected.HasValue());
        REQUIRE(inspected.Value().has_value());
        REQUIRE(fixture.document->Replay(fixture.document->Snapshot().fence, false).HasValue());
        REQUIRE(fixture.document
                    ->Recover(fixture.document->Snapshot().fence, *inspected.Value(), fixture.project.mutations, fixture.project.files)
                    .HasValue());
        CHECK(fixture.document->Snapshot().source->Data().objects.front().name == name + " recovered");
        CHECK(fixture.document->Snapshot().acceptedSource == before.acceptedSource);
        CHECK(fixture.document->Snapshot().dirty);
    }

}  // namespace Horo::Editor
