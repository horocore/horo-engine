#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorModalHost.h"

#include <catch2/catch_test_macros.hpp>
#include <memory>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    struct ModalStats {
        EditorModalContext *context = nullptr;
        int openCalls = 0;
        int updateCalls = 0;
        int drawCalls = 0;
        int closeCalls = 0;
        bool requestCloseOnUpdate = false;
        bool requestCloseOnDraw = false;
        bool remainedOpenAfterUpdateCloseRequest = false;
        bool allowShutdown = true;
        ModalCloseReason closeReason = ModalCloseReason::Cancelled;
    };

    class RecordingModal final : public EditorModal {
    public:
        RecordingModal(std::uint64_t id, ModalStats &stats) : m_id(ModalId{id}), m_stats(stats) {}

        [[nodiscard]] ModalId Id() const override {
            return m_id;
        }

        [[nodiscard]] ModalPresentation Presentation() const override {
            return {};
        }

        [[nodiscard]] ModalClosePolicy ClosePolicy() const override {
            return {};
        }

        Result<void> OnOpen(EditorModalContext &context) override {
            m_stats.context = &context;
            ++m_stats.openCalls;
            return Result<void>::Success();
        }

        void OnUpdate(float) override {
            ++m_stats.updateCalls;
            if (m_stats.requestCloseOnUpdate) {
                const Result<void> result = m_stats.context->modals.RequestClose(m_id, ModalCloseReason::Completed);
                REQUIRE((result.HasValue()));
                m_stats.remainedOpenAfterUpdateCloseRequest = m_stats.context->modals.HasOpenModal();
            }
        }

        ModalFrameResult Draw() override {
            ++m_stats.drawCalls;
            return m_stats.requestCloseOnDraw ? ModalFrameResult::RequestClose(ModalCloseReason::Cancelled) : ModalFrameResult::None();
        }

        [[nodiscard]] CloseDecision CanClose(ModalCloseReason reason) override {
            return reason == ModalCloseReason::ApplicationShutdown && !m_stats.allowShutdown ? CloseDecision::Deny : CloseDecision::Allow;
        }

        void OnClose(ModalCloseReason reason) override {
            ++m_stats.closeCalls;
            m_stats.closeReason = reason;
        }

    private:
        ModalId m_id;
        ModalStats &m_stats;
    };

    struct WorkspaceCaptureOwner final : Input::IInputCaptureOwner {
        explicit WorkspaceCaptureOwner(const ModalStats &stats) : stats(stats) {}

        void OnInputCaptureCancelled(const Input::CaptureCancellationReason value) noexcept override {
            reason = value;
            cancelledBeforeModalOpen = stats.openCalls == 0;
        }

        const ModalStats &stats;
        std::optional<Input::CaptureCancellationReason> reason;
        bool cancelledBeforeModalOpen{false};
    };

    TEST_CASE("Modal activation cancels workspace capture before open and blocks close-frame input", "[unit][editor][input]") {
        Input::InputService input;
        input.BeginFrame(1);
        input.Collector().SetKey(Input::Key::A, true);
        static_cast<void>(input.CommitFrame());
        auto workspace = input.Router().PushContext(Input::InputContextId{"workspace"}, Input::InputContextKind::EditorWorkspace);
        EditorDataBus events;
        ModalStats stats;
        EditorModalHost host(events, input.Router());
        WorkspaceCaptureOwner owner(stats);
        auto capture = input.Router().CapturePointer(workspace, Input::PointerButton::Primary, owner);
        REQUIRE(capture.HasValue());

        REQUIRE(host.OpenRoot(std::make_unique<RecordingModal>(1, stats)).HasValue());
        REQUIRE(owner.reason == Input::CaptureCancellationReason::ModalOpened);
        REQUIRE(owner.cancelledBeforeModalOpen);
        REQUIRE_FALSE(input.Router().ConsumeKey(workspace, Input::Key::A));
        host.OnUpdate(0.0F);
        REQUIRE(stats.openCalls == 1);
        REQUIRE(host.RequestClose(ModalId{1}, ModalCloseReason::Cancelled).HasValue());
        host.OnUpdate(0.0F);
        REQUIRE_FALSE(input.Router().ConsumeKey(workspace, Input::Key::A));

        input.BeginFrame(2);
        input.Collector().SetKey(Input::Key::A, false);
        static_cast<void>(input.CommitFrame());
        REQUIRE(input.Router().IsContextActive(workspace));
    }

    TEST_CASE("Accepted Root Gates Interaction Before Its First Open Boundary", "[unit][editor]") {
        EditorDataBus events;
        Input::InputRouter input;
        ModalStats stats;
        EditorModalHost host(events, input);

        REQUIRE((host.OpenRoot(std::make_unique<RecordingModal>(1, stats)).HasValue()));
        REQUIRE((host.InteractionScope().kind == EditorInteractionScopeKind::Modal));
        REQUIRE((host.InteractionScope().modalId == ModalId{1}));
        REQUIRE((stats.openCalls == 0));

        host.OnUpdate(0.0F);
        REQUIRE((stats.openCalls == 1));
        REQUIRE((stats.context != nullptr));
        REQUIRE((&stats.context->events == &events));
        REQUIRE((&stats.context->modals == &host));
    }

    TEST_CASE("Second Root Is Rejected As Busy", "[unit][editor]") {
        EditorDataBus events;
        Input::InputRouter input;
        ModalStats firstStats;
        ModalStats secondStats;
        EditorModalHost host(events, input);
        REQUIRE((host.OpenRoot(std::make_unique<RecordingModal>(1, firstStats)).HasValue()));

        const Result<void> result = host.OpenRoot(std::make_unique<RecordingModal>(2, secondStats));
        REQUIRE((result.HasError()));
        REQUIRE((result.ErrorValue().code.Value() == "editor.modal_host.busy"));
    }

    TEST_CASE("Close Requested During Update Is Deferred Until The Frame Boundary", "[unit][editor]") {
        EditorDataBus events;
        Input::InputRouter input;
        ModalStats stats{.requestCloseOnUpdate = true};
        EditorModalHost host(events, input);

        REQUIRE((host.OpenRoot(std::make_unique<RecordingModal>(1, stats)).HasValue()));
        host.OnUpdate(0.0F);

        REQUIRE((stats.openCalls == 1));
        REQUIRE((stats.remainedOpenAfterUpdateCloseRequest));
        REQUIRE((stats.closeCalls == 1));
        REQUIRE((!host.HasOpenModal()));
    }

    TEST_CASE("Close Requested During Draw Is Deferred Until The Draw Boundary", "[unit][editor]") {
        EditorDataBus events;
        Input::InputRouter input;
        ModalStats stats{.requestCloseOnDraw = true};
        EditorModalHost host(events, input);

        REQUIRE((host.OpenRoot(std::make_unique<RecordingModal>(1, stats)).HasValue()));
        host.OnUpdate(0.0F);
        host.Draw();

        REQUIRE((stats.openCalls == 1));
        REQUIRE((stats.drawCalls == 1));
        REQUIRE((stats.closeCalls == 1));
    }

    TEST_CASE("Top And Scope Restore Only After Deferred Close Boundary", "[unit][editor]") {
        EditorDataBus events;
        Input::InputRouter input;
        ModalStats stats;
        EditorModalHost host(events, input);
        REQUIRE((host.OpenRoot(std::make_unique<RecordingModal>(1, stats)).HasValue()));
        host.OnUpdate(0.0F);

        REQUIRE((host.RequestClose(ModalId{1}, ModalCloseReason::Cancelled).HasValue()));
        REQUIRE((host.TopModalId() == ModalId{1}));
        REQUIRE((host.InteractionScope().kind == EditorInteractionScopeKind::Modal));

        host.OnUpdate(0.0F);
        REQUIRE((!host.TopModalId().has_value()));
        REQUIRE((host.InteractionScope().kind == EditorInteractionScopeKind::Workspace));
    }

    TEST_CASE("Only Current Top Modal May Push A Child", "[unit][editor]") {
        EditorDataBus events;
        Input::InputRouter input;
        ModalStats rootStats;
        ModalStats rejectedStats;
        ModalStats childStats;
        EditorModalHost host(events, input);
        REQUIRE((host.OpenRoot(std::make_unique<RecordingModal>(1, rootStats)).HasValue()));
        host.OnUpdate(0.0F);

        const Result<void> invalid = host.PushChild(ModalId{2}, std::make_unique<RecordingModal>(3, rejectedStats));
        REQUIRE((invalid.HasError()));
        REQUIRE((invalid.ErrorValue().code.Value() == "editor.modal_host.parent_not_top"));

        REQUIRE((host.PushChild(ModalId{1}, std::make_unique<RecordingModal>(2, childStats)).HasValue()));
        host.OnUpdate(0.0F);
        REQUIRE((host.TopModalId() == ModalId{2}));
    }

    TEST_CASE("Child Push Commits At The Next Host Frame Boundary", "[unit][editor]") {
        EditorDataBus events;
        Input::InputRouter input;
        ModalStats rootStats;
        ModalStats childStats;
        EditorModalHost host(events, input);

        REQUIRE((host.OpenRoot(std::make_unique<RecordingModal>(1, rootStats)).HasValue()));
        host.OnUpdate(0.0F);
        REQUIRE((host.PushChild(ModalId{1}, std::make_unique<RecordingModal>(2, childStats)).HasValue()));

        REQUIRE((host.TopModalId() == ModalId{1}));
        REQUIRE((host.InteractionScope().modalId == ModalId{1}));
        REQUIRE((childStats.openCalls == 0));

        host.OnUpdate(0.0F);
        REQUIRE((host.TopModalId() == ModalId{2}));
        REQUIRE((host.InteractionScope().modalId == ModalId{2}));
        REQUIRE((childStats.openCalls == 1));
    }

    TEST_CASE("Force Detach Closes Each Modal Once And Does Not Repeat Callbacks", "[unit][editor]") {
        EditorDataBus events;
        Input::InputRouter input;
        ModalStats rootStats;
        ModalStats childStats;
        EditorModalHost host(events, input);

        REQUIRE((host.OpenRoot(std::make_unique<RecordingModal>(1, rootStats)).HasValue()));
        host.OnUpdate(0.0F);
        REQUIRE((host.PushChild(ModalId{1}, std::make_unique<RecordingModal>(2, childStats)).HasValue()));
        host.OnUpdate(0.0F);

        host.ForceDetachAllForShutdown();
        REQUIRE((rootStats.closeCalls == 1));
        REQUIRE((childStats.closeCalls == 1));
        REQUIRE((!host.HasOpenModal()));
        REQUIRE((host.InteractionScope().kind == EditorInteractionScopeKind::Workspace));

        host.ForceDetachAllForShutdown();
        REQUIRE((rootStats.closeCalls == 1));
        REQUIRE((childStats.closeCalls == 1));
    }

    TEST_CASE("Orderly Shutdown Is Synchronous Idempotent And Stops New Requests", "[unit][editor]") {
        EditorDataBus events;
        Input::InputRouter input;
        ModalStats stats;
        ModalStats rejected;
        EditorModalHost host(events, input);
        REQUIRE((host.OpenRoot(std::make_unique<RecordingModal>(1, stats)).HasValue()));
        host.OnUpdate(0.0F);

        REQUIRE((host.RequestCloseAllForShutdown().HasValue()));
        REQUIRE((stats.closeCalls == 1));
        REQUIRE((stats.closeReason == ModalCloseReason::ApplicationShutdown));
        REQUIRE((!host.HasOpenModal()));
        REQUIRE((host.RequestCloseAllForShutdown().HasValue()));
        REQUIRE((stats.closeCalls == 1));

        const Result<void> openAfterShutdown = host.OpenRoot(std::make_unique<RecordingModal>(2, rejected));
        REQUIRE((openAfterShutdown.HasError()));
        REQUIRE((openAfterShutdown.ErrorValue().code.Value() == "editor.modal_host.busy"));
    }

    TEST_CASE("Denied Shutdown Keeps The Modal And Host Usable", "[unit][editor]") {
        EditorDataBus events;
        Input::InputRouter input;
        ModalStats stats{.allowShutdown = false};
        EditorModalHost host(events, input);
        REQUIRE((host.OpenRoot(std::make_unique<RecordingModal>(1, stats)).HasValue()));
        host.OnUpdate(0.0F);

        const Result<void> shutdown = host.RequestCloseAllForShutdown();
        REQUIRE((shutdown.HasError()));
        REQUIRE((shutdown.ErrorValue().code.Value() == "editor.modal_host.close_denied"));
        REQUIRE((host.HasOpenModal()));
        REQUIRE((stats.closeCalls == 0));
        REQUIRE((host.RequestClose(ModalId{1}, ModalCloseReason::Cancelled).HasValue()));
        host.OnUpdate(0.0F);
        REQUIRE((stats.closeCalls == 1));
    }
}  // namespace
