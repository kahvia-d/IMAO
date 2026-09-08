#pragma once
#include "Runtime/GamepadWorldActions.h"
#include <atomic>
#include <string>
#include <thread>
#include <vector>

inline void TestGamepadWorldActions(void (*check)(bool, const std::string&)) {
    using namespace std::chrono_literals;
    using Queue = GamepadWorldActions;
    using Action = Queue::Action;
    using Clock = Queue::Clock;
    const auto start = Clock::time_point{10s};
    GamepadContextSnapshot snapshot;
    snapshot.Begin(21, 123, 456);
    snapshot.ObserveUi(21, "local", false, false, true, true, start, 500ms, start);
    const auto gameplay = snapshot.Read("local", start);
    const Queue::Request complete{Action::CompleteCurrent, "local", gameplay.generation, 123};
    const Queue::Request guide{Action::ToggleGuide, "local", gameplay.generation, 123};

    Queue queue;
    check(queue.Enqueue(complete, gameplay, true, start) && queue.Enqueue(guide, gameplay, true, start + 1ms),
        "gamepad world queue accepts only fresh foreground gameplay intents");
    const auto first = queue.Take(gameplay, true, start + 50ms);
    const auto second = queue.Take(gameplay, true, start + 51ms);
    check(first && first->action == Action::CompleteCurrent && second && second->action == Action::ToggleGuide &&
        !queue.Take(gameplay, true, start + 52ms), "gamepad world queue preserves order and consumes each intent once");
    check(Queue::ParseAction("completeCurrent") == Action::CompleteCurrent &&
        Queue::ParseAction("toggleGuide") == Action::ToggleGuide && !Queue::ParseAction("completeAll") &&
        !Queue::ParseAction("CompleteCurrent"), "gamepad world action protocol accepts only its exact two semantic actions");

    for (const auto& change : {std::string("stopped"), std::string("unobservable"), std::string("map"),
        std::string("not-gameplay"), std::string("profile"), std::string("generation"), std::string("window"),
        std::string("session"), std::string("process")}) {
        queue.Clear();
        queue.Enqueue(complete, gameplay, true, start);
        auto changed = gameplay;
        if (change == "stopped") changed.running = false;
        else if (change == "unobservable") changed.observable = false;
        else if (change == "map") changed.bigMap = true;
        else if (change == "not-gameplay") changed.gameplay = false;
        else if (change == "profile") changed.profileId = "account-b";
        else if (change == "generation") ++changed.generation;
        else if (change == "window") ++changed.gameHwnd;
        else if (change == "session") ++changed.session;
        else ++changed.gameProcessId;
        check(!queue.Take(changed, true, start + 50ms) && !queue.Take(gameplay, true, start + 51ms),
            "gamepad queued world intent is permanently discarded after " + change + " changes");
        if (change != "session" && change != "process")
            check(!queue.Enqueue(complete, changed, true, start + 50ms),
                "gamepad world enqueue rejects mismatched or unavailable " + change + " context");
    }
    queue.Enqueue(complete, gameplay, true, start);
    check(!queue.Take(gameplay, false, start + 10ms) && !queue.Take(gameplay, true, start + 20ms) &&
        !queue.Enqueue(complete, gameplay, false, start + 20ms),
        "gamepad world focus loss cancels queued intents and cannot revive after refocus");

    queue.Enqueue(complete, gameplay, true, start);
    check(queue.Take(gameplay, true, start + 249ms).has_value(), "gamepad world intent may be consumed before its 250ms deadline");
    queue.Enqueue(complete, gameplay, true, start);
    check(!queue.Take(gameplay, true, start + 250ms), "gamepad world queue rejects intents at the 250ms expiry boundary");
    queue.Enqueue(complete, gameplay, true, start);
    check(!queue.Take(gameplay, true, start - 1ms), "gamepad world clock rollback discards pending input");
    queue.Enqueue(complete, gameplay, true, start + 300ms);
    check(!queue.Take(snapshot.Read("local", start + 500ms), true, start + 500ms),
        "gamepad fresh context read revokes a queued action after visual capture expires");

    auto malformed = complete; malformed.gameHwnd = 0;
    check(!queue.Enqueue(malformed, gameplay, true, start), "gamepad world request cannot omit the exact game window");
    malformed = complete; malformed.action = static_cast<Action>(999);
    check(!queue.Enqueue(malformed, gameplay, true, start), "gamepad world queue rejects an unknown action enum");

    queue.Clear();
    bool bounded = true;
    for (std::size_t index = 0; index < Queue::MaximumPending; ++index)
        bounded = bounded && queue.Enqueue(complete, gameplay, true, start);
    check(bounded && !queue.Enqueue(complete, gameplay, true, start),
        "gamepad world queue bounds IPC accumulation without silently replacing an accepted intent");
    const bool replaced = queue.Enqueue(guide, gameplay, true, start + 251ms);
    const auto replacement = queue.Take(gameplay, true, start + 252ms);
    check(replaced && replacement && replacement->action == Action::ToggleGuide &&
        !queue.Take(gameplay, true, start + 253ms),
        "gamepad world queue removes expired backlog before accepting new input");

    queue.Clear();
    std::atomic<int> accepted = 0, consumed = 0;
    std::vector<std::thread> writers;
    for (int index = 0; index < 4; ++index)
        writers.emplace_back([&] { for (int attempt = 0; attempt < 4; ++attempt)
            if (queue.Enqueue(complete, gameplay, true, start)) ++accepted; });
    for (auto& writer : writers) writer.join();
    std::thread consumerA([&] { while (queue.Take(gameplay, true, start + 50ms)) ++consumed; });
    std::thread consumerB([&] { while (queue.Take(gameplay, true, start + 50ms)) ++consumed; });
    consumerA.join(); consumerB.join();
    check(accepted == 16 && consumed == accepted && !queue.Take(gameplay, true, start + 50ms),
        "gamepad concurrent queue access neither duplicates nor loses accepted intents");
}
