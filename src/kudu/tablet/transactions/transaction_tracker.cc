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

#include "kudu/tablet/transactions/transaction_tracker.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <mutex>
#include <ostream>
#include <string>
#include <vector>

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <google/protobuf/message.h>

#include "kudu/gutil/map-util.h"
#include "kudu/gutil/port.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/tablet/tablet.h"
#include "kudu/tablet/tablet_replica.h"
#include "kudu/tablet/transactions/transaction.h"
#include "kudu/tablet/transactions/transaction_driver.h"
#include "kudu/util/flag_tags.h"
#include "kudu/util/flag_validators.h"
#include "kudu/util/logging.h"
#include "kudu/util/mem_tracker.h"
#include "kudu/util/metrics.h"
#include "kudu/util/monotime.h"

DEFINE_int64(tablet_transaction_memory_limit_mb, 64,
             "Maximum amount of memory that may be consumed by all in-flight "
             "transactions belonging to a particular tablet. When this limit "
             "is reached, new transactions will be rejected and clients will "
             "be forced to retry them. If -1, transaction memory tracking is "
             "disabled.");
TAG_FLAG(tablet_transaction_memory_limit_mb, advanced);

DECLARE_int64(rpc_max_message_size);

METRIC_DEFINE_gauge_uint64(tablet, all_transactions_inflight,
                           "Transactions In Flight",
                           kudu::MetricUnit::kTransactions,
                           "Number of transactions currently in-flight, including any type.",
                           kudu::MetricLevel::kDebug);
METRIC_DEFINE_gauge_uint64(tablet, write_transactions_inflight,
                           "Write Transactions In Flight",
                           kudu::MetricUnit::kTransactions,
                           "Number of write transactions currently in-flight",
                           kudu::MetricLevel::kDebug);
METRIC_DEFINE_gauge_uint64(tablet, alter_schema_transactions_inflight,
                           "Alter Schema Transactions In Flight",
                           kudu::MetricUnit::kTransactions,
                           "Number of alter schema transactions currently in-flight",
                           kudu::MetricLevel::kDebug);

METRIC_DEFINE_counter(tablet, transaction_memory_pressure_rejections,
                      "Transaction Memory Pressure Rejections",
                      kudu::MetricUnit::kTransactions,
                      "Number of transactions rejected because the tablet's transaction"
                      "memory usage exceeds the transaction memory limit or the limit"
                      "of an ancestral tracker.",
                      kudu::MetricLevel::kWarn);

METRIC_DEFINE_counter(tablet, transaction_memory_limit_rejections,
                      "Tablet Transaction Memory Limit Rejections",
                      kudu::MetricUnit::kTransactions,
                      "Number of transactions rejected because the tablet's "
                      "transaction memory limit was reached.",
                      kudu::MetricLevel::kWarn);

using std::shared_ptr;
using std::string;
using std::vector;
using strings::Substitute;

static bool ValidateTransactionMemoryLimit(const char* flagname, int64_t value) {
  // -1 is a special value for the  --tablet_transaction_memory_limit_mb flag.
  if (value < -1) {
    LOG(ERROR) << Substitute("$0: invalid value for flag $1", value, flagname);
    return false;
  }
  return true;
}
DEFINE_validator(tablet_transaction_memory_limit_mb, ValidateTransactionMemoryLimit);

static bool ValidateTransactionMemoryAndRpcSize() {
  const int64_t transaction_max_size =
      FLAGS_tablet_transaction_memory_limit_mb * 1024 * 1024;
  const int64_t rpc_max_size = FLAGS_rpc_max_message_size;
  if (transaction_max_size >= 0 && transaction_max_size < rpc_max_size) {
    LOG(ERROR) << Substitute(
        "--tablet_transaction_memory_limit_mb is set too low compared with "
        "--rpc_max_message_size; increase --tablet_transaction_memory_limit_mb "
        "at least up to $0", (rpc_max_size + 1024 * 1024 - 1) / (1024 * 1024));
    return false;
  }
  return true;
}
GROUP_FLAG_VALIDATOR(transaction_memory_and_rpc_size,
                     ValidateTransactionMemoryAndRpcSize);

