#pragma once

#include "Horo/Packages/PackagePublisherVerification.h"
#include "PackageCommand.h"

#include <filesystem>
#include <string>

namespace Horo::PackageCommand {
    [[nodiscard]] Outcome LoadSignature(const std::filesystem::path &path, Security::DetachedSignatureEnvelope &envelope);
    [[nodiscard]] Outcome LoadTrust(const std::filesystem::path &path, Packages::PackagePublisherVerificationPolicy &policy);
    [[nodiscard]] std::string EncodeSignature(const Security::DetachedSignatureEnvelope &envelope);
}  // namespace Horo::PackageCommand
