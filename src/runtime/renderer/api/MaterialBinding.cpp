#include "Horo/Runtime/Render/MaterialBinding.h"

#include "Horo/Foundation/Assertions.h"
#include "Horo/Runtime/Render/MaterialBindingErrors.h"

#include <algorithm>
#include <limits>
#include <thread>
#include <tuple>

namespace Horo::Render {
    namespace {
        /** @brief Owner-thread accounting shared with consumer-retained immutable generations. */
        struct BindingBudget {
            std::thread::id thread{std::this_thread::get_id()};
            std::size_t generations{0};
            std::size_t parameterBytes{0};
        };

        /** @brief Requires exact target packing and logical layout for an authored fallback. */
        [[nodiscard]] bool Compatible(const MaterialBindingLayout &left, const MaterialBindingLayout &right) {
            const bool resources = std::ranges::equal(left.resources, right.resources, [](const auto &a, const auto &b) {
                return std::tie(a.id, a.kind, a.access, a.arrayCount, a.stages, a.active) ==
                       std::tie(b.id, b.kind, b.access, b.arrayCount, b.stages, b.active);
            });
            const bool targets = std::ranges::equal(left.targetBindings, right.targetBindings, [](const auto &a, const auto &b) {
                return std::tie(a.id, a.generatedHelperIndex, a.nativeSpace, a.nativeBinding, a.nativeName, a.pairedSampler, a.active) ==
                       std::tie(b.id, b.generatedHelperIndex, b.nativeSpace, b.nativeBinding, b.nativeName, b.pairedSampler, b.active);
            });
            const bool parameters = std::ranges::equal(left.parameters, right.parameters, [](const auto &a, const auto &b) {
                return std::tie(a.id, a.binding, a.type, a.rows, a.columns, a.arrayCount, a.byteOffset, a.arrayStride, a.matrixStride,
                                a.columnMajor, a.active) == std::tie(b.id, b.binding, b.type, b.rows, b.columns, b.arrayCount, b.byteOffset,
                                                                     b.arrayStride, b.matrixStride, b.columnMajor, b.active);
            });
            return left.backend == right.backend && left.shaderInterface == right.shaderInterface && resources && targets && parameters;
        }

        /** @brief Permits fallback only for the selected adapter's explicit unsupported result. */
        [[nodiscard]] bool IsUnsupported(const Error &error) {
            const auto expected = MakeError(MaterialBindingErrors::Unsupported);
            return error.domain.Value() == expected.domain.Value() && error.code.Value() == expected.code.Value();
        }

        /** @brief Preserves adapter failures unless the authored fallback permits Unsupported. */
        [[nodiscard]] Result<MaterialBindingSelection> HandleFailure(const Error &error,
                                                                     const std::shared_ptr<const ResidentMaterialBinding> &fallback) {
            if (fallback && IsUnsupported(error))
                return Result<MaterialBindingSelection>::Success({fallback->Id(), true, error});
            return Result<MaterialBindingSelection>::Failure(error);
        }
    }  // namespace

    struct ResidentMaterialBinding::Storage {
        MaterialBindingGenerationId id;
        MaterialBindingDescriptor descriptor;
        std::unique_ptr<IResidentMaterialBinding> backend;
        std::shared_ptr<BindingBudget> budget;
        std::size_t parameterBytes{0};
        bool charged{false};

        ~Storage() {
            HORO_INVARIANT(std::this_thread::get_id() == budget->thread);
            backend.reset();
            if (charged) {
                --budget->generations;
                budget->parameterBytes -= parameterBytes;
            }
        }
    };

    class MaterialBindingTable::Impl {
    public:
        Impl(const RenderResourceOwnerId id, IMaterialBindingBackend &port, const MaterialBindingLimits &bounds)
            : owner(id), backend(&port), limits(bounds) {
            entries.resize(limits.maximumGenerations);
        }

        /** @brief Rejects owner affinity before reading table state. */
        [[nodiscard]] Result<void> CheckThread() const {
            if (std::this_thread::get_id() != budget->thread)
                return Result<void>::Failure(MakeError(MaterialBindingErrors::WrongThread));
            return Result<void>::Success();
        }

        /** @brief Finds only a currently discoverable exact generation. */
        [[nodiscard]] std::shared_ptr<const ResidentMaterialBinding> Find(const MaterialBindingGenerationId id) const {
            if (id.owner != owner || !id.IsValid())
                return {};
            for (const auto &entry : entries)
                if (entry && entry->Id() == id)
                    return entry;
            return {};
        }

