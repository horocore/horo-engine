#include "Horo/Assets/AssetPreviewService.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::Assets;

    class CountingPreviewProvider final : public IAssetPreviewProvider {
    public:
        [[nodiscard]] Result<AssetPreviewImage> GeneratePreview(const AssetPreviewInput &input,
                                                                const CancellationToken &cancellation) const override {
            ++calls;
            if (cancellation.IsCancellationRequested())
                return Result<AssetPreviewImage>::Success({});
            AssetPreviewImage image{
                .width = input.width,
                .height = input.height,
                .pixels = std::vector<std::uint8_t>(static_cast<std::size_t>(input.width) * input.height * 4U, 0x7f),
            };
            return Result<AssetPreviewImage>::Success(std::move(image));
        }

        mutable std::atomic<std::uint32_t> calls{};
    };

    class CancellablePreviewProvider final : public IAssetPreviewProvider {
    public:
        [[nodiscard]] Result<AssetPreviewImage> GeneratePreview(const AssetPreviewInput &,
                                                                const CancellationToken &cancellation) const override {
            entered.store(true);
            while (!cancellation.IsCancellationRequested())
                std::this_thread::yield();
            return Result<AssetPreviewImage>::Success({});
        }

        mutable std::atomic<bool> entered{};
    };

    class InvalidPreviewProvider final : public IAssetPreviewProvider {
    public:
        [[nodiscard]] Result<AssetPreviewImage> GeneratePreview(const AssetPreviewInput &, const CancellationToken &) const override {
            return Result<AssetPreviewImage>::Success({.width = 2, .height = 2, .pixels = {1, 2, 3}});
        }
    };

    const ErrorCodeDescriptor PreviewProviderFailure{
        .domain = ErrorDomainId{"test.preview"},
        .code = ErrorCode{"test.preview.failed"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Injected preview failure.",
    };

    class FailingPreviewProvider final : public IAssetPreviewProvider {
    public:
        [[nodiscard]] Result<AssetPreviewImage> GeneratePreview(const AssetPreviewInput &, const CancellationToken &) const override {
            return Result<AssetPreviewImage>::Failure(MakeError(PreviewProviderFailure));
        }
    };

    class ThrowingPreviewProvider final : public IAssetPreviewProvider {
    public:
        [[nodiscard]] Result<AssetPreviewImage> GeneratePreview(const AssetPreviewInput &, const CancellationToken &) const override {
            throw std::runtime_error{"injected preview exception"};
        }
    };

    class TemporaryAsset final {
    public:
        explicit TemporaryAsset(const std::vector<std::uint8_t> &bytes) {
            static std::atomic<std::uint64_t> nextId{};
            path_ = std::filesystem::temp_directory_path() /
                    ("horo-preview-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                     std::to_string(nextId.fetch_add(1)) + ".horoasset");
            std::ofstream output(path_, std::ios::binary);
            output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }

        ~TemporaryAsset() {
            std::error_code error;
            std::filesystem::remove(path_, error);
        }

        [[nodiscard]] const std::filesystem::path &Path() const noexcept {
            return path_;
        }

    private:
        std::filesystem::path path_;
    };

    [[nodiscard]] AssetPreviewRequest MakeRequest(const std::filesystem::path &path,
                                                  std::shared_ptr<const IAssetPreviewProvider> provider) {
        auto type = AssetTypeId::Parse("core.mesh");
        REQUIRE(type.HasValue());
        return AssetPreviewRequest{
            .contributionId = "test.preview",
            .moduleId = "test.module",
            .moduleVersion = "1.0.0",
            .providerVersion = "1.0.0",
            .absoluteAssetPath = path,
            .assetType = std::move(type).Value(),
            .width = 2,
            .height = 2,
            .provider = std::move(provider),
        };
    }

    [[nodiscard]] Result<AssetPreviewResult> RunPreview(AssetPreviewService &service, AssetPreviewRequest request) {
        auto submitted = service.Submit(std::move(request));
        REQUIRE(submitted.HasValue());
        AssetPreviewHandle handle = std::move(submitted).Value();
        REQUIRE(handle.Wait().HasValue());
        return handle.TakeResult();
    }

    void RequirePreviewError(std::shared_ptr<const IAssetPreviewProvider> provider, const std::string_view expectedCode) {
        JobSystem jobs{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 2}};
        AssetPreviewService service{jobs};
        TemporaryAsset asset{{1, 2, 3, 4}};
        const auto result = RunPreview(service, MakeRequest(asset.Path(), std::move(provider)));
        REQUIRE(result.HasError());
        REQUIRE(result.ErrorValue().code.Value() == expectedCode);
    }
}  // namespace

