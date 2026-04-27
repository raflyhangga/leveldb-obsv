// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/builder.h"

#include "db/compaction_trace_writer.h"
#include "db/dbformat.h"
#include "db/filename.h"
#include "db/table_cache.h"
#include "db/version_edit.h"
#include <cstdio>

#include "leveldb/db.h"
#include "leveldb/env.h"
#include "leveldb/iterator.h"

namespace leveldb {

Status BuildTable(const std::string& dbname, Env* env, const Options& options,
                  TableCache* table_cache, Iterator* iter, FileMetaData* meta) {
  Status s;
  meta->file_size = 0;
  iter->SeekToFirst();

  std::string fname = TableFileName(dbname, meta->number);
  if (iter->Valid()) {
    WritableFile* file;
    fprintf(stderr, "[BuildTable][tid=%llu] Creating output file %s\n",
            (unsigned long long)CurrentOsThreadId(), fname.c_str());
    s = env->NewWritableFile(fname, &file);
    if (!s.ok()) {
      return s;
    }
    if (options.compaction_trace_path != nullptr) {
      CompactionTraceWriter* trace_writer =
          CompactionTraceWriter::Open(env, options.compaction_trace_path);
      if (trace_writer != nullptr) {
        CompactionTraceWriter::Row r;
        r.trace_ts_us = env->NowMicros();
        r.event = "build_table_output_file_create";
        r.db_name = dbname;
        r.thread_id = CurrentOsThreadId();
        r.file_number = meta->number;
        r.file_name = fname.substr(
            fname.rfind('/') == std::string::npos ? 0 : fname.rfind('/') + 1);
        trace_writer->Write(r);
        delete trace_writer;
      }
    }

    TableBuilder* builder = new TableBuilder(options, file);
    meta->smallest.DecodeFrom(iter->key());
    Slice key;
    for (; iter->Valid(); iter->Next()) {
      key = iter->key();
      builder->Add(key, iter->value());
    }
    if (!key.empty()) {
      meta->largest.DecodeFrom(key);
    }

    // Finish and check for builder errors
    s = builder->Finish();
    if (s.ok()) {
      meta->file_size = builder->FileSize();
      assert(meta->file_size > 0);
    }
    delete builder;

    // Finish and check for file errors
    if (s.ok()) {
      s = file->Sync();
    }
    if (s.ok()) {
      s = file->Close();
    }
    delete file;
    file = nullptr;

    if (s.ok()) {
      // Verify that the table is usable
      Iterator* it = table_cache->NewIterator(ReadOptions(), meta->number,
                                              meta->file_size);
      s = it->status();
      delete it;
    }
  }

  // Check for input iterator errors
  if (!iter->status().ok()) {
    s = iter->status();
  }

  if (s.ok() && meta->file_size > 0) {
    // Keep it
  } else {
    env->RemoveFile(fname);
  }
  return s;
}

}  // namespace leveldb
