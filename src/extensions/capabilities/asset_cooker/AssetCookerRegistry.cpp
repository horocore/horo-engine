#include "Horo/Extensions/AssetCookerRegistry.h"

#include "../../ExtensionAuthorityIdentityValidation.h"
#include "Horo/Extensions/ExtensionErrors.h"
#include "Horo/Foundation/Sha256.h"

#include <algorithm>
#include <atomic>
#include <format>
#include <mutex>
#include <ranges>
#include <utility>

namespace Horo::Extensions {
    struct AssetCookerProviderState final {
        AssetCookerDescriptor descriptor;
        std::shared_ptr<const IAssetCooker> provider;
        std::atomic_bool registered{true};
    };

    struct AssetCookerRegistryState final {
        std::mutex mutex;
        std::vector<std::shared_ptr<AssetCookerProviderState>> providers;
        bool shutdown{};
    };

    namespace {
        constexpr std::size_t kMaximumVersionBytes = 128;
        constexpr std::size_t kMaximumSourceBytes = 1ULL << 30U;
        constexpr std::size_t kMaximumArtifactBytes = 1ULL << 30U;
        constexpr std::size_t kMaximumDependencies = 1ULL << 16U;
        constexpr std::size_t kMaximumDiagnostics = 1ULL << 12U;
        constexpr std::size_t kMaximumDiagnosticCodeBytes = 1ULL << 10U;
        constexpr std::size_t kMaximumDiagnosticMessageBytes = 1ULL << 16U;

        [[nodiscard]] bool IsVersionCharacter(const char character) noexcept {
            return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                   (character >= '0' && character <= '9') || character == '.' || character == '-' || character == '_';
        }

        [[nodiscard]] bool ValidVersion(const std::string_view value) noexcept {
            return !value.empty() && value.size() <= kMaximumVersionBytes && std::ranges::all_of(value, IsVersionCharacter);
        }

        [[nodiscard]] bool ValidDescriptorIdentity(const AssetCookerDescriptor &descriptor) noexcept {
            return Detail::IsCanonicalExtensionAuthorityId(descriptor.cookerId.value) &&
                   Detail::IsCanonicalExtensionAuthorityId(descriptor.providerId) && descriptor.providerGeneration != 0U &&
                   !descriptor.assetType.Value().empty() && !descriptor.targets.empty() && ValidVersion(descriptor.cookerVersion) &&
                   descriptor.artifactFormatVersion != 0U;
        }

        [[nodiscard]] bool ValidDescriptor(const AssetCookerDescriptor &descriptor) noexcept {
            if (!ValidDescriptorIdentity(descriptor))
                return false;
            std::string_view previous;
            for (const AssetCookTargetId &target : descriptor.targets) {
                if (target.Value().empty() || (!previous.empty() && previous >= target.Value()))
                    return false;
                previous = target.Value();
            }
            return true;
        }

        [[nodiscard]] bool ValidBound(const std::size_t value, const std::size_t maximum) noexcept {
            return value > 0U && value <= maximum;
        }

        [[nodiscard]] bool ValidLimits(const AssetCookerLimits &limits) noexcept {
            return ValidBound(limits.maximumSourceBytes, kMaximumSourceBytes) &&
                   ValidBound(limits.maximumArtifactBytes, kMaximumArtifactBytes) &&
                   ValidBound(limits.maximumDependencies, kMaximumDependencies) &&
                   ValidBound(limits.maximumDiagnostics, kMaximumDiagnostics) &&
                   ValidBound(limits.maximumDiagnosticCodeBytes, kMaximumDiagnosticCodeBytes) &&
                   ValidBound(limits.maximumDiagnosticMessageBytes, kMaximumDiagnosticMessageBytes);
        }

        [[nodiscard]] bool Handles(const AssetCookerDescriptor &descriptor, const Assets::AssetTypeId &type,
                                   const AssetCookTargetId &target) noexcept {
            return descriptor.assetType == type &&
                   std::ranges::binary_search(descriptor.targets, target.Value(), {}, &AssetCookTargetId::Value);
        }

        [[nodiscard]] bool ValidInputShape(const AssetCookerRequest &request) noexcept {
            return ValidLimits(request.limits) && request.input.assetId.IsValid() && !request.input.assetType.Value().empty() &&
                   !request.input.target.Value().empty() && request.input.metadataSchemaVersion != 0U &&
                   request.input.settingsSchemaVersion != 0U && !request.input.sourceBytes.empty() &&
                   request.input.sourceBytes.size() <= request.limits.maximumSourceBytes;
        }

