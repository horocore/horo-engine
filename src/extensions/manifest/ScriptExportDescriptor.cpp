#include "ScriptExportDescriptorInternal.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <new>
#include <ranges>
#include <set>
#include <utility>

namespace Horo::Extensions {
    namespace {
        [[nodiscard]] Result<std::vector<ScriptExportDescriptor>> BuildCandidate(const std::span<const ScriptExportDescriptor> descriptors,
                                                                                 const ScriptExportDescriptorLimits &limits) {
            if (!Detail::IsValidScriptExportLimits(limits))
                return Detail::ScriptExportDescriptorInvalid<std::vector<ScriptExportDescriptor>>(
                    "Script export descriptor limits are malformed.");
            if (descriptors.size() > limits.maximumDescriptors)
                return Detail::ScriptExportDescriptorCapacity<std::vector<ScriptExportDescriptor>>(
                    "Script export descriptor generation is too large.");

            std::vector<ScriptExportDescriptor> candidate{descriptors.begin(), descriptors.end()};
            std::set<std::string, std::less<>> namespaces;
            for (ScriptExportDescriptor &descriptor : candidate) {
                if (const Result<void> valid = Detail::ValidateScriptExportDescriptorData(descriptor, limits); valid.HasError())
                    return Result<std::vector<ScriptExportDescriptor>>::Failure(valid.ErrorValue());
                if (!namespaces.insert(descriptor.nameSpace).second)
                    return Detail::ScriptExportDescriptorConflict<std::vector<ScriptExportDescriptor>>(
                        "Script API namespaces must be unique in one generation.");
                Detail::SortScriptExportDescriptor(descriptor);
            }
            std::ranges::sort(candidate, {}, &ScriptExportDescriptor::id);
            if (std::ranges::adjacent_find(candidate, {}, &ScriptExportDescriptor::id) != candidate.end())
                return Detail::ScriptExportDescriptorConflict<std::vector<ScriptExportDescriptor>>(
                    "Script API identities must be unique in one generation.");
            return Result<std::vector<ScriptExportDescriptor>>::Success(std::move(candidate));
        }
    }  // namespace

    ScriptExportDescriptorSnapshot::ScriptExportDescriptorSnapshot(ConstructionKey, std::vector<ScriptExportDescriptor> descriptors,
                                                                   const Sha256Digest &fingerprint, const std::uint64_t generation)
        : descriptors_(std::move(descriptors)), fingerprint_(fingerprint), generation_(generation) {}

    /** @copydoc ScriptExportDescriptorSnapshot::Descriptors */
    std::span<const ScriptExportDescriptor> ScriptExportDescriptorSnapshot::Descriptors() const noexcept {
        return descriptors_;
    }

    /** @copydoc ScriptExportDescriptorSnapshot::Find */
    const ScriptExportDescriptor *ScriptExportDescriptorSnapshot::Find(const std::string_view id) const noexcept {
        return Detail::FindScriptExportDescriptor(descriptors_, id);
    }

    /** @copydoc ScriptExportDescriptorSnapshot::Fingerprint */
    const Sha256Digest &ScriptExportDescriptorSnapshot::Fingerprint() const noexcept {
        return fingerprint_;
    }

    /** @copydoc ScriptExportDescriptorSnapshot::Generation */
    std::uint64_t ScriptExportDescriptorSnapshot::Generation() const noexcept {
        return generation_;
    }

    Result<std::shared_ptr<const ScriptExportDescriptorSnapshot>> ScriptExportDescriptorSnapshot::BuildFromCandidate(
        Result<std::vector<ScriptExportDescriptor>> candidate, const std::uint64_t generation) {
        if (candidate.HasError())
            return Result<std::shared_ptr<const ScriptExportDescriptorSnapshot>>::Failure(candidate.ErrorValue());
        auto canonical = std::move(candidate).Value();
        const Sha256Digest fingerprint = Detail::ComputeScriptExportDescriptorFingerprint(canonical);
        return Result<std::shared_ptr<const ScriptExportDescriptorSnapshot>>::Success(
            std::make_shared<const ScriptExportDescriptorSnapshot>(ConstructionKey{}, std::move(canonical), fingerprint, generation));
    }

    /** @copydoc BuildScriptExportDescriptorSnapshot */
    Result<ScriptExportDescriptorSnapshotPtr> BuildScriptExportDescriptorSnapshot(const std::span<const ScriptExportDescriptor> descriptors,
                                                                                  const ScriptExportDescriptorLimits &limits) {
        try {
            return ScriptExportDescriptorSnapshot::BuildFromCandidate(BuildCandidate(descriptors, limits), 1);
        } catch (const std::bad_alloc &) {
            return Result<ScriptExportDescriptorSnapshotPtr>::Failure(MakeError(ExtensionErrors::ScriptExportDescriptorCapacityExceeded));
        }
    }

    /** @copydoc BuildScriptExportDescriptorReplacement */
    Result<ScriptExportDescriptorSnapshotPtr> BuildScriptExportDescriptorReplacement(
        const ScriptExportDescriptorSnapshotPtr &previous, const std::span<const ScriptExportDescriptor> descriptors,
        const ScriptExportDescriptorLimits &limits) {
        try {
            auto candidate = BuildCandidate(descriptors, limits);
            if (candidate.HasError())
                return Result<ScriptExportDescriptorSnapshotPtr>::Failure(candidate.ErrorValue());
            if (const Result<void> compatible = Detail::ValidateScriptExportReplacement(previous, candidate.Value()); compatible.HasError())
                return Result<ScriptExportDescriptorSnapshotPtr>::Failure(compatible.ErrorValue());
            if (previous->Generation() == std::numeric_limits<std::uint64_t>::max())
                return Result<ScriptExportDescriptorSnapshotPtr>::Failure(
                    MakeError(ExtensionErrors::ScriptExportDescriptorCapacityExceeded));
            return ScriptExportDescriptorSnapshot::BuildFromCandidate(std::move(candidate), previous->Generation() + 1U);
        } catch (const std::bad_alloc &) {
            return Result<ScriptExportDescriptorSnapshotPtr>::Failure(MakeError(ExtensionErrors::ScriptExportDescriptorCapacityExceeded));
        }
    }
}  // namespace Horo::Extensions
