#include "SaveThumbnailCodec.h"

#include "Horo/Runtime/Save/SaveErrors.h"

#include <algorithm>
#include <array>
#include <utility>

namespace Horo::Runtime::SaveThumbnailDetail {
    namespace {
        constexpr std::array kMagic{std::byte{'H'}, std::byte{'S'}, std::byte{'T'}, std::byte{'H'},
                                    std::byte{'M'}, std::byte{'B'}, std::byte{'1'}, std::byte{0}};
        constexpr std::size_t kMetadataBytes = 104;

        /** @brief Stores explicit little-endian integers in already-sized metadata. */
        void Put(std::vector<std::byte> &bytes, const std::size_t offset, const std::uint64_t value, const std::size_t width) {
            for (std::size_t i = 0; i < width; ++i)
                bytes[offset + i] = static_cast<std::byte>((value >> (i * 8)) & 255U);
        }

        /** @brief Reads explicit little-endian integers after fixed-size admission. */
        [[nodiscard]] std::uint64_t Get(const std::span<const std::byte> bytes, const std::size_t offset, const std::size_t width) {
            std::uint64_t value{};
            for (std::size_t i = 0; i < width; ++i)
                value |= std::to_integer<std::uint64_t>(bytes[offset + i]) << (i * 8);
            return value;
        }

        /** @brief Copies canonical opaque identity bytes into fixed metadata. */
        void PutIdentity(std::vector<std::byte> &bytes, const std::size_t offset, const SaveIdentityDetail::Bytes &identity) {
            std::ranges::transform(identity, bytes.begin() + offset, [](const std::uint8_t byte) {
                return static_cast<std::byte>(byte);
            });
        }

        /** @brief Matches canonical expected identities without parsing or allocation. */
        [[nodiscard]] bool Matches(const std::span<const std::byte> bytes, const std::size_t offset,
                                   const SaveIdentityDetail::Bytes &identity) {
            return std::ranges::equal(bytes.subspan(offset, identity.size()), identity, {}, [](const std::byte byte) {
                return std::to_integer<std::uint8_t>(byte);
            });
        }
    }  // namespace

    /** @copydoc Encode */
    std::vector<std::byte> Encode(const SaveThumbnailArtifact &artifact) {
        std::vector<std::byte> bytes(kMetadataBytes);
        std::ranges::copy(kMagic, bytes.begin());
        const auto &request = artifact.Request();
        PutIdentity(bytes, 8, request.slot.Bytes());
        PutIdentity(bytes, 24, request.generation.Bytes());
        PutIdentity(bytes, 40, request.thumbnail.Bytes());
        const auto source = artifact.Source();
        Put(bytes, 56, source.runtime, 8);
        Put(bytes, 64, source.scene, 8);
        Put(bytes, 72, source.view, 8);
        Put(bytes, 80, source.frame, 8);
        Put(bytes, 88, request.width, 4);
        Put(bytes, 92, request.height, 4);
        Put(bytes, 96, artifact.Bytes().size(), 8);
        return bytes;
    }

    /** @copydoc Decode */
    Result<std::shared_ptr<const SaveThumbnailArtifact>> Decode(const std::span<const std::byte> metadata, std::vector<std::byte> image,
                                                                const SaveSlotPublicationMetadata &publication,
                                                                const SaveThumbnailLimits &limits) {
        using Return = Result<std::shared_ptr<const SaveThumbnailArtifact>>;
        if (metadata.size() != kMetadataBytes || !std::ranges::equal(metadata.first(kMagic.size()), kMagic) || !publication.thumbnail)
            return Return::Failure(MakeError(SaveErrors::ThumbnailInvalid));
        if (!Matches(metadata, 8, publication.slot.Bytes()) || !Matches(metadata, 24, publication.generation.Bytes()) ||
            !Matches(metadata, 40, publication.thumbnail->Bytes()))
            return Return::Failure(MakeError(SaveErrors::ThumbnailStale));
        if (Get(metadata, 96, 8) != image.size())
            return Return::Failure(MakeError(SaveErrors::ThumbnailInvalid));
        SaveThumbnailRequest request{.slot = publication.slot,
                                     .generation = publication.generation,
                                     .thumbnail = *publication.thumbnail,
                                     .source = {.runtime = Get(metadata, 56, 8),
                                                .scene = Get(metadata, 64, 8),
                                                .view = Get(metadata, 72, 8),
                                                .frame = Get(metadata, 80, 8)},
                                     .width = static_cast<std::uint32_t>(Get(metadata, 88, 4)),
                                     .height = static_cast<std::uint32_t>(Get(metadata, 92, 4)),
                                     .limits = limits};
        request.limits.timeout = std::chrono::milliseconds{1};
        SaveThumbnailCapture capture;
        const auto serial = capture.Request(request, SaveThumbnailAvailability::Available, {});
        if (serial.HasError())
            return Return::Failure(serial.ErrorValue());
        if (const auto completed = capture.Complete({.requestSerial = serial.Value(),
                                                     .slot = request.slot,
                                                     .generation = request.generation,
                                                     .thumbnail = request.thumbnail,
                                                     .source = request.source,
                                                     .width = request.width,
                                                     .height = request.height,
                                                     .encoded = std::move(image)},
                                                    {}, request.source);
            completed.HasError())
            return Return::Failure(completed.ErrorValue());
        if (capture.Snapshot().state != SaveThumbnailCaptureState::Captured)
            return Return::Failure(capture.Snapshot().error.value_or(MakeError(SaveErrors::ThumbnailInvalid)));
        return Return::Success(capture.Snapshot().artifact);
    }
}  // namespace Horo::Runtime::SaveThumbnailDetail
