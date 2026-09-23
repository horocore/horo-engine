#include "Horo/Terrain/TerrainFoliageRegistry.h"

#include <algorithm>
#include <limits>
#include <ranges>
#include <utility>

namespace Horo::Terrain {
    struct TerrainFoliageRegistrySnapshot::State final {
        TerrainFoliageRegistryBinding binding{};
        TerrainFoliageCapabilitySet capabilities{};
        std::vector<TerrainDatasetRegistration> datasets;
        std::vector<TerrainFoliageTypeRegistration> foliageTypes;
    };

    namespace {
        [[nodiscard]] constexpr bool IsKnown(const TerrainFoliageCapability capability) noexcept {
            return static_cast<std::uint8_t>(capability) < static_cast<std::uint8_t>(TerrainFoliageCapability::Count);
        }

        [[nodiscard]] constexpr bool IsKnown(const TerrainFeatureTier tier) noexcept {
            return static_cast<std::uint8_t>(tier) < static_cast<std::uint8_t>(TerrainFeatureTier::Count);
        }

        [[nodiscard]] bool IsValidRegistration(const TerrainDatasetRegistration &registration,
                                               const TerrainFoliageCapabilitySet available) {
            const auto &data = registration.descriptor.Data();
            return data.dataset.IsValid() && data.content.IsValid() && data.bounds.revision.IsValid() &&
                   registration.supportedTiers.IsValid() && registration.requiredCapabilities.IsValid() &&
                   available.ContainsAll(registration.requiredCapabilities);
        }

        [[nodiscard]] bool IsValidRegistration(const TerrainFoliageTypeRegistration &registration,
                                               const TerrainFoliageCapabilitySet available) {
            const auto &data = registration.definition.Data();
            return data.type.IsValid() && data.revision.IsValid() && registration.requiredCapabilities.IsValid() &&
                   available.ContainsAll(registration.requiredCapabilities);
        }

