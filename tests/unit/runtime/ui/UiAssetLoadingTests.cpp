#include "Horo/Foundation/Sha256.h"
#include "Horo/Runtime/Ui/UiAssetLoading.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace Horo::Runtime::Ui {
    namespace {
        Assets::AssetId Asset(const std::uint8_t marker) {
            std::array<std::uint8_t, 16> bytes{};
            bytes.back() = marker;
            return Assets::AssetId::FromBytes(bytes);
        }

        Assets::AssetTypeId Type(const std::string_view value) {
            return Assets::AssetTypeId::Parse(value).Value();
        }

        AssetCookTargetId Target() {
            return AssetCookTargetId::Parse("linux-x64").Value();
        }

        template <typename Id> Id UiId(const std::uint8_t marker) {
            SerializedUiId bytes{};
            bytes.back() = marker;
            return Id::Create(bytes).Value();
        }

        UiDocumentRevision Revision(const std::uint64_t value) {
            return UiDocumentRevision::Create(value).Value();
        }

        UiDocument MakeDocument(const Assets::AssetId dependency, const bool required = true) {
            UiDocumentBuilder builder{UiId<UiDocumentId>(1), Revision(3)};
            REQUIRE(builder.AddCanvas({UiId<UiCanvasId>(2), UiId<UiElementId>(3)}).HasValue());
            REQUIRE(builder.RequireAsset({dependency, Type("core.texture"), required}).HasValue());
            return std::move(builder).Build().Value();
        }

        Assets::AssetRecord Record(const Assets::AssetId id, const Assets::AssetTypeId type) {
            const std::string stem = id.ToString();
            return {id, type, ProjectPath::Parse("assets/" + stem + ".horoasset").Value(),
                    ProjectPath::Parse("assets/" + stem + ".horoasset.horo").Value()};
        }

        std::vector<std::uint8_t> Artifact(const Assets::AssetId id, const Assets::AssetTypeId type, std::vector<std::uint8_t> payload) {
            Assets::AssetCookArtifact artifact;
            artifact.id = id;
            artifact.type = type;
            artifact.target = Target();
            artifact.payloadDigest = ComputeSha256(std::as_bytes(std::span{payload}));
            artifact.payload = std::move(payload);
            return Assets::EncodeCookedArtifact(artifact).Value();
        }

        UiRuntimeAssetLoadRequest Request(const Assets::AssetId root, const Assets::AssetTypeId rootType) {
            return {.canvas = {root, UiId<UiDocumentId>(1), UiId<UiCanvasId>(2), Revision(1)},
                    .expectedAssetType = rootType,
                    .target = Target()};
        }

        class BlockingProvider final : public Assets::IAssetProvider {
        public:
            explicit BlockingProvider(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {}

            Result<bool> Exists(Assets::AssetId, const CancellationToken &cancellation) const override {
                if (cancellation.IsCancellationRequested())
                    return Result<bool>::Failure(MakeError(UiErrors::AssetLoadCancelled));
                return Result<bool>::Success(true);
            }

            Result<std::vector<std::uint8_t>> Load(Assets::AssetId, const CancellationToken &cancellation) const override {
                entered.store(true, std::memory_order_release);
                while (!release.load(std::memory_order_acquire) && !cancellation.IsCancellationRequested())
                    std::this_thread::yield();
                if (cancellation.IsCancellationRequested())
                    return Result<std::vector<std::uint8_t>>::Failure(MakeError(UiErrors::AssetLoadCancelled));
                return Result<std::vector<std::uint8_t>>::Success(bytes_);
            }

            mutable std::atomic<bool> entered{};
            mutable std::atomic<bool> release{};

        private:
            std::vector<std::uint8_t> bytes_;
        };

        TEST_CASE("Runtime UI cooking is deterministic and decodes immutable runtime data", "[runtime_ui][cook]") {
            const UiDocument source = MakeDocument(Asset(7));
            const auto first = CookedUiDocument::Cook(source);
            const auto second = CookedUiDocument::Cook(source);
            REQUIRE(first.HasValue());
            REQUIRE(second.HasValue());
            REQUIRE(std::ranges::equal(first.Value().Payload(), second.Value().Payload()));
            const auto decoded = CookedUiDocument::Decode(first.Value().Payload());
            REQUIRE(decoded.HasValue());
            REQUIRE(decoded.Value().Id() == source.Id());
            REQUIRE(decoded.Value().SourceRevision() == source.Revision());
            REQUIRE(std::ranges::equal(decoded.Value().Canvases(), source.Canvases()));
            REQUIRE(std::ranges::equal(decoded.Value().Dependencies(), source.Dependencies()));

            auto malformed = std::vector<std::uint8_t>{first.Value().Payload().begin(), first.Value().Payload().end()};
            malformed.front() ^= 0x01U;
            const auto malformedResult = CookedUiDocument::Decode(malformed);
            REQUIRE(malformedResult.HasError());
            REQUIRE(malformedResult.ErrorValue().code.Value() == UiErrors::CookedPayloadMalformed.code.Value());
        }

        TEST_CASE("Runtime UI asset loading resolves a bounded packaged closure", "[runtime_ui][asset_loading]") {
            const Assets::AssetId root = Asset(1);
            const Assets::AssetId dependency = Asset(7);
            const Assets::AssetTypeId rootType = Type("runtime.ui.document");
            const UiDocument source = MakeDocument(dependency);
            const auto cooked = CookedUiDocument::Cook(source);
            REQUIRE(cooked.HasValue());

            Assets::AssetRegistry registry;
            REQUIRE(registry.Publish({Record(root, rootType), Record(dependency, Type("core.texture"))}).status ==
                    Assets::AssetRegistryBuildStatus::Complete);
            Assets::MemoryAssetProvider provider;
            provider.Insert(root, Artifact(root, rootType, {cooked.Value().Payload().begin(), cooked.Value().Payload().end()}));
            provider.Insert(dependency, Artifact(dependency, Type("core.texture"), {9, 8, 7}));
            JobSystem jobs{JobSystemConfig{2, 16}};
            Assets::AssetLoadService assetLoads{jobs, provider};
            UiRuntimeAssetLoadService loader{registry, assetLoads};

            auto requested = loader.LoadAsync(registry.Snapshot(), Request(root, rootType));
            REQUIRE(requested.HasValue());
            auto handle = std::move(requested).Value();
            REQUIRE(handle.Wait().HasValue());
            auto loaded = handle.TakeResult();
            REQUIRE(loaded.HasValue());
            REQUIRE(loaded.Value().registryRevision == registry.Snapshot().Revision());
            REQUIRE(loaded.Value().assets.size() == 1);
            REQUIRE(*loaded.Value().assets.front().payload == std::vector<std::uint8_t>({9, 8, 7}));

            auto closure = std::move(loaded).Value();
            const auto owner = UiOwnershipGeneration::Create(21).Value();
            auto instanceResult = std::move(closure).CreateInstance({owner, 1, 1});
            REQUIRE(instanceResult.HasValue());
            auto instance = std::move(instanceResult).Value();
            REQUIRE(instance.FindAsset(dependency) != nullptr);
            REQUIRE(instance.FindAsset(dependency)->payload->front() == 9);
            REQUIRE(instance.Activate().HasValue());
            instance.Shutdown();
            loader.Shutdown();
            assetLoads.Shutdown();
            jobs.Shutdown(ShutdownPolicy::Drain);
        }

        TEST_CASE("Runtime UI optional assets may be absent but required assets fail closed", "[runtime_ui][asset_loading]") {
            const Assets::AssetId root = Asset(1);
            const Assets::AssetId optional = Asset(8);
            const Assets::AssetTypeId rootType = Type("runtime.ui.document");
            const UiDocument source = MakeDocument(optional, false);
            const auto cooked = CookedUiDocument::Cook(source);
            REQUIRE(cooked.HasValue());
            Assets::AssetRegistry registry;
            REQUIRE(registry.Publish({Record(root, rootType)}).status == Assets::AssetRegistryBuildStatus::Complete);
            Assets::MemoryAssetProvider provider;
            provider.Insert(root, Artifact(root, rootType, {cooked.Value().Payload().begin(), cooked.Value().Payload().end()}));
            JobSystem jobs{JobSystemConfig{1, 8}};
            Assets::AssetLoadService assetLoads{jobs, provider};
            UiRuntimeAssetLoadService loader{registry, assetLoads};
            auto requested = loader.LoadAsync(Request(root, rootType));
            REQUIRE(requested.HasValue());
            auto handle = std::move(requested).Value();
            REQUIRE(handle.Wait().HasValue());
            auto loaded = handle.TakeResult();
            REQUIRE(loaded.HasValue());
            REQUIRE(loaded.Value().assets.empty());

            const UiDocument requiredSource = MakeDocument(optional);
            const auto requiredCooked = CookedUiDocument::Cook(requiredSource);
            REQUIRE(requiredCooked.HasValue());
            provider.Insert(root,
                            Artifact(root, rootType, {requiredCooked.Value().Payload().begin(), requiredCooked.Value().Payload().end()}));
            auto requiredRequest = loader.LoadAsync(Request(root, rootType));
            REQUIRE(requiredRequest.HasValue());
            auto requiredHandle = std::move(requiredRequest).Value();
            REQUIRE(requiredHandle.Wait().HasValue());
            const auto requiredResult = requiredHandle.TakeResult();
            REQUIRE(requiredResult.HasError());
            REQUIRE(requiredResult.ErrorValue().code.Value() == UiErrors::AssetMissing.code.Value());

            loader.Shutdown();
            assetLoads.Shutdown();
            jobs.Shutdown(ShutdownPolicy::Drain);
        }

        TEST_CASE("Runtime UI loading rejects stale registry generations and cancellation", "[runtime_ui][asset_loading][reload]") {
            const Assets::AssetId root = Asset(1);
            const Assets::AssetTypeId rootType = Type("runtime.ui.document");
            const UiDocument source = MakeDocument(Asset(7), false);
            const auto cooked = CookedUiDocument::Cook(source);
            REQUIRE(cooked.HasValue());
            const auto bytes = Artifact(root, rootType, {cooked.Value().Payload().begin(), cooked.Value().Payload().end()});
            Assets::AssetRegistry registry;
            REQUIRE(registry.Publish({Record(root, rootType)}).status == Assets::AssetRegistryBuildStatus::Complete);
            BlockingProvider provider{bytes};
            JobSystem jobs{JobSystemConfig{1, 8}};
            Assets::AssetLoadService assetLoads{jobs, provider};
            UiRuntimeAssetLoadService loader{registry, assetLoads};
            auto requested = loader.LoadAsync(registry.Snapshot(), Request(root, rootType));
            REQUIRE(requested.HasValue());
            auto handle = std::move(requested).Value();
            while (!provider.entered.load(std::memory_order_acquire))
                std::this_thread::yield();
            REQUIRE(registry.Publish({Record(root, rootType)}).status == Assets::AssetRegistryBuildStatus::Complete);
            provider.release.store(true, std::memory_order_release);
            REQUIRE(handle.Wait().HasValue());
            REQUIRE(handle.TakeResult().ErrorValue().code.Value() == UiErrors::AssetRegistryStale.code.Value());

            provider.entered.store(false, std::memory_order_release);
            provider.release.store(false, std::memory_order_release);
            auto cancelledRequest = loader.LoadAsync(registry.Snapshot(), Request(root, rootType));
            REQUIRE(cancelledRequest.HasValue());
            auto cancelled = std::move(cancelledRequest).Value();
            while (!provider.entered.load(std::memory_order_acquire))
                std::this_thread::yield();
            REQUIRE(cancelled.RequestCancel().HasValue());
            REQUIRE(cancelled.Wait().HasValue());
            REQUIRE(cancelled.TakeResult().ErrorValue().code.Value() == UiErrors::AssetLoadCancelled.code.Value());
            loader.Shutdown();
            assetLoads.Shutdown();
            jobs.Shutdown(ShutdownPolicy::Drain);
        }

        TEST_CASE("Runtime UI shutdown cancels and joins in-flight asset loads", "[runtime_ui][asset_loading][shutdown]") {
            const Assets::AssetId root = Asset(1);
            const Assets::AssetTypeId rootType = Type("runtime.ui.document");
            const UiDocument source = MakeDocument(Asset(7), false);
            const auto cooked = CookedUiDocument::Cook(source);
            REQUIRE(cooked.HasValue());
            const auto bytes = Artifact(root, rootType, {cooked.Value().Payload().begin(), cooked.Value().Payload().end()});
            Assets::AssetRegistry registry;
            REQUIRE(registry.Publish({Record(root, rootType)}).status == Assets::AssetRegistryBuildStatus::Complete);
            BlockingProvider provider{bytes};
            JobSystem jobs{JobSystemConfig{1, 8}};
            Assets::AssetLoadService assetLoads{jobs, provider};
            UiRuntimeAssetLoadService loader{registry, assetLoads};
            auto requested = loader.LoadAsync(Request(root, rootType));
            REQUIRE(requested.HasValue());
            auto handle = std::move(requested).Value();
            while (!provider.entered.load(std::memory_order_acquire))
                std::this_thread::yield();

            loader.Shutdown();
            REQUIRE(handle.Wait().HasValue());
            const auto result = handle.TakeResult();
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == UiErrors::AssetLoadCancelled.code.Value());
            assetLoads.Shutdown();
            jobs.Shutdown(ShutdownPolicy::Drain);
        }
    }  // namespace
}  // namespace Horo::Runtime::Ui
