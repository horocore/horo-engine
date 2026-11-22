#include "EditorCloudModelProfile.h"

#include "Horo/Security/SecurityErrors.h"

#include <algorithm>
#include <array>
#include <span>
#include <utility>

namespace Horo::Editor {
    namespace {
        constexpr std::string_view Schema = "horo-cloud-model-v1";
        constexpr std::size_t MaximumDocumentBytes = 1024;
        constexpr std::size_t MaximumFieldBytes = 256;

        /** @brief Preserves the route format and prevents hidden control bytes in a consent review. */
        bool SafeField(const std::string_view value) noexcept {
            return value.size() <= MaximumFieldBytes && std::ranges::none_of(value, [](const unsigned char byte) {
                return byte < 0x20 || byte == 0x7f;
            });
        }

        /** @brief Decodes the canonical record, including any prior binding queued for retirement. */
        std::optional<EditorCloudModelProfile> ParseProfile(std::string_view encoded) {
            std::array<std::string_view, 5> fields;
            for (auto &field : fields) {
                const auto end = encoded.find('\n');
                if (end == std::string_view::npos)
                    return std::nullopt;
                field = encoded.substr(0, end);
                encoded.remove_prefix(end + 1);
            }
            if (!encoded.empty() || fields[0] != Schema || !ValidCloudModelEndpoint(fields[1]) || !SafeField(fields[2]))
                return std::nullopt;
            auto credential = CredentialReference::Parse(fields[3]);
            if (credential.HasError())
                return std::nullopt;
            std::optional<CredentialReference> retired;
            if (!fields[4].empty()) {
                auto parsed = CredentialReference::Parse(fields[4]);
                if (parsed.HasError() || parsed.Value() == credential.Value())
                    return std::nullopt;
                retired.emplace(std::move(parsed).Value());
            }
            return EditorCloudModelProfile{std::string{fields[1]}, std::string{fields[2]}, std::move(credential).Value(),
                                           std::move(retired)};
        }
    }  // namespace

    /** @copydoc ValidCloudModelEndpoint */
    bool ValidCloudModelEndpoint(std::string_view endpoint) noexcept {
        if (!endpoint.starts_with("https://") || !SafeField(endpoint) || endpoint.find_first_of(" @?#\\") != std::string_view::npos)
            return false;
        endpoint.remove_prefix(std::string_view{"https://"}.size());
        return !endpoint.empty() && endpoint.front() != '/';
    }

    /** @copydoc EditorCloudModelProfileStore::EditorCloudModelProfileStore */
    EditorCloudModelProfileStore::EditorCloudModelProfileStore(std::filesystem::path path,
                                                               std::shared_ptr<Security::PersistentCredentialStore> credentials)
        : path_(std::move(path)), credentials_(std::move(credentials)) {}

    /** @copydoc EditorCloudModelProfileStore::Load */
    Result<std::optional<EditorCloudModelProfile>> EditorCloudModelProfileStore::Load() const {
        using Outcome = Result<std::optional<EditorCloudModelProfile>>;
        if (!path_.is_absolute())
            return Outcome::Failure(MakeError(SecurityErrors::InvalidInput));
        const auto entry = files_.InspectEntry(path_);
        if (entry.HasError())
            return Outcome::Failure(entry.ErrorValue());
        if (!entry.Value())
            return Outcome::Success(std::nullopt);
        const auto bytes = files_.ReadFileBounded(path_, MaximumDocumentBytes);
        if (bytes.HasError())
            return Outcome::Failure(bytes.ErrorValue());
        const auto &contents = bytes.Value();
        if (!contents.empty()) {
            if (auto decoded = ParseProfile({reinterpret_cast<const char *>(contents.data()), contents.size()}))
                return Outcome::Success(std::move(decoded));
        }
        return Outcome::Failure(MakeError(SecurityErrors::InvalidInput));
    }

    /** @brief Takes the writer lease before comparing the live binding with the caller's snapshot. */
    Result<ExclusiveFileLock> EditorCloudModelProfileStore::LockCurrent(const std::optional<EditorCloudModelProfile> &expected) {
        using Outcome = Result<ExclusiveFileLock>;
        if (!path_.is_absolute())
            return Outcome::Failure(MakeError(SecurityErrors::InvalidInput));
        auto lockPath = path_;
        lockPath += ".lock";
        auto lease = files_.TryAcquireExclusive(lockPath, "horo.editor.cloud-model");
        if (lease.HasError())
            return lease;
        const auto current = Load();
        if (current.HasError())
            return Outcome::Failure(current.ErrorValue());
        if (current.Value() != expected)
            return Outcome::Failure(MakeError(SecurityErrors::InvalidInput));
        return lease;
    }

