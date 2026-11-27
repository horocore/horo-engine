#pragma once

/** @file EditorCloudModelProfile.h @brief User-scoped cloud connection preferences. */

#include "Horo/Foundation/Platform.h"
#include "Horo/Security/CredentialStore.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace Horo::Editor {
    /** @brief One saved connection containing only non-secret routing information. */
    struct EditorCloudModelProfile final {
        std::string endpoint;
        std::string model;
        CredentialReference credential;
        std::optional<CredentialReference> retired; /**< Prior binding retained for idempotent retirement. */
        bool operator==(const EditorCloudModelProfile &) const = default;
    };

    /** @brief Validates a bounded HTTPS base address for credential binding.
     * @param endpoint Complete provider base address.
     * @return True when the address can be stored and displayed for review. */
    [[nodiscard]] bool ValidCloudModelEndpoint(std::string_view endpoint) noexcept;

    /** @brief Owns one user-wide route and its durable credential enrollment.
     * @details Operations are synchronous. The editor serializes them with model requests on its background lane. A native
     * file lock and expected-profile comparison reject stale updates from other editor processes.
     */
    class EditorCloudModelProfileStore final {
    public:
        /** @brief Selects a user configuration file and its native credential provider.
         * @param path Host-admitted absolute user configuration path.
         * @param credentials Credential owner retained for the store's lifetime. */
        EditorCloudModelProfileStore(std::filesystem::path path, std::shared_ptr<Security::PersistentCredentialStore> credentials);

        /** @brief Reads the route and opaque identity without accessing the credential provider.
         * @return The saved profile, absence, or a typed read or validation failure. */
        [[nodiscard]] Result<std::optional<EditorCloudModelProfile>> Load() const;

        /** @brief Enrolls a key for this address and atomically publishes its non-secret route.
         * @param expected Exact prior profile, or absence for first enrollment.
         * @param endpoint HTTPS base address that owns this binding.
         * @param model Initial model name, or empty until chosen.
         * @param key Secret consumed by the native credential provider.
         * @return Published profile or typed failure. */
        [[nodiscard]] Result<EditorCloudModelProfile> Enroll(const std::optional<EditorCloudModelProfile> &expected, std::string endpoint,
                                                             std::string model, Security::SecureBytes key);

        /** @brief Publishes the model choice under the still-current credential binding.
         * @param expected Exact profile currently shown to the user.
         * @param model Nonempty model identifier.
         * @return Published profile or typed failure. */
        [[nodiscard]] Result<EditorCloudModelProfile> SelectModel(const EditorCloudModelProfile &expected, std::string model);

        /** @brief Revokes the exact active key and removes the user connection.
         * @param expected Exact profile currently shown to the user.
         * @return Success when both the key and its route are withdrawn, or typed failure. */
        [[nodiscard]] Result<void> Revoke(const EditorCloudModelProfile &expected);

        /** @brief Supplies the operation-local credential lookup authority.
         * @return Shared provider that keeps the native authority alive through synchronous transport. */
        [[nodiscard]] std::shared_ptr<Security::CredentialProvider> Provider() const;

    private:
        [[nodiscard]] Result<ExclusiveFileLock> LockCurrent(const std::optional<EditorCloudModelProfile> &expected);
        [[nodiscard]] bool Publish(const EditorCloudModelProfile &profile);

        std::filesystem::path path_;
        std::shared_ptr<Security::PersistentCredentialStore> credentials_;
        NativeDurableFileSystem files_;
    };
}  // namespace Horo::Editor
