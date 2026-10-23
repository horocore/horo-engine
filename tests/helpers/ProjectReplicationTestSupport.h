#pragma once

#include "Horo/Application/ProjectCompatibility.h"
#include "Horo/Network/NetworkProjectSettings.h"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>

namespace Horo::Tests {
    /** @brief Authors an actual current canonical project document; absent policy remains absent. */
    inline void WriteReplicationProject(const std::filesystem::path &root,
                                        const std::optional<Network::NetworkReplicationInventory> &inventory = {}) {
        const auto release = Application::CurrentEngineReleaseVersion();
        const auto *decision = Application::BuiltInReleaseCompatibilityRegistry().Find(release);
        REQUIRE(decision);
        std::filesystem::create_directories(root / ".horo");
        std::ofstream stream{root / ".horo/project.json", std::ios::binary | std::ios::trunc};
        stream << "{\"horoVersion\":\"" << Application::FormatHoroVersion(release.value) << "\",\"persistentContract\":\""
               << Application::FormatPersistentContractHash(decision->persistentContract)
               << "\",\"projectId\":\"replication-test\",\"name\":\"Replication\",\"projectVersion\":\"0.1.0\","
                  "\"createdAt\":\"2026-10-22T00:00:00Z\",\"settings\":{\"renderBackend\":\"opengl\"";
        if (inventory) {
            auto defaults = Network::DefaultNetworkProjectSettings(Network::NetworkProjectSettingsId::Create(1).Value());
            REQUIRE(defaults.HasValue());
            auto input = std::move(defaults).Value();
            input.replication = *inventory;
            auto settings = Network::NetworkProjectSettings::Create(input);
            REQUIRE(settings.HasValue());
            stream << ",\"network\":" << Network::SerializeNetworkProjectSettings(settings.Value());
        }
        stream << "}}";
        stream.close();
        REQUIRE(stream.good());
    }
}  // namespace Horo::Tests
