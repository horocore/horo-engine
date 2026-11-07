#include "ColorCreativeLookPackage.h"

#include <atomic>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace Horo::Render::Internal;
    using Json = nlohmann::json;
    using Bytes = std::vector<std::uint8_t>;

    /** @brief Writes real finite test storage; never supplies native shader or mathematical look evidence. */
    void Write(const std::filesystem::path &path, std::span<const std::uint8_t> bytes) {
        std::ofstream output(path, std::ios::binary);
        REQUIRE(output.good());
        output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        REQUIRE(output.good());
    }

    /** @brief Copies ordinary test text into portable owned bytes. */
    Bytes BytesOf(std::string_view text) {
        return {text.begin(), text.end()};
    }

    /** @brief Hashes exactly the bytes stored in one synthetic codec fixture. */
    std::string Hash(const Bytes &bytes) {
        return FormatSha256(ComputeSha256(std::as_bytes(std::span{bytes}))).substr(7);
    }

    /** @brief Reads original test files only to prove failed decoding never writes caller data. */
    Bytes Read(const std::filesystem::path &path) {
        std::ifstream input(path, std::ios::binary);
        REQUIRE(input.good());
        return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    }

    /** @brief Owns a synthetically declared codec generation, explicitly without OCIO/conformance/native qualification. */
    struct Fixture final {
        std::filesystem::path root;
        Bytes payload, reference = BytesOf("{\"scope\":\"synthetic-codec-integrity-only\"}\n");
        Json manifest;

        /** @brief Prepares exact finite little-endian RGB values and real hashes over the supplied synthetic files. */
        Fixture() {
            static std::atomic<std::uint64_t> serial{};
            root = std::filesystem::temp_directory_path() /
                   ("horo creative codec " + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + " " +
                    std::to_string(serial.fetch_add(1)));
            REQUIRE(std::filesystem::create_directory(root));
            for (unsigned i = 0; i < 24; ++i)
                Append(std::bit_cast<std::uint32_t>(float(i) / 16));
            manifest = {{"schemaVersion", 1},
                        {"kind", "horo-creative-look-package"},
                        {"descriptor",
                         {{"schemaVersion", 2},
                          {"role", "CreativeLook"},
                          {"configurationRelease", 40000},
                          {"configurationSHA256", std::string(64, '1')},
                          {"dimension", 2},
                          {"domainMinimum", {-1, -1, -1}},
                          {"domainMaximum", {3, 3, 3}},
                          {"interpolation", "Tetrahedral"},
                          {"domainPolicy", "RejectOutsideV1"}}}};
            Records();
        }

        /** @brief Removes only the temporary directory originally claimed by this fixture. */
        ~Fixture() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }

        /** @brief Appends exact portable float bits without native endianness assumptions. */
        void Append(std::uint32_t bits) {
            for (unsigned shift = 0; shift < 32; shift += 8)
                payload.push_back(static_cast<std::uint8_t>(bits >> shift));
        }

        /** @brief Updates the sole authoritative file records after an intentional controlled fixture mutation. */
        void Records() {
            manifest["payload"] = {{"file", "look.rgb.f32le"}, {"bytes", payload.size()}, {"sha256", Hash(payload)}};
            manifest["reference"] = {{"file", "reference.json"}, {"bytes", reference.size()}, {"sha256", Hash(reference)}};
        }

        /** @brief Writes the exact three ordinary original files; no upload atlas is serialized or invented. */
        void Save() const {
            Write(root / "manifest.json", BytesOf(manifest.dump() + "\n"));
            Write(root / "look.rgb.f32le", payload);
            Write(root / "reference.json", reference);
        }
    };

    /** @brief Requires typed failure and verifies all three original files remain exactly unchanged. */
    void DeniedUnchanged(const Fixture &fixture) {
        const auto manifest = Read(fixture.root / "manifest.json");
        const auto result = LoadColorCreativeLookPackage(fixture.root);
        REQUIRE(result.HasError());
        CHECK_FALSE(result.ErrorValue().code.Value().empty());
        CHECK(Read(fixture.root / "manifest.json") == manifest);
        CHECK(Read(fixture.root / "look.rgb.f32le") == fixture.payload);
        CHECK(Read(fixture.root / "reference.json") == fixture.reference);
    }
}  // namespace

