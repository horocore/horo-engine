#include "Horo/Runtime/Scene/SceneCellAttachments.h"

#include <algorithm>
#include <utility>

namespace Horo::Runtime {
    namespace W = WorldStreaming;

    namespace Detail {
        /** @brief Shared published capability status, independent of pending binding replacement. */
        struct SceneCellAttachmentPublication final {
            std::vector<SceneCellAttachmentStatus> active;
            const void *owner{};
        };

        /** @brief Owner-thread admission evidence shared with native-resource-owning Scene candidates. */
        struct SceneCellAttachmentState final {
            SceneCellAttachmentContext context;
            W::CellAttachmentManifest manifest;
            std::vector<SceneCellAttachmentProvider> providers;
            std::vector<SceneCellAttachmentBytes> artifacts;
            std::shared_ptr<SceneCellAttachmentPublication> publication;
            bool closed{};
            bool replaced{};
        };
    }  // namespace Detail

    namespace {
        /** @brief Returns the explicitly selected exact feature implementation. */
        const SceneCellAttachmentProvider *Provider(const Detail::SceneCellAttachmentState &state, W::StreamingCellProvider provider) {
            const auto found = std::ranges::find(state.providers, provider, &SceneCellAttachmentProvider::provider);
            return found == state.providers.end() ? nullptr : std::to_address(found);
        }

        /** @brief Resolves one immutable artifact; never falls back to another revision or path. */
        const SceneCellAttachmentBytes *Artifact(const Detail::SceneCellAttachmentState &state, const Assets::AssetId &asset) {
            const auto found = std::ranges::find(state.artifacts, asset, &SceneCellAttachmentBytes::asset);
            return found == state.artifacts.end() ? nullptr : std::to_address(found);
        }

        /** @brief Validates exact provider schema and full artifact integrity before preparation. */
        Result<void> ReferenceReady(const Detail::SceneCellAttachmentState &state, const W::CellAttachmentReference &reference) {
            if (const auto *provider = Provider(state, reference.provider); !provider || provider->version != reference.version)
                return Result<void>::Failure(MakeError(W::CellAttachmentErrors::Unsupported));
            const auto *artifact = Artifact(state, reference.asset);
            if (!artifact)
                return Result<void>::Failure(MakeError(W::CellAttachmentErrors::NotReady));
            if (artifact->lease.Bytes().size() != reference.bytes || artifact->lease.Digest() != reference.digest)
                return Result<void>::Failure(MakeError(W::CellAttachmentErrors::Stale));
            return Result<void>::Success();
        }

        /** @brief Checks mandatory exact owner evidence and finite binding/byte capacity. */
        Result<void> ValidateContext(const Detail::SceneCellAttachmentState &state) {
            const auto &context = state.context;
            if (!context.scene.IsValid() || context.sceneRevision.value == 0 || !context.operation.IsValid() ||
                !context.manifestRevision.IsValid() || context.maximumProviders == 0 || context.maximumArtifacts == 0)
                return Result<void>::Failure(MakeError(W::CellAttachmentErrors::Invalid));
            if (context.operation != state.manifest.Operation() || context.manifestRevision != state.manifest.Revision())
                return Result<void>::Failure(MakeError(W::CellAttachmentErrors::Stale));
            if (state.providers.size() > context.maximumProviders || state.artifacts.size() > context.maximumArtifacts)
                return Result<void>::Failure(MakeError(W::CellAttachmentErrors::CapacityExceeded));
            return Result<void>::Success();
        }

        /** @brief Rejects duplicate/unknown providers and duplicate byte identities before domain work. */
        Result<void> ValidateBindings(Detail::SceneCellAttachmentState &state) {
            std::ranges::sort(state.providers, {}, &SceneCellAttachmentProvider::provider);
            std::ranges::sort(state.artifacts, {}, &SceneCellAttachmentBytes::asset);
            for (std::size_t index{}; index < state.providers.size(); ++index) {
                const auto &provider = state.providers[index];
                if (!W::IsCellAttachmentProvider(provider.provider) || !provider.identity.IsValid() || !provider.revision.IsValid() ||
                    provider.version == 0 || !provider.prepare)
                    return Result<void>::Failure(MakeError(W::CellAttachmentErrors::Invalid));
                for (std::size_t prior{}; prior < index; ++prior)
                    if (state.providers[prior].provider == provider.provider || state.providers[prior].identity == provider.identity)
                        return Result<void>::Failure(MakeError(W::CellAttachmentErrors::Invalid));
            }
            if (std::ranges::adjacent_find(state.artifacts, {}, &SceneCellAttachmentBytes::asset) != state.artifacts.end())
                return Result<void>::Failure(MakeError(W::CellAttachmentErrors::Invalid));
            for (const auto &artifact : state.artifacts)
                if (!artifact.asset.IsValid() || artifact.lease.Bytes().empty() ||
                    std::ranges::none_of(state.manifest.References(), [&](const auto &reference) {
                    return reference.asset == artifact.asset;
                }))
                    return Result<void>::Failure(MakeError(W::CellAttachmentErrors::Invalid));
            for (const auto &reference : state.manifest.References())
                if (reference.requirement == W::StreamingCellPayloadRequirement::Required)
                    if (const auto ready = ReferenceReady(state, reference); ready.HasError())
                        return ready;
            return Result<void>::Success();
        }

