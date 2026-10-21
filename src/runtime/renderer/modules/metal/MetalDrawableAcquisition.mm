#include "MetalDrawableAcquisition.h"

#include "Horo/Runtime/Render/RenderPresentationOutput.h"

#include <condition_variable>
#include <cstdio>
#include <exception>
#include <mutex>
#include <utility>

namespace Horo::Render::Detail {
    namespace {
        /** @brief Copies bounded original native failure context; no borrowed exception survives the worker call. */
        void RecordNativeFailure(std::array<char, 512> &destination, NSException *exception) noexcept {
            const char *name = exception.name.UTF8String;
            const char *reason = exception.reason.UTF8String;
            std::snprintf(destination.data(), destination.size(), "Metal drawable exception %.100s: %.350s",
                          name == nullptr ? "unknown" : name, reason == nullptr ? "no reason" : reason);
        }

        /** @brief Production acquisition runs exclusively on the explicitly owned native worker. */
        class NativeDrawableSource final : public IMetalDrawableSource {
        public:
            id<CAMetalDrawable> Acquire(CAMetalLayer *layer) override {
                return [layer nextDrawable];
            }
        };
    }  // namespace

    struct MetalDrawableAcquisition::State final {
        __strong CAMetalLayer *layer{nil};
        __strong id<MTLDevice> device{nil};
        std::unique_ptr<IMetalDrawableSource> source;
        mutable std::mutex mutex;
        std::condition_variable wake;
        MetalDrawableResult result;
        std::uint64_t generation{};
        FramebufferExtent extent;
        bool requested{};
        bool working{};
        bool completed{};
        bool closing{};

        /** @brief Owns the native exception boundary separately from the synchronized handoff. */
        MetalDrawableResult Capture(std::uint64_t capturedGeneration, FramebufferExtent capturedExtent) noexcept {
            MetalDrawableResult next;
            next.generation = capturedGeneration;
            next.extent = capturedExtent;
            @autoreleasepool {
                @try {
                    try {
                        next.drawable = source->Acquire(layer);
                        next.progress = next.drawable == nil ? MetalDrawablePoll::Unavailable : MetalDrawablePoll::Ready;
                    } catch (const std::exception &exception) {
                        next.progress = MetalDrawablePoll::Failed;
                        std::snprintf(next.failure.data(), next.failure.size(), "Metal drawable source failed: %.440s", exception.what());
                    } catch (...) {
                        next.progress = MetalDrawablePoll::Failed;
                        std::snprintf(next.failure.data(), next.failure.size(), "Metal drawable source raised an unknown C++ exception.");
                    }
                } @catch (NSException *exception) {
                    next.progress = MetalDrawablePoll::Failed;
                    RecordNativeFailure(next.failure, exception);
                }
            }
            return next;
        }

        /** @brief Only this native thread blocks; the owner keeps all state alive until NSThread proves termination. */
        void Run() noexcept {
            for (;;) {
                std::unique_lock lock{mutex};
                wake.wait(lock, [this] {
                    return requested || closing;
                });
                if (closing) {
                    return;
                }
                MetalDrawableResult next;
                next.generation = generation;
                next.extent = extent;
                requested = false;
                working = true;
                lock.unlock();
                next = Capture(next.generation, next.extent);
                lock.lock();
                working = false;
                if (closing) {
                    if (next.progress == MetalDrawablePoll::Failed) {
                        result = std::move(next);
                        completed = true;
                    }
                    return;
                }
                result = std::move(next);
                completed = true;
            }
        }
    };

    /** @copydoc MetalDrawableAcquisition::Create */
    Result<std::unique_ptr<MetalDrawableAcquisition>> MetalDrawableAcquisition::Create(CAMetalLayer *layer, id<MTLDevice> device,
                                                                                       std::unique_ptr<IMetalDrawableSource> source) {
        using Creation = Result<std::unique_ptr<MetalDrawableAcquisition>>;
        if (layer == nil || device == nil || !source) {
            return Creation::Failure(MakeError(RenderPresentationErrors::InvalidOutput));
        }
        auto state = std::make_shared<State>();
        state->layer = layer;
        state->device = device;
        state->source = std::move(source);
        NSThread *thread = [[NSThread alloc] initWithBlock:^{
          state->Run();
        }];
        if (thread == nil) {
            return Creation::Failure(MakeError(RenderPresentationErrors::OutputUnavailable));
        }
        auto owner = std::unique_ptr<MetalDrawableAcquisition>(new MetalDrawableAcquisition(std::move(state), thread));
        return Creation::Success(std::move(owner));
    }

