#pragma once

/** @file D3D12CommandTestSupport.h
 * @brief Real WARP command probes and bounded test-only queue/lifetime guards.
 */
#include "D3D12NativeCommands.h"
#include "D3D12ResourceTestSupport.h"

#if defined(_WIN32) && defined(_M_X64)
namespace Horo::Render::D3D12CommandTests {
    using namespace D3D12Tests;

    struct CompletionLease final : IRenderGraphResourceLease {
        unsigned releases{0};
        std::thread::id releasedOn;

        void Release() noexcept override {
            ++releases;
            releasedOn = std::this_thread::get_id();
        }
    };

    /** @brief Declared after every submitted backing/lease and before a QueueGate. */
    struct DrainGuard {
        Fixture &fixture;

        ~DrainGuard() {
            if (!fixture.resources)
                return;
            fixture.resources->StopAdmission();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
            while (std::chrono::steady_clock::now() < deadline) {
                const auto drained = fixture.resources->PollDrain();
                if (drained.HasValue() && drained.Value())
                    return;
                if (FAILED(fixture.device->GetDeviceRemovedReason())) {
                    fixture.resources.reset();
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            HORO_INVARIANT_MSG(false, "Test deadline expired while native commands still retain borrowed backing.");
        }
    };

    /** @brief Injects a native GPU-side queue stall, always released before DrainGuard runs. */
    class QueueGate {
    public:
        explicit QueueGate(Fixture &fixture) {
            REQUIRE(SUCCEEDED(fixture.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))));
            REQUIRE(SUCCEEDED(fixture.queue->Wait(fence_.Get(), 1)));
        }

        ~QueueGate() {
            Open();
        }

        void Open() noexcept {
            (void)fence_->Signal(1);
        }

    private:
        ComPtr<ID3D12Fence> fence_;
    };

    /** @brief Real GPU timestamp and native resolve output; never a fake completion source. */
    class TimestampProbe {
    public:
        explicit TimestampProbe(ID3D12Device &device) {
            D3D12_QUERY_HEAP_DESC description{};
            description.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            description.Count = 1;
            REQUIRE(SUCCEEDED(device.CreateQueryHeap(&description, IID_PPV_ARGS(&queries_))));
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC buffer{};
            buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width = sizeof(UINT64);
            buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
            buffer.SampleDesc.Count = 1;
            buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            REQUIRE(SUCCEEDED(device.CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                             IID_PPV_ARGS(&readback_))));
        }

        void Record(ID3D12GraphicsCommandList &list) {
            list.EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
            list.ResolveQueryData(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 1, readback_.Get(), 0);
        }

        UINT64 Value() {
            void *mapped = nullptr;
            const D3D12_RANGE range{0, sizeof(UINT64)};
            REQUIRE(SUCCEEDED(readback_->Map(0, &range, &mapped)));
            const auto value = *static_cast<const UINT64 *>(mapped);
            const D3D12_RANGE noWrite{0, 0};
            readback_->Unmap(0, &noWrite);
            return value;
        }

    private:
        ComPtr<ID3D12QueryHeap> queries_;
        ComPtr<ID3D12Resource> readback_;
    };

    inline ID3D12Commands &Commands(Fixture &fixture) {
        auto *commands = fixture.resources->Commands();
        REQUIRE(commands != nullptr);
        REQUIRE(commands->Configure({1}).HasValue());
        return *commands;
    }

    inline D3D12CommandId Closed(ID3D12Commands &commands) {
        const auto recording = commands.Begin();
        REQUIRE(recording.HasValue());
        REQUIRE(commands.Close(recording.Value()).HasValue());
        return recording.Value();
    }

    inline void Await(ID3D12Commands &commands, D3D12CommandId id) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
        while (std::chrono::steady_clock::now() < deadline) {
            const auto polled = commands.Poll(id);
            REQUIRE(polled.HasValue());
            if (polled.Value())
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        FAIL("Bounded real WARP command completion timed out");
    }

    inline void AwaitDrain(Fixture &fixture) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
        while (std::chrono::steady_clock::now() < deadline) {
            const auto polled = fixture.resources->PollDrain();
            REQUIRE(polled.HasValue());
            if (polled.Value())
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        FAIL("Bounded native command drain timed out");
    }
}  // namespace Horo::Render::D3D12CommandTests
#endif
