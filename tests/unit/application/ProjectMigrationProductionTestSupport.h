#pragma once

#include "Horo/Application/ProjectMigrationCatalog.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>

namespace project_migration_tests {
    [[nodiscard]] inline Horo::Application::ProjectMigrationDefinition ProductionCompressionDefinition() {
        const auto catalog = Horo::Application::BuildBuiltInProjectMigrationCatalog();
        INFO((catalog.HasError() ? catalog.ErrorValue().code.Value() + ": " + catalog.ErrorValue().message : std::string{}));
        REQUIRE((catalog.HasValue()));
        const auto definition = std::ranges::find_if(catalog.Value(), [](const auto &entry) {
            return entry.id.value == "core.project_settings.compression_defaults";
        });
        REQUIRE((definition != catalog.Value().end()));
        return *definition;
    }
}  // namespace project_migration_tests
