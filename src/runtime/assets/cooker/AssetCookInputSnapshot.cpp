#include "Horo/Assets/AssetCookInputSnapshot.h"

#include "../AssetErrors.h"

#include <algorithm>
#include <fstream>
#include <limits>

namespace Horo::Assets {
    namespace {
        constexpr std::uint64_t MaximumSidecarBytes = 1024 * 1024;

        /** @brief Rejects escaping and symlinked path components before a bounded host read. */
        Result<std::filesystem::path> CheckedPath(const std::filesystem::path &root, const std::string_view relative) {
            const std::filesystem::path path{relative};
            if (path.empty() || path.is_absolute() || path != path.lexically_normal())
                return Result<std::filesystem::path>::Failure(MakeError(CookErrors::SourceReadFailed));
            auto current = root;
            for (const auto &component : path) {
                if (component == ".." || component == ".")
                    return Result<std::filesystem::path>::Failure(MakeError(CookErrors::SourceReadFailed));
                current /= component;
                std::error_code error;
                const auto status = std::filesystem::symlink_status(current, error);
                if (error || std::filesystem::is_symlink(status))
                    return Result<std::filesystem::path>::Failure(MakeError(CookErrors::SourceReadFailed));
            }
            return Result<std::filesystem::path>::Success(std::move(current));
        }

