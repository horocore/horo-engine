#include "SaveFilesystemPinnedRead.h"
#include "SaveFilesystemTestSupport.h"

#include <array>
#include <fstream>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace Horo::Runtime {
    namespace {
        using namespace SaveFilesystemTest;
        using namespace SaveFilesystemReadDetails;

        TEST_CASE("Namespace lock ownership survives moves and unlocked stale files are reusable", "[unit][save][storage]") {
            StorageFixture fixture;
            const auto &root = fixture.root;
            const auto &name = fixture.name;
            const auto slots = root.CanonicalPath() / name.environment.ToString() / "server" /
                               std::get<ServerWorldOwner>(name.owner).owner.ToString() / "slots";
            {
                auto opened = SaveFilesystemStorage::Open(root, name);
                REQUIRE(opened.HasValue());
                auto storage = std::move(opened).Value();
                auto moved = std::move(storage);
                const auto competing = SaveFilesystemStorage::Open(root, name);
                REQUIRE(competing.HasError());
                CHECK(competing.ErrorValue().code.Value() == SaveErrors::OperationInProgress.code.Value());
                const auto other = SaveNamespaceId{.product = Product(),
                                                   .environment = name.environment,
                                                   .owner = ServerWorldOwner{Test::Id<ServerStorageOwnerId>(4)}};
                CHECK(SaveFilesystemStorage::Open(root, other).HasValue());
                CHECK(std::filesystem::exists(slots / ".namespace.lock"));
            }
            const auto activeTemporary = slots / ".other-operation.temporary";
            {
                std::ofstream output(activeTemporary);
                output << "owned work";
            }
            {
                std::ofstream output(slots / ".namespace.lock");
                output << "stale owner metadata is not authority";
            }
            CHECK(SaveFilesystemStorage::Open(root, name).HasValue());
            CHECK(std::filesystem::exists(activeTemporary));
            CHECK(std::filesystem::exists(slots / ".namespace.lock"));
        }

        TEST_CASE("Pinned archive reads survive replacement deletion and storage shutdown", "[unit][save][storage]") {
            StorageFixture fixture;
            const auto slot = Test::Id<SaveGameSlotId>(5);
            const auto target = fixture.Slots() / (slot.ToString() + ".horosave");
            const std::vector first(4096, std::byte{1}), second(8192, std::byte{2});
            std::optional<PinnedArchiveRead> oldRead, newRead;
            {
                auto opened = SaveFilesystemStorage::Open(fixture.root, fixture.name);
                REQUIRE(opened.HasValue());
                auto storage = std::move(opened).Value();
                REQUIRE(storage.Replace(slot, first).HasValue());
#ifdef _WIN32
                ArchiveFile directory{::CreateFileW(fixture.Slots().c_str(), FILE_READ_ATTRIBUTES | FILE_TRAVERSE,
                                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
                REQUIRE(directory.IsValid());
#else
                ArchiveFile directory{::open(fixture.Slots().c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
                REQUIRE(directory.Fd() >= 0);
#endif
                auto oldPin = PinArchiveRead(directory, slot, first.size());
                REQUIRE(oldPin.HasValue());
                oldRead.emplace(std::move(oldPin).Value());
                const auto replaced = storage.Replace(slot, second);
                if (replaced.HasError()) {
                    INFO(replaced.ErrorValue().message);
                }
                REQUIRE(replaced.HasValue());
                REQUIRE(storage.Read(slot, second.size()).Value() == second);
                auto newPin = PinArchiveRead(directory, slot, second.size());
                REQUIRE(newPin.HasValue());
                newRead.emplace(std::move(newPin).Value());
                REQUIRE(std::filesystem::remove(target));
                CHECK(storage.Read(slot, second.size()).HasError());
            }
            // Both directory entries are retired and the namespace owner is destroyed before byte I/O.
            REQUIRE(std::move(*oldRead).Read().Value() == first);
            REQUIRE(std::move(*newRead).Read().Value() == second);
            CHECK(SaveFilesystemStorage::Open(fixture.root, fixture.name).HasValue());
        }

        TEST_CASE("Concurrent archive reads observe complete replacements", "[unit][save][storage]") {
            StorageFixture fixture;
            const auto &root = fixture.root;
            const auto &name = fixture.name;
            auto opened = SaveFilesystemStorage::Open(root, name);
            REQUIRE(opened.HasValue());
            auto storage = std::move(opened).Value();
            const auto slot = Test::Id<SaveGameSlotId>(5);
            const std::vector first(4096, std::byte{1}), second(8192, std::byte{2});
            REQUIRE(storage.Replace(slot, first).HasValue());
            bool writesValid = true, readsValid = true;
            std::thread writer([&] {
                for (int index = 0; index < 20; ++index)
                    if (storage.Replace(slot, index % 2 == 0 ? second : first).HasError())
                        writesValid = false;
            });
            std::thread reader([&] {
                for (int index = 0; index < 40; ++index) {
                    const auto bytes = storage.Read(slot, second.size());
                    if (bytes.HasError() || (bytes.Value() != first && bytes.Value() != second))
                        readsValid = false;
                }
            });
            writer.join();
            reader.join();
            CHECK(writesValid);
            CHECK(readsValid);
        }

#ifndef _WIN32
        TEST_CASE("Process termination releases namespace ownership without deleting active save artifacts", "[unit][save][storage]") {
            StorageFixture fixture;
            const auto &root = fixture.root;
            const auto &name = fixture.name;
            {
                auto initialized = SaveFilesystemStorage::Open(root, name);
                REQUIRE(initialized.HasValue());
            }
            const auto artifact = fixture.Slots() / ".in-progress.temporary";
            {
                std::ofstream output(artifact);
                output << "operation owned";
            }
            int ready[2];
            REQUIRE(::pipe(ready) == 0);
            const pid_t child = ::fork();
            REQUIRE(child >= 0);
            if (child == 0) {
                ::close(ready[0]);
                auto opened = SaveFilesystemStorage::Open(root, name);
                const char status = opened.HasValue() ? 'Y' : 'N';
                if (::write(ready[1], &status, 1) != 1)
                    ::_exit(2);
                ::close(ready[1]);
                for (;;)
                    ::pause();
            }
            ::close(ready[1]);
            char status{};
            // flawfinder: ignore - exactly one byte into a one-byte object, without a loop or variable-length input.
            const auto received = ::read(ready[0], &status, 1);
            ::close(ready[0]);
            const auto competing = SaveFilesystemStorage::Open(root, name);
            const bool preservedWhileLive = std::filesystem::exists(artifact);
            const auto killed = ::kill(child, SIGKILL);
            int exitStatus{};
            const auto waited = ::waitpid(child, &exitStatus, 0);
            REQUIRE(received == 1);
            REQUIRE(status == 'Y');
            REQUIRE(competing.HasError());
            CHECK(competing.ErrorValue().code.Value() == SaveErrors::OperationInProgress.code.Value());
            REQUIRE(killed == 0);
            REQUIRE(waited == child);
            CHECK(WIFSIGNALED(exitStatus));
            CHECK(preservedWhileLive);
            CHECK(SaveFilesystemStorage::Open(root, name).HasValue());
            CHECK(std::filesystem::exists(artifact));
        }
#endif

        TEST_CASE("Namespace locks reject linked and replaced ownership files", "[unit][save][storage]") {
            StorageFixture fixture;
            const auto lockPath = fixture.Slots() / ".namespace.lock";
            SECTION("Replacement invalidates an existing owner") {
                auto opened = SaveFilesystemStorage::Open(fixture.root, fixture.name);
                REQUIRE(opened.HasValue());
                auto storage = std::move(opened).Value();
                std::filesystem::rename(lockPath, fixture.Slots() / ".retired.lock");
                {
                    std::ofstream output(lockPath);
                    output << "replacement";
                }
                const std::array bytes{std::byte{1}};
                CHECK(storage.Replace(Test::Id<SaveGameSlotId>(5), bytes).HasError());
                CHECK(storage.Read(Test::Id<SaveGameSlotId>(5), 1).HasError());
            }
            SECTION("Redirected locks cannot acquire ownership") {
                {
                    auto initialized = SaveFilesystemStorage::Open(fixture.root, fixture.name);
                    REQUIRE(initialized.HasValue());
                }
                REQUIRE(std::filesystem::remove(lockPath));
                const auto outside = fixture.temporary.Path() / "outside";
                {
                    std::ofstream output(outside);
                    output << "protected";
                }
                std::error_code error;
                std::filesystem::create_symlink(outside, lockPath, error);
                if (!error) {
                    CHECK(SaveFilesystemStorage::Open(fixture.root, fixture.name).HasError());
                    REQUIRE(std::filesystem::remove(lockPath));
                }
                std::filesystem::create_hard_link(outside, lockPath, error);
                REQUIRE_FALSE(error);
                CHECK(SaveFilesystemStorage::Open(fixture.root, fixture.name).HasError());
                std::ifstream input(outside);
                std::string content;
                input >> content;
                CHECK(content == "protected");
            }
        }

    }  // namespace
}  // namespace Horo::Runtime
