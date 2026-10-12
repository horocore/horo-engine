#pragma once

#include "Horo/Application/UiTemplateHost.h"
#include "Horo/Runtime/Ui/UiErrors.h"

#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <span>
#include <string>

namespace Horo::Application::TemplateHostTests {
    /** @brief Explicit test Localization snapshot; never reads source fallback text or Editor state. */
    class FixedMessages final : public UiTemplateLocalizationSnapshotQuery {
    public:
        Runtime::Ui::UiLocaleTag locale{Runtime::Ui::UiLocaleTag::Parse("tr-TR").Value()};
        mutable Sha256Digest identity{ComputeSha256(std::as_bytes(std::span{"validated game messages", 23}))};
        bool wrongIdentity{};
        bool wrongKey{};
        bool wrongLocale{};
        bool invalidUtf8{};
        bool invalidSpan{};
        bool fail{};
        bool mutateSnapshot{};
        CancellationSource *cancel{};

        Sha256Digest Identity() const noexcept override {
            return identity;
        }

        const Runtime::Ui::UiLocaleTag &Locale() const noexcept override {
            return locale;
        }

        Result<UiTemplateResolvedMessage> Resolve(const Runtime::Ui::LocalizedMessageRef &reference, std::size_t,
                                                  const CancellationToken &) const override {
            if (fail)
                return Result<UiTemplateResolvedMessage>::Failure(MakeError(Runtime::Ui::UiErrors::LocalizedArgumentInvalid));
            CHECK(reference.Arguments().size() == 2);
            if (cancel)
                cancel->RequestCancellation();
            if (mutateSnapshot)
                std::as_writable_bytes(std::span{identity.bytes}).front() ^= std::byte{1};
            UiTemplateResolvedMessage result{identity,
                                             reference.Key(),
                                             locale,
                                             Runtime::Ui::UiTextParagraphDirection::LeftToRight,
                                             "Merhaba Ada — 42",
                                             {{0, 7, UiTemplateMessageSpanKind::Strong}}};
            if (wrongIdentity)
                std::as_writable_bytes(std::span{result.snapshot.bytes}).front() ^= std::byte{1};
            if (wrongKey)
                result.key.localKey = "different";
            if (wrongLocale)
                result.locale = Runtime::Ui::UiLocaleTag::Parse("en-US").Value();
            if (invalidUtf8)
                result.text = std::string(1, std::bit_cast<char>(std::byte{0x80}));
            if (invalidSpan)
                result.spans = {{12, 1, UiTemplateMessageSpanKind::Strong}};
            return Result<UiTemplateResolvedMessage>::Success(std::move(result));
        }
    };
}  // namespace Horo::Application::TemplateHostTests
