#include "D3D12ResourceInternals.h"

#if defined(_WIN32) && defined(_M_X64)
namespace Horo::Render::D3D12Detail {

    namespace {
        /** @brief Validates the supported complete-base-level texture copy shape and usage. */
        bool TextureRangeSupported(const Resource &resource, std::size_t offset, std::size_t bytes, bool readback) {
            if (resource.kind != Resource::Kind::Texture || offset != 0 || resource.texture.sampleCount != 1)
                return false;
            if (IsDepth(resource.texture.format) || RenderTextureBaseLevelByteSize(resource.texture) != bytes)
                return false;
            return !readback || HasTextureUsage(resource.texture.usage, RenderTextureUsage::CopySource);
        }

        /** @brief Packs or expands the checked tight color payload across native row and slice pitches. */
        void CopyRows(const Transfer &transfer, const std::byte *source, std::byte *destination, bool pack) {
            if (transfer.footprints.empty()) {
                std::memcpy(destination, source, transfer.payloadBytes);
                return;
            }
            std::size_t tightOffset = 0;
            const auto rowBytes = transfer.resource->texture.extent.width * *RenderTextureTexelBytes(transfer.resource->texture.format);
            for (const auto &layout : transfer.footprints) {
                for (UINT z = 0; z < layout.Footprint.Depth; ++z)
                    for (UINT y = 0; y < layout.Footprint.Height; ++y) {
                        const auto nativeOffset =
                            layout.Offset + (std::size_t(z) * layout.Footprint.Height + y) * layout.Footprint.RowPitch;
                        const auto sourceOffset = pack ? nativeOffset : tightOffset;
                        const auto destinationOffset = pack ? tightOffset : nativeOffset;
                        std::memcpy(destination + destinationOffset, source + sourceOffset, rowBytes);
                        tightOffset += rowBytes;
                    }
            }
        }
    }  // namespace

    /** @copydoc Resources::Upload */
    Result<D3D12TransferId> Resources::Upload(std::uint64_t instance, std::span<const std::byte> bytes, std::size_t offset,
                                              const CancellationToken &cancellation) {
        if (auto checked = Check(); checked.HasError())
            return Result<D3D12TransferId>::Failure(checked.ErrorValue());
        if (cancellation.IsCancellationRequested())
            return Failure<D3D12TransferId>("render.d3d12.cancelled", "Upload cancelled before submission.");
        const auto resource = Find(instance);
        if (!resource || !ValidRange(*resource, offset, bytes.size(), false))
            return Failure<D3D12TransferId>("render.d3d12.upload_range",
                                            "Use a live copy-compatible destination and a valid nonempty range.");
        if (resource->kind == Resource::Kind::Buffer && resource->buffer.access == RenderBufferAccess::HostVisible) {
            // Host writes must be prepared before GPU submission; frontend ownership pins enforce that boundary.
            void *mapped = nullptr;
            const D3D12_RANGE readRange{0, 0};
            const HRESULT status = resource->native->Map(0, &readRange, &mapped);
            if (FAILED(status))
                return Result<D3D12TransferId>::Failure(ResourceError("render.d3d12.map_failed", "Map host-visible buffer.", status));
            std::memcpy(static_cast<std::byte *>(mapped) + offset, bytes.data(), bytes.size());
            const D3D12_RANGE written{offset, offset + bytes.size()};
            resource->native->Unmap(0, &written);
            if (nextTransfer_ == std::numeric_limits<std::uint64_t>::max())
                return Failure<D3D12TransferId>("render.d3d12.transfer_capacity",
                                                "Create a new service before transfer identity exhaustion.");
            return Result<D3D12TransferId>::Success({nextTransfer_++});
        }
        return Submit(resource, offset, bytes.size(), bytes, false, cancellation);
    }

