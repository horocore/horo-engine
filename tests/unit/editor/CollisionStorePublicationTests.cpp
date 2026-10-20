#include "CollisionSettingsTestSupport.h"

namespace Horo::Tests::CollisionSettings {
    namespace {
        const ErrorCodeDescriptor Injected{ErrorDomainId{"test.collision"}, ErrorCode{"publication.uncertain"}, ErrorSeverity::Error,
                                           "Injected post-replacement directory uncertainty.", "Retry with the published source identity."};

        class PublicationFiles final : public DurableFileSystem {
        public:
            enum class Mode {
                Durable,
                Uncertain,
                FailPreparation
            };

            void SetMode(const Mode mode) noexcept {
                mode_ = mode;
            }

            std::size_t CommittedReplacements() const noexcept {
                return committed_;
            }

            Horo::Result<bool> CreateDirectoryPath(const std::filesystem::path &path, Horo::DirectoryCreationMode mode) override {
                return native_.CreateDirectoryPath(path, mode);
            }

            Result<ExclusiveFileLock> TryAcquireExclusive(const std::filesystem::path &path, const std::string_view owner) override {
                return native_.TryAcquireExclusive(path, owner);
            }

            Result<std::uint64_t> AvailableBytes(const std::filesystem::path &path) const override {
                return native_.AvailableBytes(path);
            }

            Result<void> WriteDurable(const std::filesystem::path &path, std::span<const std::byte> bytes) override {
                return native_.WriteDurable(path, bytes);
            }

            Result<void> WritePrivateDurable(const std::filesystem::path &path, std::span<const std::byte> bytes, bool &created,
                                             std::filesystem::perms permissions) override {
                if (mode_ == Mode::FailPreparation)
                    return Result<void>::Failure(MakeError(Injected));
                return native_.WritePrivateDurable(path, bytes, created, permissions);
            }

            Result<void> CopyDurable(const std::filesystem::path &from, const std::filesystem::path &to) override {
                return native_.CopyDurable(from, to);
            }

            Result<void> AtomicReplace(const std::filesystem::path &from, const std::filesystem::path &to) override {
                return native_.AtomicReplace(from, to);
            }

            Result<void> AtomicReplaceTracked(const std::filesystem::path &from, const std::filesystem::path &to,
                                              AtomicFileReplacementReceipt &receipt) override {
                auto result = native_.AtomicReplaceTracked(from, to, receipt);
                if (result.HasError())
                    return result;
                ++committed_;
                return mode_ == Mode::Uncertain ? Result<void>::Failure(MakeError(Injected)) : result;
            }

            Result<void> RemoveDurable(const std::filesystem::path &path) override {
                return native_.RemoveDurable(path);
            }

            Result<void> SyncDirectory(const std::filesystem::path &path) override {
                return native_.SyncDirectory(path);
            }

        private:
            NativeDurableFileSystem native_;
            Mode mode_{Mode::Durable};
            std::size_t committed_{};
        };
    }  // namespace

    TEST_CASE("Collision actual atomic replacement uncertainty advances source identity and retries without false clean state",
              "[collision][publication]") {
        Project project;
        PublicationFiles files;
        const Editor::CollisionProjectSettingsOwners owners{project.registry, files, project.mutations, project.inventory, *project.source};
        auto opening = Editor::CollisionProjectSettingsSession::Open(project.root, "collision-project", owners, project.activation);
        REQUIRE(opening.HasValue());
        auto session = opening.Value();
        const auto oldSource = project.store.Load().Value();
        auto input = session->Snapshot().Value()->Input();
        input.layers.front().name = "Durable intent";
        Commit(*session, input);
        files.SetMode(PublicationFiles::Mode::Uncertain);
        auto uncertain = session->Save();
        REQUIRE(uncertain.HasValue());
        REQUIRE(uncertain.Value().durabilityError.has_value());
        CHECK(uncertain.Value().snapshot.identity != oldSource.identity);
        CHECK(session->IsDirty());
        CHECK(project.store.Load().Value().identity == uncertain.Value().snapshot.identity);
        CHECK(files.CommittedReplacements() == 1);
        files.SetMode(PublicationFiles::Mode::Durable);
        auto retried = session->Save();
        REQUIRE(retried.HasValue());
        CHECK_FALSE(retried.Value().durabilityError);
        CHECK(retried.Value().snapshot.identity != uncertain.Value().snapshot.identity);
        CHECK_FALSE(session->IsDirty());
        CHECK(files.CommittedReplacements() == 2);
        CHECK(retried.Value().snapshot.settings->Input().revision == 3);
        CHECK(oldSource.settings->Input().revision == 1);
    }

    TEST_CASE("Collision failed preparation preserves canonical file dirty draft and replay", "[collision][publication]") {
        Project project;
        PublicationFiles files;
        const Editor::CollisionProjectSettingsOwners owners{project.registry, files, project.mutations, project.inventory, *project.source};
        auto opening = Editor::CollisionProjectSettingsSession::Open(project.root, "collision-project", owners, project.activation);
        REQUIRE(opening.HasValue());
        auto session = opening.Value();
        const auto oldSource = project.store.Load().Value();
        auto input = session->Snapshot().Value()->Input();
        input.layers.front().name = "Pending";
        Commit(*session, input);
        const auto draft = session->Snapshot().Value();
        files.SetMode(PublicationFiles::Mode::FailPreparation);
        CHECK(session->Save().HasError());
        CHECK(session->IsDirty());
        CHECK(session->Snapshot().Value() == draft);
        CHECK(project.store.Load().Value().identity == oldSource.identity);
        CHECK(files.CommittedReplacements() == 0);
        REQUIRE(session->Replay(false).HasValue());
        CHECK_FALSE(session->IsDirty());
    }
}  // namespace Horo::Tests::CollisionSettings