        [[nodiscard]] Result<void> ValidateDatasetRegistration(const TerrainDatasetRegistration &registration,
                                                               const TerrainFoliageCapabilitySet available,
                                                               const TerrainFoliageRegistryLimits &limits) {
            if (!IsValidRegistration(registration, available)) {
                const auto &data = registration.descriptor.Data();
                if (!registration.requiredCapabilities.IsValid())
                    return Result<void>::Failure(MakeError(TerrainErrors::RegistryDescriptorInvalid));
                if (!available.ContainsAll(registration.requiredCapabilities))
                    return Result<void>::Failure(MakeError(TerrainErrors::CapabilityUnsupported));
                if (!data.dataset.IsValid() || !data.content.IsValid() || !data.bounds.revision.IsValid() ||
                    !registration.supportedTiers.IsValid())
                    return Result<void>::Failure(MakeError(TerrainErrors::RegistryDescriptorInvalid));
                return Result<void>::Failure(MakeError(TerrainErrors::RegistryDescriptorInvalid));
            }
            if (limits.maximumDatasets == 0)
                return Result<void>::Failure(MakeError(TerrainErrors::CapacityExceeded));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateFoliageRegistration(const TerrainFoliageTypeRegistration &registration,
                                                               const TerrainFoliageCapabilitySet available,
                                                               const TerrainFoliageRegistryLimits &limits) {
            if (!IsValidRegistration(registration, available)) {
                const auto &data = registration.definition.Data();
                if (!registration.requiredCapabilities.IsValid())
                    return Result<void>::Failure(MakeError(TerrainErrors::RegistryDescriptorInvalid));
                if (!available.ContainsAll(registration.requiredCapabilities))
                    return Result<void>::Failure(MakeError(TerrainErrors::CapabilityUnsupported));
                if (!data.type.IsValid() || !data.revision.IsValid())
                    return Result<void>::Failure(MakeError(TerrainErrors::RegistryDescriptorInvalid));
                return Result<void>::Failure(MakeError(TerrainErrors::RegistryDescriptorInvalid));
            }
            if (limits.maximumFoliageTypes == 0)
                return Result<void>::Failure(MakeError(TerrainErrors::CapacityExceeded));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateQuery(const TerrainFoliageCapabilitySet required,
                                                 const std::optional<TerrainFeatureTier> tier = std::nullopt) {
            if (!required.IsValid())
                return Result<void>::Failure(MakeError(TerrainErrors::RegistryDescriptorInvalid));
            if (tier.has_value() && !IsKnown(*tier))
                return Result<void>::Failure(MakeError(TerrainErrors::TierInvalid));
            return Result<void>::Success();
        }

        template <typename Range> [[nodiscard]] auto LowerBoundDataset(Range &datasets, const TerrainDatasetId dataset) noexcept {
            return std::ranges::lower_bound(datasets, dataset, {}, [](const TerrainDatasetRegistration &registration) {
                return registration.descriptor.Data().dataset;
            });
        }

        template <typename Range> [[nodiscard]] auto LowerBoundFoliageType(Range &foliageTypes, const FoliageTypeId type) noexcept {
            return std::ranges::lower_bound(foliageTypes, type, {}, [](const TerrainFoliageTypeRegistration &registration) {
                return registration.definition.Data().type;
            });
        }

        [[nodiscard]] bool HasNewerDatasetRevision(const TerrainDatasetDescriptor &candidate,
                                                   const TerrainDatasetDescriptor &current) noexcept {
            const auto &candidateData = candidate.Data();
            const auto &currentData = current.Data();
            return candidateData.content > currentData.content || candidateData.bounds.revision > currentData.bounds.revision;
        }

        [[nodiscard]] Result<TerrainFoliageRegistryRevision> NextRevision(const TerrainFoliageRegistryRevision current) {
            if (!current.IsValid())
                return Result<TerrainFoliageRegistryRevision>::Failure(MakeError(TerrainErrors::RegistryDescriptorInvalid));
            if (current.Value() == std::numeric_limits<std::uint64_t>::max())
                return Result<TerrainFoliageRegistryRevision>::Failure(MakeError(TerrainErrors::RegistryGenerationExhausted));
            return TerrainFoliageRegistryRevision::Create(current.Value() + 1U);
        }

        [[nodiscard]] Result<TerrainFoliageRegistryRevision> ValidateMutation(const TerrainFoliageRegistryState lifecycle,
                                                                              const TerrainFoliageRegistrySnapshot::State *state) {
            if (lifecycle != TerrainFoliageRegistryState::Active)
                return Result<TerrainFoliageRegistryRevision>::Failure(MakeError(TerrainErrors::RegistryClosed));
            return state == nullptr ? Result<TerrainFoliageRegistryRevision>::Failure(MakeError(TerrainErrors::RegistryClosed))
                                    : NextRevision(state->binding.revision);
        }
    }  // namespace

    /** @copydoc TerrainFoliageCapabilitySet::Create */
    Result<TerrainFoliageCapabilitySet> TerrainFoliageCapabilitySet::Create(const std::span<const TerrainFoliageCapability> capabilities) {
        std::uint32_t bits{};
        for (const TerrainFoliageCapability capability : capabilities) {
            if (!IsKnown(capability))
                return Result<TerrainFoliageCapabilitySet>::Failure(MakeError(TerrainErrors::RegistryDescriptorInvalid));
            bits |= std::uint32_t{1} << static_cast<std::uint8_t>(capability);
        }
        return Result<TerrainFoliageCapabilitySet>::Success(TerrainFoliageCapabilitySet{bits});
    }

    /** @copydoc ProjectTerrainFoliageCapabilities */
    Result<TerrainFoliageCapabilityProjection> ProjectTerrainFoliageCapabilities(const TerrainFoliageCapabilitySet available,
                                                                                 const TerrainFoliageCapabilitySet required) {
        if (!available.IsValid() || !required.IsValid())
            return Result<TerrainFoliageCapabilityProjection>::Failure(MakeError(TerrainErrors::RegistryDescriptorInvalid));
        if (!available.ContainsAll(required))
            return Result<TerrainFoliageCapabilityProjection>::Failure(MakeError(TerrainErrors::CapabilityUnsupported));
        return Result<TerrainFoliageCapabilityProjection>::Success({available, required});
    }

    /** @copydoc TerrainFoliageRegistrySnapshot::IsValid */
    bool TerrainFoliageRegistrySnapshot::IsValid() const noexcept {
        return state_ != nullptr && state_->binding.IsValid();
    }

    /** @copydoc TerrainFoliageRegistrySnapshot::Binding */
    const TerrainFoliageRegistryBinding &TerrainFoliageRegistrySnapshot::Binding() const noexcept {
        static const TerrainFoliageRegistryBinding InvalidBinding{};
        return state_ == nullptr ? InvalidBinding : state_->binding;
    }

    /** @copydoc TerrainFoliageRegistrySnapshot::Capabilities */
    TerrainFoliageCapabilitySet TerrainFoliageRegistrySnapshot::Capabilities() const noexcept {
        return state_ == nullptr ? TerrainFoliageCapabilitySet::Empty() : state_->capabilities;
    }

    /** @copydoc TerrainFoliageRegistrySnapshot::Datasets */
    std::span<const TerrainDatasetRegistration> TerrainFoliageRegistrySnapshot::Datasets() const noexcept {
        return state_ == nullptr ? std::span<const TerrainDatasetRegistration>{}
                                 : std::span<const TerrainDatasetRegistration>{state_->datasets};
    }

    /** @copydoc TerrainFoliageRegistrySnapshot::FoliageTypes */
    std::span<const TerrainFoliageTypeRegistration> TerrainFoliageRegistrySnapshot::FoliageTypes() const noexcept {
        return state_ == nullptr ? std::span<const TerrainFoliageTypeRegistration>{}
                                 : std::span<const TerrainFoliageTypeRegistration>{state_->foliageTypes};
    }

    /** @copydoc TerrainFoliageRegistrySnapshot::FindDataset */
    Result<TerrainDatasetRegistryHandle> TerrainFoliageRegistrySnapshot::FindDataset(const TerrainDatasetId dataset) const {
        if (!IsValid() || !dataset.IsValid())
            return Result<TerrainDatasetRegistryHandle>::Failure(MakeError(TerrainErrors::RegistryHandleInvalid));
        const auto datasets = Datasets();
        const auto found = LowerBoundDataset(datasets, dataset);
        if (found == datasets.end() || found->descriptor.Data().dataset != dataset)
            return Result<TerrainDatasetRegistryHandle>::Failure(MakeError(TerrainErrors::IdentityUnknown));
        const auto slot = static_cast<std::uint32_t>(std::distance(datasets.begin(), found));
        return Result<TerrainDatasetRegistryHandle>::Success(
            TerrainDatasetRegistryHandle{Binding(), slot, dataset, found->descriptor.Data().content});
    }

    /** @copydoc TerrainFoliageRegistrySnapshot::FindFoliageType */
    Result<TerrainFoliageTypeRegistryHandle> TerrainFoliageRegistrySnapshot::FindFoliageType(const FoliageTypeId type) const {
        if (!IsValid() || !type.IsValid())
            return Result<TerrainFoliageTypeRegistryHandle>::Failure(MakeError(TerrainErrors::RegistryHandleInvalid));
        const auto foliageTypes = FoliageTypes();
        const auto found = LowerBoundFoliageType(foliageTypes, type);
        if (found == foliageTypes.end() || found->definition.Data().type != type)
            return Result<TerrainFoliageTypeRegistryHandle>::Failure(MakeError(TerrainErrors::IdentityUnknown));
        const auto slot = static_cast<std::uint32_t>(std::distance(foliageTypes.begin(), found));
        return Result<TerrainFoliageTypeRegistryHandle>::Success(
            TerrainFoliageTypeRegistryHandle{Binding(), slot, type, found->definition.Data().revision});
    }

    /** @copydoc TerrainFoliageRegistrySnapshot::QueryDatasets */
    Result<TerrainFoliageRegistryQueryResult> TerrainFoliageRegistrySnapshot::QueryDatasets(
        const TerrainDatasetRegistryQuery &query, const std::span<TerrainDatasetRegistryHandle> output) const {
        if (!IsValid())
            return Result<TerrainFoliageRegistryQueryResult>::Failure(MakeError(TerrainErrors::RegistryClosed));
        if (const auto valid = ValidateQuery(query.requiredCapabilities, query.tier); valid.HasError())
            return Result<TerrainFoliageRegistryQueryResult>::Failure(valid.ErrorValue());
        if (const auto projected = ProjectCapabilities(query.requiredCapabilities); projected.HasError())
            return Result<TerrainFoliageRegistryQueryResult>::Failure(projected.ErrorValue());

        const auto datasets = Datasets();
        std::size_t matches{};
        for (const auto &registration : datasets) {
            if ((!query.tier.has_value() || registration.supportedTiers.Contains(*query.tier)) &&
                Capabilities().ContainsAll(registration.requiredCapabilities))
                ++matches;
        }
        if (matches > output.size())
            return Result<TerrainFoliageRegistryQueryResult>::Failure(MakeError(TerrainErrors::CapacityExceeded));

        std::size_t written{};
        for (std::size_t index = 0; index < datasets.size(); ++index) {
            const auto &registration = datasets[index];
            if ((!query.tier.has_value() || registration.supportedTiers.Contains(*query.tier)) &&
                Capabilities().ContainsAll(registration.requiredCapabilities))
                output[written++] = {Binding(), static_cast<std::uint32_t>(index), registration.descriptor.Data().dataset,
                                     registration.descriptor.Data().content};
        }
        return Result<TerrainFoliageRegistryQueryResult>::Success({Binding(), written, datasets.size()});
    }

    /** @copydoc TerrainFoliageRegistrySnapshot::QueryFoliageTypes */
    Result<TerrainFoliageRegistryQueryResult> TerrainFoliageRegistrySnapshot::QueryFoliageTypes(
        const TerrainFoliageTypeRegistryQuery &query, const std::span<TerrainFoliageTypeRegistryHandle> output) const {
        if (!IsValid())
            return Result<TerrainFoliageRegistryQueryResult>::Failure(MakeError(TerrainErrors::RegistryClosed));
        if (const auto valid = ValidateQuery(query.requiredCapabilities); valid.HasError())
            return Result<TerrainFoliageRegistryQueryResult>::Failure(valid.ErrorValue());
        if (const auto projected = ProjectCapabilities(query.requiredCapabilities); projected.HasError())
            return Result<TerrainFoliageRegistryQueryResult>::Failure(projected.ErrorValue());

        const auto foliageTypes = FoliageTypes();
        std::size_t matches{};
        for (const auto &registration : foliageTypes) {
            if (Capabilities().ContainsAll(registration.requiredCapabilities))
                ++matches;
        }
        if (matches > output.size())
            return Result<TerrainFoliageRegistryQueryResult>::Failure(MakeError(TerrainErrors::CapacityExceeded));

        std::size_t written{};
        for (std::size_t index = 0; index < foliageTypes.size(); ++index) {
            const auto &registration = foliageTypes[index];
            if (Capabilities().ContainsAll(registration.requiredCapabilities))
                output[written++] = {Binding(), static_cast<std::uint32_t>(index), registration.definition.Data().type,
                                     registration.definition.Data().revision};
        }
        return Result<TerrainFoliageRegistryQueryResult>::Success({Binding(), written, foliageTypes.size()});
    }

    /** @copydoc TerrainFoliageRegistrySnapshot::Resolve(const TerrainDatasetRegistryHandle &) const */
    Result<const TerrainDatasetRegistration *> TerrainFoliageRegistrySnapshot::Resolve(const TerrainDatasetRegistryHandle &handle) const {
        if (!IsValid() || !handle.IsValid())
            return Result<const TerrainDatasetRegistration *>::Failure(MakeError(TerrainErrors::RegistryHandleInvalid));
        if (handle.binding != Binding())
            return Result<const TerrainDatasetRegistration *>::Failure(MakeError(TerrainErrors::RegistryHandleStale));
        const auto datasets = Datasets();
        if (handle.slot >= datasets.size())
            return Result<const TerrainDatasetRegistration *>::Failure(MakeError(TerrainErrors::RegistryHandleInvalid));
        const auto &registration = datasets[handle.slot];
        if (registration.descriptor.Data().dataset != handle.dataset || registration.descriptor.Data().content != handle.content)
            return Result<const TerrainDatasetRegistration *>::Failure(MakeError(TerrainErrors::RegistryHandleStale));
        return Result<const TerrainDatasetRegistration *>::Success(&registration);
    }

    /** @copydoc TerrainFoliageRegistrySnapshot::Resolve(const TerrainFoliageTypeRegistryHandle &) const */
    Result<const TerrainFoliageTypeRegistration *> TerrainFoliageRegistrySnapshot::Resolve(
        const TerrainFoliageTypeRegistryHandle &handle) const {
        if (!IsValid() || !handle.IsValid())
            return Result<const TerrainFoliageTypeRegistration *>::Failure(MakeError(TerrainErrors::RegistryHandleInvalid));
        if (handle.binding != Binding())
            return Result<const TerrainFoliageTypeRegistration *>::Failure(MakeError(TerrainErrors::RegistryHandleStale));
        const auto foliageTypes = FoliageTypes();
        if (handle.slot >= foliageTypes.size())
            return Result<const TerrainFoliageTypeRegistration *>::Failure(MakeError(TerrainErrors::RegistryHandleInvalid));
        const auto &registration = foliageTypes[handle.slot];
        if (registration.definition.Data().type != handle.type || registration.definition.Data().revision != handle.revision)
            return Result<const TerrainFoliageTypeRegistration *>::Failure(MakeError(TerrainErrors::RegistryHandleStale));
        return Result<const TerrainFoliageTypeRegistration *>::Success(&registration);
    }

    /** @copydoc TerrainFoliageRegistrySnapshot::ProjectCapabilities */
    Result<TerrainFoliageCapabilityProjection> TerrainFoliageRegistrySnapshot::ProjectCapabilities(
        const TerrainFoliageCapabilitySet required) const {
        if (!IsValid())
            return Result<TerrainFoliageCapabilityProjection>::Failure(MakeError(TerrainErrors::RegistryClosed));
        return ProjectTerrainFoliageCapabilities(Capabilities(), required);
    }

    TerrainFoliageRegistry::TerrainFoliageRegistry(const TerrainFoliageRegistryInstanceId instance,
                                                   const TerrainFoliageCapabilitySet capabilities,
                                                   const TerrainFoliageRegistryLimits limits,
                                                   std::shared_ptr<const TerrainFoliageRegistrySnapshot::State> state)
        : instance_(instance), capabilities_(capabilities), limits_(limits), state_(std::move(state)) {}

    TerrainFoliageRegistry::~TerrainFoliageRegistry() {
        Shutdown();
    }

    TerrainFoliageRegistry::TerrainFoliageRegistry(TerrainFoliageRegistry &&other) noexcept
        : instance_(other.instance_), capabilities_(other.capabilities_), limits_(other.limits_), state_(std::move(other.state_)),
          lifecycle_(other.lifecycle_) {
        other.lifecycle_ = TerrainFoliageRegistryState::Closed;
    }

    TerrainFoliageRegistry &TerrainFoliageRegistry::operator=(TerrainFoliageRegistry &&other) noexcept {
        if (this == &other)
            return *this;
        Shutdown();
        instance_ = other.instance_;
        capabilities_ = other.capabilities_;
        limits_ = other.limits_;
        state_ = std::move(other.state_);
        lifecycle_ = other.lifecycle_;
        other.lifecycle_ = TerrainFoliageRegistryState::Closed;
        return *this;
    }

    /** @copydoc TerrainFoliageRegistry::Create */
    Result<TerrainFoliageRegistry> TerrainFoliageRegistry::Create(const TerrainFoliageRegistryInstanceId instance,
                                                                  const TerrainFoliageCapabilitySet capabilities,
                                                                  const TerrainFoliageRegistryLimits limits) {
        if (!instance.IsValid() || !capabilities.IsValid() || !limits.IsValid())
            return Result<TerrainFoliageRegistry>::Failure(MakeError(TerrainErrors::RegistryDescriptorInvalid));
        auto state = std::make_shared<TerrainFoliageRegistrySnapshot::State>();
        const auto revision = TerrainFoliageRegistryRevision::Create(1);
        if (revision.HasError())
            return Result<TerrainFoliageRegistry>::Failure(revision.ErrorValue());
        state->binding = {instance, revision.Value()};
        state->capabilities = capabilities;
        return Result<TerrainFoliageRegistry>::Success(TerrainFoliageRegistry{instance, capabilities, limits, std::move(state)});
    }

    /** @copydoc TerrainFoliageRegistry::RegisterDataset */
    Result<TerrainFoliageRegistryRevision> TerrainFoliageRegistry::RegisterDataset(TerrainDatasetRegistration registration) {
        if (const auto mutation = ValidateMutation(lifecycle_, state_.get()); mutation.HasError())
            return mutation;
        if (const auto valid = ValidateDatasetRegistration(registration, capabilities_, limits_); valid.HasError())
            return Result<TerrainFoliageRegistryRevision>::Failure(valid.ErrorValue());
        const auto insertion = LowerBoundDataset(state_->datasets, registration.descriptor.Data().dataset);
        if (insertion != state_->datasets.end() && insertion->descriptor.Data().dataset == registration.descriptor.Data().dataset)
            return Result<TerrainFoliageRegistryRevision>::Failure(MakeError(TerrainErrors::RegistryDuplicate));
        if (state_->datasets.size() >= limits_.maximumDatasets)
            return Result<TerrainFoliageRegistryRevision>::Failure(MakeError(TerrainErrors::CapacityExceeded));
        auto datasets = state_->datasets;
        datasets.insert(datasets.begin() + std::distance(state_->datasets.begin(), insertion), std::move(registration));
        return Publish(std::move(datasets), state_->foliageTypes);
    }

    /** @copydoc TerrainFoliageRegistry::ReplaceDataset */
    Result<TerrainFoliageRegistryRevision> TerrainFoliageRegistry::ReplaceDataset(TerrainDatasetRegistration registration) {
        if (const auto mutation = ValidateMutation(lifecycle_, state_.get()); mutation.HasError())
            return mutation;
        if (const auto valid = ValidateDatasetRegistration(registration, capabilities_, limits_); valid.HasError())
            return Result<TerrainFoliageRegistryRevision>::Failure(valid.ErrorValue());
        auto datasets = state_->datasets;
        const auto found = LowerBoundDataset(datasets, registration.descriptor.Data().dataset);
        if (found == datasets.end() || found->descriptor.Data().dataset != registration.descriptor.Data().dataset)
            return Result<TerrainFoliageRegistryRevision>::Failure(MakeError(TerrainErrors::IdentityUnknown));
        if (!HasNewerDatasetRevision(registration.descriptor, found->descriptor))
            return Result<TerrainFoliageRegistryRevision>::Failure(MakeError(TerrainErrors::RevisionStale));
        *found = std::move(registration);
        return Publish(std::move(datasets), state_->foliageTypes);
    }

    /** @copydoc TerrainFoliageRegistry::UnregisterDataset */
    Result<bool> TerrainFoliageRegistry::UnregisterDataset(const TerrainDatasetId dataset) {
        if (lifecycle_ != TerrainFoliageRegistryState::Active)
            return Result<bool>::Failure(MakeError(TerrainErrors::RegistryClosed));
        if (!dataset.IsValid())
            return Result<bool>::Failure(MakeError(TerrainErrors::IdentityInvalid));
        auto datasets = state_->datasets;
        const auto found = LowerBoundDataset(datasets, dataset);
        if (found == datasets.end() || found->descriptor.Data().dataset != dataset)
            return Result<bool>::Success(false);
        datasets.erase(found);
        const auto published = Publish(std::move(datasets), state_->foliageTypes);
        return published.HasError() ? Result<bool>::Failure(published.ErrorValue()) : Result<bool>::Success(true);
    }

    /** @copydoc TerrainFoliageRegistry::RegisterFoliageType */
    Result<TerrainFoliageRegistryRevision> TerrainFoliageRegistry::RegisterFoliageType(TerrainFoliageTypeRegistration registration) {
        if (const auto mutation = ValidateMutation(lifecycle_, state_.get()); mutation.HasError())
            return mutation;
        if (const auto valid = ValidateFoliageRegistration(registration, capabilities_, limits_); valid.HasError())
            return Result<TerrainFoliageRegistryRevision>::Failure(valid.ErrorValue());
        const auto insertion = LowerBoundFoliageType(state_->foliageTypes, registration.definition.Data().type);
        if (insertion != state_->foliageTypes.end() && insertion->definition.Data().type == registration.definition.Data().type)
            return Result<TerrainFoliageRegistryRevision>::Failure(MakeError(TerrainErrors::RegistryDuplicate));
        if (state_->foliageTypes.size() >= limits_.maximumFoliageTypes)
            return Result<TerrainFoliageRegistryRevision>::Failure(MakeError(TerrainErrors::CapacityExceeded));
        auto foliageTypes = state_->foliageTypes;
        foliageTypes.insert(foliageTypes.begin() + std::distance(state_->foliageTypes.begin(), insertion), std::move(registration));
        return Publish(state_->datasets, std::move(foliageTypes));
    }

    /** @copydoc TerrainFoliageRegistry::ReplaceFoliageType */
    Result<TerrainFoliageRegistryRevision> TerrainFoliageRegistry::ReplaceFoliageType(TerrainFoliageTypeRegistration registration) {
        if (const auto mutation = ValidateMutation(lifecycle_, state_.get()); mutation.HasError())
            return mutation;
        if (const auto valid = ValidateFoliageRegistration(registration, capabilities_, limits_); valid.HasError())
            return Result<TerrainFoliageRegistryRevision>::Failure(valid.ErrorValue());
        auto foliageTypes = state_->foliageTypes;
        const auto found = LowerBoundFoliageType(foliageTypes, registration.definition.Data().type);
        if (found == foliageTypes.end() || found->definition.Data().type != registration.definition.Data().type)
            return Result<TerrainFoliageRegistryRevision>::Failure(MakeError(TerrainErrors::IdentityUnknown));
        if (registration.definition.Data().revision <= found->definition.Data().revision)
            return Result<TerrainFoliageRegistryRevision>::Failure(MakeError(TerrainErrors::RevisionStale));
        *found = std::move(registration);
        return Publish(state_->datasets, std::move(foliageTypes));
    }

    /** @copydoc TerrainFoliageRegistry::UnregisterFoliageType */
    Result<bool> TerrainFoliageRegistry::UnregisterFoliageType(const FoliageTypeId type) {
        if (lifecycle_ != TerrainFoliageRegistryState::Active)
            return Result<bool>::Failure(MakeError(TerrainErrors::RegistryClosed));
        if (!type.IsValid())
            return Result<bool>::Failure(MakeError(TerrainErrors::IdentityInvalid));
        auto foliageTypes = state_->foliageTypes;
        const auto found = LowerBoundFoliageType(foliageTypes, type);
        if (found == foliageTypes.end() || found->definition.Data().type != type)
            return Result<bool>::Success(false);
        foliageTypes.erase(found);
        const auto published = Publish(state_->datasets, std::move(foliageTypes));
        return published.HasError() ? Result<bool>::Failure(published.ErrorValue()) : Result<bool>::Success(true);
    }

    /** @copydoc TerrainFoliageRegistry::Snapshot */
    Result<TerrainFoliageRegistrySnapshot> TerrainFoliageRegistry::Snapshot() const {
        if (lifecycle_ != TerrainFoliageRegistryState::Active)
            return Result<TerrainFoliageRegistrySnapshot>::Failure(MakeError(TerrainErrors::RegistryClosed));
        return Result<TerrainFoliageRegistrySnapshot>::Success(TerrainFoliageRegistrySnapshot{state_});
    }

    /** @copydoc TerrainFoliageRegistry::BeginCancellation */
    void TerrainFoliageRegistry::BeginCancellation() noexcept {
        if (lifecycle_ == TerrainFoliageRegistryState::Active)
            lifecycle_ = TerrainFoliageRegistryState::Cancelling;
    }

    /** @copydoc TerrainFoliageRegistry::Shutdown */
    void TerrainFoliageRegistry::Shutdown() noexcept {
        lifecycle_ = TerrainFoliageRegistryState::Closed;
        state_.reset();
    }

    /** @copydoc TerrainFoliageRegistry::Lifecycle */
    TerrainFoliageRegistryState TerrainFoliageRegistry::Lifecycle() const noexcept {
        return lifecycle_;
    }

    Result<TerrainFoliageRegistryRevision> TerrainFoliageRegistry::Publish(std::vector<TerrainDatasetRegistration> datasets,
                                                                           std::vector<TerrainFoliageTypeRegistration> foliageTypes) {
        const auto next = NextRevision(state_->binding.revision);
        if (next.HasError())
            return next;
        auto state = std::make_shared<TerrainFoliageRegistrySnapshot::State>();
        state->binding = {instance_, next.Value()};
        state->capabilities = capabilities_;
        state->datasets = std::move(datasets);
        state->foliageTypes = std::move(foliageTypes);
        state_ = std::move(state);
        return next;
    }
}  // namespace Horo::Terrain
