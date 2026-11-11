#pragma once

/** @file AccessibilityBridge.h
 * @brief Optional platform accessibility transport over the existing Runtime UI semantic owner.
 */
#include <cstddef>
#include <cstdint>

namespace Horo::Runtime::Ui {
    class UiAccessibilitySnapshot;
    class UiAccessibilityChangePublisher;
    struct UiAccessibilityActionRequest;
    struct UiPresentationReceipt;
}  // namespace Horo::Runtime::Ui

namespace Horo::Platform {
    /** @brief Actual native availability, independent of preference and semantic Runtime UI availability. */
    enum class AccessibilityBridgeCapability : std::uint8_t {
        Supported,
        Unavailable,
        Unsupported
    };
    /** @brief Bounded synchronous admission outcome; Accepted never means native completion. */
    enum class AccessibilityEnqueueResult : std::uint8_t {
        Accepted,
        QueueFull,
        StaleGeneration,
        Unavailable,
        Unsupported,
        ShuttingDown
    };
    /** @brief Explicit native technology; None is used by the no-op adapter. */
    enum class AccessibilityTechnology : std::uint8_t {
        None,
        WindowsUiAutomation,
        MacOSAccessibility,
        AtSpi2
    };

    /** @brief Revisioned native capability and queue budget; unsupported adapters advertise no native schema or queue. */
    struct AccessibilityBridgeDescriptor final {
        AccessibilityBridgeCapability capability{AccessibilityBridgeCapability::Unsupported};
        AccessibilityTechnology technology{AccessibilityTechnology::None};
        std::uint64_t sessionGeneration{};
        std::uint32_t minimumSemanticSchema{}, maximumSemanticSchema{};
        std::size_t maximumQueuedMessages{}, maximumQueuedBytes{};
    };

    /**
     * @brief Platform-owned asynchronous transport; UI retains node/focus/action authority.
     * @details Calls are serialized on the composing owner thread. A native implementation
     * copies accepted call-duration data into bounded owned storage and dispatches at its
     * native thread/run-loop boundary, never waiting in audio/input/render/UI submission.
     * It admits native visibility only for a successful receipt matching the snapshot's
     * canvas/interaction generation. Native actions are returned to the existing UI owner
     * for normal generation, audience, visibility, permission and modal-scope validation.
     * Overflow requires bounded resynchronization; it never acknowledges accepted speech.
     * The host closes admission, detaches callbacks, and retains the adapter through native
     * retirement. Null and recording implementations never qualify native support.
     */
    class IPlatformAccessibilityBridge {
    public:
        virtual ~IPlatformAccessibilityBridge() = default;
        IPlatformAccessibilityBridge(const IPlatformAccessibilityBridge &) = delete;
        IPlatformAccessibilityBridge &operator=(const IPlatformAccessibilityBridge &) = delete;
        /** @brief Returns actual native capability. @return Copied capability/generation/budget descriptor. */
        [[nodiscard]] virtual AccessibilityBridgeDescriptor Descriptor() const noexcept = 0;
        /** @brief Admits a presentation-correlated semantic publication without native waits.
         * @param snapshot Borrowed immutable UI semantics for the presented interaction.
         * @param changes Borrowed corresponding change/retained-announcement producer.
         * @param receipt Exact successful presentation evidence; native adapters reject stale/failed/skipped receipts.
         * @return Explicit admission result. No borrowed reference may survive this call.
         */
        [[nodiscard]] virtual AccessibilityEnqueueResult Submit(const Runtime::Ui::UiAccessibilitySnapshot &snapshot,
                                                                const Runtime::Ui::UiAccessibilityChangePublisher &changes,
                                                                const Runtime::Ui::UiPresentationReceipt &receipt) noexcept = 0;
        /** @brief Dequeues one copied native action for validation by the sole UI authority.
         * @param action Destination written only on success.
         * @return True when a bounded action was copied; false leaves action unchanged.
         */
        [[nodiscard]] virtual bool TryDequeueAction(Runtime::Ui::UiAccessibilityActionRequest &action) noexcept = 0;
        /** @brief Idempotently closes admission and cancels pending work; native retirement remains host-owned. */
        virtual void Shutdown() noexcept = 0;

    protected:
        IPlatformAccessibilityBridge() = default;
    };

    /** @brief Explicit no-op adapter: Unsupported, zero native effects, zero queues and zero diagnostic recording. */
    class NullPlatformAccessibilityBridge final : public IPlatformAccessibilityBridge {
    public:
        /** @copydoc IPlatformAccessibilityBridge::Descriptor */
        [[nodiscard]] AccessibilityBridgeDescriptor Descriptor() const noexcept override;
        /** @copydoc IPlatformAccessibilityBridge::Submit */
        [[nodiscard]] AccessibilityEnqueueResult Submit(const Runtime::Ui::UiAccessibilitySnapshot &snapshot,
                                                        const Runtime::Ui::UiAccessibilityChangePublisher &changes,
                                                        const Runtime::Ui::UiPresentationReceipt &receipt) noexcept override;
        /** @copydoc IPlatformAccessibilityBridge::TryDequeueAction */
        [[nodiscard]] bool TryDequeueAction(Runtime::Ui::UiAccessibilityActionRequest &action) noexcept override;
        /** @copydoc IPlatformAccessibilityBridge::Shutdown */
        void Shutdown() noexcept override;

    private:
        bool stopped_{};
    };
}  // namespace Horo::Platform
