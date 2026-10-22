#pragma once
#include "Horo/Editor/Localization/ILocalizationService.h"

#include <string>
#include <string_view>
#include <unordered_map>

namespace Horo::Tests {
    class ViewportRenderLocalization final : public Horo::Editor::ILocalizationService {
    public:
        [[nodiscard]] const std::string &Get(const std::string_view, const std::string_view localKey) const override {
            std::string_view value = localKey;
            if (localKey == "workspace.viewport.object_count") {
                value = "{} objects";
            }
            const auto [entry, inserted] = values_.try_emplace(std::string(localKey), value);
            static_cast<void>(inserted);
            return entry->second;
        }

    private:
        mutable std::unordered_map<std::string, std::string> values_;
    };

}  // namespace Horo::Tests
