// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
#ifndef YB_TABLET_MVCC_H_
#define YB_TABLET_MVCC_H_

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <gtest/gtest_prod.h>

#include "yb/gutil/gscoped_ptr.h"
#include "yb/server/clock.h"
#include "yb/util/locks.h"

namespace yb {
class CountDownLatch;
namespace tablet {
class MvccManager;

using std::string;

// A snapshot of the current MVCC state, which can determine whether a transaction ID (represented
// as a timestamp unique within a tablet) should be considered visible.
class MvccSnapshot {
 public:
  MvccSnapshot();

  // Create a snapshot with the current state of the given manager
  explicit MvccSnapshot(const MvccManager &manager);

  // Create a snapshot at a specific Timestamp.
  //
  // This snapshot considers all transactions timestamps lower than the provided 'timestamp' to be
  // committed, and all other transactions to be uncommitted.
  explicit MvccSnapshot(const Timestamp& timestamp);

  // Create a snapshot which considers all transactions as committed.
  // This is mostly useful in test contexts.
  static MvccSnapshot CreateSnapshotIncludingAllTransactions();

  // Creates a snapshot which considers no transactions committed.
  static MvccSnapshot CreateSnapshotIncludingNoTransactions();

  // Return true if the given transaction ID should be considered committed
  // in this snapshot.
  inline bool IsCommitted(const Timestamp& timestamp) const {
    // Inline the most likely path, in which our watermarks determine
    // whether a transaction is committed.
    if (PREDICT_TRUE(timestamp.CompareTo(all_committed_before_) < 0)) {
      return true;
    }
    if (PREDICT_TRUE(timestamp.CompareTo(none_committed_at_or_after_) >= 0)) {
      return false;
    }
    // Out-of-line the unlikely case which involves more complex (loopy) code.
    return IsCommittedFallback(timestamp);
  }

  // Returns true if this snapshot may have any committed transactions with ID
  // equal to or higher than the provided 'timestamp'.
  //
  // Kudu-specific:
  // If MayHaveCommittedTransactionsAtOrAfter(delta_stats.min) returns true
  // it means that there might be transactions that need to be applied in the
  // context of this snapshot; otherwise no scanning is necessary.
  // This is mostly useful to avoid scanning REDO deltas in certain cases.
  bool MayHaveCommittedTransactionsAtOrAfter(const Timestamp& timestamp) const;

  // Returns true if this snapshot may have any uncommitted transactions with ID
  // equal to or lower than the provided 'timestamp'.
  //
  // Kudu-specific:
  // This is mostly useful to avoid scanning UNDO deltas in certain cases.
  // If MayHaveUncommittedTransactionsAtOrBefore(delta_stats.max) returns false it
  // means that all UNDO delta transactions are committed in the context of this
  // snapshot and no scanning is necessary; otherwise there might be some
  // transactions that need to be undone.
  bool MayHaveUncommittedTransactionsAtOrBefore(const Timestamp& timestamp) const;

  // Return a string representation of the set of committed transactions
  // in this snapshot, suitable for debug printouts.
  string ToString() const;

  // Return true if the snapshot is considered 'clean'. A clean snapshot is one
  // which is determined only by a timestamp -- the snapshot considers all
  // transactions with timestamps less than some timestamp to be committed,
  // and all other transactions to be uncommitted.
  bool is_clean() const {
    return committed_timestamps_.empty();
  }

  // Consider the given list of timestamps to be committed in this snapshot,
  // even if they weren't when the snapshot was constructed.
  // This is used in the flush path, where the set of commits going into a
  // flushed file may not be a consistent snapshot from the MVCC point of view,
  // yet we need to construct a scanner that accurately represents that set.
  void AddCommittedTimestamps(const std::vector<Timestamp>& timestamps);

  // We use this to translate between Kudu's MVCC snapshots and timestamps needed for reading from
  // DocDB. This assumes the snapshot is "clean" (no "holes" in the set of committed timestamps).
  // We should gradually get rid of the need for handling dirty snapshots in YB, because we are
  // making sure timestamps increase monotonically along with Raft indexes, at least in the absence
  // of cross-shard transactions, and we always commit/apply REPLICATE messages in the Raft log
  // order.
  Timestamp LastCommittedTimestamp() const {
    if (!is_clean()) {
      if (committed_timestamps_.size() == 1 &&
          all_committed_before_.value() == committed_timestamps_.front()) {
        // This is a degenerate case of a dirty snapshot that is in fact clean, consiting of all
        // timestamps less than X and the set {X}, e.g.:
        // MvccSnapshot[committed={T|T < 6041797920884666368 or (T in {6041797920884666368})}]
        return all_committed_before_;
      }
      LOG(WARNING) << __func__ << " called on a dirty snapshot: " << ToString();
    }
    return Timestamp(all_committed_before_.value() - 1);
  }

