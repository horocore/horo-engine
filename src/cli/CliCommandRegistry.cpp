#include "Horo/Cli/CliCommandRegistry.h"

#include "Horo/Cli/CliErrors.h"
#include "Horo/Cli/CliOptionParser.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <locale>
#include <memory>
#include <sstream>

namespace Horo::Cli {
    namespace {
        [[nodiscard]] bool IsConfigurationKeyCharacter(const char character) noexcept {
            return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '-' ||
                   character == '_' || character == '.';
        }

        [[nodiscard]] bool IsCanonicalToken(const std::string_view value, const std::size_t maximumBytes) noexcept {
            if (value.empty() || value.size() > maximumBytes || value.front() < 'a' || value.front() > 'z')
                return false;
            return std::ranges::all_of(value, [](const char character) {
                return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '-';
            });
        }

        [[nodiscard]] bool IsSafeSummary(const std::string_view value, const std::size_t maximumBytes) noexcept {
            return !value.empty() && value.size() <= maximumBytes && std::ranges::all_of(value, [](const char character) {
                const auto byte = static_cast<unsigned char>(character);
                return byte >= 0x20U && byte != 0x7FU;
            });
        }

        [[nodiscard]] bool IsCanonicalNamespacedId(const std::string_view value, const std::size_t maximumBytes) noexcept {
            if (value.empty() || value.size() > maximumBytes)
                return false;
            std::size_t begin = 0;
            bool foundSeparator = false;
            while (begin < value.size()) {
                const std::size_t end = value.find('.', begin);
                if (const std::string_view token = value.substr(begin, end == std::string_view::npos ? value.size() - begin : end - begin);
                    !IsCanonicalToken(token, maximumBytes))
                    return false;
                if (end == std::string_view::npos)
                    break;
                foundSeparator = true;
                begin = end + 1;
            }
            return foundSeparator;
        }

        [[nodiscard]] bool IsConfigurationKey(const std::string_view value, const std::size_t maximumBytes) noexcept {
            if (value.empty() || value.size() > maximumBytes)
                return false;
            if (value.front() == '.' || value.back() == '.')
                return false;
            return value.find('.') != std::string_view::npos && value.find("..") == std::string_view::npos &&
                   std::ranges::all_of(value, IsConfigurationKeyCharacter);
        }

        template <typename Enum> [[nodiscard]] bool IsKnown(Enum value) noexcept;

        template <> [[nodiscard]] bool IsKnown(const CliHostKind value) noexcept {
            return value == CliHostKind::HoroEngine || value == CliHostKind::HoroPak;
        }

        template <> [[nodiscard]] bool IsKnown(const CliOptionValueKind value) noexcept {
            using enum CliOptionValueKind;
            switch (value) {
                case Flag:
                case String:
                case SignedInteger:
                case FloatingPoint:
                case Enumeration:
                case Path:
                    return true;
            }
            return false;
        }

        template <> [[nodiscard]] bool IsKnown(const CliInteractivePolicy value) noexcept {
            using enum CliInteractivePolicy;
            return value == Forbidden || value == Optional || value == Required;
        }

        template <> [[nodiscard]] bool IsKnown(const CliSideEffectPolicy value) noexcept {
            using enum CliSideEffectPolicy;
            switch (value) {
                case None:
                case ReadsState:
                case MutatesState:
                case WritesFiles:
                case StartsExternalProcess:
                    return true;
            }
            return false;
        }

        template <> [[nodiscard]] bool IsKnown(const CliCancellationPolicy value) noexcept {
            return value == CliCancellationPolicy::Unsupported || value == CliCancellationPolicy::Cooperative;
        }

        template <> [[nodiscard]] bool IsKnown(const CliStdinPolicy value) noexcept {
            using enum CliStdinPolicy;
            switch (value) {
                case None:
                case JsonDocument:
                case JsonLines:
                case BinaryStream:
                    return true;
            }
            return false;
        }

        template <> [[nodiscard]] bool IsKnown(const CliCommandOrigin value) noexcept {
            return value == CliCommandOrigin::BuiltIn || value == CliCommandOrigin::Contribution;
        }

