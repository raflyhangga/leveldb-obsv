// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/compaction_trace_writer.h"

#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <cstdio>
#include <string>

#include "util/mutexlock.h"

namespace leveldb {

uint64_t CurrentOsThreadId() {
#if defined(__linux__)
  return static_cast<uint64_t>(::syscall(SYS_gettid));
#elif defined(_WIN32)
  return static_cast<uint64_t>(::GetCurrentThreadId());
#else
  return 0;
#endif
}

namespace {

static const char kCsvHeader[] =
    "trace_ts_us,event_index,job_id,event_type,db_name,cf_name,"
    "is_manual,is_trivial_move,compaction_reason,"
    "source_level,target_level,bg_thread_id,"
    "file_number,file_name,file_size,"
    "smallest_user_key,largest_user_key,seqno_smallest,seqno_largest,"
    "output_level,status,"
    "bytes_read_logical,bytes_written_logical,"
    "input_count,output_count,notes\n";

// Hex-encodes arbitrary bytes for safe CSV embedding.
std::string HexEncode(const std::string& s) {
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(s.size() * 2);
  for (unsigned char c : s) {
    out += kHex[c >> 4];
    out += kHex[c & 0xf];
  }
  return out;
}

// Wraps s in double-quotes and escapes internal double-quotes per RFC 4180.
std::string CsvEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 2);
  out += '"';
  for (char c : s) {
    if (c == '"') out += '"';
    out += c;
  }
  out += '"';
  return out;
}

}  // namespace

CompactionTraceWriter* CompactionTraceWriter::Open(Env* env,
                                                    const std::string& path) {
  WritableFile* file;
  Status s = env->NewWritableFile(path, &file);
  if (!s.ok()) return nullptr;
  auto* writer = new CompactionTraceWriter(file);
  s = file->Append(kCsvHeader);
  if (s.ok()) s = file->Flush();
  if (!s.ok()) {
    delete writer;
    return nullptr;
  }
  return writer;
}

CompactionTraceWriter::CompactionTraceWriter(WritableFile* file)
    : file_(file), next_event_index_(0) {}

CompactionTraceWriter::~CompactionTraceWriter() {
  file_->Flush();
  file_->Close();
  delete file_;
}

void CompactionTraceWriter::Write(const Row& row) {
  MutexLock lock(&mu_);
  uint64_t idx = next_event_index_++;

  char buf[64];
  std::string line;
  line.reserve(512);

  // Appends a uint64 value followed by a comma separator.
  auto uint_csv = [&](uint64_t v) {
    snprintf(buf, sizeof(buf), "%llu,", static_cast<unsigned long long>(v));
    line += buf;
  };

  // Appends an int value (omitted when v < 0) followed by a comma separator.
  auto int_csv = [&](int v) {
    if (v >= 0) {
      snprintf(buf, sizeof(buf), "%d", v);
      line += buf;
    }
    line += ',';
  };

  // Appends a pre-formatted string followed by a comma separator.
  auto str_csv = [&](const std::string& v) {
    line += v;
    line += ',';
  };

  // Column 1: trace_ts_us
  uint_csv(row.trace_ts_us);
  // Column 2: event_index (global monotonic)
  uint_csv(idx);
  // Column 3: job_id
  uint_csv(row.job_id);
  // Column 4: event_type
  str_csv(row.event_type);
  // Column 5: db_name (escaped; may be a filesystem path)
  str_csv(row.db_name.empty() ? "" : CsvEscape(row.db_name));
  // Column 6: cf_name (always "default" in vanilla LevelDB)
  line += "default,";
  // Column 7: is_manual
  int_csv(row.is_manual);
  // Column 8: is_trivial_move
  int_csv(row.is_trivial_move);
  // Column 9: compaction_reason
  str_csv(row.compaction_reason);
  // Column 10: source_level
  int_csv(row.source_level);
  // Column 11: target_level
  int_csv(row.target_level);
  // Column 12: bg_thread_id (0 = omit)
  if (row.bg_thread_id != 0) {
    snprintf(buf, sizeof(buf), "%llu",
             static_cast<unsigned long long>(row.bg_thread_id));
    line += buf;
  }
  line += ',';
  // Column 13: file_number (0 = omit)
  if (row.file_number != 0) {
    snprintf(buf, sizeof(buf), "%llu",
             static_cast<unsigned long long>(row.file_number));
    line += buf;
  }
  line += ',';
  // Column 14: file_name (escaped)
  str_csv(row.file_name.empty() ? "" : CsvEscape(row.file_name));
  // Column 15: file_size (0 = omit)
  if (row.file_size != 0) {
    snprintf(buf, sizeof(buf), "%llu",
             static_cast<unsigned long long>(row.file_size));
    line += buf;
  }
  line += ',';
  // Columns 16-19: key range fields (hex-encoded user keys + seqnos).
  // When has_file_keys is false, all four columns are emitted empty.
  if (row.has_file_keys) {
    str_csv(HexEncode(row.smallest_user_key));
    str_csv(HexEncode(row.largest_user_key));
    snprintf(buf, sizeof(buf), "%llu,",
             static_cast<unsigned long long>(row.seqno_smallest));
    line += buf;
    snprintf(buf, sizeof(buf), "%llu,",
             static_cast<unsigned long long>(row.seqno_largest));
    line += buf;
  } else {
    // Four empty columns; the preceding comma (after file_size) plus these four
    // commas produce the five separators needed to span fields 15–20.
    line += ",,,,";
  }
  // Column 20: output_level
  int_csv(row.output_level);
  // Column 21: status (escaped; may be a LevelDB error string)
  str_csv(row.status.empty() ? "" : CsvEscape(row.status));
  // Column 22: bytes_read_logical (0 = omit)
  if (row.bytes_read_logical != 0) {
    snprintf(buf, sizeof(buf), "%llu",
             static_cast<unsigned long long>(row.bytes_read_logical));
    line += buf;
  }
  line += ',';
  // Column 23: bytes_written_logical (0 = omit)
  if (row.bytes_written_logical != 0) {
    snprintf(buf, sizeof(buf), "%llu",
             static_cast<unsigned long long>(row.bytes_written_logical));
    line += buf;
  }
  line += ',';
  // Column 24: input_count
  int_csv(row.input_count);
  // Column 25: output_count
  int_csv(row.output_count);
  // Column 26: notes (last field; newline instead of comma)
  if (!row.notes.empty()) {
    line += CsvEscape(row.notes);
  }
  line += '\n';

  file_->Append(line);
  file_->Flush();
}

}  // namespace leveldb
