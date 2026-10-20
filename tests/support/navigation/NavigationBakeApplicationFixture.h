#pragma once

/** @file NavigationBakeApplicationFixture.h
 * @brief Real retained-project and native service owners shared by application and adapter regressions.
 */
#include "Horo/Application/NavigationBakeApplication.h"
#include "navigation/NavigationProjectBakeNativeFixture.h"

#include <chrono>
#include <thread>

namespace Horo::Application::NavigationApplicationTestSupport {
    namespace Nav = Navigation;

    /** @brief Owns actual project capture, native tile building and teardown even when an assertion unwinds. */
    struct Session final {
        NavigationInputTestSupport::Project project;
        std::shared_ptr<BakeTestSupport::ControlledBuilder> builder{std::make_shared<BakeTestSupport::ControlledBuilder>()};
        NavigationBakeServiceConfig service{NavigationInputNativeTestSupport::Config(project, builder)};
        OperationStore operations{8, 16};
        JobSystem jobs{{.workerCount = 2, .maxQueuedJobs = 16, .maxRetainedTerminalJobs = 32, .ioWorkerCount = 1}};
        std::unique_ptr<NavigationBakeApplication> application;

        Session() {
            const auto files = project.source.files;
            const auto context = project.source.Context();
            const auto compatibility = project.source.Compatibility();
            auto created =
                NavigationBakeApplication::Create({.service = service,
                                                   .compatibility = Nav::TestSupport::IncrementalBakeFixture{}.compatibility,
                                                   .captureSource =
                                                       [files, context, compatibility](const NavigationBakeInvocation &operation) {
                auto invocation = context;
                invocation.cancellation = operation.cancellation;
                invocation.deadline = std::min(invocation.deadline, operation.deadline);
                invocation.authorityStopped = operation.authorityStopped;
                return NavigationProjectBakeSource::Capture(files, std::move(invocation), compatibility);
            }},
                                                  operations, jobs);
            REQUIRE(created.HasValue());
            application = std::move(created).Value();
        }

        ~Session() {
            if (application) {
                static_cast<void>(application->Close());
                jobs.Shutdown(ShutdownPolicy::Cancel);
                static_cast<void>(application->Pump());
                if (application->Close().HasError())
                    std::terminate();
            }
        }

        /** @brief Observes only this operation with a finite tooling deadline. */
        NavigationBakeCommandSnapshot Finish(std::uint64_t id) const {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
            while (std::chrono::steady_clock::now() < deadline) {
                REQUIRE(application->Pump().HasValue());
                const auto snapshot = application->Query(id);
                REQUIRE(snapshot.HasValue());
                if (snapshot.Value().IsTerminal())
                    return snapshot.Value();
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            FAIL("Navigation application did not drain within its test deadline");
            return {};
        }

        /** @brief Admits an exact command and observes its authoritative terminal result. */
        NavigationBakeCommandSnapshot Execute(Nav::NavigationBakeExecutionMode mode) const {
            const auto started = application->Start({.execution = mode});
            REQUIRE(started.HasValue());
            return Finish(started.Value());
        }
    };

}  // namespace Horo::Application::NavigationApplicationTestSupport
