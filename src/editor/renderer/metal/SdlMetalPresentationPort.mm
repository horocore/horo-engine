#include "SdlMetalPresentationPort.h"

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#include <SDL3/SDL_metal.h>
#include <exception>
#include <limits>
#include <string>
#include <thread>
#include <utility>

namespace Horo::Editor {
    namespace {
        [[nodiscard]] Error MakeMetalSurfaceError(const char *code, std::string message) {
            return Error{.code = ErrorCode{code},
                         .domain = ErrorDomainId{"horo.editor.sdl.metal"},
                         .severity = ErrorSeverity::Error,
                         .message = std::move(message)};
        }
    }  // namespace

    struct SdlMetalPresentationPort::Impl {
        explicit Impl(SDL_Window &borrowedWindow) noexcept : window(&borrowedWindow), ownerThread(std::this_thread::get_id()) {}

        [[nodiscard]] bool OnOwnerThread() const noexcept {
            return ownerThread == std::this_thread::get_id() && [NSThread isMainThread];
        }

        // AppKit notifications and surface operations are serialized on the main owner thread. The block owns only a
        // weak state lease; removing observers and clearing active prevents queued callbacks from touching retired views.
        void RefreshDisplayFacts() {
            if (!active || !OnOwnerThread()) {
                return;
            }
            NSScreen *screen = nativeWindow.screen;
            Render::RenderOutputDisplayFacts candidate;
            candidate.attached = screen != nil;
            const std::uint64_t monitor = screen == nil ? 0 : [screen.deviceDescription[@"NSScreenNumber"] unsignedLongLongValue];
            if (screen != nil) {
                candidate.linearHeadroom = static_cast<float>(screen.maximumExtendedDynamicRangeColorComponentValue);
                candidate.potentialLinearHeadroom = static_cast<float>(screen.maximumPotentialExtendedDynamicRangeColorComponentValue);
                candidate.hdr = *candidate.potentialLinearHeadroom > 1.0F ? Render::RenderDisplayHdrSupport::Supported
                                                                          : Render::RenderDisplayHdrSupport::Unsupported;
            }
            candidate.revision = facts.revision;
            if (facts.revision != 0 && candidate == facts && monitor == monitorIdentity && screen.colorSpace == observedColorSpace) {
                return;
            }
            if (facts.revision == std::numeric_limits<std::uint64_t>::max()) {
                revisionExhausted = true;
                return;
            }
            candidate.revision = facts.revision + 1;
            facts = candidate;
            monitorIdentity = monitor;
            observedColorSpace = screen.colorSpace;
        }

        void ObserveDisplay(const std::shared_ptr<Impl> &owner) {
            const std::weak_ptr<Impl> weakOwner{owner};
            void (^refresh)(NSNotification *) = ^(NSNotification *) {
              if (const auto state = weakOwner.lock()) {
                  state->RefreshDisplayFacts();
              }
            };
            NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
            screenObserver = [center addObserverForName:NSApplicationDidChangeScreenParametersNotification
                                                 object:nil
                                                  queue:[NSOperationQueue mainQueue]
                                             usingBlock:refresh];
            profileObserver = [center addObserverForName:NSScreenColorSpaceDidChangeNotification
                                                  object:nil
                                                   queue:[NSOperationQueue mainQueue]
                                              usingBlock:refresh];
            windowObserver = [center addObserverForName:NSWindowDidChangeScreenNotification
                                                 object:nativeWindow
                                                  queue:[NSOperationQueue mainQueue]
                                             usingBlock:refresh];
        }

        void StopObserving() noexcept {
            active = false;
            NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
            if (screenObserver != nil) {
                [center removeObserver:screenObserver];
            }
            if (profileObserver != nil) {
                [center removeObserver:profileObserver];
            }
            if (windowObserver != nil) {
                [center removeObserver:windowObserver];
            }
            screenObserver = nil;
            profileObserver = nil;
            windowObserver = nil;
            nativeWindow = nil;
            observedColorSpace = nil;
        }

