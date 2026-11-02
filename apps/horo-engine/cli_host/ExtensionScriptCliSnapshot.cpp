#include "ExtensionScriptCliInternal.h"
#include "Horo/Extensions/ExtensionErrors.h"
#include "Horo/Foundation/Platform.h"

namespace Horo::Application::Internal {
    namespace {
        template <typename T> Result<T> IoFailure() {
            return Result<T>::Failure(
                MakeError(Extensions::ExtensionErrors::LoadFailed,
                          "Use an existing exclusively owned temporary parent outside the signed package and host trust roots."));
        }

        bool Within(std::filesystem::path path, const std::filesystem::path &root) {
            for (;;) {
                if (path == root)
                    return true;
                const auto parent = path.parent_path();
                if (parent == path)
                    return false;
                path = parent;
            }
        }

        struct Candidate final {
            std::filesystem::path path;
            bool owned{};

            ~Candidate() {
                if (owned) {
                    std::error_code error;
                    std::filesystem::remove_all(path, error);
                }
            }

            Result<void> Claim(const ExtensionScriptOptions &options) {
                if (Within(options.temporaryRoot, options.packageRoot) || Within(options.packageRoot, options.temporaryRoot))
                    return IoFailure<void>();
                for (unsigned index = 0; index < 32; ++index) {
                    path = options.temporaryRoot / (".horo-extension-script-" + std::to_string(index));
                    std::error_code error;
                    owned = std::filesystem::create_directory(path, error);
                    if (owned) {
                        std::filesystem::permissions(path, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace,
                                                     error);
                        if (error)
                            return IoFailure<void>();
                        return Result<void>::Success();
                    }
                    if (error && error != std::errc::file_exists)
                        return IoFailure<void>();
                }
                return IoFailure<void>();
            }
        };

        Result<void> Write(NativeDurableFileSystem &files, const std::filesystem::path &root, const ExtensionScriptArtifact &artifact) {
            const auto destination = root / artifact.relative;
            std::error_code error;
            std::filesystem::create_directories(destination.parent_path(), error);
            if (error)
                return IoFailure<void>();
            bool created{};
            return files.WritePrivateDurable(destination, artifact.bytes, created);
        }
    }  // namespace

    Result<std::filesystem::path> StageExtensionScript(const ExtensionScriptOptions &options, const AuthenticatedExtensionScript &packet) {
        auto trusted = RevalidateExtensionScript(packet);
        if (trusted.HasError())
            return Result<std::filesystem::path>::Failure(trusted.ErrorValue());
        Candidate candidate;
        if (auto claimed = candidate.Claim(options); claimed.HasError())
            return Result<std::filesystem::path>::Failure(claimed.ErrorValue());
        NativeDurableFileSystem files;
        for (const auto &artifact : packet.artifacts)
            if (auto written = Write(files, candidate.path, artifact); written.HasError())
                return Result<std::filesystem::path>::Failure(written.ErrorValue());
        candidate.owned = false;
        return Result<std::filesystem::path>::Success(std::move(candidate.path));
    }

    ExtensionScriptArtifactGate::ExtensionScriptArtifactGate(std::filesystem::path root,
                                                             std::shared_ptr<const AuthenticatedExtensionScript> packet)
        : root_(std::move(root)), packet_(std::move(packet)) {}

    Result<Security::VerifiedArtifactEvidence> ExtensionScriptArtifactGate::Verify(const std::filesystem::path &path) const {
        auto trusted = RevalidateExtensionScript(*packet_);
        if (trusted.HasError())
            return Result<Security::VerifiedArtifactEvidence>::Failure(trusted.ErrorValue());
        for (const auto &artifact : packet_->artifacts)
            if (path == root_ / artifact.relative)
                return trusted.Value()->VerifyFile(path, artifact.bytes.size(), artifact.signature);
        return Result<Security::VerifiedArtifactEvidence>::Failure(MakeError(Extensions::ExtensionErrors::ContributionRejected));
    }
}  // namespace Horo::Application::Internal
