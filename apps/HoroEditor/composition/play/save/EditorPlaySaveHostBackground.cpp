#include "EditorPlaySaveHostInternal.h"
#include "Horo/Foundation/Logging/Logger.h"

namespace Horo::Application::Internal {
    /** @copydoc PlaySaveGeneration::PlaySaveGeneration */
    PlaySaveGeneration::PlaySaveGeneration(Runtime::RuntimeSceneService &scenes, std::uint64_t session, std::uint64_t registry) noexcept
        : scenes_(scenes), session_(session), registry_(registry) {}

    /** @copydoc Runtime::ISceneRestoreGenerationAuthority::Current */
    Runtime::StagedRestoreActivationEvidence PlaySaveGeneration::Current() const noexcept {
        const auto current = scenes_.ActiveScene();
        return {registry_, session_, current ? current->RuntimeId().value : 0};
    }

    /** @copydoc PlaySaveGeneration::Generation */
    Runtime::SaveRuntimeGeneration PlaySaveGeneration::Generation() const noexcept {
        const auto current = Current();
        return {current.sessionGeneration, current.sceneIncarnation, current.registryGeneration};
    }

    /** @copydoc PlaySaveJobSettled */
    bool PlaySaveJobSettled(const JobHandle &job) {
        using enum Horo::JobState;
        const auto current = job.Snapshot();
        return job.Id() == 0 || (current && (current->state == Succeeded || current->state == Failed || current->state == Cancelled));
    }

    namespace {
        /** @brief Mutates only the exact selected sandbox through ordinary lifecycle consent and cancellation checks. */
        Result<void> ApplyStorageAction(PlaySaveStorageAction action, PlaySaveStorage &storage,
                                        const std::optional<Runtime::SaveManagerCommand> &selection,
                                        const CancellationToken &cancellation) {
            if (action == PlaySaveStorageAction::Cleanup)
                return storage.Cleanup();
            if (action != PlaySaveStorageAction::Delete)
                return Result<void>::Success();
            if (!selection)
                return Result<void>::Failure(MakeError(Runtime::SaveErrors::CompositionUnsupported));
            const auto deleted = storage.Delete(*selection, cancellation);
            return deleted.HasError() ? Result<void>::Failure(deleted.ErrorValue()) : Result<void>::Success();
        }
    }  // namespace

    /** @copydoc RunPlaySaveStorageAction */
    Result<PlaySaveStorageUpdate> RunPlaySaveStorageAction(PlaySaveStorageAction action, PlaySaveStorageConfig config,
                                                           std::shared_ptr<const PlaySaveStorageSchema> schema,
                                                           std::shared_ptr<PlaySaveStorage> storage,
                                                           std::optional<Runtime::SaveManagerCommand> selection,
                                                           const CancellationToken &cancellation) {
        using Outcome = Result<PlaySaveStorageUpdate>;
        if (cancellation.IsCancellationRequested())
            return Outcome::Failure(MakeError(Runtime::SaveErrors::OperationCancelled));
        if (action == PlaySaveStorageAction::Open) {
            auto opened = PlaySaveStorage::Open(std::move(config), std::move(schema));
            if (opened.HasError())
                return Outcome::Failure(opened.ErrorValue());
            storage = std::move(opened).Value();
        }
        if (!storage)
            return Outcome::Failure(MakeError(Runtime::SaveErrors::LifecycleUnavailable));
        if (const auto applied = ApplyStorageAction(action, *storage, selection, cancellation); applied.HasError())
            return Outcome::Failure(applied.ErrorValue());
        auto catalog = storage->List();
        if (catalog.HasError())
            return Outcome::Failure(catalog.ErrorValue());
        return Outcome::Success({std::move(storage), std::move(catalog).Value()});
    }

    /** @copydoc EditorPlaySaveHost::State::StartStorage */
    Result<void> EditorPlaySaveHost::State::StartStorage(PlaySaveStorageAction action,
                                                         std::optional<Runtime::SaveManagerCommand> selection) {
        if (storageWork)
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::OperationInProgress));
        auto work = std::make_shared<PlaySaveStorageWork>();
        const JobDescriptor descriptor{.parentCancellation =
                                           action == PlaySaveStorageAction::Cleanup ? CancellationToken{} : cancellation.Token()};
        auto submitted =
            environment.jobs.SubmitResult(descriptor, [work, action, config = environment.storage, schema = schema, storage = storage,
                                                       selection = std::move(selection)](const CancellationToken &token) mutable {
            auto result =
                RunPlaySaveStorageAction(action, std::move(config), std::move(schema), std::move(storage), std::move(selection), token);
            const auto failed = result.HasError() ? std::optional<Error>{result.ErrorValue()} : std::nullopt;
            {
                std::lock_guard lock(work->publication);
                work->result.emplace(std::move(result));
            }
            return failed ? Result<void>::Failure(*failed) : Result<void>::Success();
        });
        if (submitted.HasError())
            return Result<void>::Failure(submitted.ErrorValue());
        storageAction = action;
        storageWork = std::move(work);
        storageJob = std::move(submitted).Value();
        dirty = true;
        return Result<void>::Success();
    }

    /** @copydoc EditorPlaySaveHost::State::PumpStorage */
    Result<void> EditorPlaySaveHost::State::PumpStorage() {
        if (!storageWork || !PlaySaveJobSettled(storageJob))
            return Result<void>::Success();
        auto work = std::move(storageWork);
        const auto job = storageJob.Snapshot();
        if (storageAction == PlaySaveStorageAction::Cleanup && storage)
            storage->CloseAdmission();
        dirty = true;
        const auto consumed = ConsumeStorage(*work, job);
        if (consumed.HasError() && storageAction == PlaySaveStorageAction::Cleanup)
            LOG_WARN("editor.play.save", "Sandbox cleanup failed: %s", consumed.ErrorValue().message.c_str());
        return consumed;
    }

    /** @copydoc EditorPlaySaveHost::State::ConsumeStorage */
    Result<void> EditorPlaySaveHost::State::ConsumeStorage(PlaySaveStorageWork &work, const std::optional<JobSnapshot> &job) {
        std::lock_guard lock(work.publication);
        if (!work.result) {
            if (job && job->terminalResult && job->terminalResult->error)
                return Result<void>::Failure(*job->terminalResult->error);
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::OperationCancelled));
        }
        if (work.result->HasError())
            return Result<void>::Failure(work.result->ErrorValue());
        auto update = std::move(*work.result).Value();
        if (storageAction == PlaySaveStorageAction::Cleanup)
            update.storage->CloseAdmission();
        storage = std::move(update.storage);
        catalog.emplace(std::move(update.catalog));
        return Result<void>::Success();
    }
}  // namespace Horo::Application::Internal
