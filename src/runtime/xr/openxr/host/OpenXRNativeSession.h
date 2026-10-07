#pragma once

/** @file OpenXRNativeSession.h
 * @brief Non-installed explicit host composition and native session ownership contract.
 */

#include "Horo/XR/XRLoaderPreflight.h"
#include "OpenXRHostBindings.h"

#include <array>
#include <memory>
#include <optional>
#include <span>

namespace Horo::XR::OpenXRInternal {
    /** @brief Fixed native discovery limits; oversized publications fail, never truncate. */
    inline constexpr std::size_t MaximumNativeExtensions = 128;
    inline constexpr std::size_t MaximumNativeLayers = 32;
    inline constexpr std::size_t MaximumEnabledExtensions = 16;

    /** @brief Private native extension policy; host construction copies names only during bounded activation. */
    struct NativeExtensionRequest final {
        std::string_view name;
        bool required{};
    };

    /** @brief Non-owning private native tuple; valid only during a fenced control-thread host operation before Close. */
    struct NativeSessionBorrow final {
        XrInstance instance;
        XrSystemId system;
        XrSession session;
    };

    /** @brief Exact host publication fence, called before acquisition and before exposing a native candidate. */
    class IOpenXRCompositionFence {
    public:
        virtual ~IOpenXRCompositionFence() = default;
        /** @brief Validates current runtime, capability, selected graphics device and installation attempt. */
        [[nodiscard]] virtual Result<void> Validate(const XRSessionId &candidate, const XRLoaderPreflightSnapshot &preflight,
                                                    const XRFeaturePlan &plan) const = 0;
    };

    /** @brief Borrowed activation input; no spans or snapshot references escape Create. */
    struct NativeSessionRequest final {
        XRSessionId candidate;
        const XRLoaderPreflightSnapshot &preflight;
        const XRCapabilitySnapshot &capabilities;
        const XRFeaturePlan &plan;
        XRLoaderPreflightAttempt activeAttempt;
        std::span<const NativeExtensionRequest> extensions;
        std::span<const std::string_view> layers;
        bool developmentLayersApproved{};
        XRPreflightProductMode productMode{XRPreflightProductMode::Shipping};
    };

    /**
     * @brief Concrete single-control-thread native transaction composed only by the application host.
     *
     * Loader shared ownership outlives every dispatch/native object. Host graphics/fence ports outlive this owner.
     * Create prepares beside any other separately owned generation; it never changes the XRRuntime publication.
     * The host publishes Ready only after its remaining spaces/swapchain/Renderer/Input stages succeed.
     * No operation selects a renderer, searches for a loader or accepts a headless graphics binding.
     */
    class OpenXRNativeSession final {
    public:
        /** @brief Retains an exact verified loader lease and borrows host-selected ports. */
        OpenXRNativeSession(std::shared_ptr<const Platform::DynamicLibrary> loader, IOpenXRGraphicsBinding &graphics,
                            const IOpenXRCompositionFence &fence) noexcept;
        /** @brief Performs reverse-order cleanup; host must quiesce native users and Renderer first. */
        ~OpenXRNativeSession();
        OpenXRNativeSession(const OpenXRNativeSession &) = delete;
        OpenXRNativeSession &operator=(const OpenXRNativeSession &) = delete;

        /** @brief Acquires one exact instance/system/session atomically or rolls candidate resources back. */
        [[nodiscard]] Result<void> Create(const NativeSessionRequest &request);
        /** @brief Fences retained Horo identity before downstream native use. */
        [[nodiscard]] Result<void> Validate(const XRSessionId &session) const;
        /** @brief Idempotently retires native resources; failed destruction retains handles/loader for explicit retry. */
        [[nodiscard]] Result<void> Close();
        /** @brief Returns only Horo identity, invalid until complete candidate creation succeeds. */
        [[nodiscard]] XRSessionId Session() const noexcept;
        /** @brief Returns whether negotiation enabled an exact extension; false while unpublished. */
        [[nodiscard]] bool HasExtension(std::string_view name) const noexcept;
        /**
         * @brief Borrows the exact complete native tuple for explicitly composed downstream native adapters.
         * @param session Exact Horo identity; stale owner/capability/device evidence rejects the borrow.
         * @return Native tuple or actionable admission failure. No ownership is transferred.
         * @pre The host serializes native use and teardown on the declared control thread.
         * @post The borrow must not escape that operation or survive Close, replacement or capability revocation.
         */
        [[nodiscard]] Result<NativeSessionBorrow> Borrow(const XRSessionId &session) const;

