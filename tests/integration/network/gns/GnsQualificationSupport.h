#pragma once
/** @file GnsQualificationSupport.h @brief Private ephemeral PKI and credential capabilities for real child-process qualification. */
#include "GnsTransportFactory.h"

#include <filesystem>

namespace Horo::Network::NativeQualification {
    /** @brief Generate test-only CA and credential files inside an already exclusively owned private directory.
     * @param directory Parent-created directory, restricted before any secret is written.
     * @return Explicit generation/write failure or success. Existing files are never replaced.
     * @details No connection key is exported. The ephemeral CA is qualification provisioning only.
     */
    [[nodiscard]] Result<void> CreateEphemeralFixture(const std::filesystem::path &directory);
    /** @brief Load the explicit fixture root and retained issuer without starting a native adapter.
     * @param directory Owned fixture directory retained until both children have fully retired.
     * @param side Exact server0/client1 identity selection.
     * @param trust Same canonical policy later supplied to admission.
     * @return Exact native selection or a typed file/key failure; private material is wiped on every path.
     */
    [[nodiscard]] Result<GnsNativeSecurityOptions> LoadNativeSelection(const std::filesystem::path &directory, unsigned side,
                                                                       NetworkTrustPolicySnapshot trust);
    /** @brief Load only credential/proof/entropy providers; native certificate/peer/key/channel slots remain empty.
     * @param directory Same explicitly selected fixture directory.
     * @param side Exact server0/client1 principal selection.
     * @return Three capabilities or typed failure; never a native transport or activation authority.
     */
    [[nodiscard]] Result<NetworkAdmissionProviders> LoadCredentialProviders(const std::filesystem::path &directory, unsigned side);
}  // namespace Horo::Network::NativeQualification
