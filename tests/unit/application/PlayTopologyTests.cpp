#include "Horo/Application/PlayTopology.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>

using namespace Horo;
using namespace Horo::Application;

namespace {
    PlayTopologyProfile Profile(const PlayTopologyKind kind = PlayTopologyKind::Listen, const std::uint64_t id = 1) {
        PlayTopologyProfile profile{.id = id, .name = "Preview", .kind = kind, .map = "assets/scenes/main.horo"};
        if (kind != PlayTopologyKind::Standalone) {
            profile.serverCount = 1;
            profile.clientCount = 2;
            profile.transport = Network::NetworkTransportProviderId::Create(17).Value();
            profile.port = 7777;
        }
        return profile;
    }

    PlayTopologyCapabilities Capabilities() {
        return {.generation = 4,
                .enabled = true,
                .preview = true,
                .roles = Network::NetworkProjectRoleSet::Standalone | Network::NetworkProjectRoleSet::Client |
                         Network::NetworkProjectRoleSet::ListenServer | Network::NetworkProjectRoleSet::DedicatedServer,
                .transports = {Network::NetworkTransportProviderId::Create(17).Value()},
                .simulationPresets = {21},
                .maps = {"assets/scenes/main.horo"}};
    }

    struct Directory final {
        Directory()
            : root(std::filesystem::temp_directory_path() /
                   ("horo-play-topology-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
            std::filesystem::create_directories(root);
        }

        ~Directory() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }

        std::filesystem::path root;
    };

    std::string Read(const std::filesystem::path &path) {
        std::ifstream file(path, std::ios::binary);
        return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    }

    void Write(const std::filesystem::path &path, const std::string &text) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << text;
        REQUIRE(file.good());
    }
}  // namespace

TEST_CASE("Play topology admits each complete bounded preview without changing project defaults", "[network][play_topology]") {
    for (auto kind : {PlayTopologyKind::Standalone, PlayTopologyKind::Listen, PlayTopologyKind::Dedicated}) {
        const PlayTopologyCatalog catalog{.profiles = {Profile(kind)}};
        PlayTopologyUserSettings user;
        if (kind != PlayTopologyKind::Standalone)
            user.overrides.push_back({1, 8000});
        const auto admitted = PreflightPlayTopology(catalog, user, 1, Capabilities());
        REQUIRE(admitted.HasValue());
        const auto &plan = admitted.Value();
        CHECK(plan.participantCount == (kind == PlayTopologyKind::Standalone ? 1 : 3));
        CHECK(plan.participants[0].role == (kind == PlayTopologyKind::Standalone ? Network::NetworkProjectRole::Standalone
                                            : kind == PlayTopologyKind::Listen   ? Network::NetworkProjectRole::ListenServer
                                                                                 : Network::NetworkProjectRole::DedicatedServer));
        for (std::size_t i = 1; i < plan.participantCount; ++i) {
            CHECK(plan.participants[i].role == Network::NetworkProjectRole::Client);
            CHECK(plan.participants[i].port == 8000);
        }
        CHECK(catalog.profiles[0].port == (kind == PlayTopologyKind::Standalone ? 0 : 7777));
        CHECK(ValidatePlayTopologyPlan(plan, catalog, user, Capabilities()).HasValue());
    }
}

TEST_CASE("Play topology validates finite counts and portable scene identity", "[network][play_topology]") {
    auto profile = Profile();
    for (const auto *map : {"/tmp/main.horo", "C:/main.horo", "../main.horo", "assets/../main.horo", "assets//main.horo",
                            "assets\\main.horo", "main.txt", "./main.horo"}) {
        profile.map = map;
        CHECK(ValidatePlayTopology(profile).HasError());
    }
    profile = Profile();
    profile.clientCount = 7;
    CHECK(ValidatePlayTopology(profile).HasValue());
    profile.clientCount = 8;
    CHECK(ValidatePlayTopology(profile).HasError());
    profile.kind = PlayTopologyKind::Dedicated;
    CHECK(ValidatePlayTopology(profile).HasValue());
    profile.clientCount = 9;
    CHECK(ValidatePlayTopology(profile).HasError());
    profile = Profile(PlayTopologyKind::Standalone);
    profile.port = 7777;
    CHECK(ValidatePlayTopology(profile).HasError());
    profile = Profile();
    profile.transport = {};
    CHECK(ValidatePlayTopology(profile).HasError());
    profile = Profile();
    profile.name = std::string(97, 'x');
    CHECK(ValidatePlayTopology(profile).HasError());
    profile.name = std::string(1, static_cast<char>(0xff));
    CHECK(ValidatePlayTopology(profile).HasError());
}

