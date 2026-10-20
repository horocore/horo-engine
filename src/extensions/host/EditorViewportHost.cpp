#include "Horo/Extensions/EditorViewportHost.h"

#include "Horo/Extensions/ExtensionErrors.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <thread>

namespace Horo::Extensions {
    namespace {
        /** @brief Returns the established restricted-surface error vocabulary without erasing downstream failures. */
        template <class T> Result<T> Rejected(const ErrorCodeDescriptor &reason = ExtensionErrors::ContributionRejected) {
            return Result<T>::Failure(MakeError(reason));
        }

        /** @brief Checks both sealed policy and the exact activation's current revocable capability. */
        bool Allowed(const ExtensionAdmissionPolicy &policy, const EditorSurfaceContext &context) {
            const bool gizmo = context.Surface().kind == EditorSurfaceKind::Gizmo;
            static const ExtensionPermissionId overlayPermission{"editor.viewport-overlay"};
            static const ExtensionPermissionId gizmoPermission{"editor.gizmo"};
            const auto &permission = gizmo ? gizmoPermission : overlayPermission;
            const auto &capability = gizmo ? EditorGizmoCapability : EditorViewportOverlayCapability;
            return policy.revision && context.IsUsable() &&
                   std::ranges::find(policy.knownPermissions, permission) != policy.knownPermissions.end() &&
                   std::ranges::find(policy.approvedPermissions, permission) != policy.approvedPermissions.end() &&
                   std::ranges::find(policy.availableCapabilities, capability) != policy.availableCapabilities.end() &&
                   std::ranges::find(context.Surface().requiredPermissions, permission) != context.Surface().requiredPermissions.end() &&
                   context.AcquireCapabilityUse(capability).HasValue();
        }
    }  // namespace

    /** @brief Exact revocation/publication owner; no callback, document reference or provider serializer is retained. */
    struct EditorViewportSession final : IExtensionRetirementContribution {
        explicit EditorViewportSession(EditorSurfaceContext view) : context(std::move(view)) {}

        EditorSurfaceContext context;
        std::shared_ptr<const EditorViewportDefinition> definition;
        std::uint64_t identity{};
        std::optional<ExtensionCapabilityUseLease> capability;
        bool visible{true};
        std::optional<EditorSurfaceRegistration> registration;
        std::shared_ptr<ExtensionExecutableLease> publication;
        bool revoked{};

        /** @brief Removes registry discovery and invalidates existing context copies before releasing the provider pin. */
        void Revoke() noexcept override {
            if (revoked)
                return;
            revoked = true;
            if (registration)
                registration->Reset();
            publication.reset();
        }
    };

    namespace {
        /** @brief Revokes staged publication on every failure/exception before the final owner token is returned. */
        struct PublicationGuard final {
            explicit PublicationGuard(std::shared_ptr<EditorViewportSession> staged) : session(std::move(staged)) {}

            PublicationGuard(const PublicationGuard &) = delete;
            PublicationGuard &operator=(const PublicationGuard &) = delete;
            PublicationGuard(PublicationGuard &&) = delete;
            PublicationGuard &operator=(PublicationGuard &&) = delete;
            std::shared_ptr<EditorViewportSession> session;

            ~PublicationGuard() {
                if (session)
                    session->Revoke();
            }
        };

        /** @brief Prepares copied geometry and its revocable provider pin before any registry publication. */
        Result<std::shared_ptr<EditorViewportSession>> PrepareViewportSession(const EditorSurfaceContext &context,
                                                                              EditorViewportDefinition definition, std::uint64_t identity,
                                                                              const std::shared_ptr<ExtensionRetirement> &retirement) {
            auto session = std::make_shared<EditorViewportSession>(context);
            session->identity = identity;
            auto capability = context.AcquireCapabilityUse(
                context.Surface().kind == EditorSurfaceKind::Gizmo ? EditorGizmoCapability : EditorViewportOverlayCapability);
            if (capability.HasError())
                return Result<std::shared_ptr<EditorViewportSession>>::Failure(capability.ErrorValue());
            session->capability.emplace(std::move(capability).Value());
            session->visible = definition.visible;
            auto copied = std::make_shared<EditorViewportDefinition>(std::move(definition));
            session->definition = copied;
            PublicationGuard rollback{session};
            session->publication =
                retirement->Acquire(context.Provider().moduleId, ExtensionLeaseKind::UiSurface, context.Surface().id, std::move(copied));
            if (!session->publication || !retirement->RegisterContribution(context.Provider().moduleId, session))
                return Rejected<std::shared_ptr<EditorViewportSession>>(ExtensionErrors::CapabilityRevoked);
            rollback.session.reset();
            return Result<std::shared_ptr<EditorViewportSession>>::Success(std::move(session));
        }
    }  // namespace

