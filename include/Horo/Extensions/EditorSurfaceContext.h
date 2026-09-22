#pragma once

/**
 * @file EditorSurfaceContext.h
 * @brief Activation-scoped, backend-neutral access context for extension editor surfaces.
 */

#include "Horo/Extensions/EditorSurfaceDescriptor.h"
#include "Horo/Foundation/Result.h"

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace Horo::Extensions {
    struct EditorSurfaceContextProviderState;
    struct EditorSurfaceContextState;

    /** @brief Stable identity of a command admitted to one extension surface. */
    struct EditorSurfaceCommandId final {
        std::string value; /**< Canonical command identity resolved by the host. */

        bool operator==(const EditorSurfaceCommandId &) const noexcept = default;
    };

    /** @brief Stable key of bounded presentation or draft state admitted to one extension surface. */
    struct EditorSurfaceStateKey final {
        std::string value; /**< Canonical state key resolved by the host. */

        bool operator==(const EditorSurfaceStateKey &) const noexcept = default;
    };

    /** @brief Stable identity of a backend-neutral service admitted to one extension surface. */
    struct EditorSurfaceServiceId final {
        std::string value; /**< Canonical service identity resolved by the host. */

        bool operator==(const EditorSurfaceServiceId &) const noexcept = default;
    };

    /** @brief Stable localization key admitted to one extension surface. */
    struct EditorSurfaceLocalizationKey final {
        std::string value; /**< Canonical dotted message key resolved by the host. */

        bool operator==(const EditorSurfaceLocalizationKey &) const noexcept = default;
    };

    /** @brief Stable diagnostic identity that an extension surface may present or emit. */
    struct EditorSurfaceDiagnosticCode final {
        std::string value; /**< Canonical diagnostic identity resolved by the host. */

        bool operator==(const EditorSurfaceDiagnosticCode &) const noexcept = default;
    };

    /** @brief Finite bounds applied to the access lists carried by one surface context. */
    struct EditorSurfaceContextLimits final {
        std::size_t maximumIdentityBytes{256};        /**< Maximum command, state, service, or diagnostic identity bytes. */
        std::size_t maximumLocalizationKeyBytes{256}; /**< Maximum localization-key bytes. */
        std::size_t maximumCommands{64};              /**< Maximum admitted command identities. */
        std::size_t maximumStateKeys{64};             /**< Maximum admitted state keys. */
        std::size_t maximumServices{64};              /**< Maximum admitted service identities. */
        std::size_t maximumLocalizationKeys{128};     /**< Maximum admitted localization keys. */
        std::size_t maximumDiagnostics{128};          /**< Maximum admitted diagnostic identities. */
        std::size_t maximumCapabilities{64};          /**< Maximum admitted capability grants. */
        std::size_t maximumContexts{256};             /**< Maximum simultaneously attached surface contexts. */
    };

    /**
     * @brief Host-approved access identities for one extension surface activation.
     *
     * The lists are allowlists, not service locators. They describe which typed
     * host adapters may be resolved later; they do not contain callbacks,
     * mutable editor models, persistence authorities, or native objects.
     */
    struct EditorSurfaceContextDescriptor final {
        EditorSurfaceDescriptor surface;                        /**< Inert surface metadata and exact provider owner. */
        std::vector<EditorSurfaceCommandId> commands;           /**< Commands approved for this contribution. */
        std::vector<EditorSurfaceStateKey> state;               /**< State keys approved for this contribution. */
        std::vector<EditorSurfaceServiceId> services;           /**< Backend-neutral services approved for this contribution. */
        std::vector<EditorSurfaceLocalizationKey> localization; /**< Localization keys approved for this contribution. */
        std::vector<EditorSurfaceDiagnosticCode> diagnostics;   /**< Diagnostic identities approved for this contribution. */
    };

    /**
     * @brief Validates context metadata without attaching a provider or touching editor state.
     * @param descriptor Context metadata and its bounded access allowlists.
     * @param limits Host-owned finite resource bounds; zero collection bounds express an empty allowlist.
     * @return Success or a typed context/descriptor rejection; the input is never modified.
     * @post No service locator, editor host, renderer, callback, or native handle is accessed.
     */
    [[nodiscard]] Result<void> ValidateEditorSurfaceContextDescriptor(const EditorSurfaceContextDescriptor &descriptor,
                                                                      const EditorSurfaceContextLimits &limits = {});

    /**
     * @brief Copyable view of one provider-owned surface context.
     *
     * Copies retain only immutable metadata and revocable activation evidence.
     * Provider reset or activation revocation makes every copy unusable. The
     * context never owns an editor model, renderer resource, native handle, or
     * callback target.
     */
    class EditorSurfaceContext final {
    public:
        EditorSurfaceContext(const EditorSurfaceContext &) = default;
        EditorSurfaceContext &operator=(const EditorSurfaceContext &) = default;
        EditorSurfaceContext(EditorSurfaceContext &&) noexcept = default;
        EditorSurfaceContext &operator=(EditorSurfaceContext &&) noexcept = default;

        /** @brief Returns whether this context still admits host-mediated access. */
        [[nodiscard]] bool IsUsable() const noexcept;

        /** @brief Returns the inert descriptor that established this context. */
        [[nodiscard]] const EditorSurfaceDescriptor &Surface() const noexcept;

        /** @brief Returns the exact provider identity and activation generation. */
        [[nodiscard]] const EditorSurfaceProviderIdentity &Provider() const noexcept;

        /** @brief Returns the approved command identities in host-provided order. */
        [[nodiscard]] std::span<const EditorSurfaceCommandId> Commands() const noexcept;

        /** @brief Returns the approved state keys in host-provided order. */
        [[nodiscard]] std::span<const EditorSurfaceStateKey> State() const noexcept;

        /** @brief Returns the approved backend-neutral service identities in host-provided order. */
        [[nodiscard]] std::span<const EditorSurfaceServiceId> Services() const noexcept;

        /** @brief Returns the approved localization keys in host-provided order. */
        [[nodiscard]] std::span<const EditorSurfaceLocalizationKey> Localization() const noexcept;

        /** @brief Returns the approved diagnostic identities in host-provided order. */
        [[nodiscard]] std::span<const EditorSurfaceDiagnosticCode> Diagnostics() const noexcept;

        /**
         * @brief Returns an activation-scoped use lease for one approved capability.
         * @param capability Exact capability identity requested by the surface adapter.
         * @return A move-only lease, or an explicit unavailable/revoked context failure.
         * @note The lease must remain alive for the complete host-mediated operation.
         */
        [[nodiscard]] Result<ExtensionCapabilityUseLease> AcquireCapabilityUse(const ExtensionCapabilityId &capability) const;

        /** @brief Tests whether a command is present in this context's allowlist. */
        [[nodiscard]] bool Allows(const EditorSurfaceCommandId &command) const noexcept;

        /** @brief Tests whether a state key is present in this context's allowlist. */
        [[nodiscard]] bool Allows(const EditorSurfaceStateKey &state) const noexcept;

        /** @brief Tests whether a service is present in this context's allowlist. */
        [[nodiscard]] bool Allows(const EditorSurfaceServiceId &service) const noexcept;

        /** @brief Tests whether a localization key is present in this context's allowlist. */
        [[nodiscard]] bool Allows(const EditorSurfaceLocalizationKey &key) const noexcept;

        /** @brief Tests whether a diagnostic identity is present in this context's allowlist. */
        [[nodiscard]] bool Allows(const EditorSurfaceDiagnosticCode &diagnostic) const noexcept;

    private:
        friend class EditorSurfaceContextProvider;
        friend class EditorSurfaceContextRegistration;

        explicit EditorSurfaceContext(std::shared_ptr<const EditorSurfaceContextState> state) noexcept;

        std::shared_ptr<const EditorSurfaceContextState> state_;
    };

    /**
     * @brief Move-only ownership of one attached surface context.
     *
     * Reset revokes access before releasing the registration. A context copied
     * from Context() remains safe to destroy after reset, but cannot admit a new
     * capability use.
     */
    class EditorSurfaceContextRegistration final {
    public:
        ~EditorSurfaceContextRegistration() noexcept;
        EditorSurfaceContextRegistration(const EditorSurfaceContextRegistration &) = delete;
        EditorSurfaceContextRegistration &operator=(const EditorSurfaceContextRegistration &) = delete;
        EditorSurfaceContextRegistration(EditorSurfaceContextRegistration &&other) noexcept;
        EditorSurfaceContextRegistration &operator=(EditorSurfaceContextRegistration &&other) noexcept;

        /** @brief Revokes context access and removes this registration from its provider. */
        void Reset() const noexcept;

        /** @brief Reports whether the provider and activation still admit this context. */
        [[nodiscard]] bool IsRegistered() const noexcept;

        /**
         * @brief Returns the immutable context view owned by this registration.
         * @pre This registration has not been moved from.
         * @return Borrowed context view valid until this registration is destroyed or reset.
         */
        [[nodiscard]] const EditorSurfaceContext &Context() const noexcept;

    private:
        friend class EditorSurfaceContextProvider;

        EditorSurfaceContextRegistration(std::weak_ptr<EditorSurfaceContextProviderState> provider,
                                         std::shared_ptr<EditorSurfaceContextState> context, EditorSurfaceContext view) noexcept;

        mutable std::weak_ptr<EditorSurfaceContextProviderState> provider_;
        mutable std::shared_ptr<EditorSurfaceContextState> context_;
        mutable EditorSurfaceContext view_;
    };

    /**
     * @brief Host-owned factory and teardown authority for extension surface contexts.
     *
     * A provider must be retained by the host for no longer than the matching
     * extension activation. Shutting it down revokes every attached context and
     * rejects all subsequent registrations.
     */
    class EditorSurfaceContextProvider final {
    public:
        explicit EditorSurfaceContextProvider(const EditorSurfaceContextLimits &limits = {});
        ~EditorSurfaceContextProvider() noexcept;
        EditorSurfaceContextProvider(const EditorSurfaceContextProvider &) = delete;
        EditorSurfaceContextProvider &operator=(const EditorSurfaceContextProvider &) = delete;
        EditorSurfaceContextProvider(EditorSurfaceContextProvider &&) noexcept = delete;
        EditorSurfaceContextProvider &operator=(EditorSurfaceContextProvider &&) noexcept = delete;

        /**
         * @brief Attaches one context to an exact live extension activation.
         * @param descriptor Validated surface metadata and host-approved allowlists.
         * @param activation Exact activation lease; revocation invalidates the context.
         * @param approvedCapabilities Capability grants admitted for this surface; omitted grants stay unavailable.
         * @return Move-only registration, or a typed validation, ownership, capacity, revocation, or shutdown failure.
         */
        [[nodiscard]] Result<EditorSurfaceContextRegistration> Attach(EditorSurfaceContextDescriptor descriptor,
                                                                      ExtensionActivationLease activation,
                                                                      std::span<const ExtensionCapabilityHandle> approvedCapabilities = {});

        /** @brief Revokes all attached contexts and closes future attachment admission. */
        void BeginShutdown() noexcept;

        /** @brief Reports whether this provider has entered terminal shutdown. */
        [[nodiscard]] bool IsShutdown() const noexcept;

    private:
        std::shared_ptr<EditorSurfaceContextProviderState> state_;
        EditorSurfaceContextLimits limits_;
    };
}  // namespace Horo::Extensions
