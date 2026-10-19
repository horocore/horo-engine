#include "GameplayBuildLibraries.h"
#include "PackageContentStaging.h"

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>

namespace Horo::Application::Detail {
    namespace {
        using CachedPaths = std::set<std::filesystem::path>;
        const ErrorDomainId Domain{"horo.application.gameplay_libraries"};
        const ErrorCodeDescriptor StorageFailed{Domain,
                                                ErrorCode{"storage_failed"},
                                                ErrorSeverity::Error,
                                                "Gameplay library cache could not be prepared or verified.",
                                                "Check project-local storage and remove damaged imported-library cache entries.",
                                                true,
                                                true};
        const ErrorCodeDescriptor Cancelled{Domain,
                                            ErrorCode{"cancelled"},
                                            ErrorSeverity::Info,
                                            "Gameplay library preparation was cancelled.",
                                            "Retry the build when ready.",
                                            true,
                                            false};

        /** @brief Removes only a freshly created private candidate; published cache directories remain available for incremental builds. */
        struct CandidateCleanup final {
            explicit CandidateCleanup(std::filesystem::path owned) : path(std::move(owned)) {}

            CandidateCleanup(const CandidateCleanup &) = delete;
            CandidateCleanup &operator=(const CandidateCleanup &) = delete;
            CandidateCleanup(CandidateCleanup &&) = delete;
            CandidateCleanup &operator=(CandidateCleanup &&) = delete;
            std::filesystem::path path;

            ~CandidateCleanup() {
                if (!path.empty()) {
                    std::error_code ignored;
                    std::filesystem::remove_all(path, ignored);
                }
            }
        };

        /** @brief Creates the build cache one component at a time, rejecting redirects before writing through them. */
        Result<std::filesystem::path> PrepareCacheRoot(const GameplayBuildRequest &request) {
            std::error_code error;
            auto current = std::filesystem::canonical(request.projectRoot, error);
            if (error)
                return Result<std::filesystem::path>::Failure(MakeError(StorageFailed));
            for (const char *component : {".horo", "local", "build", "gameplay-debug", "imported"}) {
                current /= component;
                const auto status = std::filesystem::symlink_status(current, error);
                if (error && error != std::errc::no_such_file_or_directory)
                    return Result<std::filesystem::path>::Failure(MakeError(StorageFailed));
                error.clear();
                if (std::filesystem::exists(status)) {
                    if (!std::filesystem::is_directory(status))
                        return Result<std::filesystem::path>::Failure(MakeError(StorageFailed));
                } else if (!std::filesystem::create_directory(current, error) || error) {
                    return Result<std::filesystem::path>::Failure(MakeError(StorageFailed));
                }
            }
            return Result<std::filesystem::path>::Success(std::move(current));
        }

        /** @brief Checks the size and digest of one declared cache file without trusting its directory name. */
        Result<void> VerifyCachedFile(const std::filesystem::path &path, const Packages::PackageFileEntry &entry) {
            std::error_code error;
            const auto size = std::filesystem::file_size(path, error);
            if (error || size != entry.size)
                return Result<void>::Failure(MakeError(StorageFailed));
            std::ifstream stream{path, std::ios::binary};
            std::vector<char> bytes(static_cast<std::size_t>(size));
            stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            if (!stream || static_cast<std::uintmax_t>(stream.gcount()) != size ||
                ComputeSha256(std::as_bytes(std::span{bytes})) != entry.digest)
                return Result<void>::Failure(MakeError(StorageFailed));
            return Result<void>::Success();
        }

        /** @brief Rejects redirects, special files and unknown inventory entries before reading cached bytes. */
        Result<void> VerifyCacheEntry(const std::filesystem::directory_entry &item, const Packages::GameplaySourceLibrary &library,
                                      const std::filesystem::path &root, CachedPaths &expected) {
            std::error_code error;
            const auto status = item.symlink_status(error);
            if (error || std::filesystem::is_symlink(status))
                return Result<void>::Failure(MakeError(StorageFailed));
            if (std::filesystem::is_directory(status))
                return Result<void>::Success();
            if (!std::filesystem::is_regular_file(status))
                return Result<void>::Failure(MakeError(StorageFailed));
            const auto relative = item.path().lexically_relative(root);
            if (expected.erase(relative) != 1U)
                return Result<void>::Failure(MakeError(StorageFailed));
            const auto entry = std::ranges::find(library.Files(), relative, [](const auto &file) {
                return GameplayLibraryPathFromUtf8(file.id.path.Value());
            });
            return VerifyCachedFile(item.path(), *entry);
        }

        /** @brief Rechecks every cached byte and bounds traversal through the complete declared inventory. */
        Result<void> VerifyCachedLibrary(const Packages::GameplaySourceLibrary &library, const std::filesystem::path &root,
                                         const CancellationToken &cancellation) {
            std::error_code error;
            if (!std::filesystem::is_directory(std::filesystem::symlink_status(root, error)) || error)
                return Result<void>::Failure(MakeError(StorageFailed));
            CachedPaths expected;
            for (const auto &entry : library.Files())
                expected.insert(GameplayLibraryPathFromUtf8(entry.id.path.Value()));
            std::filesystem::recursive_directory_iterator iterator{root, error};
            const std::filesystem::recursive_directory_iterator end;
            if (error)
                return Result<void>::Failure(MakeError(StorageFailed));
            std::size_t inspected{};
            while (iterator != end) {
                if (++inspected > library.Files().size() * 24U || iterator.depth() >= 24)
                    return Result<void>::Failure(MakeError(StorageFailed));
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(Cancelled));
                if (const auto valid = VerifyCacheEntry(*iterator, library, root, expected); valid.HasError())
                    return valid;
                iterator.increment(error);
                if (error)
                    return Result<void>::Failure(MakeError(StorageFailed));
            }
            return expected.empty() ? Result<void>::Success() : Result<void>::Failure(MakeError(StorageFailed));
        }