        [[nodiscard]] CliHostAvailability AvailabilityFor(const CliHostKind host) noexcept {
            return host == CliHostKind::HoroEngine ? CliHostAvailability::HoroEngine : CliHostAvailability::HoroPak;
        }

        [[nodiscard]] bool HasFormat(const CliOutputFormat formats, const CliOutputFormat requested) noexcept {
            return (formats & requested) == requested;
        }

        [[nodiscard]] bool ValidFormats(const CliOutputFormat formats) noexcept {
            using enum CliOutputFormat;
            constexpr std::byte KnownBits = std::byte{static_cast<std::uint8_t>(Human)} | std::byte{static_cast<std::uint8_t>(Json)} |
                                            std::byte{static_cast<std::uint8_t>(JsonLines)};
            const std::byte bits{static_cast<std::uint8_t>(formats)};
            const bool hasMachineFormat = HasFormat(formats, Json) || HasFormat(formats, JsonLines);
            return (bits & ~KnownBits) == std::byte{} && HasFormat(formats, Human) && hasMachineFormat;
        }

        [[nodiscard]] bool ValidAvailability(const CliHostAvailability availability) noexcept {
            constexpr std::byte KnownBits{static_cast<std::uint8_t>(CliHostAvailability::All)};
            const std::byte bits{static_cast<std::uint8_t>(availability)};
            return bits != std::byte{} && (bits & ~KnownBits) == std::byte{};
        }

        [[nodiscard]] bool ValidFloat(const std::string &value) {
            std::istringstream stream{value};
            stream.imbue(std::locale::classic());
            double parsed{};
            stream >> parsed;
            return stream && stream.peek() == std::char_traits<char>::eof() && std::isfinite(parsed);
        }

        [[nodiscard]] bool ContainsCapability(const std::span<const CliCapabilityId> capabilities,
                                              const CliCapabilityId &candidate) noexcept {
            return std::ranges::find(capabilities, candidate) != capabilities.end();
        }

        [[nodiscard]] bool ValidLimits(const CliCommandRegistryLimits &limits) noexcept {
            return limits.maximumCommands > 0 && limits.maximumPathSegments > 0 && limits.maximumOptionsPerCommand > 0 &&
                   limits.maximumPositionalsPerCommand > 0 && limits.maximumCapabilitiesPerCommand > 0 &&
                   limits.maximumEnumerationValues > 0 && limits.maximumIdentifierBytes > 0 && limits.maximumSummaryBytes > 0;
        }

        [[nodiscard]] Result<void> ValidatePolicy(const CliCommandRegistryPolicy &policy) {
            if (!IsKnown(policy.activeHost) || policy.supportedContractVersion.major == 0 || !ValidLimits(policy.limits))
                return Result<void>::Failure(MakeError(CliErrors::DescriptorInvalid, "CLI registry policy is invalid."));
            for (std::size_t index = 0; index < policy.grantedCapabilities.size(); ++index) {
                const CliCapabilityId &capability = policy.grantedCapabilities[index];
                if (!IsCanonicalNamespacedId(capability.value, policy.limits.maximumIdentifierBytes) ||
                    ContainsCapability(std::span{policy.grantedCapabilities}.first(index), capability))
                    return Result<void>::Failure(MakeError(CliErrors::DescriptorInvalid, "CLI registry capability grants are invalid."));
            }
            return Result<void>::Success();
        }

        [[nodiscard]] bool HasDuplicateOption(const std::span<const CliOptionDescriptor> options, const std::size_t candidate) noexcept {
            for (std::size_t prior = 0; prior < candidate; ++prior) {
                if (options[prior].name == options[candidate].name ||
                    (options[prior].shortName.has_value() && options[prior].shortName == options[candidate].shortName))
                    return true;
            }
            return false;
        }

        [[nodiscard]] bool HasDuplicateString(const std::span<const std::string> values, const std::size_t candidate) noexcept {
            return std::ranges::find(values.first(candidate), values[candidate]) != values.first(candidate).end();
        }

        [[nodiscard]] bool IsValidShortName(const char value) noexcept {
            return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9');
        }

