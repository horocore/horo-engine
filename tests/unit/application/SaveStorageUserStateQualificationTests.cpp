#include "Horo/Release/UserStateMigration.h"
#include "Horo/Release/UserStateMigrationErrors.h"
#include "SaveArchiveReaderTestHelpers.h"
#include "SaveFilesystemTestSupport.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::Release;
    using namespace Horo::Runtime;
    using namespace Horo::Runtime::SaveFilesystemTest;

    /** @brief Isolates packaged user preferences, installation versions and the product save root. */
    struct UpdateFixture final {
        TemporaryDirectory temporary;
        FixedEnvironment environment;
        std::filesystem::path userState = temporary.Path() / std::filesystem::path{u8"user state-ç"};
        std::filesystem::path cache = temporary.Path() / "cache";
        SaveArchiveHeader header = ArchiveReaderTest::Header();
        ProductSaveRoot root =
            Resolve({.product = header.product, .platform = SaveRootPlatform::Test, .testStateRoot = userState}, environment);
        SaveNamespaceId name{header.product, header.environment, UserProfileOwner{header.user, header.profile}};
        SaveGameSlotId slot = header.slot;
        std::vector<std::byte> archive = ArchiveReaderTest::MakeArchive().bytes;

        UpdateFixture() {
            std::filesystem::create_directories(cache);
            std::filesystem::create_directories(temporary.Path() / "install" / "v1");
            std::filesystem::create_directories(temporary.Path() / "install" / "v2");
            std::ofstream preferences(userState / "preferences.json", std::ios::binary);
            REQUIRE(preferences.good());
            preferences << "old-preferences";
            preferences.close();
            auto opened = SaveFilesystemStorage::Open(root, name);
            REQUIRE(opened.HasValue());
            REQUIRE(opened.Value().Replace(slot, archive).HasValue());
        }

        void CheckSaveUnchanged() const {
            auto opened = SaveFilesystemStorage::Open(root, name);
            REQUIRE(opened.HasValue());
            const auto retained = opened.Value().Read(slot, archive.size());
            REQUIRE(retained.HasValue());
            CHECK(retained.Value() == archive);
            const auto admitted = SaveArchiveReader{}.Read(retained.Value());
            REQUIRE(admitted.HasValue());
            CHECK(admitted.Value().Header() == header);
            CHECK(std::filesystem::is_empty(temporary.Path() / "install" / "v1"));
            CHECK(std::filesystem::is_empty(temporary.Path() / "install" / "v2"));
        }
    };

    /** @brief Explicit post-start host transform; failure occurs before any replacement. */
    class PreferenceTransform final : public IUserStateMigrationTransformer {
    public:
        bool fail{};
        std::size_t calls{};

        Result<std::vector<std::byte>> Transform(const UserStateMigrationStep &, std::span<const std::byte>) override {
            ++calls;
            if (fail)
                return Result<std::vector<std::byte>>::Failure(MakeError(UserStateMigrationErrors::TransformFailed));
            constexpr std::string_view text = "new-preferences";
            const auto bytes = std::as_bytes(std::span{text});
            return Result<std::vector<std::byte>>::Success({bytes.begin(), bytes.end()});
        }

        Result<void> ReauthorizeCredentialReference(std::string_view) override {
            return Result<void>::Success();
        }
    };

    /** @brief Declares one exact preference edge without including runtime save files in the plan. */
    UserStateMigrationStep PreferenceStep() {
        constexpr std::string_view source = "old-preferences", target = "new-preferences";
        return {.family = UserStateFamily::Preferences,
                .action = UserStateMigrationAction::Transform,
                .relativePath = "preferences.json",
                .sourceSchema = 1,
                .targetSchema = 2,
                .sourceDigest = ComputeSha256(std::as_bytes(std::span{source})),
                .targetDigest = ComputeSha256(std::as_bytes(std::span{target}))};
    }

    TEST_CASE("Post-start update migration and backup restore leave packaged runtime saves intact",
              "[save][qualification][release][headless]") {
        UpdateFixture fixture;
        NativeDurableFileSystem files;
        PreferenceTransform transform;
        const std::array steps{PreferenceStep()};
        const UserStateMigrationRequest request{fixture.userState, fixture.cache, steps};
        auto migrated = RunUserStateMigration(request, files, transform);
        REQUIRE(migrated.HasValue());
        CHECK(migrated.Value().transformed == 1);
        fixture.CheckSaveUnchanged();
        auto repeated = RunUserStateMigration(request, files, transform);
        REQUIRE(repeated.HasValue());
        CHECK(repeated.Value().alreadyApplied == 1);
        CHECK(transform.calls == 1);
        REQUIRE(RestoreUserStateMigrationBackup(fixture.userState, steps.front(), files).HasValue());
        fixture.CheckSaveUnchanged();
        const auto afterRestore = RunUserStateMigration(request, files, transform);
        REQUIRE(afterRestore.HasError());
        CHECK(afterRestore.ErrorValue().code.Value() == UserStateMigrationErrors::BackupRequiresRepair.code.Value());
        CHECK(transform.calls == 1);
        fixture.CheckSaveUnchanged();
    }

    TEST_CASE("Rejected user-state transformations preserve native saves across reopening", "[save][qualification][release][headless]") {
        UpdateFixture fixture;
        NativeDurableFileSystem files;
        PreferenceTransform transform;
        transform.fail = true;
        const std::array steps{PreferenceStep()};
        const auto failed = RunUserStateMigration({fixture.userState, fixture.cache, steps}, files, transform);
        REQUIRE(failed.HasError());
        CHECK(transform.calls == 1);
        fixture.CheckSaveUnchanged();
        transform.fail = false;
        const auto retried = RunUserStateMigration({fixture.userState, fixture.cache, steps}, files, transform);
        REQUIRE(retried.HasValue());
        CHECK(retried.Value().transformed == 1);
        fixture.CheckSaveUnchanged();
    }
}  // namespace