    /** @copydoc Resources::Readback */
    Result<D3D12TransferId> Resources::Readback(std::uint64_t instance, std::size_t offset, std::size_t bytes,
                                                const CancellationToken &cancellation) {
        if (auto checked = Check(); checked.HasError())
            return Result<D3D12TransferId>::Failure(checked.ErrorValue());
        if (cancellation.IsCancellationRequested())
            return Failure<D3D12TransferId>("render.d3d12.cancelled", "Readback cancelled before submission.");
        const auto resource = Find(instance);
        if (!resource || !ValidRange(*resource, offset, bytes, true))
            return Failure<D3D12TransferId>("render.d3d12.readback_range", "Use a live copy-source resource and a valid nonempty range.");
        return Submit(resource, offset, bytes, {}, true, cancellation);
    }

    /** @copydoc Resources::Cancel */
    Result<void> Resources::Cancel(D3D12TransferId transfer) {
        if (auto checked = Check(false); checked.HasError())
            return checked;
        auto *record = FindTransfer(transfer);
        if (!record)
            return Failure<void>("render.d3d12.transfer_invalid", "Use an outstanding transfer identity from this service.");
        record->cancelled = true;
        return Result<void>::Success();
    }

    /** @copydoc Resources::FindTransfer */
    Transfer *Resources::FindTransfer(D3D12TransferId id) {
        const auto found = std::ranges::find_if(transfers_, [&](const Transfer &entry) {
            return entry.id.value == id.value;
        });
        return found == transfers_.end() ? nullptr : &*found;
    }

    /** @copydoc Resources::ValidRange */
    bool Resources::ValidRange(const Resource &resource, std::size_t offset, std::size_t bytes, bool readback) const {
        if (bytes == 0)
            return false;
        if (resource.kind == Resource::Kind::Buffer) {
            if (offset > resource.buffer.byteSize || bytes > resource.buffer.byteSize - offset)
                return false;
            return !readback || HasBufferUsage(resource.buffer.usage, RenderBufferUsage::CopySource);
        }
        return TextureRangeSupported(resource, offset, bytes, readback);
    }

    /** @copydoc Resources::Submit */
    Result<D3D12TransferId> Resources::Submit(const std::shared_ptr<Resource> &resource, std::size_t offset, std::size_t bytes,
                                              std::span<const std::byte> payload, bool readback, const CancellationToken &cancellation) {
        if (auto result = Poll(); result.HasError())
            return Result<D3D12TransferId>::Failure(result.ErrorValue());
        if (transfers_.size() >= limits_.maximumTransfers || nextFence_ == std::numeric_limits<std::uint64_t>::max() ||
            nextTransfer_ == std::numeric_limits<std::uint64_t>::max())
            return Failure<D3D12TransferId>("render.d3d12.transfer_capacity", "Drain or consume transfers before submitting another copy.");
        Transfer transfer;
        transfer.id = {nextTransfer_++};
        transfer.fence = nextFence_++;
        transfer.resource = resource;
        transfer.readback = readback;
        transfer.payloadBytes = bytes;
        if (auto footprints = PrepareFootprints(transfer); footprints.HasError())
            return Result<D3D12TransferId>::Failure(footprints.ErrorValue());
        auto prepared = PrepareStaging(transfer, payload);
        if (prepared.HasError())
            return Result<D3D12TransferId>::Failure(prepared.ErrorValue());
        if (auto commands = PrepareCopyCommands(transfer, offset); commands.HasError())
            return Result<D3D12TransferId>::Failure(commands.ErrorValue());
        if (cancellation.IsCancellationRequested())
            return Failure<D3D12TransferId>("render.d3d12.cancelled", "Copy cancelled before GPU submission.");
        // Reserved vector capacity makes publication non-allocating before the irreversible Execute call.
        transfers_.push_back(std::move(transfer));
        auto &submitted = transfers_.back();
        ID3D12CommandList *commands[]{submitted.commands.Get()};
        queue_->ExecuteCommandLists(1, commands);
        const HRESULT status = queue_->Signal(fence_.Get(), submitted.fence);
        if (FAILED(status)) {
            submitted.signalPending = true;
            submitted.cancelled = true;
            return Result<D3D12TransferId>::Failure(
                ResourceError("render.d3d12.signal_failed", "Copy submitted; retain storage and retry signaling during Poll.", status));
        }
        return Result<D3D12TransferId>::Success(submitted.id);
    }

