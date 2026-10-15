#include "AndroidGameActivityInput.h"

#include <android/asset_manager.h>
#include <android/log.h>
#include <array>
#include <cstddef>
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
}  // namespace

/** @brief Actual GameActivity entry for package assembly qualification; performs no renderer/runtime selection. */
extern "C" void android_main(android_app *app) {
    CheckAsset(*app);
    Horo::Input::RawInputCollector collector;
    Horo::Platform::Android::GameActivityInput input{*app, collector, {true, true, false}};
    app->userData = &input;
    app->onAppCmd = [](android_app *source, const std::int32_t command) {
        static_cast<Horo::Platform::Android::GameActivityInput *>(source->userData)->Command(command);
    };
    if (input.Status() != Horo::Platform::Android::InputAdmission::Accepted)
        __android_log_write(ANDROID_LOG_ERROR, "HoroPackageQualification", "Android input bridge unavailable");
    std::uint64_t frame{};
    while (app->destroyRequested == 0) {
        collector.BeginFrame(++frame);
        for (std::size_t polled = 0; polled < 64; ++polled) {
            void *data{};
            const int result = ALooper_pollOnce(polled == 0 ? 16 : 0, nullptr, nullptr, &data);
            if (result == ALOOPER_POLL_ERROR) {
                app->destroyRequested = 1;
                break;
            }
            if (result == ALOOPER_POLL_TIMEOUT)
                break;
            if (result >= 0 && data) {
                auto *source = static_cast<android_poll_source *>(data);
                source->process(app, source);
            }
        }
        input.Pump();
        const auto &snapshot = collector.Commit();
        static_cast<void>(snapshot);  // Same immutable Input frame consumed by normal routing in a product composition.
    }
    app->onAppCmd = nullptr;
    app->userData = nullptr;
}
