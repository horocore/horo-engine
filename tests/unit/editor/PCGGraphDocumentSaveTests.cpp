#include "PCGDocumentTestSupport.h"

namespace Horo::Editor {
    using namespace PCGDocumentTestSupport;

    TEST_CASE("PCG source lease exceptions preserve source and document", "[unit][editor][pcg][failure]") {
        Project project;
        auto document = project.Open();
        const auto accepted = Capture();
        REQUIRE(document.Execute(Edit(document.Snapshot().Value(), 12)).HasValue());
        auto save = document.CaptureSave(Fence(document.Snapshot().Value()));
        REQUIRE(save.HasValue());
        const auto before = document.Snapshot().Value();
        SECTION("standard lease exception preserves the source and document") {
            project.files.beforeLock = [] {
                throw std::runtime_error("lease acquisition");
            };
            ErrorIs(document.Save(save.Value(), project.root, project.path, project.mutations, project.files),
                    Assets::AssetSourceErrors::ReadFailed);
            CHECK(Assets::CaptureAssetSource(project.root, project.path, 1024 * 1024).Value().bytes == accepted.bytes);
        }
        SECTION("foreign lease exception preserves the source and document") {
            project.files.beforeLock = [] {
                throw 42;
            };
            ErrorIs(document.Save(save.Value(), project.root, project.path, project.mutations, project.files),
                    Assets::AssetSourceErrors::ReadFailed);
            CHECK(Assets::CaptureAssetSource(project.root, project.path, 1024 * 1024).Value().bytes == accepted.bytes);
        }
        CHECK(document.Snapshot().Value().source == before.source);
        CHECK(document.Snapshot().Value().acceptedSource == before.acceptedSource);
        CHECK(document.Snapshot().Value().dirty);
        CHECK(document.UndoCount() == 1);
        CHECK_FALSE(std::filesystem::exists(project.path.string() + ".source-publication.tmp"));
    }

    TEST_CASE("PCG precommit write replacement and provider failures preserve document owners", "[unit][editor][pcg][failure]") {
        Project project;
        auto document = project.Open();
        const auto accepted = Capture();
        REQUIRE(document.Execute(Edit(document.Snapshot().Value(), 12)).HasValue());
        auto save = document.CaptureSave(Fence(document.Snapshot().Value()));
        REQUIRE(save.HasValue());
        const auto before = document.Snapshot().Value();
        SECTION("stage write failure") {
            project.files.failWrite = true;
            REQUIRE(document.Save(save.Value(), project.root, project.path, project.mutations, project.files).HasError());
        }
        SECTION("replace failure") {
            project.files.failReplace = true;
            REQUIRE(document.Save(save.Value(), project.root, project.path, project.mutations, project.files).HasError());
        }
        SECTION("standard exception after private creation rolls back owned staging") {
            project.files.afterCreation = [](bool &created) {
                CHECK(created);
                throw std::runtime_error("after private creation");
            };
            ErrorIs(document.Save(save.Value(), project.root, project.path, project.mutations, project.files),
                    Assets::AssetSourceErrors::ReadFailed);
            CHECK(Assets::CaptureAssetSource(project.root, project.path, 1024 * 1024).Value().bytes == accepted.bytes);
        }
        SECTION("foreign exception before replacement rolls back owned staging") {
            project.files.beforeReplacement = [] {
                throw 42;
            };
            ErrorIs(document.Save(save.Value(), project.root, project.path, project.mutations, project.files),
                    Assets::AssetSourceErrors::ReadFailed);
            CHECK(Assets::CaptureAssetSource(project.root, project.path, 1024 * 1024).Value().bytes == accepted.bytes);
        }
        CHECK(document.Snapshot().Value().source == before.source);
        CHECK(document.Snapshot().Value().acceptedSource == before.acceptedSource);
        CHECK(document.Snapshot().Value().dirty);
        CHECK(document.UndoCount() == 1);
        CHECK_FALSE(std::filesystem::exists(project.path.string() + ".source-publication.tmp"));
    }

