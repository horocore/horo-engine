#include "Horo/Editor/FractureAssetDocument.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <new>
#include <ranges>

namespace Horo::Editor {
    namespace {
        /** @brief Tests exact dependency fingerprints without treating a missing digest as current. */
        bool HasDigest(const Sha256Digest &digest) noexcept {
            return std::ranges::any_of(digest.bytes, [](const auto byte) {
                return byte != 0;
            });
        }

        /** @brief Finds a stable chunk in a validated canonical table. */
        const FractureSourceChunk *FindChunk(const FractureAssetSource &source, Destruction::DestructionChunkId id) {
            const auto found = std::ranges::lower_bound(source.chunks, id, {}, &FractureSourceChunk::id);
            return found != source.chunks.end() && found->id == id ? std::to_address(found) : nullptr;
        }

        /** @brief Validates the hierarchy with bounded parent traversal and exact material references. */
        bool ValidChunks(const FractureAssetSource &source, const std::uint32_t maximumDepth) {
            Destruction::DestructionChunkId previous;
            for (const auto &chunk : source.chunks) {
                if (!chunk.id.IsValid() || chunk.id <= previous ||
                    !std::ranges::binary_search(source.materials, chunk.materialSlot, {}, &FractureSourceMaterial::slot))
                    return false;
                previous = chunk.id;
                const auto *current = &chunk;
                std::uint32_t depth = 1;
                while (current->parent.IsValid()) {
                    current = FindChunk(source, current->parent);
                    if (!current)
                        return false;
                    ++depth;
                    if (depth > maximumDepth)
                        return false;
                }
            }
            return true;
        }

        /** @brief Validates strict stable edge order; undirected connectivity cycles are permitted. */
        bool ValidContacts(const FractureAssetSource &source) {
            std::pair<Destruction::DestructionChunkId, Destruction::DestructionChunkId> previous;
            for (const auto &contact : source.contacts) {
                const auto key = std::pair{contact.low, contact.high};
                if (contact.low >= contact.high || key <= previous || !FindChunk(source, contact.low) || !FindChunk(source, contact.high) ||
                    !std::isfinite(contact.weight) || contact.weight <= 0)
                    return false;
                previous = key;
            }
            return true;
        }

        /** @brief Walks the bounded authored support graph; every required chunk must reach an anchor. */
        bool ValidRequiredSupport(const FractureAssetSource &source) {
            std::array<bool, Destruction::DestructionHardLimits::ChunksPerDestructible> reachable{};
            std::array<std::size_t, Destruction::DestructionHardLimits::ChunksPerDestructible> queue{};
            std::size_t count = 0;
            for (std::size_t index = 0; index < source.chunks.size(); ++index) {
                if (source.chunks[index].anchor) {
                    reachable[index] = true;
                    queue[count++] = index;
                }
            }
            std::size_t next{};
            while (next < count) {
                const auto id = source.chunks[queue[next]].id;
                ++next;
                for (const auto &contact : source.contacts) {
                    if (contact.low != id && contact.high != id)
                        continue;
                    const auto other = contact.low == id ? contact.high : contact.low;
                    const auto index = static_cast<std::size_t>(FindChunk(source, other) - source.chunks.data());
                    if (!reachable[index]) {
                        reachable[index] = true;
                        queue[count++] = index;
                    }
                }
            }
            for (std::size_t index = 0; index < source.chunks.size(); ++index) {
                if (source.chunks[index].required && !reachable[index])
                    return false;
            }
            return true;
        }

        /** @brief Validates material identity and stable slot order without source names or paths. */
        bool ValidMaterials(const FractureAssetSource &source) {
            std::optional<std::uint32_t> previous;
            for (const auto &material : source.materials) {
                if ((previous && material.slot <= *previous) || !material.asset.IsValid() || !HasDigest(material.digest))
                    return false;
                previous = material.slot;
            }
            return std::ranges::binary_search(source.materials, source.settings.interiorMaterialSlot, {}, &FractureSourceMaterial::slot);
        }

        /** @brief Checks explicit stable Voronoi sites; imported source retains no generated geometry. */
        bool ValidSites(const FractureAssetSource &source) {
            const auto &settings = source.settings;
            if (settings.algorithm == FractureSourceAlgorithm::PreFractured)
                return settings.sites.empty();
            if (settings.sites.empty() || settings.sites.size() != source.chunks.size())
                return false;
            for (std::size_t index = 0; index < settings.sites.size(); ++index) {
                const auto &site = settings.sites[index];
                if (site.chunk != source.chunks[index].id || !std::ranges::all_of(site.position, [](double value) {
                    return std::isfinite(value);
                }))
                    return false;
                for (std::size_t other = 0; other < index; ++other) {
                    if (settings.sites[other].position == site.position)
                        return false;
                }
            }
            return true;
        }
    }  // namespace

    /** @copydoc ValidateFractureAssetSource */
    Result<void> ValidateFractureAssetSource(const FractureAssetSource &source) {
        using namespace Destruction;
        const auto fail = [](const ErrorCodeDescriptor &error) {
            return Result<void>::Failure(MakeError(error));
        };
        const auto &settings = source.settings;
        if (source.chunks.size() > DestructionHardLimits::ChunksPerDestructible || source.contacts.size() > MaximumFractureContacts ||
            source.materials.size() > MaximumFractureMaterials || settings.sites.size() > DestructionHardLimits::ChunksPerDestructible)
            return fail(FractureDocumentErrors::LimitExceeded);
        if (!source.asset.IsValid() || !settings.sourceMesh.IsValid() || !settings.sourceRevision.IsValid() ||
            !HasDigest(settings.sourceDigest) || !settings.recipe.IsValid() || !settings.recipeRevision.IsValid() ||
            settings.algorithmVersion == 0 || !HasDigest(settings.toolchainDigest) ||
            settings.algorithm > FractureSourceAlgorithm::Voronoi || !std::isfinite(settings.exteriorUvScale) ||
            !std::isfinite(settings.interiorUvScale) || settings.exteriorUvScale <= 0 || settings.interiorUvScale <= 0 ||
            source.chunks.empty())
            return fail(FractureDocumentErrors::InvalidSource);
        const auto profile = GetDestructionTierProfile(settings.tier);
        if (profile.HasError())
            return Result<void>::Failure(profile.ErrorValue());
        if (source.chunks.size() > profile.Value().limits.maximumChunksPerDestructible)
            return fail(FractureDocumentErrors::LimitExceeded);
        const auto content =
            FractureArtifactContentIdentity::Create(source.asset, FractureContentRevision::Create(1).Value(), settings.sourceDigest);
        if (content.HasError())
            return Result<void>::Failure(content.ErrorValue());
        if (const auto descriptor =
                DestructibleDescriptor::Create({.destructible = DestructibleId::Create(1).Value(),
                                                .content = content.Value(),
                                                .configurationRevision = DestructionConfigurationRevision::Create(1).Value(),
                                                .tier = settings.tier,
                                                .features = {.required = settings.requiredFeatures},
                                                .limits = profile.Value().limits,
                                                .health = source.damage.health,
                                                .behavior = source.damage.behavior,
                                                .cleanup = source.damage.cleanup});
            descriptor.HasError())
            return Result<void>::Failure(descriptor.ErrorValue());
        if (!ValidMaterials(source) || !ValidChunks(source, profile.Value().limits.maximumHierarchyDepth) || !ValidContacts(source) ||
            !ValidRequiredSupport(source) || !ValidSites(source))
            return fail(FractureDocumentErrors::InvalidSource);
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
