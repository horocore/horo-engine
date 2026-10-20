#include "EditorNavigationBakeHost.h"

#include "Horo/Platform/NativeProjectIoOwners.h"
#include "NativeNavigationBakeComposition.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <map>

namespace Horo::Editor {
    namespace {
        namespace Nav = Navigation;
        using namespace Application;

        /** @brief Complete unpublished worker result; facade construction remains on its owner lane. */
        struct Prepared final {
            NavigationBakeApplicationConfig config;
            std::shared_ptr<const NavigationBakeCatalog> catalog;
        };

        /** @brief Copies the complete bounded authored choices without reducing them to surviving geometry. */
        Result<std::shared_ptr<const NavigationBakeCatalog>> Catalog(const NavigationProjectBakeSource &source) {
            using CatalogResult = Result<std::shared_ptr<const NavigationBakeCatalog>>;
            const auto definition = source.Settings().Input().definition;
            const auto found = std::ranges::find(source.Definitions(), definition, &NavigationDefinitionBakeSource::asset);
            if (found == source.Definitions().end())
                return CatalogResult::Failure(MakeError(Nav::NavigationErrors::BakeInputInvalid));
            auto catalog = std::make_shared<NavigationBakeCatalog>();
            catalog->definition = definition;
            catalog->profiles.assign(found->definition->Profiles().begin(), found->definition->Profiles().end());
            catalog->projectProfile = source.Settings().Input().runtimeProfile;
            for (const auto &scene : source.Scenes()) {
                const auto scope = Nav::NavigationBakeScopeId::ForScene(scene.asset.Bytes());
                if (scope.HasError())
                    return CatalogResult::Failure(scope.ErrorValue());
                for (const auto &object : scene.document.objects) {
                    const auto &surface = object.components.navigationSurface;
                    if (surface && surface->definition == definition) {
                        if (catalog->surfaces.size() >= 4096)
                            return CatalogResult::Failure(MakeError(Nav::NavigationErrors::CapacityExceeded));
                        catalog->surfaces.push_back({surface->id, scope.Value()});
                    }
                }
            }
            std::ranges::sort(catalog->surfaces);
            if (std::ranges::adjacent_find(catalog->surfaces) != catalog->surfaces.end())
                return CatalogResult::Failure(MakeError(Nav::NavigationErrors::SourceDuplicateIdentity));
            return CatalogResult::Success(std::move(catalog));
        }

        /** @brief Captures real canonical project choices and composes only the explicitly selected provider. */
        Result<Prepared> Prepare(const std::filesystem::path &root, const CancellationToken &token,
                                 const std::shared_ptr<std::atomic<bool>> &closed) {
            using Return = Result<Prepared>;
            const auto compatibility = InspectProjectCompatibility(root);
            if (!compatibility.metadata || compatibility.markerUpdateRequired ||
                (compatibility.status != ProjectCompatibilityStatus::Current &&
                 compatibility.status != ProjectCompatibilityStatus::CompatibleReleaseLine))
                return Return::Failure(compatibility.diagnostic.value_or(MakeError(Nav::NavigationErrors::BakeInputInvalid)));
            // Reading the project catalog grants no package execution or publication authority.
            auto reads = Platform::CreateNativeProjectIoOwners(root, compatibility.metadata->projectId, 1, ".horo/cache/navigation",
                                                               ".horo/cooked/navigation");
            if (reads.HasError())
                return Return::Failure(reads.ErrorValue());
            auto source = Application::Internal::CaptureNativeNavigationBakeSource(reads.Value().source, compatibility, closed,
                                                                                   {.cancellation = token});
            if (source.HasError())
                return Return::Failure(source.ErrorValue());
            auto catalog = Catalog(*source.Value());
            if (catalog.HasError())
                return Return::Failure(catalog.ErrorValue());
            const auto definition = catalog.Value()->definition;
            auto config = Application::Internal::ComposeNativeNavigationBake(root, definition, compatibility, closed);
            if (config.HasError())
                return Return::Failure(config.ErrorValue());
            return Return::Success({std::move(config).Value(), std::move(catalog).Value()});
        }
    }  // namespace

