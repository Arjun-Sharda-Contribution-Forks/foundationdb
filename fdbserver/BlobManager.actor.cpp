/*
 * BlobManager.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <sstream>
#include <queue>
#include <vector>
#include <unordered_map>

#include "contrib/fmt-8.0.1/include/fmt/format.h"
#include "fdbclient/BackupContainerFileSystem.h"
#include "fdbclient/BlobGranuleCommon.h"
#include "fdbclient/BlobWorkerInterface.h"
#include "fdbclient/KeyRangeMap.h"
#include "fdbclient/ReadYourWrites.h"
#include "fdbclient/SystemData.h"
#include "fdbserver/BlobManagerInterface.h"
#include "fdbserver/Knobs.h"
#include "fdbserver/BlobGranuleServerCommon.actor.h"
#include "fdbserver/QuietDatabase.h"
#include "fdbserver/WaitFailure.h"
#include "fdbserver/WorkerInterface.actor.h"
#include "flow/Error.h"
#include "flow/IRandom.h"
#include "flow/UnitTest.h"
#include "flow/actorcompiler.h" // has to be last include

#define BM_DEBUG false

// DO NOT CHANGE THIS
// Special key where the value means the epoch + sequence number of the split, instead of the actual boundary
// Chosen because this should not be a start or end key in any split
static Key splitBoundarySpecialKey = "\xff\xff\xff"_sr;

// TODO add comments + documentation
void handleClientBlobRange(KeyRangeMap<bool>* knownBlobRanges,
                           Arena& ar,
                           VectorRef<KeyRangeRef>* rangesToAdd,
                           VectorRef<KeyRangeRef>* rangesToRemove,
                           KeyRef rangeStart,
                           KeyRef rangeEnd,
                           bool rangeActive) {
	if (BM_DEBUG) {
		fmt::print(
		    "db range [{0} - {1}): {2}\n", rangeStart.printable(), rangeEnd.printable(), rangeActive ? "T" : "F");
	}
	KeyRange keyRange(KeyRangeRef(rangeStart, rangeEnd));
	auto allRanges = knownBlobRanges->intersectingRanges(keyRange);
	for (auto& r : allRanges) {
		if (r.value() != rangeActive) {
			KeyRef overlapStart = (r.begin() > keyRange.begin) ? r.begin() : keyRange.begin;
			KeyRef overlapEnd = (keyRange.end < r.end()) ? keyRange.end : r.end();
			KeyRangeRef overlap(overlapStart, overlapEnd);
			if (rangeActive) {
				if (BM_DEBUG) {
					fmt::print("BM Adding client range [{0} - {1})\n",
					           overlapStart.printable().c_str(),
					           overlapEnd.printable().c_str());
				}
				rangesToAdd->push_back_deep(ar, overlap);
			} else {
				if (BM_DEBUG) {
					fmt::print("BM Removing client range [{0} - {1})\n",
					           overlapStart.printable().c_str(),
					           overlapEnd.printable().c_str());
				}
				rangesToRemove->push_back_deep(ar, overlap);
			}
		}
	}
	knownBlobRanges->insert(keyRange, rangeActive);
}

void updateClientBlobRanges(KeyRangeMap<bool>* knownBlobRanges,
                            RangeResult dbBlobRanges,
                            Arena& ar,
                            VectorRef<KeyRangeRef>* rangesToAdd,
                            VectorRef<KeyRangeRef>* rangesToRemove) {
	if (BM_DEBUG) {
		fmt::print("Updating {0} client blob ranges", dbBlobRanges.size() / 2);
		for (int i = 0; i < dbBlobRanges.size() - 1; i += 2) {
			fmt::print("  [{0} - {1})", dbBlobRanges[i].key.printable(), dbBlobRanges[i + 1].key.printable());
		}
		printf("\n");
	}
	// essentially do merge diff of current known blob ranges and new ranges, to assign new ranges to
	// workers and revoke old ranges from workers

	// basically, for any range that is set in results that isn't set in ranges, assign the range to the
	// worker. for any range that isn't set in results that is set in ranges, revoke the range from the
	// worker. and, update ranges to match results as you go

	// FIXME: could change this to O(N) instead of O(NLogN) by doing a sorted merge instead of requesting the
	// intersection for each insert, but this operation is pretty infrequent so it's probably not necessary
	if (dbBlobRanges.size() == 0) {
		// special case. Nothing in the DB, reset knownBlobRanges and revoke all existing ranges from workers
		handleClientBlobRange(
		    knownBlobRanges, ar, rangesToAdd, rangesToRemove, normalKeys.begin, normalKeys.end, false);
	} else {
		if (dbBlobRanges[0].key > normalKeys.begin) {
			handleClientBlobRange(
			    knownBlobRanges, ar, rangesToAdd, rangesToRemove, normalKeys.begin, dbBlobRanges[0].key, false);
		}
		for (int i = 0; i < dbBlobRanges.size() - 1; i++) {
			if (dbBlobRanges[i].key >= normalKeys.end) {
				if (BM_DEBUG) {
					fmt::print("Found invalid blob range start {0}\n", dbBlobRanges[i].key.printable());
				}
				break;
			}
			bool active = dbBlobRanges[i].value == LiteralStringRef("1");
			if (active) {
				if (BM_DEBUG) {
					fmt::print("BM sees client range [{0} - {1})\n",
					           dbBlobRanges[i].key.printable(),
					           dbBlobRanges[i + 1].key.printable());
				}
			}
			KeyRef endKey = dbBlobRanges[i + 1].key;
			if (endKey > normalKeys.end) {
				if (BM_DEBUG) {
					fmt::print("Removing system keyspace from blob range [{0} - {1})\n",
					           dbBlobRanges[i].key.printable(),
					           endKey.printable());
				}
				endKey = normalKeys.end;
			}
			handleClientBlobRange(
			    knownBlobRanges, ar, rangesToAdd, rangesToRemove, dbBlobRanges[i].key, endKey, active);
		}
		if (dbBlobRanges[dbBlobRanges.size() - 1].key < normalKeys.end) {
			handleClientBlobRange(knownBlobRanges,
			                      ar,
			                      rangesToAdd,
			                      rangesToRemove,
			                      dbBlobRanges[dbBlobRanges.size() - 1].key,
			                      normalKeys.end,
			                      false);
		}
	}
	knownBlobRanges->coalesce(normalKeys);
}

void getRanges(std::vector<std::pair<KeyRangeRef, bool>>& results, KeyRangeMap<bool>& knownBlobRanges) {
	if (BM_DEBUG) {
		printf("Getting ranges:\n");
	}
	auto allRanges = knownBlobRanges.ranges();
	for (auto& r : allRanges) {
		results.emplace_back(r.range(), r.value());
		if (BM_DEBUG) {
			fmt::print("  [{0} - {1}): {2}\n", r.begin().printable(), r.end().printable(), r.value() ? "T" : "F");
		}
	}
}

struct RangeAssignmentData {
	AssignRequestType type;

	RangeAssignmentData() : type(AssignRequestType::Normal) {}
	RangeAssignmentData(AssignRequestType type) : type(type) {}
};

struct RangeRevokeData {
	bool dispose;

	RangeRevokeData() {}
	RangeRevokeData(bool dispose) : dispose(dispose) {}
};

struct RangeAssignment {
	bool isAssign;
	KeyRange keyRange;
	Optional<UID> worker;

	// I tried doing this with a union and it was just kind of messy
	Optional<RangeAssignmentData> assign;
	Optional<RangeRevokeData> revoke;
};

// TODO: track worker's reads/writes eventually
struct BlobWorkerStats {
	int numGranulesAssigned;

	BlobWorkerStats(int numGranulesAssigned = 0) : numGranulesAssigned(numGranulesAssigned) {}
};

struct BlobManagerData : NonCopyable, ReferenceCounted<BlobManagerData> {
	UID id;
	Database db;
	Optional<Key> dcId;
	PromiseStream<Future<Void>> addActor;
	Promise<Void> doLockCheck;

	Reference<BackupContainerFileSystem> bstore;

	std::unordered_map<UID, BlobWorkerInterface> workersById;
	std::unordered_map<UID, BlobWorkerStats> workerStats; // mapping between workerID -> workerStats
	std::unordered_set<NetworkAddress> workerAddresses;
	std::unordered_set<UID> deadWorkers;
	KeyRangeMap<UID> workerAssignments;
	KeyRangeActorMap assignsInProgress;
	KeyRangeMap<bool> knownBlobRanges;

	AsyncTrigger startRecruiting;
	Debouncer restartRecruiting;
	std::set<NetworkAddress> recruitingLocalities; // the addrs of the workers being recruited on
	AsyncVar<int> recruitingStream;
	Promise<Void> foundBlobWorkers;
	Promise<Void> doneRecovering;

	int64_t epoch = -1;
	int64_t seqNo = 1;

	Promise<Void> iAmReplaced;

	// The order maintained here is important. The order ranges are put into the promise stream is the order they get
	// assigned sequence numbers
	PromiseStream<RangeAssignment> rangesToAssign;

	BlobManagerData(UID id, Database db, Optional<Key> dcId)
	  : id(id), db(db), dcId(dcId), knownBlobRanges(false, normalKeys.end),
	    restartRecruiting(SERVER_KNOBS->DEBOUNCE_RECRUITING_DELAY), recruitingStream(0) {}

	// TODO REMOVE
	~BlobManagerData() {
		if (BM_DEBUG) {
			fmt::print("Destroying blob manager data for {0} {1}\n", epoch, id.toString());
		}
	}
};

ACTOR Future<Standalone<VectorRef<KeyRef>>> splitRange(Reference<ReadYourWritesTransaction> tr,
                                                       KeyRange range,
                                                       bool writeHot) {
	// TODO is it better to just pass empty metrics to estimated?
	// redo split if previous txn failed to calculate it
	loop {
		try {
			if (BM_DEBUG) {
				fmt::print("Splitting new range [{0} - {1}): {2}\n",
				           range.begin.printable(),
				           range.end.printable(),
				           writeHot ? "hot" : "normal");
			}
			state StorageMetrics estimated =
			    wait(tr->getTransaction().getStorageMetrics(range, CLIENT_KNOBS->TOO_MANY));

			if (BM_DEBUG) {
				fmt::print("Estimated bytes for [{0} - {1}): {2}\n",
				           range.begin.printable(),
				           range.end.printable(),
				           estimated.bytes);
			}

			if (estimated.bytes > SERVER_KNOBS->BG_SNAPSHOT_FILE_TARGET_BYTES || writeHot) {
				// only split on bytes and write rate
				state StorageMetrics splitMetrics;
				splitMetrics.bytes = SERVER_KNOBS->BG_SNAPSHOT_FILE_TARGET_BYTES;
				splitMetrics.bytesPerKSecond = SERVER_KNOBS->SHARD_SPLIT_BYTES_PER_KSEC;
				if (writeHot) {
					splitMetrics.bytesPerKSecond =
					    std::min(splitMetrics.bytesPerKSecond, estimated.bytesPerKSecond / 2);
					splitMetrics.bytesPerKSecond =
					    std::max(splitMetrics.bytesPerKSecond, SERVER_KNOBS->SHARD_MIN_BYTES_PER_KSEC);
				}
				splitMetrics.iosPerKSecond = splitMetrics.infinity;
				splitMetrics.bytesReadPerKSecond = splitMetrics.infinity;

				state PromiseStream<Key> resultStream;
				state Standalone<VectorRef<KeyRef>> keys;
				state Future<Void> streamFuture =
				    tr->getTransaction().splitStorageMetricsStream(resultStream, range, splitMetrics, estimated);
				loop {
					try {
						Key k = waitNext(resultStream.getFuture());
						keys.push_back_deep(keys.arena(), k);
					} catch (Error& e) {
						if (e.code() != error_code_end_of_stream) {
							throw;
						}
						break;
					}
				}

				ASSERT(keys.size() >= 2);
				ASSERT(keys.front() == range.begin);
				ASSERT(keys.back() == range.end);
				return keys;
			} else {
				if (BM_DEBUG) {
					printf("Not splitting range\n");
				}
				Standalone<VectorRef<KeyRef>> keys;
				keys.push_back_deep(keys.arena(), range.begin);
				keys.push_back_deep(keys.arena(), range.end);
				return keys;
			}
		} catch (Error& e) {
			if (BM_DEBUG) {
				printf("Splitting range got error %s\n", e.name());
			}
			wait(tr->onError(e));
		}
	}
}

// Picks a worker with the fewest number of already assigned ranges.
// If there is a tie, picks one such worker at random.
ACTOR Future<UID> pickWorkerForAssign(Reference<BlobManagerData> bmData) {
	// wait until there are BWs to pick from
	while (bmData->workerStats.size() == 0) {
		// TODO REMOVE
		if (BM_DEBUG) {
			fmt::print("BM {0} waiting for blob workers before assigning granules\n", bmData->epoch);
		}
		bmData->restartRecruiting.trigger();
		wait(bmData->recruitingStream.onChange() || bmData->foundBlobWorkers.getFuture());
	}

	int minGranulesAssigned = INT_MAX;
	std::vector<UID> eligibleWorkers;

	for (auto const& worker : bmData->workerStats) {
		UID currId = worker.first;
		int granulesAssigned = worker.second.numGranulesAssigned;

		if (granulesAssigned < minGranulesAssigned) {
			eligibleWorkers.resize(0);
			minGranulesAssigned = granulesAssigned;
			eligibleWorkers.emplace_back(currId);
		} else if (granulesAssigned == minGranulesAssigned) {
			eligibleWorkers.emplace_back(currId);
		}
	}

	// pick a random worker out of the eligible workers
	ASSERT(eligibleWorkers.size() > 0);
	int idx = deterministicRandom()->randomInt(0, eligibleWorkers.size());
	if (BM_DEBUG) {
		fmt::print("picked worker {0}, which has a minimal number ({1}) of granules assigned\n",
		           eligibleWorkers[idx].toString(),
		           minGranulesAssigned);
	}

	return eligibleWorkers[idx];
}

ACTOR Future<Void> doRangeAssignment(Reference<BlobManagerData> bmData,
                                     RangeAssignment assignment,
                                     UID workerID,
                                     int64_t seqNo) {

	if (BM_DEBUG) {
		fmt::print("BM {0} {1} range [{2} - {3}) @ ({4}, {5}) to {6}\n",
		           bmData->epoch,
		           assignment.isAssign ? "assigning" : "revoking",
		           assignment.keyRange.begin.printable(),
		           assignment.keyRange.end.printable(),
		           bmData->epoch,
		           seqNo,
		           workerID.toString());
	}

	try {
		if (assignment.isAssign) {
			ASSERT(assignment.assign.present());
			ASSERT(!assignment.revoke.present());

			AssignBlobRangeRequest req;
			req.keyRange = KeyRangeRef(StringRef(req.arena, assignment.keyRange.begin),
			                           StringRef(req.arena, assignment.keyRange.end));
			req.managerEpoch = bmData->epoch;
			req.managerSeqno = seqNo;
			req.type = assignment.assign.get().type;

			// if that worker isn't alive anymore, add the range back into the stream
			if (bmData->workersById.count(workerID) == 0) {
				throw no_more_servers();
			}
			wait(bmData->workersById[workerID].assignBlobRangeRequest.getReply(req));
		} else {
			ASSERT(!assignment.assign.present());
			ASSERT(assignment.revoke.present());

			RevokeBlobRangeRequest req;
			req.keyRange = KeyRangeRef(StringRef(req.arena, assignment.keyRange.begin),
			                           StringRef(req.arena, assignment.keyRange.end));
			req.managerEpoch = bmData->epoch;
			req.managerSeqno = seqNo;
			req.dispose = assignment.revoke.get().dispose;

			// if that worker isn't alive anymore, this is a noop
			if (bmData->workersById.count(workerID)) {
				wait(bmData->workersById[workerID].revokeBlobRangeRequest.getReply(req));
			} else {
				return Void();
			}
		}
	} catch (Error& e) {
		if (e.code() == error_code_operation_cancelled) {
			throw;
		}
		if (e.code() == error_code_blob_manager_replaced) {
			if (bmData->iAmReplaced.canBeSet()) {
				bmData->iAmReplaced.send(Void());
			}
			return Void();
		}
		if (e.code() == error_code_granule_assignment_conflict) {
			// Another blob worker already owns the range, don't retry.
			// And, if it was us that send the request to another worker for this range, this actor should have been
			// cancelled. So if it wasn't, it's likely that the conflict is from a new blob manager. Trigger the lock
			// check to make sure, and die if so.
			if (BM_DEBUG) {
				fmt::print("BM {0} got conflict assigning [{1} - {2}) to worker {3}, ignoring\n",
				           bmData->epoch,
				           assignment.keyRange.begin.printable(),
				           assignment.keyRange.end.printable(),
				           workerID.toString());
			}
			if (bmData->doLockCheck.canBeSet()) {
				bmData->doLockCheck.send(Void());
			}
			return Void();
		}

		// TODO confirm: using reliable delivery this should only trigger if the worker is marked as failed, right?
		// So assignment needs to be retried elsewhere, and a revoke is trivially complete
		if (assignment.isAssign) {
			if (BM_DEBUG) {
				fmt::print("BM got error {0} assigning range [{1} - {2}) to worker {3}, requeueing\n",
				           e.name(),
				           assignment.keyRange.begin.printable(),
				           assignment.keyRange.end.printable(),
				           workerID.toString());
			}

			// re-send revoke to queue to handle range being un-assigned from that worker before the new one
			RangeAssignment revokeOld;
			revokeOld.isAssign = false;
			revokeOld.worker = workerID;
			revokeOld.keyRange = assignment.keyRange;
			revokeOld.revoke = RangeRevokeData(false);
			bmData->rangesToAssign.send(revokeOld);

			// send assignment back to queue as is, clearing designated worker if present
			// if we failed to send continue or reassign to the worker we thought owned the shard, it should be retried
			// as a normal assign
			ASSERT(assignment.assign.present());
			assignment.assign.get().type = AssignRequestType::Normal;
			assignment.worker.reset();
			bmData->rangesToAssign.send(assignment);
			// FIXME: improvement would be to add history of failed workers to assignment so it can try other ones first
		} else {
			if (BM_DEBUG) {
				fmt::print("BM got error revoking range [{0} - {1}) from worker",
				           assignment.keyRange.begin.printable(),
				           assignment.keyRange.end.printable());
			}

			if (assignment.revoke.get().dispose) {
				if (BM_DEBUG) {
					printf(", retrying for dispose\n");
				}
				// send assignment back to queue as is, clearing designated worker if present
				assignment.worker.reset();
				bmData->rangesToAssign.send(assignment);
				//
			} else {
				if (BM_DEBUG) {
					printf(", ignoring\n");
				}
			}
		}
	}
	return Void();
}

ACTOR Future<Void> rangeAssigner(Reference<BlobManagerData> bmData) {
	loop {
		// inject delay into range assignments
		if (BUGGIFY_WITH_PROB(0.05)) {
			wait(delay(deterministicRandom()->random01()));
		}
		state RangeAssignment assignment = waitNext(bmData->rangesToAssign.getFuture());
		state int64_t seqNo = bmData->seqNo;
		bmData->seqNo++;

		// modify the in-memory assignment data structures, and send request off to worker
		state UID workerId;
		if (assignment.isAssign) {
			bool skip = false;
			// Ensure range isn't currently assigned anywhere, and there is only 1 intersecting range
			auto currentAssignments = bmData->workerAssignments.intersectingRanges(assignment.keyRange);
			int count = 0;
			for (auto i = currentAssignments.begin(); i != currentAssignments.end(); ++i) {
				if (assignment.assign.get().type == AssignRequestType::Continue) {
					ASSERT(assignment.worker.present());
					if (i.range() != assignment.keyRange || i.cvalue() != assignment.worker.get()) {
						if (BM_DEBUG) {
							fmt::print("Out of date re-assign for ({0}, {1}). Assignment must have changed while "
							           "checking split.\n  Reassign: [{2} - {3}): {4}\n  Existing: [{5} - {6}): {7}\n",
							           bmData->epoch,
							           seqNo,
							           assignment.keyRange.begin.printable(),
							           assignment.keyRange.end.printable(),
							           assignment.worker.get().toString().substr(0, 5),
							           i.begin().printable(),
							           i.end().printable(),
							           i.cvalue().toString().substr(0, 5));
						}
						skip = true;
					}
				}
				count++;
			}
			ASSERT(count == 1);
			if (skip) {
				continue;
			}

			if (assignment.worker.present() && assignment.worker.get().isValid()) {
				if (BM_DEBUG) {
					fmt::print("BW {0} already chosen for seqno {1} in BM {2}\n",
					           assignment.worker.get().toString(),
					           seqNo,
					           bmData->id.toString());
				}
				workerId = assignment.worker.get();
			} else {
				UID _workerId = wait(pickWorkerForAssign(bmData));
				if (BM_DEBUG) {
					fmt::print("Chose BW {0} for seqno {1} in BM {2}\n", _workerId.toString(), seqNo, bmData->epoch);
				}
				workerId = _workerId;
			}
			bmData->workerAssignments.insert(assignment.keyRange, workerId);

			// If we know about the worker and this is not a continue, then this is a new range for the worker
			if (bmData->workerStats.count(workerId) && assignment.assign.get().type != AssignRequestType::Continue) {
				bmData->workerStats[workerId].numGranulesAssigned += 1;
			}

			// FIXME: if range is assign, have some sort of semaphore for outstanding assignments so we don't assign
			// a ton ranges at once and blow up FDB with reading initial snapshots.
			bmData->assignsInProgress.insert(assignment.keyRange,
			                                 doRangeAssignment(bmData, assignment, workerId, seqNo));
		} else {
			if (assignment.worker.present()) {
				// revoke this specific range from this specific worker. Either part of recovery or failing a worker
				if (bmData->workerStats.count(assignment.worker.get())) {
					bmData->workerStats[assignment.worker.get()].numGranulesAssigned -= 1;
				}
				bmData->addActor.send(doRangeAssignment(bmData, assignment, assignment.worker.get(), seqNo));
			} else {
				auto currentAssignments = bmData->workerAssignments.intersectingRanges(assignment.keyRange);
				for (auto& it : currentAssignments) {
					// ensure range doesn't truncate existing ranges
					ASSERT(it.begin() >= assignment.keyRange.begin);
					ASSERT(it.end() <= assignment.keyRange.end);

					// It is fine for multiple disjoint sub-ranges to have the same sequence number since they were part
					// of the same logical change

					if (bmData->workerStats.count(it.value())) {
						bmData->workerStats[it.value()].numGranulesAssigned -= 1;
					}

					// revoke the range for the worker that owns it, not the worker specified in the revoke
					bmData->addActor.send(doRangeAssignment(bmData, assignment, it.value(), seqNo));
				}
				bmData->workerAssignments.insert(assignment.keyRange, UID());
			}

			bmData->assignsInProgress.cancel(assignment.keyRange);
		}
	}
}

ACTOR Future<Void> checkManagerLock(Reference<ReadYourWritesTransaction> tr, Reference<BlobManagerData> bmData) {
	Optional<Value> currentLockValue = wait(tr->get(blobManagerEpochKey));
	ASSERT(currentLockValue.present());
	int64_t currentEpoch = decodeBlobManagerEpochValue(currentLockValue.get());
	if (currentEpoch != bmData->epoch) {
		ASSERT(currentEpoch > bmData->epoch);

		if (BM_DEBUG) {
			fmt::print(
			    "BM {0} found new epoch {1} > {2} in lock check\n", bmData->id.toString(), currentEpoch, bmData->epoch);
		}
		if (bmData->iAmReplaced.canBeSet()) {
			bmData->iAmReplaced.send(Void());
		}

		throw blob_manager_replaced();
	}
	tr->addReadConflictRange(singleKeyRange(blobManagerEpochKey));

	return Void();
}

ACTOR Future<Void> writeInitialGranuleMapping(Reference<BlobManagerData> bmData,
                                              Standalone<VectorRef<KeyRef>> boundaries) {
	state Reference<ReadYourWritesTransaction> tr = makeReference<ReadYourWritesTransaction>(bmData->db);
	// don't do too many in one transaction
	state int i = 0;
	state int transactionChunkSize = BUGGIFY ? deterministicRandom()->randomInt(2, 5) : 1000;
	while (i < boundaries.size() - 1) {
		TEST(i > 0); // multiple transactions for large granule split
		tr->reset();
		state int j = 0;
		loop {
			try {
				tr->setOption(FDBTransactionOptions::Option::PRIORITY_SYSTEM_IMMEDIATE);
				tr->setOption(FDBTransactionOptions::Option::ACCESS_SYSTEM_KEYS);
				while (i + j < boundaries.size() - 1 && j < transactionChunkSize) {
					// TODO REMOVE
					if (BM_DEBUG) {
						fmt::print("Persisting initial mapping for [{0} - {1})\n",
						           boundaries[i + j].printable(),
						           boundaries[i + j + 1].printable());
					}
					// set to empty UID - no worker assigned yet
					wait(krmSetRange(tr,
					                 blobGranuleMappingKeys.begin,
					                 KeyRangeRef(boundaries[i + j], boundaries[i + j + 1]),
					                 blobGranuleMappingValueFor(UID())));
					j++;
				}
				wait(tr->commit());
				if (BM_DEBUG) {
					for (int k = 0; k < j; k++) {
						fmt::print("Persisted initial mapping for [{0} - {1})\n",
						           boundaries[i + k].printable(),
						           boundaries[i + k + 1].printable());
					}
				}
				break;
			} catch (Error& e) {
				if (BM_DEBUG) {
					fmt::print("Persisting initial mapping got error {}\n", e.name());
				}
				wait(tr->onError(e));
				j = 0;
			}
		}
		i += j;
	}
	return Void();
}

// FIXME: this does all logic in one transaction. Adding a giant range to an existing database to blobify would
// require doing a ton of storage metrics calls, which we should split up across multiple transactions likely.
ACTOR Future<Void> monitorClientRanges(Reference<BlobManagerData> bmData) {
	state Optional<Value> lastChangeKeyValue;
	state bool needToCoalesce = bmData->epoch > 1;
	loop {
		state Reference<ReadYourWritesTransaction> tr = makeReference<ReadYourWritesTransaction>(bmData->db);

		if (BM_DEBUG) {
			printf("Blob manager checking for range updates\n");
		}
		loop {
			try {
				tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
				tr->setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);

				// read change key at this point along with ranges
				state Optional<Value> ckvBegin = wait(tr->get(blobRangeChangeKey));

				// TODO probably knobs here? This should always be pretty small though
				RangeResult results = wait(krmGetRanges(
				    tr, blobRangeKeys.begin, KeyRange(normalKeys), 10000, GetRangeLimits::BYTE_LIMIT_UNLIMITED));
				ASSERT(!results.more && results.size() < CLIENT_KNOBS->TOO_MANY);

				state Arena ar;
				ar.dependsOn(results.arena());
				VectorRef<KeyRangeRef> rangesToAdd;
				VectorRef<KeyRangeRef> rangesToRemove;
				updateClientBlobRanges(&bmData->knownBlobRanges, results, ar, &rangesToAdd, &rangesToRemove);

				if (needToCoalesce) {
					// recovery has granules instead of known ranges in here. We need to do so to identify any parts of
					// known client ranges the last manager didn't finish blob-ifying.
					// To coalesce the map, we simply override known ranges with the current DB ranges after computing
					// rangesToAdd + rangesToRemove
					needToCoalesce = false;

					for (int i = 0; i < results.size() - 1; i++) {
						bool active = results[i].value == LiteralStringRef("1");
						bmData->knownBlobRanges.insert(KeyRangeRef(results[i].key, results[i + 1].key), active);
					}
				}

				for (KeyRangeRef range : rangesToRemove) {
					if (BM_DEBUG) {
						fmt::print(
						    "BM Got range to revoke [{0} - {1})\n", range.begin.printable(), range.end.printable());
					}

					RangeAssignment ra;
					ra.isAssign = false;
					ra.keyRange = range;
					ra.revoke = RangeRevokeData(true); // dispose=true
					bmData->rangesToAssign.send(ra);
				}

				state std::vector<Future<Standalone<VectorRef<KeyRef>>>> splitFutures;
				// Divide new ranges up into equal chunks by using SS byte sample
				for (KeyRangeRef range : rangesToAdd) {
					splitFutures.push_back(splitRange(tr, range, false));
				}

				for (auto f : splitFutures) {
					state Standalone<VectorRef<KeyRef>> splits = wait(f);
					if (BM_DEBUG) {
						fmt::print("Split client range [{0} - {1}) into {2} ranges:\n",
						           splits[0].printable(),
						           splits[splits.size() - 1].printable(),
						           splits.size() - 1);
					}

					// Write to DB BEFORE sending assign requests, so that if manager dies before/during, new manager
					// picks up the same ranges
					wait(writeInitialGranuleMapping(bmData, splits));

					for (int i = 0; i < splits.size() - 1; i++) {
						KeyRange range = KeyRange(KeyRangeRef(splits[i], splits[i + 1]));
						// only add the client range if this is the first BM or it's not already assigned
						if (BM_DEBUG) {
							fmt::print(
							    "    [{0} - {1})\n", range.begin.printable().c_str(), range.end.printable().c_str());
						}

						RangeAssignment ra;
						ra.isAssign = true;
						ra.keyRange = range;
						ra.assign = RangeAssignmentData(); // type=normal
						bmData->rangesToAssign.send(ra);
					}
					wait(bmData->rangesToAssign.onEmpty());
				}

				lastChangeKeyValue =
				    ckvBegin; // the version of the ranges we processed is the one read alongside the ranges

				// do a new transaction, check for change in change key, watch if none
				tr->reset();
				tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
				tr->setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);
				state Future<Void> watchFuture;

				Optional<Value> ckvEnd = wait(tr->get(blobRangeChangeKey));

				if (ckvEnd == lastChangeKeyValue) {
					watchFuture = tr->watch(blobRangeChangeKey); // watch for change in key
					wait(tr->commit());
					if (BM_DEBUG) {
						printf("Blob manager done processing client ranges, awaiting update\n");
					}
				} else {
					watchFuture = Future<Void>(Void()); // restart immediately
				}

				wait(watchFuture);
				break;
			} catch (Error& e) {
				if (BM_DEBUG) {
					fmt::print("Blob manager got error looking for range updates {}\n", e.name());
				}
				wait(tr->onError(e));
			}
		}
	}
}

// split recursively in the middle to guarantee roughly equal splits across different parts of key space
static void downsampleSplit(const Standalone<VectorRef<KeyRef>>& splits,
                            Standalone<VectorRef<KeyRef>>& out,
                            int startIdx,
                            int endIdx,
                            int remaining) {
	ASSERT(endIdx - startIdx >= remaining);
	ASSERT(remaining >= 0);
	if (remaining == 0) {
		return;
	}
	if (endIdx - startIdx == remaining) {
		out.append(out.arena(), splits.begin() + startIdx, remaining);
	} else {
		int mid = (startIdx + endIdx) / 2;
		int startCount = (remaining - 1) / 2;
		int endCount = remaining - startCount - 1;
		// ensure no infinite recursion
		ASSERT(mid != endIdx);
		ASSERT(mid + 1 != startIdx);
		downsampleSplit(splits, out, startIdx, mid, startCount);
		out.push_back(out.arena(), splits[mid]);
		downsampleSplit(splits, out, mid + 1, endIdx, endCount);
	}
}

ACTOR Future<Void> maybeSplitRange(Reference<BlobManagerData> bmData,
                                   UID currentWorkerId,
                                   KeyRange granuleRange,
                                   UID granuleID,
                                   Version granuleStartVersion,
                                   Version latestVersion,
                                   bool writeHot) {
	state Reference<ReadYourWritesTransaction> tr = makeReference<ReadYourWritesTransaction>(bmData->db);
	state Standalone<VectorRef<KeyRef>> newRanges;
	state int64_t newLockSeqno = -1;

	// first get ranges to split
	Standalone<VectorRef<KeyRef>> _newRanges = wait(splitRange(tr, granuleRange, writeHot));
	newRanges = _newRanges;

	ASSERT(newRanges.size() >= 2);
	if (newRanges.size() == 2) {
		// not large enough to split, just reassign back to worker
		if (BM_DEBUG) {
			fmt::print("Not splitting existing range [{0} - {1}). Continuing assignment to {2}\n",
			           granuleRange.begin.printable(),
			           granuleRange.end.printable(),
			           currentWorkerId.toString());
		}
		RangeAssignment raContinue;
		raContinue.isAssign = true;
		raContinue.worker = currentWorkerId;
		raContinue.keyRange = granuleRange;
		raContinue.assign = RangeAssignmentData(AssignRequestType::Continue); // continue assignment and re-snapshot
		bmData->rangesToAssign.send(raContinue);
		return Void();
	}

	// TODO KNOB for this.
	// Enforce max split fanout of 10 for performance reasons
	int maxSplitFanout = 10;
	if (newRanges.size() >= maxSplitFanout + 2) { // +2 because this is boundaries, so N keys would have N+1 bounaries.
		TEST(true); // downsampling granule split because fanout too high
		Standalone<VectorRef<KeyRef>> coalescedRanges;
		coalescedRanges.arena().dependsOn(newRanges.arena());
		coalescedRanges.push_back(coalescedRanges.arena(), newRanges.front());

		// since we include start + end boundaries here, only need maxSplitFanout-1 split boundaries to produce
		// maxSplitFanout granules
		downsampleSplit(newRanges, coalescedRanges, 1, newRanges.size() - 1, maxSplitFanout - 1);

		coalescedRanges.push_back(coalescedRanges.arena(), newRanges.back());
		ASSERT(coalescedRanges.size() == maxSplitFanout + 1);
		if (BM_DEBUG) {
			fmt::print("Downsampled split from {0} -> {1} granules", newRanges.size() - 1, maxSplitFanout);
		}

		newRanges = coalescedRanges;
	}

	if (BM_DEBUG) {
		fmt::print("Splitting range [{0} - {1}) into {2} granules @ {3}:\n",
		           granuleRange.begin.printable(),
		           granuleRange.end.printable(),
		           newRanges.size() - 1,
		           latestVersion);
		for (int i = 0; i < newRanges.size(); i++) {
			fmt::print("    {}\n", newRanges[i].printable());
		}
	}
	ASSERT(granuleRange.begin == newRanges.front());
	ASSERT(granuleRange.end == newRanges.back());

	// Have to make set of granule ids deterministic across retries to not end up with extra UIDs in the split
	// state, which could cause recovery to fail and resources to not be cleaned up.
	// This entire transaction must be idempotent across retries for all splitting state
	state std::vector<UID> newGranuleIDs;
	newGranuleIDs.reserve(newRanges.size() - 1);
	for (int i = 0; i < newRanges.size() - 1; i++) {
		newGranuleIDs.push_back(deterministicRandom()->randomUniqueID());
	}

	state int64_t splitSeqno = bmData->seqNo;
	bmData->seqNo++;

	// Need to split range. Persist intent to split and split metadata to DB BEFORE sending split assignments to blob
	// workers, so that nothing is lost on blob manager recovery
	loop {
		try {
			tr->reset();
			tr->setOption(FDBTransactionOptions::Option::PRIORITY_SYSTEM_IMMEDIATE);
			tr->setOption(FDBTransactionOptions::Option::ACCESS_SYSTEM_KEYS);
			ASSERT(newRanges.size() > 2);

			// make sure we're still manager when this transaction gets committed
			wait(checkManagerLock(tr, bmData));

			// acquire lock for old granule to make sure nobody else modifies it
			state Key lockKey = blobGranuleLockKeyFor(granuleRange);
			Optional<Value> lockValue = wait(tr->get(lockKey));
			ASSERT(lockValue.present());
			std::tuple<int64_t, int64_t, UID> prevGranuleLock = decodeBlobGranuleLockValue(lockValue.get());
			if (std::get<0>(prevGranuleLock) > bmData->epoch) {
				if (BM_DEBUG) {
					fmt::print("BM {0} found a higher epoch {1} than {2} for granule lock of [{3} - {4})\n",
					           bmData->id.toString(),
					           std::get<0>(prevGranuleLock),
					           bmData->epoch,
					           granuleRange.begin.printable(),
					           granuleRange.end.printable());
				}

				if (bmData->iAmReplaced.canBeSet()) {
					bmData->iAmReplaced.send(Void());
				}
				return Void();
			}
			int64_t ownerEpoch = std::get<0>(prevGranuleLock);
			int64_t ownerSeqno = std::get<1>(prevGranuleLock);
			if (newLockSeqno == -1) {
				newLockSeqno = bmData->seqNo;
				bmData->seqNo++;
				if (!(bmData->epoch > ownerEpoch || (bmData->epoch == ownerEpoch && newLockSeqno > ownerSeqno))) {
					printf("BM seqno for granule [%s - %s) out of order for lock! manager: (%lld, %lld), owner: %lld, "
					       "%lld)\n",
					       granuleRange.begin.printable().c_str(),
					       granuleRange.end.printable().c_str(),
					       bmData->epoch,
					       newLockSeqno,
					       ownerEpoch,
					       ownerSeqno);
				}
				ASSERT(bmData->epoch > ownerEpoch || (bmData->epoch == ownerEpoch && newLockSeqno > ownerSeqno));
			} else {
				if (!(bmData->epoch > ownerEpoch || (bmData->epoch == ownerEpoch && newLockSeqno >= ownerSeqno))) {
					printf("BM seqno for granule [%s - %s) out of order for lock on retry! manager: (%lld, %lld), "
					       "owner: %lld, "
					       "%lld)\n",
					       granuleRange.begin.printable().c_str(),
					       granuleRange.end.printable().c_str(),
					       bmData->epoch,
					       newLockSeqno,
					       ownerEpoch,
					       ownerSeqno);
				}
				// previous transaction could have succeeded but got commit_unknown_result, so use >= instead of > for
				// seqno if epochs are equal
				ASSERT(bmData->epoch > ownerEpoch || (bmData->epoch == ownerEpoch && newLockSeqno >= ownerSeqno));
			}

			// acquire granule lock so nobody else can make changes to this granule.
			tr->set(lockKey, blobGranuleLockValueFor(bmData->epoch, newLockSeqno, std::get<2>(prevGranuleLock)));

			// set up split metadata
			/*fmt::print("Persisting granule split {0} [{1} - {2})\n",
			           granuleID.toString().substr(0, 6),
			           granuleRange.begin.printable(),
			           granuleRange.end.printable());*/

			// first key in split boundaries is special: key that doesn't occur normally to the (epoch, seqno) of split
			tr->set(blobGranuleSplitBoundaryKeyFor(granuleID, splitBoundarySpecialKey),
			        blobGranuleSplitBoundaryValueFor(bmData->epoch, splitSeqno));
			for (int i = 0; i < newRanges.size() - 1; i++) {
				/*fmt::print("    {0} [{1} - {2})\n",
				           newGranuleIDs[i].toString().substr(0, 6),
				           newRanges[i].printable(),
				           newRanges[i + 1].printable());*/

				Key splitKey = blobGranuleSplitKeyFor(granuleID, newGranuleIDs[i]);
				tr->set(blobGranuleSplitBoundaryKeyFor(granuleID, newRanges[i]), Value());

				tr->atomicOp(splitKey,
				             blobGranuleSplitValueFor(BlobGranuleSplitState::Initialized),
				             MutationRef::SetVersionstampedValue);

				Key historyKey = blobGranuleHistoryKeyFor(KeyRangeRef(newRanges[i], newRanges[i + 1]), latestVersion);

				Standalone<BlobGranuleHistoryValue> historyValue;
				historyValue.granuleID = newGranuleIDs[i];
				historyValue.parentGranules.push_back(historyValue.arena(),
				                                      std::pair(granuleRange, granuleStartVersion));

				/*fmt::print("Creating history entry [{0} - {1}) - [{2} - {3})\n",
				           newRanges[i].printable(),
				           newRanges[i + 1].printable(),
				           granuleStartVersion,
				           latestVersion);*/
				tr->set(historyKey, blobGranuleHistoryValueFor(historyValue));
			}
			tr->set(blobGranuleSplitBoundaryKeyFor(granuleID, newRanges.back()), Value());

			wait(tr->commit());
			break;
		} catch (Error& e) {
			if (e.code() == error_code_operation_cancelled) {
				throw;
			}
			if (BM_DEBUG) {
				fmt::print("BM {0} Persisting granule split got error {1}\n", bmData->epoch, e.name());
			}
			if (e.code() == error_code_granule_assignment_conflict) {
				if (bmData->iAmReplaced.canBeSet()) {
					bmData->iAmReplaced.send(Void());
				}
				return Void();
			}
			wait(tr->onError(e));
		}
	}

	// transaction committed, send range assignments
	// revoke from current worker
	RangeAssignment raRevoke;
	raRevoke.isAssign = false;
	raRevoke.worker = currentWorkerId;
	raRevoke.keyRange = granuleRange;
	raRevoke.revoke = RangeRevokeData(false); // not a dispose
	bmData->rangesToAssign.send(raRevoke);

	for (int i = 0; i < newRanges.size() - 1; i++) {
		// reassign new range and do handover of previous range
		RangeAssignment raAssignSplit;
		raAssignSplit.isAssign = true;
		raAssignSplit.keyRange = KeyRangeRef(newRanges[i], newRanges[i + 1]);
		raAssignSplit.assign = RangeAssignmentData();
		// don't care who this range gets assigned to
		bmData->rangesToAssign.send(raAssignSplit);
	}

	return Void();
}