    TEST_CASE("PCG allocation and untracked publication failures reject concurrent source edits", "[unit][editor][pcg][failure]") {
        Project project;
        auto document = project.Open();
        const auto accepted = Capture();
        REQUIRE(document.Execute(Edit(document.Snapshot().Value(), 12)).HasValue());
        auto save = document.CaptureSave(Fence(document.Snapshot().Value()));
        REQUIRE(save.HasValue());
        const auto before = document.Snapshot().Value();
        SECTION("allocation failure after private creation rolls back owned staging") {
            project.files.afterCreation = [](bool &) {
                throw std::bad_alloc();
            };
            ErrorIs(document.Save(save.Value(), project.root, project.path, project.mutations, project.files),
                    Assets::AssetSourceErrors::Limit);
            CHECK(Assets::CaptureAssetSource(project.root, project.path, 1024 * 1024).Value().bytes == accepted.bytes);
        }
        SECTION("success without a native commit receipt is rejected") {
            project.files.untrackedSuccess = true;
            REQUIRE(document.Save(save.Value(), project.root, project.path, project.mutations, project.files).HasError());
            CHECK(Assets::CaptureAssetSource(project.root, project.path, 1024 * 1024).Value().bytes == accepted.bytes);
        }
        SECTION("source already newer") {
            project.Write(Capture(Graph(7, 99)));
            ErrorIs(document.Save(save.Value(), project.root, project.path, project.mutations, project.files),
                    Assets::AssetSourceErrors::Conflict);
            CHECK(Assets::CaptureAssetSource(project.root, project.path, 1024 * 1024).Value().bytes == Capture(Graph(7, 99)).bytes);
        }
        SECTION("external source changes during staging") {
            project.files.duringWrite = [&] {
                project.Write(Capture(Graph(7, 99)));
            };
            ErrorIs(document.Save(save.Value(), project.root, project.path, project.mutations, project.files),
                    Assets::AssetSourceErrors::Conflict);
            CHECK(project.files.replacements == 0);
            CHECK(Assets::CaptureAssetSource(project.root, project.path, 1024 * 1024).Value().bytes == Capture(Graph(7, 99)).bytes);
        }
        CHECK(document.Snapshot().Value().source == before.source);
        CHECK(document.Snapshot().Value().acceptedSource == before.acceptedSource);
        CHECK(document.Snapshot().Value().dirty);
        CHECK(document.UndoCount() == 1);
        CHECK_FALSE(std::filesystem::exists(project.path.string() + ".source-publication.tmp"));
    }

    TEST_CASE("PCG visible replacement failures retain truthful reconciliation and dirty owners", "[unit][editor][pcg][failure]") {
        Project project;
        auto document = project.Open();
        const auto accepted = Capture();
        REQUIRE(document.Execute(Edit(document.Snapshot().Value(), 12)).HasValue());
        auto save = document.CaptureSave(Fence(document.Snapshot().Value()));
        REQUIRE(save.HasValue());
        const auto before = document.Snapshot().Value();
        SECTION("replacement committed but sync failed") {
            project.files.failAfterCommit = true;
            auto result = document.Save(save.Value(), project.root, project.path, project.mutations, project.files);
            REQUIRE(result.HasValue());
            CHECK(result.Value().status == Assets::AssetSourcePublicationStatus::ReconciliationRequired);
            CHECK(result.Value().durabilityError.has_value());
            CHECK(document.Snapshot().Value().externalConflict);
            CHECK(Assets::CaptureAssetSource(project.root, project.path, 1024 * 1024).Value().bytes != accepted.bytes);
        }
        SECTION("standard exception after visible replacement") {
            project.files.afterReplacement = [] {
                throw std::runtime_error("after commit");
            };
            const auto result = document.Save(save.Value(), project.root, project.path, project.mutations, project.files);
            REQUIRE(result.HasValue());
            CHECK(result.Value().status == Assets::AssetSourcePublicationStatus::ReconciliationRequired);
            CHECK(result.Value().durabilityError.has_value());
            CHECK(document.Snapshot().Value().externalConflict);
            CHECK(Assets::CaptureAssetSource(project.root, project.path, 1024 * 1024).Value().bytes != accepted.bytes);
        }
        SECTION("foreign exception after visible replacement") {
            project.files.afterReplacement = [] {
                throw 42;
            };
            const auto result = document.Save(save.Value(), project.root, project.path, project.mutations, project.files);
            REQUIRE(result.HasValue());
            CHECK(result.Value().status == Assets::AssetSourcePublicationStatus::ReconciliationRequired);
            CHECK(result.Value().durabilityError.has_value());
            CHECK(document.Snapshot().Value().externalConflict);
            CHECK(Assets::CaptureAssetSource(project.root, project.path, 1024 * 1024).Value().bytes != accepted.bytes);
        }
        CHECK(document.Snapshot().Value().source == before.source);
        CHECK(document.Snapshot().Value().acceptedSource == before.acceptedSource);
        CHECK(document.Snapshot().Value().dirty);
        CHECK(document.UndoCount() == 1);
        CHECK_FALSE(std::filesystem::exists(project.path.string() + ".source-publication.tmp"));
    }

