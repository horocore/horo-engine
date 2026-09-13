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
                auto hash = static_cast<std::size_t>(key.kind);
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
                                                           const std::span<const std::uint8_t> payload) {
            if (payload.empty())
                return Result<void>::Failure(CacheError(PhysicsErrors::ShapeArtifactInvalid,
                                                        "Cooked shape cache acquisition requires a non-empty artifact payload."));
            if (const Sha256Digest actual = ComputeSha256(std::as_bytes(payload)); actual != *descriptor.payloadDigest)
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
            auto resource = std::make_shared<const Detail::PhysicsCookedShapeResource>(descriptor, std::move(shape), bytes);
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
            using enum PhysicsCookedShapeKind;
            switch (descriptor.kind) {
                case ConvexHull:
                    return ConstructConvexResource(descriptor, target, payload);
                case TriangleMesh:
                    return ConstructTriangleResource(descriptor, target, payload);
                case HeightField:
                case Compound:
                    return Result<std::shared_ptr<const Detail::PhysicsCookedShapeResource>>::Failure(
                        CacheError(PhysicsErrors::OperationUnsupported,
                                   "The cooked shape cache supports only qualified ConvexHull and TriangleMesh artifact contracts."));
            }
            return Result<std::shared_ptr<const Detail::PhysicsCookedShapeResource>>::Failure(
                CacheError(PhysicsErrors::OperationUnsupported, "The cooked shape cache received an unknown shape kind."));
        }
    }  // namespace

    namespace Detail {
        struct PhysicsCookedShapeCacheState final {
            struct Entry final {
                CacheKey key;
                std::shared_ptr<const PhysicsCookedShapeResource> resource;
                std::uint64_t leaseCount{};
            };

        private:
            PhysicsShapeCookTargetDigest target;
            PhysicsCookedShapeCacheLimits limits;
            std::list<Entry> activeEntries;
            std::list<Entry> evictableEntries;
            std::unordered_map<CacheKey, std::list<Entry>::iterator, CacheKeyHash> byKey;
            std::uint64_t residentBytes{};
            bool admissionClosed{};

        public:
            [[nodiscard]] std::shared_ptr<const PhysicsCookedShapeResource> FindAndPin(const CacheKey &key) {
                const auto found = byKey.find(key);
                if (found == byKey.end())
                    return {};
                auto entry = found->second;
                if (entry->leaseCount == 0)
                    activeEntries.splice(activeEntries.end(), evictableEntries, entry);
                ++entry->leaseCount;
                return entry->resource;
            }

            void Release(const std::shared_ptr<const PhysicsCookedShapeResource> &resource) noexcept {
                const CacheKey key = MakeCacheKey(resource->descriptor);
                std::scoped_lock lock{mutex};
                const auto found = byKey.find(key);
                if (found == byKey.end() || found->second->resource != resource || found->second->leaseCount == 0)
                    return;
                auto entry = found->second;
                --entry->leaseCount;
                if (entry->leaseCount == 0)
                    evictableEntries.splice(evictableEntries.end(), activeEntries, entry);
            }

            [[nodiscard]] Result<void> MakeRoom(const std::uint64_t incomingBytes, std::list<Entry> &retired) {
                while (byKey.size() >= limits.maximumShapes || incomingBytes > limits.maximumResidentBytes - residentBytes) {
                    if (evictableEntries.empty())
                        return Result<void>::Failure(
                            CacheError(PhysicsErrors::CapacityExceeded,
                                       "Active cooked shape leases prevent eviction needed by the configured cache budget."));
                    auto victim = evictableEntries.begin();
                    residentBytes -= victim->resource->residentBytes;
                    byKey.erase(victim->key);
                    retired.splice(retired.end(), evictableEntries, victim);
                }
                return Result<void>::Success();
            }

            void InsertPinned(CacheKey key, std::shared_ptr<const PhysicsCookedShapeResource> resource) {
                residentBytes += resource->residentBytes;
                activeEntries.emplace_back(std::move(key), std::move(resource), 1);
                byKey.try_emplace(activeEntries.back().key, std::prev(activeEntries.end()));
            }

        private:
            friend class Horo::Physics::PhysicsCookedShapeCache;
            mutable std::mutex mutex;
        };
    }  // namespace Detail

    struct PhysicsCookedShapeCache::Impl final {
        std::shared_ptr<Detail::PhysicsCookedShapeCacheState> state;
    };

    PhysicsCookedShapeLease::PhysicsCookedShapeLease(std::shared_ptr<const Detail::PhysicsCookedShapeResource> resource,
                                                     std::shared_ptr<Detail::PhysicsCookedShapeCacheState> state) noexcept
        : resource_(std::move(resource)), state_(std::move(state)) {}

    PhysicsCookedShapeLease::PhysicsCookedShapeLease(PhysicsCookedShapeLease &&other) noexcept
        : resource_(std::move(other.resource_)), state_(std::move(other.state_)) {}

    PhysicsCookedShapeLease &PhysicsCookedShapeLease::operator=(PhysicsCookedShapeLease &&other) noexcept {
        if (this != &other) {
            Release();
            resource_ = std::move(other.resource_);
            state_ = std::move(other.state_);
        }
        return *this;
    }

    PhysicsCookedShapeLease::~PhysicsCookedShapeLease() {
        Release();
    }

    void PhysicsCookedShapeLease::Release() noexcept {
        if (resource_ != nullptr && state_ != nullptr)
            state_->Release(resource_);
        state_.reset();
        resource_.reset();
    }

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

    PhysicsCookedShapeCache &PhysicsCookedShapeCache::operator=(PhysicsCookedShapeCache &&other) noexcept {
        if (this != &other) {
            if (impl_ != nullptr)
                (void)Shutdown();
            impl_ = std::move(other.impl_);
        }
        return *this;
    }

    PhysicsCookedShapeCache::~PhysicsCookedShapeCache() {
        if (impl_ != nullptr)
            (void)Shutdown();
    }

    Result<PhysicsCookedShapeCache> PhysicsCookedShapeCache::Create(const PhysicsShapeCookTargetDigest &target,
                                                                    const PhysicsCookedShapeCacheLimits &limits) {
        if (Result<void> validation = ValidateLimits(limits); validation.HasError())
            return Result<PhysicsCookedShapeCache>::Failure(validation.ErrorValue());
        auto impl = std::make_unique<Impl>();
        impl->state = std::make_shared<Detail::PhysicsCookedShapeCacheState>();
        impl->state->target = target;
        impl->state->limits = limits;
        return Result<PhysicsCookedShapeCache>::Success(PhysicsCookedShapeCache{std::move(impl)});
    }

    Result<PhysicsCookedShapeLease> PhysicsCookedShapeCache::Acquire(const PhysicsCookedShapeDescriptor &descriptor,
                                                                     const std::span<const std::uint8_t> payload) const {
        const auto state = impl_->state;
        if (Result<void> descriptorValidation = ValidatePhysicsCookedShapeDescriptor(descriptor, state->target);
            descriptorValidation.HasError())
            return Result<PhysicsCookedShapeLease>::Failure(descriptorValidation.ErrorValue());
        const CacheKey key = MakeCacheKey(descriptor);
        {
            std::scoped_lock lock{state->mutex};
            if (state->admissionClosed)
                return Result<PhysicsCookedShapeLease>::Failure(
                    CacheError(PhysicsErrors::InvalidState,
                               "Cooked shape cache admission is closed after shutdown; create a new runtime cache."));
            if (auto resident = state->FindAndPin(key))
                return Result<PhysicsCookedShapeLease>::Success(PhysicsCookedShapeLease{std::move(resident), state});
        }

        if (Result<void> payloadValidation = ValidatePayloadIdentity(descriptor, payload); payloadValidation.HasError())
            return Result<PhysicsCookedShapeLease>::Failure(payloadValidation.ErrorValue());
        Result<std::shared_ptr<const Detail::PhysicsCookedShapeResource>> constructed =
            ConstructResource(descriptor, state->target, payload);
        if (constructed.HasError())
            return Result<PhysicsCookedShapeLease>::Failure(constructed.ErrorValue());
        std::shared_ptr<const Detail::PhysicsCookedShapeResource> resource = std::move(constructed).Value();
        if (resource->residentBytes > state->limits.maximumResidentBytes)
            return Result<PhysicsCookedShapeLease>::Failure(
                CacheError(PhysicsErrors::CapacityExceeded,
                           "Cooked shape resource cannot fit within the configured resident-byte budget."));

        std::list<Detail::PhysicsCookedShapeCacheState::Entry> retired;
        {
            std::unique_lock lock{state->mutex};
            if (state->admissionClosed)
                return Result<PhysicsCookedShapeLease>::Failure(
                    CacheError(PhysicsErrors::InvalidState,
                               "Cooked shape cache admission closed while the resource was being constructed."));
            if (auto resident = state->FindAndPin(key))
                return Result<PhysicsCookedShapeLease>::Success(PhysicsCookedShapeLease{std::move(resident), state});
            if (Result<void> room = state->MakeRoom(resource->residentBytes, retired); room.HasError())
                return Result<PhysicsCookedShapeLease>::Failure(room.ErrorValue());
            state->InsertPinned(key, resource);
        }
        retired.clear();
        return Result<PhysicsCookedShapeLease>::Success(PhysicsCookedShapeLease{std::move(resource), state});
    }

    Result<bool> PhysicsCookedShapeCache::Evict(const PhysicsCookedShapeDescriptor &descriptor) const {
        const auto state = impl_->state;
        if (Result<void> validation = ValidatePhysicsCookedShapeDescriptor(descriptor, state->target); validation.HasError())
            return Result<bool>::Failure(validation.ErrorValue());
        const CacheKey key = MakeCacheKey(descriptor);
        std::shared_ptr<const Detail::PhysicsCookedShapeResource> retired;
        {
            std::unique_lock lock{state->mutex};
            if (state->admissionClosed)
                return Result<bool>::Failure(
                    CacheError(PhysicsErrors::InvalidState, "Cooked shape cache eviction is unavailable after shutdown."));
            const auto found = state->byKey.find(key);
            if (found == state->byKey.end())
                return Result<bool>::Success(false);
            const auto entry = found->second;
            state->residentBytes -= entry->resource->residentBytes;
            retired = std::move(entry->resource);
            state->byKey.erase(found);
            if (entry->leaseCount == 0)
                state->evictableEntries.erase(entry);
            else
                state->activeEntries.erase(entry);
        }
        retired.reset();
        return Result<bool>::Success(true);
    }

    Result<void> PhysicsCookedShapeCache::Shutdown() const noexcept {
        const auto state = impl_->state;
        std::list<Detail::PhysicsCookedShapeCacheState::Entry> retired;
        {
            std::scoped_lock lock{state->mutex};
            if (state->admissionClosed)
                return Result<void>::Success();
            state->admissionClosed = true;
            retired.splice(retired.end(), state->activeEntries);
            retired.splice(retired.end(), state->evictableEntries);
            state->byKey.clear();
            state->residentBytes = 0;
        }
        retired.clear();
        return Result<void>::Success();
    }

    PhysicsCookedShapeCacheStats PhysicsCookedShapeCache::Stats() const noexcept {
        const auto state = impl_->state;
        std::scoped_lock lock{state->mutex};
        return {static_cast<std::uint32_t>(state->byKey.size()), state->residentBytes, state->admissionClosed};
    }
}  // namespace Horo::Physics