ACTOR Future<Void> deregisterBlobWorker(Reference<BlobManagerData> bmData, BlobWorkerInterface interf) {
	state Reference<ReadYourWritesTransaction> tr = makeReference<ReadYourWritesTransaction>(bmData->db);
	loop {
		tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
		tr->setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);
		try {
			wait(checkManagerLock(tr, bmData));
			Key blobWorkerListKey = blobWorkerListKeyFor(interf.id());
			tr->addReadConflictRange(singleKeyRange(blobWorkerListKey));
			tr->clear(blobWorkerListKey);

			wait(tr->commit());

			if (BM_DEBUG) {
				fmt::print("Deregistered blob worker {0}\n", interf.id().toString());
			}
			return Void();
		} catch (Error& e) {
			if (BM_DEBUG) {
				fmt::print("Deregistering blob worker {0} got error {1}\n", interf.id().toString(), e.name());
			}
			wait(tr->onError(e));
		}
	}
}

ACTOR Future<Void> haltBlobWorker(Reference<BlobManagerData> bmData, BlobWorkerInterface bwInterf) {
	loop {
		try {
			wait(bwInterf.haltBlobWorker.getReply(HaltBlobWorkerRequest(bmData->epoch, bmData->id)));
			break;
		} catch (Error& e) {
			// throw other errors instead of returning?
			if (e.code() == error_code_operation_cancelled) {
				throw;
			}
			// TODO REMOVE
			fmt::print("BM {0} got error {1} trying to halt blob worker {2}\n",
			           bmData->epoch,
			           e.name(),
			           bwInterf.id().toString());
			if (e.code() != error_code_blob_manager_replaced) {
				break;
			}
			if (bmData->iAmReplaced.canBeSet()) {
				bmData->iAmReplaced.send(Void());
			}
		}
	}

	return Void();
}

