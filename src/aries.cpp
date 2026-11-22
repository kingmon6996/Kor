#include "kor/aries.hpp"

#include "kor/exceptions.hpp"
#include "kor/serializer.hpp"
#include "kor/snapshot.hpp"

namespace kor {

ARIESRecovery::ARIESRecovery(WalLog&                wal,
                              HashIndex&             index,
                              MemoryTracker&         tracker,
                              SecondaryIndexManager& secondary_indexes,
                              ApplyFn                apply_record_fn,
                              RestoreCheckpointFn    restore_checkpoint_fn) noexcept
    : wal_(wal),
      index_(index),
      tracker_(tracker),
      secondary_indexes_(secondary_indexes),
      apply_record_fn_(std::move(apply_record_fn)),
      restore_checkpoint_fn_(std::move(restore_checkpoint_fn)) {}

ARIESStats ARIESRecovery::recover() {
    // Phase 1 — Analysis
    auto stats = analyse();

    // Phase 2 — Redo
    redo(stats);

    // Phase 3 — Undo (no-op; see header comment)
    undo(stats);

    return stats;
}

ARIESStats ARIESRecovery::analyse() {
    ARIESStats stats;
    const auto ws         = wal_.stats();
    stats.checkpoint_lsn  = ws.checkpoint_lsn;
    stats.committed_lsn   = ws.committed_lsn;
    stats.checkpoint_present = ws.checkpoint_present;

    // Load the checkpoint snapshot (if any) into the in-memory index,
    // bringing state to checkpoint_lsn.  Subsequent REDO brings it to
    // committed_lsn.
    if (auto cp = wal_.read_checkpoint(); cp.has_value()) {
        restore_checkpoint_fn_(index_, tracker_, secondary_indexes_, *cp);
    }
    return stats;
}

void ARIESRecovery::redo(ARIESStats& stats) {
    // replay_each iterates every committed WAL record after checkpoint_lsn,
    // verifying LSN monotonicity and CRC32 on every record.
    // apply_record_fn applies each record to the in-memory index.
    wal_.replay_each([&](const WalRecord& record) {
        apply_record_fn_(index_, tracker_, secondary_indexes_, record);
        ++stats.records_replayed;
    });
}

void ARIESRecovery::undo(ARIESStats& stats) noexcept {
    // The WAL only persists a record after a double-fsync confirms durability.
    // Any write that did not complete fsync is not in [checkpoint_lsn+1 ..
    // committed_lsn] and is never visited by replay_each().
    // Therefore there are no uncommitted records to roll back.
    stats.records_undone = 0;
}

}  // namespace kor
