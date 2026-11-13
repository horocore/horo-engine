#include "../../support/OwnedTestDirectory.h"
#include "../../support/project/ProjectMigrationFailingFilesystem.h"
#include "Horo/Editor/ProjectMutation.h"

#include <catch2/catch_test_macros.hpp>
#include <new>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using Horo::Tests::MigrationFailure::FailingFilesystem;
    using Horo::Tests::OwnedTestDirectory;
}  // namespace

TEST_CASE("Mutation admission rolls back when native acquisition throws", "[unit][editor][mutation]") {
    OwnedTestDirectory project{"mutation admission \xC3\xA7"};
    FailingFilesystem files(project.Path());
    ProjectMutationCoordinator coordinator(files);
    bool nativeAcquired = false;
    files.onExclusiveAcquired = [&] {
        nativeAcquired = true;
        throw std::bad_alloc{};
    };
    REQUIRE_THROWS_AS((coordinator.TryAcquire({project.Path(), ProjectMutationOwner::Save, "throw-after-native-acquire"})), std::bad_alloc);
    REQUIRE(nativeAcquired);
    {
        NativeDurableFileSystem native;
        const auto probe = native.TryAcquireExclusive(project.Path() / ".horo/local/project-mutation.lock", "native-release-probe");
        REQUIRE(probe.HasValue());
    }
    const auto retried = coordinator.TryAcquire({project.Path(), ProjectMutationOwner::Save, "retry-after-exception"});
    REQUIRE(retried.HasValue());
    const auto competing = coordinator.TryAcquire({project.Path(), ProjectMutationOwner::Save, "while-retry-held"});
    REQUIRE(competing.HasError());
    REQUIRE(competing.ErrorValue().code.Value() == "project.migration.locked");
}

TEST_CASE("Mutation admission retains held owners and rolls back native busy results", "[unit][editor][mutation]") {
    OwnedTestDirectory project{"mutation busy \xC3\xA7"};
    FailingFilesystem files(project.Path());
    ProjectMutationCoordinator coordinator(files);
    {
        NativeDurableFileSystem native;
        const auto held = native.TryAcquireExclusive(project.Path() / ".horo/local/project-mutation.lock", "independent-native-owner");
        REQUIRE(held.HasValue());
        const auto busy = coordinator.TryAcquire({project.Path(), ProjectMutationOwner::Save, "native-busy"});
        REQUIRE(busy.HasError());
        REQUIRE(busy.ErrorValue().code.Value() == "project.migration.locked");
    }
    {
        const auto retained = coordinator.TryAcquire({project.Path(), ProjectMutationOwner::Save, "after-native-release"});
        REQUIRE(retained.HasValue());
        const auto first = coordinator.TryAcquire({project.Path(), ProjectMutationOwner::Save, "first-contender"});
        REQUIRE(first.HasError());
        const auto second = coordinator.TryAcquire({project.Path(), ProjectMutationOwner::Save, "second-contender"});
        REQUIRE(second.HasError());
    }
    const auto afterRelease = coordinator.TryAcquire({project.Path(), ProjectMutationOwner::Save, "after-owned-release"});
    REQUIRE(afterRelease.HasValue());
}
