#include "Horo/Editor/EditorSurfaceIdentity.h"

#include <catch2/catch_test_macros.hpp>
#include <string_view>

namespace {
    using namespace Horo::Editor;

    TEST_CASE("Surface type identifiers accept canonical namespaced values", "[unit][editor][surface]") {
        const auto viewport = SurfaceTypeId::Parse("horo.viewport");
        REQUIRE(viewport.HasValue());
        REQUIRE(viewport.Value().IsValid());
        REQUIRE(viewport.Value().Value() == "horo.viewport");

        for (const std::string_view invalid : {"", ".horo.viewport", "horo..viewport", "Horo.viewport", "horo viewport", "horo."}) {
            REQUIRE(SurfaceTypeId::Parse(invalid).HasError());
        }
    }

    TEST_CASE("Source document identities are project relative and canonical", "[unit][editor][surface]") {
        const auto source = SourceDocumentId::Parse("assets/scripts/player.horo_script");
        REQUIRE(source.HasValue());
        REQUIRE(source.Value().IsValid());
        REQUIRE(source.Value().Value() == "assets/scripts/player.horo_script");

        for (const std::string_view invalid :
             {"", "/tmp/player.horo_script", "../outside.horo_script", "assets/./player.horo_script", "assets\\\\player.horo_script"}) {
            REQUIRE(SourceDocumentId::Parse(invalid).HasError());
        }
    }

    TEST_CASE("Document kinds have stable serialized names", "[unit][editor][surface]") {
        for (const DocumentKind kind : {DocumentKind::Scene, DocumentKind::Source, DocumentKind::Shader, DocumentKind::Asset,
                                        DocumentKind::Project, DocumentKind::Custom}) {
            const std::string_view serialized = ToString(kind);
            REQUIRE(!serialized.empty());
            const auto parsed = ParseDocumentKind(serialized);
            REQUIRE(parsed.HasValue());
            REQUIRE(parsed.Value() == kind);
        }
        REQUIRE(ParseDocumentKind("unknown").HasError());
        REQUIRE(ToString(DocumentKind::None).empty());
    }

    TEST_CASE("Built-in viewport and game surfaces preserve lifecycle semantics", "[unit][editor][surface]") {
        const auto viewport = SurfaceDescriptor::MakeViewport();
        REQUIRE(viewport.HasValue());
        REQUIRE(viewport.Value().type.Value() == "horo.viewport");
        REQUIRE(viewport.Value().capabilities.Has(SurfaceCapability::Pinned));
        REQUIRE(viewport.Value().capabilities.Has(SurfaceCapability::Restorable));
        REQUIRE(!viewport.Value().capabilities.Has(SurfaceCapability::Closable));

        const auto game = SurfaceDescriptor::MakeGame();
        REQUIRE(game.HasValue());
        REQUIRE(game.Value().type.Value() == "horo.game");
        REQUIRE(game.Value().capabilities.Has(SurfaceCapability::Conditional));
        REQUIRE(game.Value().capabilities.Has(SurfaceCapability::Closable));
        REQUIRE(game.Value().capabilities.Has(SurfaceCapability::Restorable));
    }

    TEST_CASE("Opening the same source focuses its existing document instance", "[unit][editor][surface]") {
        const auto sourceA = SourceDocumentId::Parse("assets/scripts/a.horo_script");
        const auto sourceB = SourceDocumentId::Parse("assets/scripts/b.horo_script");
        REQUIRE(sourceA.HasValue());
        REQUIRE(sourceB.HasValue());

        DocumentIdentityRegistry registry;
        const DocumentOpenKey keyA{DocumentKind::Source, sourceA.Value()};
        const DocumentOpenKey keyB{DocumentKind::Source, sourceB.Value()};

        const auto first = registry.Open(keyA);
        REQUIRE(first.HasValue());
        REQUIRE(first.Value().disposition == DocumentOpenDisposition::Opened);
        REQUIRE(first.Value().identity.IsValid());

        const auto focused = registry.Open(keyA);
        REQUIRE(focused.HasValue());
        REQUIRE(focused.Value().disposition == DocumentOpenDisposition::FocusExisting);
        REQUIRE(focused.Value().identity == first.Value().identity);
        REQUIRE(registry.Size() == 1);

        const auto second = registry.Open(keyB);
        REQUIRE(second.HasValue());
        REQUIRE(second.Value().disposition == DocumentOpenDisposition::Opened);
        REQUIRE(second.Value().identity.instance != first.Value().identity.instance);
        REQUIRE(second.Value().identity.key.kind == DocumentKind::Source);
        REQUIRE(registry.Size() == 2);
    }

    TEST_CASE("Document open keys survive restore without serializing session instances", "[unit][editor][surface]") {
        const auto source = SourceDocumentId::Parse("assets/scenes/main.horo");
        REQUIRE(source.HasValue());
        const DocumentOpenKey key{DocumentKind::Scene, source.Value()};

        const auto serialized = SerializeDocumentOpenKey(key);
        REQUIRE(serialized.HasValue());
        REQUIRE(serialized.Value().kind == "scene");
        REQUIRE(serialized.Value().source == "assets/scenes/main.horo");

        const auto restored = DeserializeDocumentOpenKey(serialized.Value());
        REQUIRE(restored.HasValue());
        REQUIRE(restored.Value() == key);

        DocumentIdentityRegistry registry;
        const auto first = registry.Open(restored.Value());
        REQUIRE(first.HasValue());
        REQUIRE(registry.Close(first.Value().identity.instance).HasValue());

        const auto reopened = registry.Open(restored.Value());
        REQUIRE(reopened.HasValue());
        REQUIRE(reopened.Value().disposition == DocumentOpenDisposition::Opened);
        REQUIRE(reopened.Value().identity.instance != first.Value().identity.instance);
    }

    TEST_CASE("Closing an unknown or stale document instance is rejected", "[unit][editor][surface]") {
        const auto source = SourceDocumentId::Parse("assets/scenes/main.horo");
        REQUIRE(source.HasValue());
        DocumentIdentityRegistry registry;
        const auto opened = registry.Open({DocumentKind::Scene, source.Value()});
        REQUIRE(opened.HasValue());

        const auto unknown = DocumentInstanceId::Create(9999);
        REQUIRE(unknown.HasValue());
        const auto closeUnknown = registry.Close(unknown.Value());
        REQUIRE(closeUnknown.HasError());
        REQUIRE(closeUnknown.ErrorValue().code.Value() == "editor.surface_identity.instance_unknown");

        REQUIRE(registry.Close(opened.Value().identity.instance).HasValue());
        const auto closeStale = registry.Close(opened.Value().identity.instance);
        REQUIRE(closeStale.HasError());
        REQUIRE(closeStale.ErrorValue().code.Value() == "editor.surface_identity.instance_unknown");
    }
}  // namespace
