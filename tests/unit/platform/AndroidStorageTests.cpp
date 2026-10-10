#include "AndroidStorageAdapter.h"
#include "Horo/Platform/AndroidStorage.h"
#include "support/NativePublicationFiles.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <fstream>
#include <thread>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
    using namespace Horo;
    using namespace Horo::Platform;
    using Android::AndroidStorageAdapter;

    struct Files final : TestSupport::NativePublicationFiles {
        bool noCapacity{};
        bool interrupted{};
        bool afterCommit{};

        Result<std::uint64_t> AvailableBytes(const std::filesystem::path &path) const override {
            return noCapacity ? Result<std::uint64_t>::Success(0) : native.AvailableBytes(path);
        }

        Result<void> WriteDurable(const std::filesystem::path &path, const std::span<const std::byte> bytes) override {
            if (interrupted) {
                auto written = native.WriteDurable(path, bytes.first(bytes.size() / 2));
                REQUIRE(written.HasValue());
                return Result<void>::Failure(AndroidStorageAdapter::AccessFailure(false, false));
            }
            return native.WriteDurable(path, bytes);
        }

        Result<void> AtomicReplaceTracked(const std::filesystem::path &prepared, const std::filesystem::path &destination,
                                          AtomicFileReplacementReceipt &receipt) override {
            auto result = native.AtomicReplaceTracked(prepared, destination, receipt);
            if (afterCommit && result.HasValue())
                return Result<void>::Failure(AndroidStorageAdapter::AccessFailure(false, false));
            return result;
        }
    };

    struct Fixture {
        Files files;
        std::filesystem::path root;
        std::unique_ptr<AndroidStorage> storage;

        Fixture() {
            static std::atomic<unsigned> next{};
            root = std::filesystem::temp_directory_path() /
                   ("horo-android-storage-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                    std::to_string(next.fetch_add(1)));
            std::filesystem::create_directories(root / "private");
            std::filesystem::create_directories(root / "cache");
            auto created = AndroidStorageAdapter::Create(files, root / "private", root / "cache");
            REQUIRE(created.HasValue());
            storage = std::move(created).Value();
        }

        ~Fixture() {
            storage.reset();
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }

        void Publish(std::string_view name, std::string_view content, AndroidStorageRoot location = AndroidStorageRoot::AppPrivate) {
            AtomicFileReplacementReceipt receipt;
            REQUIRE(storage->Publish(location, name, std::as_bytes(std::span(content)), receipt).HasValue());
            REQUIRE(receipt.WasCommitted());
        }

        std::string Read(std::string_view name, AndroidStorageRoot location = AndroidStorageRoot::AppPrivate) {
            const auto read = storage->Read(location, name, 4096);
            REQUIRE(read.HasValue());
            return {reinterpret_cast<const char *>(read.Value().data()), read.Value().size()};
        }
    };

    TEST_CASE("Android logical roots preserve Unicode and spaces through native durable publication", "[unit][platform][android]") {
        Fixture fixture;
        fixture.Publish("saved games/İstanbul 世界.dat", "original");
        fixture.Publish("saved games/İstanbul 世界.dat", "replacement");
        REQUIRE(fixture.Read("saved games/İstanbul 世界.dat") == "replacement");
        fixture.Publish("cached file.dat", "cache", AndroidStorageRoot::Cache);
        REQUIRE(fixture.Read("cached file.dat", AndroidStorageRoot::Cache) == "cache");
        REQUIRE(fixture.storage->AvailableBytes(AndroidStorageRoot::Cache).HasValue());
        REQUIRE(fixture.storage->AvailableBytes(AndroidStorageRoot::PackagedAssets).HasError());
    }

    TEST_CASE("Android names reject traversal absolute paths URIs and transaction aliases", "[unit][platform][android]") {
        Fixture fixture;
        const std::array names{"../escape",
                               "nested/../escape",
                               "/absolute",
                               "content://provider/file",
                               "C:/file",
                               "nested\\file",
                               "file.horo-android-stage",
                               "nested//file",
                               "nested/",
                               "./file"};
        for (const auto *name : names) {
            AtomicFileReplacementReceipt receipt;
            REQUIRE(fixture.storage->Publish(AndroidStorageRoot::AppPrivate, name, {}, receipt).HasError());
            REQUIRE_FALSE(receipt.WasCommitted());
            REQUIRE(fixture.storage->Read(AndroidStorageRoot::AppPrivate, name, 4096).HasError());
        }
        REQUIRE(fixture.storage->Read(AndroidStorageRoot::Cache, std::string_view("a\0b", 3), 32).HasError());
        REQUIRE(fixture.storage->Read(AndroidStorageRoot::Cache, std::string_view("\xC0\xAF", 2), 32).HasError());
        AtomicFileReplacementReceipt receipt;
        REQUIRE(fixture.storage->Publish(AndroidStorageRoot::PackagedAssets, "asset", {}, receipt).HasError());
        REQUIRE(fixture.storage->Read(AndroidStorageRoot::AppPrivate, "missing", 64).ErrorValue().code.Value() ==
                "android_storage.not_found");
    }

    TEST_CASE("Android byte limits and storage pressure preserve the published file", "[unit][platform][android]") {
        Fixture fixture;
        fixture.Publish("save", "original");
        REQUIRE(fixture.storage->Read(AndroidStorageRoot::AppPrivate, "save", 7).HasError());
        REQUIRE(fixture.storage->Read(AndroidStorageRoot::AppPrivate, "save", 8).HasValue());
        REQUIRE(fixture.storage->Read(AndroidStorageRoot::AppPrivate, "save", 0).HasError());
        fixture.files.noCapacity = true;
        AtomicFileReplacementReceipt receipt;
        const std::string replacement = "replacement";
        const auto published =
            fixture.storage->Publish(AndroidStorageRoot::AppPrivate, "save", std::as_bytes(std::span(replacement)), receipt);
        REQUIRE(published.HasError());
        REQUIRE(published.ErrorValue().code.Value() == "android_storage.storage_pressure");
        REQUIRE_FALSE(receipt.WasCommitted());
        REQUIRE(fixture.Read("save") == "original");
    }

    TEST_CASE("Android interrupted stage recovers under writer authority and postcommit failure preserves receipt",
              "[unit][platform][android]") {
        Fixture fixture;
        fixture.Publish("save", "original");
        fixture.files.interrupted = true;
        const std::string replacement = "replacement";
        AtomicFileReplacementReceipt failed;
        REQUIRE(fixture.storage->Publish(AndroidStorageRoot::AppPrivate, "save", std::as_bytes(std::span(replacement)), failed).HasError());
        REQUIRE_FALSE(failed.WasCommitted());
        REQUIRE(fixture.Read("save") == "original");
        REQUIRE(std::filesystem::exists(fixture.root / "private/save.horo-android-stage"));
        fixture.files.interrupted = false;
        fixture.Publish("save", replacement);
        REQUIRE_FALSE(std::filesystem::exists(fixture.root / "private/save.horo-android-stage"));
        fixture.files.afterCommit = true;
        AtomicFileReplacementReceipt committed;
        const std::string newer = "newer";
        REQUIRE(fixture.storage->Publish(AndroidStorageRoot::AppPrivate, "save", std::as_bytes(std::span(newer)), committed).HasError());
        REQUIRE(committed.WasCommitted());
        REQUIRE(fixture.Read("save") == newer);
    }

    TEST_CASE("Android storage rejects symlink escapes and aliased staging files", "[unit][platform][android]") {
        Fixture fixture;
        std::filesystem::create_directories(fixture.root / "outside");
        std::error_code error;
        std::filesystem::create_directory_symlink(fixture.root / "outside", fixture.root / "private/link", error);
        if (error) {
            SKIP("Host does not permit symlink creation.");
        }
        AtomicFileReplacementReceipt receipt;
        REQUIRE(fixture.storage->Publish(AndroidStorageRoot::AppPrivate, "link/escape", {}, receipt).HasError());
        REQUIRE_FALSE(std::filesystem::exists(fixture.root / "outside/escape"));
        fixture.Publish("save", "original");
        std::filesystem::create_symlink(fixture.root / "private/save", fixture.root / "private/other.horo-android-stage");
        REQUIRE(fixture.storage->Publish(AndroidStorageRoot::AppPrivate, "other", {}, receipt).HasError());
        std::filesystem::create_hard_link(fixture.root / "private/save", fixture.root / "private/hard");
        REQUIRE(fixture.storage->Read(AndroidStorageRoot::AppPrivate, "hard", 4096).HasError());
    }

#if !defined(_WIN32)
    TEST_CASE("Android filesystem locations promptly reject actual FIFO nodes without opening them", "[unit][platform][android]") {
        Fixture fixture;
        REQUIRE(mkfifo((fixture.root / "private/pipe").c_str(), 0600) == 0);
        REQUIRE(mkfifo((fixture.root / "cache/pipe").c_str(), 0600) == 0);
        const auto started = std::chrono::steady_clock::now();
        const auto privateRead = fixture.storage->Read(AndroidStorageRoot::AppPrivate, "pipe", 32);
        const auto cacheRead = fixture.storage->Read(AndroidStorageRoot::Cache, "pipe", 32);
        REQUIRE(privateRead.HasError());
        REQUIRE(cacheRead.HasError());
        REQUIRE(privateRead.ErrorValue().code.Value() == "android_storage.invalid_name");
        REQUIRE(cacheRead.ErrorValue().code.Value() == "android_storage.invalid_name");
        REQUIRE(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));
        AtomicFileReplacementReceipt receipt;
        REQUIRE(fixture.storage->Publish(AndroidStorageRoot::AppPrivate, "pipe", {}, receipt).HasError());
        REQUIRE_FALSE(receipt.WasCommitted());
    }

    TEST_CASE("Android document capability consumes actual ContentResolver style descriptors without path conversion",
              "[unit][platform][android]") {
        Fixture fixture;
        fixture.Publish("selected 世界 file", "document bytes");
        const auto path = fixture.root / "private/selected 世界 file";
        const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        REQUIRE(descriptor >= 0);
        auto admitted = AndroidStorageAdapter::AdoptDocument(*fixture.storage, descriptor, AndroidDocumentGrantLifetime::Persisted);
        REQUIRE(admitted.HasValue());
        auto read = fixture.storage->ReadDocument(admitted.Value(), 4096);
        REQUIRE(read.HasValue());
        REQUIRE(read.Value().size() == 14);
        REQUIRE(fcntl(descriptor, F_GETFD) == -1);
        REQUIRE(fixture.storage->ReadDocument(admitted.Value(), 4096).HasError());
        REQUIRE(fixture.storage->ReadDocument(AndroidDocumentHandle{}, 4096).HasError());
        Fixture other;
        REQUIRE(other.storage->ReadDocument(admitted.Value(), 4096).HasError());
        const auto retired = admitted.Value();
        fixture.storage.reset();
        auto replacement = AndroidStorageAdapter::Create(fixture.files, fixture.root / "private", fixture.root / "cache");
        REQUIRE(replacement.HasValue());
        fixture.storage = std::move(replacement).Value();
        auto fresh =
            AndroidStorageAdapter::AdoptDocument(*fixture.storage, open(path.c_str(), O_RDONLY), AndroidDocumentGrantLifetime::Persisted);
        REQUIRE(fresh.HasValue());
        REQUIRE(fixture.storage->ReadDocument(retired, 4096).HasError());
        REQUIRE(AndroidStorageAdapter::Revoke(*fixture.storage, retired).HasError());
        REQUIRE(fixture.storage->ReadDocument(fresh.Value(), 4096).HasValue());
        REQUIRE(AndroidStorageAdapter::AdoptDocument(*fixture.storage, -1, AndroidDocumentGrantLifetime::Activity).HasError());
    }

    TEST_CASE("Android pipe-backed document supports bounded streams and revocation during access", "[unit][platform][android]") {
        Fixture fixture;
        int descriptors[2]{};
        REQUIRE(pipe(descriptors) == 0);
        const auto admitted =
            AndroidStorageAdapter::AdoptDocument(*fixture.storage, descriptors[0], AndroidDocumentGrantLifetime::Activity);
        REQUIRE(admitted.HasValue());
        const char content[] = "payload";
        REQUIRE(write(descriptors[1], content, 7) == 7);
        REQUIRE(close(descriptors[1]) == 0);
        REQUIRE(fixture.storage->ReadDocument(admitted.Value(), 7).HasValue());

        REQUIRE(pipe(descriptors) == 0);
        const auto waiting = AndroidStorageAdapter::AdoptDocument(*fixture.storage, descriptors[0], AndroidDocumentGrantLifetime::Activity);
        REQUIRE(waiting.HasValue());
        std::atomic<bool> revoked{};
        std::thread callback([&] {
            while ((fcntl(descriptors[0], F_GETFL) & O_NONBLOCK) == 0)
                std::this_thread::yield();
            revoked.store(AndroidStorageAdapter::Revoke(*fixture.storage, waiting.Value()).HasValue(), std::memory_order_release);
        });
        const auto interrupted = fixture.storage->ReadDocument(waiting.Value(), 32);
        callback.join();
        REQUIRE(close(descriptors[1]) == 0);
        REQUIRE(revoked.load(std::memory_order_acquire));
        REQUIRE(interrupted.HasError());
        REQUIRE(interrupted.ErrorValue().code.Value() == "android_storage.revoked");
        REQUIRE(fcntl(descriptors[0], F_GETFD) == -1);
    }

    TEST_CASE("Android activity grant retirement preserves persisted grants and oversized documents close descriptors",
              "[unit][platform][android]") {
        Fixture fixture;
        fixture.Publish("document", "bytes");
        auto activity = AndroidStorageAdapter::AdoptDocument(*fixture.storage, open((fixture.root / "private/document").c_str(), O_RDONLY),
                                                             AndroidDocumentGrantLifetime::Activity);
        auto persisted = AndroidStorageAdapter::AdoptDocument(*fixture.storage, open((fixture.root / "private/document").c_str(), O_RDONLY),
                                                              AndroidDocumentGrantLifetime::Persisted);
        REQUIRE(activity.HasValue());
        REQUIRE(persisted.HasValue());
        REQUIRE(AndroidStorageAdapter::RetireActivityGrants(*fixture.storage).HasValue());
        REQUIRE(fixture.storage->ReadDocument(activity.Value(), 32).ErrorValue().code.Value() == "android_storage.revoked");
        REQUIRE(fixture.storage->ReadDocument(persisted.Value(), 32).HasValue());
        const int descriptor = open((fixture.root / "private/document").c_str(), O_RDONLY);
        const auto oversized = AndroidStorageAdapter::AdoptDocument(*fixture.storage, descriptor, AndroidDocumentGrantLifetime::Persisted);
        REQUIRE(oversized.HasValue());
        REQUIRE(fixture.storage->ReadDocument(oversized.Value(), 4).ErrorValue().code.Value() == "android_storage.too_large");
        REQUIRE(fcntl(descriptor, F_GETFD) == -1);
        REQUIRE(AndroidStorageAdapter::AccessFailure(true, false).code.Value() == "android_storage.revoked");
        REQUIRE(AndroidStorageAdapter::AccessFailure(false, false).code.Value() == "android_storage.access_denied");
    }
#endif
}  // namespace
