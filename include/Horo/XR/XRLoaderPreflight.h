#pragma once

/**
 * @file XRLoaderPreflight.h
 * @brief Backend-neutral OpenXR loader discovery, runtime selection, and preflight evidence.
 */

#include "Horo/Foundation/StrongId.h"
#include "Horo/XR/XRContract.h"
#include "Horo/XR/XRIdentity.h"

#include <compare>
#include <cstdint>

namespace Horo::XR {
    /** @brief Strong non-zero identity used by the application-owned XR composition boundary. */
    template <typename Tag> using XRCompositionId = Foundation::Detail::NonZeroId64<Tag, XRErrors::IdentityInvalid>;

    struct XRBackendIdentityTag;
    struct XRInstallRecordIdentityTag;
    struct XRProductProfileIdentityTag;
    struct XRLoaderPreflightAttemptTag;

    /** @brief Stable Horo-owned backend identity; never a library or runtime name. */
    using XRBackendId = XRCompositionId<XRBackendIdentityTag>;
    /** @brief Stable identity of one verified installation record. */
    using XRInstallRecordId = XRCompositionId<XRInstallRecordIdentityTag>;
    /** @brief Stable product-owned XR capability-profile identity. */
    using XRProductProfileId = XRCompositionId<XRProductProfileIdentityTag>;
    /** @brief Monotonic identity of one application-owned loader preflight attempt. */
    using XRLoaderPreflightAttempt = XRGeneration<XRLoaderPreflightAttemptTag>;

    /** @brief Exact product-selected loader source; no fallback order is implied. */
    enum class XRLoaderSourcePolicy : std::uint8_t {
        BundledVerified,
        PlatformProvided,
        Count
    };

    /** @brief Product mode that controls whether a developer runtime override is admissible. */
    enum class XRPreflightProductMode : std::uint8_t {
        Shipping,
        Development,
        Count
    };

    /** @brief Runtime selection authority used by the official loader contract. */
    enum class XRRuntimeSelectionPolicy : std::uint8_t {
        SystemDefault,
        ApprovedDeveloperOverride,
        Count
    };

    /** @brief Loader discovery outcome before instance or session activation. */
    enum class XRLoaderAvailability : std::uint8_t {
        Available,
        Absent,
        Incompatible,
        OpenFailed,
        Count
    };

    /** @brief Active-runtime discovery outcome reported by the selected loader. */
    enum class XRRuntimeAvailability : std::uint8_t {
        Available,
        Unavailable,
        Rejected,
        Count
    };

    /** @brief Runtime-system discovery outcome before session activation. */
    enum class XRSystemAvailability : std::uint8_t {
        Supported,
        Unsupported,
        TemporarilyUnavailable,
        Count
    };

    /** @brief Backend-neutral semantic loader API version used only for compatibility admission. */
    struct XRLoaderApiVersion final {
        std::uint16_t major{}; /**< Breaking API generation; zero is reserved. */
        std::uint16_t minor{}; /**< Backward-compatible API feature level. */
        std::uint16_t patch{}; /**< Compatible correction level. */

        /** @brief Checks representation. @return True when the major generation is non-zero. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return major != 0;
        }

        constexpr auto operator<=>(const XRLoaderApiVersion &) const noexcept = default;
    };

    /** @brief Closed inclusive loader-version admission interval owned by product composition. */
    struct XRLoaderApiVersionRange final {
        XRLoaderApiVersion minimum; /**< Oldest admitted loader API. */
        XRLoaderApiVersion maximum; /**< Newest admitted loader API. */
    };

    /** @brief Immutable application input for exactly one bounded loader/runtime preflight. */
    struct XRLoaderPreflightRequest final {
        XRContractVersion contractVersion{CurrentXRContractVersion};              /**< Horo XRApi schema version. */
        XRLoaderPreflightAttempt attempt;                                         /**< Exact owner-issued attempt generation. */
        XRBackendId backend;                                                      /**< One explicitly composed backend. */
        XRInstallRecordId installRecord;                                          /**< One already verified installation record. */
        XRProductProfileId productProfile;                                        /**< Exact requirements resolved after discovery. */
        XRLoaderSourcePolicy loaderSource{XRLoaderSourcePolicy::BundledVerified}; /**< Single product-selected loader source. */
        XRPreflightProductMode productMode{XRPreflightProductMode::Shipping};     /**< Shipping or explicit development policy. */
        XRRuntimeSelectionPolicy runtimeSelection{XRRuntimeSelectionPolicy::SystemDefault}; /**< Runtime-selection authority. */
        XRLoaderApiVersionRange admittedLoaderVersions;                                     /**< Closed compatibility interval. */
        bool developerOverrideApproved{}; /**< Explicit host approval; must be false for system default. */
        bool cancellationRequested{};     /**< Cancellation sampled before native activation. */
    };

