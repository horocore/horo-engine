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
            std::copy(digest.bytes.begin(), digest.bytes.end(), bytes.begin() + 128);
            return bytes;
        }
    }  // namespace

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
        for (const auto [offset, width, value] :
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