    /** @brief Owner-lane catalog borrows one existing registry; weak entries cannot prolong contribution publication. */
    struct EditorViewportHost::State final {
        State(EditorSurfaceRegistry &surfaces, ExtensionAdmissionPolicy evidence) : registry(surfaces), policy(std::move(evidence)) {}

        EditorSurfaceRegistry &registry;
        const ExtensionAdmissionPolicy policy;
        const std::thread::id owner{std::this_thread::get_id()};
        std::vector<std::weak_ptr<EditorViewportSession>> sessions;
        bool shutdown{};
        std::uint64_t nextGeneration{1};
    };

    /** @copydoc EditorViewportRegistration::EditorViewportRegistration */
    EditorViewportRegistration::EditorViewportRegistration(std::shared_ptr<EditorViewportSession> session) noexcept
        : session_(std::move(session)) {}

    /** @copydoc EditorViewportRegistration::~EditorViewportRegistration */
    EditorViewportRegistration::~EditorViewportRegistration() {
        Reset();
    }

    /** @copydoc EditorViewportRegistration::EditorViewportRegistration */
    EditorViewportRegistration::EditorViewportRegistration(EditorViewportRegistration &&) noexcept = default;

    /** @copydoc EditorViewportRegistration::operator= */
    EditorViewportRegistration &EditorViewportRegistration::operator=(EditorViewportRegistration &&other) noexcept {
        if (this != &other) {
            Reset();
            session_ = std::move(other.session_);
        }
        return *this;
    }

    /** @copydoc EditorViewportRegistration::Reset */
    void EditorViewportRegistration::Reset() noexcept {
        if (session_)
            session_->Revoke();
        session_.reset();
    }

    /** @copydoc EditorViewportHost::EditorViewportHost */
    EditorViewportHost::EditorViewportHost(EditorSurfaceRegistry &registry, ExtensionAdmissionPolicy policy)
        : state_(std::make_unique<State>(registry, std::move(policy))) {}

    /** @copydoc EditorViewportHost::~EditorViewportHost */
    EditorViewportHost::~EditorViewportHost() {
        BeginShutdown();
    }

    /** @copydoc EditorViewportHost::AdmissionPolicy */
    const ExtensionAdmissionPolicy &EditorViewportHost::AdmissionPolicy() const noexcept {
        return state_->policy;
    }

    /** @copydoc EditorViewportHost::Register */
    Result<EditorViewportRegistration> EditorViewportHost::Register(EditorSurfaceContextRegistration context,
                                                                    EditorViewportDefinition definition,
                                                                    std::shared_ptr<ExtensionRetirement> retirement) {
        if (state_->shutdown || state_->owner != std::this_thread::get_id() || !context.IsRegistered() || !retirement)
            return Rejected<EditorViewportRegistration>();
        if (auto valid = ValidateEditorViewportDefinition(context.Context().Surface(), definition); valid.HasError())
            return Result<EditorViewportRegistration>::Failure(valid.ErrorValue());
        if (!Allowed(state_->policy, context.Context()))
            return Rejected<EditorViewportRegistration>(ExtensionErrors::CapabilityUnavailable);
        std::erase_if(state_->sessions, [](const auto &weak) {
            const auto live = weak.lock();
            return !live || live->revoked;
        });
        if (state_->sessions.size() >= 64)
            return Rejected<EditorViewportRegistration>(ExtensionErrors::EditorSurfaceRegistryCapacityExceeded);
        state_->sessions.reserve(state_->sessions.size() + 1);
        std::size_t total = definition.lines.size();
        for (const auto &weak : state_->sessions)
            if (const auto live = weak.lock(); live && !live->revoked)
                total += live->definition->lines.size();
        if (total > 256)
            return Rejected<EditorViewportRegistration>(ExtensionErrors::EditorSurfaceRegistryCapacityExceeded);
        const auto identity = AllocateGeneration();
        if (!identity)
            return Rejected<EditorViewportRegistration>(ExtensionErrors::CapabilityRevoked);
        auto prepared = PrepareViewportSession(context.Context(), std::move(definition), identity, retirement);
        if (prepared.HasError())
            return Result<EditorViewportRegistration>::Failure(prepared.ErrorValue());
        auto session = std::move(prepared).Value();
        PublicationGuard rollback{session};
        auto registered = state_->registry.Register(std::move(context));
        if (registered.HasError()) {
            session->Revoke();
            return Result<EditorViewportRegistration>::Failure(registered.ErrorValue());
        }
        session->registration.emplace(std::move(registered).Value());
        state_->sessions.push_back(session);
        std::ranges::sort(state_->sessions, [](const auto &a, const auto &b) {
            return a.lock()->context.Surface().id < b.lock()->context.Surface().id;
        });
        rollback.session.reset();
        return Result<EditorViewportRegistration>::Success(EditorViewportRegistration{std::move(session)});
    }

