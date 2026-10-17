#include "../../support/NativePublicationFiles.h"
#include "../../support/OwnedTestDirectory.h"
#include "Horo/Editor/WorkspaceLayoutPersistence.h"
#include "Horo/Platform/PlatformErrors.h"

#include <catch2/catch_test_macros.hpp>
#include <fstream>

namespace Horo::Editor::Tests {
    namespace {
        struct CleanupFailure final {};

        struct PublicationFailure final {};

        /** @brief Fault injection around the real native commit, retaining the exact committed receipt. */
        class PublicationFiles final : public TestSupport::NativePublicationFiles {
        public:
            void FailBeforeCommit(const bool fail) noexcept {
                failBefore_ = fail;
            }

            void FailAfterCommit(const bool fail) noexcept {
                failAfter_ = fail;
            }

            void ThrowOnCleanup() noexcept {
                throwCleanup_ = true;
            }

            void ThrowOnReplacement() noexcept {
                throwReplacement_ = true;
            }

            [[nodiscard]] std::size_t CleanupCalls() const noexcept {
                return cleanupCalls_;
            }

            void ObserveWriterLease(const std::filesystem::path &destination) {
                writerLock_ = destination;
                writerLock_ += ".lock";
            }

            [[nodiscard]] std::size_t WriterChecks() const noexcept {
                return writerChecks_;
            }

            [[nodiscard]] std::size_t UnexpectedWriterChecks() const noexcept {
                return unexpectedWriterChecks_;
            }

            Result<void> RemoveDurable(const std::filesystem::path &path) override {
                CheckWriterLease();
                ++cleanupCalls_;
                if (throwCleanup_)
                    throw CleanupFailure{};
                return native.RemoveDurable(path);
            }

            Result<void> WritePrivateDurable(const std::filesystem::path &path, std::span<const std::byte> bytes, bool &created,
                                             std::filesystem::perms permissions) override {
                CheckWriterLease();
                return native.WritePrivateDurable(path, bytes, created, permissions);
            }

            Result<void> AtomicReplaceTracked(const std::filesystem::path &stage, const std::filesystem::path &destination,
                                              AtomicFileReplacementReceipt &receipt) override {
                CheckWriterLease();
                if (throwReplacement_)
                    throw PublicationFailure{};
                if (failBefore_)
                    return Result<void>::Failure(MakeError(PlatformErrors::ConfigurationWriteFailed));
                auto replaced = native.AtomicReplaceTracked(stage, destination, receipt);
                if (replaced.HasError())
                    return replaced;
                if (failAfter_)
                    return Result<void>::Failure(MakeError(PlatformErrors::ConfigurationWriteFailed));
                return replaced;
            }

        private:
            bool failBefore_{};
            bool failAfter_{};
            bool throwCleanup_{};
            bool throwReplacement_{};
            std::size_t cleanupCalls_{};
            std::filesystem::path writerLock_;
            std::size_t writerChecks_{};
            std::size_t unexpectedWriterChecks_{};

            void CheckWriterLease() {
                if (writerLock_.empty())
                    return;
                ++writerChecks_;
                const auto competing = native.TryAcquireExclusive(writerLock_, "workspace-test-observer");
                if (competing.HasValue() || competing.ErrorValue().domain.Value() != "horo.platform.filesystem" ||
                    competing.ErrorValue().code.Value() != "filesystem.lock_busy")
                    ++unexpectedWriterChecks_;
            }
        };

        /** @brief Creates a valid workspace whose opaque zero bytes must survive provider absence. */
        WorkspaceLayout Layout() {
            WorkspaceLayout layout;
            layout.root = LayoutNode{PanelNode{"root", "horo.scene"}};
            layout.surfaces.push_back({"com.example.inspector", "com.example", "com.example.editor", true, false, true, {0, 255, 3}, {}});
            return layout;
        }
    }  // namespace

    TEST_CASE("Workspace publication failures preserve the previous document and distinguish uncertain commit",
              "[unit][editor][persistence][extension-recovery]") {
        Horo::Tests::OwnedTestDirectory directory{"workspace-recovery"};
        const auto path = directory.Path() / "çalışma alanı.json";
        PublicationFiles files;
        auto layout = Layout();
        AtomicFileReplacementReceipt initial;
        REQUIRE(WorkspaceLayoutPersistence::Save(files, initial, path, layout));
        REQUIRE(initial.WasCommitted());
        auto candidate = layout;
        candidate.surfaces.front().state = {6, 7};
        files.FailBeforeCommit(true);
        AtomicFileReplacementReceipt before;
        REQUIRE_FALSE(WorkspaceLayoutPersistence::Save(files, before, path, candidate));
        CHECK_FALSE(before.WasCommitted());
        REQUIRE(WorkspaceLayoutPersistence::Load(path));
        CHECK(WorkspaceLayoutPersistence::Load(path)->surfaces == layout.surfaces);
        files.FailBeforeCommit(false);
        files.FailAfterCommit(true);
        AtomicFileReplacementReceipt after;
        std::string error;
        REQUIRE_FALSE(WorkspaceLayoutPersistence::Save(files, after, path, candidate, &error));
        CHECK(after.WasCommitted());
        CHECK(error.find("uncertain") != std::string::npos);
        CHECK(WorkspaceLayoutPersistence::Load(path)->surfaces == candidate.surfaces);
    }

