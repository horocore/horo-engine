#pragma once

#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/Localization/LocalizedGui.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Input.h"

#include <memory>
#include <optional>
#include <vector>

namespace Horo::Editor {
    class EditorModalHost;

    namespace GuiLive {
        class IRegistry;
    }

    /** @brief Validated stable identity for an editor modal in the active stack. */
    class ModalId {
    public:
        constexpr explicit ModalId(std::uint64_t value) noexcept : m_value(value) {}

        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return m_value != 0;
        }

        [[nodiscard]] constexpr std::uint64_t Value() const noexcept {
            return m_value;
        }

        [[nodiscard]] friend constexpr bool operator==(ModalId, ModalId) noexcept = default;

    private:
        std::uint64_t m_value = 0;
    };

    /** @brief Source of an editor modal close request. */
    enum class ModalCloseReason : std::uint8_t {
        Cancelled,
        Completed,
        ApplicationShutdown
    };
    /** @brief Decision supplied by a modal before the host removes it. */
    enum class CloseDecision : std::uint8_t {
        Allow,
        Deny,
        RequireChildConfirmation
    };
    /** @brief Machine-readable modal-host operation failures. */
    enum class ModalHostError : std::uint8_t {
        Busy,
        InvalidModal,
        DuplicateId,
        ParentNotTop,
        ModalNotTop,
        StackLimitReached,
        CloseDenied
    };
    /** @brief Top-level interaction owner while the editor GUI is active. */
    enum class EditorInteractionScopeKind : std::uint8_t {
        Workspace,
        Modal,
        NativeDialog
    };
    /** @brief Token-driven layout size for a modal workflow surface. */
    enum class ModalSizePolicy : std::uint8_t {
        Compact,
        Medium,
        Large,
        Workspace
    };

    /** @brief Current exclusive interaction owner. */
    struct EditorInteractionScope {
        EditorInteractionScopeKind kind = EditorInteractionScopeKind::Workspace;
        std::optional<ModalId> modalId;
    };

    /** @brief Window-level presentation requested by a modal. */
    struct ModalPresentation {
        ModalSizePolicy size = ModalSizePolicy::Large;
        bool dimWorkspace = true;
    };

    /** @brief Modal close-source policy enforced jointly by the modal presentation and host lifecycle boundary. */
    struct ModalClosePolicy {
        bool allowCloseButton = true;
        bool allowEscape = true;
        bool allowOutsideClick = false;
        bool allowApplicationShutdown = true;
    };

    /** @brief Action requested by a modal's draw pass. */
    class ModalFrameResult {
    public:
        constexpr ModalFrameResult() noexcept = default;

        [[nodiscard]] static constexpr ModalFrameResult None() noexcept {
            return {};
        }

        [[nodiscard]] static constexpr ModalFrameResult RequestClose(ModalCloseReason reason) noexcept {
            return ModalFrameResult(reason);
        }

        [[nodiscard]] constexpr std::optional<ModalCloseReason> CloseRequest() const noexcept {
            return m_closeReason;
        }

    private:
        constexpr explicit ModalFrameResult(ModalCloseReason reason) noexcept : m_closeReason(reason) {}

        std::optional<ModalCloseReason> m_closeReason;
    };

    /** @brief Narrow modal lifecycle context; it intentionally exposes no general service locator. */
    struct EditorModalContext {
        EditorDataBus &events;
        EditorModalHost &modals;
    };

    /** @brief Headless-capable contract for one editor modal workflow surface. */
    class EditorModal : public LocalizedGui {
    protected:
        using LocalizedGui::LocalizedGui;

    public:
        virtual ~EditorModal() = default;
        [[nodiscard]] virtual ModalId Id() const = 0;
        [[nodiscard]] virtual ModalPresentation Presentation() const = 0;
        [[nodiscard]] virtual ModalClosePolicy ClosePolicy() const = 0;
        [[nodiscard]] virtual Result<void> OnOpen(EditorModalContext &context) = 0;

        virtual void OnUpdate(float /*dt*/) {
            // Most headless modals have no per-frame update work.
        }

        [[nodiscard]] virtual ModalFrameResult Draw() = 0;
        [[nodiscard]] virtual CloseDecision CanClose(ModalCloseReason reason) = 0;

        virtual void OnClose(ModalCloseReason /*reason*/) {
            // The host owns removal; subclasses opt in only when cleanup is required.
        }
    };

    /** @brief Owns the exclusive editor modal stack and commits lifecycle changes only at host frame boundaries. */
    class EditorModalHost {
    public:
        /** @brief Creates a headless modal host bound to one editor-session event bus. */
        explicit EditorModalHost(EditorDataBus &events, Input::InputRouter &inputRouter, std::size_t maximumDepth = 8);
        ~EditorModalHost();

        EditorModalHost(const EditorModalHost &) = delete;
        EditorModalHost &operator=(const EditorModalHost &) = delete;

        /** @brief Schedules the only root workflow modal and immediately gates workspace interaction. */
        [[nodiscard]] Result<void> OpenRoot(std::unique_ptr<EditorModal> modal);
        /** @brief Schedules a child owned by the current top modal. */
        [[nodiscard]] Result<void> PushChild(ModalId parentId, std::unique_ptr<EditorModal> modal);
        /** @brief Defers removal of the current top modal until the current host-frame boundary. */
        [[nodiscard]] Result<void> RequestClose(ModalId modalId, ModalCloseReason reason);
        /** @brief Returns whether the host logically owns an active or pending modal. */
        [[nodiscard]] bool HasOpenModal() const noexcept;
        /** @brief Returns the active stack top, including a pending accepted modal. */
        [[nodiscard]] std::optional<ModalId> TopModalId() const;
        /** @brief Returns a pointer to the top modal for typed access. */
        [[nodiscard]] EditorModal *TopModal();
        /** @brief Returns the current input-routing scope; accepted pending roots are already modal-scoped. */
        [[nodiscard]] EditorInteractionScope InteractionScope() const noexcept;
        /** @brief Performs modal updates and commits any deferred lifecycle transition at its boundary. */
        void OnUpdate(float dt);
        /** @brief Draws the top modal and commits any draw-requested close at its boundary.
         * @param liveReload Optional host-owned drawing registry, borrowed only for this call; never stored on a modal.
         */
        void Draw(const GuiLive::IRegistry *liveReload = nullptr);
        /** @brief Requests orderly shutdown closes after every modal accepts the shutdown reason. */
        [[nodiscard]] Result<void> RequestCloseAllForShutdown();
        /** @brief Immediately invokes close callbacks once and releases every modal during forced shutdown. */
        void ForceDetachAllForShutdown();

    private:
        struct Entry {
            std::unique_ptr<EditorModal> modal;
            bool opened = false;
            Input::InputContextToken inputContext;
        };

        [[nodiscard]] Result<void> ValidateModalForPush(const EditorModal &modal) const;
        [[nodiscard]] Result<void> ErrorFor(ModalHostError error) const;
        void CommitPendingOpens();
        void CommitPendingCloses();
        void RemoveTop(ModalCloseReason reason);
        [[nodiscard]] bool ContainsId(ModalId id) const noexcept;

        EditorDataBus &m_events;
        Input::InputRouter &m_inputRouter;
        EditorModalContext m_context;
        std::size_t m_maximumDepth;
        std::vector<Entry> m_stack;
        std::vector<Entry> m_pendingChildOpens;
        std::vector<ModalCloseReason> m_pendingCloseReasons;
        bool m_acceptingRequests{true};
        bool m_shutdown{false};
    };
}  // namespace Horo::Editor
