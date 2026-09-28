#include "Horo/Release/UpdateDownloadSession.h"
#include "Horo/Release/UpdateHttpDownload.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
    class AcceptingProvider final : public Horo::Security::SignatureProvider {
    public:
        [[nodiscard]] bool Supports(Horo::Security::SignatureAlgorithm) const noexcept override {
            return true;
        }

        [[nodiscard]] Horo::Result<void> Verify(Horo::Security::SignatureAlgorithm, std::span<const std::byte>, const Horo::Sha256Digest &,
                                                std::span<const std::byte>) const override {
            return Horo::Result<void>::Success();
        }
    };

    [[nodiscard]] Horo::Security::ArtifactVerifier Verifier() {
        auto roots = std::make_shared<Horo::Security::TrustedRootStore>();
        std::vector<std::byte> key(65U, std::byte{1});
        key.front() = std::byte{0x04};
        if (roots->Add({.publisherId = "com.horo.updates", .keyId = "key-1", .publicKey = std::move(key)}).HasError())
            throw std::runtime_error("Unable to establish test trust root");
        return {std::make_shared<AcceptingProvider>(), std::move(roots)};
    }

    [[nodiscard]] Horo::Release::UpdatePackageRecord Package(const std::string &url, const std::string_view payload) {
        Horo::Release::UpdatePackageRecord package;
        package.url = url;
        package.size = payload.size();
        package.digest = Horo::ComputeSha256(std::as_bytes(std::span{payload}));
        package.signature = {.publisherId = "com.horo.updates",
                             .keyId = "key-1",
                             .artifactDigest = package.digest,
                             .signature = std::vector<std::byte>(64U, std::byte{1})};
        return package;
    }

    [[nodiscard]] bool PrepareResume(const Horo::Release::UpdatePackageRecord &package, const Horo::Release::UpdateDownloadPaths &paths,
                                     const Horo::Release::UpdateDownloadLimits &limits, Horo::NativeDurableFileSystem &files,
                                     const std::string_view payload) {
        const Horo::Release::UpdateTransferResponse response{.status = 200U,
                                                             .requestedUrl = package.url,
                                                             .effectiveUrl = package.url,
                                                             .strongEtag = "\"version-1\"",
                                                             .contentLength = package.size};
        auto started = Horo::Release::UpdateDownloadSession::Begin(package, response, paths, limits, files, {});
        if (started.HasError())
            return false;
        auto session = std::move(started).Value();
        return session.Append(std::as_bytes(std::span{payload.data(), 7U})).HasValue();
    }
}  // namespace

int main(const int argc, const char *const argv[]) {
    if (argc != 5)
        return 2;
    const std::string url = argv[1];
    const std::filesystem::path caBundle = argv[2];
    const std::filesystem::path stageRoot = argv[3];
    const std::string mode = argv[4];
    constexpr std::string_view payload = "verified-update-package";
    const auto package = Package(url, payload);
    const Horo::Release::UpdateDownloadPaths paths{stageRoot / "package.partial", stageRoot / "package.checkpoint"};
    constexpr Horo::Release::UpdateDownloadLimits limits{.maximumPackageBytes = 1024U, .reserveBytes = 0U};
    Horo::NativeDurableFileSystem files;
    if ((mode == "resume" || mode == "bad-range") && !PrepareResume(package, paths, limits, files, payload))
        return 3;
    std::uint64_t durableProgress{};
    const auto downloaded =
        Horo::Release::DownloadUpdatePackageHttps({package, paths, limits, {.certificateAuthorityBundle = caBundle}}, files, Verifier(), {},
                                                  [&](const std::uint64_t durable, const std::uint64_t total) {
        if (total == package.size)
            durableProgress = durable;
    });
    if (mode == "bad-range" || mode == "redirect" || mode == "untrusted") {
        if (downloaded.HasValue() || (mode == "bad-range" && std::filesystem::file_size(paths.partialFile) != 7U) ||
            (mode == "untrusted" && std::filesystem::exists(paths.partialFile)) || std::filesystem::exists(stageRoot / "candidate.ready"))
            return 4;
        return 0;
    }
    if (downloaded.HasError()) {
        std::cerr << downloaded.ErrorValue().code.Value() << ": " << downloaded.ErrorValue().message << '\n';
        return 5;
    }
    if (downloaded.Value().durableBytes != package.size || durableProgress != package.size)
        return 6;
    std::ifstream input(paths.partialFile, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>{input}, {}};
    return bytes == payload ? 0 : 7;
}
