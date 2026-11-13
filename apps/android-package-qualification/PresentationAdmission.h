#pragma once
/** @file PresentationAdmission.h @brief App-private projection of genuine native readiness into canonical lifecycle generations. */
#include "Horo/Platform/Internal/AndroidLifecycleAdapter.h"
#include "Horo/Platform/Internal/AndroidWindowHost.h"

#include <optional>

namespace Horo::AndroidQualification {
    namespace PresentationAdmissionErrors {
        extern const ErrorCodeDescriptor InvalidReceipt;
    }

    /** @brief Retains only derived native-ready facts; the lifecycle controller/adapter remain the generation authority. */
    class PresentationAdmission final {
    public:
        /** @brief Borrows the existing serialized owner authorities for this helper's complete lifetime.
         * @param lifecycle Canonical publication state. @param adapter Exact generation issuer for that controller. */
        PresentationAdmission(Platform::AndroidLifecycleController &lifecycle,
                              Platform::Android::AndroidLifecycleAdapter &adapter) noexcept;
        /** @brief Reconciles one completed native readiness poll, including replacement without an intervening false result.
         * @param window Actual committed window facts used by the native owner. @param ready Actual native readiness result.
         * @return Success or original typed authority failure; invalid/stale/pending evidence cannot manufacture readiness.
         * @details Activity admission remains separate. Changed ready facts retire the old presentation then issue a fresh generation.
         */
        [[nodiscard]] Result<void> Reconcile(const Platform::Android::AndroidWindowSnapshot &window, bool ready);

    private:
        [[nodiscard]] Result<void> Lose(const Platform::AndroidLifecycleSnapshot &current);
        [[nodiscard]] Result<void> Publish(const Platform::Android::AndroidWindowSnapshot &window);
        [[nodiscard]] Result<void> Commit();
        Platform::AndroidLifecycleController &lifecycle_;
        Platform::Android::AndroidLifecycleAdapter &adapter_;
        std::optional<Platform::Android::AndroidWindowSnapshot> ready_;
        std::uint64_t latestRevision_{}; /**< Derived native-facts watermark, never a lifecycle generation issuer. */
    };
}  // namespace Horo::AndroidQualification
