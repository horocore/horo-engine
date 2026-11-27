#pragma once

/** @file OpenXRAndroidDiscovery.h
 * @brief Private native Android loader ABI and cold instance/system diagnostic.
 */

#if defined(__ANDROID__)

#include "Horo/Platform/DynamicLibrary.h"
#include "OpenXRViewRecommendations.h"

#include <jni.h>
#include <openxr/openxr.h>
#include <thread>

namespace Horo::XR::OpenXRInternal {
    /** @brief Host-owned JNI globals for one exact Activity, retained until native retirement.
     * @details Handler composition keeps the Context through loader unload and supplies a new owner after Activity replacement.
     */
    struct NativeOpenXRAndroidApplication final {
        JavaVM *vm{};
        jobject applicationContext{};         /**< Global process application Context reference. */
        jobject activity{};                   /**< Global reference to the exact selected Activity. */
        std::shared_ptr<const void> lifetime; /**< Host token retaining both references. */
    };

    inline constexpr std::string_view AndroidOpenXRInstanceExtension{"XR_KHR_android_create_instance"};

    /** @brief Initializes the selected Android loader with the retained process application Context.
     * @param dispatch Exact global dispatch from the already verified library.
     * @param application Android host's retained VM, Context and Activity.
     * @return Native initialization result or a typed unrecoverable bootstrap failure.
     */
    [[nodiscard]] XrResult InitializeAndroidOpenXRLoader(PFN_xrGetInstanceProcAddr dispatch,
                                                         const NativeOpenXRAndroidApplication &application) noexcept;

    /** @brief Creates an instance with the selected Android Activity at the head of the admitted next chain.
     * @param create Exact native creation procedure from the initialized loader.
     * @param info Canonical native instance request already admitting the Android create extension.
     * @param application Android host's retained VM, Context and Activity.
     * @param instance Instance result written by the native runtime.
     * @return Native creation result, or typed invalid bootstrap input.
     */
    [[nodiscard]] XrResult CreateAndroidOpenXRInstance(PFN_xrCreateInstance create, const XrInstanceCreateInfo &info,
                                                       const NativeOpenXRAndroidApplication &application, XrInstance &instance) noexcept;

    /** @brief Terminal stage of the cold instance/system probe. */
    enum class AndroidOpenXRProbeStage {
        LoaderDispatch,
        LoaderInitialization,
        ExtensionAdmission,
        InstanceCreation,
        SystemDiscovery,
        RuntimeProperties,
        InstanceRetirement,
        Discovered
    };

    /** @brief Bounded native evidence. No instance, system identity, runtime name or tracking data escapes. */
    struct AndroidOpenXRProbeResult final {
        AndroidOpenXRProbeStage stage;
        XrResult result;
        XrVersion runtimeVersion{};                                 /**< Set only after successful discovery and retirement. */
        std::optional<OpenXRPrimaryStereoExtents> recommendedViews; /**< Instance-scoped recommendations, when available. */
    };

    /** @brief One owner for a cold system diagnostic on the GameActivity control lane.
     * @details No native session, graphics binding or active runtime capability can be issued by this contract.
     * On failed instance retirement, the native owner and JNI/loader leases remain intact for explicit retry.
     */
    class AndroidOpenXRColdDiscovery final {
    public:
        /** @brief Retains the installed loader and exact Android Activity inputs for discovery and teardown.
         * @param loader Immutable native lease admitted from the installed base APK.
         * @param application Android host's retained application/Activity inputs.
         */
        AndroidOpenXRColdDiscovery(std::shared_ptr<const Platform::DynamicLibrary> loader,
                                   NativeOpenXRAndroidApplication application) noexcept;
        /** @brief Enforces successful native retirement before releasing the loader and JNI references. */
        ~AndroidOpenXRColdDiscovery();
        AndroidOpenXRColdDiscovery(const AndroidOpenXRColdDiscovery &) = delete;
        AndroidOpenXRColdDiscovery &operator=(const AndroidOpenXRColdDiscovery &) = delete;

        /** @brief Opens an instance and finds an HMD system, then retires before returning success.
         * @param apiVersion Explicit native API version for this diagnostic profile.
         * @return The terminal stage and native result. A retirement failure retains the instance for explicit retry.
         */
        [[nodiscard]] AndroidOpenXRProbeResult Probe(XrVersion apiVersion) noexcept;

        /** @brief Retries retirement of the exact retained instance on its control lane.
         * @return Native retirement result. A failed call keeps retirement debt.
         */
        [[nodiscard]] XrResult Retire() noexcept;

    private:
        [[nodiscard]] AndroidOpenXRProbeResult Discover(XrVersion apiVersion) noexcept;

        const std::thread::id controlLane_{std::this_thread::get_id()};
        NativeOpenXRAndroidApplication application_;
        std::shared_ptr<const Platform::DynamicLibrary> loader_;
        XrInstance instance_{XR_NULL_HANDLE};
        PFN_xrDestroyInstance destroy_{};
    };
}  // namespace Horo::XR::OpenXRInternal
#endif
