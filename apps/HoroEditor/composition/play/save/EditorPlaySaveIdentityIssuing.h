#pragma once
/** @file EditorPlaySaveIdentityIssuing.h
 * @brief Existing Play sandbox UUID issuance from its explicitly retained entropy source.
 */
#include "Horo/Runtime/Save/SaveIdentity.h"
#include "Horo/Security/SecureMemory.h"

namespace Horo::Application::Internal {
    /** @brief Issues one opaque UUID from actual host entropy; no Scene slot, path, hash, counter or global namespace participates.
     * @tparam Identity Existing canonical persistent Save identity type.
     * @param entropy Actual host-owned secure source, synchronously borrowed.
     * @return Valid canonical identity or original entropy/identity failure.
     */
    template <typename Identity> Result<Identity> IssuePlaySaveIdentity(Security::SecureRandomSource &entropy) {
        Runtime::SaveIdentityDetail::Bytes bytes{};
        if (const auto filled = entropy.Fill(std::as_writable_bytes(std::span{bytes})); filled.HasError())
            return Result<Identity>::Failure(filled.ErrorValue());
        bytes[6] = static_cast<std::uint8_t>((bytes[6] & 15U) | 64U);
        bytes[8] = static_cast<std::uint8_t>((bytes[8] & 63U) | 128U);
        return Identity::FromBytes(bytes);
    }
}  // namespace Horo::Application::Internal