ACTOR Future<Void> killBlobWorker(Reference<BlobManagerData> bmData, BlobWorkerInterface bwInterf, bool registered) {
	state UID bwId = bwInterf.id();

	// Remove blob worker from stats map so that when we try to find a worker to takeover the range,
	// the one we just killed isn't considered.
	// Remove it from workersById also since otherwise that worker addr will remain excluded
	// when we try to recruit new blob workers.

	if (registered) {
		bmData->deadWorkers.insert(bwId);
		bmData->workerStats.erase(bwId);
		bmData->workersById.erase(bwId);
		bmData->workerAddresses.erase(bwInterf.stableAddress());
	}

	// Remove blob worker from persisted list of blob workers
	Future<Void> deregister = deregisterBlobWorker(bmData, bwInterf);

	// for every range owned by this blob worker, we want to
	// - send a revoke request for that range
	// - add the range back to the stream of ranges to be assigned
	if (BM_DEBUG) {
		fmt::print("Taking back ranges from BW {0}\n", bwId.toString());
	}
	// copy ranges into vector before sending, because send then modifies workerAssignments
	state std::vector<KeyRange> rangesToMove;
	for (auto& it : bmData->workerAssignments.ranges()) {
		if (it.cvalue() == bwId) {
			rangesToMove.push_back(it.range());
		}
	}
	for (auto& it : rangesToMove) {
		// Send revoke request
		RangeAssignment raRevoke;
		raRevoke.isAssign = false;
		raRevoke.keyRange = it;
		raRevoke.revoke = RangeRevokeData(false);
		bmData->rangesToAssign.send(raRevoke);

		// Add range back into the stream of ranges to be assigned
		RangeAssignment raAssign;
		raAssign.isAssign = true;
		raAssign.worker = Optional<UID>();
		raAssign.keyRange = it;
		raAssign.assign = RangeAssignmentData(); // not a continue
		bmData->rangesToAssign.send(raAssign);
	}

	// Send halt to blob worker, with no expectation of hearing back
	if (BM_DEBUG) {
		fmt::print("Sending halt to BW {}\n", bwId.toString());
	}
	bmData->addActor.send(haltBlobWorker(bmData, bwInterf));

	// wait for blob worker to be removed from DB and in-memory mapping to have reassigned all shards from this worker
	// before removing it from deadWorkers, to avoid a race with checkBlobWorkerList
	wait(deregister && bmData->rangesToAssign.onEmpty());
	// delay(0) after onEmpty to yield back to the range assigner on the final pop to ensure it gets processed before
	// deadWorkers.erase
	wait(delay(0));

	// restart recruiting to replace the dead blob worker
	bmData->restartRecruiting.trigger();

	if (registered) {
		bmData->deadWorkers.erase(bwInterf.id());
	}

	return Void();
}

