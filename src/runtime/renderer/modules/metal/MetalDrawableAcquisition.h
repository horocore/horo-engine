#pragma once

#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Render/RenderBackend.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <array>
#include <cstdint>
#include <memory>
#include <thread>

namespace Horo::Render::Detail {
    /** @brief Explicit native acquisition seam, injected once at owner creation and used only by its private worker. */
    class IMetalDrawableSource {
    public:
        virtual ~IMetalDrawableSource() = default;
        /** @brief Acquires from the retained layer. May block; never called by the render/event owner. */
        [[nodiscard]] virtual id<CAMetalDrawable> Acquire(CAMetalLayer *layer) = 0;
    };

    enum class MetalDrawablePoll : std::uint8_t {
        Pending,
        Ready,
        Unavailable,
        Failed,
        Stale
    };

    /** @brief Single-slot worker result; generation and extent identify the exact immutable request. */
    struct MetalDrawableResult final {
        MetalDrawablePoll progress{MetalDrawablePoll::Pending};
        __strong id<CAMetalDrawable> drawable{nil};
        std::uint64_t generation{};
        FramebufferExtent extent;
        std::array<char, 512> failure{}; /**< Bounded owned native/C++ failure context; no borrowed exception text. */
    };

    /** @brief Explicitly owned one-thread/one-slot acquisition owner; never detaches or borrows a host/frontend.
     * @details The mutex protects the single request/result slot and close admission. Owner operations use try-lock,
     * so contention reports Pending. The worker alone calls the injected source and retains strong layer/device parents.
     * Construction preallocates the native thread/block; first Request starts it only after host ownership is established.
     * Layer mutation is forbidden until Idle() succeeds. Close is idempotent, cancels unstarted work and drops nonfailed completed
     * results. Failed completion remains observable through Poll even when close races it. IsDrained proves the native thread finished;
     * until then the host must retain this owner and its parents. Destruction requires IsDrained; no timeout is translated into terminal
     * success.
     */
    class MetalDrawableAcquisition final {
    public:
        /** @brief Preallocates one worker, slot and retained native-parent set without starting acquisition.
         * @param layer Exact admitted layer, retained strongly. @param device Its admitted device, retained strongly.
         * @param source Explicitly transferred native/injected acquisition implementation, never an ambient service.
         * @return Inert owner or typed malformed/unavailable failure; no thread is started on failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<MetalDrawableAcquisition>> Create(CAMetalLayer *layer, id<MTLDevice> device,
                                                                                      std::unique_ptr<IMetalDrawableSource> source);
        ~MetalDrawableAcquisition();
        MetalDrawableAcquisition(const MetalDrawableAcquisition &) = delete;
        MetalDrawableAcquisition &operator=(const MetalDrawableAcquisition &) = delete;

        /** @brief Starts one request only from an idle slot on the declared render owner.
         * @param generation Positive immutable surface/output generation. @param extent Positive requested pixel size.
         * @return False for closing, invalid, occupied or contended state; no request or generation is queued then.
         * @details First request starts the preallocated native thread at the host's explicit preparation boundary.
         */
        [[nodiscard]] bool Request(std::uint64_t generation, FramebufferExtent extent) noexcept;
        /** @brief Takes one result without waiting; stale generations never expose their drawable.
         * @param generation Current owner generation. @return Pending, exact ready value, stale drop or owned failure context.
         */
        [[nodiscard]] MetalDrawableResult Poll(std::uint64_t generation) noexcept;
        /** @brief Tests that no request, native call or unconsumed result can observe mutable layer properties.
         * @return True only for an idle uncontended slot. This does not authorize mutation from another thread.
         */
        [[nodiscard]] bool Idle() const noexcept;
        /** @brief Closes admission using a nonblocking lock attempt; repeat until true before polling drain.
         * @return True after admission closes; false for contention, retaining every parent and request.
         */
        [[nodiscard]] bool Close() noexcept;
        /** @brief Reports actual native-thread termination, independently of a cancellation request.
         * @return True for an unstarted or finished thread; started pending work remains false even after Close.
         */
        [[nodiscard]] bool IsDrained() const noexcept;

    private:
        struct State;
        explicit MetalDrawableAcquisition(std::shared_ptr<State> state, NSThread *thread) noexcept;
        const std::thread::id ownerThread_;
        std::shared_ptr<State> state_;
        __strong NSThread *thread_{nil};
        std::array<char, 512> startFailure_{};
        bool startFailed_{false};
        bool started_{false}; /**< Owner-thread state; construction preallocates but does not launch native work. */
    };

    /** @brief Constructs the production nextDrawable source; no native work occurs until the owned worker requests it. */
    [[nodiscard]] std::unique_ptr<IMetalDrawableSource> MakeMetalDrawableSource();
}  // namespace Horo::Render::Detail
