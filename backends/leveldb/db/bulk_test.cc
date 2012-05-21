// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "leveldb/db.h"
#include "leveldb/filter_policy.h"
#include "db/db_impl.h"
#include "db/filename.h"
#include "db/version_set.h"
#include "db/write_batch_internal.h"
#include "leveldb/cache.h"
#include "leveldb/env.h"
#include "leveldb/table.h"
#include "util/random.h"
#include "util/hash.h"
#include "util/logging.h"
#include "util/mutexlock.h"
#include "util/testharness.h"
#include "util/testutil.h"

namespace leveldb {

static std::string RandomString(Random* rnd, int len) {
  std::string r;
  test::RandomString(rnd, len, &r);
  return r;
}

namespace {
class AtomicCounter {
 private:
  port::Mutex mu_;
  int count_;
 public:
  AtomicCounter() : count_(0) { }
  void Increment() {
    MutexLock l(&mu_);
    count_++;
  }
  int Read() {
    MutexLock l(&mu_);
    return count_;
  }
  void Reset() {
    MutexLock l(&mu_);
    count_ = 0;
  }
};
}

// Special Env used to delay background operations
class SpecialEnv : public EnvWrapper {
 public:
  // sstable Sync() calls are blocked while this pointer is non-NULL.
  port::AtomicPointer delay_sstable_sync_;

  // Simulate no-space errors while this pointer is non-NULL.
  port::AtomicPointer no_space_;

  bool count_random_reads_;
  AtomicCounter random_read_counter_;

  explicit SpecialEnv(Env* base) : EnvWrapper(base) {
    delay_sstable_sync_.Release_Store(NULL);
    no_space_.Release_Store(NULL);
    count_random_reads_ = false;
  }

  Status NewWritableFile(const std::string& f, WritableFile** r) {
    class SSTableFile : public WritableFile {
     private:
      SpecialEnv* env_;
      WritableFile* base_;

     public:
      SSTableFile(SpecialEnv* env, WritableFile* base)
          : env_(env),
            base_(base) {
      }
      ~SSTableFile() { delete base_; }
      Status Append(const Slice& data) {
        if (env_->no_space_.Acquire_Load() != NULL) {
          // Drop writes on the floor
          return Status::OK();
        } else {
          return base_->Append(data);
        }
      }
      Status Close() { return base_->Close(); }
      Status Flush() { return base_->Flush(); }
      Status Sync() {
        while (env_->delay_sstable_sync_.Acquire_Load() != NULL) {
          env_->SleepForMicroseconds(100000);
        }
        return base_->Sync();
      }
    };

    Status s = target()->NewWritableFile(f, r);
    if (s.ok()) {
      if (strstr(f.c_str(), ".sst") != NULL) {
        *r = new SSTableFile(this, *r);
      }
    }
    return s;
  }

  Status NewRandomAccessFile(const std::string& f, RandomAccessFile** r) {
    class CountingFile : public RandomAccessFile {
     private:
      RandomAccessFile* target_;
      AtomicCounter* counter_;
     public:
      CountingFile(RandomAccessFile* target, AtomicCounter* counter)
          : target_(target), counter_(counter) {
      }
      virtual ~CountingFile() { delete target_; }
      virtual Status Read(uint64_t offset, size_t n, Slice* result,
                          char* scratch) const {
        counter_->Increment();
        return target_->Read(offset, n, result, scratch);
      }
    };

    Status s = target()->NewRandomAccessFile(f, r);
    if (s.ok() && count_random_reads_) {
      *r = new CountingFile(*r, &random_read_counter_);
    }
    return s;
  }
};

class DBTest {
 private:
  const FilterPolicy* filter_policy_;

  // Sequence of option configurations to try
  enum OptionConfig {
    kDefault,
    kFilter,
    kEnd
  };
  int option_config_;

 public:
  std::string dbname_;
  SpecialEnv* env_;
  DB* db_;

  Options last_options_;