ACTOR Future<Void> monitorBlobWorkerStatus(Reference<BlobManagerData> bmData, BlobWorkerInterface bwInterf) {
	state KeyRangeMap<std::pair<int64_t, int64_t>> lastSeenSeqno;
	// outer loop handles reconstructing stream if it got a retryable error
	// do backoff, we can get a lot of retries in a row

	// wait for blob manager to be done recovering, so it has initial granule mapping and worker data
	wait(bmData->doneRecovering.getFuture());

	// TODO knob?
	state double backoff = 0.1;
	loop {
		try {
			state ReplyPromiseStream<GranuleStatusReply> statusStream =
			    bwInterf.granuleStatusStreamRequest.getReplyStream(GranuleStatusStreamRequest(bmData->epoch));
			// read from stream until worker fails (should never get explicit end_of_stream)
			loop {
				GranuleStatusReply rep = waitNext(statusStream.getFuture());

				if (BM_DEBUG) {
					fmt::print("BM {0} got status of [{1} - {2}) @ ({3}, {4}) from BW {5}: {6} {7}\n",
					           bmData->epoch,
					           rep.granuleRange.begin.printable(),
					           rep.granuleRange.end.printable(),
					           rep.epoch,
					           rep.seqno,
					           bwInterf.id().toString(),
					           rep.doSplit ? "split" : "",
					           rep.writeHotSplit ? "hot" : "normal");
				}
				// if we get a reply from the stream, reset backoff
				backoff = 0.1;
				if (rep.epoch > bmData->epoch) {
					if (BM_DEBUG) {
						fmt::print("BM heard from BW {0} that there is a new manager with higher epoch\n",
						           bwInterf.id().toString());
					}
					if (bmData->iAmReplaced.canBeSet()) {
						bmData->iAmReplaced.send(Void());
					}
				}

				// TODO maybe this won't be true eventually, but right now the only time the blob worker reports
				// back is to split the range.
				ASSERT(rep.doSplit);

				// only evaluate for split if this worker currently owns the granule in this blob manager's mapping
				auto currGranuleAssignment = bmData->workerAssignments.rangeContaining(rep.granuleRange.begin);
				if (!(currGranuleAssignment.begin() == rep.granuleRange.begin &&
				      currGranuleAssignment.end() == rep.granuleRange.end &&
				      currGranuleAssignment.cvalue() == bwInterf.id())) {
					if (BM_DEBUG) {
						fmt::print(
						    "Manager {0} ignoring status from BW {1} for granule [{2} - {3}) since BW {4} owns it.\n",
						    bmData->epoch,
						    bwInterf.id().toString().substr(0, 5),
						    rep.granuleRange.begin.printable(),
						    rep.granuleRange.end.printable(),
						    currGranuleAssignment.cvalue().toString().substr(0, 5));
					}
					// FIXME: could send revoke request
					continue;
				}

				auto lastReqForGranule = lastSeenSeqno.rangeContaining(rep.granuleRange.begin);
				if (rep.granuleRange.begin == lastReqForGranule.begin() &&
				    rep.granuleRange.end == lastReqForGranule.end() && rep.epoch == lastReqForGranule.value().first &&
				    rep.seqno == lastReqForGranule.value().second) {
					if (BM_DEBUG) {
						fmt::print("Manager {0} received repeat status for the same granule [{1} - {2}), ignoring.\n",
						           bmData->epoch,
						           rep.granuleRange.begin.printable(),
						           rep.granuleRange.end.printable());
					}
				} else {
					if (BM_DEBUG) {
						fmt::print("Manager {0} evaluating [{1} - {2}) @ ({3}, {4}) for split\n",
						           bmData->epoch,
						           rep.granuleRange.begin.printable().c_str(),
						           rep.granuleRange.end.printable().c_str(),
						           rep.epoch,
						           rep.seqno);
					}
					lastSeenSeqno.insert(rep.granuleRange, std::pair(rep.epoch, rep.seqno));
					bmData->addActor.send(maybeSplitRange(bmData,
					                                      bwInterf.id(),
					                                      rep.granuleRange,
					                                      rep.granuleID,
					                                      rep.startVersion,
					                                      rep.latestVersion,
					                                      rep.writeHotSplit));
				}
			}
		} catch (Error& e) {
			if (e.code() == error_code_operation_cancelled) {
				throw e;
			}

			// on known network errors or stream close errors, throw
			if (e.code() == error_code_broken_promise) {
				throw e;
			}

			// if manager is replaced, die
			if (e.code() == error_code_blob_manager_replaced) {
				if (bmData->iAmReplaced.canBeSet()) {
					bmData->iAmReplaced.send(Void());
				}
				return Void();
			}

			// if we got an error constructing or reading from stream that is retryable, wait and retry.
			// Sometimes we get connection_failed without the failure monitor tripping. One example is simulation's
			// rollRandomClose. In this case, just reconstruct the stream. If it was a transient failure, it works, and
			// if it is permanent, the failure monitor will eventually trip.
			ASSERT(e.code() != error_code_end_of_stream);
			if (e.code() == error_code_request_maybe_delivered || e.code() == error_code_connection_failed) {
				wait(delay(backoff));
				backoff = std::min(backoff * 1.5, 5.0);
				continue;
			} else {
				if (BM_DEBUG) {
					fmt::print(
					    "BM got unexpected error {0} monitoring BW {1} status\n", e.name(), bwInterf.id().toString());
				}
				// TODO change back from SevError?
				TraceEvent(SevError, "BWStatusMonitoringFailed", bmData->id)
				    .detail("BlobWorkerID", bwInterf.id())
				    .error(e);
				throw e;
			}
		}
	}
}

ACTOR Future<Void> monitorBlobWorker(Reference<BlobManagerData> bmData, BlobWorkerInterface bwInterf) {
	try {
		state Future<Void> waitFailure = waitFailureClient(bwInterf.waitFailure, SERVER_KNOBS->BLOB_WORKER_TIMEOUT);
		state Future<Void> monitorStatus = monitorBlobWorkerStatus(bmData, bwInterf);

		choose {
			when(wait(waitFailure)) {
				if (BM_DEBUG) {
					fmt::print("BM {0} detected BW {1} is dead\n", bmData->epoch, bwInterf.id().toString());
				}
				TraceEvent("BlobWorkerFailed", bmData->id).detail("BlobWorkerID", bwInterf.id());
			}
			when(wait(monitorStatus)) {
				// should only return when manager got replaced
				ASSERT(!bmData->iAmReplaced.canBeSet());
			}
		}
	} catch (Error& e) {
		// will blob worker get cleaned up in this case?
		if (e.code() == error_code_operation_cancelled) {
			throw e;
		}

		if (BM_DEBUG) {
			fmt::print(
			    "BM {0} got monitoring error {1} from BW {2}\n", bmData->epoch, e.name(), bwInterf.id().toString());
		}

		// TODO: re-evaluate the expected errors here once wait failure issue is resolved
		// Expected errors here are: [broken_promise]
		if (e.code() != error_code_broken_promise) {
			if (BM_DEBUG) {
				fmt::print("BM got unexpected error {0} monitoring BW {1}\n", e.name(), bwInterf.id().toString());
			}
			// TODO change back from SevError?
			TraceEvent(SevError, "BWMonitoringFailed", bmData->id).detail("BlobWorkerID", bwInterf.id()).error(e);
			throw e;
		}
	}

	// kill the blob worker
	wait(killBlobWorker(bmData, bwInterf, true));

	if (BM_DEBUG) {
		fmt::print("No longer monitoring BW {0}\n", bwInterf.id().toString());
	}
	return Void();
}

ACTOR Future<Void> checkBlobWorkerList(Reference<BlobManagerData> bmData, Promise<Void> workerListReady) {

	try {
		loop {
			// Get list of last known blob workers
			// note: the list will include every blob worker that the old manager knew about,
			// but it might also contain blob workers that died while the new manager was being recruited
			std::vector<BlobWorkerInterface> blobWorkers = wait(getBlobWorkers(bmData->db));
			// add all blob workers to this new blob manager's records and start monitoring it
			bool foundAnyNew = false;
			for (auto& worker : blobWorkers) {
				if (!bmData->deadWorkers.count(worker.id())) {
					if (!bmData->workerAddresses.count(worker.stableAddress()) &&
					    worker.locality.dcId() == bmData->dcId) {
						bmData->workerAddresses.insert(worker.stableAddress());
						bmData->workersById[worker.id()] = worker;
						bmData->workerStats[worker.id()] = BlobWorkerStats();
						bmData->addActor.send(monitorBlobWorker(bmData, worker));
						foundAnyNew = true;
					} else if (!bmData->workersById.count(worker.id())) {
						bmData->addActor.send(killBlobWorker(bmData, worker, false));
					}
				}
			}
			if (workerListReady.canBeSet()) {
				workerListReady.send(Void());
			}
			// if any assigns are stuck on workers, and we have workers, wake them
			if (foundAnyNew || !bmData->workersById.empty()) {
				Promise<Void> hold = bmData->foundBlobWorkers;
				bmData->foundBlobWorkers = Promise<Void>();
				hold.send(Void());
			}
			wait(delay(SERVER_KNOBS->BLOB_WORKERLIST_FETCH_INTERVAL));
		}
	} catch (Error& e) {
		if (BM_DEBUG) {
			fmt::print("BM {0} got error {1} reading blob worker list!!\n", bmData->epoch, e.name());
		}
		throw e;
	}
}

// Shared code for handling KeyRangeMap<tuple(UID, epoch, seqno)> that is used several places in blob manager recovery
// when there can be conflicting sources of what assignments exist or which workers owns a granule.
// Resolves these conflicts by comparing the epoch + seqno for the range
// Special epoch/seqnos:
//   (0,0): range is not mapped
//   (0,1): range is mapped, but worker is unknown
static void addAssignment(KeyRangeMap<std::tuple<UID, int64_t, int64_t>>& map,
                          const KeyRangeRef& newRange,
                          UID newId,
                          int64_t newEpoch,
                          int64_t newSeqno,
                          std::vector<std::pair<UID, KeyRange>>* outOfDate = nullptr) {
	std::vector<std::pair<KeyRange, std::tuple<UID, int64_t, int64_t>>> newer;
	auto intersecting = map.intersectingRanges(newRange);
	bool allNewer = true;
	for (auto& old : intersecting) {
		UID oldWorker = std::get<0>(old.value());
		int64_t oldEpoch = std::get<1>(old.value());
		int64_t oldSeqno = std::get<2>(old.value());
		if (oldEpoch > newEpoch || (oldEpoch == newEpoch && oldSeqno > newSeqno)) {
			if (newId != oldWorker && newId != UID() && newEpoch == 0 && newSeqno == 1 &&
			    old.begin() == newRange.begin && old.end() == newRange.end) {
				// granule mapping disagrees with worker with highest value. Just do an explicit reassign to a random
				// worker for now to ensure the conflict is resolved.
				newer.push_back(std::pair(old.range(), std::tuple(UID(), oldEpoch, oldSeqno)));
				allNewer = false;
			} else {
				newer.push_back(std::pair(old.range(), std::tuple(oldWorker, oldEpoch, oldSeqno)));
			}
		} else {
			allNewer = false;
			if (newId != UID()) {
				// different workers can't have same epoch and seqno for granule assignment
				ASSERT(oldEpoch != newEpoch || oldSeqno != newSeqno);
			}
			if (outOfDate != nullptr && oldWorker != UID() &&
			    (oldEpoch < newEpoch || (oldEpoch == newEpoch && oldSeqno < newSeqno))) {
				outOfDate->push_back(std::pair(oldWorker, old.range()));
			}
		}
	}

	if (!allNewer) {
		// if this range supercedes an old range insert it over that
		map.insert(newRange, std::tuple(newId, newEpoch, newSeqno));

		// then, if there were any ranges superceded by this one, insert them over this one
		if (newer.size()) {
			if (outOfDate != nullptr && newId != UID()) {
				outOfDate->push_back(std::pair(newId, newRange));
			}
			for (auto& it : newer) {
				map.insert(it.first, it.second);
			}
		}
	} else {
		if (outOfDate != nullptr && newId != UID()) {
			outOfDate->push_back(std::pair(newId, newRange));
		}
	}
}