        [[nodiscard]] Result<void> ValidateOptionMetadata(const CliOptionDescriptor &option, const CliCommandRegistryLimits &limits) {
            if (!IsCanonicalToken(option.name, limits.maximumIdentifierBytes) ||
                !IsSafeSummary(option.summary, limits.maximumSummaryBytes) || !IsKnown(option.valueKind))
                return Result<void>::Failure(MakeError(CliErrors::DescriptorInvalid, "CLI option metadata is invalid."));
            if (option.shortName.has_value() && !IsValidShortName(*option.shortName))
                return Result<void>::Failure(MakeError(CliErrors::DescriptorInvalid, "CLI option metadata is invalid."));
            return Result<void>::Success();
        }

        [[nodiscard]] bool HasInvalidFlagSchema(const CliOptionDescriptor &option) noexcept {
            if (option.valueKind != CliOptionValueKind::Flag)
                return false;
            return option.defaultValue.has_value() || !option.enumerationValues.empty() || option.sensitive ||
                   option.numericRange.minimumInteger.has_value() || option.numericRange.maximumInteger.has_value() ||
                   option.numericRange.minimumNumber.has_value() || option.numericRange.maximumNumber.has_value();
        }

        [[nodiscard]] bool HasIntegerRange(const CliNumericRange &range) noexcept {
            return range.minimumInteger.has_value() || range.maximumInteger.has_value();
        }

        [[nodiscard]] bool HasNumberRange(const CliNumericRange &range) noexcept {
            return range.minimumNumber.has_value() || range.maximumNumber.has_value();
        }

        [[nodiscard]] bool HasValidIntegerRange(const CliNumericRange &range) noexcept {
            const bool ordered =
                !range.minimumInteger.has_value() || !range.maximumInteger.has_value() || *range.minimumInteger <= *range.maximumInteger;
            return !HasNumberRange(range) && ordered;
        }

        [[nodiscard]] bool HasValidNumberRange(const CliNumericRange &range) noexcept {
            const auto finite = [](const std::optional<double> &value) noexcept {
                return !value.has_value() || std::isfinite(*value);
            };
            const bool ordered =
                !range.minimumNumber.has_value() || !range.maximumNumber.has_value() || *range.minimumNumber <= *range.maximumNumber;
            const bool minimumValid = finite(range.minimumNumber);
            const bool maximumValid = finite(range.maximumNumber);
            return !HasIntegerRange(range) && minimumValid && maximumValid && ordered;
        }

        [[nodiscard]] bool HasValidNumericRange(const CliOptionValueKind kind, const CliNumericRange &range) noexcept {
            if (kind == CliOptionValueKind::SignedInteger)
                return HasValidIntegerRange(range);
            if (kind == CliOptionValueKind::FloatingPoint)
                return HasValidNumberRange(range);
            return !HasIntegerRange(range) && !HasNumberRange(range);
        }

        [[nodiscard]] bool HasInvalidEnumerationSchema(const CliOptionDescriptor &option) noexcept {
            const bool isEnumeration = option.valueKind == CliOptionValueKind::Enumeration;
            return isEnumeration == option.enumerationValues.empty();
        }

        /** @brief Sensitive options cannot acquire retained defaults or configuration values. */
        [[nodiscard]] bool HasInvalidValueSource(const CliOptionDescriptor &option) noexcept {
            return (option.required && option.defaultValue.has_value()) ||
                   (option.sensitive && (option.defaultValue.has_value() || option.configurationKey.has_value()));
        }

