#include "Horo/Editor/Localization/LocalizationService.h"

#include <algorithm>
#include <fstream>
#include <functional>
#include <regex>
#include <utility>

namespace Horo::Editor {
    namespace {
        [[nodiscard]] bool IsAlpha(const char value) noexcept {
            return std::isalpha(static_cast<unsigned char>(value)) != 0;
        }

        [[nodiscard]] bool IsAlnumOrHyphen(const char value) noexcept {
            return std::isalnum(static_cast<unsigned char>(value)) != 0 || value == '-';
        }

        [[nodiscard]] std::string DecodeEscapedJsonText(std::string_view encoded) {
            std::string decoded;
            decoded.reserve(encoded.size());
            const char *pos = encoded.data();
            const char *const end = pos + encoded.size();
            while (pos < end) {
                if (const char value = *pos; value == '\\' && pos + 1 < end) {
                    ++pos;
                    if (const char escaped = *pos; escaped == 'n')
                        decoded += '\n';
                    else if (escaped == 't')
                        decoded += '\t';
                    else
                        decoded += escaped;
                } else {
                    decoded += value;
                }
                ++pos;
            }
            return decoded;
        }

        [[nodiscard]] std::string NormalizeTag(std::string_view tag) {
            std::string normalized(tag);
            if (normalized.size() >= 2 && !IsAlpha(normalized[0]))
                return {};
            for (char &value : normalized)
                value = static_cast<char>(std::tolower(static_cast<unsigned char>(value)));

            std::size_t partStart = 0;
            bool firstPart = true;
            while (partStart < normalized.size()) {
                const std::size_t partEnd = normalized.find('-', partStart);
                if (const std::size_t length = partEnd == std::string::npos ? normalized.size() - partStart : partEnd - partStart;
                    length == 0 || (firstPart && (length != 2 && length != 3)))
                    return {};
                if (partEnd == std::string::npos)
                    break;
                partStart = partEnd + 1;
                firstPart = false;
            }

            if (std::ranges::any_of(normalized, [](const char value) {
                return !IsAlnumOrHyphen(value);
            }))
                return {};

            if (normalized.size() >= 5 && normalized[2] == '-') {
                for (std::size_t i = 3; i < normalized.size() && normalized[i] != '-'; ++i)
                    normalized[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(normalized[i])));
            }
            return normalized;
        }
    }  // namespace

    std::optional<LocaleTag> LocaleTag::Parse(const std::string_view tag) {
        std::string normalized = NormalizeTag(tag);
        if (normalized.empty())
            return std::nullopt;
        return LocaleTag{std::move(normalized)};
    }

    std::string MessageKey::Canonical() const {
        return namespaceId + ':' + localKey;
    }

    /** @copydoc MessageKeyHash::operator()(const MessageKey &) */
    std::size_t MessageKeyHash::operator()(const MessageKey &key) const noexcept {
        return (*this)(MessageKeyView{key.namespaceId, {}, key.localKey});
    }

    /** @copydoc MessageKeyHash::operator()(MessageKeyView) */
    std::size_t MessageKeyHash::operator()(const MessageKeyView key) const noexcept {
        std::size_t hash = 2166136261U;
        const auto append = [&hash](const std::string_view part) {
            for (const unsigned char value : part)
                hash = (hash ^ value) * 16777619U;
        };
        append(key.namespaceId);
        append(":");
        append(key.scope);
        if (!key.scope.empty())
            append(".");
        append(key.key);
        return hash;
    }

    /** @copydoc MessageKeyEqual::operator()(const MessageKey &, const MessageKey &) */
    bool MessageKeyEqual::operator()(const MessageKey &left, const MessageKey &right) const noexcept {
        return left == right;
    }

    /** @copydoc MessageKeyEqual::operator()(const MessageKey &, MessageKeyView) */
    bool MessageKeyEqual::operator()(const MessageKey &left, const MessageKeyView right) const noexcept {
        if (left.namespaceId != right.namespaceId)
            return false;
        std::string_view local = left.localKey;
        if (!right.scope.empty()) {
            if (!local.starts_with(right.scope))
                return false;
            local.remove_prefix(right.scope.size());
            if (local.empty() || local.front() != '.')
                return false;
            local.remove_prefix(1);
        }
        return local == right.key;
    }

    /** @copydoc MessageKeyEqual::operator()(MessageKeyView, const MessageKey &) */
    bool MessageKeyEqual::operator()(const MessageKeyView left, const MessageKey &right) const noexcept {
        return (*this)(right, left);
    }

    LocalizationService::LocalizationService(LocaleTag sourceFallback) : m_sourceFallback(std::move(sourceFallback)) {
        m_active = std::make_shared<LocalizationCatalog>(LocalizationCatalog{m_sourceFallback, {}});
    }

    bool LocalizationService::RegisterCatalog(LocalizationCatalog catalog, LocalizationError *error) {
        if (catalog.locale.value.empty()) {
            SetError(error, "editor.localization.invalid_locale", "Catalog locale is empty.");
            return false;
        }
        if (const auto parsed = LocaleTag::Parse(catalog.locale.value); !parsed.has_value() || parsed->value != catalog.locale.value) {
            SetError(error, "editor.localization.invalid_locale", "Catalog locale is not normalized BCP 47.");
            return false;
        }
        auto immutable = std::make_shared<const LocalizationCatalog>(std::move(catalog));
        m_catalogs[immutable->locale.value] = std::move(immutable);
        return true;
    }

    bool LocalizationService::LoadCatalogFile(const std::filesystem::path &path, LocalizationError *error) {
        std::ifstream input(path);
        if (!input) {
            SetError(error, "editor.localization.file_missing", "Localization catalog could not be opened.");
            return false;
        }
        const std::string json{std::istreambuf_iterator{input}, std::istreambuf_iterator<char>{}};
        std::smatch localeMatch;
        std::smatch namespaceMatch;
        const std::regex localePattern{R"re("locale"\s*:\s*"([^"]+)")re"};
        if (const std::regex namespacePatternRe{R"re("namespace"\s*:\s*"([^"]+)")re"};
            !std::regex_search(json, localeMatch, localePattern) || !std::regex_search(json, namespaceMatch, namespacePatternRe)) {
            SetError(error, "editor.localization.invalid_catalog", "Catalog must define locale and namespace.");
            return false;
        }

        LocalizationCatalog catalog{LocaleTag{localeMatch[1].str()}, {}};
        const std::string namespaceId = namespaceMatch[1].str();
        const std::regex messagePattern{R"re("([^"]+)"\s*:\s*\{\s*"text"\s*:\s*"((?:\\.|[^"\\])*)"\s*\})re"};
        const std::sregex_iterator matchEnd;
        for (std::sregex_iterator it(json.begin(), json.end(), messagePattern); it != matchEnd; ++it) {
            std::string decoded = DecodeEscapedJsonText((*it)[2].str());
            catalog.messages.try_emplace(MessageKey{namespaceId, (*it)[1].str()}, std::move(decoded));
        }
        if (catalog.messages.empty()) {
            SetError(error, "editor.localization.empty_catalog", "Catalog does not contain any messages.");
            return false;
        }
        return RegisterCatalog(std::move(catalog), error);
    }

    bool LocalizationService::Prepare(const LocaleTag &locale, LocalizationError *error) {
        if (const auto parsed = LocaleTag::Parse(locale.value); !parsed.has_value() || parsed->value != locale.value) {
            SetError(error, "editor.localization.invalid_locale", "Requested locale is not normalized BCP 47.");
            return false;
        }
        m_prepared = FindCatalog(locale);
        if (!m_prepared) {
            SetError(error, "editor.localization.catalog_missing", "No catalog is registered for the requested locale.");
            return false;
        }
        return true;
    }

    bool LocalizationService::ActivatePrepared(LocalizationError *error) {
        if (!m_prepared) {
            SetError(error, "editor.localization.no_prepared_snapshot", "No localization snapshot is prepared.");
            return false;
        }
        m_active = std::move(m_prepared);
        ++m_revision;
        return true;
    }

    LocalizationSnapshot LocalizationService::Snapshot() const noexcept {
        return LocalizationSnapshot{m_active, nullptr, m_revision};
    }

    /** @copydoc LocalizationService::Get */
    const std::string &LocalizationService::Get(std::string_view namespaceId, std::string_view localKey) const {
        return GetScoped(namespaceId, {}, localKey);
    }

    /** @copydoc LocalizationService::GetScoped */
    const std::string &LocalizationService::GetScoped(const std::string_view namespaceId, const std::string_view scope,
                                                      const std::string_view localKey) const {
        if (m_active) {
            const MessageKeyView key{namespaceId, scope, localKey};
            if (const auto it = m_active->messages.find(key); it != m_active->messages.end())
                return it->second;
        }

        std::string missingText = "[missing:" + std::string(namespaceId) + ":";
        if (!scope.empty()) {
            missingText += scope;
            missingText += '.';
        }
        missingText += localKey;
        missingText += ']';
        const auto [insertedIt, inserted] = m_missingCache.insert(std::move(missingText));
        return *insertedIt;
    }

    LocaleTag LocalizationService::ActiveLocale() const {
        return m_active->locale;
    }

    std::shared_ptr<const LocalizationCatalog> LocalizationService::FindCatalog(const LocaleTag &locale) const {
        const auto it = m_catalogs.find(locale.value);
        return it == m_catalogs.end() ? nullptr : it->second;
    }

    void LocalizationService::SetError(LocalizationError *error, std::string code, std::string message) {
        if (error)
            *error = LocalizationError{std::move(code), std::move(message)};
    }
}  // namespace Horo::Editor
