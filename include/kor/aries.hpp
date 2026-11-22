#pragma once

// Phase 2 - Durability: ARIES Three-Phase Recovery
//
// Formalises the three ARIES recovery phases that the Kor WAL already
// implements correctly.  The class is a thin coordinator that calls back
// into the existing recovery helpers (apply_record, restore_checkpoint)
// that live in storage.cpp, keeping the ARIES logic separate and testable.
//
// Algorithm:
//   1. ANALYSIS  - read WAL header + checkpoint, establish redo window
//                  [checkpoint_lsn+1 .. committed_lsn].
//   2. REDO      - replay every committed WAL record in LSN order via
//                  the existing apply_record path.
//   3. UNDO      - no uncommitted records survive a WAL checkpoint; no-op.
//
// Recovery guarantees:
//   - Every fsync-acked committed write is replayed exactly once.
//   - No uncommitted write is ever visible after recovery.
//   - Idempotent: safe on a fully consistent database (zero records → done).

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "kor/hash_index.hpp"
#include "kor/memory_tracker.hpp"
#include "kor/secondary_index.hpp"
#include "kor/snapshot.hpp"
#include "kor/wal.hpp"

namespace kor {

struct ARIESStats {
    std::uint64_t checkpoint_lsn   = 0;  ///< LSN of the last valid checkpoint
    std::uint64_t committed_lsn    = 0;  ///< Highest committed LSN in the WAL
    std::uint64_t records_replayed = 0;  ///< Records applied during REDO
    std::uint64_t records_undone   = 0;  ///< Records undone (always 0 — no-op)
    bool          checkpoint_present = false;
};

/// ARIES three-phase recovery coordinator.
///
/// Callers supply two callbacks that match the anonymous-namespace helpers in
/// storage.cpp so that aries.cpp does not duplicate their logic:
///
///   apply_record_fn      — applies one WAL record to index + secondary indexes
///   restore_checkpoint_fn — restores a checkpoint snapshot into the index
///
/// Usage:
///   ARIESRecovery r(wal, index, tracker, secondary_indexes,
///                   apply_record, restore_checkpoint);
///   auto stats = r.recover();
class ARIESRecovery {
public:
    using ApplyFn = std::function<void(HashIndex&,
                                       MemoryTracker&,
                                       SecondaryIndexManager&,
                                       const WalRecord&)>;
    using RestoreCheckpointFn = std::function<void(HashIndex&,
                                                    MemoryTracker&,
                                                    SecondaryIndexManager&,
                                                    CheckpointData&)>;

    ARIESRecovery(WalLog&              wal,
                  HashIndex&           index,
                  MemoryTracker&       tracker,
                  SecondaryIndexManager& secondary_indexes,
                  ApplyFn              apply_record_fn,
                  RestoreCheckpointFn  restore_checkpoint_fn) noexcept;

    /// Run all three recovery phases.  Throws RecoveryError on corruption.
    ARIESStats recover();

private:
    ARIESStats analyse();
    void       redo(ARIESStats& stats);
    void       undo(ARIESStats& stats) noexcept;

    WalLog&                wal_;
    HashIndex&             index_;
    MemoryTracker&         tracker_;
    SecondaryIndexManager& secondary_indexes_;
    ApplyFn                apply_record_fn_;
    RestoreCheckpointFn    restore_checkpoint_fn_;
};

}  // namespace kor