TEST_CASE("Creative look package decodes exact synthetic source bytes and pins the canonical schema2 factory result",
          "[renderer][color][look-package]") {
    Fixture fixture;
    const auto interpolation = GENERATE("Trilinear", "Tetrahedral");
    fixture.manifest["descriptor"]["interpolation"] = interpolation;
    fixture.Save();
    const auto loaded = LoadColorCreativeLookPackage(fixture.root);
    INFO((loaded.HasError() ? std::string{loaded.ErrorValue().code.Value()} + ": " + loaded.ErrorValue().message : std::string{"decoded"}));
    REQUIRE(loaded.HasValue());
    REQUIRE(loaded.Value().look != nullptr);
    const auto &look = *loaded.Value().look;
    CHECK(look.Descriptor().schemaVersion == 2);
    CHECK(look.Descriptor().interpolation ==
          (std::string_view{interpolation} == "Trilinear" ? ColorLutInterpolation::Trilinear : ColorLutInterpolation::Tetrahedral));
    CHECK(look.Descriptor().domainMinimum.x == -1);
    CHECK(look.Descriptor().domainMaximum.z == 3);
    REQUIRE(look.Rgb().size() == 24);
    for (std::size_t i = 0; i < 24; ++i)
        CHECK(look.Rgb()[i] == float(i) / 16);
    CHECK(FormatSha256(look.Descriptor().payloadDigest).substr(7) == Hash(fixture.payload));
    CHECK(FormatSha256(look.Descriptor().referenceDigest).substr(7) == Hash(fixture.reference));
    CHECK(loaded.Value().files[0] == Read(fixture.root / "manifest.json"));
    CHECK(loaded.Value().files[1] == fixture.payload);
    CHECK(loaded.Value().files[2] == fixture.reference);
    Write(fixture.root / "look.rgb.f32le", BytesOf("later mutation"));
    CHECK(look.Rgb()[1] == 1.0F / 16);
    CHECK(loaded.Value().files[1] == fixture.payload);
}

TEST_CASE("Creative look package rejects unknown missing and obsolete schema vocabulary without changing files",
          "[renderer][color][look-package]") {
    Fixture fixture;
    auto &descriptor = fixture.manifest["descriptor"];
    SECTION("unknown top field") {
        fixture.manifest["extra"] = true;
    }
    SECTION("wrong kind") {
        fixture.manifest["kind"] = "cube";
    }
    SECTION("old container") {
        fixture.manifest["schemaVersion"] = 0;
    }
    SECTION("old inert transform") {
        descriptor["schemaVersion"] = 1;
    }
    SECTION("missing domain policy") {
        descriptor.erase("domainPolicy");
    }
    SECTION("unknown role") {
        descriptor["role"] = "OutputTransform";
    }
    SECTION("unknown interpolation") {
        descriptor["interpolation"] = "Nearest";
    }
    SECTION("clamping policy is unsupported") {
        descriptor["domainPolicy"] = "Clamp";
    }
    SECTION("invalid dimension") {
        descriptor["dimension"] = 66;
    }
    SECTION("noninteger dimension") {
        descriptor["dimension"] = 2.5;
    }
    SECTION("missing configuration") {
        descriptor["configurationSHA256"] = std::string(64, '0');
    }
    fixture.Save();
    DeniedUnchanged(fixture);
}

TEST_CASE("Creative look package binds fixed paths exact lengths and actual bytes to the sole hash records",
          "[renderer][color][look-package]") {
    Fixture fixture;
    SECTION("foreign path") {
        fixture.manifest["payload"]["file"] = "../outside.rgb";
    }
    SECTION("incorrect payload size") {
        fixture.manifest["payload"]["bytes"] = fixture.payload.size() - 1;
    }
    SECTION("incorrect payload digest") {
        fixture.manifest["payload"]["sha256"] = std::string(64, '2');
    }
    SECTION("incorrect reference digest") {
        fixture.manifest["reference"]["sha256"] = std::string(64, '2');
    }
    SECTION("malformed digest") {
        fixture.manifest["reference"]["sha256"] = "sha256:wrong";
    }
    SECTION("prefixed digest is outside the raw-hex schema") {
        fixture.manifest["payload"]["sha256"] = "sha256:" + Hash(fixture.payload);
    }
    SECTION("duplicate hash authority") {
        fixture.manifest["descriptor"]["payloadDigest"] = Hash(fixture.payload);
    }
    SECTION("extra record field") {
        fixture.manifest["reference"]["extra"] = 1;
    }
    fixture.Save();
    DeniedUnchanged(fixture);
}

