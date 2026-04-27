// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_DB_COMPACTION_TRACE_WRITER_H_
#define STORAGE_LEVELDB_DB_COMPACTION_TRACE_WRITER_H_

#include <cstdint>
#include <string>

#include "leveldb/env.h"
#include "leveldb/status.h"

#include "port/port.h"

namespace leveldb {

// Returns the OS-level thread ID (Linux: gettid, Windows: GetCurrentThreadId,
// other: 0).
uint64_t CurrentOsThreadId();

// Writes simplified compaction events to a CSV file with a fixed schema.
// All public methods are thread-safe.
//
// CSV columns (10 total):
//   trace_ts_us, event_index, event, db_name, thread_id,
//   file_number, file_name, manifest_file_number, status, notes
class CompactionTraceWriter {
 public:
  // Payload for a single trace row.
  // Sentinel values for omitted fields:
  //   uint64_t thread_id / file_number / manifest_file_number: 0
  //   std::string: empty
  struct Row {
    uint64_t trace_ts_us = 0;
    std::string event;
    std::string db_name;
    uint64_t thread_id = 0;    // 0 = omit
    uint64_t file_number = 0;  // 0 = omit
    std::string file_name;
    uint64_t manifest_file_number = 0;  // 0 = omit
    std::string status;
    std::string notes;
  };

  // Opens (or creates) the file at path and appends rows.
  // Writes the CSV header only when opening an empty file.
  // Returns nullptr if the file cannot be opened.
  static CompactionTraceWriter* Open(Env* env, const std::string& path);

  ~CompactionTraceWriter();

  // Appends one CSV row to the trace file and flushes.
  // Thread-safe.
  void Write(const Row& row);

 private:
  explicit CompactionTraceWriter(WritableFile* file);

  port::Mutex mu_;
  WritableFile* file_ GUARDED_BY(mu_);
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_COMPACTION_TRACE_WRITER_H_
