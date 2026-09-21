#pragma once

#include "Horo/Extensions/ExtensionErrors.h"
#include "Horo/Extensions/ScriptExportDescriptor.h"

#include <set>
#include <string>
#include <string_view>

namespace Horo::Extensions::Detail {
    template <typename T> [[nodiscard]] Result<T> ScriptExportDescriptorInvalid(const std::string_view reason) {
        return Result<T>::Failure(MakeError(ExtensionErrors::ScriptExportDescriptorInvalid, std::string{reason}));
    }

    template <typename T> [[nodiscard]] Result<T> ScriptExportDescriptorConflict(const std::string_view reason) {
        return Result<T>::Failure(MakeError(ExtensionErrors::ScriptExportDescriptorConflict, std::string{reason}));
    }

    template <typename T> [[nodiscard]] Result<T> ScriptExportDescriptorCapacity(const std::string_view reason) {
        return Result<T>::Failure(MakeError(ExtensionErrors::ScriptExportDescriptorCapacityExceeded, std::string{reason}));
    }

    template <typename T> [[nodiscard]] Result<T> ScriptExportDescriptorIncompatible(const std::string_view reason) {
        return Result<T>::Failure(MakeError(ExtensionErrors::ScriptExportDescriptorIncompatible, std::string{reason}));
    }

    [[nodiscard]] bool IsCanonicalScriptExportIdentity(std::string_view value, std::size_t maximumBytes) noexcept;
    [[nodiscard]] bool IsKnownPrimitive(ScriptExportPrimitiveKind primitive) noexcept;
    [[nodiscard]] bool IsKnownNullability(ScriptExportNullability nullability) noexcept;
    [[nodiscard]] bool IsKnownNamedTypeKind(ScriptExportNamedTypeKind kind) noexcept;
    [[nodiscard]] bool IsKnownRequirement(ScriptExportParameterRequirement requirement) noexcept;
    [[nodiscard]] bool IsKnownInvocationMode(ScriptExportInvocationMode mode) noexcept;
    [[nodiscard]] bool IsValidScriptExportVersion(const ScriptExportVersion &version) noexcept;
    [[nodiscard]] bool IsValidScriptExportLimits(const ScriptExportDescriptorLimits &limits) noexcept;
    [[nodiscard]] bool AddScriptExportBytes(std::size_t &total, std::size_t bytes, std::size_t maximum) noexcept;
    [[nodiscard]] std::string MakeScopedScriptExportIdentity(std::string_view parent, std::string_view child);
    [[nodiscard]] bool IsBoundedScriptExportScopedIdentity(std::string_view parent, std::string_view child,
                                                           std::size_t maximumBytes) noexcept;

    [[nodiscard]] Result<void> ValidateScriptExportText(std::string_view value, std::size_t maximumBytes, std::size_t &totalBytes,
                                                        const ScriptExportDescriptorLimits &limits, bool required, std::string_view field);
    [[nodiscard]] Result<void> ValidateScriptExportDocumentation(const ScriptExportDocumentation &documentation, std::size_t &totalBytes,
                                                                 const ScriptExportDescriptorLimits &limits);
    [[nodiscard]] Result<void> ValidateScriptExportVersionIntroduction(const ScriptExportVersion &introduced,
                                                                       const ScriptExportVersion &current);
    [[nodiscard]] Result<void> ValidateScriptExportTypeReference(const ScriptExportTypeReference &reference,
                                                                 const std::set<std::string, std::less<>> &namedTypes,
                                                                 const ScriptExportDescriptorLimits &limits, std::size_t &totalBytes);
    [[nodiscard]] Result<void> ValidateScriptExportDefault(ScriptExportParameterRequirement requirement,
                                                           const std::optional<std::vector<std::byte>> &canonicalDefault,
                                                           const ScriptExportDescriptorLimits &limits, std::size_t &totalBytes);
    [[nodiscard]] Result<void> ValidateScriptExportField(const ScriptExportFieldDescriptor &field, const ScriptExportVersion &current,
                                                         const std::set<std::string, std::less<>> &namedTypes,
                                                         const ScriptExportDescriptorLimits &limits, std::size_t &totalBytes);
    [[nodiscard]] Result<void> ValidateScriptExportParameter(const ScriptExportParameterDescriptor &parameter,
                                                             const ScriptExportVersion &current,
                                                             const std::set<std::string, std::less<>> &namedTypes,
                                                             const ScriptExportDescriptorLimits &limits, std::size_t &totalBytes);
    [[nodiscard]] Result<void> ValidateScriptExportResult(const ScriptExportResultDescriptor &result, const ScriptExportVersion &current,
                                                          const std::set<std::string, std::less<>> &namedTypes,
                                                          const ScriptExportDescriptorLimits &limits, std::size_t &totalBytes);

