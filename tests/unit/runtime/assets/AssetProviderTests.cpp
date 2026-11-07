#include "AssetProviderRead.h"
#include "Horo/Assets/AssetProvider.h"
#include "Horo/Foundation/Platform.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

namespace {
    using namespace Horo;
    using namespace Horo::Assets;

    AssetId Id(const std::string_view value) {
        auto parsed = AssetId::Parse(value);
        REQUIRE((parsed.HasValue()));
        return parsed.Value();
    }

    AssetRecord Record(const AssetId id) {
        auto type = AssetTypeId::Parse("core.mesh");
        auto source = ProjectPath::Parse("assets/models/mesh.obj");
        auto metadata = ProjectPath::Parse("assets/models/mesh.obj.horo");
        REQUIRE((type.HasValue() && source.HasValue() && metadata.HasValue()));
        return {id, type.Value(), source.Value(), metadata.Value()};
    }

    class BlockingProvider final : public IAssetProvider {
    public:
        Result<bool> Exists(AssetId, const CancellationToken &cancellation) const override {
            return cancellation.IsCancellationRequested()
                       ? Result<bool>::Failure(
                             Error{ErrorCode{"asset.load.cancelled"}, ErrorDomainId{"horo.asset"}, ErrorSeverity::Error, "cancelled", {}})
                       : Result<bool>::Success(true);
        }

        Result<std::vector<std::uint8_t>> Load(AssetId, const CancellationToken &cancellation) const override {
            entered.store(true, std::memory_order_release);
            while (!release.load(std::memory_order_acquire) && !cancellation.IsCancellationRequested())
                std::this_thread::yield();
            if (cancellation.IsCancellationRequested())
                return Result<std::vector<std::uint8_t>>::Failure(
                    Error{ErrorCode{"asset.load.cancelled"}, ErrorDomainId{"horo.asset"}, ErrorSeverity::Error, "cancelled", {}});
            return Result<std::vector<std::uint8_t>>::Success({7, 8, 9});
        }

        mutable std::atomic<bool> entered{};
        mutable std::atomic<bool> release{};
    };

    TEST_CASE("Memory And Filesystem Providers Remain Typed", "[unit][runtime][assets]") {
        const AssetId id = Id("00112233-4455-6677-8899-aabbccddeeff");
        CancellationSource cancellation;
        MemoryAssetProvider memory;
        memory.Insert(id, {1, 2, 3});
        REQUIRE((memory.Exists(id, cancellation.Token()).Value()));
        REQUIRE((memory.Load(id, cancellation.Token()).Value() == std::vector<std::uint8_t>({1, 2, 3})));
        REQUIRE((memory.Load(Id("11112233-4455-6677-8899-aabbccddeeff"), cancellation.Token()).HasError()));
        cancellation.RequestCancellation();
        REQUIRE((memory.Load(id, cancellation.Token()).ErrorValue().code.Value() == "asset.load.cancelled"));

        const auto root = std::filesystem::temp_directory_path() / "horo-asset-provider-tests";
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        {
            std::ofstream output(root / (id.ToString() + ".cooked"), std::ios::binary);
            output.write("abc", 3);
        }
        CancellationSource active;
        FilesystemAssetProvider files{root};
        REQUIRE((files.Load(id, active.Token()).Value() == std::vector<std::uint8_t>({'a', 'b', 'c'})));
        FilesystemAssetProvider bounded{root, AssetProviderLimits{2}};
        REQUIRE((bounded.Load(id, active.Token()).ErrorValue().code.Value() == "asset.provider.too_large"));
        std::filesystem::remove_all(root);
    }