  DBTest() : option_config_(kDefault),
             env_(new SpecialEnv(Env::Default())) {
    filter_policy_ = NewBloomFilterPolicy(10);
    dbname_ = test::TmpDir() + "/db_test";
    DestroyDB(dbname_, Options());
    db_ = NULL;
    Reopen();
  }

  ~DBTest() {
    delete db_;
    DestroyDB(dbname_, Options());
    delete env_;
    delete filter_policy_;
  }

  // Switch to a fresh database with the next option configuration to
  // test.  Return false if there are no more configurations to test.
  bool ChangeOptions() {
    if (option_config_ == kEnd) {
      return false;
    } else {
      option_config_++;
      DestroyAndReopen();
      return true;
    }
  }

  // Return the current option configuration.
  Options CurrentOptions() {
    Options options;
    switch (option_config_) {
      case kFilter:
        options.filter_policy = filter_policy_;
        break;
      default:
        break;
    }
    return options;
  }

  DBImpl* dbfull() {
    return reinterpret_cast<DBImpl*>(db_);
  }

  void Reopen(Options* options = NULL) {
    ASSERT_OK(TryReopen(options));
  }

  void Close() {
    delete db_;
    db_ = NULL;
  }

  void DestroyAndReopen(Options* options = NULL) {
    delete db_;
    db_ = NULL;
    DestroyDB(dbname_, Options());
    ASSERT_OK(TryReopen(options));
  }

  Status TryReopen(Options* options) {
    delete db_;
    db_ = NULL;
    Options opts;
    if (options != NULL) {
      opts = *options;
    } else {
      opts = CurrentOptions();
      opts.create_if_missing = true;
    }
    last_options_ = opts;

    return DB::Open(opts, dbname_, &db_);
  }

  Status Put(const std::string& k, const std::string& v) {
    return db_->Put(WriteOptions(), k, v);
  }

  Status Delete(const std::string& k) {
    return db_->Delete(WriteOptions(), k);
  }

  std::string Get(const std::string& k, const Snapshot* snapshot = NULL) {
    ReadOptions options;
    options.snapshot = snapshot;
    std::string result;
    Status s = db_->Get(options, k, &result);
    if (s.IsNotFound()) {
      result = "NOT_FOUND";
    } else if (!s.ok()) {
      result = s.ToString();
    }
    return result;
  }

  // Return a string that contains all key,value pairs in order,
  // formatted like "(k1->v1)(k2->v2)".
  std::string Contents() {
    std::vector<std::string> forward;
    std::string result;
    Iterator* iter = db_->NewIterator(ReadOptions());
    for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
      std::string s = IterStatus(iter);
      result.push_back('(');
      result.append(s);
      result.push_back(')');
      forward.push_back(s);
    }

    // Check reverse iteration results are the reverse of forward results
    int matched = 0;
    for (iter->SeekToLast(); iter->Valid(); iter->Prev()) {
      ASSERT_LT(matched, forward.size());
      ASSERT_EQ(IterStatus(iter), forward[forward.size() - matched - 1]);
      matched++;
    }
    ASSERT_EQ(matched, forward.size());

    delete iter;
    return result;
  }

  std::string AllEntriesFor(const Slice& user_key) {
    Iterator* iter = dbfull()->TEST_NewInternalIterator();
    InternalKey target(user_key, kMaxSequenceNumber, kTypeValue);
    iter->Seek(target.Encode());
    std::string result;
    if (!iter->status().ok()) {
      result = iter->status().ToString();
    } else {
      result = "[ ";
      bool first = true;
      while (iter->Valid()) {
        ParsedInternalKey ikey;
        if (!ParseInternalKey(iter->key(), &ikey)) {
          result += "CORRUPTED";
        } else {
          if (last_options_.comparator->Compare(ikey.user_key, user_key) != 0) {
            break;
          }
          if (!first) {
            result += ", ";
          }
          first = false;
          switch (ikey.type) {
            case kTypeValue:
              result += iter->value().ToString();
              break;
            case kTypeDeletion:
              result += "DEL";
              break;
          }
        }
        iter->Next();
      }
      if (!first) {
        result += " ";
      }
      result += "]";
    }
    delete iter;
    return result;
  }