    /** @brief Publishes one bounded profile while the caller holds the writer lease. */
    bool EditorCloudModelProfileStore::Publish(const EditorCloudModelProfile &profile) {
        auto text = std::string{Schema} + '\n' + profile.endpoint + '\n' + profile.model + '\n';
        text.append(profile.credential.Value());
        text += '\n';
        if (profile.retired)
            text.append(profile.retired->Value());
        text += '\n';
        auto prepared = path_;
        prepared += ".pending";
        if (const auto removed = files_.RemoveDurable(prepared); removed.HasError())
            return false;
        bool created{};
        if (const auto written = files_.WritePrivateDurable(prepared, std::as_bytes(std::span{text}), created); written.HasError()) {
            if (created)
                static_cast<void>(files_.RemoveDurable(prepared));
            return false;
        }
        AtomicFileReplacementReceipt receipt;
        const auto replaced = files_.AtomicReplaceTracked(prepared, path_, receipt);
        if (!receipt.WasCommitted() && replaced.HasError())
            static_cast<void>(files_.RemoveDurable(prepared));
        return receipt.WasCommitted();
    }

    /** @copydoc EditorCloudModelProfileStore::Enroll */
    Result<EditorCloudModelProfile> EditorCloudModelProfileStore::Enroll(const std::optional<EditorCloudModelProfile> &expected,
                                                                         std::string endpoint, std::string model,
                                                                         Security::SecureBytes key) {
        using Outcome = Result<EditorCloudModelProfile>;
        if (!credentials_ || !ValidCloudModelEndpoint(endpoint) || !SafeField(model))
            return Outcome::Failure(MakeError(SecurityErrors::InvalidInput));
        const auto lease = LockCurrent(expected);
        if (lease.HasError())
            return Outcome::Failure(lease.ErrorValue());
        if (expected && expected->retired) {
            const auto retired = credentials_->Revoke(*expected->retired);
            if (retired.HasError())
                return Outcome::Failure(retired.ErrorValue());
        }
        auto enrolled = credentials_->Enroll(std::move(key));
        if (enrolled.HasError())
            return Outcome::Failure(enrolled.ErrorValue());
        EditorCloudModelProfile next{std::move(endpoint), std::move(model), std::move(enrolled).Value()};
        if (expected)
            next.retired = expected->credential;
        if (!Publish(next)) {
            static_cast<void>(credentials_->Revoke(next.credential));
            return Outcome::Failure(MakeError(SecurityErrors::CredentialBackendUnavailable));
        }
        // The committed profile retains the old identity even if the OS cannot retire it yet.
        if (next.retired)
            static_cast<void>(credentials_->Revoke(*next.retired));
        return Outcome::Success(std::move(next));
    }

    /** @copydoc EditorCloudModelProfileStore::SelectModel */
    Result<EditorCloudModelProfile> EditorCloudModelProfileStore::SelectModel(const EditorCloudModelProfile &expected, std::string model) {
        using Outcome = Result<EditorCloudModelProfile>;
        if (!SafeField(model) || model.empty())
            return Outcome::Failure(MakeError(SecurityErrors::InvalidInput));
        const auto lease = LockCurrent(expected);
        if (lease.HasError())
            return Outcome::Failure(lease.ErrorValue());
        EditorCloudModelProfile next{expected.endpoint, std::move(model), expected.credential, expected.retired};
        if (!Publish(next))
            return Outcome::Failure(MakeError(SecurityErrors::CredentialBackendUnavailable));
        return Outcome::Success(std::move(next));
    }

    /** @copydoc EditorCloudModelProfileStore::Revoke */
    Result<void> EditorCloudModelProfileStore::Revoke(const EditorCloudModelProfile &expected) {
        if (!credentials_)
            return Result<void>::Failure(MakeError(SecurityErrors::CredentialBackendUnavailable));
        const auto lease = LockCurrent(expected);
        if (lease.HasError())
            return Result<void>::Failure(lease.ErrorValue());
        if (expected.retired) {
            const auto retired = credentials_->Revoke(*expected.retired);
            if (retired.HasError())
                return retired;
        }
        if (const auto revoked = credentials_->Revoke(expected.credential); revoked.HasError())
            return revoked;
        return files_.RemoveDurable(path_);
    }

    /** @copydoc EditorCloudModelProfileStore::Provider */
    std::shared_ptr<Security::CredentialProvider> EditorCloudModelProfileStore::Provider() const {
        return credentials_;
    }
}  // namespace Horo::Editor
