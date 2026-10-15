#include "GameModuleHostDetail.h"
#include "Horo/Gameplay/GameModuleHost.h"
#include "Horo/Gameplay/GameplayErrors.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <system_error>

namespace Horo::Gameplay {
    namespace {
        [[nodiscard]] bool IsSafeAbsolutePath(const std::filesystem::path &path) noexcept {
            return !path.empty() && path.is_absolute() && std::ranges::none_of(path, [](const std::filesystem::path &part) {
                return part == "..";
            });
        }

        [[nodiscard]] Error ShadowCopyError(const std::string &message) {
            return MakeError(GameplayErrors::InvalidBehaviorComponent, "Gameplay module shadow copy failed: " + message);
        }

        [[nodiscard]] Result<std::filesystem::path> PrepareShadowRoot(const std::filesystem::path &shadowRoot) {
            std::error_code error;
            std::filesystem::create_directories(shadowRoot, error);  // NOSONAR
            if (error)
                return Result<std::filesystem::path>::Failure(ShadowCopyError(error.message()));
            const std::filesystem::path canonicalRoot = std::filesystem::weakly_canonical(shadowRoot, error);
            if (error || !IsSafeAbsolutePath(canonicalRoot))
                return Result<std::filesystem::path>::Failure(ShadowCopyError("shadow root validation failed"));
            return Result<std::filesystem::path>::Success(canonicalRoot);
        }

        [[nodiscard]] Result<std::filesystem::path> MakeShadowPath(const std::filesystem::path &libraryPath,
                                                                   const std::filesystem::path &canonicalRoot) {
            static std::atomic<std::uint64_t> sequence{1};
            const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
            const std::filesystem::path shadowPath =
                canonicalRoot / std::format("{}.horo_reload_{}_{}{}", libraryPath.stem().string(), timestamp, sequence.fetch_add(1),
                                            libraryPath.extension().string());
            std::error_code error;
            const std::filesystem::path canonicalShadow = std::filesystem::weakly_canonical(shadowPath, error);
            if (const std::filesystem::path relativeShadow = canonicalShadow.lexically_relative(canonicalRoot);
                error || relativeShadow.empty() || relativeShadow.is_absolute() || *relativeShadow.begin() == "..")
                return Result<std::filesystem::path>::Failure(ShadowCopyError("shadow path validation failed"));
            return Result<std::filesystem::path>::Success(canonicalShadow);
        }

        [[nodiscard]] Result<void> CopyShadowArtifact(const std::filesystem::path &source, const std::filesystem::path &destination) {
            std::error_code error;
            if (std::filesystem::copy_file(source, destination, std::filesystem::copy_options::none, error))  // NOSONAR
                return Result<void>::Success();
            return Result<void>::Failure(ShadowCopyError(error ? error.message() : "candidate could not be copied."));
        }
    }  // namespace

    /** @copydoc GameModuleHost::LoadShadowCopy */
    Result<std::unique_ptr<LoadedGameModule>> GameModuleHost::LoadShadowCopy(const std::filesystem::path &libraryPath,
                                                                             const std::filesystem::path &shadowRoot,
                                                                             const GameModuleLoadExpectation &expectation) const {
        if (!IsSafeAbsolutePath(libraryPath) || !IsSafeAbsolutePath(shadowRoot))
            return Result<std::unique_ptr<LoadedGameModule>>::Failure(
                ShadowCopyError("source and destination paths must be absolute and without traversal."));

        auto canonicalRoot = PrepareShadowRoot(shadowRoot);
        if (canonicalRoot.HasError())
            return Result<std::unique_ptr<LoadedGameModule>>::Failure(canonicalRoot.ErrorValue());
        auto shadowPath = MakeShadowPath(libraryPath, canonicalRoot.Value());
        if (shadowPath.HasError())
            return Result<std::unique_ptr<LoadedGameModule>>::Failure(shadowPath.ErrorValue());
        if (const Result<void> copied = CopyShadowArtifact(libraryPath, shadowPath.Value()); copied.HasError())
            return Result<std::unique_ptr<LoadedGameModule>>::Failure(copied.ErrorValue());

        return LoadArtifact(shadowPath.Value(), expectation, true);
    }
}  // namespace Horo::Gameplay