  int NumTableFilesAtLevel(int level) {
    std::string property;
    ASSERT_TRUE(
        db_->GetProperty("leveldb.num-files-at-level" + NumberToString(level),
                         &property));
    return atoi(property.c_str());
  }

  int TotalTableFiles() {
    int result = 0;
    for (int level = 0; level < config::kNumLevels; level++) {
      result += NumTableFilesAtLevel(level);
    }
    return result;
  }

  // Return spread of files per level
  std::string FilesPerLevel() {
    std::string result;
    int last_non_zero_offset = 0;
    for (int level = 0; level < config::kNumLevels; level++) {
      int f = NumTableFilesAtLevel(level);
      char buf[100];
      snprintf(buf, sizeof(buf), "%s%d", (level ? "," : ""), f);
      result += buf;
      if (f > 0) {
        last_non_zero_offset = result.size();
      }
    }
    result.resize(last_non_zero_offset);
    return result;
  }

  int CountFiles() {
    std::vector<std::string> files;
    env_->GetChildren(dbname_, &files);
    return static_cast<int>(files.size());
  }

  int CountDirFiles(const std::string &dirname) {
    std::vector<std::string> files;
    env_->GetChildren(dirname, &files);
    return static_cast<int>(files.size());
  }

  uint64_t Size(const Slice& start, const Slice& limit) {
    Range r(start, limit);
    uint64_t size;
    db_->GetApproximateSizes(&r, 1, &size);
    return size;
  }

  void Compact(const Slice& start, const Slice& limit) {
    db_->CompactRange(&start, &limit);
  }

  // Do n memtable compactions, each of which produces an sstable
  // covering the range [small,large].
  void MakeTables(int n, const std::string& small, const std::string& large) {
    for (int i = 0; i < n; i++) {
      Put(small, "begin");
      Put(large, "end");
      dbfull()->TEST_CompactMemTable();
    }
  }

  // Prevent pushing of new sstables into deeper levels by adding
  // tables that cover a specified range to all levels.
  void FillLevels(const std::string& smallest, const std::string& largest) {
    MakeTables(config::kNumLevels, smallest, largest);
  }

  void DumpFileCounts(const char* label) {
    fprintf(stderr, "---\n%s:\n", label);
    fprintf(stderr, "maxoverlap: %lld\n",
            static_cast<long long>(
                dbfull()->TEST_MaxNextLevelOverlappingBytes()));
    for (int level = 0; level < config::kNumLevels; level++) {
      int num = NumTableFilesAtLevel(level);
      if (num > 0) {
        fprintf(stderr, "  level %3d : %d files\n", level, num);
      }
    }
  }

  std::string DumpSSTableList() {
    std::string property;
    db_->GetProperty("leveldb.sstables", &property);
    return property;
  }

  std::string IterStatus(Iterator* iter) {
    std::string result;
    if (iter->Valid()) {
      result = iter->key().ToString() + "->" + iter->value().ToString();
    } else {
      result = "(invalid)";
    }
    return result;
  }

  void BulkSplit(const Slice& start, const Slice& limit, uint64_t seqno=0) {
    WriteOptions options;
    options.sync = true;
    std::string dirname = test::TmpDir() + "/db_bulk";
    db_->BulkSplit(options, seqno, &start, &limit, dirname);
  }

  void BulkInsert(uint64_t max_sequence_number) {
    WriteOptions options;
    options.sync = true;
    std::string dirname = test::TmpDir() + "/db_bulk";
    std::vector<std::string> files;
    env_->GetChildren(dirname, &files);
    for (int i = 0; i < files.size(); ++i)
      if (files[i].compare(".") != 0 && files[i].compare("..") != 0)
        db_->BulkInsert(options, dirname+"/"+files[i], 0, max_sequence_number);
  }