 private:
  friend class MvccManager;
  FRIEND_TEST(MvccTest, TestMayHaveCommittedTransactionsAtOrAfter);
  FRIEND_TEST(MvccTest, TestMayHaveUncommittedTransactionsBefore);
  FRIEND_TEST(MvccTest, TestWaitUntilAllCommitted_SnapAtTimestampWithInFlights);

  bool IsCommittedFallback(const Timestamp& timestamp) const;

  void AddCommittedTimestamp(Timestamp timestamp);

  // Summary rule:
  //   A transaction T is committed if and only if:
  //      T < all_committed_before_ or
  //   or committed_timestamps_.contains(T)
  //
  // In ASCII form, where 'C' represents a committed transaction,
  // and 'U' represents an uncommitted one:
  //
  //   CCCCCCCCCCCCCCCCCUUUUUCUUUCU
  //                    |    \___\___ committed_timestamps_
  //                    |
  //                    \- all_committed_before_


  // A transaction ID below which all transactions have been committed.
  // For any timestamp X, if X < all_committed_timestamp_, then X is committed.
  Timestamp all_committed_before_;

  // A transaction ID at or beyond which no transactions have been committed.
  // For any timestamp X, if X >= none_committed_after_, then X is uncommitted.
  // This is equivalent to max(committed_timestamps_) + 1, but since
  // that vector is unsorted, we cache it.
  Timestamp none_committed_at_or_after_;

  // The set of transactions higher than all_committed_before_timestamp_ which
  // are committed in this snapshot.
  // It might seem like using an unordered_set<> or a set<> would be faster here,
  // but in practice, this list tends to be stay pretty small, and is only
  // rarely consulted (most data will be culled by 'all_committed_before_'
  // or none_committed_at_or_after_. So, using the compact vector structure fits
  // the whole thing on one or two cache lines, and it ends up going faster.
  std::vector<Timestamp::val_type> committed_timestamps_;

};

// Coordinator of MVCC transactions. Threads wishing to make updates use
// the MvccManager to obtain a unique timestamp, usually through the ScopedTransaction
// class defined below.
//
// MVCC is used to defer updates until commit time, and allow iterators to
// operate on a snapshot which contains only committed transactions.
//
// There are two valid paths for a transaction:
//
// 1) StartTransaction() -> StartApplyingTransaction() -> CommitTransaction()
//   or
// 2) StartTransaction() -> AbortTransaction()
//
// When a transaction is started, a timestamp is assigned. The manager will
// never assign a timestamp if there is already another transaction with
// the same timestamp in flight or previously committed.
//
// When a transaction is ready to start making changes to in-memory data,
// it should transition to APPLYING state by calling StartApplyingTransaction().
// At this point, the transaction should apply its in-memory operations and
// must commit in a bounded amount of time (i.e it should not wait on external
// input such as an RPC from another host).
//
// NOTE: we do not support "rollback" of in-memory edits. Thus, once we call
// StartApplyingTransaction(), the transaction _must_ commit.
//
class MvccManager {
 public:
  explicit MvccManager(const scoped_refptr<server::Clock>& clock);

  // Begin a new transaction, assigning it a transaction ID.
  // Callers should generally prefer using the ScopedTransaction class defined
  // below, which will automatically finish the transaction when it goes out
  // of scope.
  Timestamp StartTransaction();

  // The same as the above but but starts the transaction at the latest possible
  // time, i.e. now + max_error. Returns Timestamp::kInvalidTimestamp if it was
  // not possible to obtain the latest time.
  Timestamp StartTransactionAtLatest();

  // Begins a new transaction, which is assigned the provided timestamp.
  // Returns Status::OK() if the transaction was started successfully or
  // Status::IllegalState() if the provided timestamp is already considered
  // committed, e.g. if timestamp < 'all_committed_before_'.
  CHECKED_STATUS StartTransactionAtTimestamp(Timestamp timestamp);

  // Mark that the transaction with the given timestamp is starting to apply
  // its writes to in-memory stores. This must be called before CommitTransaction().
  // If this is called, then AbortTransaction(timestamp) must never be called.
  void StartApplyingTransaction(Timestamp timestamp);

