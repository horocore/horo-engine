#include "Horo/Assets/AssetPreviewService.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
    using namespace Horo;
    using namespace Horo::Assets;

    class RevisionAsset final {
    public:
        RevisionAsset() {
            static std::atomic<unsigned> next{};
            path = std::filesystem::temp_directory_path() /
                   ("horo-preview-revision-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                    std::to_string(next.fetch_add(1)) + ".horoasset");
            std::ofstream output(path, std::ios::binary);
            output << "revision-test";
            REQUIRE(output.good());
        }

        ~RevisionAsset() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }

        RevisionAsset(const RevisionAsset &) = delete;
        RevisionAsset &operator=(const RevisionAsset &) = delete;
        std::filesystem::path path;
    };

    class RootOnlyProvider : public IAssetPreviewProvider {
    public:
        Result<AssetPreviewImage> GeneratePreview(const AssetPreviewInput &input, const CancellationToken &) const override {
            ++generations;
            sawRevision.store(input.capturedRevision.has_value());
            return Result<AssetPreviewImage>::Success(
                {.width = input.width,
                 .height = input.height,
                 .pixels = std::vector<std::uint8_t>(static_cast<std::size_t>(input.width) * input.height * 4, 127)});
        }

        mutable std::atomic<unsigned> generations{};
        mutable std::atomic<bool> sawRevision{};
    };

    class RevisionProvider final : public RootOnlyProvider {
    public:
        Result<AssetPreviewImage> GeneratePreview(const AssetPreviewInput &input, const CancellationToken &token) const override {
            const auto value = revision.load();
            correctProof.store(input.capturedRevision == ComputeSha256(std::as_bytes(std::span{&value, 1})));
            return RootOnlyProvider::GeneratePreview(input, token);
        }

        Result<std::optional<Sha256Digest>> CaptureRevision(const AssetPreviewInput &, const CancellationToken &) const override {
            const auto call = ++captures;
            if (throws)
                throw std::runtime_error("injected revision capture failure");
            if (absent)
                return Result<std::optional<Sha256Digest>>::Success(std::nullopt);
            if (call == changeOnCapture)
                ++revision;
            const auto value = revision.load();
            return Result<std::optional<Sha256Digest>>::Success(ComputeSha256(std::as_bytes(std::span{&value, 1})));
        }

        bool throws{};
        bool absent{};
        unsigned changeOnCapture{};
        mutable std::atomic<unsigned> captures{};
        mutable std::atomic<unsigned> revision{1};
        mutable std::atomic<bool> correctProof{};
    };

    AssetPreviewRequest RevisionRequest(const RevisionAsset &asset, std::shared_ptr<const IAssetPreviewProvider> provider,
                                        const bool required = false) {
        auto type = AssetTypeId::Parse("core.mesh");
        REQUIRE(type.HasValue());
        AssetPreviewRequest request{
            .contributionId = "test.preview.revision",
            .moduleId = "test.module",
            .moduleVersion = "1",
            .providerVersion = "1",
            .absoluteAssetPath = asset.path,
            .assetType = std::move(type).Value(),
            .width = 2,
            .height = 2,
            .provider = std::move(provider),
        };
        request.requiresRevision = required;
        return request;
    }

    Result<AssetPreviewResult> CompleteRevisionPreview(AssetPreviewService &service, AssetPreviewRequest request) {
        auto submitted = service.Submit(std::move(request));
        REQUIRE(submitted.HasValue());
        auto handle = std::move(submitted).Value();
        REQUIRE(handle.Wait().HasValue());
        REQUIRE(handle.State() != AssetPreviewState::Queued);
        REQUIRE(handle.State() != AssetPreviewState::Running);
        return handle.TakeResult();
    }
}  // namespace

