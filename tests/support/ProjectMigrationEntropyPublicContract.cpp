#include <Horo/Application/ProjectMigration.h>
#include <Horo/Application/ProjectMigrationErrors.h>
#include <concepts>

namespace {
    using namespace Horo;
    using namespace Horo::Application;
    static_assert(requires(const std::filesystem::path &source, const std::filesystem::path &candidate, const ProjectMigrationPlan &plan,
                           JobSystem &jobs, const ProjectMigrationExecutionServices &services) {
        {
            ProjectMigrationExecutor::Prepare(source, candidate, plan, jobs, {}, {}, services)
        } -> std::same_as<Result<PreparedProjectMigration>>;
        {
            ProjectMigrationExecutor::VerifiedDryRun(source, plan, jobs, {}, {}, services)
        } -> std::same_as<Result<ProjectMigrationDryRunResult>>;
    });
}  // namespace