TEST_CASE("Asset preview service reuses a content-addressed provider result", "[unit][assets][preview]") {
    JobSystem jobs{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 4}};
    AssetPreviewService service{jobs};
    TemporaryAsset asset{{1, 2, 3, 4}};
    const auto provider = std::make_shared<CountingPreviewProvider>();

    auto firstResult = RunPreview(service, MakeRequest(asset.Path(), provider));
    REQUIRE(firstResult.HasValue());
    REQUIRE_FALSE(firstResult.Value().cacheHit);
    REQUIRE(firstResult.Value().image.IsValid());

    auto secondResult = RunPreview(service, MakeRequest(asset.Path(), provider));
    REQUIRE(secondResult.HasValue());
    REQUIRE(secondResult.Value().cacheHit);
    REQUIRE(provider->calls.load() == 1);
}

TEST_CASE("Asset preview cancellation retains the provider through terminal completion", "[unit][assets][preview]") {
    JobSystem jobs{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 2}};
    AssetPreviewService service{jobs};
    TemporaryAsset asset{{1, 2, 3, 4}};
    auto provider = std::make_shared<CancellablePreviewProvider>();
    std::weak_ptr<CancellablePreviewProvider> lifetime = provider;

    auto submitted = service.Submit(MakeRequest(asset.Path(), provider));
    REQUIRE(submitted.HasValue());
    AssetPreviewHandle handle = std::move(submitted).Value();
    provider.reset();
    while (!lifetime.lock()->entered.load())
        std::this_thread::yield();
    REQUIRE_FALSE(lifetime.expired());
    REQUIRE(handle.RequestCancel().HasValue());
    REQUIRE(handle.Wait().HasValue());
    REQUIRE(handle.State() == AssetPreviewState::Cancelled);
    auto result = handle.TakeResult();
    REQUIRE(result.HasError());
    REQUIRE(result.ErrorValue().code.Value() == "asset.preview.cancelled");

    handle = {};
    service.Shutdown();
    REQUIRE(lifetime.expired());
}

TEST_CASE("Asset preview service rejects oversized input before provider execution", "[unit][assets][preview]") {
    JobSystem jobs{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 2}};
    AssetPreviewService service{jobs, AssetPreviewServiceLimits{.maximumInputBytes = 3}};
    TemporaryAsset asset{{1, 2, 3, 4}};
    const auto provider = std::make_shared<CountingPreviewProvider>();

    auto result = RunPreview(service, MakeRequest(asset.Path(), provider));
    REQUIRE(result.HasError());
    REQUIRE(result.ErrorValue().code.Value() == "asset.preview.input_too_large");
    REQUIRE(provider->calls.load() == 0);
}

TEST_CASE("Asset preview service rejects malformed provider output", "[unit][assets][preview]") {
    RequirePreviewError(std::make_shared<InvalidPreviewProvider>(), "asset.preview.output_invalid");
}

TEST_CASE("Asset preview service contains provider exceptions", "[unit][assets][preview]") {
    RequirePreviewError(std::make_shared<ThrowingPreviewProvider>(), "asset.preview.provider_failed");
}

TEST_CASE("Asset preview service enforces request and queue bounds", "[unit][assets][preview]") {
    JobSystem jobs{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 2}};
    AssetPreviewService service{jobs, AssetPreviewServiceLimits{.maximumOutstanding = 0, .maximumDimension = 64}};
    TemporaryAsset asset{{1, 2, 3, 4}};
    const auto provider = std::make_shared<CountingPreviewProvider>();

    auto request = MakeRequest(asset.Path(), provider);
    request.width = 65;
    auto invalid = service.Submit(std::move(request));
    REQUIRE(invalid.HasError());
    REQUIRE(invalid.ErrorValue().code.Value() == "asset.preview.request_invalid");

    auto full = service.Submit(MakeRequest(asset.Path(), provider));
    REQUIRE(full.HasError());
    REQUIRE(full.ErrorValue().code.Value() == "asset.preview.queue_full");
}

