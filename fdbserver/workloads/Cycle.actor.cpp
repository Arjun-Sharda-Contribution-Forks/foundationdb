/*
 * Cycle.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2022 Apple Inc. and the FoundationDB project authors
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

#include <cstring>

#include "flow/Arena.h"
#include "flow/IRandom.h"
#include "flow/Trace.h"
#include "flow/serialize.h"
#include "fdbrpc/simulator.h"
#include "fdbrpc/TokenSign.h"
#include "fdbclient/FDBOptions.g.h"
#include "fdbclient/NativeAPI.actor.h"
#include "fdbserver/TesterInterface.actor.h"
#include "fdbserver/workloads/workloads.actor.h"
#include "fdbserver/workloads/BulkSetup.actor.h"

#include "flow/actorcompiler.h" // This must be the last #include.

template <bool MultiTenancy>
struct CycleMembers {};

template <>
struct CycleMembers<true> {
	Arena arena;
	TenantName tenant;
	authz::jwt::TokenRef token;
	StringRef signedToken;
};

template <bool MultiTenancy>
struct CycleWorkload : TestWorkload, CycleMembers<MultiTenancy> {
	int actorCount, nodeCount;
	double testDuration, transactionsPerSecond, minExpectedTransactionsPerSecond, traceParentProbability;
	Key keyPrefix;

	std::vector<Future<Void>> clients;
	PerfIntCounter transactions, retries, tooOldRetries, commitFailedRetries;
	PerfDoubleCounter totalLatency;

	CycleWorkload(WorkloadContext const& wcx)
	  : TestWorkload(wcx), transactions("Transactions"), retries("Retries"), tooOldRetries("Retries.too_old"),
	    commitFailedRetries("Retries.commit_failed"), totalLatency("Latency") {
		testDuration = getOption(options, "testDuration"_sr, 10.0);
		transactionsPerSecond = getOption(options, "transactionsPerSecond"_sr, 5000.0) / clientCount;
		actorCount = getOption(options, "actorsPerClient"_sr, transactionsPerSecond / 5);
		nodeCount = getOption(options, "nodeCount"_sr, transactionsPerSecond * clientCount);
		keyPrefix = unprintable(getOption(options, "keyPrefix"_sr, LiteralStringRef("")).toString());
		traceParentProbability = getOption(options, "traceParentProbability"_sr, 0.01);
		minExpectedTransactionsPerSecond = transactionsPerSecond * getOption(options, "expectedRate"_sr, 0.7);
		if constexpr (MultiTenancy) {
			ASSERT(g_network->isSimulated());
			auto k = g_simulator.authKeys.begin();
			this->tenant = getOption(options, "tenant"_sr, "CycleTenant"_sr);
			// make it comfortably longer than the timeout of the workload
			auto currentTime = uint64_t(lround(g_network->timer()));
			this->token.algorithm = authz::Algorithm::ES256;
			this->token.issuedAtUnixTime = currentTime;
			this->token.expiresAtUnixTime =
			    currentTime + uint64_t(std::lround(getCheckTimeout())) + uint64_t(std::lround(testDuration)) + 100;
			this->token.keyId = k->first;
			this->token.notBeforeUnixTime = currentTime - 10;
			VectorRef<StringRef> tenants;
			tenants.push_back_deep(this->arena, this->tenant);
			this->token.tenants = tenants;
			// we currently don't support this workload to be run outside of simulation
			this->signedToken = authz::jwt::signToken(this->arena, this->token, k->second);
		}
	}

	template <bool MT = MultiTenancy>
	std::enable_if_t<MT, StringRef> getAuthToken() const {
		return this->signedToken;
	}

	std::string description() const override {
		if constexpr (MultiTenancy) {
			return "TenantCycleWorkload";
		} else {
			return "CycleWorkload";
		}
	}

	Future<Void> setup(Database const& cx) override {
		if constexpr (MultiTenancy) {
			cx->defaultTenant = this->tenant;
		}
		return bulkSetup(cx, this, nodeCount, Promise<double>());
	}
	Future<Void> start(Database const& cx) override {
		if constexpr (MultiTenancy) {
			cx->defaultTenant = this->tenant;
		}
		for (int c = 0; c < actorCount; c++)
			clients.push_back(
			    timeout(cycleClient(cx->clone(), this, actorCount / transactionsPerSecond), testDuration, Void()));
		return delay(testDuration);
	}
	Future<bool> check(Database const& cx) override {
		if constexpr (MultiTenancy) {
			cx->defaultTenant = this->tenant;
		}
		int errors = 0;
		for (int c = 0; c < clients.size(); c++)
			errors += clients[c].isError();
		if (errors)
			TraceEvent(SevError, "TestFailure").detail("Reason", "There were client errors.");
		clients.clear();
		return cycleCheck(cx->clone(), this, !errors);
	}
	void getMetrics(std::vector<PerfMetric>& m) override {
		m.push_back(transactions.getMetric());
		m.push_back(retries.getMetric());
		m.push_back(tooOldRetries.getMetric());
		m.push_back(commitFailedRetries.getMetric());
		m.emplace_back("Avg Latency (ms)", 1000 * totalLatency.getValue() / transactions.getValue(), Averaged::True);
		m.emplace_back("Read rows/simsec (approx)", transactions.getValue() * 3 / testDuration, Averaged::False);
		m.emplace_back("Write rows/simsec (approx)", transactions.getValue() * 4 / testDuration, Averaged::False);
	}

	Key keyForIndex(int n) { return key(n); }
	Key key(int n) { return doubleToTestKey((double)n / nodeCount, keyPrefix); }
	Value value(int n) { return doubleToTestKey(n, keyPrefix); }
	int fromValue(const ValueRef& v) { return testKeyToDouble(v, keyPrefix); }

	Standalone<KeyValueRef> operator()(int n) { return KeyValueRef(key(n), value((n + 1) % nodeCount)); }

	void badRead(const char* name, int r, Transaction& tr) {
		TraceEvent(SevError, "CycleBadRead")
		    .detail(name, r)
		    .detail("Key", printable(key(r)))
		    .detail("Version", tr.getReadVersion().get())
		    .detailf("From", "%016llx", debug_lastLoadBalanceResultEndpointToken);
	}

	template <bool B = MultiTenancy>
	std::enable_if_t<B> setAuthToken(Transaction& tr) {
		tr.setOption(FDBTransactionOptions::AUTHORIZATION_TOKEN, this->signedToken);
	}

	template <bool B = MultiTenancy>
	std::enable_if_t<!B> setAuthToken(Transaction& tr) {}

	ACTOR Future<Void> cycleClient(Database cx, CycleWorkload* self, double delay) {
		state double lastTime = now();
		try {
			loop {
				wait(poisson(&lastTime, delay));

				state double tstart = now();
				state int r = deterministicRandom()->randomInt(0, self->nodeCount);
				state Transaction tr(cx);
				self->setAuthToken(tr);
				if (deterministicRandom()->random01() <= self->traceParentProbability) {
					state Span span("CycleClient"_loc);
					TraceEvent("CycleTracingTransaction", span.context.traceID).log();
					tr.setOption(FDBTransactionOptions::SPAN_PARENT,
					             BinaryWriter::toValue(span.context, IncludeVersion()));
				}
				while (true) {
					try {
						// Reverse next and next^2 node
						Optional<Value> v = wait(tr.get(self->key(r)));
						if (!v.present())
							self->badRead("KeyR", r, tr);
						state int r2 = self->fromValue(v.get());
						Optional<Value> v2 = wait(tr.get(self->key(r2)));
						if (!v2.present())
							self->badRead("KeyR2", r2, tr);
						state int r3 = self->fromValue(v2.get());
						Optional<Value> v3 = wait(tr.get(self->key(r3)));
						if (!v3.present())
							self->badRead("KeyR3", r3, tr);
						int r4 = self->fromValue(v3.get());

						tr.clear(self->key(r)); //< Shouldn't have an effect, but will break with wrong ordering
						tr.set(self->key(r), self->value(r3));
						tr.set(self->key(r2), self->value(r4));
						tr.set(self->key(r3), self->value(r2));
						// TraceEvent("CyclicTest").detail("Key", self->key(r).toString()).detail("Value", self->value(r3).toString());
						// TraceEvent("CyclicTest").detail("Key", self->key(r2).toString()).detail("Value", self->value(r4).toString());
						// TraceEvent("CyclicTest").detail("Key", self->key(r3).toString()).detail("Value", self->value(r2).toString());

						wait(tr.commit());
						// TraceEvent("CycleCommit");
						break;
					} catch (Error& e) {
						if (e.code() == error_code_transaction_too_old)
							++self->tooOldRetries;
						else if (e.code() == error_code_not_committed)
							++self->commitFailedRetries;
						wait(tr.onError(e));
					}
					++self->retries;
				}
				++self->transactions;
				self->totalLatency += now() - tstart;
			}
		} catch (Error& e) {
			TraceEvent(SevError, "CycleClient").error(e);
			throw;
		}
	}

	void logTestData(const VectorRef<KeyValueRef>& data) {
		TraceEvent("TestFailureDetail").log();
		int index = 0;
		for (auto& entry : data) {
			TraceEvent("CurrentDataEntry")
			    .detail("Index", index)
			    .detail("Key", entry.key.toString())
			    .detail("Value", entry.value.toString());
			index++;
		}
	}

	bool cycleCheckData(const VectorRef<KeyValueRef>& data, Version v) {
		if (data.size() != nodeCount) {
			logTestData(data);
			TraceEvent(SevError, "TestFailure")
			    .detail("Reason", "Node count changed")
			    .detail("Before", nodeCount)
			    .detail("After", data.size())
			    .detail("Version", v)
			    .detail("KeyPrefix", keyPrefix.printable());
			TraceEvent(SevError, "TestFailureInfo")
			    .detail("DataSize", data.size())
			    .detail("NodeCount", nodeCount)
			    .detail("Workload", description());
			return false;
		}
		int i = 0;
		int iPrev = 0;
		double d;
		int c;
		for (c = 0; c < nodeCount; c++) {
			if (c && !i) {
				TraceEvent(SevError, "TestFailure")
				    .detail("Reason", "Cycle got shorter")
				    .detail("Before", nodeCount)
				    .detail("After", c)
				    .detail("KeyPrefix", keyPrefix.printable());
				logTestData(data);
				return false;
			}
			if (data[i].key != key(i)) {
				TraceEvent(SevError, "TestFailure")
				    .detail("Reason", "Key changed")
				    .detail("KeyPrefix", keyPrefix.printable());
				logTestData(data);
				return false;
			}
			d = testKeyToDouble(data[i].value, keyPrefix);
			iPrev = i;
			i = (int)d;
			if (i != d || i < 0 || i >= nodeCount) {
				TraceEvent(SevError, "TestFailure")
				    .detail("Reason", "Invalid value")
				    .detail("KeyPrefix", keyPrefix.printable());
				logTestData(data);
				return false;
			}
		}
		if (i != 0) {
			TraceEvent(SevError, "TestFailure")
			    .detail("Reason", "Cycle got longer")
			    .detail("KeyPrefix", keyPrefix.printable())
			    .detail("Key", key(i))
			    .detail("Value", data[i].value)
			    .detail("Iteration", c)
			    .detail("Nodecount", nodeCount)
			    .detail("Int", i)
			    .detail("Double", d)
			    .detail("ValuePrev", data[iPrev].value)
			    .detail("KeyPrev", data[iPrev].key);
			logTestData(data);
			return false;
		}
		return true;
	}

	ACTOR Future<bool> cycleCheck(Database cx, CycleWorkload* self, bool ok) {
		if (self->transactions.getMetric().value() < self->testDuration * self->minExpectedTransactionsPerSecond) {
			TraceEvent(SevWarnAlways, "TestFailure")
			    .detail("Reason", "Rate below desired rate")
			    .detail("File", __FILE__)
			    .detail(
			        "Details",
			        format("%.2f",
			               self->transactions.getMetric().value() / (self->transactionsPerSecond * self->testDuration)))
			    .detail("TransactionsAchieved", self->transactions.getMetric().value())
			    .detail("MinTransactionsExpected", self->testDuration * self->minExpectedTransactionsPerSecond)
			    .detail("TransactionGoal", self->transactionsPerSecond * self->testDuration);
			ok = false;
		}
		if (!self->clientId) {
			// One client checks the validity of the cycle
			state Transaction tr(cx);
			state int retryCount = 0;
			self->setAuthToken(tr);
			loop {
				try {
					state Version v = wait(tr.getReadVersion());
					RangeResult data = wait(tr.getRange(firstGreaterOrEqual(doubleToTestKey(0.0, self->keyPrefix)),
					                                    firstGreaterOrEqual(doubleToTestKey(1.0, self->keyPrefix)),
					                                    self->nodeCount + 1));
					ok = self->cycleCheckData(data, v) && ok;
					break;
				} catch (Error& e) {
					retryCount++;
					TraceEvent(retryCount > 20 ? SevWarnAlways : SevWarn, "CycleCheckError").error(e);
					if (g_network->isSimulated() && retryCount > 50) {
						CODE_PROBE(true, "Cycle check enable speedUpSimulation because too many transaction_too_old()");
						// try to make the read window back to normal size (5 * version_per_sec)
						g_simulator.speedUpSimulation = true;
					}
					wait(tr.onError(e));
				}
			}
		}
		return ok;
	}
};

WorkloadFactory<CycleWorkload<false>> CycleWorkloadFactory("Cycle", false);
WorkloadFactory<CycleWorkload<true>> TenantCycleWorkloadFactory("TenantCycle", true);