    [[nodiscard]] const ScriptExportTypeDescriptor *FindScriptExportType(const ScriptExportDescriptor &descriptor,
                                                                         std::string_view id) noexcept;
    [[nodiscard]] const ScriptExportFunctionDescriptor *FindScriptExportFunction(const ScriptExportDescriptor &descriptor,
                                                                                 std::string_view id) noexcept;
    [[nodiscard]] const ScriptExportConstantDescriptor *FindScriptExportConstant(const ScriptExportDescriptor &descriptor,
                                                                                 std::string_view id) noexcept;
    [[nodiscard]] const ScriptExportErrorDescriptor *FindScriptExportError(const ScriptExportDescriptor &descriptor,
                                                                           std::string_view id) noexcept;
    [[nodiscard]] const ScriptExportParameterDescriptor *FindScriptExportParameter(const ScriptExportFunctionDescriptor &function,
                                                                                   std::string_view id) noexcept;
    [[nodiscard]] const ScriptExportResultDescriptor *FindScriptExportResult(const ScriptExportFunctionDescriptor &function,
                                                                             std::string_view id) noexcept;
    [[nodiscard]] const ScriptExportFieldDescriptor *FindScriptExportField(const ScriptExportTypeDescriptor &type,
                                                                           std::string_view id) noexcept;
    [[nodiscard]] const ScriptExportEnumValueDescriptor *FindScriptExportEnumValue(const ScriptExportTypeDescriptor &type,
                                                                                   std::string_view id) noexcept;
    [[nodiscard]] const ScriptExportDescriptor *FindScriptExportDescriptor(std::span<const ScriptExportDescriptor> descriptors,
                                                                           std::string_view id) noexcept;
    [[nodiscard]] bool ContainsString(const std::vector<std::string> &values, std::string_view value) noexcept;
    [[nodiscard]] bool HasTombstone(const ScriptExportDescriptor &descriptor, std::string_view id) noexcept;
    [[nodiscard]] bool SameTypeReference(const ScriptExportTypeReference &left, const ScriptExportTypeReference &right) noexcept;
    [[nodiscard]] bool SameFieldShape(const ScriptExportFieldDescriptor &left, const ScriptExportFieldDescriptor &right) noexcept;
    [[nodiscard]] bool SameParameterShape(const ScriptExportParameterDescriptor &left,
                                          const ScriptExportParameterDescriptor &right) noexcept;
    [[nodiscard]] bool SameResultShape(const ScriptExportResultDescriptor &left, const ScriptExportResultDescriptor &right) noexcept;
    [[nodiscard]] bool SameEnumValueShape(const ScriptExportEnumValueDescriptor &left,
                                          const ScriptExportEnumValueDescriptor &right) noexcept;
    [[nodiscard]] bool SameTypeBase(const ScriptExportTypeDescriptor &left, const ScriptExportTypeDescriptor &right) noexcept;
    [[nodiscard]] bool SameFunctionBase(const ScriptExportFunctionDescriptor &left, const ScriptExportFunctionDescriptor &right) noexcept;
    [[nodiscard]] bool SameConstantShape(const ScriptExportConstantDescriptor &left, const ScriptExportConstantDescriptor &right) noexcept;
    [[nodiscard]] bool SameErrorShape(const ScriptExportErrorDescriptor &left, const ScriptExportErrorDescriptor &right) noexcept;
    [[nodiscard]] bool ContainsAllStrings(const std::vector<std::string> &required, const std::vector<std::string> &available) noexcept;

    [[nodiscard]] Result<void> ValidateScriptExportType(const ScriptExportTypeDescriptor &type, const ScriptExportVersion &current,
                                                        const std::set<std::string, std::less<>> &namedTypes,
                                                        std::set<std::string, std::less<>> &activeSymbols,
                                                        const ScriptExportDescriptorLimits &limits, std::size_t &totalBytes);
    [[nodiscard]] Result<void> ValidateScriptExportTypeGraph(const ScriptExportDescriptor &descriptor,
                                                             const ScriptExportDescriptorLimits &limits);
    [[nodiscard]] Result<void> ValidateScriptExportError(const ScriptExportErrorDescriptor &error, const ScriptExportVersion &current,
                                                         const ScriptExportDescriptorLimits &limits, std::size_t &totalBytes);
    [[nodiscard]] Result<void> ValidateScriptExportConstant(const ScriptExportConstantDescriptor &constant,
                                                            const ScriptExportVersion &current,
                                                            const std::set<std::string, std::less<>> &namedTypes,
                                                            const ScriptExportDescriptorLimits &limits, std::size_t &totalBytes);
    [[nodiscard]] Result<void> ValidateScriptExportFunction(const ScriptExportFunctionDescriptor &function,
                                                            const ScriptExportDescriptor &descriptor,
                                                            const std::set<std::string, std::less<>> &namedTypes,
                                                            const std::set<std::string, std::less<>> &errorIds,
                                                            std::set<std::string, std::less<>> &activeSymbols,
                                                            const ScriptExportDescriptorLimits &limits, std::size_t &totalBytes);
    [[nodiscard]] Result<void> ValidateScriptExportSymbolCapacity(const ScriptExportDescriptor &descriptor,
                                                                  const ScriptExportDescriptorLimits &limits);
    [[nodiscard]] Result<void> ValidateScriptExportDescriptorData(const ScriptExportDescriptor &descriptor,
                                                                  const ScriptExportDescriptorLimits &limits);

    void SortScriptExportDescriptor(ScriptExportDescriptor &descriptor);
    [[nodiscard]] Sha256Digest ComputeScriptExportDescriptorFingerprint(std::span<const ScriptExportDescriptor> descriptors);

    [[nodiscard]] Result<void> ValidateScriptExportReplacement(const ScriptExportDescriptorSnapshotPtr &previous,
                                                               std::span<const ScriptExportDescriptor> candidate);
}  // namespace Horo::Extensions::Detail