  void BulkInsert(const std::string &dirname, uint64_t max_sequence_number) {
    WriteOptions options;
    options.sync = true;
    std::vector<std::string> files;
    env_->GetChildren(dirname, &files);
    for (int i = 0; i < files.size(); ++i)
      if (files[i].compare(".") != 0 && files[i].compare("..") != 0)
        db_->BulkInsert(options, dirname+"/"+files[i], 0, max_sequence_number);
  }
};

/*
TEST(DBTest, BulkDeletion) {
  ASSERT_EQ(config::kMaxMemCompactLevel, 2)
      << "Need to update this test to match kMaxMemCompactLevel";
  std::string dirname = test::TmpDir() + "/db_bulk";

  MakeTables(3, "p", "q");
  ASSERT_EQ("1,1,1", FilesPerLevel());

  // BulkSplit range falls before files
  BulkSplit("", "c");
  ASSERT_EQ("1,1,1", FilesPerLevel());

  // BulkSplit range falls after files
  BulkSplit("r", "z");
  ASSERT_EQ("1,1,1", FilesPerLevel());

  // BulkSplit range overlaps files
  BulkSplit("p", "p10");
  ASSERT_EQ(CountDirFiles(dirname), 3);
  Compact("p", "q");
  ASSERT_EQ("0,0,1",FilesPerLevel());
}

TEST(DBTest, BulkInsertion) {
  ASSERT_EQ(config::kMaxMemCompactLevel, 2)
      << "Need to update this test to match kMaxMemCompactLevel";
  std::string dirname = test::TmpDir() + "/db_bulk";

  MakeTables(3, "p", "q");
  ASSERT_EQ("1,1,1", FilesPerLevel());

  // BulkSplit range falls before files
  BulkSplit("", "c");
  ASSERT_EQ("1,1,1", FilesPerLevel());

  // BulkSplit range falls after files
  BulkSplit("r", "z");
  ASSERT_EQ("1,1,1", FilesPerLevel());

  // BulkSplit range overlaps files
  BulkSplit("p", "p10");
  ASSERT_EQ(CountDirFiles(dirname), 3);
  Compact("p", "q");
  BulkInsert();
  ASSERT_EQ("0,0,2",FilesPerLevel());

  int count = 0;
  Iterator* iter = db_->NewIterator(ReadOptions());
  iter->Seek("p");
  ASSERT_EQ(iter->key().ToString(), "p");
  iter->Next();
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "q");
  delete iter;
}

TEST(DBTest, BulkInsertion2) {
  std::string dirname = test::TmpDir() + "/db_bulk";

  int num_entries = 100000;

  MakeTables(3, "p", "q");
  ASSERT_EQ("1,1,1", FilesPerLevel());

  Random randgen(100);
  WriteOptions options;
  for (int i = 0; i < num_entries; ++i) {
    char key[20];
    sprintf(key, "%08x", randgen.Uniform(num_entries));
    db_->Put(options, key, "abcdefghijklmnopqrstuvwxy");
  }
  db_->CompactRange(NULL, NULL);
  int count = 0;
  Iterator* iter = db_->NewIterator(ReadOptions());
  for (iter->SeekToFirst(); iter->Valid(); iter->Next())
    count++;
  delete iter;

  // BulkSplit range falls before files
  BulkSplit("00000000", "00001234");

  ASSERT_EQ(CountDirFiles(dirname), 3);
  db_->CompactRange(NULL, NULL);
  BulkInsert();

  int new_count = 0;
  iter = db_->NewIterator(ReadOptions());
  for (iter->SeekToFirst(); iter->Valid(); iter->Next())
    new_count++;
  delete iter;
  ASSERT_EQ(count, new_count);
}
*/

TEST(DBTest, BulkInsertion3) {
  std::string dirname = "/tmp/extract";
  BulkInsert(dirname, 500);
  int new_count = 0;
  Iterator* iter = db_->NewIterator(ReadOptions());
  for (iter->SeekToFirst(); iter->Valid(); iter->Next())
    new_count++;
  delete iter;
  printf("new_count: %d\n", new_count);
  ASSERT_TRUE(new_count > 0);
}

}  // namespace leveldb

int main(int argc, char** argv) {
  return leveldb::test::RunAllTests();
}