        [[nodiscard]] Result<void> ValidateOptionShape(const CliOptionDescriptor &option, const CliCommandRegistryLimits &limits) {
            if (option.enumerationValues.size() > limits.maximumEnumerationValues)
                return Result<void>::Failure(MakeError(CliErrors::RegistryCapacityExceeded));
            if (HasInvalidValueSource(option) || HasInvalidFlagSchema(option) || HasInvalidEnumerationSchema(option) ||
                !HasValidNumericRange(option.valueKind, option.numericRange) ||
                (option.configurationKey.has_value() && !IsConfigurationKey(*option.configurationKey, limits.maximumIdentifierBytes)))
                return Result<void>::Failure(MakeError(CliErrors::OptionSchemaIncompatible));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateEnumerationValues(const CliOptionDescriptor &option, const CliCommandRegistryLimits &limits) {
            for (std::size_t index = 0; index < option.enumerationValues.size(); ++index) {
                if (!IsCanonicalToken(option.enumerationValues[index], limits.maximumIdentifierBytes) ||
                    HasDuplicateString(option.enumerationValues, index))
                    return Result<void>::Failure(MakeError(CliErrors::OptionSchemaIncompatible));
            }
            return Result<void>::Success();
        }

        /** @brief Parses the complete integer spelling before applying its declared range. */
        [[nodiscard]] bool HasValidIntegerDefault(const CliOptionDescriptor &option) {
            std::int64_t parsed{};
            const auto [end, error] =
                std::from_chars(option.defaultValue->data(), option.defaultValue->data() + option.defaultValue->size(), parsed);
            return error == std::errc{} && end == option.defaultValue->data() + option.defaultValue->size() &&
                   (!option.numericRange.minimumInteger.has_value() || parsed >= *option.numericRange.minimumInteger) &&
                   (!option.numericRange.maximumInteger.has_value() || parsed <= *option.numericRange.maximumInteger);
        }

        /** @brief Parses a finite locale-independent number before applying its declared range. */
        [[nodiscard]] bool HasValidNumberDefault(const CliOptionDescriptor &option) {
            if (!ValidFloat(*option.defaultValue))
                return false;
            std::istringstream stream{*option.defaultValue};
            stream.imbue(std::locale::classic());
            double parsed{};
            stream >> parsed;
            return (!option.numericRange.minimumNumber.has_value() || parsed >= *option.numericRange.minimumNumber) &&
                   (!option.numericRange.maximumNumber.has_value() || parsed <= *option.numericRange.maximumNumber);
        }

        [[nodiscard]] bool HasValidDefaultValue(const CliOptionDescriptor &option) {
            using enum CliOptionValueKind;
            switch (option.valueKind) {
                case Flag:
                    return false;
                case String:
                case Path:
                    return !option.defaultValue->empty();
                case SignedInteger:
                    return HasValidIntegerDefault(option);
                case FloatingPoint:
                    return HasValidNumberDefault(option);
                case Enumeration:
                    return std::ranges::find(option.enumerationValues, *option.defaultValue) != option.enumerationValues.end();
            }
            return false;
        }

        [[nodiscard]] Result<void> ValidateDefaultValue(const CliOptionDescriptor &option, const CliCommandRegistryLimits &limits) {
            if (!option.defaultValue.has_value())
                return Result<void>::Success();
            if (option.defaultValue->size() > limits.maximumIdentifierBytes)
                return Result<void>::Failure(MakeError(CliErrors::RegistryCapacityExceeded));
            if (!std::ranges::all_of(*option.defaultValue, [](const char character) {
                const auto byte = static_cast<unsigned char>(character);
                return byte >= 0x20U && byte != 0x7FU;
            }))
                return Result<void>::Failure(MakeError(CliErrors::OptionSchemaIncompatible));
            return HasValidDefaultValue(option) ? Result<void>::Success()
                                                : Result<void>::Failure(MakeError(CliErrors::OptionSchemaIncompatible));
        }

        [[nodiscard]] Result<void> ValidateOption(const CliOptionDescriptor &option, const CliCommandRegistryLimits &limits) {
            if (const Result<void> valid = ValidateOptionMetadata(option, limits); valid.HasError())
                return valid;
            if (const Result<void> valid = ValidateOptionShape(option, limits); valid.HasError())
                return valid;
            if (const Result<void> valid = ValidateEnumerationValues(option, limits); valid.HasError())
                return valid;
            return ValidateDefaultValue(option, limits);
        }

        [[nodiscard]] bool HasDuplicatePositionalName(const std::span<const CliPositionalDescriptor> positionals,
                                                      const std::size_t index) noexcept {
            return std::ranges::any_of(positionals.first(index), [&positionals, index](const CliPositionalDescriptor &prior) {
                return prior.name == positionals[index].name;
            });
        }

        [[nodiscard]] bool HasValidPositionalMetadata(const CliPositionalDescriptor &positional,
                                                      const CliCommandRegistryLimits &limits) noexcept {
            return IsCanonicalToken(positional.name, limits.maximumIdentifierBytes) &&
                   IsSafeSummary(positional.summary, limits.maximumSummaryBytes) && IsKnown(positional.valueKind);
        }

        [[nodiscard]] bool HasValidPositionalPolicy(const CliPositionalDescriptor &positional, const bool isLast,
                                                    const bool optionalSeen) noexcept {
            return positional.valueKind != CliOptionValueKind::Flag && !positional.sensitive && (!positional.repeatable || isLast) &&
                   (!optionalSeen || !positional.required);
        }

        [[nodiscard]] bool HasValidPositionalValueSchema(const CliPositionalDescriptor &positional,
                                                         const CliCommandRegistryLimits &limits) noexcept {
            return positional.enumerationValues.size() <= limits.maximumEnumerationValues &&
                   HasValidNumericRange(positional.valueKind, positional.numericRange);
        }

        [[nodiscard]] bool HasValidPositionalShape(const CliPositionalDescriptor &positional, const CliCommandRegistryLimits &limits,
                                                   const bool isLast, const bool optionalSeen) noexcept {
            return HasValidPositionalMetadata(positional, limits) && HasValidPositionalPolicy(positional, isLast, optionalSeen) &&
                   HasValidPositionalValueSchema(positional, limits);
        }

        [[nodiscard]] Result<void> ValidatePositionalEnumeration(const CliPositionalDescriptor &positional,
                                                                 const CliCommandRegistryLimits &limits) {
            const CliOptionDescriptor equivalent{.name = positional.name,
                                                 .summary = positional.summary,
                                                 .valueKind = positional.valueKind,
                                                 .enumerationValues = positional.enumerationValues,
                                                 .numericRange = positional.numericRange};
            return ValidateEnumerationValues(equivalent, limits);
        }

        [[nodiscard]] Result<void> ValidatePositionals(const std::span<const CliPositionalDescriptor> positionals,
                                                       const CliCommandRegistryLimits &limits) {
            bool optionalSeen = false;
            for (std::size_t index = 0; index < positionals.size(); ++index) {
                const CliPositionalDescriptor &positional = positionals[index];
                if (!HasValidPositionalShape(positional, limits, index + 1 == positionals.size(), optionalSeen) ||
                    HasDuplicatePositionalName(positionals, index))
                    return Result<void>::Failure(MakeError(CliErrors::OptionSchemaIncompatible));
                optionalSeen = optionalSeen || !positional.required;
                if (const Result<void> valid = ValidatePositionalEnumeration(positional, limits); valid.HasError())
                    return valid;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] bool HasKnownDescriptorPolicies(const CliCommandDescriptor &descriptor) noexcept {
            return IsKnown(descriptor.interactive) && IsKnown(descriptor.sideEffects) && IsKnown(descriptor.cancellation) &&
                   IsKnown(descriptor.stdinPolicy) && IsKnown(descriptor.origin);
        }

        [[nodiscard]] bool HasValidDescriptorIdentities(const CliCommandDescriptor &descriptor,
                                                        const CliCommandRegistryLimits &limits) noexcept {
            return IsCanonicalNamespacedId(descriptor.ownerId, limits.maximumIdentifierBytes) &&
                   IsCanonicalNamespacedId(descriptor.output.id, limits.maximumIdentifierBytes) && ValidAvailability(descriptor.hosts);
        }

        /** @brief Keeps prompt availability and its explicit non-interactive alternative coherent. */
        [[nodiscard]] bool HasValidInteractiveAlternative(const CliCommandDescriptor &descriptor, const CliCommandRegistryLimits &limits) {
            if (descriptor.interactive == CliInteractivePolicy::Forbidden)
                return !descriptor.interactiveAlternativeOption.has_value();
            return descriptor.interactiveAlternativeOption.has_value() &&
                   IsCanonicalToken(*descriptor.interactiveAlternativeOption, limits.maximumIdentifierBytes);
        }

        [[nodiscard]] Result<void> ValidateDescriptorMetadata(const CliCommandDescriptor &descriptor,
                                                              const CliCommandRegistryLimits &limits) {
            if (descriptor.path.segments.empty() || descriptor.path.segments.size() > limits.maximumPathSegments)
                return Result<void>::Failure(MakeError(CliErrors::DescriptorInvalid));
            if (!IsSafeSummary(descriptor.summary, limits.maximumSummaryBytes))
                return Result<void>::Failure(MakeError(CliErrors::DescriptorInvalid));
            if (descriptor.options.size() > limits.maximumOptionsPerCommand ||
                descriptor.positionals.size() > limits.maximumPositionalsPerCommand ||
                descriptor.requiredCapabilities.size() > limits.maximumCapabilitiesPerCommand)
                return Result<void>::Failure(MakeError(CliErrors::DescriptorInvalid));
            if (!HasKnownDescriptorPolicies(descriptor) || !HasValidDescriptorIdentities(descriptor, limits))
                return Result<void>::Failure(MakeError(CliErrors::DescriptorInvalid));
            if (!HasValidInteractiveAlternative(descriptor, limits))
                return Result<void>::Failure(MakeError(CliErrors::DescriptorInvalid));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateOutputSchema(const CliOutputSchema &output) {
            if (output.version == 0 || !ValidFormats(output.formats) ||
                (output.progressRecords && (output.formats & CliOutputFormat::JsonLines) == CliOutputFormat::None))
                return Result<void>::Failure(MakeError(CliErrors::OutputSchemaIncompatible));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateTimeoutPolicy(const CliTimeoutPolicy &timeout) {
            if ((timeout.defaultMilliseconds == 0) != (timeout.maximumMilliseconds == 0) ||
                timeout.defaultMilliseconds > timeout.maximumMilliseconds)
                return Result<void>::Failure(MakeError(CliErrors::DescriptorInvalid, "CLI timeout policy is invalid."));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateCommandPath(const CommandPath &path, const CliCommandRegistryLimits &limits) {
            if (!std::ranges::all_of(path.segments, [&limits](const std::string &segment) {
                return IsCanonicalToken(segment, limits.maximumIdentifierBytes);
            }))
                return Result<void>::Failure(MakeError(CliErrors::DescriptorInvalid, "CLI command path is invalid."));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateDescriptorAdmission(const CliCommandDescriptor &descriptor,
                                                               const CliCommandRegistryPolicy &policy) {
            if ((descriptor.hosts & AvailabilityFor(policy.activeHost)) == CliHostAvailability::None)
                return Result<void>::Failure(MakeError(CliErrors::HostUnsupported));
            if (descriptor.contractVersion.major == 0 || descriptor.contractVersion.major != policy.supportedContractVersion.major ||
                descriptor.contractVersion.minor > policy.supportedContractVersion.minor)
                return Result<void>::Failure(MakeError(CliErrors::ContractVersionIncompatible));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateOptions(const std::span<const CliOptionDescriptor> options,
                                                   const CliCommandRegistryLimits &limits) {
            for (std::size_t index = 0; index < options.size(); ++index) {
                if (HasDuplicateOption(options, index))
                    return Result<void>::Failure(MakeError(CliErrors::OptionNameDuplicate));
                if (std::ranges::any_of(CliOptionParser::CommonOptions(), [&options, index](const CliOptionDescriptor &common) {
                    return common.name == options[index].name ||
                           (common.shortName.has_value() && common.shortName == options[index].shortName);
                }))
                    return Result<void>::Failure(MakeError(CliErrors::OptionNameDuplicate));
                if (const Result<void> valid = ValidateOption(options[index], limits); valid.HasError())
                    return valid;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateCapabilities(const std::span<const CliCapabilityId> capabilities,
                                                        const CliCommandRegistryPolicy &policy) {
            const auto &limits = policy.limits;
            for (std::size_t index = 0; index < capabilities.size(); ++index) {
                const CliCapabilityId &capability = capabilities[index];
                if (!IsCanonicalNamespacedId(capability.value, limits.maximumIdentifierBytes) ||
                    ContainsCapability(capabilities.first(index), capability))
                    return Result<void>::Failure(MakeError(CliErrors::DescriptorInvalid, "CLI capability requirement is invalid."));
                if (!ContainsCapability(policy.grantedCapabilities, capability))
                    return Result<void>::Failure(MakeError(CliErrors::CapabilityUnauthorized));
            }
            return Result<void>::Success();
        }

        /** @brief Resolves the declared prompt alternative against command and shared host options. */
        [[nodiscard]] Result<void> ValidateInteractiveOption(const CliCommandDescriptor &descriptor) {
            if (!descriptor.interactiveAlternativeOption)
                return Result<void>::Success();
            const auto namedAlternative = [&descriptor](const CliOptionDescriptor &option) {
                return option.name == *descriptor.interactiveAlternativeOption;
            };
            const bool foundInCommand = std::ranges::any_of(descriptor.options, namedAlternative);
            if (const bool foundInCommon = std::ranges::any_of(CliOptionParser::CommonOptions(), namedAlternative);
                !foundInCommand && !foundInCommon)
                return Result<void>::Failure(MakeError(CliErrors::DescriptorInvalid));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateDescriptor(const CliCommandDescriptor &descriptor, const CliCommandRegistryPolicy &policy) {
            if (const Result<void> valid = ValidateDescriptorMetadata(descriptor, policy.limits); valid.HasError())
                return valid;
            if (const Result<void> valid = ValidateOutputSchema(descriptor.output); valid.HasError())
                return valid;
            if (const Result<void> valid = ValidateTimeoutPolicy(descriptor.timeout); valid.HasError())
                return valid;
            if (const Result<void> valid = ValidateCommandPath(descriptor.path, policy.limits); valid.HasError())
                return valid;
            if (const Result<void> valid = ValidateDescriptorAdmission(descriptor, policy); valid.HasError())
                return valid;
            if (const Result<void> valid = ValidateOptions(descriptor.options, policy.limits); valid.HasError())
                return valid;
            if (const Result<void> valid = ValidatePositionals(descriptor.positionals, policy.limits); valid.HasError())
                return valid;
            if (const Result<void> valid = ValidateInteractiveOption(descriptor); valid.HasError())
                return valid;
            return ValidateCapabilities(descriptor.requiredCapabilities, policy);
        }

        void Canonicalize(CliCommandDescriptor &descriptor) {
            std::ranges::sort(descriptor.options, {}, &CliOptionDescriptor::name);
            for (CliOptionDescriptor &option : descriptor.options)
                std::ranges::sort(option.enumerationValues);
            std::ranges::sort(descriptor.requiredCapabilities, {}, &CliCapabilityId::value);
        }

    }  // namespace

    /** @copydoc CliCommandRegistry::Create */
    Result<CliCommandRegistry> CliCommandRegistry::Create(const std::span<const CliCommandDescriptor> descriptors,
                                                          const CliCommandRegistryPolicy &policy) {
        if (const Result<void> validPolicy = ValidatePolicy(policy); validPolicy.HasError())
            return Result<CliCommandRegistry>::Failure(validPolicy.ErrorValue());
        if (descriptors.size() > policy.limits.maximumCommands)
            return Result<CliCommandRegistry>::Failure(MakeError(CliErrors::RegistryCapacityExceeded));

        for (const CliCommandDescriptor &descriptor : descriptors) {
            if (const Result<void> validDescriptor = ValidateDescriptor(descriptor, policy); validDescriptor.HasError())
                return Result<CliCommandRegistry>::Failure(validDescriptor.ErrorValue());
        }

        CliCommandRegistry registry;
        registry.commands_.assign(descriptors.begin(), descriptors.end());
        for (CliCommandDescriptor &descriptor : registry.commands_)
            Canonicalize(descriptor);
        std::ranges::sort(registry.commands_, {}, &CliCommandDescriptor::path);
        for (std::size_t index = 1; index < registry.commands_.size(); ++index) {
            if (registry.commands_[index - 1].path == registry.commands_[index].path)
                return Result<CliCommandRegistry>::Failure(MakeError(CliErrors::CommandPathDuplicate));
        }
        return Result<CliCommandRegistry>::Success(std::move(registry));
    }

    /** @copydoc CliCommandRegistry::Commands */
    std::span<const CliCommandDescriptor> CliCommandRegistry::Commands() const noexcept {
        return commands_;
    }

    /** @copydoc CliCommandRegistry::Find */
    const CliCommandDescriptor *CliCommandRegistry::Find(const CommandPath &path) const noexcept {
        const auto found = std::ranges::lower_bound(commands_, path, {}, &CliCommandDescriptor::path);
        return found != commands_.end() && found->path == path ? std::to_address(found) : nullptr;
    }

}  // namespace Horo::Cli
