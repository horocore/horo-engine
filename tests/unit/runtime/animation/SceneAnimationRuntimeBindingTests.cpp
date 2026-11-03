#include "Horo/Runtime/Scene/SceneAnimationRuntime.h"

#include <atomic>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <thread>

namespace Horo::Runtime {
    namespace {
        SceneAnimationRuntimeConfig Config(RuntimeDispatchSource source, CancellationToken lifetime) {
            SceneAnimationRuntimeConfig config;
            config.expectedDispatch = std::move(source);
            config.runtimeOwner = Animation::AnimationRuntimeId::Create(7).Value();
            config.target = AssetCookTargetId::Parse("test-runtime").Value();
            config.ownerCancellation = std::move(lifetime);
            return config;
        }

        class ColdCreator final : public RuntimeLifecycleParticipant {
        public:
            explicit ColdCreator(SceneAnimationRuntimeConfig config) : config_(std::move(config)) {}

            Result<void> Startup(const CancellationToken &) override {
                return Result<void>::Success();
            }

            Result<void> OnFixedUpdate(const FixedStepContext &) override {
                return Result<void>::Success();
            }

            Result<void> OnPhase(RuntimePhase phase, const FrameContext &) override {
                if (phase != RuntimePhase::VariableUpdate || invoked)
                    return Result<void>::Success();
                invoked = true;
                observedBinding = config_.expectedDispatch.BindingStatus();
                auto prepared = SceneAnimationRuntime::Create(config_);
                if (prepared.HasError())
                    return Result<void>::Failure(std::move(prepared).ErrorValue());
                adapter = std::move(prepared).Value();
                return Result<void>::Success();
            }

            void Shutdown() noexcept override {
                adapter.reset();
            }

            RuntimeDispatchStatus observedBinding{};
            bool invoked{};
            std::unique_ptr<SceneAnimationRuntime> adapter;

        private:
            SceneAnimationRuntimeConfig config_;
        };
    }  // namespace

    TEST_CASE("Animation cold owner binds the actual already active GUI-style issuer without granting dispatch",
              "[animation][scene][binding]") {
        DeterministicClock clock;
        auto prepared = FrameScheduler::Create(clock);
        REQUIRE(prepared.HasValue());
        auto scheduler = std::move(prepared).Value();
        CancellationSource lifetime;
        auto participant = std::make_unique<ColdCreator>(Config(scheduler->DispatchSource(), lifetime.Token()));
        auto *creator = participant.get();
        RuntimeLifecycle lifecycle;
        REQUIRE(lifecycle.AddParticipant(std::move(participant)).HasValue());
        REQUIRE(lifecycle.Startup(lifetime.Token()).HasValue());
        auto frame = scheduler->RunFrame(lifecycle, lifetime.Token(), false);
        INFO((frame.HasError() ? frame.ErrorValue().code.Value() + ": " + frame.ErrorValue().message : "success"));
        REQUIRE(frame.HasValue());
        REQUIRE(creator->invoked);
        CHECK(creator->observedBinding == RuntimeDispatchStatus::Reentrant);
        REQUIRE(creator->adapter);
        REQUIRE(creator->adapter->Startup(lifetime.Token()).HasValue());
        const FixedStepContext fabricated{1, Duration::FromMilliseconds(10), lifetime.Token(), 1, 1, {}};
        CHECK(creator->adapter->OnFixedUpdate(fabricated).HasError());
        // The fabricated abort cannot acquire an attempt or mutate graph state.
        creator->adapter->AbortFixedUpdate(fabricated);
        lifecycle.Shutdown();
    }

    TEST_CASE("Animation cold owner rejects default retired and foreign-thread source identity", "[animation][scene][binding]") {
        DeterministicClock clock;
        auto prepared = FrameScheduler::Create(clock);
        REQUIRE(prepared.HasValue());
        auto scheduler = std::move(prepared).Value();
        CancellationSource lifetime;
        auto config = Config(scheduler->DispatchSource(), lifetime.Token());
        SECTION("default") {
            config.expectedDispatch = {};
            CHECK(SceneAnimationRuntime::Create(config).HasError());
        }
        SECTION("retired") {
            scheduler->RetireDispatch();
            CHECK(SceneAnimationRuntime::Create(config).HasError());
        }
        SECTION("foreign owner thread") {
            std::atomic<bool> rejected{};
            std::thread other([config, &rejected] {
                rejected.store(SceneAnimationRuntime::Create(config).HasError());
            });
            other.join();
            CHECK(rejected.load());
        }
        SECTION("cancelled actual owner lifetime") {
            lifetime.RequestCancellation();
            CHECK(SceneAnimationRuntime::Create(config).HasError());
        }
    }
}  // namespace Horo::Runtime
