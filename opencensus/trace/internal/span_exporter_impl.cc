// Copyright 2017, OpenCensus Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "opencensus/trace/internal/span_exporter_impl.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "absl/synchronization/mutex.h"
#include "opencensus/trace/exporter/span_data.h"
#include "opencensus/trace/exporter/span_exporter.h"

namespace opencensus {
namespace trace {
namespace exporter {

SpanExporterImpl* SpanExporterImpl::Get() {
  static auto global_span_exporter_impl = std::unique_ptr<SpanExporterImpl>(new SpanExporterImpl);
  return global_span_exporter_impl.get();
}

void SpanExporterImpl::Shutdown() {
  if (!thread_started_)
    return;
  shutdown_ = true;
  collect_spans_ = false;
  t_.join();
  thread_started_ = false;
  shutdown_ = false;
}

SpanExporterImpl::~SpanExporterImpl() {
  Shutdown();
}

void SpanExporterImpl::SetBatchSize(int size) {
  absl::MutexLock l(&handler_mu_);
  batch_size_ = std::max(1, size);
}

void SpanExporterImpl::SetInterval(absl::Duration interval) {
  absl::MutexLock l(&handler_mu_);
  interval_ = std::max(absl::Seconds(1), interval);
}

void SpanExporterImpl::RegisterHandler(
    std::unique_ptr<SpanExporter::Handler> handler) {
  absl::MutexLock l(&handler_mu_);
  handlers_.emplace_back(std::move(handler));
  if (!thread_started_) {
    StartExportThread();
  }
}

void SpanExporterImpl::AddSpan(
    const std::shared_ptr<opencensus::trace::SpanImpl>& span_impl) {
  absl::MutexLock l(&span_mu_);
  if (!collect_spans_) return;
  spans_.emplace_back(span_impl);
}

void SpanExporterImpl::StartExportThread() {
  t_ = std::thread(&SpanExporterImpl::RunWorkerLoop, this);
  thread_started_ = true;
  absl::MutexLock l(&span_mu_);
  collect_spans_ = true;
}

bool SpanExporterImpl::IsBatchFull() const {
  span_mu_.AssertHeld();
  return spans_.size() >= cached_batch_size_;
}

bool SpanExporterImpl::ShouldExport() const {
  return IsBatchFull() || shutdown_;
}

void SpanExporterImpl::RunWorkerLoop() {
  std::vector<opencensus::trace::exporter::SpanData> span_data;
  std::vector<std::shared_ptr<opencensus::trace::SpanImpl>> batch;
  bool stop = false;
  while (!stop) {
    int size;
    absl::Time next_forced_export_time;
    {
      // Start of loop, update batch size and interval.
      absl::MutexLock l(&handler_mu_);
      size = batch_size_;
      next_forced_export_time = absl::Now() + interval_;
    }
    {
      absl::MutexLock l(&span_mu_);
      cached_batch_size_ = size;
      // Wait until batch is full or interval time has been exceeded.
      span_mu_.AwaitWithDeadline(
          absl::Condition(this, &SpanExporterImpl::ShouldExport),
          next_forced_export_time);
      if (spans_.empty()) {
        if (shutdown_) {
          stop = true;
        }
        continue;
      }
      std::swap(batch, spans_);
    }
    for (const auto& span : batch) {
      span_data.emplace_back(span->ToSpanData());
    }
    batch.clear();
    Export(span_data);
    span_data.clear();
  }
}

void SpanExporterImpl::Export(const std::vector<SpanData>& span_data) {
  // Call each registered handler.
  absl::MutexLock lock(&handler_mu_);
  for (const auto& handler : handlers_) {
    handler->Export(span_data);
  }
}

void SpanExporterImpl::ExportForTesting() {
  std::vector<opencensus::trace::exporter::SpanData> span_data_;
  std::vector<std::shared_ptr<opencensus::trace::SpanImpl>> batch_;
  {
    absl::MutexLock l(&span_mu_);
    std::swap(batch_, spans_);
  }
  span_data_.reserve(batch_.size());
  for (const auto& span : batch_) {
    span_data_.emplace_back(span->ToSpanData());
  }
  Export(span_data_);
}

}  // namespace exporter
}  // namespace trace
}  // namespace opencensus
