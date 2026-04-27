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

#include <atomic>
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

std::atomic<uint64_t> g_next_event_index(0);

static const char kCsvHeader[] =
    "trace_ts_us,event_index,event,db_name,thread_id,file_number,file_name,"
    "manifest_file_number,status,notes\n";

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
  WritableFile* file = nullptr;
  const bool file_exists = env->FileExists(path);
  uint64_t file_size = 0;
  if (file_exists) {
    env->GetFileSize(path, &file_size);
  }
  Status s = env->NewAppendableFile(path, &file);
  if (!s.ok() && s.IsNotSupportedError()) {
    if (file_exists) {
      return nullptr;
    }
    s = env->NewWritableFile(path, &file);
  }
  if (!s.ok()) return nullptr;
  auto* writer = new CompactionTraceWriter(file);
  if (!file_exists || file_size == 0) {
    s = file->Append(kCsvHeader);
    if (s.ok()) s = file->Flush();
  }
  if (!s.ok()) {
    delete writer;
    return nullptr;
  }
  return writer;
}

CompactionTraceWriter::CompactionTraceWriter(WritableFile* file)
    : file_(file) {}

CompactionTraceWriter::~CompactionTraceWriter() {
  file_->Flush();
  file_->Close();
  delete file_;
}

void CompactionTraceWriter::Write(const Row& row) {
  MutexLock lock(&mu_);
  const uint64_t idx = g_next_event_index.fetch_add(1);

  char buf[64];
  std::string line;
  line.reserve(256);

  // Appends a uint64 value followed by a comma separator.
  auto uint_csv = [&](uint64_t v) {
    snprintf(buf, sizeof(buf), "%llu,", static_cast<unsigned long long>(v));
    line += buf;
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
  // Column 3: event
  str_csv(row.event);
  // Column 4: db_name (escaped; may be a filesystem path)
  str_csv(row.db_name.empty() ? "" : CsvEscape(row.db_name));
  // Column 5: thread_id (0 = omit)
  if (row.thread_id != 0) {
    snprintf(buf, sizeof(buf), "%llu",
             static_cast<unsigned long long>(row.thread_id));
    line += buf;
  }
  line += ',';
  // Column 6: file_number (0 = omit)
  if (row.file_number != 0) {
    snprintf(buf, sizeof(buf), "%llu",
             static_cast<unsigned long long>(row.file_number));
    line += buf;
  }
  line += ',';
  // Column 7: file_name (escaped)
  str_csv(row.file_name.empty() ? "" : CsvEscape(row.file_name));
  // Column 8: manifest_file_number (0 = omit)
  if (row.manifest_file_number != 0) {
    snprintf(buf, sizeof(buf), "%llu",
             static_cast<unsigned long long>(row.manifest_file_number));
    line += buf;
  }
  line += ',';
  // Column 9: status (escaped; may be a LevelDB error string)
  str_csv(row.status.empty() ? "" : CsvEscape(row.status));
  // Column 10: notes (last field; newline instead of comma)
  if (!row.notes.empty()) {
    line += CsvEscape(row.notes);
  }
  line += '\n';

  file_->Append(line);
  file_->Flush();
}

}  // namespace leveldb