    TEST_CASE("PCG rollback cleanup exceptions preserve the primary precommit failure", "[unit][editor][pcg][failure]") {
        Project project;
        auto document = project.Open();
        REQUIRE(document.Execute(Edit(document.Snapshot().Value(), 12)).HasValue());
        const auto save = document.CaptureSave(Fence(document.Snapshot().Value()));
        REQUIRE(save.HasValue());
        project.files.failReplace = true;
        project.files.duringRemove = [] {
            throw 42;
        };
        ErrorIs(document.Save(save.Value(), project.root, project.path, project.mutations, project.files), Injected);
        CHECK(document.Snapshot().Value().dirty);
        CHECK(Assets::CaptureAssetSource(project.root, project.path, 1024 * 1024).Value().bytes == Capture().bytes);
        CHECK(std::filesystem::exists(project.path.string() + ".source-publication.tmp"));
    }

    TEST_CASE("Shared source capture distinguishes empty missing bounded and cancelled inputs", "[unit][editor][pcg][assets]") {
        Project project;
        const auto accepted = Capture();
        SECTION("missing and empty source remain distinct") {
            const auto missing = Assets::CaptureAssetSource(project.root, project.root / "assets/missing.hpcg", 1024);
            REQUIRE(missing.HasValue());
            CHECK_FALSE(missing.Value().identity.exists);
            const auto emptyPath = project.root / "assets/empty.hpcg";
            {
                std::ofstream file(emptyPath, std::ios::binary);
            }
            const auto empty = Assets::CaptureAssetSource(project.root, emptyPath, 1024);
            REQUIRE(empty.HasValue());
            CHECK(empty.Value().identity.exists);
            CHECK(empty.Value().bytes.empty());
            CHECK(empty.Value().identity != missing.Value().identity);
        }
        SECTION("bounded and cancelled capture") {
            ErrorIs(Assets::CaptureAssetSource(project.root, project.path, 1), Assets::AssetSourceErrors::Limit);
            CancellationSource cancellation;
            cancellation.RequestCancellation();
            ErrorIs(Assets::CaptureAssetSource(project.root, project.path, 1024 * 1024, cancellation.Token()),
                    Assets::AssetSourceErrors::Cancelled);
        }
        CHECK(Assets::CaptureAssetSource(project.root, project.path, 1024 * 1024).Value().bytes == accepted.bytes);
    }

