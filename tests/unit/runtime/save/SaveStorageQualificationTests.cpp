#include "SaveFilesystemTestSupport.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <utility>
#include <vector>

namespace Horo::Runtime {
    namespace {
        using namespace SaveFilesystemTest;

        /** @brief Opens a namespace and verifies exact bytes after a new process-lifetime owner is composed. */
        void CheckReopened(const ProductSaveRoot &root, const SaveNamespaceId &name, const SaveGameSlotId slot,
                           const std::vector<std::byte> &expected) {
            auto opened = SaveFilesystemStorage::Open(root, name);
            REQUIRE(opened.HasValue());
            const auto read = opened.Value().Read(slot, expected.size());
            REQUIRE(read.HasValue());
            CHECK(read.Value() == expected);
        }

        TEST_CASE("Packaged desktop root policies retain storage across owner restart", "[save][qualification][filesystem]") {
            TemporaryDirectory temporary;
            FixedEnvironment environment;
            environment.localApplicationData = (temporary.Path() / "Windows State").string();
            environment.home = (temporary.Path() / "Home State").string();
            environment.xdgStateHome = (temporary.Path() / "Linux State").string();
            const SaveNamespaceId name{.product = Product(),
                                       .environment = Test::Id<EnvironmentStorageId>(2),
                                       .owner = UserProfileOwner{Test::Id<LocalUserStorageId>(3), Test::Id<GameProfileId>(4)}};
            const auto slot = Test::Id<SaveGameSlotId>(5);
            const std::vector bytes{std::byte{0}, std::byte{0xff}, std::byte{0x48}};
            for (const auto platform : {SaveRootPlatform::Windows, SaveRootPlatform::MacOS, SaveRootPlatform::Linux}) {
                INFO(static_cast<int>(platform));
                const auto root = Resolve({.product = Product(), .platform = platform}, environment);
                {
                    auto opened = SaveFilesystemStorage::Open(root, name);
                    REQUIRE(opened.HasValue());
                    REQUIRE(opened.Value().Replace(slot, bytes).HasValue());
                }
                const auto restarted = Resolve({.product = Product(), .platform = platform}, environment);
                CHECK(restarted.CanonicalPath() == root.CanonicalPath());
                CheckReopened(restarted, name, slot, bytes);
            }
        }

        TEST_CASE("Unicode state roots keep headless and client publications in distinct ownership partitions",
                  "[save][qualification][filesystem][headless]") {
            TemporaryDirectory temporary;
            FixedEnvironment environment;
            const auto root = Resolve({.product = Product(),
                                       .platform = SaveRootPlatform::Test,
                                       .testStateRoot = temporary.Path() / std::filesystem::path{u8"state space-ç-保存"}},
                                      environment);
            const auto slot = Test::Id<SaveGameSlotId>(7);
            const std::array names{SaveNamespaceId{Product(), Test::Id<EnvironmentStorageId>(2),
                                                   UserProfileOwner{Test::Id<LocalUserStorageId>(3), Test::Id<GameProfileId>(4)}},
                                   SaveNamespaceId{Product(), Test::Id<EnvironmentStorageId>(2),
                                                   UserProfileOwner{Test::Id<LocalUserStorageId>(3), Test::Id<GameProfileId>(5)}},
                                   SaveNamespaceId{Product(), Test::Id<EnvironmentStorageId>(2),
                                                   UserProfileOwner{Test::Id<LocalUserStorageId>(6), Test::Id<GameProfileId>(4)}},
                                   SaveNamespaceId{Product(), Test::Id<EnvironmentStorageId>(8),
                                                   UserProfileOwner{Test::Id<LocalUserStorageId>(3), Test::Id<GameProfileId>(4)}},
                                   SaveNamespaceId{Product(), Test::Id<EnvironmentStorageId>(2),
                                                   ServerWorldOwner{Test::Id<ServerStorageOwnerId>(3)}}};
            for (std::size_t index = 0; index < names.size(); ++index) {
                auto opened = SaveFilesystemStorage::Open(root, names[index]);
                REQUIRE(opened.HasValue());
                CHECK(opened.Value().Read(slot, 1).HasError());
                const std::array bytes{static_cast<std::byte>(index)};
                REQUIRE(opened.Value().Replace(slot, bytes).HasValue());
            }
            for (std::size_t index = 0; index < names.size(); ++index)
                CheckReopened(root, names[index], slot, {static_cast<std::byte>(index)});
            CHECK(environment.environmentLookups == 0);
        }

        TEST_CASE("Missing slots and invalid replacements preserve native last published bytes and unrelated staging",
                  "[save][qualification][filesystem]") {
            StorageFixture fixture;
            const auto slot = Test::Id<SaveGameSlotId>(5);
            const std::vector previous{std::byte{1}, std::byte{2}};
            {
                auto opened = SaveFilesystemStorage::Open(fixture.root, fixture.name);
                REQUIRE(opened.HasValue());
                auto storage = std::move(opened).Value();
                REQUIRE(storage.Replace(slot, previous).HasValue());
                const auto staging = fixture.Slots() / ".another-owner.temporary";
                std::ofstream unrelated(staging, std::ios::binary);
                REQUIRE(unrelated.good());
                unrelated << "retained evidence";
                unrelated.close();
                CHECK(storage.Read(Test::Id<SaveGameSlotId>(9), previous.size()).HasError());
                CHECK(storage.Replace(slot, {}).HasError());
                CHECK(storage.Replace({}, previous).HasError());
                CHECK(storage.Read(slot, previous.size() - 1).HasError());
                CHECK(storage.Read(slot, previous.size()).Value() == previous);
                CHECK(std::filesystem::file_size(staging) == 17);
            }
            CheckReopened(fixture.root, fixture.name, slot, previous);
        }

#ifndef _WIN32
        TEST_CASE("Native permission denial before replacement retains the prior archive",
                  "[save][qualification][filesystem][permission]") {
            StorageFixture fixture;
            auto opened = SaveFilesystemStorage::Open(fixture.root, fixture.name);
            REQUIRE(opened.HasValue());
            auto storage = std::move(opened).Value();
            const auto slot = Test::Id<SaveGameSlotId>(5);
            const std::vector previous{std::byte{1}, std::byte{2}};
            const std::array candidate{std::byte{9}};
            REQUIRE(storage.Replace(slot, previous).HasValue());
            const auto originalPermissions = std::filesystem::status(fixture.Slots()).permissions();
            std::filesystem::permissions(fixture.Slots(), std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
            const auto denied = storage.Replace(slot, candidate);
            std::filesystem::permissions(fixture.Slots(), originalPermissions);
            REQUIRE(denied.HasError());
            CHECK(storage.Read(slot, previous.size()).Value() == previous);
            CHECK(denied.ErrorValue().message.find(fixture.temporary.Path().string()) == std::string::npos);
        }
#endif
    }  // namespace
}  // namespace Horo::Runtime