TEST_CASE("Play topology preflight denies disabled release shutdown and unsupported inputs", "[network][play_topology]") {
    PlayTopologyCatalog catalog{.profiles = {Profile()}};
    catalog.profiles[0].simulationPreset = 21;
    const auto check = [&](const PlayTopologyCapabilities &capabilities) {
        CHECK(PreflightPlayTopology(catalog, {}, 1, capabilities).HasError());
    };
    auto caps = Capabilities();
    caps.enabled = false;
    check(caps);
    caps = Capabilities();
    caps.preview = false;
    check(caps);
    caps = Capabilities();
    caps.stopping = true;
    check(caps);
    caps = Capabilities();
    caps.generation = 0;
    check(caps);
    caps = Capabilities();
    caps.roles = Network::NetworkProjectRoleSet::ListenServer;
    check(caps);
    caps = Capabilities();
    caps.maps.clear();
    check(caps);
    caps = Capabilities();
    caps.transports.clear();
    check(caps);
    caps = Capabilities();
    caps.simulationPresets.clear();
    check(caps);
    CHECK(PreflightPlayTopology(catalog, {}, 2, Capabilities()).HasError());
    catalog.profiles.push_back(Profile());
    CHECK(PreflightPlayTopology(catalog, {}, 1, Capabilities()).HasError());
}

TEST_CASE("Play topology launch fence rejects changed snapshots and fabricated participant intent", "[network][play_topology]") {
    PlayTopologyCatalog catalog{.profiles = {Profile()}};
    PlayTopologyUserSettings user;
    auto caps = Capabilities();
    const auto admitted = PreflightPlayTopology(catalog, user, 1, caps);
    REQUIRE(admitted.HasValue());
    const auto plan = admitted.Value();
    ++catalog.revision;
    CHECK(ValidatePlayTopologyPlan(plan, catalog, user, caps).HasError());
    --catalog.revision;
    ++user.revision;
    CHECK(ValidatePlayTopologyPlan(plan, catalog, user, caps).HasError());
    --user.revision;
    ++caps.generation;
    CHECK(ValidatePlayTopologyPlan(plan, catalog, user, caps).HasError());
    --caps.generation;
    caps.transports.clear();
    CHECK(ValidatePlayTopologyPlan(plan, catalog, user, caps).HasError());
    caps = Capabilities();
    catalog.profiles[0].port = 8000;
    CHECK(ValidatePlayTopologyPlan(plan, catalog, user, caps).HasError());
    catalog.profiles[0].port = 7777;
    auto forged = plan;
    forged.participants[1].role = Network::NetworkProjectRole::DedicatedServer;
    CHECK(ValidatePlayTopologyPlan(forged, catalog, user, caps).HasError());
    forged = plan;
    forged.participantCount = 100;
    CHECK(ValidatePlayTopologyPlan(forged, catalog, user, caps).HasError());
}

TEST_CASE("Play topology codec is deterministic closed bounded and separate from machine state", "[network][play_topology]") {
    PlayTopologyCatalog catalog{.profiles = {Profile(PlayTopologyKind::Dedicated, 2), Profile(PlayTopologyKind::Standalone, 1)}};
    auto encoded = SerializePlayTopologies(catalog);
    REQUIRE(encoded.HasValue());
    std::ranges::reverse(catalog.profiles);
    CHECK(SerializePlayTopologies(catalog).Value() == encoded.Value());
    auto parsed = ParsePlayTopologies(encoded.Value());
    REQUIRE(parsed.HasValue());
    CHECK(parsed.Value() == catalog);
    const PlayTopologyUserSettings user{.overrides = {{2, 8000}, {1, 7000}}};
    auto overrides = SerializePlayTopologyOverrides(user);
    REQUIRE(overrides.HasValue());
    CHECK(ParsePlayTopologyOverrides(overrides.Value()).HasValue());
    CHECK(ParsePlayTopologies(overrides.Value()).HasError());
    CHECK(ParsePlayTopologyOverrides(encoded.Value()).HasError());
    CHECK(encoded.Value().find("credential") == std::string::npos);
    CHECK(encoded.Value().find("8000") == std::string::npos);
    CHECK(ParsePlayTopologies(std::string(65537, ' ')).HasError());
    CHECK(ParsePlayTopologies(R"({"version":1,"version":1,"revision":1,"profiles":[]})").HasError());
    const auto json = nlohmann::ordered_json::parse(encoded.Value());
    for (const auto *field : {"password", "token", "absolutePath"}) {
        auto invalid = json;
        invalid["profiles"][0][field] = "secret";
        CHECK(ParsePlayTopologies(invalid.dump()).HasError());
    }
    auto invalid = json;
    invalid["version"] = 2;
    CHECK(ParsePlayTopologies(invalid.dump()).HasError());
    invalid = json;
    invalid["profiles"][0]["clients"] = -1;
    CHECK(ParsePlayTopologies(invalid.dump()).HasError());
    invalid = json;
    invalid["profiles"][1]["port"] = 65536;
    CHECK(ParsePlayTopologies(invalid.dump()).HasError());
    invalid = json;
    invalid["profiles"].push_back(invalid["profiles"][0]);
    CHECK(ParsePlayTopologies(invalid.dump()).HasError());
    catalog.profiles.clear();
    for (std::uint64_t id = 1; id <= 16; ++id)
        catalog.profiles.push_back(Profile(PlayTopologyKind::Standalone, id));
    CHECK(SerializePlayTopologies(catalog).HasValue());
    catalog.profiles.push_back(Profile(PlayTopologyKind::Standalone, 17));
    CHECK(SerializePlayTopologies(catalog).HasError());
}