ACTOR Future<Void> recoverBlobManager(Reference<BlobManagerData> bmData) {
	state Promise<Void> workerListReady;
	bmData->addActor.send(checkBlobWorkerList(bmData, workerListReady));
	wait(workerListReady.getFuture());

	state std::vector<BlobWorkerInterface> startingWorkers;
	for (auto& it : bmData->workersById) {
		startingWorkers.push_back(it.second);
	}

	// Once we acknowledge the existing blob workers, we can go ahead and recruit new ones
	bmData->startRecruiting.trigger();

	// skip the rest of the algorithm for the first blob manager
	if (bmData->epoch == 1) {
		bmData->doneRecovering.send(Void());
		return Void();
	}

	wait(delay(0));

	// At this point, bmData->workersById is a list of all alive blob workers, but could also include some dead BWs.
	// The algorithm below works as follows:
	// 1. We get the ongoing split boundaries to construct the set of granules we should have. For these splits, we
	//    simply assign the range to the next best worker if it is not present in the assignment mapping. This is not
	//    any worse than what the old blob manager would have done. Details: Note that this means that if a worker we
	//    intended to give a splitted range to dies before the new BM recovers, then we'll simply assign the range to
	//    the next best worker.
	//
	// 2. We get the existing granule mappings. We do this by asking all active blob workers for their current granule
	//    assignments. This guarantees a consistent snapshot of the state of that worker's assignments: Any request it
	//    recieved and processed from the old manager before the granule assignment request will be included in the
	//    assignments, and any request it recieves from the old manager afterwards will be rejected with
	//    blob_manager_replaced. We will then read any gaps in the mapping from the database. We will reconcile the set
	//    of ongoing splits to this mapping, and any ranges that are not already assigned to existing blob workers will
	//    be reassigned.
	//
	// 3. For every range in our granuleAssignments, we send an assign request to the stream of requests,
	//    ultimately giving every range back to some worker (trying to mimic the state of the old BM).
	//    If the worker already had the range, this is a no-op. If the worker didn't have it, it will
	//    begin persisting it. The worker that had the same range before will now be at a lower seqno.

	state KeyRangeMap<std::tuple<UID, int64_t, int64_t>> workerAssignments;
	workerAssignments.insert(normalKeys, std::tuple(UID(), 0, 0));
	state Reference<ReadYourWritesTransaction> tr = makeReference<ReadYourWritesTransaction>(bmData->db);

	// TODO KNOB
	state int rowLimit = BUGGIFY ? deterministicRandom()->randomInt(2, 10) : 10000;

	if (BM_DEBUG) {
		fmt::print("BM {0} recovering:\n", bmData->epoch);
		fmt::print("BM {0} found in progress splits:\n", bmData->epoch);
	}

	// TODO use range stream instead

	state UID currentParentID = UID();
	state Optional<UID> nextParentID;
	state std::vector<Key> splitBoundaries;
	state std::pair<int64_t, int64_t>
	    splitEpochSeqno; // used to order splits since we can have multiple splits of the same range in progress at once

	state Key boundaryBeginKey = blobGranuleSplitBoundaryKeys.begin;
	state RangeResult boundaryResult;
	boundaryResult.readThrough = boundaryBeginKey;
	boundaryResult.more = true;
	state int boundaryResultIdx = 0;

	// Step 2. Get the latest known split and merge state. Because we can have multiple splits in progress at the same
	// time, and we don't know which parts of those are reflected in the current set of worker assignments we read, we
	// have to construct the current desired set of granules from the set of ongoing splits and merges. Then, if any of
	// those are not represented in the worker mapping, we must add them.
	state KeyRangeMap<std::tuple<UID, int64_t, int64_t>> inProgressSplits;
	inProgressSplits.insert(normalKeys, std::tuple(UID(), 0, 0));

	tr->reset();
	tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
	tr->setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);

	loop {
		// Advance boundary reader
		loop {
			if (boundaryResultIdx >= boundaryResult.size()) {
				if (!boundaryResult.more) {
					break;
				}
				ASSERT(boundaryResult.readThrough.present() || boundaryResult.size() > 0);
				boundaryBeginKey = boundaryResult.readThrough.present() ? boundaryResult.readThrough.get()
				                                                        : keyAfter(boundaryResult.back().key);
				loop {
					try {
						RangeResult r = wait(
						    tr->getRange(KeyRangeRef(boundaryBeginKey, blobGranuleSplitBoundaryKeys.end), rowLimit));
						ASSERT(r.size() > 0 || !r.more);
						boundaryResult = r;
						boundaryResultIdx = 0;
						break;
					} catch (Error& e) {
						if (BM_DEBUG) {
							fmt::print("BM {0} got error advancing boundary cursor: {1}\n", bmData->epoch, e.name());
						}
						wait(tr->onError(e));
						tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
						tr->setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);
					}
				}
				// if we got a response and there are zero rows, we are done
				if (boundaryResult.empty()) {
					break;
				}
			}
			bool foundNext = false;
			while (boundaryResultIdx < boundaryResult.size()) {
				UID parentGranuleID;
				Key boundaryKey;

				std::tie(parentGranuleID, boundaryKey) =
				    decodeBlobGranuleSplitBoundaryKey(boundaryResult[boundaryResultIdx].key);
				if (parentGranuleID != currentParentID) {
					// nextParentID should have already been set by split reader
					nextParentID = parentGranuleID;
					foundNext = true;
					break;
				}

				if (splitBoundarySpecialKey == boundaryKey) {
					ASSERT(splitEpochSeqno.first == 0 && splitEpochSeqno.second == 0);
					ASSERT(boundaryResult[boundaryResultIdx].value.size() > 0);
					splitEpochSeqno = decodeBlobGranuleSplitBoundaryValue(boundaryResult[boundaryResultIdx].value);
					ASSERT(splitEpochSeqno.first != 0 && splitEpochSeqno.second != 0);
				} else {
					ASSERT(boundaryResult[boundaryResultIdx].value.size() == 0);
					splitBoundaries.push_back(boundaryKey);
				}

				boundaryResultIdx++;
			}
			if (foundNext) {
				break;
			}
		}

		// process this split
		if (currentParentID != UID()) {
			std::sort(splitBoundaries.begin(), splitBoundaries.end());

			if (BM_DEBUG) {
				fmt::print("  [{0} - {1}) {2} @ ({3}, {4}):\n",
				           splitBoundaries.front().printable(),
				           splitBoundaries.back().printable(),
				           currentParentID.toString().substr(0, 6),
				           splitEpochSeqno.first,
				           splitEpochSeqno.second);
			}
			for (int i = 0; i < splitBoundaries.size() - 1; i++) {
				// if this split boundary has not been opened by a blob worker yet, or was not in the assignment list
				// when we previously read it, we must ensure it gets assigned to one
				KeyRange range = KeyRange(KeyRangeRef(splitBoundaries[i], splitBoundaries[i + 1]));
				if (BM_DEBUG) {
					fmt::print("    [{0} - {1})\n", range.begin.printable(), range.end.printable());
				}
				addAssignment(inProgressSplits, range, UID(), splitEpochSeqno.first, splitEpochSeqno.second);
			}
		}
		splitBoundaries.clear();
		splitEpochSeqno = std::pair(0, 0);

		if (!nextParentID.present()) {
			break;
		}
		currentParentID = nextParentID.get();
		nextParentID.reset();
	}

	// Step 3. Get the latest known mapping of granules to blob workers (i.e. assignments)
	// This must happen causally AFTER reading the split boundaries, since the blob workers can clear the split
	// boundaries for a granule as part of persisting their assignment.

	// First, ask existing workers for their mapping
	if (BM_DEBUG) {
		fmt::print("BM {0} requesting assignments from {1} workers:\n", bmData->epoch, startingWorkers.size());
	}
	state std::vector<Future<Optional<GetGranuleAssignmentsReply>>> aliveAssignments;
	aliveAssignments.reserve(startingWorkers.size());
	for (auto& it : startingWorkers) {
		GetGranuleAssignmentsRequest req;
		req.managerEpoch = bmData->epoch;
		aliveAssignments.push_back(timeout(brokenPromiseToNever(it.granuleAssignmentsRequest.getReply(req)),
		                                   SERVER_KNOBS->BLOB_WORKER_TIMEOUT));
	}
	waitForAll(aliveAssignments);

	state std::vector<std::pair<UID, KeyRange>> outOfDateAssignments;
	state int successful = 0;
	state int assignIdx = 0;

	// FIXME: more CPU efficient to do sorted merge of assignments?
	for (; assignIdx < aliveAssignments.size(); assignIdx++) {
		Optional<GetGranuleAssignmentsReply> reply = wait(aliveAssignments[assignIdx]);
		UID workerId = startingWorkers[assignIdx].id();

		if (reply.present()) {
			if (BM_DEBUG) {
				fmt::print("  Worker {}: ({})\n", workerId.toString().substr(0, 5), reply.get().assignments.size());
			}
			successful++;
			for (auto& assignment : reply.get().assignments) {
				if (BM_DEBUG) {
					fmt::print("    [{0} - {1}): ({2}, {3})\n",
					           assignment.range.begin.printable(),
					           assignment.range.end.printable(),
					           assignment.epochAssigned,
					           assignment.seqnoAssigned);
				}
				bmData->knownBlobRanges.insert(assignment.range, true);
				addAssignment(workerAssignments,
				              assignment.range,
				              workerId,
				              assignment.epochAssigned,
				              assignment.seqnoAssigned,
				              &outOfDateAssignments);
			}
			if (bmData->workerStats.count(workerId)) {
				bmData->workerStats[workerId].numGranulesAssigned = reply.get().assignments.size();
			}
		} else {
			// TODO mark as failed and kill it
			if (BM_DEBUG) {
				fmt::print("  Worker {}: failed\n", workerId.toString().substr(0, 5));
			}
		}
	}

	if (BM_DEBUG) {
		fmt::print("BM {0} got assignments from {1}/{2} workers:\n", bmData->epoch, successful, startingWorkers.size());
	}

	if (BM_DEBUG) {
		fmt::print("BM {0} found old assignments:\n", bmData->epoch);
	}

	// then, read any gaps in worker assignment from FDB
	// With a small number of blob workers, if even one is missing, doing numGranules/numWorkers small range reads from
	// FDB is probably less efficient than just reading the whole mapping anyway
	// Plus, we don't have a consistent snapshot of the mapping ACROSS blob workers, so we need the DB to reconcile any
	// differences (eg blob manager revoked from worker A, assigned to B, the revoke from A was processed but the assign
	// to B wasn't, meaning in the snapshot nobody owns the granule)
	state Key beginKey = blobGranuleMappingKeys.begin;
	loop {
		try {
			tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			tr->setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);

			// TODO: replace row limit with knob
			KeyRange nextRange(KeyRangeRef(beginKey, blobGranuleMappingKeys.end));
			// using the krm functions can produce incorrect behavior here as it does weird stuff with beginKey
			state GetRangeLimits limits(rowLimit, GetRangeLimits::BYTE_LIMIT_UNLIMITED);
			limits.minRows = 2;
			RangeResult results = wait(tr->getRange(nextRange, limits));

			// Add the mappings to our in memory key range map
			for (int rangeIdx = 0; rangeIdx < results.size() - 1; rangeIdx++) {
				// TODO REMOVE asserts eventually
				ASSERT(results[rangeIdx].key.startsWith(blobGranuleMappingKeys.begin));
				ASSERT(results[rangeIdx + 1].key.startsWith(blobGranuleMappingKeys.begin));
				Key granuleStartKey = results[rangeIdx].key.removePrefix(blobGranuleMappingKeys.begin);
				Key granuleEndKey = results[rangeIdx + 1].key.removePrefix(blobGranuleMappingKeys.begin);
				if (results[rangeIdx].value.size()) {
					// note: if the old owner is dead, we handle this in rangeAssigner
					UID existingOwner = decodeBlobGranuleMappingValue(results[rangeIdx].value);
					addAssignment(workerAssignments, KeyRangeRef(granuleStartKey, granuleEndKey), existingOwner, 0, 1);

					bmData->knownBlobRanges.insert(KeyRangeRef(granuleStartKey, granuleEndKey), true);
					if (BM_DEBUG) {
						fmt::print("  [{0} - {1})={2}\n",
						           granuleStartKey.printable(),
						           granuleEndKey.printable(),
						           existingOwner.toString().substr(0, 5));
					}
				} else {
					if (BM_DEBUG) {
						fmt::print("  [{0} - {1})\n", granuleStartKey.printable(), granuleEndKey.printable());
					}
				}
			}

			if (!results.more || results.size() <= 1) {
				break;
			}

			// re-read last key to get range that starts there
			beginKey = results.back().key;
		} catch (Error& e) {
			if (BM_DEBUG) {
				fmt::print("BM {0} got error reading granule mapping during recovery: {1}\n", bmData->epoch, e.name());
			}
			wait(tr->onError(e));
		}
	}

	if (BM_DEBUG) {
		printf("Splits overriding the following ranges:\n");
	}
	// Apply current granule boundaries to the assignment map. If they don't exactly match what is currently in the map,
	// override and assign it to a new worker
	auto splits = inProgressSplits.intersectingRanges(normalKeys);
	for (auto& it : splits) {
		int64_t epoch = std::get<1>(it.value());
		int64_t seqno = std::get<2>(it.value());
		if (epoch == 0 || seqno == 0) {
			// no in-progress splits for this range
			continue;
		}

		addAssignment(workerAssignments, it.range(), UID(), epoch, seqno, &outOfDateAssignments);
	}

	// Step 4. Send assign requests for all the granules and transfer assignments
	// from local workerAssignments to bmData
	// before we take ownership of all of the ranges, check the manager lock again
	tr->reset();
	loop {
		try {
			tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			tr->setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);
			wait(checkManagerLock(tr, bmData));
			break;
		} catch (Error& e) {
			if (BM_DEBUG) {
				fmt::print("BM {0} got error checking lock after recovery: {1}\n", bmData->epoch, e.name());
			}
			wait(tr->onError(e));
		}
	}

	// Get set of workers again. Some could have died after reporting assignments
	std::unordered_set<UID> endingWorkers;
	for (auto& it : bmData->workersById) {
		endingWorkers.insert(it.first);
	}

	// revoke assignments that are old and incorrect
	TEST(!outOfDateAssignments.empty()); // BM resolved conflicting assignments on recovery
	for (auto& it : outOfDateAssignments) {
		if (BM_DEBUG) {
			fmt::print("BM {0} revoking out of date assignment [{1} - {2}): {3}:\n",
			           bmData->epoch,
			           it.second.begin.printable().c_str(),
			           it.second.end.printable().c_str(),
			           it.first.toString().c_str());
		}
		RangeAssignment raRevoke;
		raRevoke.isAssign = false;
		raRevoke.worker = it.first;
		raRevoke.keyRange = it.second;
		raRevoke.revoke = RangeRevokeData(false);
		bmData->rangesToAssign.send(raRevoke);
	}

	if (BM_DEBUG) {
		fmt::print("BM {0} final ranges:\n", bmData->epoch);
	}

	int explicitAssignments = 0;
	for (auto& range : workerAssignments.intersectingRanges(normalKeys)) {
		int64_t epoch = std::get<1>(range.value());
		int64_t seqno = std::get<2>(range.value());
		if (epoch == 0 && seqno == 0) {
			/*if (BM_DEBUG) {
			    fmt::print("  [{0} - {1}) invalid\n", range.begin().printable(), range.end().printable());
			}*/
			continue;
		}

		UID workerId = std::get<0>(range.value());
		bmData->workerAssignments.insert(range.range(), workerId);

		if (BM_DEBUG) {
			fmt::print("  [{0} - {1}) @ ({2}, {3}): {4}\n",
			           range.begin().printable(),
			           range.end().printable(),
			           epoch,
			           seqno,
			           workerId == UID() || epoch == 0 ? " (?)" : workerId.toString().substr(0, 5).c_str());
		}

		// if worker id is already set to a known worker that replied with it in the mapping, range is already assigned
		// there. If not, need to explicitly assign it to someone
		if (workerId == UID() || epoch == 0 || !endingWorkers.count(workerId)) {
			RangeAssignment raAssign;
			raAssign.isAssign = true;
			raAssign.worker = workerId;
			raAssign.keyRange = range.range();
			raAssign.assign = RangeAssignmentData(AssignRequestType::Normal);
			bmData->rangesToAssign.send(raAssign);
			explicitAssignments++;
		}
	}

	TraceEvent("BlobManagerRecovered", bmData->id)
	    .detail("Epoch", bmData->epoch)
	    .detail("Granules", bmData->workerAssignments.size())
	    .detail("Assigned", explicitAssignments)
	    .detail("Revoked", outOfDateAssignments.size());

	ASSERT(bmData->doneRecovering.canBeSet());
	bmData->doneRecovering.send(Void());

	return Void();
}

ACTOR Future<Void> chaosRangeMover(Reference<BlobManagerData> bmData) {
	// Only move each granule once during the test, otherwise it can cause availability issues
	// KeyRange isn't hashable and this is only for simulation, so just use toString of range
	state std::unordered_set<std::string> alreadyMoved;
	ASSERT(g_network->isSimulated());
	loop {
		wait(delay(30.0));

		if (g_simulator.speedUpSimulation) {
			if (BM_DEBUG) {
				printf("Range mover stopping\n");
			}
			return Void();
		}

		if (bmData->workersById.size() > 1) {
			int tries = 10;
			while (tries > 0) {
				tries--;
				auto randomRange = bmData->workerAssignments.randomRange();
				if (randomRange.value() != UID() && !alreadyMoved.count(randomRange.range().toString())) {
					if (BM_DEBUG) {
						fmt::print("Range mover moving range [{0} - {1}): {2}\n",
						           randomRange.begin().printable().c_str(),
						           randomRange.end().printable().c_str(),
						           randomRange.value().toString().c_str());
					}
					alreadyMoved.insert(randomRange.range().toString());

					// FIXME: with low probability, could immediately revoke it from the new assignment and move
					// it back right after to test that race

					state KeyRange range = randomRange.range();
					RangeAssignment revokeOld;
					revokeOld.isAssign = false;
					revokeOld.keyRange = range;
					revokeOld.revoke = RangeRevokeData(false);
					bmData->rangesToAssign.send(revokeOld);

					RangeAssignment assignNew;
					assignNew.isAssign = true;
					assignNew.keyRange = range;
					assignNew.assign = RangeAssignmentData(); // not a continue
					bmData->rangesToAssign.send(assignNew);
					break;
				}
			}
			if (tries == 0 && BM_DEBUG) {
				printf("Range mover couldn't find random range to move, skipping\n");
			}
		} else if (BM_DEBUG) {
			fmt::print("Range mover found {0} workers, skipping\n", bmData->workerAssignments.size());
		}
	}
}

// Returns the number of blob workers on addr
int numExistingBWOnAddr(Reference<BlobManagerData> self, const AddressExclusion& addr) {
	int numExistingBW = 0;
	for (auto& server : self->workersById) {
		const NetworkAddress& netAddr = server.second.stableAddress();
		AddressExclusion usedAddr(netAddr.ip, netAddr.port);
		if (usedAddr == addr) {
			++numExistingBW;
		}
	}

	return numExistingBW;
}