        /** @brief Validates explicit fallback identity and cooked compatibility before native work. */
        [[nodiscard]] Result<std::shared_ptr<const ResidentMaterialBinding>> Fallback(const MaterialBindingDescriptor &descriptor) const {
            if (!descriptor.authoredFallback)
                return Result<std::shared_ptr<const ResidentMaterialBinding>>::Success({});
            auto entry = Find(*descriptor.authoredFallback);
            if (!entry)
                return Result<std::shared_ptr<const ResidentMaterialBinding>>::Failure(MakeError(MaterialBindingErrors::StaleBinding));
            if (!Compatible(descriptor.layout, entry->Descriptor().layout))
                return Result<std::shared_ptr<const ResidentMaterialBinding>>::Failure(MakeError(MaterialBindingErrors::InvalidDescriptor));
            return Result<std::shared_ptr<const ResidentMaterialBinding>>::Success(std::move(entry));
        }

        /** @brief Checks shared current/consumer-retained budgets before adapter work. */
        [[nodiscard]] bool HasCapacity(const std::size_t bytes) const noexcept {
            return budget->generations < limits.maximumGenerations &&
                   bytes <= limits.maximumRetainedParameterBytes - budget->parameterBytes &&
                   next != std::numeric_limits<std::uint64_t>::max();
        }

        RenderResourceOwnerId owner;
        IMaterialBindingBackend *backend;
        MaterialBindingLimits limits;
        std::shared_ptr<BindingBudget> budget{std::make_shared<BindingBudget>()};
        std::vector<std::shared_ptr<const ResidentMaterialBinding>> entries;
        std::uint64_t next{1};
        bool accepting{true};
    };

    /** @copydoc IMaterialBindingBackend::Realize */
    Result<std::unique_ptr<IResidentMaterialBinding>> IMaterialBindingBackend::Realize(const MaterialBindingDescriptor &) {
        return Result<std::unique_ptr<IResidentMaterialBinding>>::Failure(MakeError(MaterialBindingErrors::Unsupported));
    }

    /** @copydoc ResidentMaterialBinding::Id */
    MaterialBindingGenerationId ResidentMaterialBinding::Id() const noexcept {
        return storage_->id;
    }

    /** @copydoc ResidentMaterialBinding::Descriptor */
    const MaterialBindingDescriptor &ResidentMaterialBinding::Descriptor() const noexcept {
        return storage_->descriptor;
    }

    /** @copydoc ResidentMaterialBinding::BackendBinding */
    const IResidentMaterialBinding &ResidentMaterialBinding::BackendBinding() const noexcept {
        return *storage_->backend;
    }

    /** @copydoc ResidentMaterialBinding::ResidentMaterialBinding */
    ResidentMaterialBinding::ResidentMaterialBinding(std::unique_ptr<Storage> storage, ConstructionKey) noexcept
        : storage_(std::move(storage)) {}

    ResidentMaterialBinding::~ResidentMaterialBinding() = default;

    /** @copydoc MaterialBindingTable::MaterialBindingTable */
    MaterialBindingTable::MaterialBindingTable(std::unique_ptr<Impl> implementation, ConstructionKey) noexcept
        : implementation_(std::move(implementation)) {}

    MaterialBindingTable::~MaterialBindingTable() {
        HORO_INVARIANT(std::this_thread::get_id() == implementation_->budget->thread);
    }

    /** @copydoc MaterialBindingTable::Create */
    Result<std::unique_ptr<MaterialBindingTable>> MaterialBindingTable::Create(const RenderResourceOwnerId owner,
                                                                               IMaterialBindingBackend &backend,
                                                                               const MaterialBindingLimits &limits) {
        if (!owner.IsValid() || !limits.IsValid())
            return Result<std::unique_ptr<MaterialBindingTable>>::Failure(MakeError(MaterialBindingErrors::InvalidDescriptor));
        try {
            return Result<std::unique_ptr<MaterialBindingTable>>::Success(
                std::make_unique<MaterialBindingTable>(std::make_unique<Impl>(owner, backend, limits), ConstructionKey{}));
        } catch (...) {  // NOSONAR(cpp:S2738)
            return Result<std::unique_ptr<MaterialBindingTable>>::Failure(MakeError(MaterialBindingErrors::AllocationFailed));
        }
    }

