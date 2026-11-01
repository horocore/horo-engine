#include "Horo/Application/ProjectSurfaceSettings.h"

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <fstream>
#include <iterator>

namespace {
    using namespace Horo;
    using namespace Horo::Application;
    constexpr auto First = "11111111-1111-4111-8111-111111111111";
    constexpr auto Second = "22222222-2222-4222-8222-222222222222";

    Physics::SurfaceMaterialId Id(const char *text) {
        auto parsed = Physics::SurfaceMaterialId::Parse(text);
        REQUIRE(parsed.HasValue());
        return parsed.Value();
    }

    ProjectSurfaceSettingsInput Input() {
        return {.projectId = "proj_preserved", .defaultSurfaceId = Id(First), .surfaces = {{Id(Second), "Stone"}, {Id(First), "Wood"}}};
    }

    std::string Source() {
        auto settings = ProjectSurfaceSettings::Create(Input());
        REQUIRE(settings.HasValue());
        auto source = EncodeProjectSurfaceSettings(settings.Value());
        REQUIRE(source.HasValue());
        return std::move(source).Value();
    }

    struct Directory {
        std::filesystem::path root;

        Directory() {
            static std::atomic<unsigned> next{};
            root = std::filesystem::temp_directory_path() /
                   ("horo-surface-settings-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                    std::to_string(next.fetch_add(1)));
            std::filesystem::create_directories(root / ".horo");
            root = std::filesystem::canonical(root);
        }

        ~Directory() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }

        void Write(const std::string &source) const {
            std::ofstream(root / ".horo/surfaces.json", std::ios::binary) << source;
        }

        std::string Read() const {
            std::ifstream input(root / ".horo/surfaces.json", std::ios::binary);
            return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        }
    };
}  // namespace

TEST_CASE("Surface project catalog canonicalizes IDs and includes authored default in identity", "[unit][project][surface]") {
    auto first = ProjectSurfaceSettings::Create(Input());
    REQUIRE(first.HasValue());
    CHECK(first.Value().Input().projectId == "proj_preserved");
    CHECK(first.Value().Input().surfaces.front().id == Id(First));
    CHECK(first.Value().Catalog()->Contains(Id(Second)));
    auto reordered = Input();
    std::ranges::reverse(reordered.surfaces);
    auto second = ProjectSurfaceSettings::Create(reordered);
    REQUIRE(second.HasValue());
    CHECK(first.Value().Fingerprint() == second.Value().Fingerprint());
    reordered.defaultSurfaceId = Id(Second);
    auto changed = ProjectSurfaceSettings::Create(reordered);
    REQUIRE(changed.HasValue());
    CHECK(first.Value().Fingerprint() != changed.Value().Fingerprint());
    CHECK(first.Value().Catalog()->Fingerprint() == changed.Value().Catalog()->Fingerprint());
    auto decoded = DecodeProjectSurfaceSettings(Source());
    REQUIRE(decoded.HasValue());
    CHECK(decoded.Value().Fingerprint() == first.Value().Fingerprint());
}

TEST_CASE("Surface adoption represents empty inventory without an inferred default", "[unit][project][surface]") {
    ProjectSurfaceSettingsInput input{.projectId = "legacy-project"};
    auto settings = ProjectSurfaceSettings::Create(input);
    REQUIRE(settings.HasValue());
    CHECK(settings.Value().Catalog()->Entries().empty());
    CHECK_FALSE(settings.Value().Input().defaultSurfaceId.has_value());
    auto source = EncodeProjectSurfaceSettings(settings.Value());
    REQUIRE(source.HasValue());
    CHECK(source.Value().find("\"defaultSurfaceId\":null") != std::string::npos);
    auto decoded = DecodeProjectSurfaceSettings(source.Value());
    REQUIRE(decoded.HasValue());
    CHECK(decoded.Value().Fingerprint() == settings.Value().Fingerprint());
}

TEST_CASE("Surface document rejects invalid defaults identities metadata and capacities", "[unit][project][surface]") {
    auto input = Input();
    SECTION("unknown default") {
        input.defaultSurfaceId = Id("33333333-3333-4333-8333-333333333333");
    }
    SECTION("zero default") {
        input.defaultSurfaceId = Physics::SurfaceMaterialId{};
    }
    SECTION("duplicate ID") {
        input.surfaces.push_back(input.surfaces.front());
    }
    SECTION("zero ID") {
        input.surfaces.front().id = {};
    }
    SECTION("empty display") {
        input.surfaces.front().displayName.clear();
    }
    SECTION("control display") {
        input.surfaces.front().displayName = "bad\nname";
    }
    SECTION("invalid UTF8") {
        input.surfaces.front().displayName = std::string(1, static_cast<char>(0xff));
    }
    SECTION("long display") {
        input.surfaces.front().displayName.assign(129, 'x');
    }
    SECTION("too many entries") {
        input.surfaces.resize(257);
    }
    CHECK(ProjectSurfaceSettings::Create(std::move(input)).HasError());
}