    /** @copydoc EditorViewportHost::Extract */
    void EditorViewportHost::Extract(EditorViewportFrame &output, EditorViewportTool tool, bool editable, bool visible) const noexcept {
        output.count = 0;
        output.tool = tool;
        if (!visible || state_->shutdown || state_->owner != std::this_thread::get_id())
            return;
        for (const auto &weak : state_->sessions) {
            const auto live = weak.lock();
            if (!live || live->revoked || !live->visible || (!live->context.IsUsable() || !live->capability->IsUsable()) ||
                !state_->registry.IsAvailable(live->context.Provider(), live->context.Surface().id))
                continue;
            if (live->definition->tool != EditorViewportTool::None && (!editable || live->definition->tool != tool))
                continue;
            for (const auto &line : live->definition->lines)
                if (output.count < output.lines.size())
                    output.lines[output.count++] = {line, live->identity};
        }
    }

    /** @copydoc EditorViewportHost::IsLive */
    bool EditorViewportHost::IsLive(EditorViewportPick pick, EditorViewportTool tool) const noexcept {
        if (!pick || pick.axis < 0 || pick.axis > 2 || state_->shutdown || state_->owner != std::this_thread::get_id())
            return false;
        for (const auto &weak : state_->sessions) {
            const auto live = weak.lock();
            if (live && live->identity == pick.publication && !live->revoked && live->visible && live->definition->tool == tool &&
                tool != EditorViewportTool::None && (live->context.IsUsable() && live->capability->IsUsable()) &&
                state_->registry.IsAvailable(live->context.Provider(), live->context.Surface().id))
                return std::ranges::any_of(live->definition->lines, [pick](const auto &line) {
                    return line.axis == pick.axis;
                });
        }
        return false;
    }

    /** @copydoc EditorViewportHost::Pick */
    EditorViewportPick EditorViewportHost::Pick(const EditorViewportFrame &frame, float x, float y, float width,
                                                float height) const noexcept {
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0 ||
            x < 0 || x > 1 || y < 0 || y > 1 || frame.count > frame.lines.size())
            return {};
        for (std::size_t i = frame.count; i > 0; --i) {
            const auto &entry = frame.lines[i - 1];
            if (entry.line.axis < 0)
                continue;
            const float dx = (entry.line.x1 - entry.line.x0) * width;
            const float dy = (entry.line.y1 - entry.line.y0) * height;
            const float px = (x - entry.line.x0) * width;
            const float py = (y - entry.line.y0) * height;
            const float length = dx * dx + dy * dy;
            const float t = length > 0 ? std::clamp((px * dx + py * dy) / length, 0.0F, 1.0F) : 0;
            if (const float ex = px - t * dx, ey = py - t * dy;
                ex * ex + ey * ey <= 36 && IsLive({entry.publication, entry.line.axis}, frame.tool))
                return {entry.publication, entry.line.axis};
        }
        return {};
    }

    /** @copydoc EditorViewportHost::SetVisible */
    Result<void> EditorViewportHost::SetVisible(const EditorSurfaceProviderIdentity &provider, std::string_view surfaceId, bool visible) {
        if (state_->shutdown || state_->owner != std::this_thread::get_id())
            return Rejected<void>();
        for (const auto &weak : state_->sessions)
            if (const auto live = weak.lock(); live && !live->revoked && live->context.Provider() == provider &&
                                               live->context.Surface().id == surfaceId &&
                                               (live->context.IsUsable() && live->capability->IsUsable())) {
                live->visible = visible;
                return Result<void>::Success();
            }
        return Rejected<void>(ExtensionErrors::CapabilityRevoked);
    }

    /** @copydoc EditorViewportHost::AllocateGeneration */
    std::uint64_t EditorViewportHost::AllocateGeneration() noexcept {
        if (state_->shutdown || state_->owner != std::this_thread::get_id() ||
            state_->nextGeneration == std::numeric_limits<std::uint64_t>::max())
            return 0;
        return state_->nextGeneration++;
    }

    /** @copydoc EditorViewportHost::BeginShutdown */
    void EditorViewportHost::BeginShutdown() noexcept {
        state_->shutdown = true;
        for (const auto &weak : state_->sessions)
            if (const auto live = weak.lock())
                live->Revoke();
        state_->sessions.clear();
    }
}  // namespace Horo::Extensions