        /** @brief Publishes a complete private cache candidate only after every declared source/resource write succeeds. */
        Result<std::filesystem::path> CacheLibrary(const Packages::GameplaySourceLibrary &library, const std::filesystem::path &cache,
                                                   DurableFileSystem &files, const CancellationToken &cancellation) {
            const auto destination = cache / GameplayLibraryCacheKey(library);
            std::error_code error;
            const auto status = std::filesystem::symlink_status(destination, error);
            if (error && error != std::errc::no_such_file_or_directory)
                return Result<std::filesystem::path>::Failure(MakeError(StorageFailed));
            if (std::filesystem::exists(status)) {
                if (const auto valid = VerifyCachedLibrary(library, destination, cancellation); valid.HasError())
                    return Result<std::filesystem::path>::Failure(valid.ErrorValue());
                return Result<std::filesystem::path>::Success(destination);
            }
            auto created = Packages::Detail::CreatePrivatePackageRoot(cache, StorageFailed);
            if (created.HasError())
                return Result<std::filesystem::path>::Failure(created.ErrorValue());
            CandidateCleanup candidate{std::move(created).Value()};
            for (const auto &entry : library.Files()) {
                if (cancellation.IsCancellationRequested())
                    return Result<std::filesystem::path>::Failure(MakeError(Cancelled));
                const auto bytes = library.Package().archive->ReadDeclaredFile(entry.id.path, entry.size);
                if (bytes.HasError())
                    return Result<std::filesystem::path>::Failure(bytes.ErrorValue());
                const auto path = candidate.path / GameplayLibraryPathFromUtf8(entry.id.path.Value());
                std::filesystem::create_directories(path.parent_path(), error);
                if (error)
                    return Result<std::filesystem::path>::Failure(MakeError(StorageFailed));
                bool receipt{};
                if (const auto written =
                        files.WritePrivateDurable(path, bytes.Value(), receipt,
                                                  std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
                    written.HasError())
                    return Result<std::filesystem::path>::Failure(written.ErrorValue());
            }
            if (cancellation.IsCancellationRequested())
                return Result<std::filesystem::path>::Failure(MakeError(Cancelled));
            std::filesystem::rename(candidate.path, destination, error);
            if (error)
                return Result<std::filesystem::path>::Failure(MakeError(StorageFailed));
            candidate.path.clear();
            return Result<std::filesystem::path>::Success(destination);
        }
    }  // namespace

    /** @copydoc PrepareGameplayLibraries */
    Result<std::filesystem::path> PrepareGameplayLibraries(const GameplayBuildRequest &request, const std::filesystem::path &buildRoot,
                                                           DurableFileSystem &files, const CancellationToken &cancellation) {
        if (GameplayLibraries(request).empty())
            return Result<std::filesystem::path>::Success({});
        if (const auto identity = GameplayLibraryInputIdentity(request); identity.HasError())
            return Result<std::filesystem::path>::Failure(identity.ErrorValue());
        const auto cache = PrepareCacheRoot(request);
        if (cache.HasError())
            return Result<std::filesystem::path>::Failure(cache.ErrorValue());
        std::vector<const Packages::GameplaySourceLibrary *> ordered;
        for (const auto &library : GameplayLibraries(request))
            ordered.push_back(&library);
        std::ranges::sort(ordered, {}, [](const auto *library) {
            return FormatSha256(library->Fingerprint());
        });
        nlohmann::json libraries = nlohmann::json::array();
        for (const auto *library : ordered) {
            const auto staged = CacheLibrary(*library, cache.Value(), files, cancellation);
            if (staged.HasError())
                return Result<std::filesystem::path>::Failure(staged.ErrorValue());
            nlohmann::json sources = nlohmann::json::array();
            for (const auto &source : library->Sources())
                sources.push_back(GameplayLibraryPathToUtf8(staged.Value() / GameplayLibraryPathFromUtf8(source.Value())));
            libraries.push_back(
                {{"package", library->Package().lock.package.Value()},
                 {"version", library->Package().lock.version.ToString()},
                 {"artifactDigest", FormatSha256(library->Package().lock.artifactDigest)},
                 {"includeRoot", GameplayLibraryPathToUtf8(staged.Value() / GameplayLibraryPathFromUtf8(library->Root().Value()))},
                 {"sources", std::move(sources)}});
        }
        auto temporary = Packages::Detail::CreatePrivatePackageRoot(cache.Value(), StorageFailed);
        if (temporary.HasError())
            return Result<std::filesystem::path>::Failure(temporary.ErrorValue());
        const CandidateCleanup candidate{std::move(temporary).Value()};
        const auto pending = candidate.path / "manifest.json";
        const auto bytes =
            nlohmann::json{{"schemaVersion", 1}, {"preparationId", candidate.path.filename().string()}, {"libraries", std::move(libraries)}}
                .dump();
        bool receipt{};
        if (auto written = files.WritePrivateDurable(pending, std::as_bytes(std::span{bytes}), receipt); written.HasError())
            return Result<std::filesystem::path>::Failure(written.ErrorValue());
        const auto manifest = buildRoot / "gameplay_source_libraries.json";
        if (auto published = files.AtomicReplace(pending, manifest); published.HasError())
            return Result<std::filesystem::path>::Failure(published.ErrorValue());
        return Result<std::filesystem::path>::Success(manifest);
    }
}  // namespace Horo::Application::Detail
