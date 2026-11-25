#include "NpcDialogueHttpsProvider.h"

#include <array>
#include <chrono>
#include <string_view>
#include <thread>

using namespace Horo;
using namespace Horo::AI;
using namespace std::chrono_literals;

namespace {
    /** @brief Waits only within the test budget for the owner-lane result. */
    int CheckCue(NpcDialogueJob &job, const NpcDialogueRequestIdentity &owner, const NpcDialogueGrant &grant,
                 std::chrono::steady_clock::time_point stop, NpcDialogueCueId expected) {
        while (std::chrono::steady_clock::now() < stop) {
            if (const auto outcome = job.Poll(owner, grant, 2))
                return outcome->cue == expected ? 0 : 7;
            std::this_thread::sleep_for(1ms);
        }
        return 8;
    }
}  // namespace

int main(int argc, char **argv) {
    if (argc != 4)
        return 2;
    const NpcDialogueRoute route{DecisionProviderId::Create(1).Value(), NpcDialogueDestinationId::Create(2).Value(), 1};
    auto binding = App::MakeNpcDialogueHttpsProvider({route, 1, argv[1], argv[2]});
    const std::string_view mode{argv[3]};
    if (mode == "invalid")
        return binding.HasError() ? 0 : 3;
    if (binding.HasError())
        return 4;

    const NpcDialogueRequestIdentity owner{{AiRuntimeIncarnation::Create(8).Value(), {1, 1}}, 1, 3};
    const std::array allowed{NpcDialogueFactId::Create(2).Value()};
    const NpcDialogueGrant grant{route, 1, allowed};
    const std::array facts{NpcDialogueFact{NpcDialogueFactId::Create(1).Value(), NpcDialogueValueId::Create(1234).Value()},
                           NpcDialogueFact{allowed.front(), NpcDialogueValueId::Create(20).Value()}};
    const std::array cues{NpcDialogueCueId::Create(4).Value(), NpcDialogueCueId::Create(5).Value()};
    auto turn = NpcDialogueTurn::Capture(owner, route, grant, facts, cues, cues.front(), 10);
    if (turn.HasError())
        return 5;
    JobSystem jobs({.ioWorkerCount = 1});
    const auto deadline = std::chrono::steady_clock::now() + (mode == "timeout" ? 500ms : 5s);
    auto queued = NpcDialogueJob::Start(jobs, std::move(turn).Value(), owner, grant, binding.Value(), 1, deadline, {});
    if (queued.HasError())
        return 6;
    auto job = std::move(queued).Value();
    const auto expected = mode == "approved" ? cues.back() : cues.front();
    return CheckCue(job, owner, grant, deadline + 1s, expected);
}
