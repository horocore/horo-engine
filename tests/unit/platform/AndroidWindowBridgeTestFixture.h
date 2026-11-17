#pragma once

#include "AndroidWindowBridge.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <thread>
#include <utility>

namespace Horo::Tests::AndroidWindow {
    using namespace Horo::Platform;
    using namespace Horo::Platform::Android;

    struct NativeReferences {
        std::array<ANativeWindow *, 256> acquired{};
        std::array<ANativeWindow *, 256> released{};
        std::size_t acquireCount{};
        std::size_t releaseCount{};
        bool failAcquire{};
        void *hookContext{};
        void (*onAcquire)(void *) noexcept {};
        void (*onRelease)(void *) noexcept {};
        unsigned firstStorage{};
        unsigned secondStorage{};

        // Opaque host-only identities. No synthetic Android object is dereferenced.
        ANativeWindow *First() noexcept {
            return reinterpret_cast<ANativeWindow *>(&firstStorage);
        }

        ANativeWindow *Second() noexcept {
            return reinterpret_cast<ANativeWindow *>(&secondStorage);
        }

        AndroidWindowOperations Operations() noexcept {
            return {.context = this, .acquire = [](void *context, ANativeWindow *window) noexcept {
                auto &self = *static_cast<NativeReferences *>(context);
                if (self.failAcquire)
                    return false;
                if (self.onAcquire)
                    self.onAcquire(self.hookContext);
                self.acquired[self.acquireCount++] = window;
                return true;
            }, .release = [](void *context, ANativeWindow *window) noexcept {
                auto &self = *static_cast<NativeReferences *>(context);
                if (self.onRelease)
                    self.onRelease(self.hookContext);
                self.released[self.releaseCount++] = window;
            }};
        }
    };

    inline std::unique_ptr<AndroidLifecycleController> MakeController(std::size_t capacity) {
        auto created = AndroidLifecycleController::Create(std::this_thread::get_id(), capacity);
        REQUIRE(created.HasValue());
        return std::move(created).Value();
    }

    struct Fixture {
        NativeReferences native;
        std::unique_ptr<AndroidLifecycleController> controller;
        AndroidLifecycleAdapter adapter;
        AndroidActivityGeneration activity;

        explicit Fixture(std::size_t capacity = 64) : controller(MakeController(capacity)), adapter(*controller) {
            REQUIRE(adapter.OnProcessCreated().HasValue());
            REQUIRE(controller->Drain().HasValue());
            const auto created = adapter.OnActivityCreated();
            REQUIRE(created.HasValue());
            activity = created.Value();
            REQUIRE(controller->Drain().HasValue());
            REQUIRE(adapter.OnActivityStarted(activity).HasValue());
            REQUIRE(controller->Drain().HasValue());
            REQUIRE(adapter.OnActivityResumed(activity).HasValue());
            REQUIRE(controller->Drain().HasValue());
        }

        AndroidWindowBridge Bridge(AndroidWindowSelection selection = AndroidWindowSelection::AndroidVulkan) {
            return AndroidWindowBridge(adapter, selection, native.Operations());
        }

        AndroidLifecycleSnapshot State() {
            const auto state = controller->Snapshot();
            REQUIRE(state.HasValue());
            return state.Value();
        }
    };
}  // namespace Horo::Tests::AndroidWindow
