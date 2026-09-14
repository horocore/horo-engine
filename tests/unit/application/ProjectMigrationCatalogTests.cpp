#include "ProjectMigrationProductionTestSupport.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Built In Catalog Contains Compression Defaults Migration", "[unit][application]") {
    const auto definition = project_migration_tests::ProductionCompressionDefinition();
    REQUIRE((Horo::Application::FormatHoroVersion(definition.from.value) == "0.0.1"));
    REQUIRE((Horo::Application::FormatHoroVersion(definition.to.value) == "0.1.0"));

    const auto support = Horo::Application::BuildBuiltInProjectMigrationSupportDescriptor();
    REQUIRE((support.HasValue()));
    REQUIRE((Horo::Application::FormatHoroVersion(support.Value().target.value) == "0.1.0"));
    REQUIRE((Horo::Application::FormatHoroVersion(support.Value().minimumMigratable.value) == "0.0.1"));
    REQUIRE((support.Value().targetValidator != nullptr));
}