    struct EditorNavigationBakeHost::Impl final {
        struct Session final {
            std::optional<JobHandle> preparation;
            std::shared_ptr<std::optional<Result<Prepared>>> prepared;
            std::shared_ptr<std::atomic<bool>> closed{std::make_shared<std::atomic<bool>>(false)};
            std::unique_ptr<Application::NavigationBakeApplication> application;
            NavigationBakeWorkspaceSnapshot view;
            std::optional<Application::NavigationBakeCommandRequest> request;
            std::chrono::steady_clock::time_point started;
        };

        OperationStore &operations;
        JobSystem jobs{{.workerCount = 2, .maxQueuedJobs = 32, .ioWorkerCount = 1}};
        std::map<std::filesystem::path, Session> sessions;
        std::uint64_t nextOpening{1};

        /** @brief Queues preparation once per exact project root; errors are retained until an explicit reload. */
        Result<void> PrepareProject(const std::filesystem::path &root) {
            if (!root.is_absolute() || root != root.lexically_normal())
                return Result<void>::Failure(MakeError(Nav::NavigationErrors::BakeInputInvalid));
            if (sessions.contains(root))
                return Result<void>::Success();
            if (sessions.size() >= 8)
                return Result<void>::Failure(MakeError(Nav::NavigationErrors::CapacityExceeded));
            if (nextOpening == std::numeric_limits<std::uint64_t>::max())
                return Result<void>::Failure(MakeError(Nav::NavigationErrors::GenerationExhausted));
            Session session;
            session.view.opening = nextOpening++;
            session.prepared = std::make_shared<std::optional<Result<Prepared>>>();
            auto job = jobs.SubmitResult({.resource = JobResource::Io},
                                         [root, result = session.prepared, closed = session.closed](const CancellationToken &token) {
                result->emplace(Prepare(root, token, closed));
                return result->value().HasError() ? Result<void>::Failure(result->value().ErrorValue()) : Result<void>::Success();
            });
            if (job.HasError())
                return Result<void>::Failure(job.ErrorValue());
            session.preparation = std::move(job).Value();
            sessions.emplace(root, std::move(session));
            return Result<void>::Success();
        }

        /** @brief Explicitly refreshes saved choices only after accepted work has drained; old opening keys remain invalid. */
        Result<void> Reload(const std::filesystem::path &root) {
            const auto found = sessions.find(root);
            if (found != sessions.end()) {
                auto &session = found->second;
                if (session.preparation || (session.view.operation && !session.view.operation->IsTerminal()))
                    return Result<void>::Failure(MakeError(Nav::NavigationErrors::BakeJobAdmissionRejected));
                if (session.application) {
                    const auto closed = session.application->Close();
                    if (closed.HasError())
                        return closed;
                }
                session.closed->store(true);
                sessions.erase(found);
            }
            return PrepareProject(root);
        }

        /** @brief Starts through the sole shared application; accepted intent is retained only for exact-terminal retry. */
        Result<NavigationBakeOperationIdentity> Start(const std::filesystem::path &root,
                                                      Application::NavigationBakeCommandRequest request) {
            auto found = sessions.find(root);
            if (found == sessions.end() || !found->second.application)
                return Result<NavigationBakeOperationIdentity>::Failure(MakeError(Nav::NavigationErrors::BakeJobAdmissionRejected));
            auto &session = found->second;
            // The UI cannot smuggle an external invocation fence into the retained retry intent.
            request.invocation = {};
            auto started = session.application->Start(request);
            if (started.HasValue()) {
                session.request = std::move(request);
                session.started = std::chrono::steady_clock::now();
                session.view.elapsed = {};
                session.view.operation = session.application->Query(started.Value()).Value();
                session.view.diagnostic.reset();
            }
            return started.HasError() ? Result<NavigationBakeOperationIdentity>::Failure(started.ErrorValue())
                                      : Result<NavigationBakeOperationIdentity>::Success({session.view.opening, started.Value()});
        }

        /** @brief Retries only the retained terminal request of the exact application opening. */
        Result<NavigationBakeOperationIdentity> Retry(const std::filesystem::path &root, NavigationBakeOperationIdentity id) {
            const auto found = sessions.find(root);
            if (found == sessions.end() || !found->second.view.operation || found->second.view.opening != id.opening ||
                found->second.view.operation->id != id.command || !found->second.view.operation->IsTerminal() || !found->second.request)
                return Result<NavigationBakeOperationIdentity>::Failure(MakeError(Nav::NavigationErrors::InvalidHandle));
            return Start(root, *found->second.request);
        }

