#include "Horo/Foundation/Platform.h"
#include "Horo/Runtime/Save/SaveErrors.h"
#include "Horo/Runtime/Save/SaveFilesystemStorage.h"
#include "Horo/Runtime/Save/SaveRootResolver.h"
#include "SaveTestUtils.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace Horo::Runtime {
    namespace {
        constexpr std::string_view kProductUuid = "00112233-4455-6677-8899-aabbccddeeff";

        class FixedEnvironment final : public ProcessService {
        public:
            [[nodiscard]] ProcessMetadata CurrentProcess() const override {
                return {.id = 1, .executableName = "save-root-test"};
            }

            [[nodiscard]] std::optional<std::string> EnvironmentValue(const std::string_view name) const override {
                ++environmentLookups;
                if (name == "LOCALAPPDATA")
                    return localApplicationData;
                if (name == "HOME")
                    return home;
                if (name == "XDG_STATE_HOME")
                    return xdgStateHome;
                return std::nullopt;
            }

            std::optional<std::string> localApplicationData;
            std::optional<std::string> home;
            std::optional<std::string> xdgStateHome;
            mutable std::size_t environmentLookups{0};
        };

        class TemporaryDirectory final {
        public:
            TemporaryDirectory() {
                static std::atomic_uint64_t next{0};
                const auto base = std::filesystem::temp_directory_path();
                for (std::uint64_t attempt = 0; attempt < 256; ++attempt) {
                    path_ = base / ("horo-save-root-" + std::to_string(next.fetch_add(1)));
                    std::error_code error;
                    if (std::filesystem::create_directory(path_, error))
                        return;
                }
                FAIL("Could not create a unique save-root test directory");
            }

            ~TemporaryDirectory() {
                std::error_code ignored;
                std::filesystem::permissions(path_, std::filesystem::perms::owner_all, std::filesystem::perm_options::add, ignored);
                std::filesystem::remove_all(path_, ignored);
            }

            [[nodiscard]] const std::filesystem::path &Path() const noexcept {
                return path_;
            }

        private:
            std::filesystem::path path_;
        };

        [[nodiscard]] ProductStorageId Product() {
            auto parsed = ProductStorageId::Parse(kProductUuid);
            REQUIRE(parsed.HasValue());
            return parsed.Value();
        }

        [[nodiscard]] ProductSaveRoot Resolve(const SaveRootResolutionRequest &request, const FixedEnvironment &environment) {
            auto resolved = ResolveProductSaveRoot(request, environment);
            REQUIRE(resolved.HasValue());
            return resolved.Value();
        }

        TEST_CASE("Save roots follow explicit platform user-state conventions", "[unit][save][root]") {
            TemporaryDirectory temporary;
            FixedEnvironment environment;
            environment.localApplicationData = (temporary.Path() / "Windows State").string();
            environment.home = (temporary.Path() / "Home").string();
            environment.xdgStateHome = (temporary.Path() / "XDG State").string();

            SECTION("Windows uses LOCALAPPDATA") {
                const auto root = Resolve({.product = Product(), .platform = SaveRootPlatform::Windows}, environment);
                REQUIRE(root.CanonicalPath() ==
                        std::filesystem::canonical(temporary.Path() / "Windows State" / "Horo" / "Products" / kProductUuid));
            }
            SECTION("macOS uses HOME Application Support") {
                const auto root = Resolve({.product = Product(), .platform = SaveRootPlatform::MacOS}, environment);
                REQUIRE(root.CanonicalPath() == std::filesystem::canonical(temporary.Path() / "Home" / "Library" / "Application Support" /
                                                                           "Horo" / "Products" / kProductUuid));
            }
            SECTION("Linux prefers XDG_STATE_HOME") {
                const auto root = Resolve({.product = Product(), .platform = SaveRootPlatform::Linux}, environment);
                REQUIRE(root.CanonicalPath() ==
                        std::filesystem::canonical(temporary.Path() / "XDG State" / "horo" / "products" / kProductUuid));
            }
            SECTION("Linux falls back to HOME state") {
                environment.xdgStateHome.reset();
                const auto root = Resolve({.product = Product(), .platform = SaveRootPlatform::Linux}, environment);
                REQUIRE(root.CanonicalPath() ==
                        std::filesystem::canonical(temporary.Path() / "Home" / ".local" / "state" / "horo" / "products" / kProductUuid));
            }
        }

        TEST_CASE("Test save roots accept spaces and non-ASCII components without ambient lookup", "[unit][save][root]") {
            TemporaryDirectory temporary;
            FixedEnvironment environment;
            const auto sandbox = temporary.Path() / std::filesystem::path{u8"state space-ç"};
            const auto root = Resolve({.product = Product(), .platform = SaveRootPlatform::Test, .testStateRoot = sandbox}, environment);

            REQUIRE(root.IsValid());
            REQUIRE(root.Product() == Product());
            REQUIRE(root.Platform() == SaveRootPlatform::Test);
            REQUIRE(environment.environmentLookups == 0);
            REQUIRE(root.CanonicalPath() == std::filesystem::canonical(sandbox / "horo" / "products" / kProductUuid));
        }

        TEST_CASE("Save-root configuration rejects missing relative and wrong-platform inputs", "[unit][save][root]") {
            TemporaryDirectory temporary;
            FixedEnvironment environment;

            REQUIRE(ResolveProductSaveRoot({.product = Product(), .platform = SaveRootPlatform::Windows}, environment).HasError());
            environment.localApplicationData = "relative";
            REQUIRE(ResolveProductSaveRoot({.product = Product(), .platform = SaveRootPlatform::Windows}, environment).HasError());
            REQUIRE(ResolveProductSaveRoot({.product = Product(), .platform = SaveRootPlatform::Test}, environment).HasError());
            REQUIRE(ResolveProductSaveRoot({.product = Product(),
                                            .platform = static_cast<SaveRootPlatform>(255),
                                            .testStateRoot = temporary.Path()},
                                           environment)
                        .ErrorValue()
                        .code.Value() == SaveErrors::SaveRootPlatformUnsupported.code.Value());
            REQUIRE(
                ResolveProductSaveRoot({.product = {}, .platform = SaveRootPlatform::Test, .testStateRoot = temporary.Path()}, environment)
                    .ErrorValue()
                    .code.Value() == SaveErrors::IdentityInvalid.code.Value());
        }

        TEST_CASE("Save-root containment rejects redirected and unexpected entries", "[unit][save][root]") {
            TemporaryDirectory temporary;
            FixedEnvironment environment;
            const auto sandbox = temporary.Path() / "approved";
            const auto outside = temporary.Path() / "outside";
            REQUIRE(std::filesystem::create_directories(outside));
            REQUIRE(std::filesystem::create_directories(sandbox));

            std::error_code linkError;
            std::filesystem::create_directory_symlink(outside, sandbox / "horo", linkError);
            if (!linkError) {
                const auto linked =
                    ResolveProductSaveRoot({.product = Product(), .platform = SaveRootPlatform::Test, .testStateRoot = sandbox},
                                           environment);
                REQUIRE(linked.HasError());
                REQUIRE(linked.ErrorValue().code.Value() == SaveErrors::SaveRootContainmentViolation.code.Value());
                REQUIRE(linked.ErrorValue().message.find(temporary.Path().string()) == std::string::npos);
            }

            std::filesystem::remove(sandbox / "horo", linkError);
            std::ofstream unexpected{sandbox / "horo"};
            REQUIRE(unexpected.good());
            unexpected.close();
            const auto fileEntry =
                ResolveProductSaveRoot({.product = Product(), .platform = SaveRootPlatform::Test, .testStateRoot = sandbox}, environment);
            REQUIRE(fileEntry.HasError());
            REQUIRE(fileEntry.ErrorValue().code.Value() == SaveErrors::SaveRootContainmentViolation.code.Value());
        }

        TEST_CASE("Save-root filesystem failures expose safe diagnostics without native paths", "[unit][save][root]") {
            TemporaryDirectory temporary;
            FixedEnvironment environment;
            const auto blockingFile = temporary.Path() / "not-a-directory";
            std::ofstream output{blockingFile};
            REQUIRE(output.good());
            output.close();

            const auto failed = ResolveProductSaveRoot({.product = Product(),
                                                        .platform = SaveRootPlatform::Test,
                                                        .testStateRoot = blockingFile / "denied child"},
                                                       environment);
            REQUIRE(failed.HasError());
            REQUIRE(failed.ErrorValue().code.Value() == SaveErrors::SaveRootUnavailable.code.Value());
            REQUIRE(failed.ErrorValue().message.find(temporary.Path().string()) == std::string::npos);
            REQUIRE(failed.ErrorValue().message.find("denied child") == std::string::npos);
        }

        TEST_CASE("Save filesystem storage publishes complete generations inside typed profile roots", "[unit][save][storage]") {
            TemporaryDirectory temporary;
            FixedEnvironment environment;
            const auto root = Resolve({.product = Product(),
                                       .platform = SaveRootPlatform::Test,
                                       .testStateRoot = temporary.Path() / std::filesystem::path{u8"profile space-ç"}},
                                      environment);
            const SaveNamespaceId name{.product = Product(),
                                       .environment = Test::Id<EnvironmentStorageId>(2),
                                       .owner = UserProfileOwner{Test::Id<LocalUserStorageId>(3), Test::Id<GameProfileId>(4)}};
            auto opened = SaveFilesystemStorage::Open(root, name);
            REQUIRE(opened.HasValue());
            auto storage = std::move(opened).Value();
            const auto slot = Test::Id<SaveGameSlotId>(5);
            const std::array first{std::byte{1}, std::byte{2}, std::byte{3}};
            const std::array second{std::byte{4}, std::byte{5}};
            const auto firstWrite = storage.Replace(slot, first);
            if (firstWrite.HasError())
                WARN(firstWrite.ErrorValue().message);
            REQUIRE(firstWrite.HasValue());
            REQUIRE(storage.Read(slot, 3).Value() == std::vector<std::byte>(first.begin(), first.end()));
            REQUIRE(storage.Read(slot, 2).HasError());
            REQUIRE(storage.Read(slot, std::numeric_limits<std::size_t>::max()).Value() ==
                    std::vector<std::byte>(first.begin(), first.end()));
            REQUIRE(storage.Read(slot, 0).HasError());
            const auto secondWrite = storage.Replace(slot, second);
            if (secondWrite.HasError())
                WARN(secondWrite.ErrorValue().message);
            REQUIRE(secondWrite.HasValue());
            REQUIRE(storage.Read(slot, 3).Value() == std::vector<std::byte>(second.begin(), second.end()));
            REQUIRE(storage.Replace(slot, {}).HasError());
            REQUIRE(storage.Read(slot, 3).Value() == std::vector<std::byte>(second.begin(), second.end()));
        }

        TEST_CASE("Save filesystem storage rejects unsafe namespace and slot links", "[unit][save][storage]") {
            TemporaryDirectory temporary;
            FixedEnvironment environment;
            const auto root =
                Resolve({.product = Product(), .platform = SaveRootPlatform::Test, .testStateRoot = temporary.Path() / "approved"},
                        environment);
            const auto outside = temporary.Path() / "outside";
            REQUIRE(std::filesystem::create_directory(outside));
            const SaveNamespaceId name{.product = Product(),
                                       .environment = Test::Id<EnvironmentStorageId>(2),
                                       .owner = ServerWorldOwner{Test::Id<ServerStorageOwnerId>(3)}};
            const auto environmentPath = root.CanonicalPath() / name.environment.ToString();
            std::error_code error;
            std::filesystem::create_directory_symlink(outside, environmentPath, error);
            if (!error) {
                const auto escaped = SaveFilesystemStorage::Open(root, name);
                REQUIRE(escaped.HasError());
                REQUIRE(escaped.ErrorValue().message.find(temporary.Path().string()) == std::string::npos);
                REQUIRE(std::filesystem::is_empty(outside));
                REQUIRE(std::filesystem::remove(environmentPath));
            }
            auto opened = SaveFilesystemStorage::Open(root, name);
            REQUIRE(opened.HasValue());
            auto storage = std::move(opened).Value();
            const auto slot = Test::Id<SaveGameSlotId>(5);
            const auto slots = environmentPath / "server" / std::get<ServerWorldOwner>(name.owner).owner.ToString() / "slots";
            const auto target = slots / (slot.ToString() + ".horosave");
            const auto outsideFile = outside / "protected";
            {
                std::ofstream output{outsideFile};
                output << "old";
            }
            std::filesystem::create_symlink(outsideFile, target, error);
            if (!error) {
                const std::array candidate{std::byte{9}};
                REQUIRE(storage.Replace(slot, candidate).HasError());
                REQUIRE(storage.Read(slot, 4).HasError());
                std::ifstream input{outsideFile};
                std::string content;
                input >> content;
                REQUIRE(content == "old");
                REQUIRE(std::filesystem::remove(target));
            }
            std::filesystem::create_hard_link(outsideFile, target, error);
            if (!error) {
                REQUIRE(storage.Read(slot, 4).HasError());
                REQUIRE(std::filesystem::remove(target));
            }
        }

        TEST_CASE("Save filesystem storage rejects a replaced namespace after opening", "[unit][save][storage]") {
            TemporaryDirectory temporary;
            FixedEnvironment environment;
            const auto root =
                Resolve({.product = Product(), .platform = SaveRootPlatform::Test, .testStateRoot = temporary.Path() / "approved"},
                        environment);
            const SaveNamespaceId name{.product = Product(),
                                       .environment = Test::Id<EnvironmentStorageId>(2),
                                       .owner = ServerWorldOwner{Test::Id<ServerStorageOwnerId>(3)}};
            auto opened = SaveFilesystemStorage::Open(root, name);
            REQUIRE(opened.HasValue());
            auto storage = std::move(opened).Value();
            const auto slot = Test::Id<SaveGameSlotId>(5);
            const std::array previous{std::byte{3}, std::byte{4}};
            const auto previousWrite = storage.Replace(slot, previous);
            if (previousWrite.HasError())
                WARN(previousWrite.ErrorValue().message);
            REQUIRE(previousWrite.HasValue());
            const auto original = root.CanonicalPath() / name.environment.ToString();
            const auto moved = temporary.Path() / "moved-namespace";
            std::error_code renameError;
            std::filesystem::rename(original, moved, renameError);
#ifdef _WIN32
            // Windows may deny moving a tree while the storage capability owns child handles.
            if (renameError == std::errc::permission_denied) {
                const auto retained = original / "server" / std::get<ServerWorldOwner>(name.owner).owner.ToString() / "slots" /
                                      (slot.ToString() + ".horosave");
                std::ifstream input{retained, std::ios::binary};
                REQUIRE(input.good());
                REQUIRE(input.get() == 3);
                REQUIRE(input.get() == 4);
                REQUIRE(input.get() == std::char_traits<char>::eof());
                SUCCEED("Windows denied namespace replacement while child handles were held");
                return;
            }
#endif
            REQUIRE_FALSE(renameError);
            const auto outside = temporary.Path() / "outside";
            REQUIRE(std::filesystem::create_directory(outside));
            std::error_code error;
            std::filesystem::create_directory_symlink(outside, original, error);
            const std::array candidate{std::byte{8}};
            REQUIRE(storage.Replace(slot, candidate).HasError());
            REQUIRE(storage.Read(slot, 10).HasError());
            REQUIRE(std::filesystem::is_empty(outside));
            const auto retained =
                moved / "server" / std::get<ServerWorldOwner>(name.owner).owner.ToString() / "slots" / (slot.ToString() + ".horosave");
            std::ifstream input{retained, std::ios::binary};
            REQUIRE(input.good());
            REQUIRE(input.get() == 3);
            REQUIRE(input.get() == 4);
            REQUIRE(input.get() == std::char_traits<char>::eof());
        }

#ifdef _WIN32
        TEST_CASE("Windows save storage rejects case aliases for typed namespace components", "[unit][save][storage]") {
            TemporaryDirectory temporary;
            FixedEnvironment environment;
            const auto root =
                Resolve({.product = Product(), .platform = SaveRootPlatform::Test, .testStateRoot = temporary.Path() / "approved"},
                        environment);
            const SaveNamespaceId name{.product = Product(),
                                       .environment = Test::Id<EnvironmentStorageId>(0xab),
                                       .owner = ServerWorldOwner{Test::Id<ServerStorageOwnerId>(3)}};
            std::string upper = name.environment.ToString();
            for (char &value : upper)
                value = static_cast<char>(std::toupper(static_cast<unsigned char>(value)));
            REQUIRE(std::filesystem::create_directory(root.CanonicalPath() / upper));
            auto opened = SaveFilesystemStorage::Open(root, name);
            REQUIRE(opened.HasError());
            REQUIRE(opened.ErrorValue().code.Value() == SaveErrors::SaveRootContainmentViolation.code.Value());
        }

        TEST_CASE("Windows save storage rejects case aliases for existing slot files", "[unit][save][storage]") {
            TemporaryDirectory temporary;
            FixedEnvironment environment;
            const auto root =
                Resolve({.product = Product(), .platform = SaveRootPlatform::Test, .testStateRoot = temporary.Path() / "approved"},
                        environment);
            const SaveNamespaceId name{.product = Product(),
                                       .environment = Test::Id<EnvironmentStorageId>(2),
                                       .owner = ServerWorldOwner{Test::Id<ServerStorageOwnerId>(3)}};
            auto opened = SaveFilesystemStorage::Open(root, name);
            REQUIRE(opened.HasValue());
            auto storage = std::move(opened).Value();
            const auto slot = Test::Id<SaveGameSlotId>(0xab);
            std::string upper = slot.ToString() + ".horosave";
            for (char &value : upper)
                value = static_cast<char>(std::toupper(static_cast<unsigned char>(value)));
            const auto slots = root.CanonicalPath() / name.environment.ToString() / "server" /
                               std::get<ServerWorldOwner>(name.owner).owner.ToString() / "slots";
            const auto alias = slots / upper;
            {
                std::ofstream output{alias, std::ios::binary};
                REQUIRE(output.good());
                output << "old";
            }
            const std::array candidate{std::byte{9}};
            REQUIRE(storage.Replace(slot, candidate).HasError());
            std::ifstream input{alias, std::ios::binary};
            std::string content;
            input >> content;
            REQUIRE(content == "old");
        }
#endif

#ifndef _WIN32
        TEST_CASE("Save-root creation rejects an unwritable approved state directory", "[unit][save][root]") {
            TemporaryDirectory temporary;
            FixedEnvironment environment;
            const auto denied = temporary.Path() / "denied";
            REQUIRE(std::filesystem::create_directory(denied));
            std::filesystem::permissions(denied, std::filesystem::perms::none);

            const auto failed =
                ResolveProductSaveRoot({.product = Product(), .platform = SaveRootPlatform::Test, .testStateRoot = denied}, environment);
            std::filesystem::permissions(denied, std::filesystem::perms::owner_all);

            REQUIRE(failed.HasError());
            REQUIRE(failed.ErrorValue().code.Value() == SaveErrors::SaveRootUnavailable.code.Value());
            REQUIRE(failed.ErrorValue().message.find(temporary.Path().string()) == std::string::npos);
        }
#endif
    }  // namespace
}  // namespace Horo::Runtime