TEST_CASE("Asset preview handles expose not-ready, failure, and consumed states", "[unit][assets][preview]") {
    JobSystem jobs{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 2}};
    AssetPreviewService service{jobs};
    TemporaryAsset asset{{1, 2, 3, 4}};

    const auto blockingProvider = std::make_shared<CancellablePreviewProvider>();
    auto blocking = service.Submit(MakeRequest(asset.Path(), blockingProvider));
    REQUIRE(blocking.HasValue());
    AssetPreviewHandle blockingHandle = std::move(blocking).Value();
    while (!blockingProvider->entered.load())
        std::this_thread::yield();
    auto early = blockingHandle.TakeResult();
    REQUIRE(early.HasError());
    REQUIRE(early.ErrorValue().code.Value() == "asset.preview.not_ready");
    REQUIRE(blockingHandle.RequestCancel().HasValue());
    REQUIRE(blockingHandle.Wait().HasValue());

    auto submitted = service.Submit(MakeRequest(asset.Path(), std::make_shared<FailingPreviewProvider>()));
    REQUIRE(submitted.HasValue());
    AssetPreviewHandle handle = std::move(submitted).Value();
    REQUIRE(handle.Wait().HasValue());
    auto failed = handle.TakeResult();
    REQUIRE(failed.HasError());
    REQUIRE(failed.ErrorValue().code.Value() == "test.preview.failed");
    auto consumed = handle.TakeResult();
    REQUIRE(consumed.HasError());
    REQUIRE(consumed.ErrorValue().code.Value() == "asset.preview.consumed");
}

TEST_CASE("Asset preview cache identity includes payload, provider version, and dimensions", "[unit][assets][preview]") {
    JobSystem jobs{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 4}};
    AssetPreviewService service{jobs};
    TemporaryAsset firstAsset{{1, 2, 3, 4}};
    TemporaryAsset secondAsset{{4, 3, 2, 1}};
    const auto provider = std::make_shared<CountingPreviewProvider>();

    const auto run = [&service](AssetPreviewRequest request) {
        auto result = RunPreview(service, std::move(request));
        REQUIRE(result.HasValue());
        return result.Value().cacheHit;
    };

    REQUIRE_FALSE(run(MakeRequest(firstAsset.Path(), provider)));
    REQUIRE_FALSE(run(MakeRequest(secondAsset.Path(), provider)));
    auto newerProvider = MakeRequest(firstAsset.Path(), provider);
    newerProvider.providerVersion = "2.0.0";
    REQUIRE_FALSE(run(std::move(newerProvider)));
    auto wider = MakeRequest(firstAsset.Path(), provider);
    wider.width = 3;
    REQUIRE_FALSE(run(std::move(wider)));
    REQUIRE(provider->calls.load() == 4);
}

TEST_CASE("Asset preview cache evicts least recently used entries within configured bounds", "[unit][assets][preview]") {
    JobSystem jobs{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 4}};
    AssetPreviewService service{jobs, AssetPreviewServiceLimits{.maximumCacheEntries = 1, .maximumCacheBytes = 64}};
    TemporaryAsset firstAsset{{1, 2, 3, 4}};
    TemporaryAsset secondAsset{{4, 3, 2, 1}};
    const auto provider = std::make_shared<CountingPreviewProvider>();

    for (const std::filesystem::path &path : {firstAsset.Path(), secondAsset.Path(), firstAsset.Path()}) {
        REQUIRE(RunPreview(service, MakeRequest(path, provider)).HasValue());
    }
    REQUIRE(provider->calls.load() == 3);
}

TEST_CASE("Asset preview service validates files, cancellation ancestry, and shutdown", "[unit][assets][preview]") {
    JobSystem jobs{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 2}};
    AssetPreviewService service{jobs};
    TemporaryAsset asset{{1, 2, 3, 4}};
    const auto provider = std::make_shared<CountingPreviewProvider>();

    AssetPreviewHandle empty;
    REQUIRE(empty.State() == AssetPreviewState::Failed);
    REQUIRE(empty.RequestCancel().HasError());
    REQUIRE(empty.Wait().HasError());
    REQUIRE(empty.TakeResult().HasError());

    auto relative = MakeRequest(asset.Path(), provider);
    relative.absoluteAssetPath = "relative.horoasset";
    REQUIRE(service.Submit(std::move(relative)).HasError());

    CancellationSource cancelled;
    cancelled.RequestCancellation();
    auto rejected = service.Submit(MakeRequest(asset.Path(), provider), cancelled.Token());
    REQUIRE(rejected.HasError());
    REQUIRE(rejected.ErrorValue().code.Value() == "asset.preview.cancelled");

    const std::filesystem::path missing = asset.Path().parent_path() / "missing-preview.horoasset";
    auto missingResult = RunPreview(service, MakeRequest(missing, provider));
    REQUIRE(missingResult.HasError());
    REQUIRE(missingResult.ErrorValue().code.Value() == "asset.preview.read_failed");

    service.Shutdown();
    auto afterShutdown = service.Submit(MakeRequest(asset.Path(), provider));
    REQUIRE(afterShutdown.HasError());
    REQUIRE(afterShutdown.ErrorValue().code.Value() == "asset.preview.shutdown");
}