// Tries to recruit a blob worker on the candidateWorker process
ACTOR Future<Void> initializeBlobWorker(Reference<BlobManagerData> self, RecruitBlobWorkerReply candidateWorker) {
	const NetworkAddress& netAddr = candidateWorker.worker.stableAddress();
	AddressExclusion workerAddr(netAddr.ip, netAddr.port);
	self->recruitingStream.set(self->recruitingStream.get() + 1);

	// Ask the candidateWorker to initialize a BW only if the worker does not have a pending request
	if (numExistingBWOnAddr(self, workerAddr) == 0 &&
	    self->recruitingLocalities.count(candidateWorker.worker.stableAddress()) == 0) {
		state UID interfaceId = deterministicRandom()->randomUniqueID();

		state InitializeBlobWorkerRequest initReq;
		initReq.reqId = deterministicRandom()->randomUniqueID();
		initReq.interfaceId = interfaceId;

		// acknowledge that this worker is currently being recruited on
		self->recruitingLocalities.insert(candidateWorker.worker.stableAddress());

		TraceEvent("BMRecruiting")
		    .detail("State", "Sending request to worker")
		    .detail("WorkerID", candidateWorker.worker.id())
		    .detail("WorkerLocality", candidateWorker.worker.locality.toString())
		    .detail("Interf", interfaceId)
		    .detail("Addr", candidateWorker.worker.address());

		// send initialization request to worker (i.e. worker.actor.cpp)
		// here, the worker will construct the blob worker at which point the BW will start!
		Future<ErrorOr<InitializeBlobWorkerReply>> fRecruit =
		    candidateWorker.worker.blobWorker.tryGetReply(initReq, TaskPriority::BlobManager);

		// wait on the reply to the request
		state ErrorOr<InitializeBlobWorkerReply> newBlobWorker = wait(fRecruit);

		// if the initialization failed in an unexpected way, then kill the BM.
		// if it failed in an expected way, add some delay before we try to recruit again
		// on this worker
		if (newBlobWorker.isError()) {
			TraceEvent(SevWarn, "BMRecruitmentError").error(newBlobWorker.getError());
			if (!newBlobWorker.isError(error_code_recruitment_failed) &&
			    !newBlobWorker.isError(error_code_request_maybe_delivered)) {
				throw newBlobWorker.getError();
			}
			wait(delay(SERVER_KNOBS->STORAGE_RECRUITMENT_DELAY, TaskPriority::BlobManager));
		}

		// if the initialization succeeded, add the blob worker's interface to
		// the blob manager's data and start monitoring the blob worker
		if (newBlobWorker.present()) {
			BlobWorkerInterface bwi = newBlobWorker.get().interf;

			if (!self->deadWorkers.count(bwi.id())) {
				if (!self->workerAddresses.count(bwi.stableAddress()) && bwi.locality.dcId() == self->dcId) {
					self->workerAddresses.insert(bwi.stableAddress());
					self->workersById[bwi.id()] = bwi;
					self->workerStats[bwi.id()] = BlobWorkerStats();
					self->addActor.send(monitorBlobWorker(self, bwi));
				} else if (!self->workersById.count(bwi.id())) {
					self->addActor.send(killBlobWorker(self, bwi, false));
				}
			}

			TraceEvent("BMRecruiting")
			    .detail("State", "Finished request")
			    .detail("WorkerID", candidateWorker.worker.id())
			    .detail("WorkerLocality", candidateWorker.worker.locality.toString())
			    .detail("Interf", interfaceId)
			    .detail("Addr", candidateWorker.worker.address());
		}

		// acknowledge that this worker is not actively being recruited on anymore.
		// if the initialization did succeed, then this worker will still be excluded
		// since it was added to workersById.
		self->recruitingLocalities.erase(candidateWorker.worker.stableAddress());
	}

	// try to recruit more blob workers
	self->recruitingStream.set(self->recruitingStream.get() - 1);
	self->restartRecruiting.trigger();
	return Void();
}

// Recruits blob workers in a loop
ACTOR Future<Void> blobWorkerRecruiter(
    Reference<BlobManagerData> self,
    Reference<IAsyncListener<RequestStream<RecruitBlobWorkerRequest>>> recruitBlobWorker) {
	state Future<RecruitBlobWorkerReply> fCandidateWorker;
	state RecruitBlobWorkerRequest lastRequest;

	// wait until existing blob workers have been acknowledged so we don't break recruitment invariants
	loop choose {
		when(wait(self->startRecruiting.onTrigger())) { break; }
	}

	loop {
		try {
			state RecruitBlobWorkerRequest recruitReq;

			// workers that are used by existing blob workers should be excluded
			for (auto const& [bwId, bwInterf] : self->workersById) {
				auto addr = bwInterf.stableAddress();
				AddressExclusion addrExcl(addr.ip, addr.port);
				recruitReq.excludeAddresses.emplace_back(addrExcl);
			}

			// workers that are used by blob workers that are currently being recruited should be excluded
			for (auto addr : self->recruitingLocalities) {
				recruitReq.excludeAddresses.emplace_back(AddressExclusion(addr.ip, addr.port));
			}

			TraceEvent("BMRecruiting").detail("State", "Sending request to CC");

			if (!fCandidateWorker.isValid() || fCandidateWorker.isReady() ||
			    recruitReq.excludeAddresses != lastRequest.excludeAddresses) {
				lastRequest = recruitReq;
				// send req to cluster controller to get back a candidate worker we can recruit on
				fCandidateWorker =
				    brokenPromiseToNever(recruitBlobWorker->get().getReply(recruitReq, TaskPriority::BlobManager));
			}

			choose {
				// when we get back a worker we can use, we will try to initialize a blob worker onto that
				// process
				when(RecruitBlobWorkerReply candidateWorker = wait(fCandidateWorker)) {
					self->addActor.send(initializeBlobWorker(self, candidateWorker));
				}

				// when the CC changes, so does the request stream so we need to restart recruiting here
				when(wait(recruitBlobWorker->onChange())) { fCandidateWorker = Future<RecruitBlobWorkerReply>(); }

				// signal used to restart the loop and try to recruit the next blob worker
				when(wait(self->restartRecruiting.onTrigger())) {}
			}
			wait(delay(FLOW_KNOBS->PREVENT_FAST_SPIN_DELAY, TaskPriority::BlobManager));
		} catch (Error& e) {
			if (e.code() != error_code_timed_out) {
				throw;
			}
			TEST(true); // Blob worker recruitment timed out
		}
	}
}

ACTOR Future<Void> haltBlobGranules(Reference<BlobManagerData> bmData) {
	std::vector<BlobWorkerInterface> blobWorkers = wait(getBlobWorkers(bmData->db));
	std::vector<Future<Void>> deregisterBlobWorkers;
	for (auto& worker : blobWorkers) {
		// TODO: send a special req to blob workers so they clean up granules/CFs
		bmData->addActor.send(haltBlobWorker(bmData, worker));
		deregisterBlobWorkers.emplace_back(deregisterBlobWorker(bmData, worker));
	}
	waitForAll(deregisterBlobWorkers);

	return Void();
}

ACTOR Future<GranuleFiles> loadHistoryFiles(Reference<BlobManagerData> bmData, UID granuleID) {
	state Transaction tr(bmData->db);
	state KeyRange range = blobGranuleFileKeyRangeFor(granuleID);
	state Key startKey = range.begin;
	state GranuleFiles files;
	loop {
		try {
			wait(readGranuleFiles(&tr, &startKey, range.end, &files, granuleID, BM_DEBUG));
			return files;
		} catch (Error& e) {
			wait(tr.onError(e));
		}
	}
}

/*
 * Deletes all files pertaining to the granule with id granuleId and
 * also removes the history entry for this granule from the system keyspace
 * TODO ensure cannot fully delete granule that is still splitting!
 */
ACTOR Future<Void> fullyDeleteGranule(Reference<BlobManagerData> self, UID granuleId, KeyRef historyKey) {
	if (BM_DEBUG) {
		fmt::print("Fully deleting granule {0}: init\n", granuleId.toString());
	}

	// get files
	GranuleFiles files = wait(loadHistoryFiles(self->db, granuleId, BM_DEBUG));

	std::vector<Future<Void>> deletions;
	std::vector<std::string> filesToDelete; // TODO: remove, just for debugging

	for (auto snapshotFile : files.snapshotFiles) {
		std::string fname = snapshotFile.filename;
		deletions.emplace_back(self->bstore->deleteFile(fname));
		filesToDelete.emplace_back(fname);
	}

	for (auto deltaFile : files.deltaFiles) {
		std::string fname = deltaFile.filename;
		deletions.emplace_back(self->bstore->deleteFile(fname));
		filesToDelete.emplace_back(fname);
	}

	if (BM_DEBUG) {
		fmt::print("Fully deleting granule {0}: deleting {1} files\n", granuleId.toString(), deletions.size());
		for (auto filename : filesToDelete) {
			fmt::print(" - {}\n", filename.c_str());
		}
	}

	// delete the files before the corresponding metadata.
	// this could lead to dangling pointers in fdb, but this granule should
	// never be read again anyways, and we can clean up the keys the next time around.
	// deleting files before corresponding metadata reduces the # of orphaned files.
	wait(waitForAll(deletions));

	// delete metadata in FDB (history entry and file keys)
	if (BM_DEBUG) {
		fmt::print("Fully deleting granule {0}: deleting history and file keys\n", granuleId.toString());
	}

	state Transaction tr(self->db);
	tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
	tr.setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);

	loop {
		try {
			KeyRange fileRangeKey = blobGranuleFileKeyRangeFor(granuleId);
			tr.clear(historyKey);
			tr.clear(fileRangeKey);
			wait(tr.commit());
			break;
		} catch (Error& e) {
			wait(tr.onError(e));
		}
	}

	if (BM_DEBUG) {
		fmt::print("Fully deleting granule {0}: success\n", granuleId.toString());
	}

	return Void();
}

/*
 * For the granule with id granuleId, finds the first snapshot file at a
 * version <= pruneVersion and deletes all files older than it.
 *
 * Assumption: this granule's startVersion might change because the first snapshot
 * file might be deleted. We will need to ensure we don't rely on the granule's startVersion
 * (that's persisted as part of the key), but rather use the granule's first snapshot's version when needed
 */
ACTOR Future<Void> partiallyDeleteGranule(Reference<BlobManagerData> self, UID granuleId, Version pruneVersion) {
	if (BM_DEBUG) {
		fmt::print("Partially deleting granule {0}: init\n", granuleId.toString());
	}

	// get files
	GranuleFiles files = wait(loadHistoryFiles(self->db, granuleId, BM_DEBUG));

	// represents the version of the latest snapshot file in this granule with G.version < pruneVersion
	Version latestSnapshotVersion = invalidVersion;

	state std::vector<Future<Void>> deletions; // deletion work per file
	state std::vector<Key> deletedFileKeys; // keys for deleted files
	state std::vector<std::string> filesToDelete; // TODO: remove evenutally, just for debugging

	// TODO: binary search these snapshot files for latestSnapshotVersion
	for (int idx = files.snapshotFiles.size() - 1; idx >= 0; --idx) {
		// if we already found the latestSnapshotVersion, this snapshot can be deleted
		if (latestSnapshotVersion != invalidVersion) {
			std::string fname = files.snapshotFiles[idx].filename;
			deletions.emplace_back(self->bstore->deleteFile(fname));
			deletedFileKeys.emplace_back(blobGranuleFileKeyFor(granuleId, 'S', files.snapshotFiles[idx].version));
			filesToDelete.emplace_back(fname);
		} else if (files.snapshotFiles[idx].version <= pruneVersion) {
			// otherwise if this is the FIRST snapshot file with version < pruneVersion,
			// then we found our latestSnapshotVersion (FIRST since we are traversing in reverse)
			latestSnapshotVersion = files.snapshotFiles[idx].version;
		}
	}

	// we would have only partially deleted the granule if such a snapshot existed
	ASSERT(latestSnapshotVersion != invalidVersion);

	// delete all delta files older than latestSnapshotVersion
	for (auto deltaFile : files.deltaFiles) {
		// traversing in fwd direction, so stop once we find the first delta file past the latestSnapshotVersion
		if (deltaFile.version > latestSnapshotVersion) {
			break;
		}

		// otherwise deltaFile.version <= latestSnapshotVersion so delete it
		// == should also be deleted because the last delta file before a snapshot would have the same version
		std::string fname = deltaFile.filename;
		deletions.emplace_back(self->bstore->deleteFile(fname));
		deletedFileKeys.emplace_back(blobGranuleFileKeyFor(granuleId, 'D', deltaFile.version));
		filesToDelete.emplace_back(fname);
	}

	if (BM_DEBUG) {
		fmt::print("Partially deleting granule {0}: deleting {1} files\n", granuleId.toString(), deletions.size());
		for (auto filename : filesToDelete) {
			fmt::print(" - {0}\n", filename);
		}
	}

	// TODO: the following comment relies on the assumption that BWs will not get requests to
	// read data that was already pruned. confirm assumption is fine. otherwise, we'd need
	// to communicate with BWs here and have them ack the pruneVersion

	// delete the files before the corresponding metadata.
	// this could lead to dangling pointers in fdb, but we should never read data older than
	// pruneVersion anyways, and we can clean up the keys the next time around.
	// deleting files before corresponding metadata reduces the # of orphaned files.
	wait(waitForAll(deletions));

	// delete metadata in FDB (deleted file keys)
	if (BM_DEBUG) {
		fmt::print("Partially deleting granule {0}: deleting file keys\n", granuleId.toString());
	}

	state Transaction tr(self->db);
	tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
	tr.setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);

	loop {
		try {
			for (auto& key : deletedFileKeys) {
				tr.clear(key);
			}
			wait(tr.commit());
			break;
		} catch (Error& e) {
			wait(tr.onError(e));
		}
	}

	if (BM_DEBUG) {
		fmt::print("Partially deleting granule {0}: success\n", granuleId.toString());
	}
	return Void();
}

/*
 * This method is used to prune the range [startKey, endKey) at (and including) pruneVersion.
 * To do this, we do a BFS traversal starting at the active granules. Then we classify granules
 * in the history as nodes that can be fully deleted (i.e. their files and history can be deleted)
 * and nodes that can be partially deleted (i.e. some of their files can be deleted).
 * Once all this is done, we finally clear the pruneIntent key, if possible, to indicate we are done
 * processing this prune intent.
 */
