#pragma once
/** @file EditorInspectorHost.h @brief Copied, permissioned declarative inspector contributions; document authority remains in the host. */
#include "Horo/Extensions/EditorSurfaceRegistry.h"
#include "Horo/Extensions/EditorUiForm.h"
#include "Horo/Extensions/ExtensionRetirement.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Horo::Extensions {
    inline const ExtensionCapabilityId EditorInspectorCapability{"horo.editor.inspector"};
    inline const ExtensionCapabilityId EditorPropertyDrawerCapability{"horo.editor.property-drawer"};
    /** @brief Closed presentation profile; vector/quaternion/opaque data retains the owning generic inspector. */
    enum class EditorInspectorFieldKind : std::uint8_t {
        Boolean,
        Integer,
        Number,
        String
    };
    /** @brief Exact scalar edit value; signed integers never cross a double intermediary. */
    using EditorInspectorScalar = std::variant<bool, std::int64_t, double, std::string>;

    /** @brief One declared behavior field and host-owned primitive constraints, never a serialization path. */
    struct EditorInspectorField final {
        std::string field;
        EditorInspectorFieldKind kind{EditorInspectorFieldKind::Boolean};
        EditorUiId node;
        std::optional<std::int64_t> minimumInteger;
        std::optional<std::int64_t> maximumInteger;
        std::optional<double> minimumNumber;
        std::optional<double> maximumNumber;
        std::size_t maximumStringBytes{4096};
    };

    /** @brief Copied package message; no provider text pointer survives registration. */
    struct EditorInspectorMessage final {
        std::string locale;
        std::string key;
        std::string value;
    };

    /** @brief Complete inert declaration for one behavior schema or one field drawer.
     * @details field is empty for an Inspector, exact and nonempty for a PropertyDrawer. The host descriptor must resolve every
     * declared field/type before a projection can replace generic controls. Defaults in form nodes are never authored values.
     */
    struct EditorInspectorDefinition final {
        std::string behaviorType;
        std::uint32_t behaviorSchemaVersion{1};
        std::string field;
        EditorUiForm form;
        std::vector<EditorInspectorField> fields;
        std::vector<EditorInspectorMessage> messages;
    };

    /** @brief Validates inert ownership, layout, localization and complete field bindings without activating anything.
     * @param surface Manifest-owned Inspector/PropertyDrawer metadata. @param definition Copied declaration.
     * @return Success or typed invalid/unsupported failure; no service discovery, callbacks or document access.
     */
    [[nodiscard]] Result<void> ValidateEditorInspectorDefinition(const EditorSurfaceDescriptor &surface,
                                                                 const EditorInspectorDefinition &definition);
    /** @brief Validates a current exact scalar against an admitted declared field.
     * @param field Immutable admitted field. @param value Owned candidate.
     * @return Success or type/finite/bound rejection without clamping, conversion or mutation.
     */
    [[nodiscard]] Result<void> ValidateEditorInspectorValue(const EditorInspectorField &field, const EditorInspectorScalar &value);
    /** @brief Checks the actual field node and all container ancestors before admitting a user edit.
     * @param definition Admitted immutable declaration. @param field Exact admitted binding.
     * @return False for disabled/read-only/unfocusable nodes, absent parents or malformed/cyclic ancestry.
     */
    [[nodiscard]] bool CanEditEditorInspectorField(const EditorInspectorDefinition &definition, const EditorInspectorField &field) noexcept;
    struct EditorInspectorSession;

    /** @brief Copied discovery metadata; contexts are revocable authority views, never document/model/native handles. */
    struct EditorInspectorContribution final {
        EditorSurfaceContext context;
        std::shared_ptr<const EditorInspectorDefinition> definition;
    };

    /** @brief Move-only publication owner; destruction removes discovery before dropping its attributed provider lease. */
    class EditorInspectorRegistration final {
    public:
        /** @brief Closes publication and input admission. */
        ~EditorInspectorRegistration();
        /** @brief Transfers exact publication ownership without extending provider authority. @param other Source owner. */
        EditorInspectorRegistration(EditorInspectorRegistration &&other) noexcept;
        /** @brief Revokes prior publication then transfers exact ownership. @param other Source owner. @return This registration. */
        EditorInspectorRegistration &operator=(EditorInspectorRegistration &&other) noexcept;
        EditorInspectorRegistration(const EditorInspectorRegistration &) = delete;
        EditorInspectorRegistration &operator=(const EditorInspectorRegistration &) = delete;
        /** @brief Revokes this exact contribution; repeated revocation is safe. */
        void Reset() noexcept;
        /** @brief Tests current activation admission. @return False after revocation or retirement. */
        [[nodiscard]] bool IsRegistered() const noexcept;

    private:
        friend class EditorInspectorHost;
        /** @brief Adopts one fully published session. @param session Exclusive publication owner token. */
        explicit EditorInspectorRegistration(std::shared_ptr<EditorInspectorSession> session) noexcept;
        std::shared_ptr<EditorInspectorSession> session_;
    };

    /** @brief Owner-lane restricted declaration host; all registered metadata is copied and no provider code runs during draw.
     * @details Upper editor composition supplies the single registry and sealed policy. Native module registration is staged until
     * its entire activation transaction commits. Contexts expire on retirement; callers must check current capability use and exact
     * provider generation again immediately before submitting a prepared document transaction. No workspace/draft/configuration state
     * is stored here. All methods are owner-lane only, including destruction and shutdown.
     */
    class EditorInspectorHost final {
    public:
        /** @brief Composes borrowed single registry and copied host admission evidence.
         * @param registry Host registry outliving this facade. @param policy Sealed permission and capability evidence.
         */
        EditorInspectorHost(EditorSurfaceRegistry &registry, ExtensionAdmissionPolicy policy);
        /** @brief Revokes every remaining declaration at the owner-lane shutdown boundary. */
        ~EditorInspectorHost();
        EditorInspectorHost(const EditorInspectorHost &) = delete;
        EditorInspectorHost &operator=(const EditorInspectorHost &) = delete;
        /** @brief Borrows the sealed policy used by the native admission adapter. @return Immutable policy for this host lifetime. */
        [[nodiscard]] const ExtensionAdmissionPolicy &AdmissionPolicy() const noexcept;
        /** @brief Registers one complete already-admitted declaration atomically.
         * @param context Revocable exact manifest owner. @param definition Complete copied schema/localization.
         * @param retirement Real native provider retirement owner with verified executable closure.
         * @return Move-only publication or original typed failure; no partial registry/session publication.
         */
        [[nodiscard]] Result<EditorInspectorRegistration> Register(EditorSurfaceContextRegistration context,
                                                                   EditorInspectorDefinition definition,
                                                                   std::shared_ptr<ExtensionRetirement> retirement);
        /** @brief Returns this inspector catalog publication/revocation epoch, independent of activity form updates. @return Nonzero
         * current epoch; zero on wrong-thread/shutdown/exhaustion. */
        [[nodiscard]] std::uint64_t Revision() const noexcept;
        /** @brief Copies live matching declarations in deterministic provider/surface order outside draw traversal.
         * @param behaviorType Host-resolved canonical type. @param schemaVersion Exact current owner schema.
         * @return Copied declaration/context views or thread/shutdown failure; missing providers return an empty collection.
         */
        [[nodiscard]] Result<std::vector<EditorInspectorContribution>> Contributions(std::string_view behaviorType,
                                                                                     std::uint32_t schemaVersion) const;
        /** @brief Checks current owner identity, publication and every required capability without allocating a discovery list.
         * @param contribution Previously captured exact declaration. @return True while this host still admits its presentation.
         */
        [[nodiscard]] bool IsAvailable(const EditorInspectorContribution &contribution) const noexcept;
        /** @brief Resolves only copied admitted package text with explicit en-US fallback.
         * @param contribution Exact live context and declaration. @param key Approved localization key. @param locale Current host locale.
         * @return Borrowed copied text or original key; valid while contribution retains its immutable definition.
         */
        [[nodiscard]] std::string_view LocalizedText(const EditorInspectorContribution &contribution, std::string_view key,
                                                     std::string_view locale) const noexcept;
        /** @brief Revokes every publication; never waits for callbacks or alters document bytes. */
        void BeginShutdown() noexcept;

    private:
        friend struct EditorInspectorHostAccess;
        /** @brief Allocates one nonzero native publication generation, failing closed on shutdown/thread mismatch/overflow. */
        [[nodiscard]] std::uint64_t AllocateGeneration() noexcept;
        struct State;
        std::unique_ptr<State> state_;
    };
}  // namespace Horo::Extensions
