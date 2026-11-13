#include "EditorControlMetadata.h"

#include "Horo/Editor/GuiLiveReload.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <imgui.h>
#include <imgui_internal.h>
#include <limits>
#include <optional>
#include <vector>

namespace Horo::Editor::Detail {
    using namespace Runtime::Ui;

    namespace {
        constexpr int MaximumContextHooks = 64;

        /** @brief Finds only this host's known lifecycle callback; foreign framework user data is never dereferenced. */
        EditorControlMetadataCapture *BoundCapture(ImGuiContext *context) noexcept {
            if (!context || context->Hooks.Size > MaximumContextHooks)
                return nullptr;
            const auto callback = &GuiLive::HostSymbol<&EditorControlMetadataHook>();
            for (const auto &hook : context->Hooks)
                if (hook.Type == ImGuiContextHookType_NewFramePre && hook.Callback == callback)
                    return static_cast<EditorControlMetadataCapture *>(hook.UserData);
            return nullptr;
        }

        /** @brief Scans only the bounded visible label prefix; framework identity suffixes are not accessible names. */
        std::optional<std::string_view> VisibleName(const char *label, const EditorControlLabelKind kind) noexcept {
            if (!label)
                return std::string_view{};
            for (std::size_t size = 0; size <= EditorControlMetadataCapture::MaximumFieldBytes; ++size) {
                if (label[size] == '\0' || (kind == EditorControlLabelKind::FrameworkLabel && label[size] == '#' && label[size + 1] == '#'))
                    return std::string_view{label, size};
            }
            return std::nullopt;
        }

        /** @brief Validates real item geometry and the schema's closed role/state/value vocabulary. */
        bool ValidFacts(const ImGuiContext &context, const UiAccessibilityRole role, const UiAccessibilityState extra,
                        const UiAccessibilityValue &value) noexcept {
            const auto &item = context.LastItemData;
            const std::array bounds{item.Rect.Min.x, item.Rect.Min.y, item.Rect.Max.x, item.Rect.Max.y};
            return context.CurrentWindow && item.ID != 0 && role <= UiAccessibilityRole::ScrollView && extra.IsValid() && value.IsValid() &&
                   item.Rect.Min.x <= item.Rect.Max.x && item.Rect.Min.y <= item.Rect.Max.y &&
                   std::ranges::all_of(bounds, [](const float coordinate) {
                return std::isfinite(coordinate);
            });
        }

        /** @brief Projects framework-owned focus and disablement without introducing another mutable focus store. */
        UiAccessibilityState ObservedState(const UiAccessibilityState extra) noexcept {
            const auto ownedFlags =
                UiAccessibilityStateFlag::Disabled | UiAccessibilityStateFlag::Focusable | UiAccessibilityStateFlag::Focused;
            auto flags = extra.flags & ~ownedFlags;
            const auto itemFlags = GImGui->LastItemData.ItemFlags;
            if (itemFlags & ImGuiItemFlags_Disabled)
                flags |= static_cast<UiAccessibilityStateMask>(UiAccessibilityStateFlag::Disabled);
            if (!(itemFlags & (ImGuiItemFlags_NoNav | ImGuiItemFlags_Disabled)))
                flags |= static_cast<UiAccessibilityStateMask>(UiAccessibilityStateFlag::Focusable);
            if (ImGui::IsItemFocused())
                flags |= static_cast<UiAccessibilityStateMask>(UiAccessibilityStateFlag::Focused);
            return UiAccessibilityState{flags};
        }
    }  // namespace

    /** @brief Reserved double-buffered observations and their exact framework lifecycle borrow. */
    struct EditorControlMetadataCapture::Storage final {
        /** @brief One complete capture attempt; overflow invalidates the entire exposed inventory. */
        struct Buffer final {
            std::vector<EditorControlMetadataRecord> controls{MaximumControls};
            std::vector<char> text = std::vector<char>(MaximumTextBytes);
            std::array<std::uint64_t, MaximumControls * 2> identities{};
            std::size_t count{}, bytes{};
            std::uint64_t sequence{};
            std::uint32_t missingNames{};
            EditorControlMetadataStatus status{EditorControlMetadataStatus::Complete};

            /** @brief Preflights every byte before copying any field. */
            bool HasCapacity(const std::string_view name, const UiAccessibilityValueInput &value) const noexcept {
                return count < MaximumControls && value.text.text.size() <= MaximumFieldBytes &&
                       name.size() + value.text.text.size() <= MaximumTextBytes - bytes;
            }

