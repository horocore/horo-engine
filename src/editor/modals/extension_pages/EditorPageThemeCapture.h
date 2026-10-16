#pragma once
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Extensions/EditorThemeTokens.h"
#include "Horo/Foundation/Sha256.h"

#include <array>
#include <memory>
#include <optional>
struct ImFont;

namespace Horo::Editor {
    /** @brief Upper editor bridge over actual current shared design tokens and font roles; owns no theme selection or renderer resource. */
    class EditorPageThemeCapture final {
    public:
        EditorPageThemeCapture();
        /** @brief Captures at the actual owner frame boundary; structural token/font changes advance one monotonic local revision. */
        Result<void> Update(const EditorGuiContext &context);
        /** @brief Borrows immutable presentation evidence until the next serialized owner update; no native font handle is exposed. */
        std::shared_ptr<const Extensions::EditorThemeFrame> Frame() const noexcept;

    private:
        std::shared_ptr<Extensions::EditorThemeFrame> frame_;
        std::optional<Sha256Digest> signature_;
        std::array<const ImFont *, 4> fonts_{};
        std::uint64_t revision_{};
        bool exhausted_{};
    };
}  // namespace Horo::Editor
