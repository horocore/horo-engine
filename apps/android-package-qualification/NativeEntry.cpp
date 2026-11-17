#include "AndroidGameActivityInput.h"
#if defined(HORO_ANDROID_QUALIFICATION_VULKAN)
#include "QualificationHost.h"

#include <chrono>
#include <thread>
#endif
#include <android/asset_manager.h>
#include <android/log.h>
#include <array>
#include <cstddef>
#include <exception>
#include <game-activity/native_app_glue/android_native_app_glue.h>

namespace {
    /** @brief Checks that the declared packaged asset is reachable through the actual Android manager. */
    void CheckAsset(android_app &app) {
        AAsset *asset = AAssetManager_open(app.activity->assetManager, "qualification.json", AASSET_MODE_STREAMING);
        if (asset == nullptr)
            return;
        std::array<std::byte, 256> buffer{};
        const int count = AAsset_read(asset, buffer.data(), buffer.size());
        AAsset_close(asset);
        if (count > 0)
            __android_log_write(ANDROID_LOG_INFO, "HoroPackageQualification", "Packaged qualification asset available");
    }

    /** @brief Bounds native callback work on the one existing owner loop. */
    void PollEvents(android_app &app) {
        for (std::size_t polled = 0; polled < 64; ++polled) {
            void *data{};
            const int result = ALooper_pollOnce(polled == 0 ? 16 : 0, nullptr, nullptr, &data);
            if (result == ALOOPER_POLL_ERROR) {
                app.destroyRequested = 1;
                break;
            }
            if (result == ALOOPER_POLL_TIMEOUT)
                break;
            if (result >= 0 && data) {
                auto *source = static_cast<android_poll_source *>(data);
                source->process(&app, source);
            }
        }
    }

#if defined(HORO_ANDROID_QUALIFICATION_VULKAN)
    /** @brief Reports the original typed failure without implying presentation or device qualification. */
    void Report(const Horo::Error &error) {
        __android_log_print(ANDROID_LOG_ERROR, "HoroPackageQualification", "%s/%s: %s", error.domain.Value().c_str(),
                            error.code.Value().c_str(), error.message.c_str());
    }

    /** @brief Drains actual native ownership under an explicit qualification-only 30 second shutdown deadline. */
    void Shutdown(Horo::AndroidQualification::QualificationHost &host) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
        while (std::chrono::steady_clock::now() < deadline) {
            auto drained = host.PollShutdown();
            if (drained.HasError()) {
                Report(drained.ErrorValue());
                std::terminate();
            }
            if (drained.Value())
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        __android_log_write(ANDROID_LOG_ERROR, "HoroPackageQualification", "Native shutdown deadline expired; ownership remains pending");
        std::terminate();
    }
#endif

    /** @brief One callback fanout; Input and optional renderer/lifecycle composition receive each command once. */
    struct CallbackOwner final {
        Horo::Platform::Android::GameActivityInput &input;
#if defined(HORO_ANDROID_QUALIFICATION_VULKAN)
        Horo::AndroidQualification::QualificationHost &host;
#endif
        bool failed{};

        /** @brief Preserves existing Input semantics before the selected host handles lifecycle/window observations. */
        void Command(android_app &app, std::int32_t command) {
            input.Command(command);
#if defined(HORO_ANDROID_QUALIFICATION_VULKAN)
            if (auto observed = host.Command(app, command); observed.HasError()) {
                Report(observed.ErrorValue());
                failed = true;
            }
#endif
        }

        /** @brief Pumps only the explicitly selected renderer; no renderer is substituted after admission failure. */
        void Pump() {
#if defined(HORO_ANDROID_QUALIFICATION_VULKAN)
            if (auto rendered = host.Pump(); rendered.HasError()) {
                Report(rendered.ErrorValue());
                failed = true;
            }
#endif
        }
    };

    /** @brief Runs the shared bounded GameActivity/Input loop for either explicit package profile. */
    void Run(android_app &app, Horo::Input::RawInputCollector &collector, CallbackOwner &owner) {
        app.userData = &owner;
        app.onAppCmd = [](android_app *source, std::int32_t command) {
            static_cast<CallbackOwner *>(source->userData)->Command(*source, command);
        };
        std::uint64_t frame{};
        while (app.destroyRequested == 0 && !owner.failed) {
            collector.BeginFrame(++frame);
            PollEvents(app);
            owner.input.Pump();
            const auto &snapshot = collector.Commit();
            static_cast<void>(snapshot);
            if (app.destroyRequested == 0 && !owner.failed)
                owner.Pump();
        }
        app.onAppCmd = nullptr;
        app.userData = nullptr;
    }
}  // namespace

/** @brief Actual GameActivity entry; renderer policy is compiled explicitly before any native window admission. */
extern "C" void android_main(android_app *app) {
    CheckAsset(*app);
    Horo::Input::RawInputCollector collector;
    Horo::Platform::Android::GameActivityInput input{*app, collector, {true, true, false}};
    if (input.Status() != Horo::Platform::Android::InputAdmission::Accepted)
        __android_log_write(ANDROID_LOG_ERROR, "HoroPackageQualification", "Android input bridge unavailable");
#if defined(HORO_ANDROID_QUALIFICATION_VULKAN)
    auto created = Horo::AndroidQualification::QualificationHost::Create();
    if (created.HasError()) {
        Report(created.ErrorValue());
        return;
    }
    auto host = std::move(created).Value();
    CallbackOwner owner{input, *host};
    Run(*app, collector, owner);
    Shutdown(*host);
#else
    CallbackOwner owner{input};
    Run(*app, collector, owner);
#endif
}
