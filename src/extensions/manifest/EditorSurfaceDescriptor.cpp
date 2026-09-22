#include "Horo/Extensions/EditorSurfaceDescriptor.h"

#include "Horo/Extensions/ExtensionErrors.h"

#include <algorithm>
#include <ranges>
#include <string_view>

namespace Horo::Extensions {
    namespace {
        [[nodiscard]] Result<void> Invalid(const std::string_view reason) {
            return Result<void>::Failure(MakeError(ExtensionErrors::EditorSurfaceDescriptorInvalid, std::string{reason}));
        }

        [[nodiscard]] bool IsCanonicalIdentity(const std::string &value, const std::size_t maximumBytes) {
            if (value.empty() || value.size() > maximumBytes || value.front() == '.' || value.back() == '.')
                return false;
            bool previousDot = false;
            for (const unsigned char character : value) {
                if (character == '.') {
                    if (previousDot)
                        return false;
                    previousDot = true;
                    continue;
                }
                if (!((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '-' ||
                      character == '_'))
                    return false;
                previousDot = false;
            }
            return !previousDot;
        }

        [[nodiscard]] bool IsCanonicalLocalizationKey(const std::string &value, const std::size_t maximumBytes) {
            if (!IsCanonicalIdentity(value, maximumBytes))
                return false;
            return value.find('.') != std::string::npos;
        }

        [[nodiscard]] bool IsKnownSurfaceKind(const EditorSurfaceKind kind) noexcept {
            return static_cast<std::uint8_t>(kind) <= static_cast<std::uint8_t>(EditorSurfaceKind::ToolbarAction);
        }

        [[nodiscard]] bool IsKnownPlacement(const EditorSurfacePlacementKind kind) noexcept {
            return static_cast<std::uint8_t>(kind) <= static_cast<std::uint8_t>(EditorSurfacePlacementKind::Toolbar);
        }

        [[nodiscard]] bool IsKnownPersistence(const EditorSurfacePersistence policy) noexcept {
            return static_cast<std::uint8_t>(policy) <= static_cast<std::uint8_t>(EditorSurfacePersistence::Project);
        }

        [[nodiscard]] bool PlacementMatchesKind(const EditorSurfaceKind kind, const EditorSurfacePlacementKind placement) noexcept {
            switch (kind) {
                case EditorSurfaceKind::Panel:
                case EditorSurfaceKind::Tab:
                    return placement == EditorSurfacePlacementKind::Workspace;
                case EditorSurfaceKind::Modal:
                case EditorSurfaceKind::ModalPage:
                    return placement == EditorSurfacePlacementKind::Modal;
                case EditorSurfaceKind::SettingsPage:
                    return placement == EditorSurfacePlacementKind::Settings;
                case EditorSurfaceKind::Inspector:
                case EditorSurfaceKind::PropertyDrawer:
                    return placement == EditorSurfacePlacementKind::Inspector;
                case EditorSurfaceKind::ViewportOverlay:
                case EditorSurfaceKind::Gizmo:
                case EditorSurfaceKind::AssetPreview:
                    return placement == EditorSurfacePlacementKind::Viewport;
                case EditorSurfaceKind::StatusItem:
                    return placement == EditorSurfacePlacementKind::StatusBar;
                case EditorSurfaceKind::ActivityItem:
                    return placement == EditorSurfacePlacementKind::ActivityBar;
                case EditorSurfaceKind::MenuItem:
                    return placement == EditorSurfacePlacementKind::Menu;
                case EditorSurfaceKind::ToolbarAction:
                    return placement == EditorSurfacePlacementKind::Toolbar;
            }
            return false;
        }

        [[nodiscard]] bool PersistenceMatchesKind(const EditorSurfaceKind kind, const EditorSurfacePersistence policy) noexcept {
            using enum EditorSurfaceKind;
            if (policy == EditorSurfacePersistence::None)
                return true;
            return kind == Panel || kind == Tab || kind == SettingsPage;
        }

        template <typename Id> [[nodiscard]] bool HasUniqueCanonicalIds(const std::vector<Id> &ids, const std::size_t maximumBytes) {
            for (std::size_t index = 0; index < ids.size(); ++index) {
                if (!IsCanonicalIdentity(ids[index].value, maximumBytes))
                    return false;
                for (std::size_t previous = 0; previous < index; ++previous) {
                    if (ids[previous].value == ids[index].value)
                        return false;
                }
            }
            return true;
        }

        [[nodiscard]] bool HasUniqueKnownEventKinds(const std::vector<EditorSurfaceEventId> &events) noexcept {
            for (std::size_t index = 0; index < events.size(); ++index) {
                if (!IsKnownEditorEventKind(events[index]))
                    return false;
                for (std::size_t previous = 0; previous < index; ++previous) {
                    if (events[previous] == events[index])
                        return false;
                }
            }
            return true;
        }

        [[nodiscard]] bool HasNoEventOverlap(const std::vector<EditorSurfaceEventId> &editorEvents,
                                             const std::vector<EditorSurfaceEventId> &processEvents) noexcept {
            return std::ranges::none_of(editorEvents, [&processEvents](const EditorSurfaceEventId event) {
                return std::ranges::find(processEvents, event) != processEvents.end();
            });
        }

        [[nodiscard]] bool HasValidProviderOwnership(const EditorSurfaceProviderIdentity &provider, const std::size_t maximumBytes) {
            return IsCanonicalIdentity(provider.extensionId, maximumBytes) && IsCanonicalIdentity(provider.moduleId, maximumBytes) &&
                   provider.activationGeneration != 0;
        }
    }  // namespace

