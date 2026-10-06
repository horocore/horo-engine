#include "Horo/Assets/AssetArchive.h"

#include "../AssetErrors.h"
#include "Horo/Assets/AssetCook.h"

#include <algorithm>
#include <array>
#include <limits>
#include <ranges>
#include <utility>

namespace Horo::Assets {
    namespace {
        constexpr std::array<std::uint8_t, 8> Magic{'H', 'O', 'R', 'O', 'A', 'S', 'T', '1'};
        constexpr std::uint32_t FormatVersion = 1U;
        const ErrorDomainId Domain{"horo.asset"};
        const ErrorCodeDescriptor InvalidArchive{Domain, ErrorCode{"asset.archive.invalid"}, ErrorSeverity::Error,
                                                 "Release asset archive is invalid.", "Rebuild it from verified cooked artifacts."};
        const ErrorCodeDescriptor UnsupportedArchive{Domain, ErrorCode{"asset.archive.unsupported"}, ErrorSeverity::Error,
                                                     "Release asset archive format or feature is unsupported.",
                                                     "Use a compatible runtime or package profile."};
        const ErrorCodeDescriptor ArchiveTooLarge{Domain, ErrorCode{"asset.archive.too_large"}, ErrorSeverity::Error,
                                                  "Release asset archive exceeds its finite bounds.",
                                                  "Split the content into smaller archives or raise host policy."};
        const ErrorCodeDescriptor TargetMismatch{Domain, ErrorCode{"asset.archive.target_mismatch"}, ErrorSeverity::Error,
                                                 "Release asset archive targets another runtime.",
                                                 "Use an archive cooked for the selected target."};

        [[nodiscard]] Sha256Digest Digest(const std::span<const std::uint8_t> bytes) noexcept {
            return ComputeSha256({reinterpret_cast<const std::byte *>(bytes.data()), bytes.size()});
        }

        /** @brief Confirms the whole archive's detached trailer before parsing any entry. */
        [[nodiscard]] bool HasValidTrailer(const std::span<const std::uint8_t> bytes) noexcept {
            if (bytes.size() < 32U)
                return false;
            return std::ranges::equal(Digest(bytes.first(bytes.size() - 32U)).bytes, bytes.last(32U));
        }

        class Writer final {
        public:
            explicit Writer(const std::size_t maximum) : maximum_(maximum) {}

            [[nodiscard]] bool Bytes(const std::span<const std::uint8_t> bytes) {
                if (bytes.size() > maximum_ - data_.size())
                    return false;
                data_.insert(data_.end(), bytes.begin(), bytes.end());
                return true;
            }

            [[nodiscard]] bool U8(const std::uint8_t value) {
                return Bytes(std::span{&value, 1U});
            }

            [[nodiscard]] bool U16(const std::uint16_t value) {
                const std::array bytes{static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8U)};
                return Bytes(bytes);
            }

            [[nodiscard]] bool U32(const std::uint32_t value) {
                const std::array bytes{static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8U),
                                       static_cast<std::uint8_t>(value >> 16U), static_cast<std::uint8_t>(value >> 24U)};
                return Bytes(bytes);
            }

            [[nodiscard]] bool U64(const std::uint64_t value) {
                return U32(static_cast<std::uint32_t>(value)) && U32(static_cast<std::uint32_t>(value >> 32U));
            }

            [[nodiscard]] bool Text(const std::string_view value) {
                if (value.size() > std::numeric_limits<std::uint16_t>::max() || !U16(static_cast<std::uint16_t>(value.size())))
                    return false;
                return Bytes({reinterpret_cast<const std::uint8_t *>(value.data()), value.size()});
            }

            [[nodiscard]] std::span<const std::uint8_t> View() const noexcept {
                return data_;
            }

            [[nodiscard]] std::vector<std::uint8_t> Take() && {
                return std::move(data_);
            }

