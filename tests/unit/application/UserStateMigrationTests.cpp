#include "Horo/Release/UserStateMigration.h"
#include "Horo/Release/UserStateMigrationErrors.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <utility>

using namespace Horo::Release;

namespace {
    class TemporaryState final {
    public:
        TemporaryState()
            : root(std::filesystem::temp_directory_path() /
                   ("horo-user-state-migration-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
            std::filesystem::create_directories(root / "state");
            std::filesystem::create_directories(root / "cache");
            std::filesystem::create_directories(root / "project");
        }

        ~TemporaryState() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }

        std::filesystem::path root;
    };

    void Write(const std::filesystem::path &path, const std::string &value) {
        std::ofstream output(path, std::ios::binary);
        output << value;
    }

    [[nodiscard]] std::string Read(const std::filesystem::path &path) {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    }

    [[nodiscard]] Horo::Sha256Digest Digest(const std::string &value) {
        return Horo::ComputeSha256(std::as_bytes(std::span{value}));
    }

    class Transformer final : public IUserStateMigrationTransformer {
    public:
        explicit Transformer(std::string output) : output_(std::move(output)) {}

        [[nodiscard]] Horo::Result<std::vector<std::byte>> Transform(const UserStateMigrationStep &, std::span<const std::byte>) override {
            ++transformCalls;
            const auto bytes = std::as_bytes(std::span{output_});
            return Horo::Result<std::vector<std::byte>>::Success(std::vector<std::byte>{bytes.begin(), bytes.end()});
        }

        [[nodiscard]] Horo::Result<void> ReauthorizeCredentialReference(std::string_view reference) override {
            ++credentialChecks;
            if (reference != "vault:key-1" || !authorized)
                return Horo::Result<void>::Failure(Horo::MakeError(UserStateMigrationErrors::TransformFailed));
            return Horo::Result<void>::Success();
        }

        bool authorized{true};
        unsigned transformCalls{};
        unsigned credentialChecks{};

    private:
        std::string output_;
    };
}  // namespace

TEST_CASE("Post-start user-state migration preserves projects and a recoverable backup", "[release][user-state]") {
    TemporaryState state;
    Horo::NativeDurableFileSystem files;
    Write(state.root / "state" / "preferences.json", "old-state");
    Write(state.root / "project" / "project.json", "project-data");
    const std::array steps{UserStateMigrationStep{UserStateFamily::Preferences,
                                                  UserStateMigrationAction::Transform,
                                                  "preferences.json",
                                                  1U,
                                                  2U,
                                                  Digest("old-state"),
                                                  Digest("new-state"),
                                                  {"vault:key-1"}}};
    const UserStateMigrationRequest request{state.root / "state", state.root / "cache", steps};
    Transformer transformer{"new-state"};
    auto result = RunUserStateMigration(request, files, transformer);
    REQUIRE(result.HasValue());
    CHECK(result.Value().transformed == 1U);
    CHECK(transformer.credentialChecks == 1U);
    CHECK(Read(state.root / "state" / "preferences.json") == "new-state");
    CHECK(Read(state.root / "state" / "preferences.json.horo-backup-v1") == "old-state");
    CHECK(Read(state.root / "project" / "project.json") == "project-data");
    auto repeat = RunUserStateMigration(request, files, transformer);
    REQUIRE(repeat.HasValue());
    CHECK(repeat.Value().alreadyApplied == 1U);
    CHECK(transformer.transformCalls == 1U);
    Write(state.root / "state" / "preferences.json", "user-edited");
    CHECK(RestoreUserStateMigrationBackup(state.root / "state", steps.front(), files).HasError());
    CHECK(Read(state.root / "state" / "preferences.json") == "user-edited");
    Write(state.root / "state" / "preferences.json", "new-state");
    REQUIRE(RestoreUserStateMigrationBackup(state.root / "state", steps.front(), files).HasValue());
    CHECK(Read(state.root / "state" / "preferences.json") == "old-state");
}

TEST_CASE("Credential reauthorization failure keeps the original state", "[release][user-state]") {
    TemporaryState state;
    Horo::NativeDurableFileSystem files;
    Write(state.root / "state" / "toolchain.json", "old-toolchain");
    const std::array steps{UserStateMigrationStep{UserStateFamily::ToolchainProfiles,
                                                  UserStateMigrationAction::Transform,
                                                  "toolchain.json",
                                                  3U,
                                                  4U,
                                                  Digest("old-toolchain"),
                                                  Digest("new-toolchain"),
                                                  {"vault:key-1"}}};
    const UserStateMigrationRequest request{state.root / "state", state.root / "cache", steps};
    Transformer transformer{"new-toolchain"};
    transformer.authorized = false;
    CHECK(RunUserStateMigration(request, files, transformer).HasError());
    CHECK(Read(state.root / "state" / "toolchain.json") == "old-toolchain");
    CHECK_FALSE(std::filesystem::exists(state.root / "state" / "toolchain.json.horo-backup-v3"));
}