    /** @copydoc ValidateEditorSurfaceDescriptor */
    Result<void> ValidateEditorSurfaceDescriptor(const EditorSurfaceDescriptor &descriptor, const EditorSurfaceDescriptorLimits &limits) {
        if (limits.maximumIdentityBytes == 0 || limits.maximumLocalizationKeyBytes == 0 || limits.maximumPlacementTargetBytes == 0)
            return Invalid("Editor surface descriptor limits must be non-zero.");
        if (!IsCanonicalIdentity(descriptor.id, limits.maximumIdentityBytes))
            return Invalid("Editor surface ID must be a canonical lowercase identity.");
        if (!IsKnownSurfaceKind(descriptor.kind))
            return Invalid("Editor surface kind is unsupported.");
        if (!IsCanonicalLocalizationKey(descriptor.labelLocalizationKey, limits.maximumLocalizationKeyBytes))
            return Invalid("Editor surface label must be a canonical localization key.");
        if (!descriptor.tooltipLocalizationKey.empty() &&
            !IsCanonicalLocalizationKey(descriptor.tooltipLocalizationKey, limits.maximumLocalizationKeyBytes))
            return Invalid("Editor surface tooltip must be a canonical localization key.");
        if (!IsKnownPlacement(descriptor.placement.kind) || !PlacementMatchesKind(descriptor.kind, descriptor.placement.kind))
            return Invalid("Editor surface placement does not match its surface kind.");
        if (descriptor.placement.target.size() > limits.maximumPlacementTargetBytes ||
            (!descriptor.placement.target.empty() && !IsCanonicalIdentity(descriptor.placement.target, limits.maximumPlacementTargetBytes)))
            return Invalid("Editor surface placement target is not a canonical bounded identity.");
        if (!IsKnownPersistence(descriptor.persistence) || !PersistenceMatchesKind(descriptor.kind, descriptor.persistence))
            return Invalid("Editor surface persistence policy is not valid for this surface kind.");
        if (descriptor.openByDefault && descriptor.persistence == EditorSurfacePersistence::None)
            return Invalid("A non-persistent surface cannot be open by default.");
        if (descriptor.requiredCapabilities.size() > limits.maximumCapabilities ||
            descriptor.requiredPermissions.size() > limits.maximumPermissions)
            return Invalid("Editor surface capability or permission count exceeds the configured limit.");
        if (descriptor.requestedEditorEvents.size() > limits.maximumEditorEvents ||
            descriptor.requestedProcessEvents.size() > limits.maximumProcessEvents)
            return Invalid("Editor surface event request count exceeds the configured limit.");
        if (!HasUniqueKnownEventKinds(descriptor.requestedEditorEvents) || !HasUniqueKnownEventKinds(descriptor.requestedProcessEvents) ||
            !HasNoEventOverlap(descriptor.requestedEditorEvents, descriptor.requestedProcessEvents))
            return Invalid("Editor surface event requests must be known, unique, and separated by source.");
        if (!HasUniqueCanonicalIds(descriptor.requiredCapabilities, limits.maximumIdentityBytes) ||
            !HasUniqueCanonicalIds(descriptor.requiredPermissions, limits.maximumIdentityBytes))
            return Invalid("Editor surface capability and permission identities must be unique and canonical.");
        if (!HasValidProviderOwnership(descriptor.provider, limits.maximumIdentityBytes))
            return Invalid("Editor surface provider ownership is incomplete or invalid.");
        return Result<void>::Success();
    }
}  // namespace Horo::Extensions