        private:
            std::size_t maximum_;
            std::vector<std::uint8_t> data_;
        };

        class Reader final {
        public:
            explicit Reader(const std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

            [[nodiscard]] bool Bytes(const std::size_t count, std::span<const std::uint8_t> &out) {
                if (count > bytes_.size() - offset_)
                    return false;
                out = bytes_.subspan(offset_, count);
                offset_ += count;
                return true;
            }

            [[nodiscard]] bool U8(std::uint8_t &out) {
                std::span<const std::uint8_t> bytes;
                if (!Bytes(1U, bytes))
                    return false;
                out = bytes.front();
                return true;
            }

            [[nodiscard]] bool U16(std::uint16_t &out) {
                std::span<const std::uint8_t> bytes;
                if (!Bytes(2U, bytes))
                    return false;
                out = static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[0]) | (static_cast<std::uint16_t>(bytes[1]) << 8U));
                return true;
            }

            [[nodiscard]] bool U32(std::uint32_t &out) {
                std::span<const std::uint8_t> bytes;
                if (!Bytes(4U, bytes))
                    return false;
                out = static_cast<std::uint32_t>(bytes[0]) | static_cast<std::uint32_t>(bytes[1]) << 8U |
                      static_cast<std::uint32_t>(bytes[2]) << 16U | static_cast<std::uint32_t>(bytes[3]) << 24U;
                return true;
            }

            [[nodiscard]] bool U64(std::uint64_t &out) {
                std::uint32_t low{};
                std::uint32_t high{};
                if (!U32(low) || !U32(high))
                    return false;
                out = static_cast<std::uint64_t>(low) | static_cast<std::uint64_t>(high) << 32U;
                return true;
            }

            [[nodiscard]] bool Text(std::string_view &out) {
                std::uint16_t size{};
                std::span<const std::uint8_t> bytes;
                if (!U16(size) || !Bytes(size, bytes))
                    return false;
                out = {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
                return true;
            }

            [[nodiscard]] std::size_t Offset() const noexcept {
                return offset_;
            }

            [[nodiscard]] bool Done() const noexcept {
                return offset_ == bytes_.size();
            }

        private:
            std::span<const std::uint8_t> bytes_;
            std::size_t offset_{};
        };

        [[nodiscard]] bool ValidLimits(const AssetArchiveLimits &limits) noexcept {
            return limits.maximumArchiveBytes >= Magic.size() + 4U + 32U && limits.maximumAssetBytes != 0U && limits.maximumAssets != 0U &&
                   limits.maximumChunks != 0U && limits.maximumChunks <= std::numeric_limits<std::uint32_t>::max() &&
                   limits.maximumAssets <= std::numeric_limits<std::uint32_t>::max();
        }

        [[nodiscard]] Result<std::vector<const AssetArchiveInput *>> SortInputs(const AssetChunkPlan &plan,
                                                                                const std::span<const AssetArchiveInput> artifacts,
                                                                                const AssetArchiveLimits &limits) {
            std::size_t expected{};
            for (const auto &chunk : plan.Chunks()) {
                if (chunk.assets.size() > limits.maximumAssets - expected)
                    return Result<std::vector<const AssetArchiveInput *>>::Failure(MakeError(ArchiveTooLarge));
                expected += chunk.assets.size();
            }
            if (expected != artifacts.size())
                return Result<std::vector<const AssetArchiveInput *>>::Failure(MakeError(InvalidArchive));
            std::vector<const AssetArchiveInput *> sorted;
            sorted.reserve(artifacts.size());
            for (const auto &artifact : artifacts) {
                if (!artifact.id.IsValid() || artifact.bytes.empty() || artifact.bytes.size() > limits.maximumAssetBytes)
                    return Result<std::vector<const AssetArchiveInput *>>::Failure(MakeError(InvalidArchive));
                sorted.push_back(&artifact);
            }
            std::ranges::sort(sorted, {}, [](const AssetArchiveInput *value) {
                return value->id;
            });
            if (std::ranges::adjacent_find(sorted, {}, [](const AssetArchiveInput *value) {
                return value->id;
            }) != sorted.end())
                return Result<std::vector<const AssetArchiveInput *>>::Failure(MakeError(InvalidArchive));
            return Result<std::vector<const AssetArchiveInput *>>::Success(std::move(sorted));
        }

        [[nodiscard]] bool WriteChunkHeader(Writer &writer, const AssetChunkDefinition &chunk) {
            if (chunk.dependencies.size() > 128U || chunk.assets.size() > std::numeric_limits<std::uint32_t>::max() ||
                !writer.Text(chunk.id.Value()) || !writer.U8(static_cast<std::uint8_t>(chunk.kind)) ||
                !writer.U32(static_cast<std::uint32_t>(chunk.mountPriority)) ||
                !writer.U16(static_cast<std::uint16_t>(chunk.dependencies.size())))
                return false;
            for (const auto &dependency : chunk.dependencies) {
                if (!writer.Text(dependency.Value()))
                    return false;
            }
            if (!writer.U8(chunk.requiredBaseManifest.has_value() ? 1U : 0U))
                return false;
            if (chunk.requiredBaseManifest && !writer.Bytes(chunk.requiredBaseManifest->bytes))
                return false;
            return writer.U32(static_cast<std::uint32_t>(chunk.assets.size()));
        }

        [[nodiscard]] Result<AssetChunkDefinition> ReadChunkHeader(Reader &reader) {
            std::string_view idText;
            std::uint8_t kindByte{};
            std::uint32_t priority{};
            std::uint16_t dependencyCount{};
            if (!reader.Text(idText) || !reader.U8(kindByte) || !reader.U32(priority) || !reader.U16(dependencyCount) ||
                dependencyCount > 128U)
                return Result<AssetChunkDefinition>::Failure(MakeError(InvalidArchive));
            auto id = AssetChunkId::Parse(idText);
            if (id.HasError())
                return Result<AssetChunkDefinition>::Failure(MakeError(InvalidArchive));
            AssetChunkDefinition chunk{.id = std::move(id).Value(),
                                       .kind = static_cast<AssetChunkKind>(kindByte),
                                       .mountPriority = static_cast<std::int32_t>(priority)};
            chunk.dependencies.reserve(dependencyCount);
            for (std::uint16_t index = 0; index < dependencyCount; ++index) {
                std::string_view dependencyText;
                if (!reader.Text(dependencyText))
                    return Result<AssetChunkDefinition>::Failure(MakeError(InvalidArchive));
                auto dependency = AssetChunkId::Parse(dependencyText);
                if (dependency.HasError())
                    return Result<AssetChunkDefinition>::Failure(MakeError(InvalidArchive));
                chunk.dependencies.push_back(std::move(dependency).Value());
            }
            std::uint8_t hasBase{};
            if (!reader.U8(hasBase) || hasBase > 1U)
                return Result<AssetChunkDefinition>::Failure(MakeError(InvalidArchive));
            if (hasBase == 1U) {
                std::span<const std::uint8_t> digestBytes;
                if (!reader.Bytes(32U, digestBytes))
                    return Result<AssetChunkDefinition>::Failure(MakeError(InvalidArchive));
                Sha256Digest digest;
                std::ranges::copy(digestBytes, digest.bytes.begin());
                chunk.requiredBaseManifest = digest;
            }
            return Result<AssetChunkDefinition>::Success(std::move(chunk));
        }

        struct ParsedArchiveAsset {
            AssetId id;
            std::size_t offset{};
            std::size_t size{};
        };

        /** @brief Reads and verifies one complete archived cooked envelope. */
        [[nodiscard]] Result<ParsedArchiveAsset> ReadArchiveAsset(Reader &reader, const AssetCookTargetId &expectedTarget,
                                                                  const AssetArchiveLimits &limits) {
            std::span<const std::uint8_t> idBytes;
            std::string_view typeText;
            std::uint64_t byteCount{};
            std::span<const std::uint8_t> expectedDigest;
            if (!reader.Bytes(16U, idBytes) || !reader.Text(typeText) || !reader.U64(byteCount) || !reader.Bytes(32U, expectedDigest) ||
                byteCount == 0U || byteCount > limits.maximumAssetBytes)
                return Result<ParsedArchiveAsset>::Failure(MakeError(InvalidArchive));
            std::array<std::uint8_t, 16> idArray;
            std::ranges::copy(idBytes, idArray.begin());
            const AssetId id = AssetId::FromBytes(idArray);
            auto type = AssetTypeId::Parse(typeText);
            if (!id.IsValid() || type.HasError())
                return Result<ParsedArchiveAsset>::Failure(MakeError(InvalidArchive));
            const std::size_t offset = reader.Offset();
            std::span<const std::uint8_t> cookedBytes;
            if (!reader.Bytes(static_cast<std::size_t>(byteCount), cookedBytes) ||
                !std::ranges::equal(Digest(cookedBytes).bytes, expectedDigest))
                return Result<ParsedArchiveAsset>::Failure(MakeError(InvalidArchive));
            if (auto decoded = DecodeCookedArtifact(cookedBytes, {.maximumArtifactBytes = limits.maximumAssetBytes});
                decoded.HasError() || decoded.Value().id != id || decoded.Value().type != type.Value() ||
                decoded.Value().target != expectedTarget)
                return Result<ParsedArchiveAsset>::Failure(MakeError(InvalidArchive));
            return Result<ParsedArchiveAsset>::Success({id, offset, static_cast<std::size_t>(byteCount)});
        }

        struct ParsedArchiveContents final {
            std::vector<AssetChunkDefinition> chunks;
            std::vector<ParsedArchiveAsset> assets;
        };

        /** @brief Parses bounded chunk membership and every cooked payload before provider publication. */
        [[nodiscard]] Result<ParsedArchiveContents> ReadArchiveChunks(Reader &reader, const std::uint32_t count,
                                                                      const AssetCookTargetId &expectedTarget,
                                                                      const AssetArchiveLimits &limits) {
            ParsedArchiveContents parsed;
            parsed.chunks.reserve(count);
            for (std::uint32_t chunkIndex = 0; chunkIndex < count; ++chunkIndex) {
                auto header = ReadChunkHeader(reader);
                if (header.HasError())
                    return Result<ParsedArchiveContents>::Failure(std::move(header).ErrorValue());
                auto chunk = std::move(header).Value();
                std::uint32_t assets{};
                if (!reader.U32(assets) || assets == 0U || assets > limits.maximumAssets - parsed.assets.size())
                    return Result<ParsedArchiveContents>::Failure(MakeError(ArchiveTooLarge));
                chunk.assets.reserve(assets);
                for (std::uint32_t index = 0; index < assets; ++index) {
                    auto asset = ReadArchiveAsset(reader, expectedTarget, limits);
                    if (asset.HasError())
                        return Result<ParsedArchiveContents>::Failure(std::move(asset).ErrorValue());
                    chunk.assets.push_back(asset.Value().id);
                    parsed.assets.push_back(std::move(asset).Value());
                }
                parsed.chunks.emplace_back(std::move(chunk));
            }
            return Result<ParsedArchiveContents>::Success(std::move(parsed));
        }

        /** @brief Compares every authored chunk field against the authenticated release plan. */
        [[nodiscard]] bool SameChunk(const AssetChunkDefinition &actual, const AssetChunkDefinition &expected) {
            return actual.id == expected.id && actual.kind == expected.kind && actual.assets == expected.assets &&
                   actual.dependencies == expected.dependencies && actual.mountPriority == expected.mountPriority &&
                   actual.requiredBaseManifest == expected.requiredBaseManifest;
        }
    }  // namespace

    /** @copydoc BuildAssetArchive */
    Result<std::vector<std::uint8_t>> BuildAssetArchive(const AssetChunkPlan &plan, const AssetCookGeneration &generation,
                                                        const AssetArchiveLimits &limits) {
        const AssetCookLimits cookCeilings;
        const AssetCookLimits cookLimits{.maximumArtifactBytes = std::min(limits.maximumAssetBytes, cookCeilings.maximumArtifactBytes),
                                         .maximumAssets = std::min(limits.maximumAssets, cookCeilings.maximumAssets)};
        auto contents = ReadCookGenerationContents(generation, limits.maximumArchiveBytes, cookLimits);
        if (contents.HasError())
            return Result<std::vector<std::uint8_t>>::Failure(contents.ErrorValue());
        auto generationContents = std::move(contents).Value();
        std::vector<AssetArchiveInput> inputs;
        inputs.reserve(generationContents.entries.size());
        for (std::size_t i = 0; i < generationContents.entries.size(); ++i)
            inputs.emplace_back(generationContents.entries[i].assetId, std::move(generationContents.artifacts[i]));
        return BuildAssetArchive(plan, generation.target, inputs, limits);
    }

    Result<std::vector<std::uint8_t>> BuildAssetArchive(const AssetChunkPlan &plan, const AssetCookTargetId &target,
                                                        const std::span<const AssetArchiveInput> artifacts,
                                                        const AssetArchiveLimits &limits) {
        if (!ValidLimits(limits) || !target.IsValid() || plan.Chunks().empty() || plan.Chunks().size() > limits.maximumChunks)
            return Result<std::vector<std::uint8_t>>::Failure(MakeError(InvalidArchive));
        auto inputs = SortInputs(plan, artifacts, limits);
        if (inputs.HasError())
            return Result<std::vector<std::uint8_t>>::Failure(std::move(inputs).ErrorValue());

        Writer writer(limits.maximumArchiveBytes - 32U);
        if (!writer.Bytes(Magic) || !writer.U32(FormatVersion) || !writer.U32(0U) || !writer.Text(target.Value()) ||
            !writer.U32(static_cast<std::uint32_t>(plan.Chunks().size())))
            return Result<std::vector<std::uint8_t>>::Failure(MakeError(ArchiveTooLarge));

        const auto &sorted = inputs.Value();
        for (const auto &chunk : plan.Chunks()) {
            if (!WriteChunkHeader(writer, chunk))
                return Result<std::vector<std::uint8_t>>::Failure(MakeError(ArchiveTooLarge));
            for (const auto &id : chunk.assets) {
                const auto found = std::ranges::lower_bound(sorted, id, {}, [](const AssetArchiveInput *value) {
                    return value->id;
                });
                if (found == sorted.end() || (*found)->id != id)
                    return Result<std::vector<std::uint8_t>>::Failure(MakeError(InvalidArchive));
                const AssetArchiveInput &input = **found;
                auto decoded = DecodeCookedArtifact(input.bytes, {.maximumArtifactBytes = limits.maximumAssetBytes});
                if (decoded.HasError() || decoded.Value().id != id || decoded.Value().target != target)
                    return Result<std::vector<std::uint8_t>>::Failure(MakeError(InvalidArchive));
                const auto digest = Digest(input.bytes);
                if (!writer.Bytes(id.Bytes()) || !writer.Text(decoded.Value().type.Value()) || !writer.U64(input.bytes.size()) ||
                    !writer.Bytes(digest.bytes) || !writer.Bytes(input.bytes))
                    return Result<std::vector<std::uint8_t>>::Failure(MakeError(ArchiveTooLarge));
            }
        }
        const auto digest = Digest(writer.View());
        auto result = std::move(writer).Take();
        result.insert(result.end(), digest.bytes.begin(), digest.bytes.end());
        return Result<std::vector<std::uint8_t>>::Success(std::move(result));
    }

    AssetArchiveProvider::AssetArchiveProvider(std::vector<std::uint8_t> bytes, std::vector<Entry> entries,
                                               std::vector<AssetChunkDefinition> chunks, AssetCookTargetId target)
        : bytes_(std::move(bytes)), entries_(std::move(entries)), chunks_(std::move(chunks)), target_(std::move(target)),
          archiveDigest_(Digest(bytes_)) {}

    /** @copydoc AssetArchiveProvider::Open */
    Result<AssetArchiveProvider> AssetArchiveProvider::Open(const std::span<const std::uint8_t> bytes,
                                                            const AssetCookTargetId &expectedTarget, const AssetArchiveLimits &limits) {
        auto opened = OpenParsed(bytes, expectedTarget, limits);
        if (opened.HasError())
            return opened;
        auto provider = std::move(opened).Value();
        return Result<AssetArchiveProvider>::Success(std::move(provider));
    }

    /** @copydoc AssetArchiveProvider::OpenParsed */
    Result<AssetArchiveProvider> AssetArchiveProvider::OpenParsed(const std::span<const std::uint8_t> bytes,
                                                                  const AssetCookTargetId &expectedTarget,
                                                                  const AssetArchiveLimits &limits) {
        if (!ValidLimits(limits) || !expectedTarget.IsValid() || bytes.size() > limits.maximumArchiveBytes ||
            bytes.size() < Magic.size() + 4U + 32U)
            return Result<AssetArchiveProvider>::Failure(MakeError(InvalidArchive));
        if (!HasValidTrailer(bytes))
            return Result<AssetArchiveProvider>::Failure(MakeError(InvalidArchive));
        Reader reader(bytes.first(bytes.size() - 32U));
        std::span<const std::uint8_t> magic;
        std::uint32_t version{};
        std::uint32_t featureFlags{};
        std::string_view targetText;
        std::uint32_t chunkCount{};
        if (!reader.Bytes(Magic.size(), magic) || !std::ranges::equal(magic, Magic) || !reader.U32(version) || !reader.U32(featureFlags) ||
            !reader.Text(targetText) || !reader.U32(chunkCount))
            return Result<AssetArchiveProvider>::Failure(MakeError(InvalidArchive));
        if (version != FormatVersion || featureFlags != 0U)
            return Result<AssetArchiveProvider>::Failure(MakeError(UnsupportedArchive));
        if (targetText != expectedTarget.Value())
            return Result<AssetArchiveProvider>::Failure(MakeError(TargetMismatch));
        if (chunkCount == 0U || chunkCount > limits.maximumChunks)
            return Result<AssetArchiveProvider>::Failure(MakeError(ArchiveTooLarge));

        auto parsed = ReadArchiveChunks(reader, chunkCount, expectedTarget, limits);
        if (parsed.HasError())
            return Result<AssetArchiveProvider>::Failure(parsed.ErrorValue());
        if (!reader.Done())
            return Result<AssetArchiveProvider>::Failure(MakeError(InvalidArchive));
        auto plan =
            AssetChunkPlan::Create(parsed.Value().chunks, {.maximumChunks = limits.maximumChunks, .maximumAssets = limits.maximumAssets});
        if (plan.HasError())
            return Result<AssetArchiveProvider>::Failure(MakeError(InvalidArchive));
        std::vector<Entry> entries;
        entries.reserve(parsed.Value().assets.size());
        for (const auto &asset : parsed.Value().assets)
            entries.emplace_back(asset.id, asset.offset, asset.size);
        std::ranges::sort(entries, {}, &Entry::id);
        return Result<AssetArchiveProvider>::Success(
            AssetArchiveProvider{std::vector<std::uint8_t>(bytes.begin(), bytes.end()), std::move(entries),
                                 std::vector<AssetChunkDefinition>{plan.Value().Chunks().begin(), plan.Value().Chunks().end()},
                                 expectedTarget});
    }

    /** @copydoc AssetArchiveProvider::OpenSelected */
    Result<AssetArchiveProvider> AssetArchiveProvider::OpenSelected(const std::span<const std::uint8_t> bytes,
                                                                    const AssetCookTargetId &expectedTarget,
                                                                    const AssetChunkPlan &expectedPlan,
                                                                    const std::span<const AssetChunkId> selected,
                                                                    const Sha256Digest &baseManifest, const AssetArchiveLimits &limits) {
        auto opened = OpenParsed(bytes, expectedTarget, limits);
        if (opened.HasError())
            return opened;
        auto provider = std::move(opened).Value();
        const auto authenticated = expectedPlan.Chunks();
        if (provider.chunks_.size() != authenticated.size() || !std::ranges::equal(provider.chunks_, authenticated, SameChunk))
            return Result<AssetArchiveProvider>::Failure(MakeError(InvalidArchive));
        auto order = ResolveAssetChunkMountOrder(expectedPlan, selected, baseManifest);
        if (order.HasError())
            return Result<AssetArchiveProvider>::Failure(order.ErrorValue());

        std::vector<AssetId> visible;
        for (const auto &chunk : authenticated)
            if (std::ranges::find(order.Value(), chunk.id) != order.Value().end())
                visible.insert(visible.end(), chunk.assets.begin(), chunk.assets.end());
        std::ranges::sort(visible);
        std::erase_if(provider.entries_, [&visible](const Entry &entry) {
            return !std::ranges::binary_search(visible, entry.id);
        });
        std::erase_if(provider.chunks_, [&order](const AssetChunkDefinition &chunk) {
            return std::ranges::find(order.Value(), chunk.id) == order.Value().end();
        });
        return Result<AssetArchiveProvider>::Success(std::move(provider));
    }

    /** @copydoc AssetArchiveProvider::StoredByteLength */
    std::optional<std::size_t> AssetArchiveProvider::StoredByteLength(const AssetId id) const noexcept {
        if (const auto found = std::ranges::lower_bound(entries_, id, {}, &Entry::id); found != entries_.end() && found->id == id)
            return found->size;
        return std::nullopt;
    }

    /** @copydoc AssetArchiveProvider::MountedChunks */
    std::span<const AssetChunkDefinition> AssetArchiveProvider::MountedChunks() const noexcept {
        return chunks_;
    }

    /** @copydoc AssetArchiveProvider::Target */
    const AssetCookTargetId &AssetArchiveProvider::Target() const noexcept {
        return target_;
    }

    /** @copydoc AssetArchiveProvider::ArchiveDigest */
    const Sha256Digest &AssetArchiveProvider::ArchiveDigest() const noexcept {
        return archiveDigest_;
    }

    Result<bool> AssetArchiveProvider::Exists(const AssetId id, const CancellationToken &cancellation) const {
        if (cancellation.IsCancellationRequested())
            return Result<bool>::Failure(MakeError(AssetErrors::LoadCancelled));
        if (!id.IsValid())
            return Result<bool>::Failure(MakeError(AssetErrors::IdentityInvalid));
        return Result<bool>::Success(std::ranges::binary_search(entries_, id, {}, &Entry::id));
    }

    Result<std::vector<std::uint8_t>> AssetArchiveProvider::Load(const AssetId id, const CancellationToken &cancellation) const {
        auto exists = Exists(id, cancellation);
        if (exists.HasError())
            return Result<std::vector<std::uint8_t>>::Failure(std::move(exists).ErrorValue());
        if (!exists.Value())
            return Result<std::vector<std::uint8_t>>::Failure(MakeError(AssetErrors::ProviderNotFound));
        const auto found = std::ranges::lower_bound(entries_, id, {}, &Entry::id);
        std::vector<std::uint8_t> loaded(found->size);
        constexpr std::size_t BlockBytes = 64U * 1024U;
        for (std::size_t offset = 0; offset < found->size; offset += std::min(BlockBytes, found->size - offset)) {
            if (cancellation.IsCancellationRequested())
                return Result<std::vector<std::uint8_t>>::Failure(MakeError(AssetErrors::LoadCancelled));
            const std::size_t count = std::min(BlockBytes, found->size - offset);
            std::copy_n(bytes_.data() + found->offset + offset, count, loaded.data() + offset);
        }
        return Result<std::vector<std::uint8_t>>::Success(std::move(loaded));
    }
}  // namespace Horo::Assets