            /** @brief Rejects duplicate actual window/item identities using fixed, bounded open addressing. */
            bool AdmitIdentity(const std::uint32_t window, const std::uint32_t item) noexcept {
                const auto key = (static_cast<std::uint64_t>(window) << 32) | item;
                std::size_t index = (window ^ item) % identities.size();
                for (std::size_t visited = 0; visited < identities.size(); ++visited) {
                    if (identities[index] == key)
                        return false;
                    if (identities[index] == 0) {
                        identities[index] = key;
                        return true;
                    }
                    index = (index + 1) % identities.size();
                }
                return false;
            }

            /** @brief Copies one already-bounded field into reserved storage. */
            UiAccessibilityTextRef Copy(const std::string_view value, const UiAccessibilityTextSource source) noexcept {
                const UiAccessibilityTextRef reference{static_cast<std::uint32_t>(bytes), static_cast<std::uint32_t>(value.size()), source};
                if (!value.empty())
                    std::memcpy(text.data() + bytes, value.data(), value.size());
                bytes += value.size();
                return reference;
            }
        };

        std::array<Buffer, 2> buffers;
        ImGuiContext *context{};
        std::array<ImGuiID, 3> hooks{};
        std::size_t writing{}, completed{};
        std::uint64_t sequence{};
        bool active{}, hasCompleted{}, retired{};

        /** @brief Begins one bounded attempt without reallocating either store. */
        void Begin() noexcept {
            auto &buffer = buffers[writing];
            buffer.count = buffer.bytes = 0;
            buffer.missingNames = 0;
            buffer.identities.fill(0);
            buffer.status = EditorControlMetadataStatus::Complete;
            if (sequence == std::numeric_limits<std::uint64_t>::max()) {
                buffer.status = EditorControlMetadataStatus::InvalidInput;
                buffer.sequence = sequence;
            } else
                buffer.sequence = ++sequence;
            active = true;
        }

        /** @brief Publishes the whole attempt only after the framework ends its actual UI frame. */
        void Finish() noexcept {
            if (!active)
                return;
            completed = writing;
            writing = 1 - writing;
            hasCompleted = true;
            active = false;
            if (context && context->Hooks.Size > MaximumContextHooks)
                buffers[completed].status = EditorControlMetadataStatus::InvalidInput;
        }

        /** @brief Validates and copies one real item; no partial inventory escapes a rejected frame. */
        void Observe(UiAccessibilityRole role, const char *label, UiAccessibilityState extra, const UiAccessibilityValueInput &value,
                     bool redacted, EditorControlLabelKind labelKind) noexcept;
    };

    /** @brief Copies one actual control into the bounded current attempt after complete validation. */
    void EditorControlMetadataCapture::Storage::Observe(const UiAccessibilityRole role, const char *label, const UiAccessibilityState extra,
                                                        const UiAccessibilityValueInput &value, const bool redacted,
                                                        const EditorControlLabelKind labelKind) noexcept {
        if (!active)
            return;
        auto &buffer = buffers[writing];
        if (buffer.status != EditorControlMetadataStatus::Complete)
            return;
        const auto effective = redacted ? UiAccessibilityValueInput{} : value;
        const auto name = VisibleName(label, labelKind);
        if (!name || !buffer.HasCapacity(*name, effective)) {
            buffer.status = EditorControlMetadataStatus::CapacityExceeded;
            return;
        }
        const auto &item = context->LastItemData;
        const auto *window = context->CurrentWindow;
        const UiAccessibilityTextRef provisional = effective.kind == UiAccessibilityValueKind::Text
                                                       ? UiAccessibilityTextRef{0, 0, effective.text.source}
                                                       : UiAccessibilityTextRef{};
        UiAccessibilityValue owned{effective.kind, effective.boolean, effective.integer, effective.number, provisional};
        if (!ValidFacts(*context, role, extra, owned) || effective.text.source > UiAccessibilityTextSource::UserContent ||
            labelKind > EditorControlLabelKind::ResolvedText || !buffer.AdmitIdentity(window->ID, item.ID)) {
            buffer.status = EditorControlMetadataStatus::InvalidInput;
            return;
        }
        if (effective.kind == UiAccessibilityValueKind::Text)
            owned.text = buffer.Copy(effective.text.text, effective.text.source);
        const auto copiedName = buffer.Copy(*name, UiAccessibilityTextSource::ResolvedMessage);
        buffer.missingNames += name->empty();
        buffer.controls[buffer.count++] = {window->ID,
                                           item.ID,
                                           role,
                                           ObservedState(extra),
                                           copiedName,
                                           owned,
                                           {item.Rect.Min.x, item.Rect.Min.y, item.Rect.Max.x, item.Rect.Max.y},
                                           !ImGui::IsItemVisible(),
                                           redacted};
    }