    /** @brief Bounded redacted evidence produced by the private loader adapter. */
    struct XRLoaderProbeEvidence final {
        XRLoaderPreflightAttempt attempt;                                  /**< Attempt that owns this evidence. */
        XRLoaderAvailability loader{XRLoaderAvailability::Absent};         /**< Loader discovery outcome. */
        XRRuntimeAvailability runtime{XRRuntimeAvailability::Unavailable}; /**< Active-runtime discovery outcome. */
        XRSystemAvailability system{XRSystemAvailability::Unsupported};    /**< System discovery outcome. */
        XRLoaderApiVersion loaderApiVersion;                               /**< Observed semantic version when loader is available. */
        XRRuntimeGeneration runtimeGeneration;                             /**< Candidate runtime incarnation when fully supported. */
        std::uint8_t consumedProbeSteps{};                                 /**< Finite private probe work represented by this evidence. */
    };

    /** @brief Compile-time work ceiling for one loader/runtime/system preflight. */
    inline constexpr std::uint8_t MaximumXRLoaderPreflightSteps = 8;

    /** @brief Immutable successful preflight evidence; it owns no native handle, path, or callback. */
    class XRLoaderPreflightSnapshot final {
    public:
        /** @brief Returns the Horo contract version. @return Exact immutable contract version. */
        [[nodiscard]] XRContractVersion ContractVersion() const noexcept;
        /** @brief Returns the owner attempt. @return Exact generation-safe preflight attempt. */
        [[nodiscard]] XRLoaderPreflightAttempt Attempt() const noexcept;
        /** @brief Returns the composed backend. @return Stable Horo backend identity. */
        [[nodiscard]] XRBackendId Backend() const noexcept;
        /** @brief Returns the verified install record. @return Stable installation identity. */
        [[nodiscard]] XRInstallRecordId InstallRecord() const noexcept;
        /** @brief Returns the admitted product profile. @return Stable product-profile identity. */
        [[nodiscard]] XRProductProfileId ProductProfile() const noexcept;
        /** @brief Returns the exact loader source. @return Product-selected source with no fallback. */
        [[nodiscard]] XRLoaderSourcePolicy LoaderSource() const noexcept;
        /** @brief Returns the exact product mode used for this probe. @return Immutable shipping or development policy. */
        [[nodiscard]] XRPreflightProductMode ProductMode() const noexcept;
        /** @brief Returns the exact runtime-selection policy. @return Validated immutable selection policy. */
        [[nodiscard]] XRRuntimeSelectionPolicy RuntimeSelection() const noexcept;
        /** @brief Returns the observed loader API. @return Validated version within the admitted interval. */
        [[nodiscard]] XRLoaderApiVersion LoaderApiVersion() const noexcept;
        /** @brief Returns the candidate runtime incarnation. @return Non-zero generation owned by this successful attempt. */
        [[nodiscard]] XRRuntimeGeneration RuntimeGeneration() const noexcept;
        /** @brief Returns represented bounded probe work. @return Value in [1, MaximumXRLoaderPreflightSteps]. */
        [[nodiscard]] std::uint8_t ConsumedProbeSteps() const noexcept;

    private:
        friend Result<XRLoaderPreflightSnapshot> CreateXRLoaderPreflightSnapshot(const XRLoaderPreflightRequest &,
                                                                                 const XRLoaderProbeEvidence &);
        XRLoaderPreflightSnapshot(const XRLoaderPreflightRequest &request, const XRLoaderProbeEvidence &evidence) noexcept;

        XRContractVersion contractVersion_;
        XRLoaderPreflightAttempt attempt_;
        XRBackendId backend_;
        XRInstallRecordId installRecord_;
        XRProductProfileId productProfile_;
        XRLoaderSourcePolicy loaderSource_;
        XRRuntimeSelectionPolicy runtimeSelection_;
        XRLoaderApiVersion loaderApiVersion_;
        XRRuntimeGeneration runtimeGeneration_;
        std::uint8_t consumedProbeSteps_{};
        XRPreflightProductMode productMode_{XRPreflightProductMode::Shipping};
    };

    /**
     * @brief Validates bounded private discovery evidence and forms one immutable successful preflight snapshot.
     * @param request Application-owned composition and selection policy.
     * @param evidence Redacted evidence from exactly one private loader adapter probe.
     * @return Snapshot or a typed invalid, cancelled, loader, runtime, system, override, or stale failure.
     * @post Performs no loader search, native call, allocation, publication, fallback, or ambient-state mutation.
     */
    [[nodiscard]] Result<XRLoaderPreflightSnapshot> CreateXRLoaderPreflightSnapshot(const XRLoaderPreflightRequest &request,
                                                                                    const XRLoaderProbeEvidence &evidence);

    /**
     * @brief Revalidates retained successful preflight evidence before native activation.
     * @param snapshot Retained immutable preflight result.
     * @param activeAttempt Current attempt, or invalid after cancellation/shutdown.
     * @param expectedBackend Current explicitly composed backend.
     * @param expectedInstallRecord Current verified installation record.
     * @param expectedProductProfile Current product profile.
     * @return Success or a typed invalid or stale failure.
     * @post Performs bounded constant work with no allocation, native call, I/O, synchronization, or state mutation.
     */
    [[nodiscard]] Result<void> ValidateXRLoaderPreflight(const XRLoaderPreflightSnapshot &snapshot, XRLoaderPreflightAttempt activeAttempt,
                                                         XRBackendId expectedBackend, XRInstallRecordId expectedInstallRecord,
                                                         XRProductProfileId expectedProductProfile);
}  // namespace Horo::XR
