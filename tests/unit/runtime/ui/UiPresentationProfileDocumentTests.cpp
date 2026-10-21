#include "Horo/Runtime/Ui/UiDocumentSerialization.h"
#include "UiHotReloadTestFixture.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

namespace Horo::Runtime::Ui {
    namespace {
        using ReloadTests::Revision;
        using ReloadTests::Stable;

        UiDocument ProfileDocument(const bool profiles = true) {
            UiDocumentBuilder builder{Stable<UiDocumentId>(1), Revision<UiDocumentRevision>(1)};
            REQUIRE(builder.AddCanvas({Stable<UiCanvasId>(2), Stable<UiElementId>(10)}).HasValue());
            REQUIRE(builder.AddElement({Stable<UiElementId>(10), {}, Assets::AssetTypeId::Parse("core.panel").Value(), {}, {}}).HasValue());
            if (profiles) {
                UiLayoutStyle layout;
                layout.container.orientation = UiLayoutOrientation::Horizontal;
                UiPresentationProfile row{Stable<UiPresentationProfileId>(30), 1};
                row.selector.width.maximum = 640;
                row.selector.aspect.minimum = UiPresentationAspectRatio{1, 2};
                row.overrides.push_back({Stable<UiElementId>(10), layout});
                REQUIRE(
                    builder.AddPresentationProfiles({Stable<UiCanvasId>(2), Revision<UiPresentationProfileRevision>(1), {row}}).HasValue());
            }
            auto built = std::move(builder).Build();
            REQUIRE(built.HasValue());
            return std::move(built).Value();
        }

        std::vector<std::uint8_t> LegacyProfilePayload(const std::uint32_t format) {
            std::vector<std::uint8_t> bytes{'H', 'O', 'R', 'O', 'U', 'I', 'C', 0};
            const auto append = [&](std::uint64_t value, const std::uint32_t count) {
                for (std::uint32_t index = 0; index < count; ++index)
                    bytes.push_back(static_cast<std::uint8_t>(value >> (8 * index)));
            };
            const auto id = [&](const auto &value) {
                bytes.insert(bytes.end(), value.Bytes().begin(), value.Bytes().end());
            };
            append(format, 4);
            append(1, 2);
            append(format == 3 ? 3 : 1, 2);
            id(Stable<UiDocumentId>(1));
            append(1, 8);
            append(1, 4);
            append(1, 4);
            append(0, 4);
            append(0, 4);
            id(Stable<UiCanvasId>(2));
            id(Stable<UiElementId>(10));
            append(0, 1);
            append(1920, 4);
            append(1080, 4);
            append(0, 1);
            if (format >= 2) {
                append(0, 1);
                append(1, 4);
                append(1, 4);
                append(1, 4);
                append(1, 4);
                append(0, 1);
            }
            id(Stable<UiElementId>(10));
            bytes.insert(bytes.end(), 16, 0);
            const std::string type = "core.panel";
            append(type.size(), 4);
            bytes.insert(bytes.end(), type.begin(), type.end());
            append(0, 4);
            append(0, 4);
            if (format >= 3)
                append(0, 1);
            return bytes;
        }
    }  // namespace

    TEST_CASE("Responsive document source and cooked formats roundtrip canonical complete layout alternatives",
              "[runtime_ui][responsive][document]") {
        const auto document = ProfileDocument();
        REQUIRE(document.SchemaVersion() == UiDocumentSchemaVersion{1, 4});
        auto source = SerializeUiDocument(document);
        REQUIRE(source.HasValue());
        REQUIRE(source.Value().find("presentationProfiles") != std::string::npos);
        auto decoded = DeserializeUiDocument(source.Value());
        REQUIRE(decoded.HasValue());
        REQUIRE(decoded.Value().PresentationProfiles().size() == 1);
        REQUIRE(decoded.Value().PresentationProfiles().front() == document.PresentationProfiles().front());
        REQUIRE(SerializeUiDocument(decoded.Value()).Value() == source.Value());
        const auto cooked = CookedUiDocument::Cook(document);
        REQUIRE(cooked.HasValue());
        REQUIRE(cooked.Value().Payload()[8] == 4);
        const auto loaded = CookedUiDocument::Decode(cooked.Value().Payload());
        REQUIRE(loaded.HasValue());
        REQUIRE(loaded.Value().PresentationProfiles().front() == document.PresentationProfiles().front());
        const auto recooked = CookedUiDocument::Cook(decoded.Value());
        REQUIRE(recooked.HasValue());
        REQUIRE(std::ranges::equal(recooked.Value().Payload(), cooked.Value().Payload()));
    }

    TEST_CASE("Legacy cooked formats one through three decode neutral responsive base explicitly",
              "[runtime_ui][responsive][document][legacy]") {
        for (const auto format : std::array<std::uint32_t, 3>{1, 2, 3}) {
            const auto payload = LegacyProfilePayload(format);
            const auto decoded = CookedUiDocument::Decode(payload);
            REQUIRE(decoded.HasValue());
            REQUIRE(decoded.Value().PresentationProfiles().empty());
            REQUIRE(decoded.Value().Canvases().size() == 1);
            REQUIRE(decoded.Value().Elements().size() == 1);
        }
    }

