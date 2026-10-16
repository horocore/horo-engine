#include "AllocationProbe.h"
#include "CollisionSettingsTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

namespace Horo::Tests::CollisionSettings {
    TEST_CASE("Collision document codec is deterministic and display renames preserve semantic identity", "[collision][project][codec]") {
        auto first = Application::ProjectCollisionSettings::Create(Input());
        REQUIRE(first.HasValue());
        auto input = Input();
        std::ranges::reverse(input.schema.profiles);
        std::ranges::reverse(input.profiles);
        input.profiles.front().name = "Renamed stable profile";
        auto second = Application::ProjectCollisionSettings::Create(input);
        REQUIRE(second.HasValue());
        CHECK(second.Value().Schema()->SemanticFingerprint() == first.Value().Schema()->SemanticFingerprint());
        auto encoded = Application::EncodeProjectCollisionSettings(second.Value());
        REQUIRE(encoded.HasValue());
        auto decoded = Application::DecodeProjectCollisionSettings(encoded.Value());
        REQUIRE(decoded.HasValue());
        CHECK(Application::EncodeProjectCollisionSettings(decoded.Value()).Value() == encoded.Value());
        CHECK(decoded.Value().Input().profiles.front().id == IdOf<Physics::CollisionProfileId>(2));
        CHECK(decoded.Value().Capture(0).HasError());
        const auto capture = decoded.Value().Capture(42);
        REQUIRE(capture.HasValue());
        CHECK(capture.Value().schema == decoded.Value().Schema());
        CHECK(capture.Value().generation == 42);
    }

    TEST_CASE("Collision strict codec rejects malformed duplicate versioned and incomplete policy", "[collision][project][codec]") {
        const auto valid = Application::ProjectCollisionSettings::Create(Input());
        REQUIRE(valid.HasValue());
        const auto encoded = Application::EncodeProjectCollisionSettings(valid.Value()).Value();
        auto source = nlohmann::json::parse(encoded);
        SECTION("duplicate") {
            CHECK(Application::DecodeProjectCollisionSettings("{\"formatVersion\":1," + encoded.substr(1)).HasError());
        }
        SECTION("malformed") {
            CHECK(Application::DecodeProjectCollisionSettings("{").HasError());
        }
        SECTION("future") {
            source["formatVersion"] = 2;
        }
        SECTION("revision zero") {
            source["revision"] = 0;
        }
        SECTION("unknown field") {
            source["ambientRegistry"] = true;
        }
        SECTION("missing metadata") {
            source.erase("layerNames");
        }
        SECTION("oversize") {
            CHECK(Application::DecodeProjectCollisionSettings(std::string(4 * 1024 * 1024 + 1, ' ')).HasError());
        }
        if (source != nlohmann::json::parse(encoded))
            CHECK(Application::DecodeProjectCollisionSettings(source.dump()).HasError());
        auto incomplete = Input();
        incomplete.schema.pairs.clear();
        CHECK(Application::ProjectCollisionSettings::Create(incomplete).HasError());
        incomplete = Input();
        incomplete.schema.profiles.front().queryResponses.clear();
        CHECK(Application::ProjectCollisionSettings::Create(incomplete).HasError());
    }

    TEST_CASE("Collision loader never invents missing policy and binds exact project identity", "[collision][project][load]") {
        Project project;
        CHECK(Application::LoadProjectCollisionSettings(project.root, "foreign-project").HasError());
        const auto loaded = project.store.Load();
        REQUIRE(loaded.HasValue());
        CHECK(loaded.Value().identity.exists);
        std::filesystem::remove(project.root / ".horo/collision.json");
        CHECK(project.store.Load().HasError());
        CHECK_FALSE(std::filesystem::exists(project.root / ".horo/collision.json"));
    }

    TEST_CASE("Collision store checks revision source CAS and reviewed source fences before publication", "[collision][project][store]") {
        Project project;
        const auto accepted = project.store.Load().Value();
        auto input = Input();
        input.revision = 2;
        input.layers.front().name = "Rename";
        auto next = Application::ProjectCollisionSettings::Create(input);
        REQUIRE(next.HasValue());
        const auto ownerPath = project.root / "Assets/reference.txt";
        Write(ownerPath, "before");
        const Editor::CollisionSourceFence fence{ownerPath, Assets::CaptureAssetSource(project.root, ownerPath, 1024).Value().identity};
        Write(ownerPath, "after");
        CHECK(project.store
                  .Save(accepted, next.Value(), Editor::CollisionPublicationSourceFence{project.registry.Snapshot(), std::span{&fence, 1}})
                  .HasError());
        CHECK(project.store.Load().Value().identity == accepted.identity);
        REQUIRE(project.store.Save(accepted, next.Value()).HasValue());
        CHECK(project.store.Save(accepted, next.Value()).HasError());
        CHECK(project.store.Load().Value().settings->Input().revision == 2);
        CHECK(accepted.settings->Schema()->SemanticFingerprint() == next.Value().Schema()->SemanticFingerprint());
    }
}  // namespace Horo::Tests::CollisionSettings