TEST_CASE("Play topology store persists independent revisions and rejects stale writers", "[network][play_topology][storage]") {
    Directory directory;
    NativeDurableFileSystem files;
    const auto project = directory.root / "config" / "play.json";
    const auto user = directory.root / ".horo" / "local" / "play.json";
    PlayTopologyStore store(files, project, user);
    REQUIRE(store.Reload().HasValue());
    REQUIRE(store.SaveProfile(1, Profile()).HasValue());
    const auto original = Read(project);
    auto saved = store.SaveOverride(1, {1, 8000});
    REQUIRE(saved.HasValue());
    CHECK(Read(project) == original);
    CHECK(store.Project().revision == 2);
    CHECK(store.User().revision == 2);
    PlayTopologyStore restarted(files, project, user);
    REQUIRE(restarted.Reload().HasValue());
    CHECK(restarted.Project() == store.Project());
    CHECK(restarted.User() == store.User());
    auto changed = Profile();
    changed.name = "Changed";
    REQUIRE(restarted.SaveProfile(2, changed).HasValue());
    CHECK(store.SaveProfile(2, Profile()).HasError());
    CHECK(Read(project).find("Changed") != std::string::npos);
    REQUIRE(store.Reload().HasValue());
    CHECK(store.SaveProfile(2, Profile()).HasError());
    REQUIRE(store.SaveOverride(2, {1, 0}).HasValue());
    CHECK(store.User().overrides.empty());
    const auto priorProject = store.Project();
    const auto priorUser = store.User();
    Write(user, "{}");
    CHECK(store.Reload().HasError());
    CHECK(store.Project() == priorProject);
    CHECK(store.User() == priorUser);
    CHECK(store.SaveOverride(3, {1, 8100}).HasError());
    CHECK(Read(user) == "{}");
}

TEST_CASE("Play topology rejects unsafe storage and invalid drafts before publication", "[network][play_topology][storage]") {
    Directory directory;
    NativeDurableFileSystem files;
    const auto project = directory.root / "project.json";
    const auto user = directory.root / "user.json";
    PlayTopologyStore store(files, project, user);
    CHECK(store.SaveProfile(1, Profile()).HasError());
    REQUIRE(store.Reload().HasValue());
    auto invalid = Profile();
    invalid.map = "/private/main.horo";
    CHECK(store.SaveProfile(1, invalid).HasError());
    CHECK_FALSE(std::filesystem::exists(project));
    CHECK(store.SaveOverride(1, {1, 7000}).HasError());
    CHECK_FALSE(std::filesystem::exists(user));
    std::filesystem::create_directory(project);
    CHECK(store.Reload().HasError());
    PlayTopologyStore aliased(files, user, user);
    CHECK(aliased.Reload().HasError());
}

TEST_CASE("Play topology can clear an obsolete machine override after switching to standalone", "[network][play_topology][storage]") {
    Directory directory;
    NativeDurableFileSystem files;
    PlayTopologyStore store(files, directory.root / "project.json", directory.root / "user.json");
    REQUIRE(store.Reload().HasValue());
    REQUIRE(store.SaveProfile(1, Profile()).HasValue());
    REQUIRE(store.SaveOverride(1, {1, 8000}).HasValue());
    REQUIRE(store.SaveProfile(2, Profile(PlayTopologyKind::Standalone)).HasValue());
    CHECK(PreflightPlayTopology(store.Project(), store.User(), 1, Capabilities()).HasError());
    CHECK(store.SaveOverride(2, {1, 8100}).HasError());
    REQUIRE(store.SaveOverride(2, {1, 0}).HasValue());
    CHECK(PreflightPlayTopology(store.Project(), store.User(), 1, Capabilities()).HasValue());
}
