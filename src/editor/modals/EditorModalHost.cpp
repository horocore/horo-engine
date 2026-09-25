#include "Horo/Editor/EditorModalHost.h"

#include "Horo/Foundation/Logging/Logger.h"
#include "editor/EditorServiceErrors.h"

#include <algorithm>
#include <format>
#include <ranges>
#include <utility>

namespace Horo::Editor {
    using enum ModalHostError;

    namespace {
        [[nodiscard]] Error MakeModalHostError(ModalHostError error) {
            const ErrorCodeDescriptor *descriptor = &ModalErrors::InvalidModal;
            auto message = "The modal request is invalid.";
            switch (error) {
                case Busy:
                    descriptor = &ModalErrors::Busy;
                    message = "Another root modal is already active.";
                    break;
                case InvalidModal:
                    break;
                case DuplicateId:
                    descriptor = &ModalErrors::DuplicateId;
                    message = "The modal ID is already active.";
                    break;
                case ParentNotTop:
                    descriptor = &ModalErrors::ParentNotTop;
                    message = "Only the current top modal may push a child.";
                    break;
                case ModalNotTop:
                    descriptor = &ModalErrors::ModalNotTop;
                    message = "Only the current top modal may close.";
                    break;
                case StackLimitReached:
                    descriptor = &ModalErrors::StackLimitReached;
                    message = "The modal stack has reached its configured depth limit.";
                    break;
                case CloseDenied:
                    descriptor = &ModalErrors::CloseDenied;
                    message = "A modal denied the requested close.";
                    break;
            }
            return MakeError(*descriptor, message);
        }
    }  // namespace

    /** @copydoc EditorModalHost::EditorModalHost */
    EditorModalHost::EditorModalHost(EditorDataBus &events, Input::InputRouter &inputRouter, std::size_t maximumDepth)
        : m_events(events), m_inputRouter(inputRouter), m_context{.events = m_events, .modals = *this},
          m_maximumDepth(std::max<std::size_t>(maximumDepth, 1)) {}

    EditorModalHost::~EditorModalHost() {
        ForceDetachAllForShutdown();
    }

    /** @copydoc EditorModalHost::OpenRoot */
    Result<void> EditorModalHost::OpenRoot(std::unique_ptr<EditorModal> modal) {
        if (!m_acceptingRequests)
            return ErrorFor(Busy);
        if (HasOpenModal()) {
            LOG_WARN("editor.modal_host", "OpenRoot rejected: host is busy (top modal: %llu).", m_stack.back().modal->Id().Value());
            return ErrorFor(Busy);
        }
        if (!modal) {
            LOG_WARN("editor.modal_host", "OpenRoot rejected: null modal pointer.");
            return ErrorFor(InvalidModal);
        }
        if (const Result<void> validation = ValidateModalForPush(*modal); validation.HasError()) {
            LOG_WARN("editor.modal_host", "OpenRoot rejected for modal %llu: %s", modal->Id().Value(),
                     validation.ErrorValue().message.c_str());
            return validation;
        }
        LOG_INFO("editor.modal_host", "Opening root modal %llu.", modal->Id().Value());
        const ModalId id = modal->Id();
        m_stack.push_back(Entry{.modal = std::move(modal),
                                .inputContext = m_inputRouter.PushContext(Input::InputContextId{std::format("editor.modal.{}", id.Value())},
                                                                          Input::InputContextKind::ModalRoot)});
        return Result<void>::Success();
    }

    /** @copydoc EditorModalHost::PushChild */
    Result<void> EditorModalHost::PushChild(ModalId parentId, std::unique_ptr<EditorModal> modal) {
        if (!m_acceptingRequests)
            return ErrorFor(Busy);
        if (!modal)
            return ErrorFor(InvalidModal);
        if (m_stack.empty() || m_stack.back().modal->Id() != parentId)
            return ErrorFor(ParentNotTop);
        if (const Result<void> validation = ValidateModalForPush(*modal); validation.HasError())
            return validation;
        const ModalId id = modal->Id();
        m_pendingChildOpens.push_back(
            Entry{.modal = std::move(modal),
                  .inputContext = m_inputRouter.PushContext(Input::InputContextId{std::format("editor.modal.{}", id.Value())},
                                                            Input::InputContextKind::ModalChild)});

        return Result<void>::Success();
    }

    /** @copydoc EditorModalHost::RequestClose */
    Result<void> EditorModalHost::RequestClose(ModalId modalId, ModalCloseReason reason) {
        if (m_stack.empty() || m_stack.back().modal->Id() != modalId) {
            LOG_WARN("editor.modal_host", "RequestClose rejected: modal %llu is not the top of stack.", modalId.Value());
            return ErrorFor(ModalNotTop);
        }
        if (reason == ModalCloseReason::ApplicationShutdown && !m_stack.back().modal->ClosePolicy().allowApplicationShutdown)
            return ErrorFor(CloseDenied);
        if (m_stack.back().opened && m_stack.back().modal->CanClose(reason) != CloseDecision::Allow) {
            LOG_DEBUG("editor.modal_host", "RequestClose for modal %llu denied by CanClose (reason %d).", modalId.Value(),
                      static_cast<int>(reason));
            return ErrorFor(CloseDenied);
        }
        LOG_DEBUG("editor.modal_host", "Queuing close for modal %llu (reason %d).", modalId.Value(), static_cast<int>(reason));
        m_pendingCloseReasons.push_back(reason);
        return Result<void>::Success();
    }

    /** @copydoc EditorModalHost::HasOpenModal */
    bool EditorModalHost::HasOpenModal() const noexcept {
        return !m_stack.empty();
    }