    TEST_CASE("Invalid workspace and foreign staging files cannot replace or corrupt durable provider intent",
              "[unit][editor][persistence][extension-recovery]") {
        Horo::Tests::OwnedTestDirectory directory{"workspace-staging"};
        const auto path = directory.Path() / "workspace.json";
        PublicationFiles files;
        auto layout = Layout();
        AtomicFileReplacementReceipt initial;
        REQUIRE(WorkspaceLayoutPersistence::Save(files, initial, path, layout));
        auto invalid = layout;
        invalid.surfaces.push_back(invalid.surfaces.front());
        AtomicFileReplacementReceipt rejected;
        CHECK_FALSE(WorkspaceLayoutPersistence::Save(files, rejected, path, invalid));
        auto stage = path;
        stage += ".pending";
        {
            std::ofstream sentinel(stage);
            sentinel << "foreign stage";
        }
        AtomicFileReplacementReceipt collision;
        CHECK_FALSE(WorkspaceLayoutPersistence::Save(files, collision, path, layout));
        CHECK_FALSE(collision.WasCommitted());
        std::ifstream input(stage);
        std::string text;
        std::getline(input, text);
        CHECK(text == "foreign stage");
        CHECK(WorkspaceLayoutPersistence::Load(path)->surfaces == layout.surfaces);
    }

    TEST_CASE("Workspace cleanup exceptions preserve the original publication failure and previous bytes",
              "[unit][editor][persistence][extension-recovery]") {
        for (const bool throwPublication : {false, true}) {
            DYNAMIC_SECTION("publication exception " << throwPublication) {
                Horo::Tests::OwnedTestDirectory directory{"workspace-cleanup"};
                const auto path = directory.Path() / "workspace.json";
                PublicationFiles files;
                const auto layout = Layout();
                AtomicFileReplacementReceipt initial;
                REQUIRE(WorkspaceLayoutPersistence::Save(files, initial, path, layout));
                files.FailBeforeCommit(true);
                files.ThrowOnCleanup();
                if (throwPublication)
                    files.ThrowOnReplacement();
                AtomicFileReplacementReceipt rejected;
                std::string error;
                if (throwPublication)
                    REQUIRE_THROWS_AS(WorkspaceLayoutPersistence::Save(files, rejected, path, layout, &error), PublicationFailure);
                else {
                    REQUIRE_FALSE(WorkspaceLayoutPersistence::Save(files, rejected, path, layout, &error));
                    CHECK(error == MakeError(PlatformErrors::ConfigurationWriteFailed).message);
                }
                CHECK_FALSE(rejected.WasCommitted());
                CHECK(files.CleanupCalls() == 1);
                const auto retained = WorkspaceLayoutPersistence::Load(path);
                REQUIRE(retained);
                CHECK(retained->surfaces == layout.surfaces);
                CHECK(std::filesystem::is_regular_file(std::filesystem::path{path.string() + ".pending"}));
            }
        }
    }

    TEST_CASE("Workspace writer lease spans the real private write replacement and cleanup",
              "[unit][editor][persistence][extension-recovery]") {
        Horo::Tests::OwnedTestDirectory directory{"workspace-writer"};
        const auto path = directory.Path() / "workspace.json";
        PublicationFiles files;
        files.ObserveWriterLease(path);
        AtomicFileReplacementReceipt receipt;
        REQUIRE(WorkspaceLayoutPersistence::Save(files, receipt, path, Layout()));
        CHECK(receipt.WasCommitted());
        CHECK(files.WriterChecks() == 2);
        files.FailBeforeCommit(true);
        AtomicFileReplacementReceipt rejected;
        REQUIRE_FALSE(WorkspaceLayoutPersistence::Save(files, rejected, path, Layout()));
        CHECK_FALSE(rejected.WasCommitted());
        CHECK(files.CleanupCalls() == 1);
        CHECK(files.WriterChecks() == 5);
        CHECK(files.UnexpectedWriterChecks() == 0);
    }
}  // namespace Horo::Editor::Tests
