#pragma once

// Who queues story manager events. Quests started by an event run later on a worker thread, so
// the code that caused the event (a mod moving the player, the engine changing location) is not
// on the crash stack; this keeps the stacks from when the events were queued.
namespace StaleParentCellFix::Events
{
    // stressPurge: after each Change Location event, purge the buffered (unloaded) cells on the
    // main thread, as the `pcb` console command does, so cells die while the quests it starts
    // are being filled. For reproducing the crash only.
    void Install(bool stressPurge);

    // The last few Change Location (CLOC) events with their queueing stacks; empty if none or
    // not installed.
    std::string Recent();
}
