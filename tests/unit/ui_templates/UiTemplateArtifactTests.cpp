#include "Horo/UiTemplates/UiTemplateArtifact.h"
#include "UiTemplateOwnerTestFixture.h"

#include <algorithm>
#include <limits>
#include <nlohmann/json.hpp>
#include <tuple>

namespace Horo::UiTemplates {
    namespace {
        using namespace TestSupport;
        using Json = nlohmann::json;

        UiTemplateCookedDocument RichArtifact() {
            auto owner = UiTemplateDocumentOwner::Create(RichDocument(), ValueCatalog(Message()));
            REQUIRE(owner.HasValue());
            Sha256Digest manifest;
            manifest.bytes.front() = 7;
            auto cooked = owner.Value()->Capture().Cook({manifest});
            REQUIRE(cooked.HasValue());
            return std::move(cooked).Value();
        }

        /** @brief Adds a nonneutral responsive alternative to ordinary authored authority. */
        UiTemplateAuthoredDocument ProfiledDocument(const float width = 640) {
            auto source = RichDocument();
            UiDocumentBuilder builder{source.ordinary.Id(), source.ordinary.Revision(), source.ordinary.SchemaVersion()};
            for (const auto &canvas : source.ordinary.Canvases())
                REQUIRE(builder.AddCanvas(canvas).HasValue());
            for (const auto &element : source.ordinary.Elements())
                REQUIRE(builder.AddElement(element).HasValue());
            UiLayoutStyle layout;
            layout.width = UiLength::Dip(width);
            layout.container.orientation = UiLayoutOrientation::Horizontal;
            UiPresentationProfile row{Identity<UiPresentationProfileId>(30), 1};
            row.selector.width.maximum = 800;
            row.selector.aspect.minimum = UiPresentationAspectRatio{1, 2};
            row.overrides.push_back({Identity<UiElementId>(99), layout});
            REQUIRE(builder.AddPresentationProfiles({Identity<UiCanvasId>(1), UiPresentationProfileRevision::Create(7).Value(), {row}})
                        .HasValue());
            auto ordinary = std::move(builder).Build();
            REQUIRE(ordinary.HasValue());
            source.ordinary = std::move(ordinary).Value();
            return source;
        }

        /** @brief Mutates an external little-endian protocol field; this fixture never calls decoder internals. */
        void Integer(std::vector<std::uint8_t> &bytes, const std::size_t offset, const std::uint64_t value, const std::size_t width) {
            for (std::size_t i = 0; i < width; ++i)
                bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
        }

        /** @brief Produces integrity-correct hostile transport data so semantic rejection is not hidden by a hash failure. */
        std::vector<std::uint8_t> HostileRich(const UiTemplateCookedDocument &cooked, const std::string &rich) {
            auto encoded = EncodeTemplateArtifact(cooked, {});
            REQUIRE(encoded.HasValue());
            auto bytes = std::move(encoded).Value();
            bytes.resize(160 + cooked.ordinary.Payload().size());
            bytes.insert(bytes.end(), rich.begin(), rich.end());
            Integer(bytes, 24, rich.size(), 8);
            const auto hash = [](const std::string_view text) {
                return ComputeSha256(std::as_bytes(std::span{text}));
            };
            const auto hex = [](const Sha256Digest &digest) {
                return FormatSha256(digest).substr(7);
            };
            const Json identity{{"algorithm", 1},
                                {"source", hex(cooked.sourceIdentity)},
                                {"schemas", hex(cooked.schemaIdentity)},
                                {"dependencies", hex(cooked.dependencyManifest)},
                                {"ordinary", hex(ComputeSha256(std::as_bytes(cooked.ordinary.Payload())))},
                                {"rich", hex(hash(rich))}};
            const auto digest = hash(identity.dump());
            std::ranges::copy(digest.bytes, bytes.begin() + 128);
            return bytes;
        }
    }  // namespace