TEST_CASE("Root-only preview providers retain cache compatibility without a revision hook", "[assets][preview][revision]") {
    JobSystem jobs{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 4}};
    AssetPreviewService service{jobs};
    RevisionAsset asset;
    const auto provider = std::make_shared<RootOnlyProvider>();
    const auto first = CompleteRevisionPreview(service, RevisionRequest(asset, provider));
    REQUIRE(first.HasValue());
    REQUIRE_FALSE(first.Value().cacheHit);
    const auto second = CompleteRevisionPreview(service, RevisionRequest(asset, provider));
    REQUIRE(second.HasValue());
    REQUIRE(second.Value().cacheHit);
    REQUIRE(provider->generations.load() == 1);
    REQUIRE_FALSE(provider->sawRevision.load());
}

TEST_CASE("Required absent or throwing preview revision capture terminates without generation", "[assets][preview][revision]") {
    JobSystem jobs{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 4}};
    AssetPreviewService service{jobs};
    RevisionAsset asset;
    const auto provider = std::make_shared<RevisionProvider>();
    SECTION("required proof unavailable") {
        provider->absent = true;
    }
    SECTION("provider throws") {
        provider->throws = true;
    }
    const auto result = CompleteRevisionPreview(service, RevisionRequest(asset, provider, true));
    REQUIRE(result.HasError());
    REQUIRE(result.ErrorValue().code.Value() == "asset.preview.provider_failed");
    REQUIRE(provider->generations.load() == 0);
}

TEST_CASE("Provider revisions partition cached images and reach generation unchanged", "[assets][preview][revision]") {
    JobSystem jobs{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 4}};
    AssetPreviewService service{jobs};
    RevisionAsset asset;
    const auto provider = std::make_shared<RevisionProvider>();
    const auto first = CompleteRevisionPreview(service, RevisionRequest(asset, provider, true));
    REQUIRE(first.HasValue());
    REQUIRE_FALSE(first.Value().cacheHit);
    REQUIRE(provider->sawRevision.load());
    REQUIRE(provider->correctProof.load());
    const auto cached = CompleteRevisionPreview(service, RevisionRequest(asset, provider, true));
    REQUIRE(cached.HasValue());
    REQUIRE(cached.Value().cacheHit);
    ++provider->revision;
    const auto changed = CompleteRevisionPreview(service, RevisionRequest(asset, provider, true));
    REQUIRE(changed.HasValue());
    REQUIRE_FALSE(changed.Value().cacheHit);
    REQUIRE(provider->generations.load() == 2);
    REQUIRE(provider->correctProof.load());
}

TEST_CASE("A preview revision changing during cache hit validation rejects stale pixels", "[assets][preview][revision]") {
    JobSystem jobs{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 4}};
    AssetPreviewService service{jobs};
    RevisionAsset asset;
    const auto provider = std::make_shared<RevisionProvider>();
    REQUIRE(CompleteRevisionPreview(service, RevisionRequest(asset, provider, true)).HasValue());
    provider->changeOnCapture = provider->captures.load() + 2;
    const auto stale = CompleteRevisionPreview(service, RevisionRequest(asset, provider, true));
    REQUIRE(stale.HasError());
    REQUIRE(stale.ErrorValue().code.Value() == "asset.preview.provider_failed");
    REQUIRE(provider->generations.load() == 1);
    const auto retry = CompleteRevisionPreview(service, RevisionRequest(asset, provider, true));
    REQUIRE(retry.HasValue());
    REQUIRE_FALSE(retry.Value().cacheHit);
    REQUIRE(provider->generations.load() == 2);
}

TEST_CASE("A preview revision changing before fresh publication never populates its stale cache key", "[assets][preview][revision]") {
    JobSystem jobs{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 4}};
    AssetPreviewService service{jobs};
    RevisionAsset asset;
    const auto provider = std::make_shared<RevisionProvider>();
    provider->changeOnCapture = 2;
    const auto stale = CompleteRevisionPreview(service, RevisionRequest(asset, provider, true));
    REQUIRE(stale.HasError());
    REQUIRE(stale.ErrorValue().code.Value() == "asset.preview.provider_failed");
    provider->revision = 1;
    provider->changeOnCapture = 0;
    const auto retry = CompleteRevisionPreview(service, RevisionRequest(asset, provider, true));
    REQUIRE(retry.HasValue());
    REQUIRE_FALSE(retry.Value().cacheHit);
    REQUIRE(provider->generations.load() == 2);
}
