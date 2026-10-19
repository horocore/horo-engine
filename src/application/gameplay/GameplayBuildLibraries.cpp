#include "GameplayBuildLibraries.h"

#include "Horo/Packages/PackageRequest.h"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>

namespace Horo::Application::Detail {
    namespace {
        using StringSet = std::set<std::string, std::less<>>;
        const ErrorDomainId Domain{"horo.application.gameplay_libraries"};
        const ErrorCodeDescriptor InvalidInput{Domain,
                                               ErrorCode{"invalid_input"},
                                               ErrorSeverity::Error,
                                               "Gameplay library inputs are stale, incompatible or unavailable.",
                                               "Restore current packages and select approved source artifacts again.",
                                               false,
                                               true};

        /** @brief Reads actual bounded project metadata; caller-supplied digests cannot stand in for current intent. */
        Result<std::string> ReadMetadata(const std::filesystem::path &path, const std::uintmax_t maximumBytes) {
            std::error_code error;
            const auto size = std::filesystem::file_size(path, error);
            if (error || size > maximumBytes)
                return Result<std::string>::Failure(MakeError(InvalidInput));
            if (const auto status = std::filesystem::symlink_status(path, error); error || !std::filesystem::is_regular_file(status))
                return Result<std::string>::Failure(MakeError(InvalidInput));
            std::ifstream stream{path, std::ios::binary};
            std::string bytes(static_cast<std::size_t>(size), '\0');
            stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            if (!stream || static_cast<std::uintmax_t>(stream.gcount()) != size)
                return Result<std::string>::Failure(MakeError(InvalidInput));
            return Result<std::string>::Success(std::move(bytes));
        }

        /** @brief Requires the selected archive and every installed dependency to match the current exact lock. */
        Result<void> ValidateInstalledGraph(const Packages::GameplaySourceLibrary &library,
                                            const Packages::ValidatedPackageLockfileV1 &lock) {
            const auto &graph = *library.Install()->Graph();
            if (auto valid = lock.ValidateForRestore(graph.requestHash, graph.platform); valid.HasError())
                return valid;
            if (lock.Packages().size() != graph.packages.size())
                return Result<void>::Failure(MakeError(InvalidInput));
            for (const auto &installed : graph.packages) {
                const auto current = std::ranges::find(lock.Packages(), installed.lock.package, &Packages::LockedPackage::package);
                if (current == lock.Packages().end() || current->version != installed.lock.version ||
                    current->source != installed.lock.source || current->artifactDigest != installed.lock.artifactDigest ||
                    current->manifestDigest != installed.lock.manifestDigest ||
                    current->fileManifestDigest != installed.lock.fileManifestDigest ||
                    current->dependencies != installed.lock.dependencies)
                    return Result<void>::Failure(MakeError(InvalidInput));
            }
            return Result<void>::Success();
        }

        /** @brief Enforces aggregate work bounds and rejects competing selections for the same package contribution. */
        Result<StringSet> LibraryFingerprints(const GameplayBuildRequest &request, const Packages::ValidatedPackageLockfileV1 &lock) {
            StringSet ordered;
            std::set<std::pair<std::string, std::string>> selections;
            std::uint64_t bytes{};
            std::size_t files{};
            for (const auto &library : GameplayLibraries(request)) {
                if (const auto valid = ValidateInstalledGraph(library, lock); valid.HasError())
                    return Result<StringSet>::Failure(valid.ErrorValue());
                if (!selections.emplace(library.Package().lock.package.Value(), library.Root().Value()).second)
                    return Result<StringSet>::Failure(MakeError(InvalidInput));
                files += library.Files().size();
                if (files > 4096U)
                    return Result<StringSet>::Failure(MakeError(InvalidInput));
                for (const auto &file : library.Files()) {
                    if (file.size > 64U * 1024U * 1024U - bytes)
                        return Result<StringSet>::Failure(MakeError(InvalidInput));
                    bytes += file.size;
                }
                ordered.insert(FormatSha256(library.Fingerprint()));
            }
            return Result<StringSet>::Success(std::move(ordered));
        }