    TEST_CASE("Template reconstruction retains responsive profiles through flatten detach undo and rich artifacts",
              "[runtime_ui][template][headless][responsive]") {
        const auto source = ProfiledDocument();
        const auto profiles = source.ordinary.PresentationProfiles().front();
        auto roundtrip = RoundTrip(source, ValueCatalog(Message()));
        auto &owner = *roundtrip.reopened;
        const auto flattened = owner.Capture().Flatten();
        REQUIRE(flattened.HasValue());
        REQUIRE(flattened.Value().ordinary.PresentationProfiles().size() == 1);
        CHECK(flattened.Value().ordinary.PresentationProfiles().front() == profiles);
        Sha256Digest manifest;
        manifest.bytes.front() = 7;
        const auto cooked = owner.Capture().Cook({manifest});
        REQUIRE(cooked.HasValue());
        REQUIRE(cooked.Value().ordinary.PresentationProfiles().size() == 1);
        CHECK(cooked.Value().ordinary.PresentationProfiles().front() == profiles);
        const auto encoded = EncodeTemplateArtifact(cooked.Value(), {});
        REQUIRE(encoded.HasValue());
        const auto decoded = DecodeTemplateArtifact(encoded.Value(), manifest, {});
        REQUIRE(decoded.HasValue());
        REQUIRE(decoded.Value().ordinary.PresentationProfiles().size() == 1);
        CHECK(decoded.Value().ordinary.PresentationProfiles().front() == profiles);
        CHECK(decoded.Value().cacheIdentity == cooked.Value().cacheIdentity);
        const std::array allocation{UiTemplateDetachIdentity{owner.Capture().Expansions().front().root, Identity<UiElementId>(50)}};
        const auto detached = owner.PrepareDetach(Identity<UiTemplateInstanceId>(1), allocation);
        REQUIRE(detached.HasValue());
        REQUIRE(owner.Commit(detached.Value()).HasValue());
        REQUIRE(owner.Capture().Source().ordinary.PresentationProfiles().size() == 1);
        CHECK(owner.Capture().Source().ordinary.PresentationProfiles().front() == profiles);
        REQUIRE(owner.Undo().HasValue());
        REQUIRE(owner.Capture().Source().ordinary.PresentationProfiles().size() == 1);
        CHECK(owner.Capture().Source().ordinary.PresentationProfiles().front() == profiles);
    }

    TEST_CASE("Changed responsive template layouts invalidate ordinary payload and cache commitments",
              "[runtime_ui][template][headless][responsive]") {
        const auto catalog = ValueCatalog(Message());
        auto first = UiTemplateDocumentOwner::Create(ProfiledDocument(640), catalog);
        auto second = UiTemplateDocumentOwner::Create(ProfiledDocument(480), catalog);
        REQUIRE(first.HasValue());
        REQUIRE(second.HasValue());
        Sha256Digest manifest;
        manifest.bytes.front() = 7;
        const auto firstCook = first.Value()->Capture().Cook({manifest});
        const auto secondCook = second.Value()->Capture().Cook({manifest});
        REQUIRE(firstCook.HasValue());
        REQUIRE(secondCook.HasValue());
        CHECK_FALSE(std::ranges::equal(firstCook.Value().ordinary.Payload(), secondCook.Value().ordinary.Payload()));
        CHECK(firstCook.Value().cacheIdentity != secondCook.Value().cacheIdentity);
        CHECK(firstCook.Value().sourceIdentity != secondCook.Value().sourceIdentity);
        CHECK(firstCook.Value().schemaIdentity == secondCook.Value().schemaIdentity);
        CHECK(firstCook.Value().dependencyManifest == secondCook.Value().dependencyManifest);
    }

