#include "Horo/UiTemplates/UiTemplateModel.h"

#include "Horo/UiTemplates/UiTemplateErrors.h"
#include "UiTemplateIdentityEncoder.h"

#include <algorithm>
#include <array>
#include <string_view>

namespace Horo::UiTemplates {
    /** @copydoc TemplateValueKind */
    UiTemplateValueKind TemplateValueKind(const UiTemplateValue &value) noexcept {
        return static_cast<UiTemplateValueKind>(value.index());
    }

    /** @copydoc Detail::EncodeTemplateIdentity */
    Result<Runtime::Ui::UiElementId> Detail::EncodeTemplateIdentity(const TemplateIdentityDomain domain,
                                                                    const Runtime::Ui::UiDocumentId document,
                                                                    const std::span<const UiTemplateInstanceId> lineage,
                                                                    const std::span<const Runtime::Ui::SerializedUiId> suffix) {
        if (!document.IsValid() || lineage.empty() || lineage.size() > 32 ||
            std::ranges::any_of(lineage, [](const UiTemplateInstanceId id) {
            return !id.IsValid();
        }))
            return Result<Runtime::Ui::UiElementId>::Failure(MakeError(UiErrors::TemplateGraphInvalid));

        Sha256Builder hash;
        const std::string_view wireDomain =
            domain == TemplateIdentityDomain::Element ? "horo.ui-template.element.v1" : "horo.ui-template.slot-default.v1";
        const std::size_t fieldCount = domain == TemplateIdentityDomain::Element ? 1 : 2;
        if (suffix.size() != fieldCount)
            return Result<Runtime::Ui::UiElementId>::Failure(MakeError(UiErrors::TemplateGraphInvalid));
        const std::array count{static_cast<std::uint8_t>(lineage.size())};
        // Fixed-size identity fields and the explicit lineage count make this encoding unambiguous.
        if (!hash.Update(std::as_bytes(std::span{wireDomain.data(), wireDomain.size()})) ||
            !hash.Update(std::as_bytes(std::span{document.Bytes()})) || !hash.Update(std::as_bytes(std::span{count})))
            return Result<Runtime::Ui::UiElementId>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
        for (const UiTemplateInstanceId instance : lineage)
            if (!hash.Update(std::as_bytes(std::span{instance.Bytes()})))
                return Result<Runtime::Ui::UiElementId>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
        for (const auto &field : suffix)
            if (!hash.Update(std::as_bytes(std::span{field})))
                return Result<Runtime::Ui::UiElementId>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
        Runtime::Ui::SerializedUiId bytes{};
        const auto digest = hash.Finalize();
        std::copy_n(digest.bytes.begin(), bytes.size(), bytes.begin());
        return Runtime::Ui::UiElementId::Create(bytes);
    }

    /** @copydoc DeriveTemplateElementId */
    Result<Runtime::Ui::UiElementId> DeriveTemplateElementId(const Runtime::Ui::UiDocumentId document,
                                                             const std::span<const UiTemplateInstanceId> lineage,
                                                             const UiTemplateLocalElementId local) {
        if (!local.IsValid())
            return Result<Runtime::Ui::UiElementId>::Failure(MakeError(UiErrors::TemplateGraphInvalid));
        const std::array<Runtime::Ui::SerializedUiId, 1> suffix{local.Bytes()};
        return Detail::EncodeTemplateIdentity(Detail::TemplateIdentityDomain::Element, document, lineage, suffix);
    }

    /** @copydoc UiTemplateLimits::IsValid */
    bool UiTemplateLimits::IsValid() const noexcept {
        return maximumElements > 0 && maximumElements <= 4096 && maximumDepth > 0 && maximumDepth <= 32 && maximumInstances > 0 &&
               maximumInstances <= 256 && maximumParameters > 0 && maximumParameters <= 256 && maximumSlots > 0 && maximumSlots <= 256 &&
               maximumProperties > 0 && maximumProperties <= 16384 && maximumDependencies > 0 && maximumDependencies <= 1024 &&
               maximumPayloadBytes > 0 && maximumPayloadBytes <= 4 * 1024 * 1024 && maximumUndoEntries > 0 && maximumUndoEntries <= 16;
    }
}  // namespace Horo::UiTemplates
