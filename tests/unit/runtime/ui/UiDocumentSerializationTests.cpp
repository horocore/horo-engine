#include "Horo/Runtime/Ui/UiDocumentSerialization.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename Id> Id IdWith(const std::uint8_t marker) {
            SerializedUiId bytes{};
            bytes.back() = marker;
            return Id::Create(bytes).Value();
        }

        UiDocumentRevision Revision(const std::uint64_t value) {
            return UiDocumentRevision::Create(value).Value();
        }

        Assets::AssetId Asset(const std::uint8_t marker) {
            std::array<std::uint8_t, 16> bytes{};
            bytes.back() = marker;
            return Assets::AssetId::FromBytes(bytes);
        }

        Assets::AssetTypeId Type(const std::string_view value) {
            return Assets::AssetTypeId::Parse(value).Value();
        }

        UiLocaleTag Locale(const std::string_view value) {
            return UiLocaleTag::Parse(value).Value();
        }

        UiDocument MakeDocument(const UiDocumentSchemaVersion version = CurrentUiDocumentSchemaVersion) {
            const auto documentId = IdWith<UiDocumentId>(1);
            const auto root = IdWith<UiElementId>(2);
            const auto child = IdWith<UiElementId>(3);
            const auto canvasId = IdWith<UiCanvasId>(4);
            const auto routeId = IdWith<UiRouteId>(5);
            const auto texture = Asset(6);
            const auto textureType = Type("core.texture");
            UiDocumentBuilder builder{documentId, Revision(7), version};
            REQUIRE(builder.AddCanvas({canvasId, root, UiRenderMode::ScreenSpaceOverlay, {1920, 1080}, UiScaleMode::ScaleWithScreenSize})
                        .HasValue());
            REQUIRE(builder.AddElement({root, {}, Type("core.panel"), {{"enabled", true}, {"opacity", 1.0}}, {}}).HasValue());
            UiReference elementReference{.kind = UiReferenceKind::Element, .element = root};
            UiReference assetReference{.kind = UiReferenceKind::Asset, .asset = texture, .expectedAssetType = textureType};
            REQUIRE(builder
                        .AddElement({child,
                                     root,
                                     Type("core.text"),
                                     {{"label", std::string{"Hello UI"}}, {"target", elementReference}},
                                     {assetReference}})
                        .HasValue());
            REQUIRE(builder
                        .AddLocalizedText(UiLocalizedText::Create(UiMessageKey::Create("hud", "item_count").Value(),
                                                                  std::vector<UiLocalizedArgument>{{"count", std::int64_t{2}}},
                                                                  "{count} item(s)")
                                              .Value())
                        .HasValue());
            REQUIRE(
                builder
                    .AddLocalizedAsset(UiLocalizedAssetReference::Create(textureType, UiLocalizedAssetFallbackPolicy::Required,
                                                                         std::vector<UiLocalizedAssetVariant>{{Locale("en-US"), texture}})
                                           .Value())
                    .HasValue());
            REQUIRE(builder.RequireAsset({texture, textureType, true}).HasValue());
            REQUIRE(builder.AddRoute({routeId, UiPresentationBand::Screen, 4, true}).HasValue());
            return std::move(builder).Build().Value();
        }

        Result<UiDocument> UpgradeToCurrent(const UiDocument &source) {
            UiDocumentBuilder builder{source.Id(), source.Revision(), CurrentUiDocumentSchemaVersion};
            for (const auto &canvas : source.Canvases())
                if (auto result = builder.AddCanvas(canvas); result.HasError())
                    return Result<UiDocument>::Failure(result.ErrorValue());
            for (const auto &element : source.Elements())
                if (auto result = builder.AddElement(element); result.HasError())
                    return Result<UiDocument>::Failure(result.ErrorValue());
            for (const auto &text : source.LocalizedTexts())
                if (auto result = builder.AddLocalizedText(text); result.HasError())
                    return Result<UiDocument>::Failure(result.ErrorValue());
            for (const auto &asset : source.LocalizedAssets())
                if (auto result = builder.AddLocalizedAsset(asset); result.HasError())
                    return Result<UiDocument>::Failure(result.ErrorValue());
            for (const auto &dependency : source.Dependencies())
                if (auto result = builder.RequireAsset(dependency); result.HasError())
                    return Result<UiDocument>::Failure(result.ErrorValue());
            for (const auto &route : source.Routes())
                if (auto result = builder.AddRoute(route); result.HasError())
                    return Result<UiDocument>::Failure(result.ErrorValue());
            return std::move(builder).Build();
        }

        TEST_CASE("Runtime UI document serialization round trips typed content canonically", "[runtime_ui][document][serialization]") {
            const auto document = MakeDocument();
            const auto encoded = SerializeUiDocument(document);
            REQUIRE(encoded.HasValue());
            const auto decoded = DeserializeUiDocument(encoded.Value());
            REQUIRE(decoded.HasValue());
            REQUIRE(decoded.Value().SchemaVersion() == document.SchemaVersion());
            REQUIRE(decoded.Value().Id() == document.Id());
            REQUIRE(decoded.Value().Revision() == document.Revision());
            REQUIRE(std::ranges::equal(decoded.Value().Canvases(), document.Canvases()));
            REQUIRE(std::ranges::equal(decoded.Value().Elements(), document.Elements()));
            REQUIRE(decoded.Value().LocalizedTexts().size() == document.LocalizedTexts().size());
            REQUIRE(decoded.Value().LocalizedTexts()[0].Key() == document.LocalizedTexts()[0].Key());
            REQUIRE(decoded.Value().LocalizedTexts()[0].Arguments().size() == document.LocalizedTexts()[0].Arguments().size());
            REQUIRE(decoded.Value().LocalizedTexts()[0].FallbackText() == document.LocalizedTexts()[0].FallbackText());
            REQUIRE(decoded.Value().LocalizedTexts()[0].FailurePolicy() == document.LocalizedTexts()[0].FailurePolicy());
            REQUIRE(decoded.Value().LocalizedAssets().size() == document.LocalizedAssets().size());
            REQUIRE(decoded.Value().LocalizedAssets()[0].ExpectedType() == document.LocalizedAssets()[0].ExpectedType());
            REQUIRE(std::ranges::equal(decoded.Value().LocalizedAssets()[0].Variants(), document.LocalizedAssets()[0].Variants()));
            REQUIRE(std::ranges::equal(decoded.Value().Dependencies(), document.Dependencies()));
            REQUIRE(std::ranges::equal(decoded.Value().Routes(), document.Routes()));
            REQUIRE(SerializeUiDocument(decoded.Value()).Value() == encoded.Value());
        }

        TEST_CASE("Runtime UI document serialization round trips every closed typed value and reference kind",
                  "[runtime_ui][document][serialization][typed]") {
            const auto documentId = IdWith<UiDocumentId>(40);
            const auto canvasId = IdWith<UiCanvasId>(41);
            const auto root = IdWith<UiElementId>(42);
            const auto referencedDocument = IdWith<UiDocumentId>(43);
            const auto routeId = IdWith<UiRouteId>(44);
            const auto asset = Asset(45);
            const auto assetType = Type("core.texture");

            const UiReference elementReference{.kind = UiReferenceKind::Element, .element = root};
            const UiReference canvasReference{.kind = UiReferenceKind::Canvas, .canvas = canvasId};
            const UiReference documentReference{.kind = UiReferenceKind::Document, .document = referencedDocument};
            const UiReference assetReference{.kind = UiReferenceKind::Asset, .asset = asset, .expectedAssetType = assetType};
            std::vector<UiTypedProperty> properties{{"boolean", true},
                                                    {"integer", std::int64_t{-7}},
                                                    {"number", -2.5},
                                                    {"text", std::string{"typed"}},
                                                    {"reference", elementReference}};

            UiDocumentBuilder builder{documentId, Revision(8)};
            REQUIRE(builder.AddCanvas({canvasId, root, UiRenderMode::ScreenSpaceCamera, {800, 600}, UiScaleMode::ConstantPixelSize})
                        .HasValue());
            REQUIRE(builder
                        .AddElement({root,
                                     {},
                                     Type("core.panel"),
                                     std::move(properties),
                                     {elementReference, canvasReference, documentReference, assetReference}})
                        .HasValue());
            REQUIRE(builder.RequireAsset({asset, assetType, true}).HasValue());

            const auto localizedText = UiLocalizedText::Create(UiMessageKey::Create("game", "typed_values").Value(),
                                                               std::vector<UiLocalizedArgument>{{"integer", std::int64_t{-7}},
                                                                                                {"number", 2.5},
                                                                                                {"boolean", true},
                                                                                                {"date", UiLocalizedDateTime{-10}},
                                                                                                {"duration", UiLocalizedDuration{25}},
                                                                                                {"enum", UiLocalizedStableEnum{3, 4}},
                                                                                                {"text", std::string{"display"}},
                                                                                                {"shortcut", UiLocalizedShortcut{2, 65}}},
                                                               {}, UiLocalizedTextFailurePolicy::UseSafePlaceholder);
            REQUIRE(localizedText.HasValue());
            REQUIRE(builder.AddLocalizedText(localizedText.Value()).HasValue());

            const auto neutralAsset =
                UiLocalizedAssetReference::Create(assetType, UiLocalizedAssetFallbackPolicy::UseNeutral,
                                                  std::vector<UiLocalizedAssetVariant>{{Locale("en-US"), Asset(46)}}, Asset(47));
            REQUIRE(neutralAsset.HasValue());
            REQUIRE(builder.AddLocalizedAsset(neutralAsset.Value()).HasValue());
            const auto omittedAsset = UiLocalizedAssetReference::Create(assetType, UiLocalizedAssetFallbackPolicy::Omit,
                                                                        std::vector<UiLocalizedAssetVariant>{{Locale("de-DE"), Asset(48)}});
            REQUIRE(omittedAsset.HasValue());
            REQUIRE(builder.AddLocalizedAsset(omittedAsset.Value()).HasValue());
            REQUIRE(builder.AddRoute({routeId, UiPresentationBand::Debug, 17, true}).HasValue());

            const auto document = std::move(builder).Build();
            REQUIRE(document.HasValue());
            const auto encoded = SerializeUiDocument(document.Value());
            REQUIRE(encoded.HasValue());
            const auto decoded = DeserializeUiDocument(encoded.Value());
            REQUIRE(decoded.HasValue());
            REQUIRE(std::ranges::equal(decoded.Value().Elements(), document.Value().Elements()));
            REQUIRE(std::ranges::equal(decoded.Value().Routes(), document.Value().Routes()));
            REQUIRE(decoded.Value().LocalizedTexts().size() == 1);
            REQUIRE(decoded.Value().LocalizedTexts()[0].Arguments().size() == 8);
            REQUIRE(decoded.Value().LocalizedTexts()[0].FailurePolicy() == UiLocalizedTextFailurePolicy::UseSafePlaceholder);
            REQUIRE(std::holds_alternative<UiLocalizedDateTime>(decoded.Value().LocalizedTexts()[0].Arguments()[3].value));
            REQUIRE(std::holds_alternative<UiLocalizedDuration>(decoded.Value().LocalizedTexts()[0].Arguments()[4].value));
            REQUIRE(std::holds_alternative<UiLocalizedStableEnum>(decoded.Value().LocalizedTexts()[0].Arguments()[5].value));
            REQUIRE(std::holds_alternative<UiLocalizedShortcut>(decoded.Value().LocalizedTexts()[0].Arguments()[7].value));
            REQUIRE(decoded.Value().LocalizedAssets().size() == 2);
            REQUIRE(decoded.Value().LocalizedAssets()[0].FallbackPolicy() == UiLocalizedAssetFallbackPolicy::UseNeutral);
            REQUIRE(decoded.Value().LocalizedAssets()[0].NeutralAsset().has_value());
            REQUIRE(decoded.Value().LocalizedAssets()[1].FallbackPolicy() == UiLocalizedAssetFallbackPolicy::Omit);
            REQUIRE_FALSE(decoded.Value().LocalizedAssets()[1].NeutralAsset().has_value());
            REQUIRE(SerializeUiDocument(decoded.Value()).Value() == encoded.Value());
        }

        TEST_CASE("Runtime UI localized document serialization rejects invalid policy and typed argument payloads",
                  "[runtime_ui][document][serialization][localization]") {
            const auto encoded = SerializeUiDocument(MakeDocument()).Value();
            const auto replace = [](std::string source, const std::string_view from, const std::string_view to) {
                const auto position = source.find(from);
                if (position == std::string::npos)
                    return std::string{};
                source.replace(position, from.size(), to);
                return source;
            };

            const auto invalidTextPolicy = replace(encoded, "\"failurePolicy\":\"fallback\"", "\"failurePolicy\":\"unknown\"");
            REQUIRE_FALSE(invalidTextPolicy.empty());
            REQUIRE(DeserializeUiDocument(invalidTextPolicy).HasError());

            const auto invalidArgumentType = replace(encoded, "\"type\":\"int\"", "\"type\":\"unknown\"");
            REQUIRE_FALSE(invalidArgumentType.empty());
            REQUIRE(DeserializeUiDocument(invalidArgumentType).HasError());

            const auto invalidAssetPolicy = replace(encoded, "\"fallbackPolicy\":\"required\"", "\"fallbackPolicy\":\"unknown\"");
            REQUIRE_FALSE(invalidAssetPolicy.empty());
            REQUIRE(DeserializeUiDocument(invalidAssetPolicy).HasError());

            const auto invalidLocale = replace(encoded, "\"locale\":\"en-US\"", "\"locale\":\"bad_tag\"");
            REQUIRE_FALSE(invalidLocale.empty());
            REQUIRE(DeserializeUiDocument(invalidLocale).HasError());

            const auto unexpectedNeutral =
                replace(encoded, "\"neutralAsset\":null", "\"neutralAsset\":\"000000000000000000000000000000ff\"");
            REQUIRE_FALSE(unexpectedNeutral.empty());
            REQUIRE(DeserializeUiDocument(unexpectedNeutral).HasError());
        }

        TEST_CASE("Runtime UI document validation rejects duplicate identities, properties, cycles, and missing references",
                  "[runtime_ui][document][serialization]") {
            UiDocumentBuilder duplicate{IdWith<UiDocumentId>(10), Revision(1)};
            const auto root = IdWith<UiElementId>(11);
            REQUIRE(duplicate.AddCanvas({IdWith<UiCanvasId>(12), root}).HasValue());
            REQUIRE(duplicate.AddElement({root, {}, Type("core.panel"), {{"value", true}, {"value", false}}, {}}).HasValue());
            REQUIRE(duplicate.AddElement({root, {}, Type("core.panel"), {}, {}}).HasValue());
            REQUIRE(std::move(duplicate).Build().HasError());

            UiDocumentBuilder cycle{IdWith<UiDocumentId>(20), Revision(1)};
            const auto first = IdWith<UiElementId>(21);
            const auto second = IdWith<UiElementId>(22);
            REQUIRE(cycle.AddCanvas({IdWith<UiCanvasId>(23), first}).HasValue());
            REQUIRE(cycle.AddElement({first, second, Type("core.panel"), {}, {}}).HasValue());
            REQUIRE(cycle.AddElement({second, first, Type("core.panel"), {}, {}}).HasValue());
            REQUIRE(std::move(cycle).Build().HasError());

            UiDocumentBuilder missingReference{IdWith<UiDocumentId>(30), Revision(1)};
            const auto missingRoot = IdWith<UiElementId>(31);
            REQUIRE(missingReference.AddCanvas({IdWith<UiCanvasId>(32), missingRoot}).HasValue());
            UiReference missing{.kind = UiReferenceKind::Element, .element = IdWith<UiElementId>(33)};
            REQUIRE(missingReference.AddElement({missingRoot, {}, Type("core.panel"), {}, {missing}}).HasValue());
            REQUIRE(std::move(missingReference).Build().HasError());
        }

        TEST_CASE("Runtime UI document parser rejects malformed, duplicate, oversized, and version-skewed source",
                  "[runtime_ui][document][serialization]") {
            const auto encoded = SerializeUiDocument(MakeDocument()).Value();
            auto malformed = encoded;
            const auto typeMarker = malformed.find("\"type\":\"bool\"");
            REQUIRE(typeMarker != std::string::npos);
            malformed.replace(typeMarker, std::string{"\"type\":\"bool\""}.size(), "\"type\":\"unknown\"");
            REQUIRE(DeserializeUiDocument(malformed).HasError());

            auto duplicate = encoded;
            const auto marker = std::string{"\"documentId\":"};
            const auto position = duplicate.find(marker);
            REQUIRE(position != std::string::npos);
            duplicate.insert(position, "\"documentId\":\"00000000000000000000000000000001\",");
            REQUIRE(DeserializeUiDocument(duplicate).HasError());

            auto unsupported = encoded;
            const auto minor = unsupported.find("\"minor\":1");
            REQUIRE(minor != std::string::npos);
            unsupported.replace(minor, 9, "\"minor\":99");
            REQUIRE(DeserializeUiDocument(unsupported).HasError());

            UiDocumentSerializationLimits tiny;
            tiny.maximumSourceBytes = encoded.size() - 1;
            REQUIRE(DeserializeUiDocument(encoded, tiny).HasError());
        }

        TEST_CASE("Runtime UI document migration is explicit, bounded, and detached", "[runtime_ui][document][serialization][migration]") {
            const auto source = MakeDocument({1, 0});
            const auto encoded = SerializeUiDocument(source);
            REQUIRE(encoded.HasValue());
            const auto parsed = DeserializeUiDocument(encoded.Value());
            REQUIRE(parsed.HasValue());
            const std::array steps{UiDocumentMigrationStep{{1, 0}, {1, 1}, &UpgradeToCurrent}};
            const auto migrated = MigrateUiDocument(parsed.Value(), CurrentUiDocumentSchemaVersion, steps);
            REQUIRE(migrated.HasValue());
            REQUIRE(migrated.Value().SchemaVersion() == CurrentUiDocumentSchemaVersion);
            REQUIRE(std::ranges::equal(migrated.Value().Elements(), source.Elements()));
            const UiDocumentSchemaVersion legacyVersion{1, 0};
            REQUIRE(parsed.Value().SchemaVersion() == legacyVersion);
            REQUIRE(MigrateUiDocument(parsed.Value(), CurrentUiDocumentSchemaVersion, {}).HasError());
        }
    }  // namespace
}  // namespace Horo::Runtime::Ui