        /** @brief Builds complete replacement evidence off to the side before changing any live owner. */
        Result<std::shared_ptr<Detail::SceneCellAttachmentState>> MakeState(const SceneCellAttachmentContext &context,
                                                                            W::CellAttachmentManifest manifest,
                                                                            std::vector<SceneCellAttachmentProvider> providers,
                                                                            std::vector<SceneCellAttachmentBytes> artifacts) {
            auto state =
                std::make_shared<Detail::SceneCellAttachmentState>(context, std::move(manifest), std::move(providers), std::move(artifacts),
                                                                   std::make_shared<Detail::SceneCellAttachmentPublication>(), false,
                                                                   false);
            if (const auto valid = ValidateContext(*state); valid.HasError())
                return Result<std::shared_ptr<Detail::SceneCellAttachmentState>>::Failure(valid.ErrorValue());
            if (const auto valid = ValidateBindings(*state); valid.HasError())
                return Result<std::shared_ptr<Detail::SceneCellAttachmentState>>::Failure(valid.ErrorValue());
            return Result<std::shared_ptr<Detail::SceneCellAttachmentState>>::Success(std::move(state));
        }

        /** @brief Complete detached native candidate and byte pins participating in atomic Scene publication. */
        class Attachments final : public SceneActivationCandidate {
        public:
            explicit Attachments(std::shared_ptr<Detail::SceneCellAttachmentState> state) : state_(std::move(state)) {}

            // Publication identity is this candidate's address and cannot be copied or transferred.
            Attachments(const Attachments &) = delete;
            Attachments &operator=(const Attachments &) = delete;
            Attachments(Attachments &&) = delete;
            Attachments &operator=(Attachments &&) = delete;

            ~Attachments() override {
                Shutdown();
            }

            Result<void> ValidatePublication() const override {
                if (published_)
                    return Result<void>::Failure(MakeError(W::CellAttachmentErrors::Closed));
                if (state_->closed || closed_)
                    return Result<void>::Failure(
                        MakeError(state_->replaced ? W::CellAttachmentErrors::Stale : W::CellAttachmentErrors::Closed));
                for (const auto &entry : candidates_) {
                    auto &status = status_[entry.status];
                    if (!status.available)
                        continue;
                    if (const auto valid = entry.candidate->ValidatePublication(); valid.HasError()) {
                        if (status.reference.requirement == W::StreamingCellPayloadRequirement::Required)
                            return valid;
                        status.available = false;
                        status.cause = valid.ErrorValue();
                    }
                }
                return Result<void>::Success();
            }

            void Publish() noexcept override {
                if (published_ || closed_)
                    return;
                for (const auto &entry : candidates_) {
                    if (status_[entry.status].available)
                        entry.candidate->Publish();
                    else
                        entry.candidate->Shutdown();
                }
                state_->publication->active = std::move(status_);
                state_->publication->owner = this;
                published_ = true;
            }

            void Shutdown() noexcept override {
                if (closed_)
                    return;
                closed_ = true;
                for (auto iterator = candidates_.rbegin(); iterator != candidates_.rend(); ++iterator)
                    iterator->candidate->Shutdown();
                candidates_.clear();
                if (state_->publication->owner == this) {
                    state_->publication->active.clear();
                    state_->publication->owner = nullptr;
                }
            }

            Result<void> Prepare(const RuntimeSceneDefinition &definition, const RuntimeSceneView scene) {
                candidates_.reserve(state_->manifest.References().size());
                status_.reserve(state_->manifest.References().size());
                for (const auto &reference : state_->manifest.References()) {
                    if (const auto ready = ReferenceReady(*state_, reference); ready.HasError()) {
                        if (reference.requirement == W::StreamingCellPayloadRequirement::Required)
                            return ready;
                        status_.emplace_back(reference, false, ready.ErrorValue());
                        continue;
                    }
                    auto candidate = Provider(*state_, reference.provider)
                                         ->prepare(reference, Artifact(*state_, reference.asset)->lease, definition, scene);
                    if (candidate.HasValue() && !candidate.Value())
                        candidate = Result<std::unique_ptr<SceneActivationCandidate>>::Failure(MakeError(W::CellAttachmentErrors::Invalid));
                    if (candidate.HasValue()) {
                        if (const auto valid = candidate.Value()->ValidatePublication(); valid.HasError()) {
                            candidate.Value()->Shutdown();
                            candidate = Result<std::unique_ptr<SceneActivationCandidate>>::Failure(valid.ErrorValue());
                        }
                    }
                    if (candidate.HasError()) {
                        if (reference.requirement == W::StreamingCellPayloadRequirement::Required)
                            return Result<void>::Failure(candidate.ErrorValue());
                        status_.emplace_back(reference, false, candidate.ErrorValue());
                        continue;
                    }
                    status_.emplace_back(reference, true, std::nullopt);
                    candidates_.emplace_back(std::move(candidate).Value(), status_.size() - 1);
                }
                return ValidatePublication();
            }

