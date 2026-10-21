#pragma once
/** @file EditorViewportHost.h @brief Permissioned copied viewport geometry and host-owned transform handle picking. */
#include "Horo/Extensions/EditorSurfaceRegistry.h"
#include "Horo/Extensions/ExtensionRetirement.h"

#include <array>
#include <memory>
#include <span>
#include <vector>

namespace Horo::Extensions {
    inline const ExtensionCapabilityId EditorViewportOverlayCapability{"horo.editor.viewport-overlay"};
    inline const ExtensionCapabilityId EditorGizmoCapability{"horo.editor.gizmo"};
    /** @brief Closed transform operations interpreted by the existing host transaction controller. */
    enum class EditorViewportTool : std::uint8_t {
        None,
        Move,
        Rotate,
        Scale
    };
    /** @brief Semantic colors resolved by the editor theme, never native rendering resources. */
    enum class EditorViewportTone : std::uint8_t {
        Neutral,
        Accent,
        X,
        Y,
        Z
    };

    /** @brief Copied normalized viewport line; axis -1 is passive, 0..2 requests a host transform axis. */
    struct EditorViewportLine final {
        float x0{}, y0{}, x1{}, y1{};
        float thickness{1};
        EditorViewportTone tone{EditorViewportTone::Neutral};
        std::int8_t axis{-1};
    };

    /** @brief Inert bounded presentation; gizmos bind only the canonical editable scene transform contract. */
    struct EditorViewportDefinition final {
        std::string editableType;
        EditorViewportTool tool{EditorViewportTool::None};
        std::vector<EditorViewportLine> lines;
        bool visible{true};
    };

    /** @brief One copied scalar draw command; no module storage or callbacks survive extraction. */
    struct EditorViewportDrawLine final {
        EditorViewportLine line;
        std::uint64_t publication{};
    };

    /** @brief Fixed frame budget shared across all providers; exhaustion rejects registration rather than dropping frames. */
    struct EditorViewportFrame final {
        std::array<EditorViewportDrawLine, 256> lines{};
        std::size_t count{};
        EditorViewportTool tool{EditorViewportTool::None};
    };

    /** @brief Generation-scoped host pick, revalidated before every transform input step. */
    struct EditorViewportPick final {
        std::uint64_t publication{};
        int axis{-1};

        explicit operator bool() const noexcept {
            return publication != 0;
        }
    };
    struct EditorViewportSession;

    /** @brief Move-only provider publication; reset closes rendering and picking before releasing provider ownership. */
    class EditorViewportRegistration final {
    public:
        /** @brief Revokes rendering and picking before dropping provider ownership. */
        ~EditorViewportRegistration();
        /** @brief Transfers exclusive publication ownership. @param other Prior owner. */
        EditorViewportRegistration(EditorViewportRegistration &&other) noexcept;
        /** @brief Revokes prior publication and transfers ownership. @param other Prior owner. @return This owner. */
        EditorViewportRegistration &operator=(EditorViewportRegistration &&other) noexcept;
        EditorViewportRegistration(const EditorViewportRegistration &) = delete;
        EditorViewportRegistration &operator=(const EditorViewportRegistration &) = delete;
        /** @brief Withdraws this exact publication; repeated revocation is harmless. */
        void Reset() noexcept;

    private:
        friend class EditorViewportHost;
        /** @brief Adopts the completed exact publication. @param session New publication owner. */
        explicit EditorViewportRegistration(std::shared_ptr<EditorViewportSession> session) noexcept;
        std::shared_ptr<EditorViewportSession> session_;
    };

    /** @brief Validates a complete inert declaration without publishing or invoking provider code.
     * @param surface Exact manifest owner. @param definition Copied line geometry and optional transform binding.
     * @return Typed validation rejection or success.
     */
    [[nodiscard]] Result<void> ValidateEditorViewportDefinition(const EditorSurfaceDescriptor &surface,
                                                                const EditorViewportDefinition &definition);

    /** @brief Owner-lane viewport authority; registration allocates, frame extraction/picking never allocate or call providers.
     * @details Registry and host outlive attached panels. Host copies geometry before draw; no renderer resources or native handles
     * cross this boundary. All methods and registration teardown run on the editor owner lane.
     */
    class EditorViewportHost final {
    public:
        /** @brief Composes explicit publication authority. @param registry Borrowed registry outliving this host.
         * @param policy Copied sealed permission evidence. */
        EditorViewportHost(EditorSurfaceRegistry &registry, ExtensionAdmissionPolicy policy);
        /** @brief Revokes every remaining publication on the owner lane. */
        ~EditorViewportHost();
        EditorViewportHost(const EditorViewportHost &) = delete;
        EditorViewportHost &operator=(const EditorViewportHost &) = delete;
        /** @brief Borrows the immutable host approval evidence. @return Policy for the host lifetime. */
        [[nodiscard]] const ExtensionAdmissionPolicy &AdmissionPolicy() const noexcept;
        /** @brief Publishes copied geometry atomically under actual provider retirement ownership.
         * @param context Exact revocable manifest context. @param definition Complete bounded copied geometry.
         * @param retirement Verified owning provider. @return Move-only publication or typed failure without partial publication.
         */
        [[nodiscard]] Result<EditorViewportRegistration> Register(EditorSurfaceContextRegistration context,
                                                                  EditorViewportDefinition definition,
                                                                  std::shared_ptr<ExtensionRetirement> retirement);
        /** @brief Copies live geometry into fixed caller storage, in stable surface order.
         * @param output Frame overwritten even when hidden/shutdown. @param tool Current host transform tool.
         * @param editable Whether current selection has an editable transform. @param visible Host viewport visibility.
         */
        void Extract(EditorViewportFrame &output, EditorViewportTool tool, bool editable, bool visible) const noexcept;
        /** @brief Picks the topmost eligible handle using a bounded pixel-space segment distance.
         * @param frame Copied current frame. @param x Pointer normalized X. @param y Pointer normalized Y.
         * @param width Viewport logical width. @param height Viewport logical height. @return Exact live handle or empty.
         */
        [[nodiscard]] EditorViewportPick Pick(const EditorViewportFrame &frame, float x, float y, float width, float height) const noexcept;
        /** @brief Revalidates publication, policy, visibility and selected tool before advancing a host transaction.
         * @param pick Exact picked generation. @param tool Current tool. @return Current permission and visibility admission.
         */
        [[nodiscard]] bool IsLive(EditorViewportPick pick, EditorViewportTool tool) const noexcept;
        /** @brief Changes host visibility preference without extending provider authority.
         * @param provider Exact activation. @param surfaceId Registered contribution. @param visible Visibility intent.
         * @return Success or stale/unknown failure.
         */
        [[nodiscard]] Result<void> SetVisible(const EditorSurfaceProviderIdentity &provider, std::string_view surfaceId,
                                              bool visible) const;
        /** @brief Revokes all publications before host destruction; never waits for GPU or module code. */
        void BeginShutdown() noexcept;

    private:
        friend struct EditorViewportHostAccess;
        /** @brief Allocates a nonzero never-reused activation/publication generation. @return Generation or zero on closed admission. */
        [[nodiscard]] std::uint64_t AllocateGeneration() noexcept;
        struct State;
        std::unique_ptr<State> state_;
    };
}  // namespace Horo::Extensions