    TEST_CASE("Filesystem provider loads bounded artifacts from long native cooked roots", "[unit][runtime][assets][long-path]") {
        NativeDurableFileSystem files;
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto root =
            std::filesystem::canonical(std::filesystem::temp_directory_path()) / ("horo-provider-long-" + std::to_string(unique));
        const auto created = files.CreateDirectoryPath(root, DirectoryCreationMode::Single);
        REQUIRE(created.HasValue());
        REQUIRE(created.Value());
        std::vector<std::filesystem::path> directories{root};
        for (unsigned index = 0; index < 4; ++index) {
            directories.push_back(directories.back() / (std::string(72, 'b') + " " + std::to_string(index)));
            REQUIRE(files.CreateDirectoryPath(directories.back(), DirectoryCreationMode::Single).HasValue());
        }
        const auto id = Id("00112233-4455-6677-8899-aabbccddeeff");
        const auto path = directories.back() / (id.ToString() + ".cooked");
        REQUIRE(path.native().size() > 260);
        const std::string contents{"retained cooked bytes"};
        REQUIRE(files.WriteDurable(path, std::as_bytes(std::span{contents})).HasValue());
        CancellationSource cancellation;
        FilesystemAssetProvider provider{directories.back()};
        REQUIRE(provider.Exists(id, cancellation.Token()).Value());
        const auto loaded = provider.Load(id, cancellation.Token());
        REQUIRE(loaded.HasValue());
        CHECK(std::string(loaded.Value().begin(), loaded.Value().end()) == contents);
        FilesystemAssetProvider bounded{directories.back(), AssetProviderLimits{2}};
        CHECK(bounded.Load(id, cancellation.Token()).HasError());
        cancellation.RequestCancellation();
        CHECK(provider.Load(id, cancellation.Token()).HasError());
        REQUIRE(files.RemoveDurable(path).HasValue());
        for (auto directory = directories.rbegin(); directory != directories.rend(); ++directory)
            REQUIRE(files.RemoveDurable(*directory).HasValue());
    }

    TEST_CASE("Exact Artifact Read Rejects Truncation", "[unit][runtime][assets]") {
        CancellationSource cancellation;
        std::istringstream truncated{"ab"};
        auto loaded = Internal::ReadExactArtifact(truncated, 3, cancellation.Token());
        REQUIRE((loaded.HasError()));
        REQUIRE((loaded.ErrorValue().code.Value() == "asset.provider.read_failed"));
    }

    TEST_CASE("Async Loads Capture Revision And Enforce Bounds", "[unit][runtime][assets]") {
        const AssetId id = Id("00112233-4455-6677-8899-aabbccddeeff");
        AssetRegistry registry;
        REQUIRE((registry.Publish({Record(id)}).status == AssetRegistryBuildStatus::Complete));
        const AssetRegistrySnapshot submittedSnapshot = registry.Snapshot();
        MemoryAssetProvider provider;
        provider.Insert(id, {4, 5, 6});
        JobSystem jobs{JobSystemConfig{2, 8}};
        AssetLoadService service{jobs, provider};
        auto requested = service.LoadAsync(submittedSnapshot, id);
        REQUIRE((requested.HasValue()));
        AssetLoadHandle handle = std::move(requested).Value();
        REQUIRE((handle.Wait().HasValue()));
        auto result = handle.TakeResult();
        REQUIRE((result.HasValue()));
        REQUIRE((result.Value().sourceRegistryRevision == submittedSnapshot.Revision()));
        REQUIRE((result.Value().bytes == std::vector<std::uint8_t>({4, 5, 6})));
        REQUIRE((handle.TakeResult().ErrorValue().code.Value() == "asset.load.consumed"));
        CancellationSource cancelledParent;
        cancelledParent.RequestCancellation();
        REQUIRE((service.LoadAsync(submittedSnapshot, id, cancelledParent.Token()).ErrorValue().code.Value() == "asset.load.cancelled"));
        service.Shutdown();
        REQUIRE((service.LoadAsync(registry.Snapshot(), id).ErrorValue().code.Value() == "asset.load.shutdown"));
        jobs.Shutdown(ShutdownPolicy::Drain);
    }