ACTOR Future<Void> pruneRange(Reference<BlobManagerData> self,
                              KeyRef startKey,
                              KeyRef endKey,
                              Version pruneVersion,
                              bool force) {
	if (BM_DEBUG) {
		fmt::print("pruneRange starting for range [{0} - {1}) @ pruneVersion={2}, force={3}\n",
		           startKey.printable(),
		           endKey.printable(),
		           pruneVersion,
		           force);
	}

	// queue of <range, startVersion, endVersion> for BFS traversal of history
	// TODO: consider using GranuleHistoryEntry, but that also makes it a little messy
	state std::queue<std::tuple<KeyRange, Version, Version>> historyEntryQueue;

	// stacks of <granuleId, historyKey> and <granuleId> to track which granules to delete
	state std::vector<std::tuple<UID, KeyRef>> toFullyDelete;
	state std::vector<UID> toPartiallyDelete;

	// track which granules we have already added to traversal
	// note: (startKey, startVersion) uniquely identifies a granule
	/*state std::unordered_set<std::pair<const uint8_t*, Version>, boost::hash<std::pair<const uint8_t*, Version>>>
	    visited;*/
	// TODO why doesn't the above compile in CI?
	state std::set<std::pair<const uint8_t*, Version>> visited;

	state KeyRange range(KeyRangeRef(startKey, endKey)); // range for [startKey, endKey)

	// find all active granules (that comprise the range) and add to the queue
	state KeyRangeMap<UID>::Ranges activeRanges = self->workerAssignments.intersectingRanges(range);

	state Transaction tr(self->db);
	tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
	tr.setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);

	state KeyRangeMap<UID>::iterator activeRange;
	for (activeRange = activeRanges.begin(); activeRange != activeRanges.end(); ++activeRange) {
		if (BM_DEBUG) {
			fmt::print("Checking if active range [{0} - {1}), owned by BW {2}, should be pruned\n",
			           activeRange.begin().printable(),
			           activeRange.end().printable(),
			           activeRange.value().toString());
		}

		// assumption: prune boundaries must respect granule boundaries
		if (activeRange.begin() < startKey || activeRange.end() > endKey) {
			continue;
		}

		// TODO: if this is a force prune, then revoke the assignment from the corresponding BW first
		// so that it doesn't try to interact with the granule (i.e. force it to give up gLock).
		// we'll need some way to ack that the revoke was successful

		loop {
			try {
				if (BM_DEBUG) {
					fmt::print("Fetching latest history entry for range [{0} - {1})\n",
					           activeRange.begin().printable(),
					           activeRange.end().printable());
				}
				Optional<GranuleHistory> history = wait(getLatestGranuleHistory(&tr, activeRange.range()));
				// TODO: can we tell from the krm that this range is not valid, so that we don't need to do a
				// get
				if (history.present()) {
					if (BM_DEBUG) {
						printf("Adding range to history queue\n");
					}
					visited.insert({ activeRange.range().begin.begin(), history.get().version });
					historyEntryQueue.push({ activeRange.range(), history.get().version, MAX_VERSION });
				}
				break;
			} catch (Error& e) {
				wait(tr.onError(e));
			}
		}
	}

	if (BM_DEBUG) {
		printf("Beginning BFS traversal of history\n");
	}
	while (!historyEntryQueue.empty()) {
		// process the node at the front of the queue and remove it
		KeyRange currRange;
		state Version startVersion;
		state Version endVersion;
		std::tie(currRange, startVersion, endVersion) = historyEntryQueue.front();
		historyEntryQueue.pop();

		if (BM_DEBUG) {
			fmt::print("Processing history node [{0} - {1}) with versions [{2}, {3})\n",
			           currRange.begin.printable(),
			           currRange.end.printable(),
			           startVersion,
			           endVersion);
		}

		// get the persisted history entry for this granule
		state Standalone<BlobGranuleHistoryValue> currHistoryNode;
		state KeyRef historyKey = blobGranuleHistoryKeyFor(currRange, startVersion);
		loop {
			try {
				Optional<Value> persistedHistory = wait(tr.get(historyKey));
				ASSERT(persistedHistory.present());
				currHistoryNode = decodeBlobGranuleHistoryValue(persistedHistory.get());
				break;
			} catch (Error& e) {
				wait(tr.onError(e));
			}
		}

		if (BM_DEBUG) {
			fmt::print("Found history entry for this node. It's granuleID is {0}\n",
			           currHistoryNode.granuleID.toString());
		}

		// There are three cases this granule can fall into:
		// - if the granule's end version is at or before the prune version or this is a force delete,
		//   this granule should be completely deleted
		// - else if the startVersion <= pruneVersion, then G.startVersion < pruneVersion < G.endVersion
		//   and so this granule should be partially deleted
		// - otherwise, this granule is active, so don't schedule it for deletion
		if (force || endVersion <= pruneVersion) {
			if (BM_DEBUG) {
				fmt::print("Granule {0} will be FULLY deleted\n", currHistoryNode.granuleID.toString());
			}
			toFullyDelete.push_back({ currHistoryNode.granuleID, historyKey });
		} else if (startVersion < pruneVersion) {
			if (BM_DEBUG) {
				fmt::print("Granule {0} will be partially deleted\n", currHistoryNode.granuleID.toString());
			}
			toPartiallyDelete.push_back({ currHistoryNode.granuleID });
		}

		// add all of the node's parents to the queue
		for (auto& parent : currHistoryNode.parentGranules) {
			// if we already added this node to queue, skip it; otherwise, mark it as visited
			if (visited.count({ parent.first.begin.begin(), parent.second })) {
				if (BM_DEBUG) {
					fmt::print("Already added {0} to queue, so skipping it\n", currHistoryNode.granuleID.toString());
				}
				continue;
			}
			visited.insert({ parent.first.begin.begin(), parent.second });

			if (BM_DEBUG) {
				fmt::print("Adding parent [{0} - {1}) with versions [{2} - {3}) to queue\n",
				           parent.first.begin.printable(),
				           parent.first.end.printable(),
				           parent.second,
				           startVersion);
			}

			// the parent's end version is this node's startVersion,
			// since this node must have started where it's parent finished
			historyEntryQueue.push({ parent.first, parent.second, startVersion });
		}
	}

	// The top of the stacks have the oldest ranges. This implies that for a granule located at
	// index i, it's parent must be located at some index j, where j > i. For this reason,
	// we delete granules in reverse order; this way, we will never end up with unreachable
	// nodes in the persisted history. Moreover, for any node that must be fully deleted,
	// any node that must be partially deleted must occur later on in the history. Thus,
	// we delete the 'toFullyDelete' granules first.
	//
	// Unfortunately we can't do parallelize _full_ deletions because they might
	// race and we'll end up with unreachable nodes in the case of a crash.
	// Since partial deletions only occur for "leafs", they can be done in parallel
	//
	// Note about file deletions: although we might be retrying a deletion of a granule,
	// we won't run into any issues with trying to "re-delete" a blob file since deleting
	// a file that doesn't exist is considered successful

	state int i;
	if (BM_DEBUG) {
		fmt::print("{0} granules to fully delete\n", toFullyDelete.size());
	}
	for (i = toFullyDelete.size() - 1; i >= 0; --i) {
		UID granuleId;
		KeyRef historyKey;
		std::tie(granuleId, historyKey) = toFullyDelete[i];
		// FIXME: consider batching into a single txn (need to take care of txn size limit)
		if (BM_DEBUG) {
			fmt::print("About to fully delete granule {0}\n", granuleId.toString());
		}
		wait(fullyDeleteGranule(self, granuleId, historyKey));
	}

	if (BM_DEBUG) {
		fmt::print("{0} granules to partially delete\n", toPartiallyDelete.size());
	}
	std::vector<Future<Void>> partialDeletions;
	for (i = toPartiallyDelete.size() - 1; i >= 0; --i) {
		UID granuleId = toPartiallyDelete[i];
		if (BM_DEBUG) {
			fmt::print("About to partially delete granule {0}\n", granuleId.toString());
		}
		partialDeletions.emplace_back(partiallyDeleteGranule(self, granuleId, pruneVersion));
	}

	wait(waitForAll(partialDeletions));

	// Now that all the necessary granules and their files have been deleted, we can
	// clear the pruneIntent key to signify that the work is done. However, there could have been
	// another pruneIntent that got written for this table while we were processing this one.
	// If that is the case, we should not clear the key. Otherwise, we can just clear the key.

	tr.reset();
	if (BM_DEBUG) {
		printf("About to clear prune intent\n");
	}
	loop {
		try {
			tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			tr.setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);

			state Key pruneIntentKey = blobGranulePruneKeys.begin.withSuffix(startKey);
			state Optional<Value> pruneIntentValue = wait(tr.get(pruneIntentKey));
			ASSERT(pruneIntentValue.present());

			Version currPruneVersion;
			bool currForce;
			std::tie(currPruneVersion, currForce) = decodeBlobGranulePruneValue(pruneIntentValue.get());

			if (currPruneVersion == pruneVersion && currForce == force) {
				tr.clear(pruneIntentKey.withPrefix(blobGranulePruneKeys.begin));
				wait(tr.commit());
			}
			break;
		} catch (Error& e) {
			fmt::print("Attempt to clear prune intent got error {}\n", e.name());
			wait(tr.onError(e));
		}
	}

	if (BM_DEBUG) {
		fmt::print("Successfully pruned range [{0} - {1}) at pruneVersion={2}\n",
		           startKey.printable(),
		           endKey.printable(),
		           pruneVersion);
	}
	return Void();
}

/*
 * This monitor watches for changes to a key K that gets updated whenever there is a new prune intent.
 * On this change, we scan through all blobGranulePruneKeys (which look like <startKey, endKey>=<prune_version,
 * force>) and prune any intents.
 *
 * Once the prune has succeeded, we clear the key IF the version is still the same one that was pruned.
 * That way, if another prune intent arrived for the same range while we were working on an older one,
 * we wouldn't end up clearing the intent.
 *
 * When watching for changes, we might end up in scenarios where we failed to do the work
 * for a prune intent even though the watch was triggered (maybe the BM had a blip). This is problematic
 * if the intent is a force and there isn't another prune intent for quite some time. To remedy this,
 * if we don't see a watch change in X (configurable) seconds, we will just sweep through the prune intents,
 * consolidating any work we might have missed before.
 *
 * Note: we could potentially use a changefeed here to get the exact pruneIntent that was added
 * rather than iterating through all of them, but this might have too much overhead for latency
 * improvements we don't really need here (also we need to go over all prune intents anyways in the
 * case that the timer is up before any new prune intents arrive).
 */
ACTOR Future<Void> monitorPruneKeys(Reference<BlobManagerData> self) {
	// setup bstore
	try {
		if (BM_DEBUG) {
			fmt::print("BM constructing backup container from {}\n", SERVER_KNOBS->BG_URL.c_str());
		}
		self->bstore = BackupContainerFileSystem::openContainerFS(SERVER_KNOBS->BG_URL);
		if (BM_DEBUG) {
			printf("BM constructed backup container\n");
		}
	} catch (Error& e) {
		if (BM_DEBUG) {
			fmt::print("BM got backup container init error {0}\n", e.name());
		}
		throw e;
	}

	try {
		state Value oldPruneWatchVal;
		loop {
			state Reference<ReadYourWritesTransaction> tr = makeReference<ReadYourWritesTransaction>(self->db);
			tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			tr->setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);

			// Wait for the watch to change, or some time to expire (whichever comes first)
			// before checking through the prune intents. We write a UID into the change key value
			// so that we can still recognize when the watch key has been changed while we weren't
			// monitoring it
			loop {
				try {
					tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
					tr->setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);

					state Optional<Value> newPruneWatchVal = wait(tr->get(blobGranulePruneChangeKey));

					// if the value at the change key has changed, that means there is new work to do
					if (newPruneWatchVal.present() && oldPruneWatchVal != newPruneWatchVal.get()) {
						oldPruneWatchVal = newPruneWatchVal.get();
						if (BM_DEBUG) {
							printf("the blobGranulePruneChangeKey changed\n");
						}

						// TODO: debugging code, remove it
						/*
						if (newPruneWatchVal.get().toString().substr(0, 6) == "prune=") {
						    state Reference<ReadYourWritesTransaction> dummy =
						        makeReference<ReadYourWritesTransaction>(self->db);
						    loop {
						        try {
						            dummy->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
						            dummy->setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);
						            std::istringstream iss(newPruneWatchVal.get().toString().substr(6));
						            Version version;
						            iss >> version;
						            dummy->set(blobGranulePruneKeys.begin.withSuffix(normalKeys.begin),
						                       blobGranulePruneValueFor(version, false));
						            wait(dummy->commit());
						            break;

						        } catch (Error& e) {
						            wait(dummy->onError(e));
						        }
						    }
						}
						*/
						break;
					}

					// otherwise, there are no changes and we should wait until the next change (or timeout)
					state Future<Void> watchPruneIntentsChange = tr->watch(blobGranulePruneChangeKey);
					wait(tr->commit());

					if (BM_DEBUG) {
						printf("monitorPruneKeys waiting for change or timeout\n");
					}

					choose {
						when(wait(watchPruneIntentsChange)) {
							if (BM_DEBUG) {
								printf("monitorPruneKeys saw a change\n");
							}
							tr->reset();
						}
						when(wait(delay(SERVER_KNOBS->BG_PRUNE_TIMEOUT))) {
							if (BM_DEBUG) {
								printf("monitorPruneKeys got a timeout\n");
							}
							break;
						}
					}
				} catch (Error& e) {
					wait(tr->onError(e));
				}
			}

			tr->reset();

			if (BM_DEBUG) {
				printf("Looping over prune intents\n");
			}

			// loop through all prune intentions and do prune work accordingly
			try {
				state KeyRef beginKey = normalKeys.begin;
				loop {
					tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
					tr->setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);

					state std::vector<Future<Void>> prunes;
					try {
						// TODO: replace 10000 with a knob
						KeyRange nextRange(KeyRangeRef(beginKey, normalKeys.end));
						state RangeResult pruneIntents = wait(krmGetRanges(
						    tr, blobGranulePruneKeys.begin, nextRange, 10000, GetRangeLimits::BYTE_LIMIT_UNLIMITED));
						state Key lastEndKey;

						for (int rangeIdx = 0; rangeIdx < pruneIntents.size() - 1; ++rangeIdx) {
							KeyRef rangeStartKey = pruneIntents[rangeIdx].key;
							KeyRef rangeEndKey = pruneIntents[rangeIdx + 1].key;
							lastEndKey = rangeEndKey;
							if (pruneIntents[rangeIdx].value.size() == 0) {
								continue;
							}
							KeyRange range(KeyRangeRef(rangeStartKey, rangeEndKey));
							Version pruneVersion;
							bool force;
							std::tie(pruneVersion, force) = decodeBlobGranulePruneValue(pruneIntents[rangeIdx].value);

							fmt::print("about to prune range [{0} - {1}) @ {2}, force={3}\n",
							           rangeStartKey.printable(),
							           rangeEndKey.printable(),
							           pruneVersion,
							           force ? "T" : "F");
							prunes.emplace_back(pruneRange(self, rangeStartKey, rangeEndKey, pruneVersion, force));
						}

						// wait for this set of prunes to complete before starting the next ones since if we
						// prune a range R at version V and while we are doing that, the time expires, we will
						// end up trying to prune the same range again since the work isn't finished and the
						// prunes will race
						//
						// TODO: this isn't that efficient though. Instead we could keep metadata as part of the
						// BM's memory that tracks which prunes are active. Once done, we can mark that work as
						// done. If the BM fails then all prunes will fail and so the next BM will have a clear
						// set of metadata (i.e. no work in progress) so we will end up doing the work in the
						// new BM
						wait(waitForAll(prunes));

						if (!pruneIntents.more) {
							break;
						}

						beginKey = lastEndKey;
					} catch (Error& e) {
						wait(tr->onError(e));
					}
				}
			} catch (Error& e) {
				if (e.code() == error_code_actor_cancelled) {
					throw e;
				}
				if (BM_DEBUG) {
					fmt::print("monitorPruneKeys for BM {0} saw error {1}\n", self->id.toString(), e.name());
				}
				// don't want to kill the blob manager for errors around pruning
				TraceEvent("MonitorPruneKeysError", self->id).detail("Error", e.name());
			}
			if (BM_DEBUG) {
				printf("Done pruning current set of prune intents.\n");
			}
		}
	} catch (Error& e) {
		if (BM_DEBUG) {
			fmt::print("monitorPruneKeys got error {}\n", e.name());
		}
		throw e;
	}
}