    TEST_CASE("Responsive source rejects foreign canvas targets unsupported capabilities and malformed cooked trailers",
              "[runtime_ui][responsive][document]") {
        auto document = ProfileDocument();
        auto source = SerializeUiDocument(document).Value();
        const auto capability = source.find("\"required\":0");
        REQUIRE(capability != std::string::npos);
        source.replace(capability, 12, "\"required\":128");
        const auto invalid = DeserializeUiDocument(source);
        REQUIRE(invalid.HasError());
        UiDocumentBuilder builder{document.Id(), document.Revision()};
        for (const auto &canvas : document.Canvases())
            REQUIRE(builder.AddCanvas(canvas).HasValue());
        for (const auto &element : document.Elements())
            REQUIRE(builder.AddElement(element).HasValue());
        auto table = document.PresentationProfiles().front();
        table.rows.front().overrides.front().element = Stable<UiElementId>(99);
        REQUIRE(builder.AddPresentationProfiles(table).HasValue());
        REQUIRE(std::move(builder).Build().HasError());
        const auto cooked = CookedUiDocument::Cook(document);
        REQUIRE(cooked.HasValue());
        std::vector<std::uint8_t> truncated(cooked.Value().Payload().begin(), cooked.Value().Payload().end());
        truncated.pop_back();
        REQUIRE(CookedUiDocument::Decode(truncated).HasError());
        auto legacy = LegacyProfilePayload(3);
        legacy.push_back(0);
        REQUIRE(CookedUiDocument::Decode(legacy).HasError());
    }

    TEST_CASE("Responsive source admission rejects old schema duplicate rows and bounded row overflow",
              "[runtime_ui][responsive][document]") {
        const auto document = ProfileDocument();
        const auto table = document.PresentationProfiles().front();
        UiDocumentBuilder legacy{document.Id(), document.Revision(), {1, 3}};
        REQUIRE(legacy.AddPresentationProfiles(table).HasError());
        auto duplicate = table;
        duplicate.rows.push_back(duplicate.rows.front());
        REQUIRE(ValidateUiPresentationProfiles(duplicate.rows).HasError());
        auto oversized = table;
        oversized.rows.resize(MaximumUiPresentationProfiles + 1, table.rows.front());
        REQUIRE(ValidateUiPresentationProfiles(oversized.rows).HasError());
        auto source = SerializeUiDocument(document).Value();
        const auto revision = source.find("\"revision\":1", source.find("presentationProfiles"));
        REQUIRE(revision != std::string::npos);
        source.replace(revision, 12, "\"revision\":0");
        REQUIRE(DeserializeUiDocument(source).HasError());
    }

    TEST_CASE("Profile identity and priority are unique across all authored canvases and decoders",
              "[runtime_ui][responsive][document][identity]") {
        UiDocumentBuilder builder{Stable<UiDocumentId>(1), Revision<UiDocumentRevision>(1)};
        for (const auto index : {0, 1}) {
            const auto canvas = Stable<UiCanvasId>(2 + index);
            const auto element = Stable<UiElementId>(10 + index);
            REQUIRE(builder.AddCanvas({canvas, element}).HasValue());
            REQUIRE(builder.AddElement({element, {}, Assets::AssetTypeId::Parse("core.panel").Value(), {}, {}}).HasValue());
            UiPresentationProfile row{Stable<UiPresentationProfileId>(30 + index), static_cast<std::uint32_t>(1 + index)};
            row.overrides.push_back({element, {}});
            REQUIRE(builder.AddPresentationProfiles({canvas, Revision<UiPresentationProfileRevision>(1), {row}}).HasValue());
        }
        auto built = std::move(builder).Build();
        REQUIRE(built.HasValue());
        const auto &document = built.Value();
        const auto original = nlohmann::json::parse(SerializeUiDocument(document).Value());
        const auto cooked = CookedUiDocument::Cook(document);
        REQUIRE(cooked.HasValue());
        for (const bool duplicateId : {true, false}) {
            auto second = document.PresentationProfiles()[1];
            auto source = original;
            const auto key = duplicateId ? "id" : "priority";
            source["presentationProfiles"][1]["rows"][0][key] = source["presentationProfiles"][0]["rows"][0][key];
            if (duplicateId)
                second.rows.front().id = document.PresentationProfiles().front().rows.front().id;
            else
                second.rows.front().priority = document.PresentationProfiles().front().rows.front().priority;
            UiDocumentBuilder admission{document.Id(), document.Revision()};
            REQUIRE(admission.AddPresentationProfiles(document.PresentationProfiles().front()).HasValue());
            const auto rejected = admission.AddPresentationProfiles(second);
            REQUIRE(rejected.HasError());
            REQUIRE(rejected.ErrorValue().code.Value() == UiErrors::DocumentDuplicateIdentity.code.Value());
            const auto decoded = DeserializeUiDocument(source.dump());
            REQUIRE(decoded.HasError());
            REQUIRE(decoded.ErrorValue().code.Value() == UiErrors::DocumentDuplicateIdentity.code.Value());
            std::string payload(cooked.Value().Payload().begin(), cooked.Value().Payload().end());
            const auto oldText = duplicateId ? original["presentationProfiles"][1]["rows"][0]["id"].dump() : std::string{"\"priority\":2"};
            const auto newText = duplicateId ? original["presentationProfiles"][0]["rows"][0]["id"].dump() : std::string{"\"priority\":1"};
            const auto offset = payload.rfind(oldText);
            REQUIRE(offset != std::string::npos);
            REQUIRE(oldText.size() == newText.size());
            payload.replace(offset, oldText.size(), newText);
            const auto invalidCook = CookedUiDocument::Decode({reinterpret_cast<const std::uint8_t *>(payload.data()), payload.size()});
            REQUIRE(invalidCook.HasError());
            REQUIRE(invalidCook.ErrorValue().code.Value() == UiErrors::DocumentDuplicateIdentity.code.Value());
        }
    }
}  // namespace Horo::Runtime::Ui
