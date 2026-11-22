#include "Horo/Platform/NativeCredentialBackend.h"
#include "Horo/Security/SecurityErrors.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <algorithm>
#include <array>
#include <optional>
#include <wincred.h>

namespace Horo::Platform {
    namespace {
        constexpr std::wstring_view TargetPrefix = L"HoroEngine/credentials/";
        using Target = std::array<wchar_t, TargetPrefix.size() + 64>;

        /** @brief Build the private namespace from an already validated ASCII identity without allocating. */
        std::optional<Target> CredentialTarget(const Security::CredentialReference &reference) noexcept {
            const auto value = reference.Value();
            Target target{};
            if (value.empty() || value.size() >= target.size() - TargetPrefix.size())
                return std::nullopt;
            const auto output = std::ranges::copy(TargetPrefix, target.begin()).out;
            std::ranges::transform(value, output, [](const char character) {
                return static_cast<wchar_t>(character);
            });
            return target;
        }

        /** @brief Sanitize the operation outcome instead of forwarding native diagnostics. */
        Error NativeFailure(const DWORD code) {
            return MakeError(code == ERROR_NOT_FOUND ? SecurityErrors::CredentialNotFound : SecurityErrors::CredentialBackendUnavailable);
        }

        /** @brief Clear the operating system's returned plaintext copy before giving the allocation back. */
        void FreeCredential(CREDENTIALW *credential) noexcept {
            if (credential->CredentialBlob && credential->CredentialBlobSize <= CRED_MAX_CREDENTIAL_BLOB_SIZE) {
                Security::SecureZero({reinterpret_cast<std::byte *>(credential->CredentialBlob), credential->CredentialBlobSize});
            }
            CredFree(credential);
        }

        /** @brief Maps one opaque reference to a generic credential scoped to this user and machine. */
        class WindowsCredentialBackend final : public Security::CredentialBackend {
        public:
            /** @copydoc Security::CredentialBackend::Available */
            bool Available() const noexcept override {
                std::array<DWORD, CRED_TYPE_MAXIMUM> persistence{};
                return CredGetSessionTypes(static_cast<DWORD>(persistence.size()), persistence.data()) &&
                       persistence[CRED_TYPE_GENERIC] >= CRED_PERSIST_LOCAL_MACHINE;
            }

            /** @copydoc Security::CredentialBackend::Put */
            Result<void> Put(const Security::CredentialReference &reference, Security::SecureBytes secret) override {
                auto target = CredentialTarget(reference);
                const auto bytes = secret.View();
                if (!target || bytes.empty() || bytes.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE)
                    return Result<void>::Failure(MakeError(SecurityErrors::InvalidInput));
                if (!Available())
                    return Result<void>::Failure(MakeError(SecurityErrors::CredentialBackendUnavailable));
                CREDENTIALW credential{};
                credential.Type = CRED_TYPE_GENERIC;
                credential.TargetName = target->data();
                credential.CredentialBlobSize = static_cast<DWORD>(bytes.size());
                // CredWriteW reads the input blob. Its C structure also serves as the mutable output of CredReadW.
                credential.CredentialBlob = const_cast<BYTE *>(reinterpret_cast<const BYTE *>(bytes.data()));
                credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
                if (!CredWriteW(&credential, 0))
                    return Result<void>::Failure(NativeFailure(GetLastError()));
                return Result<void>::Success();
            }

            /** @copydoc Security::CredentialBackend::Resolve */
            Result<Security::SecureBytes> Resolve(const Security::CredentialReference &reference) override {
                using Outcome = Result<Security::SecureBytes>;
                const auto target = CredentialTarget(reference);
                if (!target)
                    return Outcome::Failure(MakeError(SecurityErrors::InvalidInput));
                CREDENTIALW *native{};
                if (!CredReadW(target->data(), CRED_TYPE_GENERIC, 0, &native))
                    return Outcome::Failure(NativeFailure(GetLastError()));
                const std::unique_ptr<CREDENTIALW, decltype(&FreeCredential)> credential{native, &FreeCredential};
                if (!credential || credential->Persist != CRED_PERSIST_LOCAL_MACHINE || !credential->CredentialBlob ||
                    credential->CredentialBlobSize == 0 || credential->CredentialBlobSize > CRED_MAX_CREDENTIAL_BLOB_SIZE)
                    return Outcome::Failure(MakeError(SecurityErrors::CredentialBackendUnavailable));
                return Outcome::Success(Security::SecureBytes{
                    {reinterpret_cast<const std::byte *>(credential->CredentialBlob), credential->CredentialBlobSize}});
            }

            /** @copydoc Security::CredentialBackend::Remove */
            Result<void> Remove(const Security::CredentialReference &reference) noexcept override {
                const auto target = CredentialTarget(reference);
                if (!target)
                    return Result<void>::Failure(MakeError(SecurityErrors::InvalidInput));
                if (CredDeleteW(target->data(), CRED_TYPE_GENERIC, 0))
                    return Result<void>::Success();
                const auto failure = GetLastError();
                if (failure == ERROR_NOT_FOUND)
                    return Result<void>::Success();
                return Result<void>::Failure(NativeFailure(failure));
            }
        };
    }  // namespace

    /** @copydoc CreateNativeCredentialBackend */
    std::shared_ptr<Security::CredentialBackend> CreateNativeCredentialBackend() {
        return std::make_shared<WindowsCredentialBackend>();
    }
}  // namespace Horo::Platform
