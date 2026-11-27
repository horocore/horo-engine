#include "Horo/Platform/NativeCredentialBackend.h"
#include "Horo/Security/SecurityErrors.h"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <type_traits>

namespace Horo::Platform {
    namespace {
        constexpr std::size_t MaximumBytes = 2560;

        struct ReleaseNative {
            void operator()(const CFTypeRef value) const noexcept {
                CFRelease(value);
            }
        };

        template <typename Ref> using Native = std::unique_ptr<std::remove_pointer_t<Ref>, ReleaseNative>;

        /** @brief Selects the same unlocked file-based keychain for writing and searching.
         * @details The file-based keychain supports the editor's development and packaged signing identities.
         */
        Native<SecKeychainRef> OpenDefaultKeychain() noexcept {
            SecKeychainRef raw{};
            const auto opened = SecKeychainCopyDefault(&raw);
            Native<SecKeychainRef> keychain{raw};
            SecKeychainStatus status{};
            constexpr auto Required = kSecUnlockStateStatus | kSecWritePermStatus;
            if (opened != errSecSuccess || !keychain || SecKeychainGetStatus(keychain.get(), &status) != errSecSuccess ||
                (status & Required) != Required)
                return {};
            return keychain;
        }

        /** @brief Avoids forwarding native diagnostic text or credential attributes. */
        Error NativeFailure(const OSStatus status = errSecNotAvailable) {
            return MakeError(status == errSecItemNotFound ? SecurityErrors::CredentialNotFound
                                                          : SecurityErrors::CredentialBackendUnavailable);
        }

        /** @brief Creates a dictionary that retains its native keys and values. */
        Native<CFMutableDictionaryRef> NewDictionary() noexcept {
            return Native<CFMutableDictionaryRef>{
                CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks)};
        }

        /** @brief Restricts all operations to a single product service and exact opaque account identity. */
        Native<CFMutableDictionaryRef> ItemAttributes(const CredentialReference &reference) noexcept {
            const auto identity = reference.Value();
            const Native<CFStringRef> account{CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(identity.data()),
                                                                      static_cast<CFIndex>(identity.size()), kCFStringEncodingASCII,
                                                                      false)};
            auto attributes = NewDictionary();
            if (!account || !attributes)
                return {};
            CFDictionarySetValue(attributes.get(), kSecClass, kSecClassGenericPassword);
            CFDictionarySetValue(attributes.get(), kSecAttrService, CFSTR("dev.horo.engine.credentials"));
            CFDictionarySetValue(attributes.get(), kSecAttrAccount, account.get());
            return attributes;
        }

        /** @brief Limits queries to the selected keychain rather than the user's complete search list. */
        Native<CFMutableDictionaryRef> ItemQuery(const SecKeychainRef keychain, const CredentialReference &reference) noexcept {
            auto query = ItemAttributes(reference);
            const void *entries[]{keychain};
            const Native<CFArrayRef> search{CFArrayCreate(kCFAllocatorDefault, entries, 1, &kCFTypeArrayCallBacks)};
            if (!query || !search)
                return {};
            CFDictionarySetValue(query.get(), kSecMatchSearchList, search.get());
            return query;
        }

        /** @brief Maps one opaque application identity to a generic password in the current user's default keychain. */
        class MacCredentialBackend final : public Security::CredentialBackend {
        public:
            /** @copydoc Security::CredentialBackend::Available */
            bool Available() const noexcept override {
                return static_cast<bool>(OpenDefaultKeychain());
            }

            /** @copydoc Security::CredentialBackend::Put */
            Result<void> Put(const CredentialReference &reference, Security::SecureBytes secret) override {
                const auto bytes = secret.View();
                if (reference.Value().empty() || bytes.empty() || bytes.size() > MaximumBytes)
                    return Result<void>::Failure(MakeError(SecurityErrors::InvalidInput));
                const auto keychain = OpenDefaultKeychain();
                auto attributes = ItemAttributes(reference);
                // The move-only input owns the bytes until all synchronous calls and the CFData view finish.
                const Native<CFDataRef> data{CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(bytes.data()),
                                                                         static_cast<CFIndex>(bytes.size()), kCFAllocatorNull)};
                if (!keychain || !attributes || !data)
                    return Result<void>::Failure(NativeFailure());
                CFDictionarySetValue(attributes.get(), kSecUseKeychain, keychain.get());
                CFDictionarySetValue(attributes.get(), kSecValueData, data.get());
                const auto status = SecItemAdd(attributes.get(), nullptr);
                if (status != errSecDuplicateItem)
                    return status == errSecSuccess ? Result<void>::Success() : Result<void>::Failure(NativeFailure(status));

                auto query = ItemQuery(keychain.get(), reference);
                auto update = NewDictionary();
                if (!query || !update)
                    return Result<void>::Failure(NativeFailure());
                CFDictionarySetValue(update.get(), kSecValueData, data.get());
                const auto replaced = SecItemUpdate(query.get(), update.get());
                return replaced == errSecSuccess ? Result<void>::Success() : Result<void>::Failure(NativeFailure(replaced));
            }

            /** @copydoc Security::CredentialBackend::Resolve */
            Result<Security::SecureBytes> Resolve(const CredentialReference &reference) override {
                using Outcome = Result<Security::SecureBytes>;
                if (reference.Value().empty())
                    return Outcome::Failure(MakeError(SecurityErrors::InvalidInput));
                const auto keychain = OpenDefaultKeychain();
                if (!keychain)
                    return Outcome::Failure(NativeFailure());
                auto query = ItemQuery(keychain.get(), reference);
                if (!query)
                    return Outcome::Failure(NativeFailure());
                CFDictionarySetValue(query.get(), kSecReturnData, kCFBooleanTrue);
                CFTypeRef raw{};
                const auto status = SecItemCopyMatching(query.get(), &raw);
                const Native<CFTypeRef> value{raw};
                if (status != errSecSuccess)
                    return Outcome::Failure(NativeFailure(status));
                if (!value || CFGetTypeID(value.get()) != CFDataGetTypeID())
                    return Outcome::Failure(NativeFailure());
                const auto data = static_cast<CFDataRef>(value.get());
                const auto size = CFDataGetLength(data);
                if (size <= 0 || static_cast<std::size_t>(size) > MaximumBytes)
                    return Outcome::Failure(NativeFailure());
                return Outcome::Success(
                    Security::SecureBytes{{reinterpret_cast<const std::byte *>(CFDataGetBytePtr(data)), static_cast<std::size_t>(size)}});
            }

            /** @copydoc Security::CredentialBackend::Remove */
            Result<void> Remove(const CredentialReference &reference) noexcept override {
                if (reference.Value().empty())
                    return Result<void>::Failure(MakeError(SecurityErrors::InvalidInput));
                const auto keychain = OpenDefaultKeychain();
                if (!keychain)
                    return Result<void>::Failure(NativeFailure());
                const auto query = ItemQuery(keychain.get(), reference);
                if (!query)
                    return Result<void>::Failure(NativeFailure());
                const auto status = SecItemDelete(query.get());
                return status == errSecSuccess || status == errSecItemNotFound ? Result<void>::Success()
                                                                               : Result<void>::Failure(NativeFailure(status));
            }
        };
    }  // namespace

    /** @copydoc CreateNativeCredentialBackend */
    std::shared_ptr<Security::CredentialBackend> CreateNativeCredentialBackend() {
        return std::make_shared<MacCredentialBackend>();
    }
}  // namespace Horo::Platform