  // Commit the given transaction.
  //
  // If the transaction is not currently in-flight, this will trigger an
  // assertion error. It is an error to commit the same transaction more
  // than once.
  //
  // This should be used for 'true' online transaction processing on LEADER
  // replicas and not for delayed processing on FOLLOWER/LEARNER replicas or
  // on bootstrap, as this advances 'all_committed_before_' to clock_->Now()
  // when possible.
  //
  // The transaction must already have been marked as 'APPLYING' by calling
  // StartApplyingTransaction(), or else this logs a FATAL error.
  void CommitTransaction(Timestamp timestamp);

  // Abort the given transaction.
  //
  // If the transaction is not currently in-flight, this will trigger an
  // assertion error. It is an error to abort the same transaction more
  // than once.
  //
  // This makes sure that the transaction with 'timestamp' is removed from
  // the in-flight set but without advancing the safe time since a new
  // transaction with a lower timestamp might be executed later.
  //
  // The transaction must not have been marked as 'APPLYING' by calling
  // StartApplyingTransaction(), or else this logs a FATAL error.
  void AbortTransaction(Timestamp timestamp);

  // Same as commit transaction but does not advance 'all_committed_before_'.
  // Used for bootstrap and delayed processing in FOLLOWERS/LEARNERS.
  //
  // The transaction must already have been marked as 'APPLYING' by calling
  // StartApplyingTransaction(), or else this logs a FATAL error.
  void OfflineCommitTransaction(Timestamp timestamp);

  // Used in conjunction with OfflineCommitTransaction() so that the mvcc
  // manager can trim state.
  void OfflineAdjustSafeTime(Timestamp safe_time);

  // Take a snapshot of the current MVCC state, which indicates which
  // transactions have been committed at the time of this call.
  void TakeSnapshot(MvccSnapshot *snapshot) const;

  // Take a snapshot of the MVCC state at 'timestamp' (i.e which includes
  // all transactions which have a lower timestamp)
  //
  // If there are any in-flight transactions at a lower timestamp, waits for
  // them to complete before returning. Hence, we guarantee that, upon return,
  // snapshot->is_clean().
  //
  // TODO(KUDU-689): this may currently block forever, stalling scanner threads
  // and potentially blocking tablet shutdown.
  //
  // REQUIRES: 'timestamp' must be in the past according to the configured
  // clock.
  CHECKED_STATUS WaitForCleanSnapshotAtTimestamp(Timestamp timestamp,
                                         MvccSnapshot* snapshot,
                                         const MonoTime& deadline) const WARN_UNUSED_RESULT;

  // Wait for all operations that are currently APPLYING to commit.
  //
  // NOTE: this does _not_ guarantee that no transactions are APPLYING upon
  // return -- just that those that were APPLYING at call time are finished
  // upon return.
  void WaitForApplyingTransactionsToCommit() const;

  bool AreAllTransactionsCommitted(Timestamp ts) const;

  // Returns the earliest possible timestamp for an uncommitted transaction.
  // All timestamps before this one are guaranteed to be committed.
  Timestamp GetCleanTimestamp() const;

  // Return the timestamps of all transactions which are currently 'APPLYING'
  // (i.e. those which have started to apply their operations to in-memory data
  // structures). Other transactions may have reserved their timestamps via
  // StartTransaction() but not yet been applied.
  //
  // These transactions are guaranteed to eventually Commit() -- i.e. they will
  // never Abort().
  void GetApplyingTransactionsTimestamps(std::vector<Timestamp>* timestamps) const;

  ~MvccManager();

 private:
  friend class MvccTest;
  FRIEND_TEST(MvccTest, TestAreAllTransactionsCommitted);
  FRIEND_TEST(MvccTest, TestTxnAbort);
  FRIEND_TEST(MvccTest, TestCleanTimeCoalescingOnOfflineTransactions);
  FRIEND_TEST(MvccTest, TestWaitForApplyingTransactionsToCommit);

  enum TxnState {
    RESERVED,
    APPLYING
  };

  bool InitTransactionUnlocked(const Timestamp& timestamp);

  enum WaitFor {
    ALL_COMMITTED,
    NONE_APPLYING
  };

  struct WaitingState {
    Timestamp timestamp;
    CountDownLatch* latch;
    WaitFor wait_for;
  };

  // Returns true if all transactions before the given timestamp are committed.
  //
  // If 'ts' is not in the past, it's still possible that new transactions could
  // start with a lower timestamp after this returns.
  bool AreAllTransactionsCommittedUnlocked(Timestamp ts) const;

  // Return true if there is any APPLYING operation with a timestamp
  // less than or equal to 'ts'.
  bool AnyApplyingAtOrBeforeUnlocked(Timestamp ts) const;