        /** @brief Reads one regular file only after checking both the file and remaining aggregate budget. */
        Result<std::vector<std::uint8_t>> Read(const std::filesystem::path &root, const std::string_view relative,
                                               const std::uint64_t maximumBytes, const CancellationToken &cancel) {
            if (cancel.IsCancellationRequested())
                return Result<std::vector<std::uint8_t>>::Failure(MakeError(CookErrors::Cancelled));
            auto checked = CheckedPath(root, relative);
            if (checked.HasError())
                return Result<std::vector<std::uint8_t>>::Failure(checked.ErrorValue());
            std::error_code error;
            if (!std::filesystem::is_regular_file(checked.Value(), error) || error)
                return Result<std::vector<std::uint8_t>>::Failure(MakeError(CookErrors::SourceReadFailed));
            const auto count = std::filesystem::file_size(checked.Value(), error);
            if (error)
                return Result<std::vector<std::uint8_t>>::Failure(MakeError(CookErrors::SourceReadFailed));
            if (count > maximumBytes || count > std::numeric_limits<std::size_t>::max() ||
                count > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()))
                return Result<std::vector<std::uint8_t>>::Failure(MakeError(CookErrors::TooLarge));
            std::ifstream file{checked.Value(), std::ios::binary};
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count));
            file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(count));
            if (!file || file.peek() != std::char_traits<char>::eof())
                return Result<std::vector<std::uint8_t>>::Failure(MakeError(CookErrors::SourceReadFailed));
            return Result<std::vector<std::uint8_t>>::Success(std::move(bytes));
        }

        /** @brief Serializes only identity schema fields accepted by the registry, with fixed field boundaries. */
        Sha256Digest MetadataDigest(const AssetRecord &record) {
            const std::string canonical =
                "horo.asset.identity-metadata.v1\n" + record.id.ToString() + "\n" + std::string{record.type.Value()} + "\n";
            return ComputeSha256(std::as_bytes(std::span{canonical}));
        }
    }  // namespace

    struct AssetCookInputSnapshot::State final {
        std::filesystem::path root;
        AssetRegistrySnapshot registry;
        std::vector<AssetCookPinnedSource> sources;
        std::vector<Sha256Digest> sidecarDigests;
    };

    /** @copydoc AssetCookInputSnapshot::Capture */
    Result<AssetCookInputSnapshot> AssetCookInputSnapshot::Capture(const std::filesystem::path &sourceRoot, AssetRegistrySnapshot registry,
                                                                   const AssetCookLimits &limits, const std::uint64_t maximumCapturedBytes,
                                                                   const CancellationToken &cancellation) {
        if (std::error_code error; !sourceRoot.is_absolute() || std::filesystem::is_symlink(sourceRoot, error) || error)
            return Result<AssetCookInputSnapshot>::Failure(MakeError(CookErrors::SourceReadFailed));
        if (std::error_code error; !std::filesystem::is_directory(sourceRoot, error) || error || limits.maximumSourceBytes == 0 ||
                                   limits.maximumAssets == 0 || maximumCapturedBytes == 0 ||
                                   registry.Records().size() > limits.maximumAssets)
            return Result<AssetCookInputSnapshot>::Failure(MakeError(CookErrors::SourceReadFailed));
        auto state = std::make_shared<State>();
        state->root = sourceRoot;
        state->registry = std::move(registry);
        state->sources.reserve(state->registry.Records().size());
        state->sidecarDigests.reserve(state->registry.Records().size());
        std::uint64_t retained{};
        for (const auto &record : state->registry.Records()) {
            auto bytes = Read(sourceRoot, record.sourcePath.String(),
                              std::min<std::uint64_t>(limits.maximumSourceBytes, maximumCapturedBytes - retained), cancellation);
            if (bytes.HasError())
                return Result<AssetCookInputSnapshot>::Failure(bytes.ErrorValue());
            retained += bytes.Value().size();
            auto sidecar = Read(sourceRoot, record.metadataPath.String(), std::min(MaximumSidecarBytes, maximumCapturedBytes - retained),
                                cancellation);
            if (sidecar.HasError())
                return Result<AssetCookInputSnapshot>::Failure(sidecar.ErrorValue());
            retained += sidecar.Value().size();
            const std::string_view metadata{reinterpret_cast<const char *>(sidecar.Value().data()), sidecar.Value().size()};
            const auto decoded = DecodeAssetIdentitySidecar(record.sourcePath.String(), metadata);
            if (decoded.HasError())
                return Result<AssetCookInputSnapshot>::Failure(decoded.ErrorValue());
            if (decoded.Value().id != record.id || decoded.Value().type != record.type ||
                decoded.Value().metadataPath.String() != record.metadataPath.String())
                return Result<AssetCookInputSnapshot>::Failure(MakeError(CookErrors::MalformedArtifact));
            const auto digest = ComputeSha256(std::as_bytes(std::span{bytes.Value()}));
            state->sidecarDigests.push_back(ComputeSha256(std::as_bytes(std::span{sidecar.Value()})));
            state->sources.emplace_back(record, std::move(bytes).Value(), digest, MetadataDigest(record));
        }
        AssetCookInputSnapshot snapshot{std::move(state)};
        if (auto unchanged = snapshot.VerifyUnchanged(cancellation); unchanged.HasError())
            return Result<AssetCookInputSnapshot>::Failure(unchanged.ErrorValue());
        return Result<AssetCookInputSnapshot>::Success(std::move(snapshot));
    }

    /** @copydoc AssetCookInputSnapshot::Registry */
    const AssetRegistrySnapshot &AssetCookInputSnapshot::Registry() const noexcept {
        return state_->registry;
    }

    /** @copydoc AssetCookInputSnapshot::SourceRoot */
    const std::filesystem::path &AssetCookInputSnapshot::SourceRoot() const noexcept {
        return state_->root;
    }

    /** @copydoc AssetCookInputSnapshot::ClosureDigest */
    Sha256Digest AssetCookInputSnapshot::ClosureDigest() const {
        // Asset IDs have fixed width and types cannot contain a newline. Fixed-size hex digests
        // and explicit line boundaries make this schema unambiguous, independent of registry order.
        std::string canonical{"horo.asset.source-closure.v1\n"};
        for (const auto &source : state_->sources) {
            canonical += source.record.id.ToString() + "\n" + std::string{source.record.type.Value()} + "\n";
            for (const auto &digest : {source.sourceDigest, source.metadataDigest}) {
                constexpr std::string_view hex = "0123456789abcdef";
                for (const std::byte byte : std::as_bytes(std::span{digest.bytes})) {
                    canonical += hex[std::to_integer<std::size_t>(byte >> 4)];
                    canonical += hex[std::to_integer<std::size_t>(byte & std::byte{15})];
                }
                canonical += '\n';
            }
        }
        return ComputeSha256(std::as_bytes(std::span{canonical}));
    }

    /** @copydoc AssetCookInputSnapshot::Sources */
    std::span<const AssetCookPinnedSource> AssetCookInputSnapshot::Sources() const noexcept {
        return state_->sources;
    }

    /** @copydoc AssetCookInputSnapshot::Find */
    const AssetCookPinnedSource *AssetCookInputSnapshot::Find(const AssetId id) const noexcept {
        const auto found = std::ranges::lower_bound(state_->sources, id, {}, [](const auto &source) {
            return source.record.id;
        });
        return found == state_->sources.end() || found->record.id != id ? nullptr : std::to_address(found);
    }

    /** @copydoc AssetCookInputSnapshot::VerifyUnchanged */
    Result<void> AssetCookInputSnapshot::VerifyUnchanged(const CancellationToken &cancellation) const {
        for (std::size_t index = 0; index < state_->sources.size(); ++index) {
            const auto &source = state_->sources[index];
            auto bytes = Read(state_->root, source.record.sourcePath.String(), source.bytes.size(), cancellation);
            if (bytes.HasError())
                return Result<void>::Failure(bytes.ErrorValue());
            auto metadata = Read(state_->root, source.record.metadataPath.String(), MaximumSidecarBytes, cancellation);
            if (metadata.HasError())
                return Result<void>::Failure(metadata.ErrorValue());
            if (ComputeSha256(std::as_bytes(std::span{bytes.Value()})) != source.sourceDigest ||
                ComputeSha256(std::as_bytes(std::span{metadata.Value()})) != state_->sidecarDigests[index])
                return Result<void>::Failure(MakeError(CookErrors::HashMismatch));
        }
        if (cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(CookErrors::Cancelled));
        return Result<void>::Success();
    }
}  // namespace Horo::Assets