    TEST_CASE("Runtime rich artifact roundtrip retains localization target properties and exact cache commitments",
              "[runtime_ui][template][headless]") {
        const auto cooked = RichArtifact();
        auto encoded = EncodeTemplateArtifact(cooked, {});
        REQUIRE(encoded.HasValue());
        const auto decoded = DecodeTemplateArtifact(encoded.Value(), cooked.dependencyManifest, {});
        REQUIRE(decoded.HasValue());
        CHECK(decoded.Value().cacheIdentity == cooked.cacheIdentity);
        CHECK(decoded.Value().sourceIdentity == cooked.sourceIdentity);
        CHECK(decoded.Value().schemaIdentity == cooked.schemaIdentity);
        CHECK(decoded.Value().richPayload == cooked.richPayload);
        REQUIRE(decoded.Value().localizedTexts.size() == cooked.localizedTexts.size());
        for (std::size_t i = 0; i < cooked.localizedTexts.size(); ++i) {
            CHECK(decoded.Value().localizedTexts[i].element == cooked.localizedTexts[i].element);
            CHECK(decoded.Value().localizedTexts[i].property == cooked.localizedTexts[i].property);
            CHECK(decoded.Value().localizedTexts[i].text.Key() == cooked.localizedTexts[i].text.Key());
        }
        REQUIRE(decoded.Value().bindings.size() == 1);
        CHECK(decoded.Value().bindings.front().id == cooked.bindings.front().id);
    }

    TEST_CASE("Runtime rich artifact rejects malformed lengths headers truncation versions and manifest mismatch",
              "[runtime_ui][template][headless]") {
        const auto cooked = RichArtifact();
        const auto encoded = EncodeTemplateArtifact(cooked, {});
        REQUIRE(encoded.HasValue());
        for (const auto length : {std::size_t{0}, std::size_t{159}, encoded.Value().size() - 1})
            CHECK(DecodeTemplateArtifact(std::span{encoded.Value()}.first(length), cooked.dependencyManifest, {}).HasError());
        for (const auto &[offset, width, value] :
             std::array<std::tuple<std::size_t, std::size_t, std::uint64_t>, 7>{{{0, 1, 0},
                                                                                 {4, 4, 2},
                                                                                 {8, 4, 2},
                                                                                 {12, 4, 1},
                                                                                 {16, 8, 0},
                                                                                 {16, 8, std::numeric_limits<std::uint64_t>::max()},
                                                                                 {24, 8, 0}}}) {
            auto invalid = encoded.Value();
            Integer(invalid, offset, value, width);
            CHECK(DecodeTemplateArtifact(invalid, cooked.dependencyManifest, {}).HasError());
        }
        auto wrongManifest = cooked.dependencyManifest;
        ++wrongManifest.bytes.back();
        CHECK(DecodeTemplateArtifact(encoded.Value(), wrongManifest, {}).HasError());
        CHECK(DecodeTemplateArtifact(encoded.Value(), {}, {}).HasError());
        auto wrongCache = encoded.Value();
        ++wrongCache[128];
        CHECK(DecodeTemplateArtifact(wrongCache, cooked.dependencyManifest, {}).HasError());
        UiTemplateLimits smaller;
        smaller.maximumPayloadBytes = cooked.ordinary.Payload().size();
        CHECK(DecodeTemplateArtifact(encoded.Value(), cooked.dependencyManifest, smaller).HasError());
    }

    TEST_CASE("Integrity correct rich artifacts reject noncanonical duplicate escaped and unknown semantic fields",
              "[runtime_ui][template][headless]") {
        const auto cooked = RichArtifact();
        auto escaped = Json::parse(cooked.richPayload);
        escaped["texts"][0]["element"] = std::string(32, 'f');
        CHECK(DecodeTemplateArtifact(HostileRich(cooked, escaped.dump()), cooked.dependencyManifest, {}).HasError());
        auto duplicated = Json::parse(cooked.richPayload);
        duplicated["bindings"].push_back(duplicated["bindings"][0]);
        CHECK(DecodeTemplateArtifact(HostileRich(cooked, duplicated.dump()), cooked.dependencyManifest, {}).HasError());
        auto unknown = Json::parse(cooked.richPayload);
        unknown["instances"] = Json::array();
        CHECK(DecodeTemplateArtifact(HostileRich(cooked, unknown.dump()), cooked.dependencyManifest, {}).HasError());
        CHECK(DecodeTemplateArtifact(HostileRich(cooked, " " + cooked.richPayload), cooked.dependencyManifest, {}).HasError());
        const auto duplicateField = "{\"styles\":[]," + cooked.richPayload.substr(1);
        CHECK(DecodeTemplateArtifact(HostileRich(cooked, duplicateField), cooked.dependencyManifest, {}).HasError());
    }
}  // namespace Horo::UiTemplates
