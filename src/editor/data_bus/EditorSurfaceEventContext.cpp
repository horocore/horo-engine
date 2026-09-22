#include "Horo/Editor/EditorSurfaceEventContext.h"

#include <algorithm>
#include <atomic>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace Horo::Editor {
    namespace {
        const ErrorDomainId SurfaceEventDomain{"horo.editor.surface_events"};
        const ErrorCodeDescriptor ContextClosed{
            .domain = SurfaceEventDomain,
            .code = ErrorCode{"surface_event_context_closed"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "The editor surface event context is closed.",
            .remediationHint = "Reacquire the context for the current provider activation.",
            .retryable = false,
            .userActionable = false,
        };
        const ErrorCodeDescriptor EventNotAllowed{
            .domain = SurfaceEventDomain,
            .code = ErrorCode{"surface_event_not_allowed"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "The editor surface event was not admitted by the host.",
            .remediationHint = "Request the event through the immutable surface descriptor.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor SubscriptionLimitExceeded{
            .domain = SurfaceEventDomain,
            .code = ErrorCode{"surface_event_subscription_limit"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "The editor surface event subscription limit was reached.",
            .remediationHint = "Release an existing subscription before requesting another one.",
            .retryable = true,
            .userActionable = true,
        };
        const ErrorCodeDescriptor InvalidEvent{
            .domain = SurfaceEventDomain,
            .code = ErrorCode{"surface_event_invalid"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "The editor surface event identity is invalid.",
            .remediationHint = "Use an event identity from the admitted host contract.",
            .retryable = false,
            .userActionable = true,
        };

        /** @brief Converts a context admission descriptor into the public subscription result type. */
        [[nodiscard]] Result<Subscription> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<Subscription>::Failure(MakeError(descriptor));
        }
    }  // namespace

    struct EditorSurfaceEventContext::Slot final {
        std::weak_ptr<EditorSurfaceEventContext::State> owner;
        Subscription busSubscription;

        void Revoke() noexcept {
            busSubscription.Reset();
        }
    };

    struct EditorSurfaceEventContext::State final {
        EditorDataBus &editorEvents;
        EditorSurfaceProviderOwnership provider;
        EditorSurfaceEventContextLimits limits;
        std::array<bool, static_cast<std::size_t>(EditorEventKind::Count)> allowed{};
        std::vector<std::shared_ptr<Slot>> slots;
        std::atomic<std::size_t> activeSubscriptions{};
        std::atomic<std::size_t> acceptedSubscriptions{};
        std::atomic<std::size_t> rejectedSubscriptions{};
        std::atomic<std::uint64_t> deliveredEvents{};
        std::atomic<bool> closed{false};

        State(EditorDataBus &editor, EditorSurfaceProviderOwnership owner, const std::span<const EditorEventKind> allowedEvents,
              const EditorSurfaceEventContextLimits contextLimits)
            : editorEvents(editor), provider(std::move(owner)), limits(contextLimits) {
            for (const EditorEventKind kind : allowedEvents) {
                if (IsKnownEditorEventKind(kind))
                    allowed[static_cast<std::size_t>(kind)] = true;
            }
            if (!provider.IsValid())
                closed.store(true, std::memory_order_release);
        }

        [[nodiscard]] bool IsAllowed(const EditorEventKind kind) const noexcept {
            return IsKnownEditorEventKind(kind) && allowed[static_cast<std::size_t>(kind)];
        }

        void Remove(const Slot *slot) noexcept {
            const auto found = std::ranges::find_if(slots, [slot](const std::shared_ptr<Slot> &candidate) {
                return candidate.get() == slot;
            });
            if (found == slots.end())
                return;
            slots.erase(found);
            activeSubscriptions.fetch_sub(1, std::memory_order_relaxed);
        }

        void Close() noexcept {
            if (closed.exchange(true, std::memory_order_acq_rel))
                return;
            for (const std::shared_ptr<Slot> &slot : slots)
                slot->Revoke();
            slots.clear();
            activeSubscriptions.store(0, std::memory_order_release);
        }

        void Deliver(const EditorEventKind kind, EditorSurfaceEventPayload payload, const EditorSurfaceEventHandler &handler) {
            if (closed.load(std::memory_order_acquire))
                return;
            handler(EditorSurfaceEvent{.kind = kind, .payload = std::move(payload)});
            deliveredEvents.fetch_add(1, std::memory_order_relaxed);
        }

        template <typename EventT>
        [[nodiscard]] bool AddTyped(const EditorEventKind kind, const std::shared_ptr<Slot> &slot, const std::weak_ptr<State> &weakState,
                                    const EditorSurfaceEventHandler &handler) {
            slot->busSubscription = editorEvents.Subscribe<EventT>([weakState, kind, handler](const EventT &event) {
                if (const auto state = weakState.lock())
                    state->Deliver(kind, EditorSurfaceEventPayload{event}, handler);
            });
            return static_cast<bool>(slot->busSubscription);
        }

        [[nodiscard]] bool AddSubscription(const EditorEventKind kind, const std::shared_ptr<Slot> &slot,
                                           const std::weak_ptr<State> &weakState, const EditorSurfaceEventHandler &handler) {
            switch (kind) {
                case EditorEventKind::ProjectOpened:
                    return AddTyped<EditorProjectOpenedEvent>(kind, slot, weakState, handler);
                case EditorEventKind::ProjectClosed:
                    return AddTyped<EditorProjectClosedEvent>(kind, slot, weakState, handler);
                case EditorEventKind::AssetImported:
                    return AddTyped<EditorAssetImportedEvent>(kind, slot, weakState, handler);
                case EditorEventKind::AssetReloaded:
                    return AddTyped<EditorAssetReloadedEvent>(kind, slot, weakState, handler);
                case EditorEventKind::OperationStoreRevisionChanged:
                    return AddTyped<EditorOperationStoreRevisionChangedEvent>(kind, slot, weakState, handler);
                case EditorEventKind::ConsoleLog:
                    return AddTyped<EditorConsoleLogEvent>(kind, slot, weakState, handler);
                case EditorEventKind::MetricsChanged:
                    return AddTyped<EditorMetricsChangedEvent>(kind, slot, weakState, handler);
                case EditorEventKind::ProfilerCaptureState:
                    return AddTyped<EditorProfilerCaptureStateEvent>(kind, slot, weakState, handler);
                case EditorEventKind::McpToolInvocation:
                    return AddTyped<EditorMcpToolInvocationEvent>(kind, slot, weakState, handler);
                case EditorEventKind::Count:
                    return false;
            }
            return false;
        }

        [[nodiscard]] Result<Subscription> Reject(const ErrorCodeDescriptor &descriptor) {
            rejectedSubscriptions.fetch_add(1, std::memory_order_relaxed);
            return Failure(descriptor);
        }

        [[nodiscard]] Result<Subscription> Subscribe(const EditorEventKind kind, EditorSurfaceEventHandler handler,
                                                     const std::shared_ptr<State> &self) {
            if (closed.load(std::memory_order_acquire))
                return Reject(ContextClosed);
            if (!IsKnownEditorEventKind(kind))
                return Reject(InvalidEvent);
            if (!IsAllowed(kind))
                return Reject(EventNotAllowed);
            if (activeSubscriptions.load(std::memory_order_relaxed) >= limits.maximumSubscriptions)
                return Reject(SubscriptionLimitExceeded);

            auto slot = std::make_shared<Slot>();
            slot->owner = self;
            const std::weak_ptr<State> weakState = self;
            if (!AddSubscription(kind, slot, weakState, handler))
                return Reject(SubscriptionLimitExceeded);

            slots.push_back(slot);
            activeSubscriptions.fetch_add(1, std::memory_order_relaxed);
            acceptedSubscriptions.fetch_add(1, std::memory_order_relaxed);
            const std::weak_ptr<Slot> weakSlot = slot;
            return Result<Subscription>::Success(Subscription::Adopt([weakSlot] {
                if (const auto lockedSlot = weakSlot.lock()) {
                    lockedSlot->Revoke();
                    // The state owns the vector entry; the local shared pointer keeps the slot
                    // alive until Remove has finished erasing it.
                    const std::weak_ptr<EditorSurfaceEventContext::State> owner = lockedSlot->owner;
                    if (const auto lockedState = owner.lock())
                        lockedState->Remove(lockedSlot.get());
                }
            }));
        }
    };

    /** @copydoc EditorSurfaceEventContext::EditorSurfaceEventContext */
    EditorSurfaceEventContext::EditorSurfaceEventContext(EditorDataBus &editorEvents, EditorSurfaceProviderOwnership provider,
                                                         const std::span<const EditorEventKind> allowedEvents,
                                                         const EditorSurfaceEventContextLimits limits)
        : m_state(std::make_shared<State>(editorEvents, std::move(provider), allowedEvents, limits)) {}

    /** @copydoc EditorSurfaceEventContext::~EditorSurfaceEventContext */
    EditorSurfaceEventContext::~EditorSurfaceEventContext() {
        Close();
    }

    /** @copydoc EditorSurfaceEventContext::EditorSurfaceEventContext */
    EditorSurfaceEventContext::EditorSurfaceEventContext(EditorSurfaceEventContext &&other) noexcept = default;

    /** @copydoc EditorSurfaceEventContext::operator= */
    EditorSurfaceEventContext &EditorSurfaceEventContext::operator=(EditorSurfaceEventContext &&other) noexcept {
        if (this != &other) {
            Close();
            m_state = std::move(other.m_state);
        }
        return *this;
    }

    /** @copydoc EditorSurfaceEventContext::Subscribe */
    Result<Subscription> EditorSurfaceEventContext::Subscribe(const EditorEventKind kind, EditorSurfaceEventHandler handler) {
        if (m_state == nullptr)
            return Failure(ContextClosed);
        return m_state->Subscribe(kind, std::move(handler), m_state);
    }

    /** @copydoc EditorSurfaceEventContext::Close */
    void EditorSurfaceEventContext::Close() noexcept {
        if (m_state != nullptr)
            m_state->Close();
    }

    /** @copydoc EditorSurfaceEventContext::IsClosed */
    bool EditorSurfaceEventContext::IsClosed() const noexcept {
        return m_state == nullptr || m_state->closed.load(std::memory_order_acquire);
    }

    /** @copydoc EditorSurfaceEventContext::Stats */
    EditorSurfaceEventContextStats EditorSurfaceEventContext::Stats() const noexcept {
        if (m_state == nullptr)
            return EditorSurfaceEventContextStats{.closed = true};
        return EditorSurfaceEventContextStats{
            .activeSubscriptions = m_state->activeSubscriptions.load(std::memory_order_relaxed),
            .acceptedSubscriptions = m_state->acceptedSubscriptions.load(std::memory_order_relaxed),
            .rejectedSubscriptions = m_state->rejectedSubscriptions.load(std::memory_order_relaxed),
            .deliveredEvents = m_state->deliveredEvents.load(std::memory_order_relaxed),
            .closed = m_state->closed.load(std::memory_order_acquire),
        };
    }

    /** @copydoc EditorSurfaceEventContext::ProviderOwnership */
    EditorSurfaceProviderOwnership EditorSurfaceEventContext::ProviderOwnership() const {
        return m_state == nullptr ? EditorSurfaceProviderOwnership{} : m_state->provider;
    }
}  // namespace Horo::Editor
