#include "Horo/Release/DistributionModel.h"

#include "Horo/Release/ReleaseErrors.h"

#include <algorithm>
#include <array>
#include <memory>
#include <string_view>
#include <variant>

namespace Horo::Release {
    namespace {
        using Capability = DistributionCapability;
        using Layout = DistributionInstallLayout;

        struct FormatDescriptor final {
            DistributionPackageFormat format;
            std::array<bool, 3> platforms;
            DistributionPackageCapabilities capabilities;
        };

        constexpr std::array
            FormatDescriptors{FormatDescriptor{DistributionPackageFormat::WindowsMsi,
                                               {true, false, false},
                                               {Layout::SystemManaged, Capability::Supported, Capability::Supported, Capability::Required,
                                                Capability::Supported, Capability::Required, Capability::Unsupported}},
                              FormatDescriptor{DistributionPackageFormat::WindowsExeInstaller,
                                               {true, false, false},
                                               {Layout::SystemManaged, Capability::Supported, Capability::Supported, Capability::Required,
                                                Capability::Supported, Capability::Required, Capability::Supported}},
                              FormatDescriptor{DistributionPackageFormat::ZipArchive,
                                               {true, true, false},
                                               {Layout::Portable, Capability::Unsupported, Capability::Unsupported, Capability::Supported,
                                                Capability::Supported, Capability::Supported, Capability::Supported}},
                              FormatDescriptor{DistributionPackageFormat::MacDmg,
                                               {false, true, false},
                                               {Layout::ApplicationBundle, Capability::Supported, Capability::Supported,
                                                Capability::Supported, Capability::Supported, Capability::Required, Capability::Supported}},
                              FormatDescriptor{DistributionPackageFormat::MacPkg,
                                               {false, true, false},
                                               {Layout::SystemManaged, Capability::Supported, Capability::Supported, Capability::Required,
                                                Capability::Supported, Capability::Required, Capability::Unsupported}},
                              FormatDescriptor{DistributionPackageFormat::MacAppBundle,
                                               {false, true, false},
                                               {Layout::ApplicationBundle, Capability::Supported, Capability::Supported,
                                                Capability::Supported, Capability::Supported, Capability::Required, Capability::Supported}},
                              FormatDescriptor{DistributionPackageFormat::LinuxAppImage,
                                               {false, false, true},
                                               {Layout::Portable, Capability::Supported, Capability::Supported, Capability::Supported,
                                                Capability::Supported, Capability::Supported, Capability::Supported}},
                              FormatDescriptor{DistributionPackageFormat::TarGzip,
                                               {false, false, true},
                                               {Layout::Portable, Capability::Unsupported, Capability::Unsupported, Capability::Supported,
                                                Capability::Supported, Capability::Supported, Capability::Supported}},
                              FormatDescriptor{DistributionPackageFormat::LinuxDeb,
                                               {false, false, true},
                                               {Layout::SystemManaged, Capability::Supported, Capability::Supported, Capability::Required,
                                                Capability::Supported, Capability::Required, Capability::Unsupported}},
                              FormatDescriptor{DistributionPackageFormat::LinuxRpm,
                                               {false, false, true},
                                               {Layout::SystemManaged, Capability::Supported, Capability::Supported, Capability::Required,
                                                Capability::Supported, Capability::Required, Capability::Unsupported}},
                              FormatDescriptor{DistributionPackageFormat::StorePackage,
                                               {true, true, true},
                                               {Layout::StoreManaged, Capability::Supported, Capability::Required, Capability::Required,
                                                Capability::Required, Capability::Required, Capability::Supported}}};

        [[nodiscard]] bool IsIdentityCharacter(const char value) noexcept {
            return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '.' || value == '_' || value == '-';
        }

        [[nodiscard]] bool IsKnownProduct(const DistributionProductKind kind) noexcept {
            return kind >= DistributionProductKind::Editor && kind <= DistributionProductKind::GameDedicatedServer;
        }