        [[nodiscard]] bool ValidRequest(const AssetCookerRequest &request) noexcept {
            if (!ValidInputShape(request))
                return false;
            if (request.selectedCooker.has_value() && !Detail::IsCanonicalExtensionAuthorityId(request.selectedCooker->value))
                return false;
            return ComputeSha256(std::as_bytes(request.input.sourceBytes)) == request.input.sourceDigest;
        }

        [[nodiscard]] Error ProviderFailure(const AssetCookerDescriptor &descriptor, Error cause = {}) {
            const std::string detail = std::format("Asset cooker failed: {}@{} ({}).", descriptor.providerId, descriptor.providerGeneration,
                                                   descriptor.cookerId.value);
            return cause.code.Value().empty() ? MakeError(ExtensionErrors::AssetCookerInvocationFailed, detail)
                                              : WrapError(ExtensionErrors::AssetCookerInvocationFailed, std::move(cause), detail);
        }

        [[nodiscard]] Error CancellationFailure(const AssetCookerDescriptor *descriptor) {
            if (descriptor == nullptr)
                return MakeError(ExtensionErrors::AssetCookCancelled);
            return MakeError(ExtensionErrors::AssetCookCancelled, std::format("Asset cook cancelled at {}@{} ({}).", descriptor->providerId,
                                                                              descriptor->providerGeneration, descriptor->cookerId.value));
        }

        [[nodiscard]] Result<std::shared_ptr<AssetCookerProviderState>> SelectProvider(
            const std::shared_ptr<AssetCookerRegistryState> &state, const AssetCookerRequest &request) {
            std::scoped_lock lock{state->mutex};
            if (state->shutdown)
                return Result<std::shared_ptr<AssetCookerProviderState>>::Failure(MakeError(ExtensionErrors::AssetCookerRegistryShutdown));

            std::shared_ptr<AssetCookerProviderState> selected;
            for (const auto &candidate : state->providers) {
                if (!Handles(candidate->descriptor, request.input.assetType, request.input.target))
                    continue;
                if (request.selectedCooker.has_value() && candidate->descriptor.cookerId != *request.selectedCooker)
                    continue;
                if (selected != nullptr)
                    return Result<std::shared_ptr<AssetCookerProviderState>>::Failure(
                        MakeError(ExtensionErrors::AssetCookerAmbiguous,
                                  "Multiple cookers claim the requested asset type and target; select one exact cooker ID."));
                selected = candidate;
            }
            if (selected == nullptr)
                return Result<std::shared_ptr<AssetCookerProviderState>>::Failure(MakeError(ExtensionErrors::AssetCookerUnavailable));
            return Result<std::shared_ptr<AssetCookerProviderState>>::Success(std::move(selected));
        }
    }  // namespace

    AssetCookerOutputSink::AssetCookerOutputSink(const AssetCookerLimits &limits) noexcept : limits_(limits) {}

    /** @copydoc AssetCookerOutputSink::WritePayload */
    Result<void> AssetCookerOutputSink::WritePayload(const std::span<const std::uint8_t> bytes) {
        if (payloadWritten_ || bytes.size() > limits_.maximumArtifactBytes) {
            rejected_ = true;
            return Result<void>::Failure(MakeError(ExtensionErrors::AssetCookerOutputInvalid));
        }
        try {
            payload_.assign(bytes.begin(), bytes.end());
        } catch (...) {  // NOSONAR(cpp:S1181) Provider-facing allocation boundary.
            rejected_ = true;
            return Result<void>::Failure(MakeError(ExtensionErrors::AssetCookerOutputInvalid));
        }
        payloadWritten_ = true;
        return Result<void>::Success();
    }

    /** @copydoc AssetCookerOutputSink::AddDependency */
    Result<void> AssetCookerOutputSink::AddDependency(const Assets::AssetId dependency) {
        if (!dependency.IsValid() || dependencies_.size() >= limits_.maximumDependencies ||
            std::ranges::find(dependencies_, dependency) != dependencies_.end()) {
            rejected_ = true;
            return Result<void>::Failure(MakeError(ExtensionErrors::AssetCookerOutputInvalid));
        }
        try {
            dependencies_.push_back(dependency);
        } catch (...) {  // NOSONAR(cpp:S1181) Provider-facing allocation boundary.
            rejected_ = true;
            return Result<void>::Failure(MakeError(ExtensionErrors::AssetCookerOutputInvalid));
        }
        return Result<void>::Success();
    }

