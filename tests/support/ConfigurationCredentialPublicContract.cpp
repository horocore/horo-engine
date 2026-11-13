#include "Horo/Foundation/Configuration.h"
#include "Horo/Security/CredentialStore.h"

#include <type_traits>

static_assert(std::is_same_v<Horo::CredentialReference, Horo::Security::CredentialReference>);
static_assert(std::is_constructible_v<Horo::SettingValue, Horo::Security::CredentialReference>);
static_assert(std::is_same_v<decltype(&Horo::Security::ResolveConfigurationCredential),
                             Horo::Result<Horo::Security::SecureBytes> (*)(const Horo::ConfigurationSnapshot &, const Horo::SettingKey &,
                                                                           Horo::Security::CredentialProvider *, std::uint64_t)>);