    /** @copydoc MaterialBindingTable::Publish */
    Result<MaterialBindingSelection> MaterialBindingTable::Publish(MaterialBindingDescriptor descriptor) {
        if (auto thread = implementation_->CheckThread(); thread.HasError())
            return Result<MaterialBindingSelection>::Failure(thread.ErrorValue());
        if (!implementation_->accepting)
            return Result<MaterialBindingSelection>::Failure(MakeError(MaterialBindingErrors::Closed));
        const auto validated = ValidateMaterialBindingDescriptor(descriptor, implementation_->owner, implementation_->limits);
        if (validated.HasError())
            return Result<MaterialBindingSelection>::Failure(validated.ErrorValue());
        const auto fallback = implementation_->Fallback(descriptor);
        if (fallback.HasError())
            return Result<MaterialBindingSelection>::Failure(fallback.ErrorValue());
        if (!implementation_->HasCapacity(validated.Value()))
            return Result<MaterialBindingSelection>::Failure(MakeError(MaterialBindingErrors::CapacityExceeded));
        try {
            auto native = implementation_->backend->Realize(descriptor);
            if (native.HasError())
                return HandleFailure(native.ErrorValue(), fallback.Value());
            if (!native.Value())
                return Result<MaterialBindingSelection>::Failure(MakeError(MaterialBindingErrors::BackendFailure));
            return Commit(std::move(descriptor), std::move(native).Value(), validated.Value());
        } catch (...) {  // NOSONAR(cpp:S2738)
            return Result<MaterialBindingSelection>::Failure(MakeError(MaterialBindingErrors::BackendFailure));
        }
    }

    /** @copydoc MaterialBindingTable::Commit */
    Result<MaterialBindingSelection> MaterialBindingTable::Commit(MaterialBindingDescriptor descriptor,
                                                                  std::unique_ptr<IResidentMaterialBinding> native,
                                                                  const std::size_t parameterBytes) {
        try {
            auto storage = std::make_unique<ResidentMaterialBinding::Storage>();
            storage->budget = implementation_->budget;
            storage->id = {implementation_->owner, implementation_->next};
            storage->descriptor = std::move(descriptor);
            storage->backend = std::move(native);
            storage->parameterBytes = parameterBytes;
            auto resident = std::make_shared<const ResidentMaterialBinding>(std::move(storage), ResidentMaterialBinding::ConstructionKey{});
            auto empty = std::ranges::find(implementation_->entries, std::shared_ptr<const ResidentMaterialBinding>{});
            HORO_INVARIANT(empty != implementation_->entries.end());
            resident->storage_->charged = true;
            ++implementation_->budget->generations;
            implementation_->budget->parameterBytes += parameterBytes;
            *empty = resident;
            ++implementation_->next;
            return Result<MaterialBindingSelection>::Success({resident->Id(), false, {}});
        } catch (...) {  // NOSONAR(cpp:S2738)
            return Result<MaterialBindingSelection>::Failure(MakeError(MaterialBindingErrors::AllocationFailed));
        }
    }

    /** @copydoc MaterialBindingTable::Acquire */
    Result<std::shared_ptr<const ResidentMaterialBinding>> MaterialBindingTable::Acquire(const MaterialBindingGenerationId binding) const {
        if (auto thread = implementation_->CheckThread(); thread.HasError())
            return Result<std::shared_ptr<const ResidentMaterialBinding>>::Failure(thread.ErrorValue());
        if (!implementation_->accepting)
            return Result<std::shared_ptr<const ResidentMaterialBinding>>::Failure(MakeError(MaterialBindingErrors::Closed));
        auto entry = implementation_->Find(binding);
        if (!entry)
            return Result<std::shared_ptr<const ResidentMaterialBinding>>::Failure(MakeError(MaterialBindingErrors::StaleBinding));
        return Result<std::shared_ptr<const ResidentMaterialBinding>>::Success(std::move(entry));
    }

    /** @copydoc MaterialBindingTable::Release */
    Result<void> MaterialBindingTable::Release(const MaterialBindingGenerationId binding) {
        if (auto thread = implementation_->CheckThread(); thread.HasError())
            return thread;
        if (!implementation_->Find(binding))
            return Result<void>::Failure(MakeError(MaterialBindingErrors::StaleBinding));
        for (auto &entry : implementation_->entries)
            if (entry && entry->Id() == binding) {
                entry.reset();
                break;
            }
        return Result<void>::Success();
    }

    /** @copydoc MaterialBindingTable::Snapshot */
    MaterialBindingSnapshot MaterialBindingTable::Snapshot() const noexcept {
        HORO_INVARIANT(std::this_thread::get_id() == implementation_->budget->thread);
        return {implementation_->budget->generations, implementation_->budget->parameterBytes, implementation_->accepting};
    }

    /** @copydoc MaterialBindingTable::Shutdown */
    Result<void> MaterialBindingTable::Shutdown() {
        if (auto thread = implementation_->CheckThread(); thread.HasError())
            return thread;
        implementation_->accepting = false;
        for (auto &entry : implementation_->entries)
            entry.reset();
        return Result<void>::Success();
    }
}  // namespace Horo::Render
