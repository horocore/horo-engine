#include "../../support/OwnedTestDirectory.h"
#include "../../support/project/ProjectMigrationFailingFilesystem.h"
#include "Horo/Editor/ProjectMutation.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <new>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using Horo::Tests::MigrationFailure::FailingFilesystem;
    using Horo::Tests::OwnedTestDirectory;
}  // namespace

TEST_CASE("Mutation admission rolls back when native acquisition throws", "[unit][editor][mutation]") {
    using enum ProjectMutationOwner;
    OwnedTestDirectory project{"mutation admission \xC3\xA7"};
    FailingFilesystem files(project.Path());
    ProjectMutationCoordinator coordinator(files);
    bool nativeAcquired = false;
    files.onExclusiveAcquired = [&nativeAcquired] {
        nativeAcquired = true;
        throw std::bad_alloc{};
    };
    REQUIRE_THROWS_AS((coordinator.TryAcquire({project.Path(), Save, "throw-after-native-acquire"})), std::bad_alloc);
    REQUIRE(nativeAcquired);
    REQUIRE_FALSE(files.onExclusiveAcquired);
    {
        NativeDurableFileSystem native;
        const auto probe = native.TryAcquireExclusive(project.Path() / ".horo/local/project-mutation.lock", "native-release-probe");
        REQUIRE(probe.HasValue());
    }
    const auto retried = coordinator.TryAcquire({project.Path(), Save, "retry-after-exception"});
    REQUIRE(retried.HasValue());
    const auto competing = coordinator.TryAcquire({project.Path(), Save, "while-retry-held"});
    REQUIRE(competing.HasError());
    REQUIRE(competing.ErrorValue().code.Value() == "project.migration.locked");
}

TEST_CASE("Mutation admission retains held owners and rolls back native busy results", "[unit][editor][mutation]") {
    using enum ProjectMutationOwner;
    OwnedTestDirectory project{"mutation busy \xC3\xA7"};
    FailingFilesystem files(project.Path());
    ProjectMutationCoordinator coordinator(files);
    {
        NativeDurableFileSystem native;
        const auto held = native.TryAcquireExclusive(project.Path() / ".horo/local/project-mutation.lock", "independent-native-owner");
        REQUIRE(held.HasValue());
        const auto busy = coordinator.TryAcquire({project.Path(), Save, "native-busy"});
        REQUIRE(busy.HasError());
        REQUIRE(busy.ErrorValue().code.Value() == "project.migration.locked");
    }
    {
        const auto retained = coordinator.TryAcquire({project.Path(), Save, "after-native-release"});
        REQUIRE(retained.HasValue());
        const auto first = coordinator.TryAcquire({project.Path(), Save, "first-contender"});
        REQUIRE(first.HasError());
        const auto second = coordinator.TryAcquire({project.Path(), Save, "second-contender"});
        REQUIRE(second.HasError());
    }
    const auto afterRelease = coordinator.TryAcquire({project.Path(), Save, "after-owned-release"});
    REQUIRE(afterRelease.HasValue());
}

TEST_CASE("Migration directory sync consumes a throwing injection exactly once", "[unit][editor][mutation]") {
    OwnedTestDirectory project{"mutation sync failure"};
    FailingFilesystem files(project.Path());
    std::size_t calls{};
    files.onSyncDirectory = [&calls] {
        ++calls;
        throw std::bad_alloc{};
    };
    REQUIRE_THROWS_AS(files.SyncDirectory(project.Path()), std::bad_alloc);
    REQUIRE(calls == 1);
    REQUIRE(files.SyncDirectory(project.Path()).HasValue());
    REQUIRE(calls == 1);
}