    /** @copydoc AssetCookerOutputSink::AddDiagnostic */
    Result<void> AssetCookerOutputSink::AddDiagnostic(AssetCookerDiagnostic diagnostic) {
        if (diagnostic.code.Value().empty() || diagnostic.code.Value().size() > limits_.maximumDiagnosticCodeBytes ||
            diagnostic.message.empty() || diagnostic.message.size() > limits_.maximumDiagnosticMessageBytes ||
            diagnostics_.size() >= limits_.maximumDiagnostics) {
            rejected_ = true;
            return Result<void>::Failure(MakeError(ExtensionErrors::AssetCookerOutputInvalid));
        }
        try {
            diagnostics_.push_back(std::move(diagnostic));
        } catch (...) {  // NOSONAR(cpp:S1181) Provider-facing allocation boundary.
            rejected_ = true;
            return Result<void>::Failure(MakeError(ExtensionErrors::AssetCookerOutputInvalid));
        }
        return Result<void>::Success();
    }

    Result<void> AssetCookerOutputSink::Complete() {
        if (!payloadWritten_ || rejected_)
            return Result<void>::Failure(MakeError(ExtensionErrors::AssetCookerOutputInvalid));
        std::ranges::sort(dependencies_);
        return Result<void>::Success();
    }

    AssetCookerRegistration::AssetCookerRegistration(std::weak_ptr<AssetCookerRegistryState> registry,
                                                     std::shared_ptr<AssetCookerProviderState> provider) noexcept
        : registry_(std::move(registry)), provider_(std::move(provider)) {}

    AssetCookerRegistration::~AssetCookerRegistration() noexcept {
        try {
            Reset();
        } catch (...) {
            if (provider_ != nullptr)
                provider_->registered.store(false, std::memory_order_release);
        }
    }

    AssetCookerRegistration::AssetCookerRegistration(AssetCookerRegistration &&other) noexcept
        : registry_(std::move(other.registry_)), provider_(std::move(other.provider_)) {}

    AssetCookerRegistration &AssetCookerRegistration::operator=(AssetCookerRegistration &&other) {
        if (this == &other)
            return *this;
        Reset();
        registry_ = std::move(other.registry_);
        provider_ = std::move(other.provider_);
        return *this;
    }

    /** @copydoc AssetCookerRegistration::Reset */
    void AssetCookerRegistration::Reset() {
        if (provider_ == nullptr)
            return;
        const auto registry = registry_.lock();
        if (registry != nullptr) {
            std::scoped_lock lock{registry->mutex};
            provider_->registered.store(false, std::memory_order_release);
            std::erase_if(registry->providers, [retired = provider_](const auto &candidate) {
                return candidate == retired;
            });
        } else {
            provider_->registered.store(false, std::memory_order_release);
        }
        registry_ = {};
        provider_ = {};
    }

    /** @copydoc AssetCookerRegistration::IsRegistered */
    bool AssetCookerRegistration::IsRegistered() const noexcept {
        return provider_ != nullptr && provider_->registered.load(std::memory_order_acquire);
    }

    AssetCookerRegistry::AssetCookerRegistry() : state_(std::make_shared<AssetCookerRegistryState>()) {
        state_->providers.reserve(MaximumProviders);
    }

    AssetCookerRegistry::~AssetCookerRegistry() noexcept {
        try {
            BeginShutdown();
        } catch (...) {
            state_.reset();
        }
    }

    AssetCookerRegistry &AssetCookerRegistry::operator=(AssetCookerRegistry &&other) {
        if (this == &other)
            return *this;
        BeginShutdown();
        state_ = std::move(other.state_);
        return *this;
    }