    private:
        [[nodiscard]] Result<void> ResolveGlobal();
        [[nodiscard]] Result<void> DiscoverAndNegotiate(const NativeSessionRequest &request);
        /** @brief Validates a bounded layer publication and admits only explicitly approved requested layers. */
        [[nodiscard]] Result<void> DiscoverLayers(std::span<const std::string_view> requested);
        /** @brief Copies one valid, unique and available layer into owner-held fixed storage. */
        [[nodiscard]] Result<void> EnableLayer(std::string_view name, std::span<const XrApiLayerProperties> available);
        /** @brief Combines runtime/layer publications and admits host graphics before optional feature extensions. */
        [[nodiscard]] Result<void> NegotiateExtensions(std::span<const NativeExtensionRequest> requested);
        /** @brief Enables one previously validated policy name only when its bounded publication contains it. */
        [[nodiscard]] Result<void> EnableExtension(std::string_view name, bool required, std::span<const XrExtensionProperties> available);
        /** @brief Enumerates one bounded global/layer extension publication without truncation. */
        [[nodiscard]] Result<void> CollectExtensions(const char *layer,
                                                     std::array<XrExtensionProperties, MaximumNativeExtensions> &available,
                                                     std::uint32_t &availableCount) const;
        [[nodiscard]] Result<void> CreateInstance(const NativeSessionRequest &request);
        [[nodiscard]] Result<void> ResolveInstance();
        [[nodiscard]] Result<void> SelectSystem();
        [[nodiscard]] Result<void> CreateSession(const NativeSessionRequest &request);
        [[nodiscard]] Result<void> Fail(Error error);
        [[nodiscard]] Result<void> CheckCurrent(const NativeSessionRequest &request) const;
        /** @brief Runs bounded native preparation; the outer entry point owns exception rollback. */
        [[nodiscard]] Result<void> PrepareTransaction(const NativeSessionRequest &request);

        std::shared_ptr<const Platform::DynamicLibrary> loader_;
        IOpenXRGraphicsBinding *graphics_;
        const IOpenXRCompositionFence *fence_;

        /** @brief Official loader/candidate dispatch, valid only while loader_ and native owners survive. */
        struct NativeDispatch final {
            PFN_xrGetInstanceProcAddr getProc{};
            PFN_xrEnumerateApiLayerProperties enumerateLayers{};
            PFN_xrEnumerateInstanceExtensionProperties enumerateExtensions{};
            PFN_xrCreateInstance createInstance{};
            PFN_xrDestroyInstance destroyInstance{};
            PFN_xrGetSystem getSystem{};
            PFN_xrCreateSession createSession{};
            PFN_xrDestroySession destroySession{};
        };

        NativeDispatch dispatch_;

        XrInstance instance_{XR_NULL_HANDLE};
        XrSystemId system_{XR_NULL_SYSTEM_ID};
        XrSession session_{XR_NULL_HANDLE};
        XRSessionId owner_{};
        XRSessionId preparing_{};
        std::optional<XRLoaderPreflightSnapshot> retainedPreflight_;
        std::optional<XRFeaturePlan> retainedPlan_;
        std::uint64_t lastRuntime_{};
        std::uint32_t lastSessionGeneration_{};

        /** @brief Bounded negotiated names and stable ABI borrows into this immovable owner. */
        struct NegotiatedNames final {
            std::array<std::array<char, XR_MAX_EXTENSION_NAME_SIZE>, MaximumEnabledExtensions> extensionNames{};
            std::array<const char *, MaximumEnabledExtensions> extensions{};
            std::array<std::array<char, XR_MAX_API_LAYER_NAME_SIZE>, MaximumNativeLayers> layerNames{};
            std::array<const char *, MaximumNativeLayers> layers{};
            std::uint32_t extensionCount{};
            std::uint32_t layerCount{};
        };

        NegotiatedNames names_;

        bool graphicsPrepared_{};
    };
}  // namespace Horo::XR::OpenXRInternal