ACTOR Future<Void> doLockChecks(Reference<BlobManagerData> bmData) {
	loop {
		Promise<Void> check = bmData->doLockCheck;
		wait(check.getFuture());
		wait(delay(0.5)); // don't do this too often if a lot of conflict

		state Reference<ReadYourWritesTransaction> tr = makeReference<ReadYourWritesTransaction>(bmData->db);

		loop {
			try {
				tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
				tr->setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);
				wait(checkManagerLock(tr, bmData));
				break;
			} catch (Error& e) {
				if (e.code() == error_code_granule_assignment_conflict) {
					if (BM_DEBUG) {
						fmt::print("BM {0} got lock out of date in lock check on conflict! Dying\n", bmData->epoch);
					}
					if (bmData->iAmReplaced.canBeSet()) {
						bmData->iAmReplaced.send(Void());
					}
					return Void();
				}
				wait(tr->onError(e));
				if (BM_DEBUG) {
					fmt::print("BM {0} still ok after checking lock on conflict\n", bmData->epoch);
				}
			}
		}
		bmData->doLockCheck = Promise<Void>();
	}
}

ACTOR Future<Void> blobManager(BlobManagerInterface bmInterf,
                               Reference<AsyncVar<ServerDBInfo> const> dbInfo,
                               int64_t epoch) {
	state Reference<BlobManagerData> self =
	    makeReference<BlobManagerData>(deterministicRandom()->randomUniqueID(),
	                                   openDBOnServer(dbInfo, TaskPriority::DefaultEndpoint, LockAware::True),
	                                   bmInterf.locality.dcId());

	state Future<Void> collection = actorCollection(self->addActor.getFuture());

	if (BM_DEBUG) {
		fmt::print("Blob manager {0} starting...\n", epoch);
	}
	TraceEvent("BlobManagerInit", bmInterf.id()).detail("Epoch", epoch).log();

	self->epoch = epoch;

	// although we start the recruiter, we wait until existing workers are ack'd
	auto recruitBlobWorker = IAsyncListener<RequestStream<RecruitBlobWorkerRequest>>::create(
	    dbInfo, [](auto const& info) { return info.clusterInterface.recruitBlobWorker; });
	self->addActor.send(blobWorkerRecruiter(self, recruitBlobWorker));

	// we need to recover the old blob manager's state (e.g. granule assignments) before
	// before the new blob manager does anything
	wait(recoverBlobManager(self));

	self->addActor.send(doLockChecks(self));
	self->addActor.send(monitorClientRanges(self));
	self->addActor.send(rangeAssigner(self));
	self->addActor.send(monitorPruneKeys(self));

	if (BUGGIFY) {
		self->addActor.send(chaosRangeMover(self));
	}

	// TODO probably other things here eventually
	try {
		loop choose {
			when(wait(self->iAmReplaced.getFuture())) {
				if (BM_DEBUG) {
					printf("Blob Manager exiting because it is replaced\n");
				}
				break;
			}
			when(HaltBlobManagerRequest req = waitNext(bmInterf.haltBlobManager.getFuture())) {
				req.reply.send(Void());
				TraceEvent("BlobManagerHalted", bmInterf.id()).detail("ReqID", req.requesterID);
				break;
			}
			when(state HaltBlobGranulesRequest req = waitNext(bmInterf.haltBlobGranules.getFuture())) {
				wait(haltBlobGranules(self));
				req.reply.send(Void());
				TraceEvent("BlobGranulesHalted", bmInterf.id()).detail("ReqID", req.requesterID);
				break;
			}
			when(wait(collection)) {
				TraceEvent("BlobManagerActorCollectionError");
				ASSERT(false);
				throw internal_error();
			}
		}
	} catch (Error& err) {
		TraceEvent("BlobManagerDied", bmInterf.id()).error(err, true);
	}
	return Void();
}

// Test:
// start empty
// DB has [A - B). That should show up in knownBlobRanges and should be in added
// DB has nothing. knownBlobRanges should be empty and [A - B) should be in removed
// DB has [A - B) and [C - D). They should both show up in knownBlobRanges and added.
// DB has [A - D). It should show up coalesced in knownBlobRanges, and [B - C) should be in added.
// DB has [A - C). It should show up coalesced in knownBlobRanges, and [C - D) should be in removed.
// DB has [B - C). It should show up coalesced in knownBlobRanges, and [A - B) should be removed.
// DB has [B - D). It should show up coalesced in knownBlobRanges, and [C - D) should be removed.
// DB has [A - D). It should show up coalesced in knownBlobRanges, and [A - B) should be removed.
// DB has [A - B) and [C - D). They should show up in knownBlobRanges, and [B - C) should be in removed.
// DB has [B - C). It should show up in knownBlobRanges, [B - C) should be in added, and [A - B) and [C - D)
// should be in removed.
TEST_CASE(":/blobmanager/updateranges") {
	KeyRangeMap<bool> knownBlobRanges(false, normalKeys.end);
	Arena ar;

	VectorRef<KeyRangeRef> added;
	VectorRef<KeyRangeRef> removed;

	StringRef active = LiteralStringRef("1");
	StringRef inactive = StringRef();

	RangeResult dbDataEmpty;
	std::vector<std::pair<KeyRangeRef, bool>> kbrRanges;

	StringRef keyA = StringRef(ar, LiteralStringRef("A"));
	StringRef keyB = StringRef(ar, LiteralStringRef("B"));
	StringRef keyC = StringRef(ar, LiteralStringRef("C"));
	StringRef keyD = StringRef(ar, LiteralStringRef("D"));

	// db data setup
	RangeResult dbDataAB;
	dbDataAB.emplace_back(ar, keyA, active);
	dbDataAB.emplace_back(ar, keyB, inactive);

	RangeResult dbDataAC;
	dbDataAC.emplace_back(ar, keyA, active);
	dbDataAC.emplace_back(ar, keyC, inactive);

	RangeResult dbDataAD;
	dbDataAD.emplace_back(ar, keyA, active);
	dbDataAD.emplace_back(ar, keyD, inactive);

	RangeResult dbDataBC;
	dbDataBC.emplace_back(ar, keyB, active);
	dbDataBC.emplace_back(ar, keyC, inactive);

	RangeResult dbDataBD;
	dbDataBD.emplace_back(ar, keyB, active);
	dbDataBD.emplace_back(ar, keyD, inactive);

	RangeResult dbDataCD;
	dbDataCD.emplace_back(ar, keyC, active);
	dbDataCD.emplace_back(ar, keyD, inactive);

	RangeResult dbDataAB_CD;
	dbDataAB_CD.emplace_back(ar, keyA, active);
	dbDataAB_CD.emplace_back(ar, keyB, inactive);
	dbDataAB_CD.emplace_back(ar, keyC, active);
	dbDataAB_CD.emplace_back(ar, keyD, inactive);

	// key ranges setup
	KeyRangeRef rangeAB = KeyRangeRef(keyA, keyB);
	KeyRangeRef rangeAC = KeyRangeRef(keyA, keyC);
	KeyRangeRef rangeAD = KeyRangeRef(keyA, keyD);

	KeyRangeRef rangeBC = KeyRangeRef(keyB, keyC);
	KeyRangeRef rangeBD = KeyRangeRef(keyB, keyD);

	KeyRangeRef rangeCD = KeyRangeRef(keyC, keyD);

	KeyRangeRef rangeStartToA = KeyRangeRef(normalKeys.begin, keyA);
	KeyRangeRef rangeStartToB = KeyRangeRef(normalKeys.begin, keyB);
	KeyRangeRef rangeStartToC = KeyRangeRef(normalKeys.begin, keyC);
	KeyRangeRef rangeBToEnd = KeyRangeRef(keyB, normalKeys.end);
	KeyRangeRef rangeCToEnd = KeyRangeRef(keyC, normalKeys.end);
	KeyRangeRef rangeDToEnd = KeyRangeRef(keyD, normalKeys.end);

	// actual test

	getRanges(kbrRanges, knownBlobRanges);
	ASSERT(kbrRanges.size() == 1);
	ASSERT(kbrRanges[0].first == normalKeys);
	ASSERT(!kbrRanges[0].second);

	// DB has [A - B)
	kbrRanges.clear();
	added.clear();
	removed.clear();
	updateClientBlobRanges(&knownBlobRanges, dbDataAB, ar, &added, &removed);

	ASSERT(added.size() == 1);
	ASSERT(added[0] == rangeAB);

	ASSERT(removed.size() == 0);

	getRanges(kbrRanges, knownBlobRanges);
	ASSERT(kbrRanges.size() == 3);
	ASSERT(kbrRanges[0].first == rangeStartToA);
	ASSERT(!kbrRanges[0].second);
	ASSERT(kbrRanges[1].first == rangeAB);
	ASSERT(kbrRanges[1].second);
	ASSERT(kbrRanges[2].first == rangeBToEnd);
	ASSERT(!kbrRanges[2].second);

	// DB has nothing
	kbrRanges.clear();
	added.clear();
	removed.clear();
	updateClientBlobRanges(&knownBlobRanges, dbDataEmpty, ar, &added, &removed);

	ASSERT(added.size() == 0);

	ASSERT(removed.size() == 1);
	ASSERT(removed[0] == rangeAB);

	getRanges(kbrRanges, knownBlobRanges);
	ASSERT(kbrRanges[0].first == normalKeys);
	ASSERT(!kbrRanges[0].second);

	// DB has [A - B) and [C - D)
	kbrRanges.clear();
	added.clear();
	removed.clear();
	updateClientBlobRanges(&knownBlobRanges, dbDataAB_CD, ar, &added, &removed);

	ASSERT(added.size() == 2);
	ASSERT(added[0] == rangeAB);
	ASSERT(added[1] == rangeCD);

	ASSERT(removed.size() == 0);

	getRanges(kbrRanges, knownBlobRanges);
	ASSERT(kbrRanges.size() == 5);
	ASSERT(kbrRanges[0].first == rangeStartToA);
	ASSERT(!kbrRanges[0].second);
	ASSERT(kbrRanges[1].first == rangeAB);
	ASSERT(kbrRanges[1].second);
	ASSERT(kbrRanges[2].first == rangeBC);
	ASSERT(!kbrRanges[2].second);
	ASSERT(kbrRanges[3].first == rangeCD);
	ASSERT(kbrRanges[3].second);
	ASSERT(kbrRanges[4].first == rangeDToEnd);
	ASSERT(!kbrRanges[4].second);

	// DB has [A - D)
	kbrRanges.clear();
	added.clear();
	removed.clear();
	updateClientBlobRanges(&knownBlobRanges, dbDataAD, ar, &added, &removed);

	ASSERT(added.size() == 1);
	ASSERT(added[0] == rangeBC);

	ASSERT(removed.size() == 0);

	getRanges(kbrRanges, knownBlobRanges);
	ASSERT(kbrRanges.size() == 3);
	ASSERT(kbrRanges[0].first == rangeStartToA);
	ASSERT(!kbrRanges[0].second);
	ASSERT(kbrRanges[1].first == rangeAD);
	ASSERT(kbrRanges[1].second);
	ASSERT(kbrRanges[2].first == rangeDToEnd);
	ASSERT(!kbrRanges[2].second);

	// DB has [A - C)
	kbrRanges.clear();
	added.clear();
	removed.clear();
	updateClientBlobRanges(&knownBlobRanges, dbDataAC, ar, &added, &removed);

	ASSERT(added.size() == 0);

	ASSERT(removed.size() == 1);
	ASSERT(removed[0] == rangeCD);

	getRanges(kbrRanges, knownBlobRanges);
	ASSERT(kbrRanges.size() == 3);
	ASSERT(kbrRanges[0].first == rangeStartToA);
	ASSERT(!kbrRanges[0].second);
	ASSERT(kbrRanges[1].first == rangeAC);
	ASSERT(kbrRanges[1].second);
	ASSERT(kbrRanges[2].first == rangeCToEnd);
	ASSERT(!kbrRanges[2].second);

	// DB has [B - C)
	kbrRanges.clear();
	added.clear();
	removed.clear();
	updateClientBlobRanges(&knownBlobRanges, dbDataBC, ar, &added, &removed);

	ASSERT(added.size() == 0);

	ASSERT(removed.size() == 1);
	ASSERT(removed[0] == rangeAB);

	getRanges(kbrRanges, knownBlobRanges);
	ASSERT(kbrRanges.size() == 3);
	ASSERT(kbrRanges[0].first == rangeStartToB);
	ASSERT(!kbrRanges[0].second);
	ASSERT(kbrRanges[1].first == rangeBC);
	ASSERT(kbrRanges[1].second);
	ASSERT(kbrRanges[2].first == rangeCToEnd);
	ASSERT(!kbrRanges[2].second);

	// DB has [B - D)
	kbrRanges.clear();
	added.clear();
	removed.clear();
	updateClientBlobRanges(&knownBlobRanges, dbDataBD, ar, &added, &removed);

	ASSERT(added.size() == 1);
	ASSERT(added[0] == rangeCD);

	ASSERT(removed.size() == 0);

	getRanges(kbrRanges, knownBlobRanges);
	ASSERT(kbrRanges.size() == 3);
	ASSERT(kbrRanges[0].first == rangeStartToB);
	ASSERT(!kbrRanges[0].second);
	ASSERT(kbrRanges[1].first == rangeBD);
	ASSERT(kbrRanges[1].second);
	ASSERT(kbrRanges[2].first == rangeDToEnd);
	ASSERT(!kbrRanges[2].second);

	// DB has [A - D)
	kbrRanges.clear();
	added.clear();
	removed.clear();
	updateClientBlobRanges(&knownBlobRanges, dbDataAD, ar, &added, &removed);

	ASSERT(added.size() == 1);
	ASSERT(added[0] == rangeAB);

	ASSERT(removed.size() == 0);

	getRanges(kbrRanges, knownBlobRanges);
	ASSERT(kbrRanges.size() == 3);
	ASSERT(kbrRanges[0].first == rangeStartToA);
	ASSERT(!kbrRanges[0].second);
	ASSERT(kbrRanges[1].first == rangeAD);
	ASSERT(kbrRanges[1].second);
	ASSERT(kbrRanges[2].first == rangeDToEnd);
	ASSERT(!kbrRanges[2].second);

	// DB has [A - B) and [C - D)
	kbrRanges.clear();
	added.clear();
	removed.clear();
	updateClientBlobRanges(&knownBlobRanges, dbDataAB_CD, ar, &added, &removed);

	ASSERT(added.size() == 0);

	ASSERT(removed.size() == 1);
	ASSERT(removed[0] == rangeBC);

	getRanges(kbrRanges, knownBlobRanges);
	ASSERT(kbrRanges.size() == 5);
	ASSERT(kbrRanges[0].first == rangeStartToA);
	ASSERT(!kbrRanges[0].second);
	ASSERT(kbrRanges[1].first == rangeAB);
	ASSERT(kbrRanges[1].second);
	ASSERT(kbrRanges[2].first == rangeBC);
	ASSERT(!kbrRanges[2].second);
	ASSERT(kbrRanges[3].first == rangeCD);
	ASSERT(kbrRanges[3].second);
	ASSERT(kbrRanges[4].first == rangeDToEnd);
	ASSERT(!kbrRanges[4].second);

	// DB has [B - C)
	kbrRanges.clear();
	added.clear();
	removed.clear();
	updateClientBlobRanges(&knownBlobRanges, dbDataBC, ar, &added, &removed);

	ASSERT(added.size() == 1);
	ASSERT(added[0] == rangeBC);

	ASSERT(removed.size() == 2);
	ASSERT(removed[0] == rangeAB);
	ASSERT(removed[1] == rangeCD);

	getRanges(kbrRanges, knownBlobRanges);
	ASSERT(kbrRanges.size() == 3);
	ASSERT(kbrRanges[0].first == rangeStartToB);
	ASSERT(!kbrRanges[0].second);
	ASSERT(kbrRanges[1].first == rangeBC);
	ASSERT(kbrRanges[1].second);
	ASSERT(kbrRanges[2].first == rangeCToEnd);
	ASSERT(!kbrRanges[2].second);

	return Void();
}
