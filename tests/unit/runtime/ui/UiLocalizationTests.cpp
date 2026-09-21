#include "Horo/Runtime/Ui/UiDocument.h"
#include "Horo/Runtime/Ui/UiLocalization.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename Id> Id StableId(const std::uint8_t marker) {
            SerializedUiId bytes{};
            bytes.back() = marker;
            return Id::Create(bytes).Value();
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

        UiLocalizedAssetReference LocalizedAsset(const UiLocalizedAssetFallbackPolicy policy = UiLocalizedAssetFallbackPolicy::UseNeutral) {
            std::vector<UiLocalizedAssetVariant> variants{
                {Locale("en-US"), Asset(1)},
                {Locale("tr-TR"), Asset(2)},
            };
            const std::optional<Assets::AssetId> neutral =
                policy == UiLocalizedAssetFallbackPolicy::UseNeutral ? std::optional<Assets::AssetId>{Asset(3)} : std::nullopt;
            return UiLocalizedAssetReference::Create(Type("core.texture"), policy, std::move(variants), neutral).Value();
        }

        TEST_CASE("Runtime UI localized text owns stable keys, typed arguments, and fallback", "[runtime_ui][localization]") {
            const auto key = UiMessageKey::Create("game.menu", "project.item_count");
            REQUIRE(key.HasValue());
            REQUIRE(key.Value().Canonical() == "game.menu:project.item_count");

            auto text = UiLocalizedText::Create(std::move(key).Value(),
                                                std::vector<UiLocalizedArgument>{{"name", std::string{"Horo"}}, {"count", std::int64_t{3}}},
                                                "{count} project(s)");
            REQUIRE(text.HasValue());
            REQUIRE(text.Value().IsValid());
            REQUIRE(text.Value().Arguments().size() == 2);
            REQUIRE(text.Value().Arguments()[0].name == "count");
            REQUIRE(text.Value().Arguments()[1].name == "name");
            REQUIRE(std::get<std::int64_t>(text.Value().FindArgument("count")->value) == 3);
            REQUIRE(text.Value().FallbackText() == "{count} project(s)");
            REQUIRE(text.Value().FailurePolicy() == UiLocalizedTextFailurePolicy::UseFallback);
        }

        TEST_CASE("Runtime UI localization rejects malformed and unbounded references", "[runtime_ui][localization][edge]") {
            REQUIRE(UiLocaleTag::Parse("tr_tr").HasError());
            REQUIRE(UiMessageKey::Create("game:invalid", "menu.title").HasError());

            auto key = UiMessageKey::Create("game", "menu.title").Value();
            REQUIRE(UiLocalizedText::Create(key, std::vector<UiLocalizedArgument>{}, "").HasError());
            REQUIRE(UiLocalizedText::Create(key, std::vector<UiLocalizedArgument>{{"count", std::numeric_limits<double>::quiet_NaN()}},
                                            "fallback")
                        .HasError());

            std::vector<UiLocaleTag> duplicateLocales{Locale("en-US"), Locale("en-US")};
            REQUIRE(UiLocaleFallbackChain::Create(std::move(duplicateLocales)).HasError());

            std::vector<UiLocalizedAssetVariant> duplicateVariants{{Locale("en-US"), Asset(1)}, {Locale("en-US"), Asset(2)}};
            REQUIRE(UiLocalizedAssetReference::Create(Type("core.texture"), UiLocalizedAssetFallbackPolicy::Required,
                                                      std::move(duplicateVariants))
                        .HasError());
        }

        TEST_CASE("Runtime UI localized asset selection is bounded and policy-explicit", "[runtime_ui][localization][asset]") {
            const auto reference = LocalizedAsset();
            const std::array trChain{Locale("tr-TR"), Locale("tr"), Locale("en-US")};
            const auto selected = reference.Resolve(trChain);
            REQUIRE(selected.HasValue());
            REQUIRE(selected.Value().has_value());
            REQUIRE(selected.Value()->asset == Asset(2));
            REQUIRE(selected.Value()->matchedLocale == "tr-TR");
            REQUIRE_FALSE(selected.Value()->usedNeutral);

            const std::array neutralChain{Locale("de-DE"), Locale("de")};
            const auto neutral = reference.Resolve(neutralChain);
            REQUIRE(neutral.HasValue());
            REQUIRE(neutral.Value().has_value());
            REQUIRE(neutral.Value()->asset == Asset(3));
            REQUIRE(neutral.Value()->matchedLocale.empty());
            REQUIRE(neutral.Value()->usedNeutral);

            const auto required = LocalizedAsset(UiLocalizedAssetFallbackPolicy::Required);
            const auto unavailable = required.Resolve(neutralChain);
            REQUIRE(unavailable.HasError());
            REQUIRE(unavailable.ErrorValue().code.Value() == UiErrors::LocalizedAssetUnavailable.code.Value());

            const auto omitted = UiLocalizedAssetReference::Create(Type("core.texture"), UiLocalizedAssetFallbackPolicy::Omit,
                                                                   std::vector<UiLocalizedAssetVariant>{{Locale("en-US"), Asset(1)}});
            REQUIRE(omitted.HasValue());
            const auto omission = omitted.Value().Resolve(neutralChain);
            REQUIRE(omission.HasValue());
            REQUIRE_FALSE(omission.Value().has_value());

            REQUIRE(reference.Dependencies().size() == 3);
            REQUIRE(reference.Dependencies()[0].asset == Asset(1));
            REQUIRE(reference.Dependencies()[1].asset == Asset(2));
            REQUIRE(reference.Dependencies()[2].asset == Asset(3));
            REQUIRE(reference.Dependencies()[2].required);
        }

        TEST_CASE("Runtime UI documents atomically enumerate localized asset dependencies", "[runtime_ui][localization][document]") {
            UiDocumentBuilder builder{StableId<UiDocumentId>(7), UiDocumentRevision::Create(1).Value()};
            const UiLocalizedText text = UiLocalizedText::Create(UiMessageKey::Create("game", "menu.title").Value(), "Title").Value();
            REQUIRE(builder.AddLocalizedText(text).HasValue());

            auto reference = LocalizedAsset();
            REQUIRE(builder.AddLocalizedAsset(std::move(reference)).HasValue());
            REQUIRE(builder.RequireAsset({Asset(4), Type("core.font"), true}).HasValue());
            const auto document = std::move(builder).Build();
            REQUIRE(document.HasError());  // A canvas is still required before publication.

            UiDocumentBuilder transactional{StableId<UiDocumentId>(9), UiDocumentRevision::Create(1).Value()};
            REQUIRE(transactional.AddLocalizedAsset(LocalizedAsset()).HasValue());
            const auto conflictingReference =
                UiLocalizedAssetReference::Create(Type("core.font"), UiLocalizedAssetFallbackPolicy::Omit,
                                                  std::vector<UiLocalizedAssetVariant>{{Locale("en-US"), Asset(1)}});
            REQUIRE(conflictingReference.HasValue());
            REQUIRE(transactional.AddLocalizedAsset(conflictingReference.Value()).HasError());
            REQUIRE(transactional.AddCanvas({StableId<UiCanvasId>(3), StableId<UiElementId>(4)}).HasValue());
            const auto transactionResult = std::move(transactional).Build();
            REQUIRE(transactionResult.HasValue());
            REQUIRE(transactionResult.Value().LocalizedAssets().size() == 1);
            REQUIRE(transactionResult.Value().Dependencies().size() == 3);

            UiDocumentBuilder complete{StableId<UiDocumentId>(8), UiDocumentRevision::Create(1).Value()};
            REQUIRE(complete.AddCanvas({StableId<UiCanvasId>(1), StableId<UiElementId>(2)}).HasValue());
            REQUIRE(complete.AddLocalizedText(text).HasValue());
            REQUIRE(complete.AddLocalizedAsset(LocalizedAsset()).HasValue());
            const auto published = std::move(complete).Build();
            REQUIRE(published.HasValue());
            REQUIRE(published.Value().LocalizedTexts().size() == 1);
            REQUIRE(published.Value().LocalizedAssets().size() == 1);
            REQUIRE(published.Value().Dependencies().size() == 3);
            REQUIRE(published.Value().Dependencies()[0].asset == Asset(1));
            REQUIRE(published.Value().Dependencies()[2].asset == Asset(3));
        }

        TEST_CASE("Runtime UI localized references remain isolated across reload and owner shutdown",
                  "[runtime_ui][localization][reload]") {
            auto first = LocalizedAsset();
            auto second = UiLocalizedAssetReference::Create(Type("core.texture"), UiLocalizedAssetFallbackPolicy::UseNeutral,
                                                            std::vector<UiLocalizedAssetVariant>{{Locale("en-US"), Asset(4)}}, Asset(5))
                              .Value();
            const std::array chain{Locale("en-US")};
            REQUIRE(first.Resolve(chain).Value()->asset == Asset(1));
            REQUIRE(second.Resolve(chain).Value()->asset == Asset(4));
            first = UiLocalizedAssetReference::Create(Type("core.texture"), UiLocalizedAssetFallbackPolicy::UseNeutral,
                                                      std::vector<UiLocalizedAssetVariant>{{Locale("en-US"), Asset(6)}}, Asset(7))
                        .Value();
            REQUIRE(second.Resolve(chain).Value()->asset == Asset(4));
            second = UiLocalizedAssetReference::Create(Type("core.texture"), UiLocalizedAssetFallbackPolicy::UseNeutral,
                                                       std::vector<UiLocalizedAssetVariant>{{Locale("en-US"), Asset(8)}}, Asset(9))
                         .Value();
            REQUIRE(first.Resolve(chain).Value()->asset == Asset(6));
        }
    }  // namespace
}  // namespace Horo::Runtime::Ui