    /** @copydoc Resources::PrepareFootprints */
    Result<void> Resources::PrepareFootprints(Transfer &transfer) {
        if (transfer.resource->kind == Resource::Kind::Texture) {
            const auto shape = transfer.resource->native->GetDesc();
            const UINT count = transfer.resource->texture.layerCount;
            transfer.footprints.resize(count);
            UINT64 end = 0;
            for (UINT layer = 0; layer < count; ++layer) {
                UINT64 size = 0;
                // Reserve full row-pitch padding, including the last row, independently of TotalBytes semantics.
                const UINT64 base =
                    (end + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~(UINT64(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT) - 1);
                device_->GetCopyableFootprints(&shape, layer * transfer.resource->texture.mipCount, 1, base, &transfer.footprints[layer],
                                               nullptr, nullptr, &size);
                const auto &footprint = transfer.footprints[layer].Footprint;
                const UINT64 padded = UINT64(footprint.RowPitch) * footprint.Height * footprint.Depth;
                if (size == std::numeric_limits<UINT64>::max() || padded > limits_.maximumTransferBytes ||
                    base > limits_.maximumTransferBytes - padded)
                    return Failure<void>("render.d3d12.transfer_capacity",
                                         "Texture copy footprints exceed the configured transfer allowance.");
                end = base + padded;
            }
            transfer.backingBytes = static_cast<std::size_t>(end);
        } else
            transfer.backingBytes = transfer.payloadBytes;
        return Result<void>::Success();
    }

    /** @copydoc Resources::PrepareCopyCommands */
    Result<void> Resources::PrepareCopyCommands(Transfer &transfer, std::size_t offset) {
        HRESULT status = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&transfer.allocator));
        if (SUCCEEDED(status))
            status = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, transfer.allocator.Get(), nullptr,
                                                IID_PPV_ARGS(&transfer.commands));
        if (FAILED(status))
            return Result<D3D12TransferId>::Failure(
                ResourceError("render.d3d12.copy_commands_failed", "Prepare a bounded copy batch.", status));
        RecordCopy(transfer, offset);
        status = transfer.commands->Close();
        if (FAILED(status))
            return Result<D3D12TransferId>::Failure(
                ResourceError("render.d3d12.copy_close_failed", "Close the prepared copy batch.", status));
        return Result<void>::Success();
    }

    /** @copydoc Resources::PrepareStaging */
    Result<void> Resources::PrepareStaging(Transfer &transfer, std::span<const std::byte> payload) {
        auto shape = BufferShape(transfer.backingBytes);
        const auto info = device_->GetResourceAllocationInfo(0, 1, &shape);
        const auto snapshot = Snapshot();
        const std::size_t occupied = snapshot.uploadBytes + snapshot.readbackBytes;
        if (info.SizeInBytes == std::numeric_limits<UINT64>::max() || info.SizeInBytes > limits_.maximumTransferBytes ||
            occupied > limits_.maximumTransferBytes - info.SizeInBytes)
            return Failure<void>("render.d3d12.transfer_capacity", "Native padded staging backing exceeds the host transfer allowance.");
        transfer.backingBytes = static_cast<std::size_t>(info.SizeInBytes);
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = transfer.readback ? D3D12_HEAP_TYPE_READBACK : D3D12_HEAP_TYPE_UPLOAD;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;
        HRESULT status =
            device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &shape,
                                             transfer.readback ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_GENERIC_READ,
                                             nullptr, IID_PPV_ARGS(&transfer.staging));
        if (FAILED(status))
            return Result<void>::Failure(ResourceError("render.d3d12.staging_failed", "Allocate bounded admitted copy staging.", status));
        if (transfer.readback)
            return Result<void>::Success();
        void *mapped = nullptr;
        const D3D12_RANGE noRead{0, 0};
        status = transfer.staging->Map(0, &noRead, &mapped);
        if (FAILED(status))
            return Result<void>::Failure(ResourceError("render.d3d12.map_failed", "Map the upload allocation.", status));
        CopyRows(transfer, payload.data(), static_cast<std::byte *>(mapped), false);
        transfer.staging->Unmap(0, nullptr);
        return Result<void>::Success();
    }

    /** @copydoc Resources::RecordCopy */
    void Resources::RecordCopy(Transfer &transfer, std::size_t offset) {
        const bool hostVisible =
            transfer.resource->kind == Resource::Kind::Buffer && transfer.resource->buffer.access == RenderBufferAccess::HostVisible;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = transfer.resource->native.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.StateAfter = transfer.readback ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_COPY_DEST;
        if (!hostVisible)
            transfer.commands->ResourceBarrier(1, &barrier);
        if (transfer.footprints.empty()) {
            if (transfer.readback)
                transfer.commands->CopyBufferRegion(transfer.staging.Get(), 0, transfer.resource->native.Get(), offset,
                                                    transfer.payloadBytes);
            else
                transfer.commands->CopyBufferRegion(transfer.resource->native.Get(), offset, transfer.staging.Get(), 0,
                                                    transfer.payloadBytes);
        } else {
            for (UINT layer = 0; layer < transfer.footprints.size(); ++layer) {
                D3D12_TEXTURE_COPY_LOCATION texture{};
                texture.pResource = transfer.resource->native.Get();
                texture.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                texture.SubresourceIndex = layer * transfer.resource->texture.mipCount;
                D3D12_TEXTURE_COPY_LOCATION staging{};
                staging.pResource = transfer.staging.Get();
                staging.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                staging.PlacedFootprint = transfer.footprints[layer];
                transfer.commands->CopyTextureRegion(transfer.readback ? &staging : &texture, 0, 0, 0,
                                                     transfer.readback ? &texture : &staging, nullptr);
            }
        }
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        if (!hostVisible)
            transfer.commands->ResourceBarrier(1, &barrier);
    }

    /** @copydoc Resources::TakeReadback */
    Result<std::vector<std::byte>> Resources::TakeReadback(D3D12TransferId id) {
        if (auto checked = Check(false); checked.HasError())
            return Result<std::vector<std::byte>>::Failure(checked.ErrorValue());
        auto polled = Poll();
        if (polled.HasError())
            return Result<std::vector<std::byte>>::Failure(polled.ErrorValue());
        auto *transfer = FindTransfer(id);
        if (!transfer || !transfer->readback || transfer->cancelled)
            return Failure<std::vector<std::byte>>("render.d3d12.transfer_invalid", "Use an outstanding uncancelled readback identity.");
        if (fence_->GetCompletedValue() < transfer->fence)
            return Failure<std::vector<std::byte>>("render.d3d12.transfer_pending", "Poll later; GPU readback has not completed.");
        std::vector<std::byte> result(transfer->payloadBytes);
        void *mapped = nullptr;
        const D3D12_RANGE range{0, static_cast<SIZE_T>(transfer->staging->GetDesc().Width)};
        const HRESULT status = transfer->staging->Map(0, &range, &mapped);
        if (FAILED(status))
            return Result<std::vector<std::byte>>::Failure(ResourceError("render.d3d12.map_failed", "Map completed CPU readback.", status));
        CopyRows(*transfer, static_cast<const std::byte *>(mapped), result.data(), true);
        const D3D12_RANGE noWrite{0, 0};
        transfer->staging->Unmap(0, &noWrite);
        std::erase_if(transfers_, [&](const Transfer &entry) {
            return entry.id.value == id.value;
        });
        Reclaim();
        return Result<std::vector<std::byte>>::Success(std::move(result));
    }
}  // namespace Horo::Render::D3D12Detail
#endif