        SDL_Window *window{nullptr};
        const std::thread::id ownerThread;
        SDL_MetalView view{nullptr};
        __strong CAMetalLayer *layer{nil};
        __weak NSWindow *nativeWindow{nil};
        __strong NSColorSpace *observedColorSpace{nil};
        __strong id screenObserver{nil};
        __strong id profileObserver{nil};
        __strong id windowObserver{nil};
        Render::RenderOutputDisplayFacts facts;
        std::uint64_t monitorIdentity{};
        bool active{};
        bool revisionExhausted{};
    };

    /** @copydoc SdlMetalPresentationPort::SdlMetalPresentationPort */
    SdlMetalPresentationPort::SdlMetalPresentationPort(SDL_Window &window) noexcept : impl_(std::make_shared<Impl>(window)) {}

    /** @copydoc SdlMetalPresentationPort::~SdlMetalPresentationPort */
    SdlMetalPresentationPort::~SdlMetalPresentationPort() {
        DestroySurface();
    }

    /** @copydoc SdlMetalPresentationPort::CreateSurface */
    Result<void> SdlMetalPresentationPort::CreateSurface() {
        if (!impl_->OnOwnerThread()) {
            return Result<void>::Failure(MakeError(Render::RenderPresentationErrors::ThreadAffinityViolation));
        }
        if (SDL_GetHintBoolean(SDL_HINT_VIDEO_METAL_AUTO_RESIZE_DRAWABLE, true)) {
            return Result<void>::Failure(
                MakeMetalSurfaceError("render.metal.external_resize_policy",
                                      "The host must disable SDL Metal automatic drawable resize before creating its view."));
        }
        if (impl_->view != nullptr) {
            return Result<void>::Failure(
                MakeMetalSurfaceError("render.metal.surface_exists", "An SDL Metal surface attachment is already retained."));
        }

        impl_->view = SDL_Metal_CreateView(impl_->window);
        if (impl_->view == nullptr) {
            return Result<void>::Failure(
                MakeMetalSurfaceError("render.metal.view_creation_failed", std::string{"SDL_Metal_CreateView failed: "} + SDL_GetError()));
        }

        impl_->layer = (__bridge CAMetalLayer *)SDL_Metal_GetLayer(impl_->view);
        if (impl_->layer == nil) {
            SDL_Metal_DestroyView(impl_->view);
            impl_->view = nullptr;
            return Result<void>::Failure(
                MakeMetalSurfaceError("render.metal.layer_unavailable", "SDL Metal view did not expose a CAMetalLayer."));
        }
        impl_->nativeWindow = (__bridge NSWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(impl_->window),
                                                                          SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
        if (impl_->nativeWindow == nil) {
            DestroySurface();
            return Result<void>::Failure(MakeError(Render::RenderPresentationErrors::OutputUnavailable));
        }
        impl_->active = true;
        impl_->RefreshDisplayFacts();
        impl_->ObserveDisplay(impl_);
        return Result<void>::Success();
    }

    /** @copydoc SdlMetalPresentationPort::OutputDisplayFacts */
    Result<Render::RenderOutputDisplayFacts> SdlMetalPresentationPort::OutputDisplayFacts() const {
        if (!impl_->OnOwnerThread()) {
            return Result<Render::RenderOutputDisplayFacts>::Failure(MakeError(Render::RenderPresentationErrors::ThreadAffinityViolation));
        }
        if (!impl_->active || impl_->revisionExhausted || !impl_->facts.IsValid()) {
            return Result<Render::RenderOutputDisplayFacts>::Failure(MakeError(Render::RenderPresentationErrors::OutputUnavailable));
        }
        return Result<Render::RenderOutputDisplayFacts>::Success(impl_->facts);
    }

    /** @copydoc SdlMetalPresentationPort::Layer */
    void *SdlMetalPresentationPort::Layer() const noexcept {
        return (__bridge void *)impl_->layer;
    }

    /** @copydoc SdlMetalPresentationPort::DestroySurface */
    void SdlMetalPresentationPort::DestroySurface() noexcept {
        if (impl_->view != nullptr && !impl_->OnOwnerThread()) {
            std::terminate();
        }
        impl_->StopObserving();
        impl_->layer = nil;
        if (impl_->view != nullptr) {
            SDL_Metal_DestroyView(impl_->view);
            impl_->view = nullptr;
        }
    }
}  // namespace Horo::Editor