TEST_CASE("Surface document requires explicit project and revision identity", "[unit][project][surface]") {
    auto input = Input();
    SECTION("missing project") {
        input.projectId.clear();
    }
    SECTION("zero revision") {
        input.revision = 0;
    }
    CHECK(ProjectSurfaceSettings::Create(std::move(input)).HasError());
}

TEST_CASE("Surface source uses closed typed bounded JSON and explicit version diagnostics", "[unit][project][surface]") {
    auto source = Source();
    SECTION("unknown field") {
        source.insert(1, "\"unknown\":true,");
    }
    SECTION("duplicate field") {
        source.insert(1, "\"revision\":1,");
    }
    SECTION("negative revision") {
        const auto p = source.find("\"revision\":1");
        source.replace(p, 12, "\"revision\":-1");
    }
    SECTION("floating revision") {
        const auto p = source.find("\"revision\":1");
        source.replace(p, 12, "\"revision\":1.0");
    }
    SECTION("uppercase ID") {
        source = "{\"schemaVersion\":1,\"projectId\":\"p\",\"revision\":1,\"defaultSurfaceId\":null,\"surfaces\":[{\"id\":\"ABCDEFAB-1111-"
                 "4111-8111-111111111111\",\"displayName\":\"a\"}]}";
    }
    SECTION("deep JSON") {
        source = std::string(40, '[') + "0" + std::string(40, ']');
    }
    SECTION("byte limit") {
        source.assign(MaximumProjectSurfaceBytes + 1, ' ');
    }
    CHECK(DecodeProjectSurfaceSettings(source).HasError());
    auto version = Input();
    version.schemaVersion = 2;
    auto rejected = ProjectSurfaceSettings::Create(version);
    REQUIRE(rejected.HasError());
    CHECK(rejected.ErrorValue().code.Value() == ProjectSurfaceErrors::Version.code.Value());
    auto future = DecodeProjectSurfaceSettings(R"({"schemaVersion":2,"futureBody":{"newField":true}})");
    REQUIRE(future.HasError());
    CHECK(future.ErrorValue().code.Value() == ProjectSurfaceErrors::Version.code.Value());
}

TEST_CASE("Surface file capture retains exact bytes and rejects foreign project", "[unit][project][surface]") {
    Directory directory;
    const auto text = Source();
    directory.Write(text);
    auto captured = LoadProjectSurfaceSettings(directory.root, "proj_preserved");
    REQUIRE(captured.HasValue());
    CHECK(captured.Value().identity.exists);
    CHECK(captured.Value().identity.bytes == text.size());
    CHECK(captured.Value().identity.digest == ComputeSha256(std::as_bytes(std::span{text.data(), text.size()})));
    CHECK(LoadProjectSurfaceSettings(directory.root, "another-project").HasError());
    CHECK(directory.Read() == text);
    directory.Write(text + " ");
    auto changed = LoadProjectSurfaceSettings(directory.root, "proj_preserved");
    REQUIRE(changed.HasValue());
    CHECK(changed.Value().identity != captured.Value().identity);
    CHECK(changed.Value().settings->Fingerprint() == captured.Value().settings->Fingerprint());
}

TEST_CASE("Missing legacy surface catalogs fail without writing adoption data", "[unit][project][surface]") {
    Directory directory;
    auto missing = LoadProjectSurfaceSettings(directory.root, "legacy-project");
    REQUIRE(missing.HasError());
    CHECK(missing.ErrorValue().code.Value() == ProjectSurfaceErrors::Unavailable.code.Value());
    CHECK_FALSE(std::filesystem::exists(directory.root / ".horo/surfaces.json"));
    CHECK(std::filesystem::is_empty(directory.root / ".horo"));
}

TEST_CASE("Surface loader rejects physically escaping catalog symlinks", "[unit][project][surface]") {
    Directory directory;
    Directory foreign;
    foreign.Write(Source());
    std::filesystem::create_symlink(foreign.root / ".horo/surfaces.json", directory.root / ".horo/surfaces.json");
    CHECK(LoadProjectSurfaceSettings(directory.root, "proj_preserved").HasError());
    CHECK(foreign.Read() == Source());
}

TEST_CASE("Surface world capture retains exact catalog default and assigned generation", "[unit][project][surface]") {
    auto settings = ProjectSurfaceSettings::Create(Input());
    REQUIRE(settings.HasValue());
    auto captured = settings.Value().Capture(17);
    REQUIRE(captured.HasValue());
    CHECK(captured.Value().catalog == settings.Value().Catalog());
    CHECK(captured.Value().defaultSurface == settings.Value().Input().defaultSurfaceId);
    CHECK(captured.Value().mappingGeneration == 17);
    CHECK(settings.Value().Capture(0).HasError());
}