        private:
            std::shared_ptr<Detail::SceneCellAttachmentState> state_;

            struct Entry final {
                std::unique_ptr<SceneActivationCandidate> candidate;
                std::size_t status{};
            };

            std::vector<Entry> candidates_;
            mutable std::vector<SceneCellAttachmentStatus> status_;
            bool closed_{};
            bool published_{};
        };
    }  // namespace

    /** @copydoc SceneCellAttachmentParticipant::SceneCellAttachmentParticipant */
    SceneCellAttachmentParticipant::SceneCellAttachmentParticipant(ConstructionKey,
                                                                   std::shared_ptr<Detail::SceneCellAttachmentState> state) noexcept
        : state_(std::move(state)) {}

    /** @copydoc SceneCellAttachmentParticipant::~SceneCellAttachmentParticipant */
    SceneCellAttachmentParticipant::~SceneCellAttachmentParticipant() {
        Shutdown();
    }

    /** @copydoc SceneCellAttachmentParticipant::Create */
    Result<std::unique_ptr<SceneCellAttachmentParticipant>> SceneCellAttachmentParticipant::Create(
        const SceneCellAttachmentContext &context, W::CellAttachmentManifest manifest, std::vector<SceneCellAttachmentProvider> providers,
        std::vector<SceneCellAttachmentBytes> artifacts) {
        auto state = MakeState(context, std::move(manifest), std::move(providers), std::move(artifacts));
        if (state.HasError())
            return Result<std::unique_ptr<SceneCellAttachmentParticipant>>::Failure(state.ErrorValue());
        return Result<std::unique_ptr<SceneCellAttachmentParticipant>>::Success(
            std::make_unique<SceneCellAttachmentParticipant>(ConstructionKey{}, std::move(state).Value()));
    }

    /** @copydoc SceneCellAttachmentParticipant::Replace */
    Result<void> SceneCellAttachmentParticipant::Replace(const SceneCellAttachmentContext &context, W::CellAttachmentManifest manifest,
                                                         std::vector<SceneCellAttachmentProvider> providers,
                                                         std::vector<SceneCellAttachmentBytes> artifacts) {
        if (state_->closed)
            return Result<void>::Failure(MakeError(W::CellAttachmentErrors::Closed));
        if (context.manifestRevision <= state_->context.manifestRevision ||
            context.operation.fence.partition != state_->context.operation.fence.partition ||
            context.operation.fence.epoch != state_->context.operation.fence.epoch)
            return Result<void>::Failure(MakeError(W::CellAttachmentErrors::Stale));
        auto successor = MakeState(context, std::move(manifest), std::move(providers), std::move(artifacts));
        if (successor.HasError())
            return Result<void>::Failure(successor.ErrorValue());
        successor.Value()->publication = state_->publication;
        state_->closed = true;
        state_->replaced = true;
        state_ = std::move(successor).Value();
        return Result<void>::Success();
    }

    /** @copydoc SceneCellAttachmentParticipant::Prepare */
    Result<std::unique_ptr<SceneActivationCandidate>> SceneCellAttachmentParticipant::Prepare(const RuntimeSceneDefinition &definition,
                                                                                              const RuntimeSceneView scene) {
        if (state_->closed)
            return Result<std::unique_ptr<SceneActivationCandidate>>::Failure(MakeError(W::CellAttachmentErrors::Closed));
        if (definition.Id() != state_->context.scene || definition.Revision() != state_->context.sceneRevision)
            return Result<std::unique_ptr<SceneActivationCandidate>>::Failure(MakeError(W::CellAttachmentErrors::Stale));
        auto candidate = std::make_unique<Attachments>(state_);
        if (const auto prepared = candidate->Prepare(definition, scene); prepared.HasError())
            return Result<std::unique_ptr<SceneActivationCandidate>>::Failure(prepared.ErrorValue());
        return Result<std::unique_ptr<SceneActivationCandidate>>::Success(std::move(candidate));
    }

    /** @copydoc SceneCellAttachmentParticipant::RequestCancellation */
    void SceneCellAttachmentParticipant::RequestCancellation() const noexcept {
        state_->closed = true;
    }

    /** @copydoc SceneCellAttachmentParticipant::Shutdown */
    void SceneCellAttachmentParticipant::Shutdown() const noexcept {
        state_->closed = true;
        state_->publication->active.clear();
    }

    /** @copydoc SceneCellAttachmentParticipant::ActiveStatus */
    std::span<const SceneCellAttachmentStatus> SceneCellAttachmentParticipant::ActiveStatus() const noexcept {
        return state_->publication->active;
    }
}  // namespace Horo::Runtime