    MetalDrawableAcquisition::MetalDrawableAcquisition(std::shared_ptr<State> state, NSThread *thread) noexcept
        : ownerThread_(std::this_thread::get_id()), state_(std::move(state)), thread_(thread) {}

    /** @copydoc MetalDrawableAcquisition::~MetalDrawableAcquisition */
    MetalDrawableAcquisition::~MetalDrawableAcquisition() {
        if (thread_ != nil && !IsDrained()) {
            std::terminate();
        }
        // Foundation may retain a finished NSThread object for bookkeeping. Clear terminal parents explicitly so its
        // captured state cannot keep retired device/layer/source ownership across later host generations.
        state_->source.reset();
        state_->result = {};
        state_->layer = nil;
        state_->device = nil;
    }

    /** @copydoc MetalDrawableAcquisition::Request */
    bool MetalDrawableAcquisition::Request(std::uint64_t generation, FramebufferExtent extent) noexcept {
        if (std::this_thread::get_id() != ownerThread_)
            return false;
        std::unique_lock lock{state_->mutex, std::try_to_lock};
        if (!lock || state_->closing || state_->requested || state_->working || state_->completed || generation == 0 || !extent.IsValid()) {
            return false;
        }
        state_->generation = generation;
        state_->extent = extent;
        state_->requested = true;
        lock.unlock();
        if (!started_) {
            started_ = true;
            @try {
                [thread_ start];
            } @catch (NSException *exception) {
                RecordNativeFailure(startFailure_, exception);
                started_ = thread_.executing || thread_.finished;
                startFailed_ = true;
                return true;
            }
        }
        state_->wake.notify_one();
        return true;
    }

    /** @copydoc MetalDrawableAcquisition::Poll */
    MetalDrawableResult MetalDrawableAcquisition::Poll(std::uint64_t generation) noexcept {
        if (std::this_thread::get_id() != ownerThread_)
            return {MetalDrawablePoll::WrongThread, nil, generation, {}};
        if (startFailed_) {
            MetalDrawableResult failed{MetalDrawablePoll::Failed, nil, generation, {}};
            failed.failure = startFailure_;
            return failed;
        }
        std::unique_lock lock{state_->mutex, std::try_to_lock};
        if (!lock || !state_->completed) {
            return {};
        }
        auto result = std::move(state_->result);
        state_->result = {};
        state_->completed = false;
        if (result.generation != generation) {
            result.drawable = nil;
            result.progress = MetalDrawablePoll::Stale;
        }
        return result;
    }

    /** @copydoc MetalDrawableAcquisition::Idle */
    bool MetalDrawableAcquisition::Idle() const noexcept {
        if (std::this_thread::get_id() != ownerThread_)
            return false;
        std::unique_lock lock{state_->mutex, std::try_to_lock};
        return lock && !state_->requested && !state_->working && !state_->completed;
    }

    /** @copydoc MetalDrawableAcquisition::Close */
    bool MetalDrawableAcquisition::Close() noexcept {
        if (std::this_thread::get_id() != ownerThread_)
            return false;
        std::unique_lock lock{state_->mutex, std::try_to_lock};
        if (!lock) {
            return false;
        }
        state_->closing = true;
        state_->requested = false;
        if (state_->result.progress != MetalDrawablePoll::Failed) {
            state_->completed = false;
            state_->result = {};
        }
        lock.unlock();
        state_->wake.notify_one();
        return true;
    }

    /** @copydoc MetalDrawableAcquisition::IsDrained */
    bool MetalDrawableAcquisition::IsDrained() const noexcept {
        if (std::this_thread::get_id() != ownerThread_)
            return false;
        return !started_ || thread_.finished;
    }

    /** @copydoc MakeMetalDrawableSource */
    std::unique_ptr<IMetalDrawableSource> MakeMetalDrawableSource() {
        return std::make_unique<NativeDrawableSource>();
    }
}  // namespace Horo::Render::Detail
