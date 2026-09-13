#pragma once

#include "Horo/Runtime/Render/Mesh.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/Texture.h"
#include "RenderResourceRegistry.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <vector>

namespace Horo::Render::Detail {
    class RenderResourceUploadQueue final {
    public:
        enum class RequestKind : std::uint8_t {
            Buffer,
            Mesh,
            Texture,
            TextureView,
            RenderTarget,
        };

        struct Request {
            RequestKind kind{RequestKind::Buffer};
            RenderResourceIdentity identity;
            RenderBufferDescriptor buffer;
            RenderMeshDescriptor mesh;
            RenderTextureDescriptor texture;
            RenderTextureViewDescriptor textureView;
            RenderTargetDescriptor renderTarget;
            std::size_t stagingOffset{0};
            std::size_t stagingByteCount{0};
            RenderMemoryReservationId memoryReservation;
            RenderMemoryPlacement memoryPlacement;
            std::optional<RenderResourceIdentity> replacedMesh;
        };

        explicit RenderResourceUploadQueue(const RenderResourceUploadLimits limits) : limits_(limits) {
            requests_.reserve(limits.maximumPendingRequests);
            stagingStorage_.reserve(limits.maximumPendingBytes);
        }

        [[nodiscard]] bool CanEnqueue(const std::size_t byteCount) const noexcept {
            if (!acceptingRequests_ || PendingRequestCount() >= limits_.maximumPendingRequests)
                return false;
            if (byteCount == 0)
                return true;
            const auto offset = AlignedOffset(occupiedStagingBytes_);
            return offset.has_value() && *offset <= limits_.maximumPendingBytes && byteCount <= limits_.maximumPendingBytes - *offset;
        }

        void EnqueueBuffer(const RenderResourceIdentity identity, const RenderBufferDescriptor &descriptor,
                           const std::span<const std::byte> initialData, const RenderMemoryReservationId memoryReservation,
                           const RenderMemoryPlacement &memoryPlacement) {
            Request request{.kind = RequestKind::Buffer,
                            .identity = identity,
                            .buffer = descriptor,
                            .stagingOffset = initialData.empty() ? 0 : *AlignedOffset(occupiedStagingBytes_),
                            .stagingByteCount = initialData.size(),
                            .memoryReservation = memoryReservation,
                            .memoryPlacement = memoryPlacement};
            requests_.push_back(std::move(request));
            StageBack(initialData);
        }

        void EnqueueMesh(const RenderResourceIdentity identity, const RenderMeshDescriptor &descriptor,
                         const std::optional<RenderResourceIdentity> replacedMesh) {
            requests_.push_back(Request{.kind = RequestKind::Mesh, .identity = identity, .mesh = descriptor, .replacedMesh = replacedMesh});
        }

        void EnqueueTexture(const RenderResourceIdentity identity, const RenderTextureDescriptor &descriptor,
                            const std::span<const std::byte> initialData, const RenderMemoryReservationId memoryReservation,
                            const RenderMemoryPlacement &memoryPlacement) {
            Request request{.kind = RequestKind::Texture,
                            .identity = identity,
                            .texture = descriptor,
                            .stagingOffset = initialData.empty() ? 0 : *AlignedOffset(occupiedStagingBytes_),
                            .stagingByteCount = initialData.size(),
                            .memoryReservation = memoryReservation,
                            .memoryPlacement = memoryPlacement};
            requests_.push_back(std::move(request));
            StageBack(initialData);
        }

        void EnqueueTextureView(const RenderResourceIdentity identity, const RenderTextureViewDescriptor &descriptor) {
            requests_.push_back(Request{.kind = RequestKind::TextureView, .identity = identity, .textureView = descriptor});
        }

        void EnqueueRenderTarget(const RenderResourceIdentity identity, const RenderTargetDescriptor &descriptor) {
            requests_.push_back(Request{.kind = RequestKind::RenderTarget, .identity = identity, .renderTarget = descriptor});
        }

        void MarkBackAsReplacement(const RenderResourceIdentity replacedMesh) noexcept {
            requests_.back().replacedMesh = replacedMesh;
        }

        [[nodiscard]] bool Empty() const noexcept {
            return readIndex_ == requests_.size();
        }

        [[nodiscard]] const Request &Front() const noexcept {
            return requests_[readIndex_];
        }

        [[nodiscard]] std::span<const std::byte> FrontInitialData() const noexcept {
            const Request &request = requests_[readIndex_];
            return std::span<const std::byte>{stagingStorage_.data(), stagingStorage_.size()}.subspan(request.stagingOffset,
                                                                                                      request.stagingByteCount);
        }

