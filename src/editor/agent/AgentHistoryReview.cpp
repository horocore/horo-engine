#include "AgentHistoryReview.h"

#include "Horo/Foundation/Utf8.h"

#include <algorithm>
#include <array>

namespace Horo::Agent {
    namespace {
        constexpr auto Redaction = "[redacted]";
        constexpr auto MaximumDisplayBytes = AgentHistoryLimits{}.textBytesPerMessage;
        using namespace std::string_view_literals;

        /** @brief Folds protocol and field spellings independently of the host locale. */
        char LowerAscii(const char value) noexcept {
            return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
        }

        /** @brief Identifies ASCII token constituents without treating UTF-8 bytes as locale-dependent letters. */
        bool TokenCharacter(const char value) noexcept {
            constexpr auto punctuation = "_-./+="sv;
            return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || punctuation.find(value) != std::string_view::npos;
        }

        /** @brief Recognizes explicit credential fields across shell, header, and JSON display forms. */
        bool CredentialField(std::string_view line) {
            constexpr std::array fields{"apikey"sv, "authorization"sv, "cookie"sv,     "credential"sv, "password"sv,
                                        "passwd"sv, "passphrase"sv,    "privatekey"sv, "secret"sv,     "token"sv};
            std::string key;
            for (;;) {
                const auto separator = line.find_first_of("=:");
                if (separator == std::string_view::npos)
                    return false;
                key.assign(line.substr(0, separator));
                std::erase_if(key, [](const char character) {
                    return character < 'a' || character > 'z';
                });
                if (std::ranges::any_of(fields, [&key](const std::string_view field) {
                    return key.find(field) != std::string::npos;
                }))
                    return true;
                line.remove_prefix(separator + 1);
            }
        }

        /** @brief Recognizes common standalone credential shapes and long opaque values. */
        bool OpaqueCredential(const std::string_view token) {
            constexpr std::array prefixes{"sk-"sv,         "ghp_"sv,  "gho_"sv,  "ghu_"sv, "ghs_"sv, "ghr_"sv,
                                          "github_pat_"sv, "xoxb-"sv, "xoxp-"sv, "akia"sv, "eyj"sv};
            if (token.size() >= 12 && std::ranges::any_of(prefixes, [token](const std::string_view prefix) {
                return token.starts_with(prefix);
            }))
                return true;
            if (token.size() < 24)
                return false;
            const bool letter = std::ranges::any_of(token, [](const char character) {
                return character >= 'a' && character <= 'z';
            });
            const bool digit = std::ranges::any_of(token, [](const char character) {
                return character >= '0' && character <= '9';
            });
            return letter && digit;
        }

        /** @brief Treats a credential-bearing line as a unit so nearby private values cannot leak. */
        bool SensitiveLine(const std::string_view line) {
            constexpr std::array phrases{"bearer "sv, "password is "sv, "passphrase is "sv, "api key is "sv, "secret is "sv, "token is "sv};
            if (CredentialField(line) || std::ranges::any_of(phrases, [line](const std::string_view phrase) {
                return line.find(phrase) != std::string_view::npos;
            }))
                return true;
            const auto scheme = line.find("://");
            if (scheme != std::string_view::npos && line.find('@', scheme + 3) != std::string_view::npos)
                return true;
            for (std::size_t first = 0; first < line.size();) {
                if (!TokenCharacter(line[first])) {
                    ++first;
                    continue;
                }
                auto last = first + 1;
                while (last < line.size() && TokenCharacter(line[last]))
                    ++last;
                if (OpaqueCredential(line.substr(first, last - first)))
                    return true;
                first = last;
            }
            return false;
        }

        /** @brief Suppresses complete or truncated PEM private key blocks, including their delimiters. */
        bool PrivateKeyMarker(const std::string_view line, const std::string_view marker) {
            return line.find(marker) != std::string_view::npos && line.find("private key") != std::string_view::npos;
        }

        /** @brief Replaces sensitive lines with a stable literal, preserving ordinary visible dialogue. */
        std::string RedactLines(std::string_view text) {
            std::string result;
            result.reserve(text.size());
            bool privateKey{};
            while (!text.empty()) {
                const auto end = text.find('\n');
                const auto line = text.substr(0, end);
                auto folded = std::string{line};
                std::ranges::transform(folded, folded.begin(), LowerAscii);
                if (privateKey) {
                    privateKey = !PrivateKeyMarker(folded, "-----end ");
                } else {
                    privateKey = PrivateKeyMarker(folded, "-----begin ");
                    result.append(privateKey || SensitiveLine(folded) ? Redaction : line);
                    if (end != std::string_view::npos)
                        result.push_back('\n');
                }
                if (end == std::string_view::npos)
                    break;
                text.remove_prefix(end + 1);
            }
            return result;
        }

        /** @brief Refuses text that cannot be represented safely by the ordinary editor text widgets. */
        bool ValidDisplayText(const std::string_view text) {
            return IsValidUtf8ScalarSequence(text) && std::ranges::none_of(text, [](const unsigned char character) {
                return character == 127 || (character < 32 && character != '\n' && character != '\t' && character != '\r');
            });
        }
    }  // namespace

    /** @copydoc RedactHistoryDisplayText */
    AgentHistoryTextDecision RedactHistoryDisplayText(const AgentHistoryTextKind kind, const std::string_view visible) {
        const auto limit = kind == AgentHistoryTextKind::Title ? AgentHistoryLimits::MaximumTitleBytes : MaximumDisplayBytes;
        if (visible.size() > limit || !ValidDisplayText(visible))
            return {};
        auto redacted = RedactLines(visible);
        if (redacted.size() > limit)
            return {};
        return {Security::AgentDataClass::EditorContext, std::move(redacted)};
    }

    AgentHistoryExchangeReview::AgentHistoryExchangeReview(AgentHistoryProjectRevision project, std::string prompt, std::string response)
        : project_(project), prompt_(std::move(prompt)), response_(std::move(response)) {}

    /** @copydoc AgentHistoryExchangeReview::Prepare */
    Result<AgentHistoryExchangeReview> AgentHistoryExchangeReview::Prepare(const AgentHistoryProjectRevision project,
                                                                           const std::string_view prompt, const std::string_view response) {
        using Output = Result<AgentHistoryExchangeReview>;
        if (project.value == 0 || prompt.empty() || response.empty())
            return Output::Failure(MakeError(AgentHistoryErrors::Invalid));
        if (prompt.size() > MaximumDisplayBytes || response.size() > MaximumDisplayBytes)
            return Output::Failure(MakeError(AgentHistoryErrors::Capacity));
        auto user = RedactHistoryDisplayText(AgentHistoryTextKind::UserMessage, prompt);
        auto assistant = RedactHistoryDisplayText(AgentHistoryTextKind::AssistantMessage, response);
        if (!user.admittedText || !assistant.admittedText)
            return Output::Failure(MakeError(AgentHistoryErrors::RejectedText));
        return Output::Success(AgentHistoryExchangeReview{project, std::move(*user.admittedText), std::move(*assistant.admittedText)});
    }

    /** @copydoc AgentHistoryExchangeReview::SaveApproved */
    Result<AgentHistoryOperationId> AgentHistoryExchangeReview::SaveApproved(AgentHistorySession &history, const std::string_view title,
                                                                             const std::uint64_t now) const {
        return history.RecordCompleted(project_, {.title = title, .userText = prompt_, .now = now}, {response_, {}});
    }
}  // namespace Horo::Agent
