#include "Horo/UiTemplates/UiTemplateDependencyGraph.h"

#include "Horo/UiTemplates/UiTemplateErrors.h"

#include <algorithm>
#include <iterator>
#include <ranges>
#include <utility>

namespace Horo::UiTemplates {
    namespace {
        template <typename T = void> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &code) {
            return Result<T>::Failure(MakeError(code));
        }

        [[nodiscard]] bool HasDigest(const Sha256Digest &digest) noexcept {
            return digest != Sha256Digest{};
        }

        [[nodiscard]] bool ValidReference(const UiTemplateReference &reference) noexcept {
            return reference.asset.IsValid() && HasDigest(reference.revision) && reference.minimumInterface.major != 0;
        }

        /** @brief Rejects resolution after shutdown or with a malformed root. */
        [[nodiscard]] Result<void> ValidateAdmission(const bool active, const UiTemplateReference &root) {
            if (!active)
                return Failure(UiErrors::TemplateGraphShutdown);
            if (!ValidReference(root))
                return Failure(UiErrors::TemplateGraphInvalid);
            return Result<void>::Success();
        }

        [[nodiscard]] bool ValidPackageRange(const Packages::PackageVersionRange &range) noexcept {
            using enum Packages::PackageVersionRange::Kind;
            return range.kind == Any || range.kind == Exact || range.kind == Caret;
        }

        [[nodiscard]] bool Compatible(const UiTemplateInterfaceVersion available, const UiTemplateInterfaceVersion minimum) noexcept {
            return available.major == minimum.major && available.minor >= minimum.minor;
        }

        /** @brief Finds the exact accepted descriptor and checks its public interface. */
        [[nodiscard]] Result<const UiTemplateDependencyDescriptor *> FindCompatibleDescriptor(
            const std::span<const UiTemplateDependencyDescriptor> descriptors, const UiTemplateReference &reference) {
            const auto found = std::ranges::find(descriptors, reference.asset, &UiTemplateDependencyDescriptor::asset);
            if (found == descriptors.end())
                return Result<const UiTemplateDependencyDescriptor *>::Failure(
                    MakeError(UiErrors::TemplateMissing, reference.asset.asset.ToString()));
            if (found->revision != reference.revision)
                return Result<const UiTemplateDependencyDescriptor *>::Failure(
                    MakeError(UiErrors::TemplateRevisionUnavailable, reference.asset.asset.ToString()));
            if (!Compatible(found->interfaceVersion, reference.minimumInterface))
                return Result<const UiTemplateDependencyDescriptor *>::Failure(
                    MakeError(UiErrors::TemplateVersionIncompatible, reference.asset.asset.ToString()));
            return Result<const UiTemplateDependencyDescriptor *>::Success(&*found);
        }

        /** @brief Adds required pinned packages once, rejecting missing or incompatible locks. */
        [[nodiscard]] Result<void> AppendPackages(const std::span<const UiTemplatePackageRequirement> requirements,
                                                  const std::span<const Packages::LockedPackageReference> locks,
                                                  const std::size_t maximumPackages,
                                                  std::vector<Packages::LockedPackageReference> &resolved) {
            for (const auto &requirement : requirements) {
                const auto locked = std::ranges::find_if(locks, [&](const auto &package) {
                    return package.package == requirement.package;
                });
                if (locked == locks.end() || !requirement.versions.Allows(locked->version))
                    return Result<void>::Failure(MakeError(UiErrors::TemplatePackageUnavailable, requirement.package.Value()));
                if (std::ranges::find_if(resolved, [&](const auto &package) {
                    return package.package == requirement.package;
                }) == resolved.end()) {
                    if (resolved.size() >= maximumPackages)
                        return Failure(UiErrors::TemplateGraphBudgetExceeded);
                    resolved.push_back(*locked);
                }
            }
            return Result<void>::Success();
        }