        /** @brief Rejects a stale configure receipt even when its source paths happen to match this selection. */
        Result<void> ValidatePreparationReceipt(const std::filesystem::path &buildRoot) {
            const auto manifestPath = buildRoot / "gameplay_source_libraries.json";
            const auto prepared = ReadMetadata(manifestPath, 2U * 1024U * 1024U);
            if (prepared.HasError())
                return Result<void>::Failure(prepared.ErrorValue());
            auto receiptPath = manifestPath;
            receiptPath += ".consumed";
            const auto receipt = ReadMetadata(receiptPath, 128U);
            if (receipt.HasError())
                return Result<void>::Failure(receipt.ErrorValue());
            if (const auto expected = FormatSha256(ComputeSha256(std::as_bytes(std::span{prepared.Value()})));
                receipt.Value() != expected + '\n' && receipt.Value() != expected + "\r\n")
                return Result<void>::Failure(MakeError(InvalidInput, "The primary SDK module did not consume this preparation."));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc GameplayLibraries */
    std::span<const Packages::GameplaySourceLibrary> GameplayLibraries(const GameplayBuildRequest &request) noexcept {
        return request.sourceLibraries ? request.sourceLibraries->Libraries() : std::span<const Packages::GameplaySourceLibrary>{};
    }

    /** @copydoc GameplayLibraryPathFromUtf8 */
    std::filesystem::path GameplayLibraryPathFromUtf8(const std::string_view text) {
        return std::filesystem::path{std::u8string{text.begin(), text.end()}};
    }

    /** @copydoc GameplayLibraryPathToUtf8 */
    std::string GameplayLibraryPathToUtf8(const std::filesystem::path &path) {
        const auto text = path.generic_u8string();
        return {text.begin(), text.end()};
    }

    /** @copydoc GameplayLibraryCacheKey */
    std::string GameplayLibraryCacheKey(const Packages::GameplaySourceLibrary &library) {
        constexpr std::string_view Prefix{"sha256:"};
        return FormatSha256(library.Fingerprint()).substr(Prefix.size());
    }

    /** @copydoc GameplayLibraryInputIdentity */
    Result<std::string> GameplayLibraryInputIdentity(const GameplayBuildRequest &request) {
        if (GameplayLibraries(request).empty())
            return Result<std::string>::Success({});
        if (GameplayLibraries(request).size() > 64U)
            return Result<std::string>::Failure(MakeError(InvalidInput));
        auto intentBytes = ReadMetadata(request.projectRoot / ".horo/packages.json", 1024U * 1024U);
        if (intentBytes.HasError())
            return Result<std::string>::Failure(intentBytes.ErrorValue());
        auto lockBytes = ReadMetadata(request.projectRoot / ".horo/packages.lock", 4U * 1024U * 1024U);
        if (lockBytes.HasError())
            return Result<std::string>::Failure(lockBytes.ErrorValue());
        const auto intent = Packages::ValidatedPackageRequest::Parse(intentBytes.Value());
        if (intent.HasError())
            return Result<std::string>::Failure(intent.ErrorValue());
        const auto lock = Packages::ValidatedPackageLockfileV1::Parse(lockBytes.Value());
        if (lock.HasError())
            return Result<std::string>::Failure(lock.ErrorValue());
        if (const auto valid = intent.Value().ValidateLock(lock.Value()); valid.HasError())
            return Result<std::string>::Failure(valid.ErrorValue());
        const auto ordered = LibraryFingerprints(request, lock.Value());
        if (ordered.HasError())
            return Result<std::string>::Failure(ordered.ErrorValue());
        std::string identity = lock.Value().SerializeCanonical();
        for (const auto &fingerprint : ordered.Value())
            identity.append(fingerprint).push_back('\0');
        return Result<std::string>::Success(FormatSha256(ComputeSha256(std::as_bytes(std::span{identity}))));
    }

    /** @copydoc ValidateGameplayLibraryConsumption */
    Result<void> ValidateGameplayLibraryConsumption(const GameplayBuildRequest &request, const std::filesystem::path &buildRoot) {
        if (GameplayLibraries(request).empty())
            return Result<void>::Success();
        if (const auto consumed = ValidatePreparationReceipt(buildRoot); consumed.HasError())
            return consumed;
        const auto manifest = ReadMetadata(request.projectRoot / ".horo/local/gameplay_build_inputs.txt", 4U * 1024U * 1024U);
        if (manifest.HasError())
            return Result<void>::Failure(manifest.ErrorValue());
        StringSet inputs;
        std::istringstream lines{manifest.Value()};
        for (std::string line; std::getline(lines, line);) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            inputs.insert(line);
        }
        for (const auto &library : GameplayLibraries(request)) {
            for (const auto &source : library.Sources()) {
                const auto path = buildRoot / "imported" / GameplayLibraryCacheKey(library) / GameplayLibraryPathFromUtf8(source.Value());
                const auto relative = GameplayLibraryPathToUtf8(path.lexically_relative(request.projectRoot));
                if (!inputs.contains(relative))
                    return Result<void>::Failure(MakeError(InvalidInput, "The primary SDK module did not consume an imported source."));
            }
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Application::Detail
