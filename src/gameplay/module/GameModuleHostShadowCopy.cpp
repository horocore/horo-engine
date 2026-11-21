#include "GameModuleHostDetail.h"
#include "Horo/Foundation/PathUtils.h"
#include "Horo/Foundation/Platform.h"
#include "Horo/Gameplay/GameModuleHost.h"
#include "Horo/Gameplay/GameplayErrors.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <string>
#include <system_error>

namespace Horo::Gameplay {
    namespace {
        using Foundation::Paths::NativeApiPath;

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
            std::filesystem::create_directories(NativeApiPath(shadowRoot), error);  // NOSONAR
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
            auto stem = libraryPath.stem().u8string();
            if (const auto marker = stem.find(u8".horo_reload_"); marker != std::u8string::npos)
                stem.resize(marker);
            if (constexpr std::size_t maximumStemBytes = 64; stem.size() > maximumStemBytes) {
                auto length = maximumStemBytes;
                while ((static_cast<unsigned char>(stem[length]) & 0xc0U) == 0x80U)
                    --length;
                stem.resize(length);
            }
            if (stem.empty())
                stem = u8"gameplay";
            auto filename = std::filesystem::path{stem};
            filename += std::format(".horo_reload_{}_{}", timestamp, sequence.fetch_add(1));
            filename += libraryPath.extension();
            const std::filesystem::path shadowPath = canonicalRoot / filename;
            std::error_code error;
            const std::filesystem::path canonicalShadow = std::filesystem::weakly_canonical(shadowPath, error);
            if (const std::filesystem::path relativeShadow = canonicalShadow.lexically_relative(canonicalRoot);
                error || relativeShadow.empty() || relativeShadow.is_absolute() || *relativeShadow.begin() == "..")
                return Result<std::filesystem::path>::Failure(ShadowCopyError("shadow path validation failed"));
            return Result<std::filesystem::path>::Success(canonicalShadow);
        }

        [[nodiscard]] Result<void> CopyShadowArtifact(const std::filesystem::path &source, const std::filesystem::path &destination) {
            std::error_code error;
            if (std::filesystem::copy_file(NativeApiPath(source), NativeApiPath(destination), std::filesystem::copy_options::none,
                                           error))  // NOSONAR
                return Result<void>::Success();
            return Result<void>::Failure(ShadowCopyError(error ? error.message() : "candidate could not be copied."));
        }

        /** @brief Removes only the private sibling created by this attempt until it transfers to the image owner. */
        struct RecordedShadow final {
            std::filesystem::path path;
            bool created{};

            ~RecordedShadow() {
                if (created) {
                    std::error_code ignored;
                    std::filesystem::remove(NativeApiPath(path), ignored);
                }
            }
        };

        /** @brief Writes the exact same bounded bytes that contribute to the replay identity. */
        Result<Sha256Digest> WriteRecordedShadow(const std::filesystem::path &source, RecordedShadow &copy,
                                                 const CancellationToken &cancellation) {
            NativeDurableFileSystem files;
            const auto bytes = files.ReadFileBounded(source, Detail::MaximumRecordedNativeArtifactBytes, cancellation);
            if (bytes.HasError())
                return Result<Sha256Digest>::Failure(bytes.ErrorValue());
            const auto data = std::as_bytes(std::span{bytes.Value()});
            const auto written = files.WritePrivateDurable(copy.path, data, copy.created);
            if (written.HasError())
                return Result<Sha256Digest>::Failure(written.ErrorValue());
            return Result<Sha256Digest>::Success(ComputeSha256(data));
        }
    }  // namespace

    /** @copydoc GameModuleHost::LoadShadowCopy */
    Result<GameModuleLoadCandidate> GameModuleHost::LoadShadowCopy(const std::filesystem::path &libraryPath,
                                                                   const std::filesystem::path &shadowRoot,
                                                                   const GameModuleLoadExpectation &expectation) const {
        if (!IsSafeAbsolutePath(libraryPath) || !IsSafeAbsolutePath(shadowRoot))
            return Result<GameModuleLoadCandidate>::Failure(
                ShadowCopyError("source and destination paths must be absolute and without traversal."));

        auto canonicalRoot = PrepareShadowRoot(shadowRoot);
        if (canonicalRoot.HasError())
            return Result<GameModuleLoadCandidate>::Failure(canonicalRoot.ErrorValue());
        auto shadowPath = MakeShadowPath(libraryPath, canonicalRoot.Value());
        if (shadowPath.HasError())
            return Result<GameModuleLoadCandidate>::Failure(shadowPath.ErrorValue());
        if (const Result<void> copied = CopyShadowArtifact(libraryPath, shadowPath.Value()); copied.HasError())
            return Result<GameModuleLoadCandidate>::Failure(copied.ErrorValue());

        return LoadArtifact(shadowPath.Value(), expectation, true);
    }

    /** @copydoc GameModuleHost::PrepareRecordedImage */
    Result<std::unique_ptr<PreparedGameModuleImage>> GameModuleHost::PrepareRecordedImage(const std::filesystem::path &libraryPath,
                                                                                          const GameModuleLoadExpectation &expectation,
                                                                                          const CancellationToken &cancellation) {
        using Output = Result<std::unique_ptr<PreparedGameModuleImage>>;
        if (!IsSafeAbsolutePath(libraryPath) ||
            libraryPath.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos)
            return Output::Failure(ShadowCopyError("recorded source must be an absolute artifact path."));
        std::error_code error;
        const auto source = std::filesystem::canonical(libraryPath, error);
        if (error || !IsSafeAbsolutePath(source))
            return Output::Failure(ShadowCopyError("recorded source must resolve to an absolute artifact."));
        auto target = MakeShadowPath(source, source.parent_path());
        if (target.HasError())
            return Output::Failure(target.ErrorValue());
        RecordedShadow copy{std::move(target).Value()};
        const auto digest = WriteRecordedShadow(source, copy, cancellation);
        if (digest.HasError())
            return Output::Failure(digest.ErrorValue());
        auto owner = PrepareArtifactImage(copy.path, expectation, true, cancellation, digest.Value());
        if (owner.HasError())
            return Output::Failure(owner.ErrorValue());
        copy.created = false;
        return Output::Success(std::unique_ptr<PreparedGameModuleImage>(new PreparedGameModuleImage(std::move(owner).Value())));
    }
}  // namespace Horo::Gameplay