        [[nodiscard]] bool IsKnownPlatform(const DistributionPlatform platform) noexcept {
            return platform >= DistributionPlatform::Windows && platform <= DistributionPlatform::Linux;
        }

        [[nodiscard]] bool IsKnownArchitecture(const DistributionArchitecture architecture) noexcept {
            return architecture >= DistributionArchitecture::X64 && architecture <= DistributionArchitecture::Arm64;
        }

        [[nodiscard]] bool IsKnownArtifactClass(const DistributionArtifactClass artifactClass) noexcept {
            return artifactClass >= DistributionArtifactClass::InstallableProduct &&
                   artifactClass <= DistributionArtifactClass::Diagnostics;
        }

        [[nodiscard]] const FormatDescriptor *FindDescriptor(const DistributionPackageFormat format) noexcept {
            const auto descriptor = std::ranges::find(FormatDescriptors, format, &FormatDescriptor::format);
            return descriptor == FormatDescriptors.end() ? nullptr : std::to_address(descriptor);
        }

        [[nodiscard]] bool ProductVersionMatches(const DistributionArtifactIdentity &artifact) noexcept {
            const bool gameProduct = artifact.product.kind == DistributionProductKind::GameRuntime ||
                                     artifact.product.kind == DistributionProductKind::GameDedicatedServer;
            return gameProduct == std::holds_alternative<GameProductVersion>(artifact.version);
        }

        /** @brief Rejects version wrappers whose public semantic-version payload bypassed canonical parsing. */
        [[nodiscard]] bool ValidProductVersion(const DistributionArtifactIdentity &artifact) {
            return std::visit([](const auto &productVersion) {
                const auto parsed = ParseReleaseVersion(FormatReleaseVersion(productVersion.value));
                return parsed.HasValue() && parsed.Value() == productVersion.value;
            }, artifact.version);
        }

        [[nodiscard]] bool ValidProductIdentity(const DistributionProductIdentity &product) noexcept {
            if (!IsKnownProduct(product.kind))
                return false;
            const bool renderer = product.kind == DistributionProductKind::RendererComponent;
            return renderer ? IsValidDistributionIdentity(product.componentId) : product.componentId.empty();
        }

        [[nodiscard]] bool ValidArtifactIdentity(const DistributionArtifactIdentity &artifact) {
            return ValidProductIdentity(artifact.product) && IsKnownArchitecture(artifact.architecture) &&
                   IsKnownArtifactClass(artifact.artifactClass) && IsValidDistributionIdentity(artifact.build.value) &&
                   IsValidDistributionIdentity(artifact.package.value) && ProductVersionMatches(artifact) && ValidProductVersion(artifact);
        }

        [[nodiscard]] bool ValidInstallationIdentity(const DistributionArtifactIdentity &artifact) {
            return !artifact.installation.has_value() || IsValidDistributionIdentity(artifact.installation->value);
        }

        [[nodiscard]] bool ValidInstallationCombination(const DistributionArtifactIdentity &artifact) noexcept {
            const bool installable = artifact.artifactClass == DistributionArtifactClass::InstallableProduct;
            return installable == artifact.installation.has_value();
        }

        [[nodiscard]] bool SupportsArtifactClassFormat(const DistributionArtifactClass artifactClass,
                                                       const DistributionPackageFormat format) noexcept {
            if (artifactClass == DistributionArtifactClass::InstallableProduct)
                return true;
            return format == DistributionPackageFormat::ZipArchive || format == DistributionPackageFormat::TarGzip;
        }

        [[nodiscard]] bool SupportsRendererComponentFormat(const DistributionPackageFormat format) noexcept {
            using enum DistributionPackageFormat;
            return format == ZipArchive || format == TarGzip;
        }

        [[nodiscard]] bool SupportsDedicatedServerFormat(const DistributionPackageFormat format) noexcept {
            using enum DistributionPackageFormat;
            return format != MacAppBundle && format != LinuxAppImage && format != StorePackage;
        }