    TEST_CASE("Replacement Does Not Invalidate In Flight Loads And Cancellation Is Deterministic", "[unit][runtime][assets]") {
        const AssetId id = Id("00112233-4455-6677-8899-aabbccddeeff");
        AssetRegistry registry;
        REQUIRE((registry.Publish({Record(id)}).status == AssetRegistryBuildStatus::Complete));
        const AssetRegistrySnapshot oldSnapshot = registry.Snapshot();
        BlockingProvider provider;
        JobSystem jobs{JobSystemConfig{1, 8}};
        AssetLoadService service{jobs, provider, 1};
        auto requested = service.LoadAsync(oldSnapshot, id);
        REQUIRE((requested.HasValue()));
        AssetLoadHandle handle = std::move(requested).Value();
        while (!provider.entered.load(std::memory_order_acquire))
            std::this_thread::yield();
        REQUIRE((service.LoadAsync(oldSnapshot, id).ErrorValue().code.Value() == "asset.load.queue_full"));
        REQUIRE((registry.Publish({Record(id)}).status == AssetRegistryBuildStatus::Complete));
        provider.release.store(true, std::memory_order_release);
        REQUIRE((handle.Wait().HasValue()));
        auto completed = handle.TakeResult();
        REQUIRE((completed.HasValue()));
        REQUIRE((completed.Value().sourceRegistryRevision == oldSnapshot.Revision()));
        REQUIRE((completed.Value().sourceRegistryRevision != registry.Snapshot().Revision()));

        provider.entered.store(false, std::memory_order_release);
        provider.release.store(false, std::memory_order_release);
        auto cancelledRequest = service.LoadAsync(registry.Snapshot(), id);
        REQUIRE((cancelledRequest.HasValue()));
        AssetLoadHandle cancelled = std::move(cancelledRequest).Value();
        while (!provider.entered.load(std::memory_order_acquire))
            std::this_thread::yield();
        REQUIRE((cancelled.RequestCancel().HasValue()));
        REQUIRE((cancelled.Wait().HasValue()));
        REQUIRE((cancelled.TakeResult().ErrorValue().code.Value() == "asset.load.cancelled"));
        service.Shutdown();
        jobs.Shutdown(ShutdownPolicy::Drain);
        REQUIRE((handle.RequestCancel().ErrorValue().code.Value() == "asset.load.shutdown"));
    }

    TEST_CASE("Queued Cancellation And Shutdown Join Are Deterministic", "[unit][runtime][assets]") {
        const AssetId id = Id("00112233-4455-6677-8899-aabbccddeeff");
        AssetRegistry registry;
        REQUIRE((registry.Publish({Record(id)}).status == AssetRegistryBuildStatus::Complete));
        BlockingProvider provider;
        JobSystem jobs{JobSystemConfig{1, 8}};
        AssetLoadService service{jobs, provider, 2};

        auto runningRequest = service.LoadAsync(registry.Snapshot(), id);
        REQUIRE((runningRequest.HasValue()));
        AssetLoadHandle running = std::move(runningRequest).Value();
        while (!provider.entered.load(std::memory_order_acquire))
            std::this_thread::yield();

        auto queuedRequest = service.LoadAsync(registry.Snapshot(), id);
        REQUIRE((queuedRequest.HasValue()));
        AssetLoadHandle queued = std::move(queuedRequest).Value();
        REQUIRE((queued.RequestCancel().HasValue()));
        REQUIRE((queued.Wait().HasValue()));
        REQUIRE((queued.TakeResult().ErrorValue().code.Value() == "asset.load.cancelled"));

        service.Shutdown();
        REQUIRE((running.Wait().HasValue()));
        REQUIRE((running.TakeResult().ErrorValue().code.Value() == "asset.load.cancelled"));
        jobs.Shutdown(ShutdownPolicy::Drain);
        REQUIRE((running.RequestCancel().ErrorValue().code.Value() == "asset.load.shutdown"));
    }
}  // namespace