        /** @brief Canonicalizes the detached package closure before publication. */
        [[nodiscard]] Result<UiTemplateResolvedDependencies> FinishClosure(UiTemplateResolvedDependencies resolved) {
            std::ranges::sort(resolved.packages, {}, &Packages::LockedPackageReference::package);
            return Result<UiTemplateResolvedDependencies>::Success(std::move(resolved));
        }
    }  // namespace

    /** @copydoc UiTemplateGraphLimits::IsValid */
    bool UiTemplateGraphLimits::IsValid() const noexcept {
        return maximumTemplates > 0 && maximumTemplates <= 4096 && maximumEdges > 0 && maximumEdges <= 16384 && maximumDepth > 0 &&
               maximumDepth <= 64 && maximumPackages > 0 && maximumPackages <= 4096;
    }

    /** @copydoc UiTemplateDependencyGraph::UiTemplateDependencyGraph */
    UiTemplateDependencyGraph::UiTemplateDependencyGraph(std::vector<UiTemplateDependencyDescriptor> descriptors,
                                                         std::vector<Packages::LockedPackageReference> packages,
                                                         const UiTemplateGraphLimits limits) noexcept
        : descriptors_(std::move(descriptors)), packages_(std::move(packages)), limits_(limits) {}

    /** @copydoc UiTemplateDependencyGraph::Create */
    Result<UiTemplateDependencyGraph> UiTemplateDependencyGraph::Create(const std::span<const UiTemplateDependencyDescriptor> descriptors,
                                                                        const std::span<const Packages::LockedPackageReference> packages,
                                                                        const UiTemplateGraphLimits limits) {
        if (!limits.IsValid() || descriptors.size() > limits.maximumTemplates || packages.size() > limits.maximumPackages)
            return Failure<UiTemplateDependencyGraph>(UiErrors::TemplateGraphBudgetExceeded);

        std::size_t admittedEdges{};
        for (const auto &descriptor : descriptors) {
            if (descriptor.nested.size() > limits.maximumEdges - admittedEdges)
                return Failure<UiTemplateDependencyGraph>(UiErrors::TemplateGraphBudgetExceeded);
            admittedEdges += descriptor.nested.size();
            if (descriptor.packages.size() > limits.maximumEdges - admittedEdges)
                return Failure<UiTemplateDependencyGraph>(UiErrors::TemplateGraphBudgetExceeded);
            admittedEdges += descriptor.packages.size();
        }

        std::vector<UiTemplateDependencyDescriptor> ownedDescriptors{descriptors.begin(), descriptors.end()};
        std::vector<Packages::LockedPackageReference> ownedPackages{packages.begin(), packages.end()};
        std::ranges::sort(ownedDescriptors, {}, &UiTemplateDependencyDescriptor::asset);
        std::ranges::sort(ownedPackages, {}, [](const auto &package) {
            return package.package.Value();
        });

        for (std::size_t index = 0; index < ownedDescriptors.size(); ++index) {
            auto &descriptor = ownedDescriptors[index];
            if (!descriptor.asset.IsValid() || !HasDigest(descriptor.revision) || descriptor.interfaceVersion.major == 0 ||
                (index > 0 && ownedDescriptors[index - 1].asset == descriptor.asset))
                return Failure<UiTemplateDependencyGraph>(UiErrors::TemplateGraphInvalid);
            if (descriptor.schema.major != CurrentUiTemplateSchemaVersion.major ||
                descriptor.schema.minor > CurrentUiTemplateSchemaVersion.minor)
                return Failure<UiTemplateDependencyGraph>(UiErrors::TemplateVersionIncompatible);
            for (const auto &nested : descriptor.nested) {
                if (!ValidReference(nested))
                    return Failure<UiTemplateDependencyGraph>(UiErrors::TemplateGraphInvalid);
            }
            for (const auto &package : descriptor.packages) {
                if (!ValidPackageRange(package.versions))
                    return Failure<UiTemplateDependencyGraph>(UiErrors::TemplateGraphInvalid);
            }
            std::ranges::sort(descriptor.nested, {}, &UiTemplateReference::asset);
            std::ranges::sort(descriptor.packages, {}, [](const auto &package) {
                return package.package.Value();
            });
        }
        for (std::size_t index = 1; index < ownedPackages.size(); ++index) {
            if (ownedPackages[index - 1].package == ownedPackages[index].package)
                return Failure<UiTemplateDependencyGraph>(UiErrors::TemplateGraphInvalid);
        }
        return Result<UiTemplateDependencyGraph>::Success(
            UiTemplateDependencyGraph{std::move(ownedDescriptors), std::move(ownedPackages), limits});
    }

    /** @copydoc UiTemplateDependencyGraph::Resolve */
    Result<UiTemplateResolvedDependencies> UiTemplateDependencyGraph::Resolve(const UiTemplateReference &root) const {
        if (const auto admission = ValidateAdmission(active_, root); admission.HasError())
            return Result<UiTemplateResolvedDependencies>::Failure(admission.ErrorValue());

        UiTemplateResolvedDependencies resolved;
        std::vector<UiTemplateAssetId> active;
        std::vector<std::size_t> subtreeHeights;
        std::size_t traversedEdges{};
        auto visit = [&](auto &&self, const UiTemplateReference &reference, const std::size_t depth) -> Result<void> {
            if (std::ranges::find(active, reference.asset) != active.end())
                return Result<void>::Failure(MakeError(UiErrors::TemplateDependencyCycle, reference.asset.asset.ToString()));
            if (depth > limits_.maximumDepth)
                return Failure(UiErrors::TemplateGraphBudgetExceeded);
            const auto descriptor = FindCompatibleDescriptor(descriptors_, reference);
            if (descriptor.HasError())
                return Result<void>::Failure(descriptor.ErrorValue());
            const auto resolvedTemplate = std::ranges::find(resolved.templates, reference.asset);
            if (resolvedTemplate != resolved.templates.end()) {
                const auto index = static_cast<std::size_t>(std::distance(resolved.templates.begin(), resolvedTemplate));
                if (subtreeHeights[index] > limits_.maximumDepth - depth + 1)
                    return Failure(UiErrors::TemplateGraphBudgetExceeded);
                return Result<void>::Success();
            }
            active.push_back(reference.asset);
            std::size_t subtreeHeight{1};
            for (const auto &nested : descriptor.Value()->nested) {
                if (traversedEdges >= limits_.maximumEdges)
                    return Failure(UiErrors::TemplateGraphBudgetExceeded);
                ++traversedEdges;
                if (const auto child = self(self, nested, depth + 1); child.HasError())
                    return child;
                const auto resolvedChild = std::ranges::find(resolved.templates, nested.asset);
                const auto childIndex = static_cast<std::size_t>(std::distance(resolved.templates.begin(), resolvedChild));
                subtreeHeight = std::max(subtreeHeight, subtreeHeights[childIndex] + 1);
            }
            if (const auto packages = AppendPackages(descriptor.Value()->packages, packages_, limits_.maximumPackages, resolved.packages);
                packages.HasError())
                return packages;
            active.pop_back();
            if (resolved.templates.size() >= limits_.maximumTemplates)
                return Failure(UiErrors::TemplateGraphBudgetExceeded);
            resolved.templates.push_back(reference.asset);
            subtreeHeights.push_back(subtreeHeight);
            return Result<void>::Success();
        };
        if (const auto result = visit(visit, root, 1); result.HasError())
            return Result<UiTemplateResolvedDependencies>::Failure(result.ErrorValue());
        return FinishClosure(std::move(resolved));
    }

    /** @copydoc UiTemplateDependencyGraph::Shutdown */
    void UiTemplateDependencyGraph::Shutdown() noexcept {
        active_ = false;
        descriptors_.clear();
        packages_.clear();
    }
}  // namespace Horo::UiTemplates