namespace kudu {
namespace tablet {

#define MINIT(x) x(METRIC_##x.Instantiate(entity))
#define GINIT(x) x(METRIC_##x.Instantiate(entity, 0))
TransactionTracker::Metrics::Metrics(const scoped_refptr<MetricEntity>& entity)
    : GINIT(all_transactions_inflight),
      GINIT(write_transactions_inflight),
      GINIT(alter_schema_transactions_inflight),
      MINIT(transaction_memory_pressure_rejections),
      MINIT(transaction_memory_limit_rejections) {
}
#undef GINIT
#undef MINIT

TransactionTracker::State::State()
  : memory_footprint(0) {
}

TransactionTracker::TransactionTracker() {
}

TransactionTracker::~TransactionTracker() {
  std::lock_guard<simple_spinlock> l(lock_);
  CHECK_EQ(pending_txns_.size(), 0);
}

Status TransactionTracker::Add(TransactionDriver* driver) {
  int64_t driver_mem_footprint = driver->state()->request()->SpaceUsed();
  if (mem_tracker_ && !mem_tracker_->TryConsume(driver_mem_footprint)) {
    if (metrics_) {
      metrics_->transaction_memory_pressure_rejections->Increment();
      if (!mem_tracker_->CanConsumeNoAncestors(driver_mem_footprint)) {
        metrics_->transaction_memory_limit_rejections->Increment();
      }
    }

    // May be null in unit tests.
    TabletReplica* replica = driver->state()->tablet_replica();

    string msg = Substitute(
        "transaction on tablet $0 rejected due to memory pressure: the memory "
        "usage of this transaction ($1) plus the current consumption ($2) "
        "exceeds the transaction memory limit ($3) or the limit of an ancestral "
        "memory tracker.",
        replica ? replica->tablet()->tablet_id() : "(unknown)",
        driver_mem_footprint, mem_tracker_->consumption(), mem_tracker_->limit());

    KLOG_EVERY_N_SECS(WARNING, 1) << msg << THROTTLE_MSG;

    return Status::ServiceUnavailable(msg);
  }

  IncrementCounters(*driver);

  // Cache the transaction memory footprint so we needn't refer to the request
  // again, as it may disappear between now and then.
  State st;
  st.memory_footprint = driver_mem_footprint;
  std::lock_guard<simple_spinlock> l(lock_);
  InsertOrDie(&pending_txns_, driver, st);
  return Status::OK();
}

void TransactionTracker::IncrementCounters(const TransactionDriver& driver) const {
  if (!metrics_) {
    return;
  }

  metrics_->all_transactions_inflight->Increment();
  switch (driver.tx_type()) {
    case Transaction::WRITE_TXN:
      metrics_->write_transactions_inflight->Increment();
      break;
    case Transaction::ALTER_SCHEMA_TXN:
      metrics_->alter_schema_transactions_inflight->Increment();
      break;
  }
}

void TransactionTracker::DecrementCounters(const TransactionDriver& driver) const {
  if (!metrics_) {
    return;
  }

  DCHECK_GT(metrics_->all_transactions_inflight->value(), 0);
  metrics_->all_transactions_inflight->Decrement();
  switch (driver.tx_type()) {
    case Transaction::WRITE_TXN:
      DCHECK_GT(metrics_->write_transactions_inflight->value(), 0);
      metrics_->write_transactions_inflight->Decrement();
      break;
    case Transaction::ALTER_SCHEMA_TXN:
      DCHECK_GT(metrics_->alter_schema_transactions_inflight->value(), 0);
      metrics_->alter_schema_transactions_inflight->Decrement();
      break;
  }
}

void TransactionTracker::Release(TransactionDriver* driver) {
  DecrementCounters(*driver);

  // Remove the transaction from the map updating memory consumption if needed.
  std::lock_guard<simple_spinlock> l(lock_);
  State st = FindOrDie(pending_txns_, driver);
  if (mem_tracker_) {
    mem_tracker_->Release(st.memory_footprint);
  }
  if (PREDICT_FALSE(pending_txns_.erase(driver) != 1)) {
    LOG(FATAL) << "Could not remove pending transaction from map: "
        << driver->ToStringUnlocked();
  }
}

void TransactionTracker::GetPendingTransactions(
    vector<scoped_refptr<TransactionDriver> >* pending_out) const {
  DCHECK(pending_out->empty());
  std::lock_guard<simple_spinlock> l(lock_);
  for (const TxnMap::value_type& e : pending_txns_) {
    // Increments refcount of each transaction.
    pending_out->push_back(e.first);
  }
}

int TransactionTracker::GetNumPendingForTests() const {
  std::lock_guard<simple_spinlock> l(lock_);
  return pending_txns_.size();
}

void TransactionTracker::WaitForAllToFinish() const {
  // Wait indefinitely.
  CHECK_OK(WaitForAllToFinish(MonoDelta::FromNanoseconds(std::numeric_limits<int64_t>::max())));
}

Status TransactionTracker::WaitForAllToFinish(const MonoDelta& timeout) const {
  static constexpr size_t kMaxTxnsToPrint = 50;
  int wait_time_us = 250;
  int num_complaints = 0;
  MonoTime start_time = MonoTime::Now();
  MonoTime next_log_time = start_time + MonoDelta::FromSeconds(1);

  while (1) {
    vector<scoped_refptr<TransactionDriver> > txns;
    GetPendingTransactions(&txns);

    if (txns.empty()) {
      break;
    }

    MonoTime now = MonoTime::Now();
    MonoDelta diff = now - start_time;
    if (diff > timeout) {
      return Status::TimedOut(Substitute("Timed out waiting for all transactions to finish. "
                                         "$0 transactions pending. Waited for $1",
                                         txns.size(), diff.ToString()));
    }
    if (now > next_log_time) {
      LOG(WARNING) << Substitute("TransactionTracker waiting for $0 outstanding transactions to"
                                 " complete now for $1", txns.size(), diff.ToString());
      LOG(INFO) << Substitute("Dumping up to $0 currently running transactions: ",
                              kMaxTxnsToPrint);
      const auto num_txn_limit = std::min(txns.size(), kMaxTxnsToPrint);
      for (auto i = 0; i < num_txn_limit; i++) {
        LOG(INFO) << txns[i]->ToString();
      }

      num_complaints++;
      // Exponential back-off on how often the transactions are dumped.
      next_log_time = now + MonoDelta::FromSeconds(1 << std::min(8, num_complaints));
    }
    wait_time_us = std::min(wait_time_us * 5 / 4, 1000000);
    SleepFor(MonoDelta::FromMicroseconds(wait_time_us));
  }
  return Status::OK();
}

void TransactionTracker::StartInstrumentation(
    const scoped_refptr<MetricEntity>& metric_entity) {
  metrics_.reset(new Metrics(metric_entity));
}

void TransactionTracker::StartMemoryTracking(
    const shared_ptr<MemTracker>& parent_mem_tracker) {
  if (FLAGS_tablet_transaction_memory_limit_mb != -1) {
    mem_tracker_ = MemTracker::CreateTracker(
        FLAGS_tablet_transaction_memory_limit_mb * 1024 * 1024,
        "txn_tracker",
        parent_mem_tracker);
  }
}

}  // namespace tablet
}  // namespace kudu
