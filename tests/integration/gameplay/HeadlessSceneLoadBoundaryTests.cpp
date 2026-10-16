#include "HeadlessSceneLoadBoundary.h"
#include "PhysicsSceneActivationTestSupport.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>
#include <utility>

namespace {
    using namespace Horo;
    using namespace Horo::Application::Internal;
    using namespace std::chrono_literals;

    /** @brief Gates a real deterministic provider to observe owned pending/cancellation paths.
     * @details Only atomics cross the provider worker/owner/releaser lanes; all lanes join before fixture destruction.
     */
    class FencedAssetProvider final : public Assets::IAssetProvider {
    public:
        Result<bool> Exists(Assets::AssetId id, const CancellationToken &cancellation) const override {
            return memory.Exists(id, cancellation);
        }

        Result<std::vector<std::uint8_t>> Load(Assets::AssetId id, const CancellationToken &cancellation) const override {
            active.store(true, std::memory_order_release);
            entered.store(true, std::memory_order_release);
            while (!released.load(std::memory_order_acquire) && !cancellation.IsCancellationRequested())
                std::this_thread::sleep_for(1ms);
            cancelled.store(cancellation.IsCancellationRequested(), std::memory_order_release);
            auto result = memory.Load(id, cancellation);
            active.store(false, std::memory_order_release);
            return result;
        }

        Assets::MemoryAssetProvider memory;
        mutable std::atomic<bool> entered{};
        mutable std::atomic<bool> active{};
        mutable std::atomic<bool> cancelled{};
        std::atomic<bool> released{};
    };

    /** @brief Starts actual workers before asset services and keeps them alive until all private Scene handles join. */
    struct LoadFixture final {
        Assets::AssetId asset{Physics::Asset(79)};
        Assets::AssetTypeId type{Physics::AssetType()};
        Assets::AssetRegistry registry;
        FencedAssetProvider provider;
        JobSystem jobs{JobSystemConfig{1, 16}};
        Assets::AssetLoadService loads{jobs, provider};
        Runtime::RuntimeSceneService scene{registry, loads, {}, Runtime::SceneRuntimeId{71}};

        LoadFixture() {
            REQUIRE(jobs.WorkerCount() > 0);
            REQUIRE(registry.Publish({Physics::Record(asset, type)}).status == Assets::AssetRegistryBuildStatus::Complete);
            provider.memory.Insert(asset, {1, 3, 7});
        }

        ~LoadFixture() {
            scene.Shutdown();
            loads.Shutdown();
            jobs.Shutdown(ShutdownPolicy::Drain);
        }

        LoadFixture(const LoadFixture &) = delete;
        LoadFixture &operator=(const LoadFixture &) = delete;
        LoadFixture(LoadFixture &&) = delete;
        LoadFixture &operator=(LoadFixture &&) = delete;

        Runtime::RuntimeSceneDefinition Definition() const {
            Runtime::SceneDefinitionBuilder builder{{7}, {1}};
            builder.Add({.object = {9}});
            REQUIRE(builder.RequireAsset({asset, type}).HasValue());
            auto definition = std::move(builder).Build();
            REQUIRE(definition.HasValue());
            return std::move(definition).Value();
        }
    };

    bool AwaitProvider(const FencedAssetProvider &provider) {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!provider.entered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        return provider.entered.load(std::memory_order_acquire);
    }

    TEST_CASE("Headless load boundary waits for actual required asset readiness on an already running executor", "[scene][host][load]") {
        LoadFixture fixture;
        bool observedPending{};
        std::jthread release{[&] {
            observedPending = AwaitProvider(fixture.provider);
            fixture.provider.released.store(true, std::memory_order_release);
        }};
        const auto loaded =
            PrepareHeadlessSceneAtLoadBoundary(fixture.scene, fixture.Definition(), {{}, std::chrono::steady_clock::now() + 3s});
        release.join();
        REQUIRE(observedPending);
        REQUIRE(loaded.HasValue());
        CHECK(loaded.Value().RuntimeId() == Runtime::SceneRuntimeId{71});
        const auto resident = loaded.Value().FindAsset(fixture.asset);
        REQUIRE(resident.has_value());
        REQUIRE(resident->bytes.size() == 3);
        CHECK(resident->bytes[2] == 7);
        CHECK_FALSE(fixture.provider.active.load());
        CHECK_FALSE(fixture.provider.cancelled.load());
    }

