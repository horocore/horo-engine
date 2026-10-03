#pragma once

/** @file PropertyTrackRuntime.h
 * @brief Scene-fenced property binding lifecycle and mandatory diagnostic delivery. */

#include "Horo/Cinematic/PropertyTrack.h"
#include "Horo/Foundation/DataBus.h"
#include "Horo/Foundation/DiagnosticsEngine.h"

namespace Horo::Cinematic {
    /** @brief Owner boundary at which binding evidence was produced. */
    enum class PropertyDiagnosticStage : std::uint8_t {
        Activation,
        Revalidation,
        Application
    };

    /** @brief Structured runtime notification; owns errors and never retains component storage. */
    struct PropertyBindingDiagnosticEvent final {
        static constexpr std::string_view HoroEventTypeName = "horo.cinematic.PropertyBindingDiagnosticEvent";
        SequenceId sequence;
        PropertySceneVersion scene;
        PropertyDiagnosticStage stage{PropertyDiagnosticStage::Activation};
        PropertyEvaluationDiagnostic diagnostic;
    };

    /** @brief Typed synchronous diagnostic consumer; Publish must not throw or reenter the controller.
     * @note Dispatch occurs only when reporting evidence, outside successful frame evaluation. */
    class IPropertyDiagnosticSink {
    public:
        /** @brief Releases a host-owned consumer. */
        virtual ~IPropertyDiagnosticSink() = default;
        /** @brief Receives owned binding evidence synchronously. @param event Failure to surface. */
        virtual void Publish(const PropertyBindingDiagnosticEvent &event) = 0;
    };

    /** @brief Mandatory borrowed reporting seam; the consumer must outlive its controller. */
    struct PropertyDiagnosticSink final {
        IPropertyDiagnosticSink *consumer{};
    };

    /** @brief Runtime projection into process diagnostic history, structured telemetry and typed notifications.
     * @note The host owns this adapter and its borrowed services until all controllers using Sink() are destroyed. */
    class RuntimePropertyDiagnosticSink final : public IPropertyDiagnosticSink {
    public:
        /** @brief Binds process-owned diagnostic services. @param diagnostics Diagnostic history. @param bus Notification bus. */
        RuntimePropertyDiagnosticSink(DiagnosticsEngine &diagnostics, EngineDataBus &bus) noexcept;
        RuntimePropertyDiagnosticSink(const RuntimePropertyDiagnosticSink &) = delete;
        RuntimePropertyDiagnosticSink &operator=(const RuntimePropertyDiagnosticSink &) = delete;
        RuntimePropertyDiagnosticSink(RuntimePropertyDiagnosticSink &&) = delete;
        RuntimePropertyDiagnosticSink &operator=(RuntimePropertyDiagnosticSink &&) = delete;
        /** @brief Returns the borrowed controller reporting seam. @return Sink backed by this adapter. */
        [[nodiscard]] PropertyDiagnosticSink Sink() noexcept;
        /** @brief Publishes owned typed evidence after the owner operation. @param event Binding failure to surface. */
        void Publish(const PropertyBindingDiagnosticEvent &event) override;

    private:
        DiagnosticsEngine &diagnostics_;
        EngineDataBus &bus_;
    };

    /**
     * @brief Owner-thread property playback adapter using the shared scene registry and immutable borrowed curves.
     * @note Create/Rebind are lifecycle boundaries and may allocate. Successful Evaluate allocates nothing.
     * The host supplies snapshots after scene lifecycle commit; components are borrowed only during Evaluate.
     * Registry, curve keys and diagnostic sink must outlive the controller. No accessor runs during activation/rebinding.
     */
    class PropertyTrackRuntime final {
    public:
        /**
         * @brief Inspects every binding, surfaces failures and admits the plan only if required bindings resolve.
         * @param sequence Durable source sequence identity.
         * @param context Current scene and owner-issued target snapshot.
         * @param tracks Borrowed authored tracks copied into bounded lifecycle storage.
         * @param registry Frozen shared inspector/runtime property registry.
         * @param sink Mandatory host diagnostic consumer.
         * @return Controller or typed activation error; all discovered binding failures are published before rejection.
         */
        [[nodiscard]] static Result<PropertyTrackRuntime> Create(SequenceId sequence, const PropertyEvaluationContext &context,
                                                                 std::span<const PropertyTrackDescriptor> tracks,
                                                                 const Runtime::PropertyBindingRegistry &registry,
                                                                 PropertyDiagnosticSink sink);
        /**
         * @brief Invalidates old bindings before resolving a replacement scene or component snapshot.
         * @param context Committed replacement snapshot.
         * @return Success or typed required-binding failure; failure leaves the old plan invalid and cannot write old storage.
         * @note Old stale outcomes and new missing/incompatible outcomes are independently surfaced. Rebind never replays events.
         */
        [[nodiscard]] Result<void> Rebind(const PropertyEvaluationContext &context);
        /**
         * @brief Samples and applies through owner setters, publishing every failed binding with sequence/track context.
         * @param time Random-access sample time.
         * @param context Current owner-boundary snapshot.
         * @return Counts or typed failure; repeated identical binding failures are deduplicated until recovery/rebind.
         */
        [[nodiscard]] Result<PropertyEvaluationResult> Evaluate(CurveTime time, const PropertyEvaluationContext &context);
        /** @brief Reports whether required bindings currently admit evaluation. @return True for a live plan. */
        [[nodiscard]] bool IsActive() const noexcept;

    private:
        PropertyTrackRuntime(SequenceId sequence, std::vector<PropertyTrackDescriptor> tracks,
                             const Runtime::PropertyBindingRegistry &registry, PropertyDiagnosticSink sink);
        [[nodiscard]] Result<void> Activate(const PropertyEvaluationContext &context);
        void Publish(PropertyDiagnosticStage stage, PropertySceneVersion scene, std::span<const PropertyEvaluationDiagnostic> diagnostics);
        void PublishFailure(PropertyDiagnosticStage stage, PropertySceneVersion scene, const Error &error);

        SequenceId sequence_;
        std::vector<PropertyTrackDescriptor> tracks_;
        const Runtime::PropertyBindingRegistry *registry_;
        PropertyDiagnosticSink sink_;
        std::optional<PropertyEvaluationPlan> plan_;
        std::vector<PropertyEvaluationValue> values_;
        std::vector<PropertyEvaluationDiagnostic> diagnostics_;
        std::vector<std::optional<PropertyEvaluationDiagnostic>> reported_;
    };
}  // namespace Horo::Cinematic
