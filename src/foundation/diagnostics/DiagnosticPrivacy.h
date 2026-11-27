#pragma once

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace Horo::Diagnostics::Detail {
    /** @brief Classifies credentials, Save data and private capture fields at log admission and bundle export. */
    [[nodiscard]] inline bool IsSensitiveKey(const std::string_view key) {
        if (key.empty())
            return false;
        std::string normalized{key};
        std::erase_if(normalized, [](const char character) {
            return character == '_' || character == '-' || character == '.';
        });
        std::ranges::transform(normalized, normalized.begin(), [](const char character) {
            return character >= 'A' && character <= 'Z' ? static_cast<char>(character + ('a' - 'A')) : character;
        });
        static constexpr std::array<std::string_view, 8> fragments{"apikey",   "authorization", "cookie", "credential",
                                                                   "password", "privatekey",    "secret", "token"};
        if (std::ranges::any_of(fragments, [&normalized](const std::string_view fragment) {
            return normalized.find(fragment) != std::string::npos;
        }))
            return true;
        std::string_view saveKey{normalized};
        if (saveKey.starts_with("runtimesave"))
            saveKey.remove_prefix(std::string_view{"runtime"}.size());
        static constexpr std::array<std::string_view, 10> privateSaveFields{"saveaccountid", "savedisplay",   "savedisplayname",
                                                                            "savepayload",   "saveproductid", "saveprofileid",
                                                                            "saveslot",      "saveslotid",    "saveslotlabel",
                                                                            "saveuserid"};
        if (std::ranges::find(privateSaveFields, saveKey) != privateSaveFields.end())
            return true;

        std::string_view captureKey{normalized};
        if (captureKey.starts_with("xr"))
            captureKey.remove_prefix(std::string_view{"xr"}.size());
        static constexpr std::array<std::string_view, 21> privateCaptures{"cameraframe",         "cameraframes",  "deviceserial",
                                                                          "environmentgeometry", "gazedirection", "gazehistory",
                                                                          "gazeorigin",          "gazesamples",   "handjoints",
                                                                          "headsetserial",       "microphonepcm", "planeboundary",
                                                                          "posehistory",         "rawaudio",      "rawcameraframe",
                                                                          "rawcameraframes",     "rawpose",       "rawposes",
                                                                          "rawposesamples",      "roomgeometry",  "speechtranscript"};
        return std::ranges::find(privateCaptures, captureKey) != privateCaptures.end();
    }
}  // namespace Horo::Diagnostics::Detail