    TEST_CASE("Headless load cancellation joins only the private candidate's actual pending asset work", "[scene][host][load]") {
        LoadFixture fixture;
        CancellationSource cancellation;
        bool observedPending{};
        std::jthread cancel{[&] {
            observedPending = AwaitProvider(fixture.provider);
            cancellation.RequestCancellation();
        }};
        const auto loaded = PrepareHeadlessSceneAtLoadBoundary(fixture.scene, fixture.Definition(),
                                                               {cancellation.Token(), std::chrono::steady_clock::now() + 3s});
        cancel.join();
        REQUIRE(observedPending);
        REQUIRE(loaded.HasError());
        CHECK(loaded.ErrorValue().code.Value() == HeadlessSceneLoadErrors::Cancelled.code.Value());
        CHECK_FALSE(fixture.scene.ActiveScene().has_value());
        CHECK_FALSE(fixture.provider.active.load());
        CHECK(fixture.provider.cancelled.load());
    }

    TEST_CASE("Headless load deadline preserves its primary classification through owned asset cancellation", "[scene][host][load]") {
        LoadFixture fixture;
        const auto loaded =
            PrepareHeadlessSceneAtLoadBoundary(fixture.scene, fixture.Definition(), {{}, std::chrono::steady_clock::now() + 100ms});
        REQUIRE(loaded.HasError());
        CHECK(loaded.ErrorValue().code.Value() == HeadlessSceneLoadErrors::Deadline.code.Value());
        CHECK_FALSE(fixture.scene.ActiveScene().has_value());
        CHECK_FALSE(fixture.provider.active.load());
        if (fixture.provider.entered.load())
            CHECK(fixture.provider.cancelled.load());
    }

    TEST_CASE("Headless load preserves the actual provider error and leaves an independently active Scene intact", "[scene][host][load]") {
        LoadFixture fixture;
        Runtime::RuntimeSceneService old{Runtime::SceneRuntimeId{69}};
        REQUIRE(old.Startup({}).HasValue());
        REQUIRE(old.QueuePreparation(Physics::Definition()).HasValue());
        const CancellationToken token;
        const Runtime::FrameContext frame{.frameNumber = 1, .cancellation = token};
        REQUIRE(old.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, frame).HasValue());
        REQUIRE(old.ActiveScene().has_value());
        const auto previous = *old.ActiveScene();
        fixture.provider.memory.Remove(fixture.asset);
        fixture.provider.released.store(true);
        const auto expected = fixture.provider.memory.Load(fixture.asset, {});
        REQUIRE(expected.HasError());
        const auto loaded =
            PrepareHeadlessSceneAtLoadBoundary(fixture.scene, fixture.Definition(), {{}, std::chrono::steady_clock::now() + 3s});
        REQUIRE(loaded.HasError());
        CHECK(loaded.ErrorValue().code.Value() == expected.ErrorValue().code.Value());
        CHECK(loaded.ErrorValue().domain.Value() == expected.ErrorValue().domain.Value());
        CHECK(loaded.ErrorValue().message == expected.ErrorValue().message);
        CHECK_FALSE(fixture.scene.ActiveScene().has_value());
        CHECK_FALSE(fixture.provider.active.load());
        REQUIRE(old.ActiveScene().has_value());
        CHECK(old.ActiveScene()->RuntimeId() == previous.RuntimeId());
        CHECK(previous.Find({1}).has_value());
        old.Shutdown();
    }

    TEST_CASE("Headless expired and cancelled load budgets reject before starting required asset work", "[scene][host][load]") {
        for (const bool cancelled : {false, true}) {
            LoadFixture fixture;
            CancellationSource cancellation;
            if (cancelled)
                cancellation.RequestCancellation();
            const auto deadline = std::chrono::steady_clock::now() + (cancelled ? 3s : -1s);
            const auto loaded = PrepareHeadlessSceneAtLoadBoundary(fixture.scene, fixture.Definition(), {cancellation.Token(), deadline});
            REQUIRE(loaded.HasError());
            CHECK(loaded.ErrorValue().code.Value() ==
                  (cancelled ? HeadlessSceneLoadErrors::Cancelled.code.Value() : HeadlessSceneLoadErrors::Deadline.code.Value()));
            CHECK_FALSE(fixture.provider.entered.load());
            CHECK_FALSE(fixture.scene.ActiveScene().has_value());
        }
    }
}  // namespace
