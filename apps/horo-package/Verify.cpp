#include "Horo/Foundation/Sha256.h"
#include "PackageCommand.h"
#include "SignatureDocument.h"

#include <chrono>
#include <format>
#include <utility>

namespace Horo::PackageCommand {
    Outcome Inspect(const std::filesystem::path &archivePath) {
        std::vector<std::byte> bytes;
        if (const auto read = ReadBounded(archivePath, MaximumArtifactBytes, bytes); !read.success)
            return read;
        std::optional<Packages::ValidatedPackageArchive> archive;
        if (const auto checked = VerifyArchive(bytes, archive); !checked.success)
            return checked;
        return Success(std::format("files={} archive={} manifest={} inventory={}", archive->Manifest().Entries().size(),
                                   FormatSha256(archive->Digest()), FormatSha256(archive->PackageManifestDigest()),
                                   FormatSha256(archive->Manifest().Digest())));
    }

    Outcome Verify(const std::filesystem::path &archivePath, const std::filesystem::path &signaturePath,
                   const std::filesystem::path &trustPath, const std::string_view packageId) {
        auto package = Packages::HoroPackageId::Parse(packageId);
        if (package.HasError())
            return Failure("package.identity_invalid", "Package identity is invalid.");
        std::vector<std::byte> bytes;
        if (const auto read = ReadBounded(archivePath, MaximumArtifactBytes, bytes); !read.success)
            return read;
        std::optional<Packages::ValidatedPackageArchive> archive;
        if (const auto checked = VerifyArchive(bytes, archive); !checked.success)
            return checked;

        Packages::PackagePublisherVerificationPolicy policy;
        if (const auto loaded = LoadTrust(trustPath, policy); !loaded.success)
            return loaded;
        auto service = Packages::PackagePublisherVerificationService::Create(std::move(policy), Security::CreateMbedTlsSignatureProvider());
        if (service.HasError())
            return Failure("package.trust_invalid", "Installer publisher policy rejected the trust file.");
        std::optional<Security::DetachedSignatureEnvelope> signature;
        if (!signaturePath.empty()) {
            Security::DetachedSignatureEnvelope parsed;
            if (const auto loaded = LoadSignature(signaturePath, parsed); !loaded.success)
                return loaded;
            signature.emplace(std::move(parsed));
        }
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
        if (auto verified = service.Value().Verify({.package = std::move(package).Value(),
                                                    .artifact = std::move(bytes),
                                                    .signature = std::move(signature),
                                                    .nowUnixMilliseconds = static_cast<std::uint64_t>(now.count())});
            verified.HasError() || !verified.Value().decision.installPermitted)
            return Failure("package.publisher_rejected", "Installer publisher policy rejected the artifact.");
        return Success("verified archive and publisher policy; sha256=" + FormatSha256(archive->Digest()));
    }
}  // namespace Horo::PackageCommand