    /** @copydoc AssetCookerRegistry::Register */
    Result<AssetCookerRegistration> AssetCookerRegistry::Register(  // NOSONAR(cpp:S5817) Publication mutates owner lifecycle state.
        AssetCookerDescriptor descriptor, std::shared_ptr<const IAssetCooker> provider) {
        if (!ValidDescriptor(descriptor) || provider == nullptr)
            return Result<AssetCookerRegistration>::Failure(MakeError(ExtensionErrors::AssetCookerRegistryInvalid));
        if (state_ == nullptr)
            return Result<AssetCookerRegistration>::Failure(MakeError(ExtensionErrors::AssetCookerRegistryShutdown));

        std::scoped_lock lock{state_->mutex};
        if (state_->shutdown)
            return Result<AssetCookerRegistration>::Failure(MakeError(ExtensionErrors::AssetCookerRegistryShutdown));
        const auto insertion = std::ranges::lower_bound(state_->providers, descriptor.cookerId.value, {}, [](const auto &candidate) {
            return candidate->descriptor.cookerId.value;
        });
        if (insertion != state_->providers.end() && (*insertion)->descriptor.cookerId == descriptor.cookerId)
            return Result<AssetCookerRegistration>::Failure(MakeError(ExtensionErrors::AssetCookerRegistryDuplicate));
        if (state_->providers.size() >= MaximumProviders)
            return Result<AssetCookerRegistration>::Failure(MakeError(ExtensionErrors::AssetCookerRegistryCapacityExceeded));

        auto published = std::make_shared<AssetCookerProviderState>();
        published->descriptor = std::move(descriptor);
        published->provider = std::move(provider);
        state_->providers.insert(insertion, published);
        return Result<AssetCookerRegistration>::Success(AssetCookerRegistration{state_, std::move(published)});
    }

    /** @copydoc AssetCookerRegistry::Cook */
    Result<AssetCookerResult> AssetCookerRegistry::Cook(const AssetCookerRequest &request, const CancellationToken &cancellation) const {
        if (!ValidRequest(request))
            return Result<AssetCookerResult>::Failure(MakeError(ExtensionErrors::AssetCookerRegistryInvalid));
        if (state_ == nullptr)
            return Result<AssetCookerResult>::Failure(MakeError(ExtensionErrors::AssetCookerRegistryShutdown));
        auto selected = SelectProvider(state_, request);
        if (selected.HasError())
            return Result<AssetCookerResult>::Failure(selected.ErrorValue());
        const auto provider = selected.Value();
        if (cancellation.IsCancellationRequested())
            return Result<AssetCookerResult>::Failure(CancellationFailure(&provider->descriptor));

        AssetCookerOutputSink output{request.limits};
        try {
            auto cooked = provider->provider->Cook(request.input, output, cancellation);
            if (cooked.HasError())
                return Result<AssetCookerResult>::Failure(ProviderFailure(provider->descriptor, cooked.ErrorValue()));
        } catch (...) {  // NOSONAR(cpp:S1181) Trusted extension callback exception boundary.
            return Result<AssetCookerResult>::Failure(ProviderFailure(provider->descriptor));
        }
        if (cancellation.IsCancellationRequested())
            return Result<AssetCookerResult>::Failure(CancellationFailure(&provider->descriptor));
        if (auto completed = output.Complete(); completed.HasError())
            return Result<AssetCookerResult>::Failure(ProviderFailure(provider->descriptor, completed.ErrorValue()));

        const Assets::AssetCookCacheKey cacheKey = Assets::BuildAssetCookCacheKey({
            .assetId = request.input.assetId,
            .assetType = request.input.assetType,
            .sourceDigest = request.input.sourceDigest,
            .metadataDigest = request.input.metadataDigest,
            .metadataSchemaVersion = request.input.metadataSchemaVersion,
            .settingsDigest = request.input.settingsDigest,
            .settingsSchemaVersion = request.input.settingsSchemaVersion,
            .cookerContributionId = provider->descriptor.cookerId.value,
            .cookerVersion = provider->descriptor.cookerVersion,
            .target = request.input.target,
            .artifactFormatVersion = provider->descriptor.artifactFormatVersion,
        });
        return Result<AssetCookerResult>::Success(AssetCookerResult{
            .provider = provider->descriptor,
            .cacheKey = cacheKey,
            .payload = std::move(output.payload_),
            .dependencies = std::move(output.dependencies_),
            .diagnostics = std::move(output.diagnostics_),
        });
    }

    /** @copydoc AssetCookerRegistry::BeginShutdown */
    void AssetCookerRegistry::BeginShutdown() {  // NOSONAR(cpp:S5817) Terminal admission mutation belongs to the owner facade.
        const auto state = state_;
        if (state == nullptr)
            return;
        std::scoped_lock lock{state->mutex};
        state->shutdown = true;
        while (!state->providers.empty()) {
            const auto provider = std::move(state->providers.back());
            state->providers.pop_back();
            provider->registered.store(false, std::memory_order_release);
        }
    }

    /** @copydoc AssetCookerRegistry::IsShutdown */
    bool AssetCookerRegistry::IsShutdown() const {
        if (state_ == nullptr)
            return true;
        std::scoped_lock lock{state_->mutex};
        return state_->shutdown;
    }
}  // namespace Horo::Extensions