    TEST_CASE("Shared source publication rejects wrong native leases and symlink paths", "[unit][editor][pcg][assets]") {
        Project project;
        const auto accepted = Capture();
        SECTION("different native source lease") {
            const auto otherPath = project.root / "assets/other.hpcg";
            auto wrong =
                project.files.TryAcquireExclusive(Assets::AssetSourcePublicationLockPath(project.root, otherPath).Value(), "wrong");
            REQUIRE(wrong.HasValue());
            ErrorIs(Assets::PublishAssetSource(project.root, project.path, accepted.identity, accepted.bytes, wrong.Value(), project.files),
                    Assets::AssetSourceErrors::Authority);
        }
        SECTION("internal symlink retarget is not a canonical source") {
            const auto alias = project.root / "assets/alias.hpcg";
            std::error_code error;
            std::filesystem::create_symlink(project.path, alias, error);
            if (error)
                SKIP("Host does not permit symlink creation.");
            ErrorIs(Assets::CaptureAssetSource(project.root, alias, 1024 * 1024), Assets::AssetSourceErrors::InvalidPath);
        }
        CHECK(Assets::CaptureAssetSource(project.root, project.path, 1024 * 1024).Value().bytes == accepted.bytes);
    }

    TEST_CASE("Shared source publication preserves foreign staging collisions", "[unit][editor][pcg][assets]") {
        Project project;
        const auto accepted = Capture();
        SECTION("existing staging file is preserved") {
            const auto stage = project.path.string() + ".source-publication.tmp";
            {
                std::ofstream file(stage, std::ios::binary);
                file << "previous recovery evidence";
            }
            auto document = project.Open();
            auto save = document.CaptureSave(Fence(document.Snapshot().Value()));
            REQUIRE(save.HasValue());
            ErrorIs(document.Save(save.Value(), project.root, project.path, project.mutations, project.files),
                    Assets::AssetSourceErrors::Conflict);
            CHECK(std::filesystem::file_size(stage) == 26);
            CHECK(project.files.writes == 0);
        }
        SECTION("staging appears immediately before exclusive creation") {
            const auto stage = project.path.string() + ".source-publication.tmp";
            project.files.duringWrite = [&] {
                std::ofstream file(stage, std::ios::binary);
                file << "foreign stage";
            };
            auto document = project.Open();
            auto save = document.CaptureSave(Fence(document.Snapshot().Value()));
            REQUIRE(save.HasValue());
            REQUIRE(document.Save(save.Value(), project.root, project.path, project.mutations, project.files).HasError());
            CHECK(std::filesystem::file_size(stage) == 13);
            CHECK(project.files.replacements == 0);
            CHECK(Assets::CaptureAssetSource(project.root, project.path, 1024 * 1024).Value().bytes == accepted.bytes);
        }
        CHECK(Assets::CaptureAssetSource(project.root, project.path, 1024 * 1024).Value().bytes == accepted.bytes);
    }

    TEST_CASE("Shared source publication rolls back owned cancelled and substituted staging", "[unit][editor][pcg][assets]") {
        Project project;
        const auto accepted = Capture();
        SECTION("cancellation during staging rolls back only the owned file") {
            CancellationSource cancellation;
            project.files.duringWrite = [&] {
                cancellation.RequestCancellation();
            };
            auto document = project.Open();
            auto save = document.CaptureSave(Fence(document.Snapshot().Value()));
            REQUIRE(save.HasValue());
            ErrorIs(document.Save(save.Value(), project.root, project.path, project.mutations, project.files, cancellation.Token()),
                    Assets::AssetSourceErrors::Cancelled);
            CHECK_FALSE(std::filesystem::exists(project.path.string() + ".source-publication.tmp"));
            CHECK(project.files.replacements == 0);
            CHECK(Assets::CaptureAssetSource(project.root, project.path, 1024 * 1024).Value().bytes == accepted.bytes);
        }
        SECTION("staged bytes changed after durable write") {
            project.files.afterWrite = [](const auto &path) {
                std::ofstream file(path, std::ios::binary | std::ios::trunc);
                file << "substituted";
            };
            auto document = project.Open();
            auto save = document.CaptureSave(Fence(document.Snapshot().Value()));
            REQUIRE(save.HasValue());
            ErrorIs(document.Save(save.Value(), project.root, project.path, project.mutations, project.files),
                    Assets::AssetSourceErrors::Conflict);
            CHECK(project.files.replacements == 0);
        }
        CHECK(Assets::CaptureAssetSource(project.root, project.path, 1024 * 1024).Value().bytes == accepted.bytes);
    }

}  // namespace Horo::Editor