  // Waits until all transactions before the given time are committed.
  CHECKED_STATUS WaitUntil(WaitFor wait_for, Timestamp ts,
                   const MonoTime& deadline) const WARN_UNUSED_RESULT;

  // Return true if the condition that the given waiter is waiting on has
  // been achieved.
  bool IsDoneWaitingUnlocked(const WaitingState& waiter) const;

  // Commits the given transaction.
  // Sets *was_earliest to true if this was the earliest in-flight transaction.
  void CommitTransactionUnlocked(Timestamp timestamp,
                                 bool* was_earliest);

  // Remove the timestamp 'ts' from the in-flight map.
  // FATALs if the ts is not in the in-flight map.
  // Returns its state.
  TxnState RemoveInFlightAndGetStateUnlocked(Timestamp ts);

  // Adjusts the clean time, i.e. the timestamp such that all transactions with
  // lower timestamps are committed or aborted, based on which transactions are
  // currently in flight and on what is the latest value of 'no_new_transactions_at_or_before_'.
  void AdjustCleanTime();

  // Advances the earliest in-flight timestamp, based on which transactions are
  // currently in-flight. Usually called when the previous earliest transaction
  // commits or aborts.
  void AdvanceEarliestInFlightTimestamp();

  int GetNumWaitersForTests() const {
    std::lock_guard<simple_spinlock> l(lock_);
    return waiters_.size();
  }

  typedef simple_spinlock LockType;
  mutable LockType lock_;

  MvccSnapshot cur_snap_;

  // The set of timestamps corresponding to currently in-flight transactions.
  typedef std::unordered_map<Timestamp::val_type, TxnState> InFlightMap;
  InFlightMap timestamps_in_flight_;

  // A transaction ID below which all transactions are either committed or in-flight,
  // meaning no new transactions will be started with a timestamp that is equal
  // to or lower than this one.
  Timestamp no_new_transactions_at_or_before_;

  // The minimum timestamp in timestamps_in_flight_, or Timestamp::kMax
  // if that set is empty. This is cached in order to avoid having to iterate
  // over timestamps_in_flight_ on every commit.
  Timestamp earliest_in_flight_;

  scoped_refptr<server::Clock> clock_;
  mutable std::vector<WaitingState*> waiters_;

  DISALLOW_COPY_AND_ASSIGN(MvccManager);
};

// A scoped handle to a running transaction.
// When this object goes out of scope, the transaction is automatically
// committed.
class ScopedTransaction {
 public:

  // How to assign the timestamp to this transaction:
  // NOW - Based on the value obtained from clock_->Now().
  // NOW_LATEST - Based on the value obtained from clock_->NowLatest().
  // PRE_ASSIGNED - Based on the value passed in the ctor.
  enum TimestampAssignmentType {
    NOW,
    NOW_LATEST,
    PRE_ASSIGNED
  };

  // Create a new transaction from the given MvccManager.
  // If 'latest' is true this transaction will use MvccManager::StartTransactionAtLatest()
  // instead of MvccManager::StartTransaction().
  //
  // The MvccManager must remain valid for the lifetime of this object.
  explicit ScopedTransaction(MvccManager *manager, TimestampAssignmentType assignment_type = NOW);

  // Like the ctor above but starts the transaction at a pre-defined timestamp.
  // When this transaction is committed it will use MvccManager::OfflineCommitTransaction()
  // so this is appropriate for offline replaying of transactions for replica catch-up or
  // bootstrap.
  explicit ScopedTransaction(MvccManager *manager,
                             Timestamp timestamp);

  // Commit the transaction referenced by this scoped object, if it hasn't
  // already been committed.
  ~ScopedTransaction();

  Timestamp timestamp() const {
    return timestamp_;
  }

  // Mark that this transaction is about to begin applying its modifications to
  // in-memory stores.
  //
  // This must be called before Commit(). Abort() may not be called after this
  // method.
  void StartApplying();

  // Commit the in-flight transaction.
  //
  // Requires that StartApplying() has been called.
  void Commit();

  // Abort the in-flight transaction.
  //
  // Requires that StartApplying() has NOT been called.
  void Abort();

 private:
  bool done_;
  MvccManager * const manager_;
  TimestampAssignmentType assignment_type_;
  Timestamp timestamp_;

  DISALLOW_COPY_AND_ASSIGN(ScopedTransaction);
};

}  // namespace tablet
}  // namespace yb

#endif  // YB_TABLET_MVCC_H_
