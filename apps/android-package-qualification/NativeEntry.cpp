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
    while (app->destroyRequested == 0) {
        void *data{};
        const int result = ALooper_pollOnce(-1, nullptr, nullptr, &data);
        if (result == ALOOPER_POLL_ERROR)
            break;
        if (result >= 0 && data != nullptr) {
            auto *source = static_cast<android_poll_source *>(data);
            source->process(app, source);
        }
    }
}
