#include "Horo/Physics/PhysicsCookedShapeCache.h"

#include "Horo/Physics/PhysicsConvexHullCook.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsTriangleMeshCook.h"

#include <cassert>
#include <limits>
#include <list>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace Horo::Physics {
    namespace Detail {
        using PhysicsCookedShapeData = std::variant<LoadedPhysicsConvexHull, LoadedPhysicsTriangleMesh>;

        struct PhysicsCookedShapeResource final {
            PhysicsCookedShapeDescriptor descriptor;
            PhysicsCookedShapeData data;
            std::uint64_t residentBytes{};
        };
    }  // namespace Detail

    namespace {
        struct CacheKey final {
            Sha256Digest cacheKey;
            Sha256Digest payload;
            PhysicsShapeCookTargetDigest target;
            PhysicsCookedShapeKind kind{};

            [[nodiscard]] bool operator==(const CacheKey &) const noexcept = default;
        };

        void HashCombine(std::size_t &seed, const std::size_t value) noexcept {
            seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U);
        }

        struct CacheKeyHash final {
            [[nodiscard]] std::size_t operator()(const CacheKey &key) const noexcept {
                std::size_t hash = static_cast<std::size_t>(key.kind);
                for (const std::uint8_t byte : key.cacheKey.bytes)
                    HashCombine(hash, byte);
                for (const std::uint8_t byte : key.payload.bytes)
                    HashCombine(hash, byte);
                for (const std::uint8_t byte : key.target.digest.bytes)
                    HashCombine(hash, byte);
                return hash;
            }
        };

        [[nodiscard]] CacheKey MakeCacheKey(const PhysicsCookedShapeDescriptor &descriptor) noexcept {
            assert(descriptor.cacheKeyDigest.has_value());
            assert(descriptor.payloadDigest.has_value());
            assert(descriptor.target.has_value());
            return {*descriptor.cacheKeyDigest, *descriptor.payloadDigest, *descriptor.target, descriptor.kind};
        }

        [[nodiscard]] Error CacheError(const ErrorCodeDescriptor &descriptor, std::string message) {
            return MakeError(descriptor, std::move(message));
        }

        [[nodiscard]] Result<void> ValidateLimits(const PhysicsCookedShapeCacheLimits &limits) {
            if (limits.maximumShapes == 0 || limits.maximumShapes > MaximumPhysicsResourceRecords || limits.maximumResidentBytes == 0 ||
                limits.maximumResidentBytes > MaximumPhysicsResidentShapeBytes) {
                return Result<void>::Failure(CacheError(PhysicsErrors::CapacityExceeded,
                                                        "Cooked shape cache limits must be positive and within the qualified "
                                                        "shape-count and resident-byte maxima."));
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidatePayloadIdentity(const PhysicsCookedShapeDescriptor &descriptor,
                                                           const PhysicsShapeCookTargetDigest &target,
                                                           const std::span<const std::uint8_t> payload) {
            Result<void> descriptorValidation = ValidatePhysicsCookedShapeDescriptor(descriptor, target);
            if (descriptorValidation.HasError())
                return descriptorValidation;
            if (payload.empty())
                return Result<void>::Failure(CacheError(PhysicsErrors::ShapeArtifactInvalid,
                                                        "Cooked shape cache acquisition requires a non-empty artifact payload."));
            const Sha256Digest actual = ComputeSha256(std::as_bytes(payload));
            if (actual != *descriptor.payloadDigest)
                return Result<void>::Failure(
                    CacheError(PhysicsErrors::ShapeArtifactInvalid,
                               "Cooked shape cache payload does not match the exact payload digest; the resource was not published."));
            return Result<void>::Success();
        }

        [[nodiscard]] bool AddBytes(std::uint64_t &total, const std::size_t count, const std::size_t elementBytes) noexcept {
            if (count != 0 && elementBytes > std::numeric_limits<std::uint64_t>::max() / count)
                return false;
            const auto bytes = static_cast<std::uint64_t>(count) * static_cast<std::uint64_t>(elementBytes);
            if (bytes > std::numeric_limits<std::uint64_t>::max() - total)
                return false;
            total += bytes;
            return true;
        }

        template <typename Shape>
        [[nodiscard]] Result<std::shared_ptr<const Detail::PhysicsCookedShapeResource>> MakeResource(
            const PhysicsCookedShapeDescriptor &descriptor, Shape shape, const std::uint64_t bytes) {
            auto resource = std::make_shared<const Detail::PhysicsCookedShapeResource>(
                Detail::PhysicsCookedShapeResource{descriptor, std::move(shape), bytes});
            return Result<std::shared_ptr<const Detail::PhysicsCookedShapeResource>>::Success(std::move(resource));
        }

        template <typename Shape, typename AccountBytes>
        [[nodiscard]] Result<std::shared_ptr<const Detail::PhysicsCookedShapeResource>> FinishConstruction(
            const PhysicsCookedShapeDescriptor &descriptor, Result<Shape> loaded, AccountBytes accountBytes) {
            if (loaded.HasError())
                return Result<std::shared_ptr<const Detail::PhysicsCookedShapeResource>>::Failure(loaded.ErrorValue());
            Shape shape = std::move(loaded).Value();
            std::uint64_t bytes = sizeof(Detail::PhysicsCookedShapeResource);
            if (!accountBytes(bytes, shape))
                return Result<std::shared_ptr<const Detail::PhysicsCookedShapeResource>>::Failure(
                    CacheError(PhysicsErrors::CapacityExceeded, "Cooked shape resident-byte accounting overflowed."));
            return MakeResource(descriptor, std::move(shape), bytes);
        }

        [[nodiscard]] Result<std::shared_ptr<const Detail::PhysicsCookedShapeResource>> ConstructConvexResource(
            const PhysicsCookedShapeDescriptor &descriptor, const PhysicsShapeCookTargetDigest &target,
            const std::span<const std::uint8_t> payload) {
            return FinishConstruction(descriptor, LoadCookedPhysicsConvexHull(descriptor, target, payload),
                                      [](std::uint64_t &bytes, const LoadedPhysicsConvexHull &shape) {
                return AddBytes(bytes, shape.vertices.size(), sizeof(Math::Vec3)) &&
                       AddBytes(bytes, shape.triangleIndices.size(), sizeof(std::uint32_t));
            });
        }

        [[nodiscard]] Result<std::shared_ptr<const Detail::PhysicsCookedShapeResource>> ConstructTriangleResource(
            const PhysicsCookedShapeDescriptor &descriptor, const PhysicsShapeCookTargetDigest &target,
            const std::span<const std::uint8_t> payload) {
            return FinishConstruction(descriptor, LoadCookedPhysicsTriangleMesh(descriptor, target, payload),
                                      [](std::uint64_t &bytes, const LoadedPhysicsTriangleMesh &shape) {
                return AddBytes(bytes, shape.vertices.size(), sizeof(Math::Vec3)) &&
                       AddBytes(bytes, shape.materialSlots.size(), sizeof(PhysicsMaterialSlotId)) &&
                       AddBytes(bytes, shape.triangles.size(), sizeof(LoadedPhysicsTriangle));
            });
        }

        [[nodiscard]] Result<std::shared_ptr<const Detail::PhysicsCookedShapeResource>> ConstructResource(
            const PhysicsCookedShapeDescriptor &descriptor, const PhysicsShapeCookTargetDigest &target,
            const std::span<const std::uint8_t> payload) {
            switch (descriptor.kind) {
                case PhysicsCookedShapeKind::ConvexHull:
                    return ConstructConvexResource(descriptor, target, payload);
                case PhysicsCookedShapeKind::TriangleMesh:
                    return ConstructTriangleResource(descriptor, target, payload);
                case PhysicsCookedShapeKind::HeightField:
                case PhysicsCookedShapeKind::Compound:
                    return Result<std::shared_ptr<const Detail::PhysicsCookedShapeResource>>::Failure(
                        CacheError(PhysicsErrors::OperationUnsupported,
                                   "The cooked shape cache supports only qualified ConvexHull and TriangleMesh artifact contracts."));
            }
        }
    }  // namespace

    struct PhysicsCookedShapeCache::Impl final {
        struct Entry final {
            CacheKey key;
            std::shared_ptr<const Detail::PhysicsCookedShapeResource> resource;
            std::uint64_t lastUse{};
        };

        PhysicsShapeCookTargetDigest target;
        PhysicsCookedShapeCacheLimits limits;
        std::list<Entry> entries;
        std::unordered_map<CacheKey, std::list<Entry>::iterator, CacheKeyHash> byKey;
        std::uint64_t residentBytes{};
        std::uint64_t useClock{};
        bool admissionClosed{};
        mutable std::mutex mutex;

        [[nodiscard]] std::shared_ptr<const Detail::PhysicsCookedShapeResource> FindAndTouch(const CacheKey &key) {
            const auto found = byKey.find(key);
            if (found == byKey.end())
                return {};
            found->second->lastUse = ++useClock;
            return found->second->resource;
        }

        [[nodiscard]] std::list<Entry>::iterator FindEvictionVictim() noexcept {
            auto victim = entries.end();
            for (auto candidate = entries.begin(); candidate != entries.end(); ++candidate) {
                if (candidate->resource.use_count() == 1 && (victim == entries.end() || candidate->lastUse < victim->lastUse))
                    victim = candidate;
            }
            return victim;
        }

        [[nodiscard]] Result<void> MakeRoom(const std::uint64_t incomingBytes, std::list<Entry> &retired) {
            while (entries.size() >= limits.maximumShapes || incomingBytes > limits.maximumResidentBytes - residentBytes) {
                const auto victim = FindEvictionVictim();
                if (victim == entries.end())
                    return Result<void>::Failure(
                        CacheError(PhysicsErrors::CapacityExceeded,
                                   "Active cooked shape leases prevent eviction needed by the configured cache budget."));
                residentBytes -= victim->resource->residentBytes;
                byKey.erase(victim->key);
                retired.splice(retired.end(), entries, victim);
            }
            return Result<void>::Success();
        }

        void Insert(CacheKey key, std::shared_ptr<const Detail::PhysicsCookedShapeResource> resource) {
            residentBytes += resource->residentBytes;
            entries.push_back({std::move(key), std::move(resource), ++useClock});
            byKey.emplace(entries.back().key, std::prev(entries.end()));
        }
    };

    PhysicsCookedShapeLease::PhysicsCookedShapeLease(std::shared_ptr<const Detail::PhysicsCookedShapeResource> resource) noexcept
        : resource_(std::move(resource)) {}

    PhysicsCookedShapeLease::operator bool() const noexcept {
        return resource_ != nullptr;
    }

    const PhysicsCookedShapeDescriptor &PhysicsCookedShapeLease::Descriptor() const noexcept {
        assert(resource_ != nullptr);
        return resource_->descriptor;
    }

    std::uint64_t PhysicsCookedShapeLease::ResidentBytes() const noexcept {
        assert(resource_ != nullptr);
        return resource_->residentBytes;
    }

    const LoadedPhysicsConvexHull *PhysicsCookedShapeLease::ConvexHull() const noexcept {
        if (resource_ == nullptr)
            return nullptr;
        return std::get_if<LoadedPhysicsConvexHull>(&resource_->data);
    }

    const LoadedPhysicsTriangleMesh *PhysicsCookedShapeLease::TriangleMesh() const noexcept {
        if (resource_ == nullptr)
            return nullptr;
        return std::get_if<LoadedPhysicsTriangleMesh>(&resource_->data);
    }

    bool PhysicsCookedShapeLease::SharesResourceWith(const PhysicsCookedShapeLease &other) const noexcept {
        return resource_ != nullptr && resource_ == other.resource_;
    }

    PhysicsCookedShapeCache::PhysicsCookedShapeCache(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    PhysicsCookedShapeCache::PhysicsCookedShapeCache(PhysicsCookedShapeCache &&) noexcept = default;
    PhysicsCookedShapeCache &PhysicsCookedShapeCache::operator=(PhysicsCookedShapeCache &&) noexcept = default;
    PhysicsCookedShapeCache::~PhysicsCookedShapeCache() = default;

    Result<PhysicsCookedShapeCache> PhysicsCookedShapeCache::Create(const PhysicsShapeCookTargetDigest target,
                                                                    const PhysicsCookedShapeCacheLimits &limits) {
        Result<void> validation = ValidateLimits(limits);
        if (validation.HasError())
            return Result<PhysicsCookedShapeCache>::Failure(validation.ErrorValue());
        auto impl = std::make_unique<Impl>();
        impl->target = target;
        impl->limits = limits;
        return Result<PhysicsCookedShapeCache>::Success(PhysicsCookedShapeCache{std::move(impl)});
    }

    Result<PhysicsCookedShapeLease> PhysicsCookedShapeCache::Acquire(const PhysicsCookedShapeDescriptor &descriptor,
                                                                     const std::span<const std::uint8_t> payload) {
        Result<void> identity = ValidatePayloadIdentity(descriptor, impl_->target, payload);
        if (identity.HasError())
            return Result<PhysicsCookedShapeLease>::Failure(identity.ErrorValue());
        const CacheKey key = MakeCacheKey(descriptor);
        {
            std::scoped_lock lock{impl_->mutex};
            if (impl_->admissionClosed)
                return Result<PhysicsCookedShapeLease>::Failure(
                    CacheError(PhysicsErrors::InvalidState,
                               "Cooked shape cache admission is closed after shutdown; create a new runtime cache."));
            if (auto resident = impl_->FindAndTouch(key))
                return Result<PhysicsCookedShapeLease>::Success(PhysicsCookedShapeLease{std::move(resident)});
        }

        Result<std::shared_ptr<const Detail::PhysicsCookedShapeResource>> constructed =
            ConstructResource(descriptor, impl_->target, payload);
        if (constructed.HasError())
            return Result<PhysicsCookedShapeLease>::Failure(constructed.ErrorValue());
        std::shared_ptr<const Detail::PhysicsCookedShapeResource> resource = std::move(constructed).Value();
        if (resource->residentBytes > impl_->limits.maximumResidentBytes)
            return Result<PhysicsCookedShapeLease>::Failure(
                CacheError(PhysicsErrors::CapacityExceeded,
                           "Cooked shape resource cannot fit within the configured resident-byte budget."));

        std::list<Impl::Entry> retired;
        std::unique_lock lock{impl_->mutex};
        if (impl_->admissionClosed)
            return Result<PhysicsCookedShapeLease>::Failure(
                CacheError(PhysicsErrors::InvalidState, "Cooked shape cache admission closed while the resource was being constructed."));
        if (auto resident = impl_->FindAndTouch(key))
            return Result<PhysicsCookedShapeLease>::Success(PhysicsCookedShapeLease{std::move(resident)});
        Result<void> room = impl_->MakeRoom(resource->residentBytes, retired);
        if (room.HasError())
            return Result<PhysicsCookedShapeLease>::Failure(room.ErrorValue());
        impl_->Insert(key, resource);
        lock.unlock();
        retired.clear();
        return Result<PhysicsCookedShapeLease>::Success(PhysicsCookedShapeLease{std::move(resource)});
    }

    Result<bool> PhysicsCookedShapeCache::Evict(const PhysicsCookedShapeDescriptor &descriptor) {
        Result<void> validation = ValidatePhysicsCookedShapeDescriptor(descriptor, impl_->target);
        if (validation.HasError())
            return Result<bool>::Failure(validation.ErrorValue());
        const CacheKey key = MakeCacheKey(descriptor);
        std::shared_ptr<const Detail::PhysicsCookedShapeResource> retired;
        std::unique_lock lock{impl_->mutex};
        if (impl_->admissionClosed)
            return Result<bool>::Failure(
                CacheError(PhysicsErrors::InvalidState, "Cooked shape cache eviction is unavailable after shutdown."));
        const auto found = impl_->byKey.find(key);
        if (found == impl_->byKey.end())
            return Result<bool>::Success(false);
        const auto entry = found->second;
        impl_->residentBytes -= entry->resource->residentBytes;
        retired = std::move(entry->resource);
        impl_->byKey.erase(found);
        impl_->entries.erase(entry);
        lock.unlock();
        retired.reset();
        return Result<bool>::Success(true);
    }

    Result<void> PhysicsCookedShapeCache::Shutdown() noexcept {
        std::list<Impl::Entry> retired;
        {
            std::scoped_lock lock{impl_->mutex};
            if (impl_->admissionClosed)
                return Result<void>::Success();
            impl_->admissionClosed = true;
            retired.splice(retired.end(), impl_->entries);
            impl_->byKey.clear();
            impl_->residentBytes = 0;
        }
        retired.clear();
        return Result<void>::Success();
    }

    PhysicsCookedShapeCacheStats PhysicsCookedShapeCache::Stats() const noexcept {
        std::scoped_lock lock{impl_->mutex};
        return {static_cast<std::uint32_t>(impl_->entries.size()), impl_->residentBytes, impl_->admissionClosed};
    }
}  // namespace Horo::Physics
