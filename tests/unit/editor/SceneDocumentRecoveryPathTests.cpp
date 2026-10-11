#include "SceneDocumentPersistenceTestSupport.h"
#include "editor/document/SceneDocumentPersistence.h"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <system_error>

using namespace Horo;
using namespace Horo::Editor;
using namespace Horo::Editor::PersistenceTestSupport;

TEST_CASE("Scene Recovery Canonicalizes Project Aliases Without Mutating Canonical Scene", "[unit][editor][persistence][recovery]") {
    TemporaryProject project;
    project.PrepareEmptyScene();
    const auto canonicalBefore = InspectProjectSceneFingerprint(project.Root(), project.ScenePath());
    REQUIRE(canonicalBefore.HasValue());
    const auto aliasRoot = project.Root() / "project-alias";
    std::error_code error;
    std::filesystem::create_directory_symlink(project.Root(), aliasRoot, error);
    if (error)
        SKIP("Host does not permit project directory aliases.");
    const auto aliasScene = aliasRoot / project.ScenePath().lexically_relative(project.Root());
    NativeDurableFileSystem files;
    ProjectMutationCoordinator mutations(files);
    REQUIRE(WriteProjectSceneRecovery(aliasRoot, aliasScene, AuthoredScene(), DocumentRevision{}, DocumentStateId{1}, mutations, files)
                .HasValue());
    const auto recovery = InspectProjectSceneRecovery(aliasRoot, aliasScene);
    REQUIRE(recovery.HasValue());
    REQUIRE(recovery.Value());
    CHECK(std::filesystem::exists(project.RecoveryPath()));
    const auto canonicalAfter = InspectProjectSceneFingerprint(project.Root(), project.ScenePath());
    REQUIRE(canonicalAfter.HasValue());
    CHECK(canonicalAfter.Value() == canonicalBefore.Value());
}

TEST_CASE("Scene Recovery Rejects Missing Project Roots Without Creating Files", "[unit][editor][persistence][recovery]") {
    TemporaryProject project;
    project.PrepareEmptyScene();
    const auto missingRoot = project.Root() / "missing-project";
    NativeDurableFileSystem files;
    ProjectMutationCoordinator mutations(files);
    const auto recovery = WriteProjectSceneRecovery(missingRoot, missingRoot / "scene.horo", AuthoredScene(), DocumentRevision{},
                                                    DocumentStateId{1}, mutations, files);
    REQUIRE(recovery.HasError());
    CHECK_FALSE(recovery.ErrorValue().message.empty());
    CHECK_FALSE(std::filesystem::exists(missingRoot));
    CHECK_FALSE(std::filesystem::exists(project.RecoveryPath()));
}
