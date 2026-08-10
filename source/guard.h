// guard.h — surviving the user walking away mid-job.
//
// The plugin runs while the Wii U Menu is up, and the user can launch vWii or a
// game, or pull the plug, at any moment. Almost everything the job does is
// harmless if interrupted: downloading, decoding and assembling containers all
// happen in memory. The one genuinely dangerous moment is the NAND write, where
// stopping halfway leaves a container neither old nor new.
//
// Two things protect that moment.
//
// A CANCELLATION FLAG, raised when the title is exiting. It is checked between
// units of work and, critically, immediately before a write begins: a write is
// never *started* once we know we are on the way out. A write already running
// is allowed to finish -- it takes a second or two, and abandoning it is the
// only way to actually cause damage.
//
// A JOURNAL on the SD card, written before a container is modified and removed
// once the new bytes are verified. Its presence at startup means a previous run
// was cut off mid-write, so the backup taken beforehand is restored before
// anything else happens. Power loss is the case this covers, since no amount of
// in-process care survives that.
#pragma once

#include <string>

class VwiiNand;

namespace guard {

// Raised when the running title is going away. `reason` is recorded so a log
// can say what asked, which matters when something requests a stop during boot
// and the job appears to give up for no reason.
void RequestStop(const char *reason);
const char *StopReason();
void ClearStop();
bool StopRequested();

// Marks a NAND write as being in progress. Exit handlers wait for this to clear
// (up to a limit) rather than letting the process die mid-write.
class CriticalSection {
public:
    CriticalSection();
    ~CriticalSection();
};
bool InCriticalSection();

// Blocks until no write is in flight, or `timeout_ms` passes. Returns true if
// it is safe to go.
bool WaitForCriticalSection(int timeout_ms);

// Records that `nand_path` is about to be overwritten and that `backup_name` on
// SD holds its previous contents.
bool OpenJournal(const std::string &nand_path, const std::string &backup_name);
void CloseJournal();

// If a journal is left over from a previous run, restore the container it names
// from its backup. Returns true if a recovery was performed.
bool RecoverIfNeeded(VwiiNand &nand);

}  // namespace guard