        [[nodiscard]] bool DrainLimitReached(const std::size_t completedRequests, const std::size_t completedBytes) const noexcept {
            if (completedRequests >= limits_.maximumRequestsPerDrain) {
                return true;
            }
            return completedRequests > 0 && (completedBytes >= limits_.maximumBytesPerDrain ||
                                             requests_[readIndex_].stagingByteCount > limits_.maximumBytesPerDrain - completedBytes);
        }

        Request Pop() {
            Request request = std::move(requests_[readIndex_]);
            ++readIndex_;
            pendingPayloadBytes_ -= request.stagingByteCount;
            return request;
        }

        [[nodiscard]] std::optional<Request> Cancel(const RenderResourceIdentity identity) {
            const auto found =
                std::ranges::find(std::ranges::subrange{requests_.begin() + static_cast<std::ptrdiff_t>(readIndex_), requests_.end()},
                                  identity, &Request::identity);
            if (found == requests_.end())
                return std::nullopt;
            Request request = std::move(*found);
            pendingPayloadBytes_ -= request.stagingByteCount;
            requests_.erase(found);
            ++cancelledRequestCount_;
            Repack();
            return request;
        }

        void StopAdmission() noexcept {
            acceptingRequests_ = false;
        }

        void CompleteBatch(const std::size_t requestCount, const std::size_t payloadBytes) noexcept {
            if (requestCount == 0)
                return;
            ++completedBatchCount_;
            lastBatchRequestCount_ = static_cast<std::uint32_t>(requestCount);
            lastBatchPayloadBytes_ = payloadBytes;
            Repack();
        }

        [[nodiscard]] RenderResourceUploadSnapshot Snapshot() const noexcept {
            return {.pendingPayloadBytes = pendingPayloadBytes_,
                    .occupiedStagingBytes = occupiedStagingBytes_,
                    .pendingRequests = static_cast<std::uint32_t>(PendingRequestCount()),
                    .completedBatchCount = completedBatchCount_,
                    .cancelledRequestCount = cancelledRequestCount_,
                    .lastBatchPayloadBytes = lastBatchPayloadBytes_,
                    .lastBatchRequestCount = lastBatchRequestCount_,
                    .acceptingRequests = acceptingRequests_};
        }

    private:
        [[nodiscard]] std::optional<std::size_t> AlignedOffset(const std::size_t offset) const noexcept {
            const std::size_t mask = limits_.stagingOffsetAlignment - 1U;
            if (offset > std::numeric_limits<std::size_t>::max() - mask)
                return std::nullopt;
            return (offset + mask) & ~mask;
        }

        void StageBack(const std::span<const std::byte> initialData) noexcept {
            Request &request = requests_.back();
            if (initialData.empty())
                return;
            stagingStorage_.resize(request.stagingOffset + request.stagingByteCount);
            std::ranges::copy(initialData, stagingStorage_.begin() + static_cast<std::ptrdiff_t>(request.stagingOffset));
            pendingPayloadBytes_ += initialData.size();
            occupiedStagingBytes_ = request.stagingOffset + initialData.size();
        }

        void Repack() noexcept {
            std::size_t nextOffset = 0;
            auto pending = requests_.begin() + static_cast<std::ptrdiff_t>(readIndex_);
            for (auto current = pending; current != requests_.end(); ++current) {
                Request &request = *current;
                if (request.stagingByteCount == 0) {
                    request.stagingOffset = 0;
                    continue;
                }
                const std::size_t destination = *AlignedOffset(nextOffset);
                if (destination != request.stagingOffset) {
                    std::memmove(stagingStorage_.data() + destination, stagingStorage_.data() + request.stagingOffset,
                                 request.stagingByteCount);
                    request.stagingOffset = destination;
                }
                nextOffset = destination + request.stagingByteCount;
            }
            occupiedStagingBytes_ = nextOffset;
            stagingStorage_.resize(occupiedStagingBytes_);
            requests_.erase(requests_.begin(), pending);
            readIndex_ = 0;
        }

        [[nodiscard]] std::size_t PendingRequestCount() const noexcept {
            return requests_.size() - readIndex_;
        }

        RenderResourceUploadLimits limits_;
        std::vector<Request> requests_;
        std::size_t readIndex_{0};
        std::vector<std::byte> stagingStorage_;
        std::size_t pendingPayloadBytes_{0};
        std::size_t occupiedStagingBytes_{0};
        std::uint64_t completedBatchCount_{0};
        std::uint64_t cancelledRequestCount_{0};
        std::size_t lastBatchPayloadBytes_{0};
        std::uint32_t lastBatchRequestCount_{0};
        bool acceptingRequests_{true};
    };
}  // namespace Horo::Render::Detail