TEST_CASE("Interrupted publication keeps a verified backup and an explicit repair path", "[release][user-state]") {
    TemporaryState state;
    Horo::NativeDurableFileSystem files;
    Write(state.root / "state" / "recent_projects.json", "old-recent");
    Write(state.root / "state" / "recent_projects.json.horo-backup-v1", "old-recent");
    Write(state.root / "state" / "recent_projects.json.horo-migration-prepared", "new-recent");
    const std::array steps{UserStateMigrationStep{UserStateFamily::RecentProjects,
                                                  UserStateMigrationAction::Transform,
                                                  "recent_projects.json",
                                                  1U,
                                                  2U,
                                                  Digest("old-recent"),
                                                  Digest("new-recent"),
                                                  {}}};
    const UserStateMigrationRequest request{state.root / "state", state.root / "cache", steps};
    Transformer transformer{"new-recent"};
    CHECK(RunUserStateMigration(request, files, transformer).HasError());
    CHECK(Read(state.root / "state" / "recent_projects.json") == "old-recent");
    REQUIRE(RestoreUserStateMigrationBackup(state.root / "state", steps.front(), files).HasValue());
    CHECK_FALSE(std::filesystem::exists(state.root / "state" / "recent_projects.json.horo-migration-prepared"));
    CHECK(Read(state.root / "state" / "recent_projects.json") == "old-recent");
}

TEST_CASE("Disposable cache is discarded without touching state or projects", "[release][user-state]") {
    TemporaryState state;
    Horo::NativeDurableFileSystem files;
    Write(state.root / "cache" / "thumbnail.bin", "cache-data");
    Write(state.root / "project" / "project.json", "project-data");
    const std::array steps{UserStateMigrationStep{UserStateFamily::DisposableCache,
                                                  UserStateMigrationAction::DiscardCache,
                                                  "thumbnail.bin",
                                                  1U,
                                                  2U,
                                                  Digest("cache-data"),
                                                  {},
                                                  {}}};
    const UserStateMigrationRequest request{state.root / "state", state.root / "cache", steps};
    Transformer transformer{"unused"};
    auto result = RunUserStateMigration(request, files, transformer);
    REQUIRE(result.HasValue());
    CHECK(result.Value().discardedCaches == 1U);
    CHECK_FALSE(std::filesystem::exists(state.root / "cache" / "thumbnail.bin"));
    CHECK(Read(state.root / "project" / "project.json") == "project-data");
    auto repeat = RunUserStateMigration(request, files, transformer);
    REQUIRE(repeat.HasValue());
    CHECK(repeat.Value().alreadyApplied == 1U);
}

TEST_CASE("Migration rejects project escapes, links, and stale source evidence", "[release][user-state]") {
    TemporaryState state;
    Horo::NativeDurableFileSystem files;
    Write(state.root / "project" / "project.json", "project-data");
    const std::array escape{UserStateMigrationStep{UserStateFamily::Preferences,
                                                   UserStateMigrationAction::Transform,
                                                   "../project/project.json",
                                                   1U,
                                                   2U,
                                                   Digest("project-data"),
                                                   Digest("changed"),
                                                   {}}};
    CHECK(PlanUserStateMigration({state.root / "state", state.root / "cache", escape}).HasError());
    std::filesystem::create_symlink(state.root / "project" / "project.json", state.root / "state" / "link.json");
    const std::array linked{UserStateMigrationStep{UserStateFamily::Preferences,
                                                   UserStateMigrationAction::Transform,
                                                   "link.json",
                                                   1U,
                                                   2U,
                                                   Digest("project-data"),
                                                   Digest("changed"),
                                                   {}}};
    Transformer transformer{"changed"};
    CHECK(RunUserStateMigration({state.root / "state", state.root / "cache", linked}, files, transformer).HasError());
    CHECK(Read(state.root / "project" / "project.json") == "project-data");
    Write(state.root / "state" / "preferences.json", "unexpected");
    const std::array stale{UserStateMigrationStep{UserStateFamily::Preferences,
                                                  UserStateMigrationAction::Transform,
                                                  "preferences.json",
                                                  1U,
                                                  2U,
                                                  Digest("expected"),
                                                  Digest("changed"),
                                                  {}}};
    CHECK(RunUserStateMigration({state.root / "state", state.root / "cache", stale}, files, transformer).HasError());
    CHECK(Read(state.root / "state" / "preferences.json") == "unexpected");
}