        [[nodiscard]] bool SupportsSdkFormat(const DistributionPackageFormat format) noexcept {
            using enum DistributionPackageFormat;
            return format != WindowsExeInstaller && format != MacDmg && format != MacAppBundle && format != LinuxAppImage &&
                   format != StorePackage;
        }

        [[nodiscard]] bool SupportsProduct(const DistributionProductIdentity &product, const DistributionPackageFormat format) noexcept {
            using enum DistributionProductKind;
            switch (product.kind) {
                case RendererComponent:
                    return SupportsRendererComponentFormat(format);
                case GameDedicatedServer:
                    return SupportsDedicatedServerFormat(format);
                case PublicSdk:
                    return SupportsSdkFormat(format);
                default:
                    return true;
            }
        }
    }  // namespace

    /** @copydoc IsValidDistributionIdentity */
    bool IsValidDistributionIdentity(const std::string_view value) noexcept {
        return !value.empty() && value.size() <= MaximumDistributionIdentityBytes && value.front() >= 'a' && value.front() <= 'z' &&
               std::ranges::all_of(value, IsIdentityCharacter);
    }

    /** @copydoc DescribeDistributionPackageFormat */
    Result<DistributionPackageCapabilities> DescribeDistributionPackageFormat(const DistributionPackageFormat format,
                                                                              const DistributionPlatform platform) {
        const FormatDescriptor *descriptor = FindDescriptor(format);
        if (!descriptor || !IsKnownPlatform(platform) || !descriptor->platforms[static_cast<std::size_t>(platform)])
            return Result<DistributionPackageCapabilities>::Failure(MakeError(ReleaseErrors::DistributionCombinationUnsupported));
        return Result<DistributionPackageCapabilities>::Success(descriptor->capabilities);
    }

    /** @copydoc ValidateDistributionProductPackageFormat */
    Result<DistributionPackageCapabilities> ValidateDistributionProductPackageFormat(const DistributionProductIdentity &product,
                                                                                     const DistributionArtifactClass artifactClass,
                                                                                     const DistributionPlatform platform,
                                                                                     const DistributionPackageFormat format) {
        if (!ValidProductIdentity(product) || !IsKnownArtifactClass(artifactClass))
            return Result<DistributionPackageCapabilities>::Failure(MakeError(ReleaseErrors::DistributionIdentityInvalid));
        auto capabilities = DescribeDistributionPackageFormat(format, platform);
        if (capabilities.HasError() || !SupportsArtifactClassFormat(artifactClass, format) || !SupportsProduct(product, format))
            return Result<DistributionPackageCapabilities>::Failure(MakeError(ReleaseErrors::DistributionCombinationUnsupported));
        return capabilities;
    }

    /** @copydoc ValidateDistributionPackageSelection */
    Result<DistributionPackageSelection> ValidateDistributionPackageSelection(const DistributionArtifactIdentity &artifact,
                                                                              const DistributionPackageFormat format) {
        if (!ValidArtifactIdentity(artifact))
            return Result<DistributionPackageSelection>::Failure(MakeError(ReleaseErrors::DistributionIdentityInvalid));
        if (!ValidInstallationIdentity(artifact))
            return Result<DistributionPackageSelection>::Failure(MakeError(ReleaseErrors::DistributionIdentityInvalid));
        if (!ValidInstallationCombination(artifact))
            return Result<DistributionPackageSelection>::Failure(MakeError(ReleaseErrors::DistributionCombinationUnsupported));
        auto capabilities = ValidateDistributionProductPackageFormat(artifact.product, artifact.artifactClass, artifact.platform, format);
        if (capabilities.HasError())
            return Result<DistributionPackageSelection>::Failure(capabilities.ErrorValue());
        return Result<DistributionPackageSelection>::Success({artifact, format, std::move(capabilities).Value()});
    }
}  // namespace Horo::Release
