#include "ProjectMigrationProductionTestSupport.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("Built In Catalog Contains Compression Defaults Migration", "[unit][application]") {
    const auto definition = project_migration_tests::ProductionCompressionDefinition();
    REQUIRE((Horo::Application::FormatHoroVersion(definition.from.value) == "0.0.1"));
    REQUIRE((Horo::Application::FormatHoroVersion(definition.to.value) == "0.1.0"));

    const auto support = Horo::Application::BuildBuiltInProjectMigrationSupportDescriptor();
    REQUIRE((support.HasValue()));
    REQUIRE((Horo::Application::FormatHoroVersion(support.Value().target.value) == "0.3.0"));
    REQUIRE((Horo::Application::FormatHoroVersion(support.Value().minimumMigratable.value) == "0.0.1"));
    REQUIRE((support.Value().targetValidator != nullptr));
}

TEST_CASE("Built in surface catalog adoption preserves the exact predecessor contract", "[unit][application][surfaces]") {
    using namespace Horo::Application;
    const auto catalog = BuildBuiltInProjectMigrationCatalog();
    REQUIRE(catalog.HasValue());
    const auto surface = std::ranges::find_if(catalog.Value(), [](const ProjectMigrationDefinition &definition) {
        return FormatHoroVersion(definition.to.value) == "0.3.0";
    });
    REQUIRE(surface != catalog.Value().end());
    REQUIRE(FormatHoroVersion(surface->from.value) == "0.2.0");
    REQUIRE(FormatPersistentContractHash(surface->sourceContract) ==
            "sha256:e068efe802defac5ec18d9742f0b1e1c50b1bfda5b26f8fc4a3a02eaf478065c");
    const auto support = BuildBuiltInProjectMigrationSupportDescriptor();
    REQUIRE(support.HasValue());
    REQUIRE(surface->targetContract == support.Value().targetContract);
    const auto registry = ProjectMigrationRegistry::Create(catalog.Value());
    REQUIRE(registry.HasValue());
    const auto plan = registry.Value().Plan(surface->from, surface->sourceContract, support.Value());
    REQUIRE(plan.HasValue());
    REQUIRE(plan.Value().definitions.size() == 1);
    REQUIRE(plan.Value().targetValidator != nullptr);
}
