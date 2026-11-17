#pragma once
#include "Horo/Foundation/Result.h"
#include "Horo/Foundation/Sha256.h"

#include <memory>

namespace Horo::Platform {
    /** @brief Verified current process image evidence; construction is restricted to the native provider.
     * @details This establishes image identity, not publisher trust or determinism qualification.
     * The retained owner pins actual native code evidence and the opened image file.
     */
    class ProcessImageIdentity final {
    public:
        /** @brief Returns the SHA-256 of the complete verified executable file. */
        [[nodiscard]] const Sha256Digest &Artifact() const noexcept {
            return artifact_;
        }

        /** @brief Retains the actual evidence owner across Physics snapshots and restore candidates. */
        [[nodiscard]] std::shared_ptr<const void> Lease() const noexcept {
            return state_;
        }

        /** @brief Revalidates native process identity and its exact retained file before binding. */
        [[nodiscard]] Result<void> Verify() const;
        /** @brief Requires a participating code address to belong to this actual main executable image.
         * @param address Actual function address supplied by the trusted host composition.
         * @return Success only for the verified process image; a dynamic module requires its own loader lease.
         */
        [[nodiscard]] Result<void> VerifyStaticCodeAddress(const void *address) const;

    private:
        ProcessImageIdentity(Sha256Digest artifact, std::shared_ptr<const void> state) : artifact_(artifact), state_(std::move(state)) {}

        friend Result<ProcessImageIdentity> CaptureCurrentProcessImageIdentity();
        Sha256Digest artifact_;
        std::shared_ptr<const void> state_;
    };

    /** @brief Captures bounded actual process image identity or returns typed unavailability.
     * @return Immutable evidence; no caller-selected file, digest, path or trust override is accepted.
     */
    [[nodiscard]] Result<ProcessImageIdentity> CaptureCurrentProcessImageIdentity();
}  // namespace Horo::Platform
