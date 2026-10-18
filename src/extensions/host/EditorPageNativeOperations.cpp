#include "EditorActivityAbiConversion.h"
#include "EditorPageAbiConversion.h"
#include "Horo/Extensions/ExtensionErrors.h"

#include <algorithm>
#include <array>
#include <format>
#include <type_traits>

namespace Horo::Extensions::Detail {
    namespace {
        /** @brief Borrows one owned value without a double intermediary; all text borrows end with the native callback. */
        HoroExtensionStringView Text(std::string_view text) noexcept {
            return {text.data(), static_cast<uint32_t>(text.size())};
        }

        /** @brief Collects bounded copied findings for exactly one synchronous provider operation. */
        struct Findings final {
            std::vector<EditorPageFinding> values;
            bool rejected{};
        };

        /** @brief Contains malformed provider output and allocation failures at the sink boundary. */
        // The finding sink is a literal C11 callback; no C++ type may replace its opaque ABI context.
        HoroExtensionStatus Append(void *context,  // NOSONAR(cpp:S5008): C ABI context.
                                   const HoroEditorPageFinding *input) noexcept {
            if (!context)
                return HORO_EXTENSION_ERROR_INVALID_ARGS;
            auto &result = *static_cast<Findings *>(context);
            try {
                EditorPageFinding finding;
                if (!HoroEditorPageFindingShape(input) || result.values.size() >= 32 ||
                    !CopyActivityText(input->binding, finding.binding.value) ||
                    !CopyActivityText(input->diagnosticCode, finding.code.value, 128) ||
                    !CopyActivityText(input->messageKey, finding.message.value, 128)) {
                    result.rejected = true;
                    return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
                }
                finding.severity = static_cast<EditorUiValidationSeverity>(input->severity);
                finding.message.kind = EditorUiTextKind::LocalizationKey;
                result.values.push_back(std::move(finding));
                return HORO_EXTENSION_SUCCESS;
            } catch (...) {
                result.rejected = true;
                return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
            }
        }

        /** @brief Reads only the callback-lifetime cancellation token; never retains a host context. */
        // The cancellation callback borrows the non-null request token for this synchronous C11 invocation.
        uint8_t Cancelled(const void *context) noexcept {  // NOSONAR(cpp:S5008): C ABI context.
            return static_cast<uint8_t>(static_cast<const CancellationToken *>(context)->IsCancellationRequested());
        }

        /** @brief Converts a bounded draft using only active exact typed members; pointers borrow the immutable draft. */
        std::vector<HoroEditorPageValue> Values(const EditorPageDraft &draft) {
            std::vector<HoroEditorPageValue> values;
            values.reserve(draft.values.size());
            for (const auto &field : draft.values) {
                HoroEditorPageValue wire{.structSize = sizeof(wire), .binding = Text(field.binding.value)};
                std::visit([&]<typename T>(const T &value) {
                    if constexpr (std::is_same_v<T, bool>) {
                        wire.kind = HORO_EDITOR_PAGE_BOOLEAN;
                        wire.booleanValue = value;
                    } else if constexpr (std::is_same_v<T, std::int64_t>) {
                        wire.kind = HORO_EDITOR_PAGE_INTEGER;
                        wire.integerValue = value;
                    } else if constexpr (std::is_same_v<T, std::string>) {
                        wire.kind = HORO_EDITOR_PAGE_STRING;
                        wire.stringValue = Text(value);
                    }
                }, field.value);
                values.push_back(wire);
            }
            return values;
        }

        /** @brief Native endpoint owns no library or host; caller-held attributed executable lease covers this whole virtual invocation. */
        class NativePageOperations final : public IEditorPageOperations {
        public:
            explicit NativePageOperations(const HoroEditorPageDescriptor &descriptor) noexcept
                : endpoint_{.structSize = sizeof(endpoint_), .moduleContext = descriptor.moduleContext, .invoke = descriptor.invoke} {}

            Result<std::vector<EditorPageFinding>> Invoke(EditorPageOperation operation, const EditorPageDraft &draft,
                                                          const EditorUiActionId &action, const CancellationToken &cancellation) override {
                if (!endpoint_.invoke || draft.values.size() > MaximumEditorPageFields)
                    return Result<std::vector<EditorPageFinding>>::Failure(MakeError(ExtensionErrors::ContributionRejected));
                if (std::ranges::any_of(draft.values, [](const auto &field) {
                    return !std::holds_alternative<bool>(field.value) && !std::holds_alternative<std::int64_t>(field.value) &&
                           !std::holds_alternative<std::string>(field.value);
                }))
                    return Result<std::vector<EditorPageFinding>>::Failure(MakeError(ExtensionErrors::EditorUiFormInvalid));
                auto values = Values(draft);
                Findings findings;
                const HoroEditorPageOperation request{sizeof(request),
                                                      static_cast<uint32_t>(operation),
                                                      draft.revision,
                                                      Text(draft.surfaceId),
                                                      Text(action.value),
                                                      values.data(),
                                                      static_cast<uint32_t>(values.size()),
                                                      {&cancellation, Cancelled}};
                const HoroEditorPageFindingSink sink{&findings, Append};
                HoroExtensionStatus status = HORO_EXTENSION_ERROR_INIT_FAILED;
                try {
                    status = HoroEditorPageInvoke(&endpoint_, &request, &sink);
                    // Arbitrary plugin exceptions must not escape native invocation or bypass typed rejection.
                } catch (...) {  // NOSONAR(cpp:S2738): Foreign ABI boundary.
                    status = HORO_EXTENSION_ERROR_INIT_FAILED;
                }
                if (status != HORO_EXTENSION_SUCCESS || findings.rejected)
                    return Result<std::vector<EditorPageFinding>>::Failure(
                        MakeError(ExtensionErrors::ContributionRejected,
                                  std::format("Native page operation rejected (status={}).", status)));
                return Result<std::vector<EditorPageFinding>>::Success(std::move(findings.values));
            }

        private:
            HoroEditorPageDescriptor endpoint_{};
        };
    }  // namespace

    /** @copydoc MakeNativePageOperations */
    std::shared_ptr<IEditorPageOperations> MakeNativePageOperations(const HoroEditorPageDescriptor &descriptor) {
        return std::make_shared<NativePageOperations>(descriptor);
    }
}  // namespace Horo::Extensions::Detail
