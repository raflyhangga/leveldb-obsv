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

// Writes compaction lifecycle events to a CSV file with a fixed schema.
// All public methods are thread-safe.
//
// CSV columns (26 total):
//   trace_ts_us, event_index, job_id, event_type, db_name, cf_name,
//   is_manual, is_trivial_move, compaction_reason,
//   source_level, target_level, bg_thread_id,
//   file_number, file_name, file_size,
//   smallest_user_key, largest_user_key, seqno_smallest, seqno_largest,
//   output_level, status,
//   bytes_read_logical, bytes_written_logical,
//   input_count, output_count, notes
//
// String key fields are hex-encoded to safely embed binary bytes in CSV.
// String fields that may contain commas or quotes are RFC-4180-escaped.
class CompactionTraceWriter {
 public:
  // Payload for a single compaction trace row.
  // Sentinel values for omitted fields:
  //   int: -1
  //   uint64_t file_number / bg_thread_id / bytes_*: 0
  //   std::string: empty
  //   bool has_file_keys: false
  struct Row {
    uint64_t trace_ts_us = 0;
    uint64_t job_id = 0;
    std::string event_type;
    std::string db_name;

    // Job-level fields (typically set on job_start).
    int is_manual = -1;         // -1 = omit; 0/1 = value
    int is_trivial_move = -1;   // -1 = omit; 0/1 = value
    std::string compaction_reason;
    int source_level = -1;
    int target_level = -1;
    uint64_t bg_thread_id = 0;  // 0 = omit

    // File-level fields.
    uint64_t file_number = 0;   // 0 = omit
    std::string file_name;
    uint64_t file_size = 0;     // 0 = omit

    // Key range fields (raw user-key bytes; hex-encoded in CSV output).
    bool has_file_keys = false;
    std::string smallest_user_key;
    std::string largest_user_key;
    uint64_t seqno_smallest = 0;
    uint64_t seqno_largest = 0;

    int output_level = -1;      // -1 = omit

    // End / install fields.
    std::string status;
    uint64_t bytes_read_logical = 0;
    uint64_t bytes_written_logical = 0;
    int input_count = -1;
    int output_count = -1;
    std::string notes;
  };

  // Opens (or creates) the file at path and writes the CSV header once.
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
  uint64_t next_event_index_ GUARDED_BY(mu_);
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_COMPACTION_TRACE_WRITER_H_