        /** @brief Adopts a drained worker result on the construction lane, retaining every preparation error. */
        void CompletePreparation(Session &session) {
            if (session.preparation) {
                const auto state = session.preparation->Snapshot();
                if (!state || !state->terminalResult)
                    return;
                session.preparation.reset();
                if (state->error || !session.prepared || !session.prepared->has_value()) {
                    session.view.diagnostic = state->error.value_or(MakeError(Nav::NavigationErrors::BakeInputInvalid));
                } else {
                    auto prepared = std::move(session.prepared->value());
                    if (prepared.HasError())
                        session.view.diagnostic = prepared.ErrorValue();
                    else {
                        session.view.budget = prepared.Value().config.service.budget;
                        auto application =
                            Application::NavigationBakeApplication::Create(std::move(prepared.Value().config), operations, jobs);
                        if (application.HasError())
                            session.view.diagnostic = application.ErrorValue();
                        else {
                            session.application = std::move(application).Value();
                            session.view.catalog = std::move(prepared.Value().catalog);
                        }
                    }
                }
                session.prepared.reset();
            }
        }

        /** @brief Projects actual owner state and freezes elapsed time at terminal observation. */
        void Observe(Session &session) {
            if (!session.view.operation || session.view.operation->IsTerminal())
                return;
            static_cast<void>(session.application->Pump());
            if (!session.view.operation->IsTerminal())
                session.view.elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - session.started);
            auto result = session.application->Query(session.view.operation->id);
            if (result.HasValue())
                session.view.operation = std::move(result).Value();
            else
                session.view.diagnostic = result.ErrorValue();
        }
    };

    /** @copydoc EditorNavigationBakeHost::EditorNavigationBakeHost */
    EditorNavigationBakeHost::EditorNavigationBakeHost(OperationStore &operations) : impl_(std::make_unique<Impl>(operations)) {}

    /** @copydoc EditorNavigationBakeHost::~EditorNavigationBakeHost */
    EditorNavigationBakeHost::~EditorNavigationBakeHost() {
        for (auto &[root, session] : impl_->sessions) {
            session.closed->store(true);
            if (session.application)
                static_cast<void>(session.application->Close());
        }
        impl_->jobs.Shutdown(ShutdownPolicy::Cancel);
        for (auto &[root, session] : impl_->sessions)
            if (session.application) {
                static_cast<void>(session.application->Pump());
                if (session.application->Close().HasError())
                    std::terminate();
            }
    }

    /** @copydoc EditorNavigationBakeHost::Pump */
    void EditorNavigationBakeHost::Pump() {
        for (auto &[root, session] : impl_->sessions) {
            impl_->CompletePreparation(session);
            if (session.application)
                impl_->Observe(session);
        }
    }

    /** @copydoc EditorNavigationBakeHost::Access */
    NavigationBakeWorkspaceAccess EditorNavigationBakeHost::Access() {
        return {.prepare =
                    [this](const auto &root) {
            return impl_->PrepareProject(root);
        },
                .reload =
                    [this](const auto &root) {
            return impl_->Reload(root);
        },
                .query = [this](const std::filesystem::path &root) -> Result<NavigationBakeWorkspaceSnapshot> {
            const auto found = impl_->sessions.find(root);
            return found == impl_->sessions.end()
                       ? Result<NavigationBakeWorkspaceSnapshot>::Failure(MakeError(Nav::NavigationErrors::InvalidHandle))
                       : Result<NavigationBakeWorkspaceSnapshot>::Success(found->second.view);
        },
                .start =
                    [this](const auto &root, auto request) {
            return impl_->Start(root, std::move(request));
        },
                .retry = [this](const std::filesystem::path &root,
                                NavigationBakeOperationIdentity id) -> Result<NavigationBakeOperationIdentity> {
            return impl_->Retry(root, id);
        },
                .cancel = [this](const std::filesystem::path &root, NavigationBakeOperationIdentity id) -> Result<void> {
            const auto found = impl_->sessions.find(root);
            if (found == impl_->sessions.end() || !found->second.application || found->second.view.opening != id.opening)
                return Result<void>::Failure(MakeError(Nav::NavigationErrors::InvalidHandle));
            return found->second.application->RequestCancel(id.command);
        }};
    }
}  // namespace Horo::Editor
