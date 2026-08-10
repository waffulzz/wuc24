// autorun.h — running the job in the background.
//
// The work must not hold up the console. The Wii U Menu has to stay responsive
// while several megabytes are downloaded, so the job runs on its own thread at
// a lower priority than everything else.
//
// The thread is cooperative about stopping: guard::RequestStop() asks it to
// wind up, and Stop() waits for it, giving any NAND write in flight a chance to
// finish rather than being cut off partway.
#pragma once

namespace autorun {

using Job = void (*)();

// Starts `job` on a background thread. Does nothing if one is already running.
bool Start(Job job);

// Asks the job to stop and waits up to `timeout_ms` for it. Returns true if the
// thread finished; false means it is still going and the process is about to
// take it down anyway -- which is what the recovery journal is for.
bool Stop(const char *reason, int timeout_ms);

bool Running();

}  // namespace autorun