    /** @copydoc EditorModalHost::TopModalId */
    std::optional<ModalId> EditorModalHost::TopModalId() const {
        if (m_stack.empty())
            return std::nullopt;
        return m_stack.back().modal->Id();
    }

    /** @copydoc EditorModalHost::TopModal */
    EditorModal *EditorModalHost::TopModal() {
        if (m_stack.empty())
            return nullptr;
        return m_stack.back().modal.get();
    }

    /** @copydoc EditorModalHost::InteractionScope */
    EditorInteractionScope EditorModalHost::InteractionScope() const noexcept {
        if (m_stack.empty())
            return {};
        return EditorInteractionScope{.kind = EditorInteractionScopeKind::Modal, .modalId = m_stack.back().modal->Id()};
    }

    /** @copydoc EditorModalHost::OnUpdate */
    void EditorModalHost::OnUpdate(float dt) {
        CommitPendingOpens();
        for (const Entry &entry : m_stack) {
            if (entry.opened)
                entry.modal->OnUpdate(dt);
        }
        CommitPendingCloses();
    }

    /** @copydoc EditorModalHost::Draw */
    void EditorModalHost::Draw() {
        CommitPendingOpens();
        if (!m_stack.empty() && m_stack.back().opened) {
            const ModalFrameResult result = m_stack.back().modal->Draw();
            if (const std::optional<ModalCloseReason> close = result.CloseRequest(); close.has_value())
                (void)RequestClose(m_stack.back().modal->Id(), *close);
        }
        CommitPendingCloses();
    }

    /** @copydoc EditorModalHost::RequestCloseAllForShutdown */
    Result<void> EditorModalHost::RequestCloseAllForShutdown() {
        using enum ModalHostError;
        if (m_shutdown)
            return Result<void>::Success();
        CommitPendingOpens();
        for (auto it = m_stack.rbegin(); it != m_stack.rend(); ++it) {
            if (!it->modal->ClosePolicy().allowApplicationShutdown ||
                (it->opened && it->modal->CanClose(ModalCloseReason::ApplicationShutdown) != CloseDecision::Allow))
                return ErrorFor(CloseDenied);
        }
        m_acceptingRequests = false;
        m_pendingCloseReasons.clear();
        m_pendingChildOpens.clear();
        while (!m_stack.empty())
            RemoveTop(ModalCloseReason::ApplicationShutdown);
        m_shutdown = true;
        return Result<void>::Success();
    }

    /** @copydoc EditorModalHost::ForceDetachAllForShutdown */
    void EditorModalHost::ForceDetachAllForShutdown() {
        if (m_shutdown)
            return;
        m_acceptingRequests = false;
        m_pendingCloseReasons.clear();
        m_pendingChildOpens.clear();
        while (!m_stack.empty()) {
            RemoveTop(ModalCloseReason::ApplicationShutdown);
        }
        m_shutdown = true;
    }

    Result<void> EditorModalHost::ValidateModalForPush(const EditorModal &modal) const {
        if (!modal.Id().IsValid())
            return ErrorFor(InvalidModal);
        if (m_stack.size() + m_pendingChildOpens.size() >= m_maximumDepth)
            return ErrorFor(StackLimitReached);
        if (ContainsId(modal.Id()))
            return ErrorFor(DuplicateId);
        return Result<void>::Success();
    }

    Result<void> EditorModalHost::ErrorFor(ModalHostError error) const {
        return Result<void>::Failure(MakeModalHostError(error));
    }

    void EditorModalHost::CommitPendingOpens() {
        for (Entry &entry : m_pendingChildOpens) {
            m_stack.push_back(std::move(entry));
        }
        m_pendingChildOpens.clear();

        std::size_t index = 0;
        while (index < m_stack.size()) {
            Entry &entry = m_stack[index];
            if (entry.opened) {
                ++index;
                continue;
            }
            if (const Result<void> result = entry.modal->OnOpen(m_context); result.HasError()) {
                LOG_ERROR("editor.modal_host", "Modal %llu OnOpen failed (%s: %s) — removing from stack.", entry.modal->Id().Value(),
                          result.ErrorValue().code.Value().c_str(), result.ErrorValue().message.c_str());
                entry.modal->OnClose(ModalCloseReason::Cancelled);
                m_stack.erase(m_stack.begin() + static_cast<std::ptrdiff_t>(index));
                continue;
            }
            LOG_DEBUG("editor.modal_host", "Modal %llu opened successfully.", entry.modal->Id().Value());
            entry.opened = true;
            ++index;
        }
    }

    void EditorModalHost::CommitPendingCloses() {
        while (!m_pendingCloseReasons.empty() && !m_stack.empty()) {
            const ModalCloseReason reason = m_pendingCloseReasons.back();
            m_pendingCloseReasons.pop_back();
            RemoveTop(reason);
        }
        if (m_stack.empty())
            m_pendingCloseReasons.clear();
    }

    void EditorModalHost::RemoveTop(ModalCloseReason reason) {
        const Entry &entry = m_stack.back();
        LOG_INFO("editor.modal_host", "Closing modal %llu (reason %d).", entry.modal->Id().Value(), static_cast<int>(reason));
        entry.modal->OnClose(reason);
        m_stack.pop_back();
    }

    bool EditorModalHost::ContainsId(ModalId id) const noexcept {
        const auto hasId = [id](const Entry &entry) {
            return entry.modal->Id() == id;
        };
        return std::ranges::any_of(m_stack, hasId) || std::ranges::any_of(m_pendingChildOpens, hasId);
    }
}  // namespace Horo::Editor