TEST_CASE("Creative look package passes finite domains and sample validation to the canonical transform factory",
          "[renderer][color][look-package]") {
    Fixture fixture;
    SECTION("wrong sample count with matching record") {
        fixture.payload.pop_back();
    }
    SECTION("NaN sample with matching digest") {
        fixture.payload[0] = 0;
        fixture.payload[1] = 0;
        fixture.payload[2] = 0xc0;
        fixture.payload[3] = 0x7f;
    }
    SECTION("infinite sample with matching digest") {
        fixture.payload[0] = 0;
        fixture.payload[1] = 0;
        fixture.payload[2] = 0x80;
        fixture.payload[3] = 0x7f;
    }
    SECTION("empty domain") {
        fixture.manifest["descriptor"]["domainMaximum"] = {-1, 3, 3};
    }
    SECTION("unrepresentable domain endpoint") {
        fixture.manifest["descriptor"]["domainMaximum"] = {1e100, 3, 3};
    }
    SECTION("unrepresentable finite domain width") {
        fixture.manifest["descriptor"]["domainMinimum"] = {-3e38, -1, -1};
        fixture.manifest["descriptor"]["domainMaximum"] = {3e38, 3, 3};
    }
    fixture.Records();
    fixture.Save();
    DeniedUnchanged(fixture);
}

TEST_CASE("Creative look reference bytes require a strict finite duplicate-free object even when their digest matches",
          "[renderer][color][look-package]") {
    Fixture fixture;
    const auto reference = GENERATE("[]", "{broken}", "{\"a\":1,\"a\":2}", "{\"a\":NaN}");
    fixture.reference = BytesOf(reference);
    fixture.Records();
    fixture.Save();
    DeniedUnchanged(fixture);
}

TEST_CASE("Creative look package rejects duplicate manifest fields instead of accepting last-value identity",
          "[renderer][color][look-package]") {
    Fixture fixture;
    fixture.Save();
    auto text = fixture.manifest.dump();
    text.insert(1, "\"schemaVersion\":1,");
    Write(fixture.root / "manifest.json", BytesOf(text));
    DeniedUnchanged(fixture);
}

TEST_CASE("Creative look package enforces bounded real file storage without truncation or fallback", "[renderer][color][look-package]") {
    Fixture fixture;
    fixture.Save();
    const auto selected = GENERATE(0U, 1U, 2U);
    constexpr std::array<std::string_view, 3> paths{"manifest.json", "look.rgb.f32le", "reference.json"};
    constexpr std::array<std::uintmax_t, 3> maximum{64U * 1024U, 65U * 65U * 65U * 12U, 128U * 1024U};
    const auto path = fixture.root / paths[selected];
    std::filesystem::resize_file(path, maximum[selected] + 1);
    const auto result = LoadColorCreativeLookPackage(fixture.root);
    REQUIRE(result.HasError());
    CHECK_FALSE(result.ErrorValue().code.Value().empty());
    CHECK(std::filesystem::file_size(path) == maximum[selected] + 1);
}

TEST_CASE("Creative look package rejects missing and statically symbolic source files without touching external targets",
          "[renderer][color][look-package]") {
    Fixture fixture;
    fixture.Save();
    const auto payload = fixture.root / "look.rgb.f32le";
    REQUIRE(std::filesystem::remove(payload));
    SECTION("missing payload") {
        CHECK(LoadColorCreativeLookPackage(fixture.root).HasError());
        CHECK_FALSE(std::filesystem::exists(payload));
    }
    SECTION("symbolic payload") {
        const auto external = fixture.root / "external bytes";
        Write(external, fixture.payload);
        std::filesystem::create_symlink(external, payload);
        CHECK(LoadColorCreativeLookPackage(fixture.root).HasError());
        CHECK(std::filesystem::is_symlink(payload));
        CHECK(Read(external) == fixture.payload);
    }
    CHECK(Read(fixture.root / "reference.json") == fixture.reference);
}
