#include "OpenXRDiagnosticHost.h"

#include "AndroidApkOpenXRLoader.h"
#include "OpenXRAndroidDiscovery.h"
#include "OpenXRLoaderPin.h"

#include <android/log.h>
#include <array>
#include <cstdio>
#include <game-activity/native_app_glue/android_native_app_glue.h>
#include <utility>

namespace Horo::AndroidQualification {
    namespace {
        using XR::OpenXRInternal::AndroidOpenXRProbeResult;
        using XR::OpenXRInternal::AndroidOpenXRProbeStage;
        using XR::OpenXRInternal::OpenXRPrimaryStereoExtents;

        /** @brief Defines the closed process-scoped diagnostic vocabulary. */
        const char *StageName(AndroidOpenXRProbeStage stage) noexcept {
            using enum AndroidOpenXRProbeStage;
            switch (stage) {
                case LoaderDispatch:
                    return "loader-dispatch";
                case LoaderInitialization:
                    return "loader-initialization";
                case ExtensionAdmission:
                    return "extension-admission";
                case InstanceCreation:
                    return "instance-creation";
                case SystemDiscovery:
                    return "system-discovery";
                case RuntimeProperties:
                    return "runtime-properties";
                case InstanceRetirement:
                    return "instance-retirement";
                case Discovered:
                    return "discovered";
            }
            return "host-application";
        }

        /** @brief Emits numeric and closed-enum facts only, keeping paths, handles and runtime names private. */
        void Report(const char *stage, XrResult result, XrVersion version = 0,
                    const std::optional<OpenXRPrimaryStereoExtents> &views = {}) noexcept {
            std::array<char, 80> recommendations{};
            if (views) {
                const auto &[left, right] = *views;
                std::snprintf(recommendations.data(), recommendations.size(), " primaryView0=%ux%u primaryView1=%ux%u",
                              static_cast<unsigned>(left.width), static_cast<unsigned>(left.height), static_cast<unsigned>(right.width),
                              static_cast<unsigned>(right.height));
            }
            __android_log_print(ANDROID_LOG_INFO, "HoroPackageQualification",
                                "host-openxr abi=%s stage=%s result=%d runtimeMajor=%u runtimeMinor=%u runtimePatch=%u%s", OpenXRLoaderAbi,
                                stage, static_cast<int>(result), static_cast<unsigned>(XR_VERSION_MAJOR(version)),
                                static_cast<unsigned>(XR_VERSION_MINOR(version)), static_cast<unsigned>(XR_VERSION_PATCH(version)),
                                recommendations.data());
        }
    }  // namespace

    /** @copydoc OpenXRDiagnosticHost::OpenXRDiagnosticHost */
    OpenXRDiagnosticHost::OpenXRDiagnosticHost() noexcept = default;

    /** @copydoc OpenXRDiagnosticHost::~OpenXRDiagnosticHost */
    OpenXRDiagnosticHost::~OpenXRDiagnosticHost() = default;

    /** @copydoc OpenXRDiagnosticHost::Probe */
    void OpenXRDiagnosticHost::Probe(android_app &app) {
        using namespace Platform::Android;
        using namespace XR::OpenXRInternal;
        auto observed = AcquireGameActivityJniApplication(app);
        const auto *references = std::get_if<GameActivityJniApplication>(&observed);
        if (!references) {
            Report("host-application", XR_ERROR_INITIALIZATION_FAILED);
            return;
        }
        const auto application = std::make_shared<GameActivityJniApplication>(*references);
        const auto digest = ParseSha256(OpenXRLoaderPin);
        if (digest.HasError()) {
            Report("bundled-loader", XR_ERROR_FILE_CONTENTS_INVALID);
            return;
        }
        auto loader = LoadBundledOpenXRLoader(*application, digest.Value());
        if (loader.HasError()) {
            Report("bundled-loader", XR_ERROR_FILE_ACCESS_ERROR);
            return;
        }
        const NativeOpenXRAndroidApplication native{application->vm, application->applicationContext.get(), application->activity.get(),
                                                    application};
        discovery_ = std::make_unique<AndroidOpenXRColdDiscovery>(std::move(loader).Value(), native);
        const auto result = discovery_->Probe(XR_MAKE_VERSION(1, 0, 0));
        if (result.stage != AndroidOpenXRProbeStage::InstanceRetirement)
            discovery_.reset();
        Report(StageName(result.stage), result.result, result.runtimeVersion, result.recommendedViews);
    }

    /** @copydoc OpenXRDiagnosticHost::Command */
    void OpenXRDiagnosticHost::Command(std::int32_t command) noexcept {
        if (state_ == State::Attempted)
            return;
        if (command == APP_CMD_RESUME)
            state_ = State::Queued;
        else if (command == APP_CMD_PAUSE || command == APP_CMD_STOP || command == APP_CMD_DESTROY)
            state_ = State::Waiting;
    }

    /** @copydoc OpenXRDiagnosticHost::Pump */
    void OpenXRDiagnosticHost::Pump(android_app &app) noexcept {
        if (!PollRetirement())
            return;
        if (state_ != State::Queued || app.destroyRequested)
            return;
        state_ = State::Attempted;
        try {
            Probe(app);
        } catch (...) {  // Native entry must not unwind into GameActivity.
            Report("host-application", XR_ERROR_INITIALIZATION_FAILED);
        }
    }

    /** @copydoc OpenXRDiagnosticHost::PollRetirement */
    bool OpenXRDiagnosticHost::PollRetirement() noexcept {
        if (!discovery_)
            return true;
        if (discovery_->Retire() != XR_SUCCESS)
            return false;
        discovery_.reset();
        return true;
    }
}  // namespace Horo::AndroidQualification