    /** @copydoc EditorControlMetadataFrame::Text */
    std::string_view EditorControlMetadataFrame::Text(const UiAccessibilityTextRef reference) const noexcept {
        if (!reference.IsPresent() || reference.offset > text.size() || reference.size > text.size() - reference.offset)
            return {};
        return {text.data() + reference.offset, reference.size};
    }

    /** @copydoc EditorControlMetadataCapture::EditorControlMetadataCapture */
    EditorControlMetadataCapture::EditorControlMetadataCapture() : storage_(std::make_unique<Storage>()) {}

    /** @copydoc EditorControlMetadataCapture::~EditorControlMetadataCapture */
    EditorControlMetadataCapture::~EditorControlMetadataCapture() {
        Unbind();
    }

    /** @copydoc EditorControlMetadataCapture::Bind */
    bool EditorControlMetadataCapture::Bind(ImGuiContext *const context) {
        if (storage_->context == context && context && BoundCapture(context) == this)
            return true;
        Unbind();
        if (!context || context->Hooks.Size > MaximumContextHooks - 3 || BoundCapture(context))
            return false;
        storage_->context = context;
        storage_->retired = false;
        constexpr std::array types{ImGuiContextHookType_NewFramePre, ImGuiContextHookType_EndFramePost, ImGuiContextHookType_Shutdown};
        for (std::size_t index = 0; index < types.size(); ++index) {
            ImGuiContextHook hook;
            hook.Type = types[index];
            hook.Callback = &GuiLive::HostSymbol<&EditorControlMetadataHook>();
            hook.UserData = this;
            storage_->hooks[index] = ImGui::AddContextHook(context, &hook);
        }
        if (context->WithinFrameScope)
            storage_->Begin();
        return true;
    }

    /** @copydoc EditorControlMetadataCapture::Unbind */
    void EditorControlMetadataCapture::Unbind() noexcept {
        if (storage_->context)
            for (const auto hook : storage_->hooks)
                ImGui::RemoveContextHook(storage_->context, hook);
        storage_->context = nullptr;
        storage_->active = storage_->hasCompleted = false;
        storage_->retired = true;
        storage_->hooks.fill(0);
    }

    /** @copydoc EditorControlMetadataCapture::Frame */
    EditorControlMetadataFrame EditorControlMetadataCapture::Frame() const noexcept {
        if (storage_->retired)
            return {.status = EditorControlMetadataStatus::Retired};
        if (!storage_->hasCompleted)
            return {};
        const auto &buffer = storage_->buffers[storage_->completed];
        const bool complete = buffer.status == EditorControlMetadataStatus::Complete;
        return {.sequence = buffer.sequence,
                .status = buffer.status,
                .controls = std::span{buffer.controls}.first(complete ? buffer.count : 0),
                .text = std::span{buffer.text}.first(complete ? buffer.bytes : 0),
                .missingNames = complete ? buffer.missingNames : 0};
    }

    /** @copydoc EditorControlMetadataHook */
    void EditorControlMetadataHook(ImGuiContext *, ImGuiContextHook *const hook) {
        auto &storage = *static_cast<EditorControlMetadataCapture *>(hook->UserData)->storage_;
        if (hook->Type == ImGuiContextHookType_NewFramePre)
            storage.Begin();
        else if (hook->Type == ImGuiContextHookType_EndFramePost)
            storage.Finish();
        else if (hook->Type == ImGuiContextHookType_Shutdown) {
            storage.context = nullptr;
            storage.active = storage.hasCompleted = false;
            storage.retired = true;
        }
    }

    /** @copydoc ObserveEditorControl */
    void ObserveEditorControl(const UiAccessibilityRole role, const char *label, const UiAccessibilityState extra,
                              const UiAccessibilityValueInput value, const bool valueRedacted,
                              const EditorControlLabelKind labelKind) noexcept {
        auto *const context = ImGui::GetCurrentContext();
        if (context && context->CurrentWindow && context->CurrentWindow->SkipItems)
            return;
        if (auto *const capture = BoundCapture(context))
            capture->storage_->Observe(role, label, extra, value, valueRedacted, labelKind);
    }

    /** @copydoc CurrentEditorControlMetadata */
    EditorControlMetadataFrame CurrentEditorControlMetadata() noexcept {
        if (const auto *capture = BoundCapture(ImGui::GetCurrentContext()))
            return capture->Frame();
        return {};
    }
}  // namespace Horo::Editor::Detail
