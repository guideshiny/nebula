/* Copyright (c) 2018 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License,
 * attached with Common Clause Condition 1.0, found in the LICENSES directory.
 */

#include "base/Base.h"
#include "kvstore/RocksEngine.h"
#include <folly/String.h>
#include "fs/FileUtils.h"
#include "kvstore/KVStore.h"
#include "kvstore/RocksEngineConfig.h"

namespace nebula {
namespace kvstore {

using fs::FileUtils;
using fs::FileType;

const char* kSystemParts = "__system__parts__";

RocksEngine::RocksEngine(GraphSpaceID spaceId,
                         const std::string& dataPath,
                         std::shared_ptr<rocksdb::MergeOperator> mergeOp,
                         std::shared_ptr<rocksdb::CompactionFilterFactory> cfFactory)
        : KVEngine(spaceId)
        , dataPath_(dataPath) {
    LOG(INFO) << "open rocksdb on " << dataPath;
    if (FileUtils::fileType(dataPath.c_str()) == FileType::NOTEXIST) {
        FileUtils::makeDir(dataPath);
    }

    rocksdb::Options options;
    rocksdb::DB* db = nullptr;
    rocksdb::Status status = initRocksdbOptions(options);
    CHECK(status.ok());
    if (mergeOp != nullptr) {
        options.merge_operator = mergeOp;
    }
    if (cfFactory != nullptr) {
        options.compaction_filter_factory = cfFactory;
    }
    status = rocksdb::DB::Open(options, dataPath_, &db);
    CHECK(status.ok());
    db_.reset(db);
    partsNum_ = allParts().size();
}


RocksEngine::~RocksEngine() {
}

ResultCode RocksEngine::get(const std::string& key, std::string* value) {
    rocksdb::ReadOptions options;
    rocksdb::Status status = db_->Get(options, rocksdb::Slice(key), value);
    if (status.ok()) {
        return ResultCode::SUCCEEDED;
    } else if (status.IsNotFound()) {
        VLOG(3) << "Get: " << key << " Not Found";
        return ResultCode::ERR_KEY_NOT_FOUND;
    } else {
        VLOG(3) << "Get Failed: " << key << " " << status.ToString();
        return ResultCode::ERR_UNKNOWN;
    }
}

ResultCode RocksEngine::multiGet(const std::vector<std::string>& keys,
                                   std::vector<std::string>* values) {
    rocksdb::ReadOptions options;
    std::vector<rocksdb::Slice> slices;
    for (unsigned int index = 0 ; index < keys.size() ; index++) {
        slices.emplace_back(keys[index]);
    }

    std::vector<rocksdb::Status> status = db_->MultiGet(options, slices, values);
    auto code = std::all_of(status.begin(), status.end(),
                            [](rocksdb::Status s) {
                                return s.ok();
                            });
    if (code) {
        return ResultCode::SUCCEEDED;
    } else {
        return ResultCode::ERR_UNKNOWN;
    }
}

ResultCode RocksEngine::put(std::string key, std::string value) {
    rocksdb::WriteOptions options;
    options.disableWAL = FLAGS_rocksdb_disable_wal;
    rocksdb::Status status = db_->Put(options, key, value);
    if (status.ok()) {
        return ResultCode::SUCCEEDED;
    } else {
        VLOG(3) << "Put Failed: " << key << status.ToString();
        return ResultCode::ERR_UNKNOWN;
    }
}


ResultCode RocksEngine::multiPut(std::vector<KV> keyValues) {
    rocksdb::WriteBatch updates(FLAGS_batch_reserved_bytes);
    for (size_t i = 0; i < keyValues.size(); i++) {
        updates.Put(keyValues[i].first, keyValues[i].second);
    }
    rocksdb::WriteOptions options;
    options.disableWAL = FLAGS_rocksdb_disable_wal;
    rocksdb::Status status = db_->Write(options, &updates);
    if (status.ok()) {
        return ResultCode::SUCCEEDED;
    } else {
        VLOG(3) << "MultiPut Failed: " << status.ToString();
        return ResultCode::ERR_UNKNOWN;
    }
}


ResultCode RocksEngine::range(const std::string& start,
                              const std::string& end,
                              std::unique_ptr<KVIterator>* storageIter) {
    rocksdb::ReadOptions options;
    rocksdb::Iterator* iter = db_->NewIterator(options);
    if (iter) {
        iter->Seek(rocksdb::Slice(start));
    }
    storageIter->reset(new RocksRangeIter(iter, start, end));
    return ResultCode::SUCCEEDED;
}


ResultCode RocksEngine::prefix(const std::string& prefix,
                               std::unique_ptr<KVIterator>* storageIter) {
    rocksdb::ReadOptions options;
    rocksdb::Iterator* iter = db_->NewIterator(options);
    if (iter) {
        iter->Seek(rocksdb::Slice(prefix));
    }
    storageIter->reset(new RocksPrefixIter(iter, prefix));
    return ResultCode::SUCCEEDED;
}


ResultCode RocksEngine::remove(const std::string& key) {
    rocksdb::WriteOptions options;
    options.disableWAL = FLAGS_rocksdb_disable_wal;
    auto status = db_->Delete(options, key);
    if (status.ok()) {
        return ResultCode::SUCCEEDED;
    } else {
        VLOG(3) << "Remove Failed: " << key << status.ToString();
        return ResultCode::ERR_UNKNOWN;
    }
}

ResultCode RocksEngine::multiRemove(std::vector<std::string> keys) {
    rocksdb::WriteBatch deletes(FLAGS_batch_reserved_bytes);
    for (size_t i = 0; i < keys.size(); i++) {
        deletes.Delete(keys[i]);
    }
    rocksdb::WriteOptions options;
    options.disableWAL = FLAGS_rocksdb_disable_wal;
    rocksdb::Status status = db_->Write(options, &deletes);
    if (status.ok()) {
        return ResultCode::SUCCEEDED;
    } else {
        VLOG(3) << "MultiRemove Failed: " << status.ToString();
        return ResultCode::ERR_UNKNOWN;
    }
}


ResultCode RocksEngine::removeRange(const std::string& start,
                                    const std::string& end) {
    rocksdb::WriteOptions options;
    options.disableWAL = FLAGS_rocksdb_disable_wal;
    auto status = db_->DeleteRange(options, db_->DefaultColumnFamily(), start, end);
    if (status.ok()) {
        return ResultCode::SUCCEEDED;
    } else {
        VLOG(3) << "RemoveRange Failed: " << status.ToString();
        return ResultCode::ERR_UNKNOWN;
    }
}


std::string RocksEngine::partKey(PartitionID partId) {
    std::string key;
    static const size_t prefixLen = ::strlen(kSystemParts);
    key.reserve(prefixLen + sizeof(PartitionID));
    key.append(kSystemParts, prefixLen);
    key.append(reinterpret_cast<const char*>(&partId), sizeof(PartitionID));
    return key;
}


void RocksEngine::addPart(PartitionID partId) {
    auto ret = put(partKey(partId), "");
    if (ret == ResultCode::SUCCEEDED) {
        partsNum_++;
        CHECK_GE(partsNum_, 0);
    }
}


void RocksEngine::removePart(PartitionID partId) {
     rocksdb::WriteOptions options;
     options.disableWAL = FLAGS_rocksdb_disable_wal;
     auto status = db_->Delete(options, partKey(partId));
     if (status.ok()) {
         partsNum_--;
         CHECK_GE(partsNum_, 0);
     }
}


std::vector<PartitionID> RocksEngine::allParts() {
    std::unique_ptr<KVIterator> iter;
    static const size_t prefixLen = ::strlen(kSystemParts);
    static const std::string prefixStr(kSystemParts, prefixLen);
    CHECK_EQ(ResultCode::SUCCEEDED, this->prefix(prefixStr, &iter));
    std::vector<PartitionID> parts;
    while (iter->valid()) {
        auto key = iter->key();
        CHECK_EQ(key.size(), prefixLen + sizeof(PartitionID));
        parts.emplace_back(
           *reinterpret_cast<const PartitionID*>(key.data() + key.size() - sizeof(PartitionID)));
        iter->next();
    }
    return parts;
}


int32_t RocksEngine::totalPartsNum() {
    return partsNum_;
}


ResultCode RocksEngine::ingest(const std::vector<std::string>& files) {
    rocksdb::IngestExternalFileOptions options;
    rocksdb::Status status = db_->IngestExternalFile(files, options);
    if (status.ok()) {
        return ResultCode::SUCCEEDED;
    } else {
        LOG(ERROR) << "Ingest Failed: " << status.ToString();
        return ResultCode::ERR_UNKNOWN;
    }
}


ResultCode RocksEngine::setOption(const std::string& configKey,
                                  const std::string& configValue) {
    std::unordered_map<std::string, std::string> configOptions = {
        {configKey, configValue}
    };

    rocksdb::Status status = db_->SetOptions(configOptions);
    if (status.ok()) {
        return ResultCode::SUCCEEDED;
    } else {
        LOG(ERROR) << "SetOption Failed: " << configKey << ":" << configValue;
        return ResultCode::ERR_INVALID_ARGUMENT;
    }
}


ResultCode RocksEngine::setDBOption(const std::string& configKey,
                                    const std::string& configValue) {
    std::unordered_map<std::string, std::string> configOptions = {
        {configKey, configValue}
    };

    rocksdb::Status status = db_->SetDBOptions(configOptions);
    if (status.ok()) {
        return ResultCode::SUCCEEDED;
    } else {
        LOG(ERROR) << "SetDBOption Failed: " << configKey << ":" << configValue;
        return ResultCode::ERR_INVALID_ARGUMENT;
    }
}

ResultCode RocksEngine::compactAll() {
    rocksdb::CompactRangeOptions options;
    rocksdb::Status status = db_->CompactRange(options, nullptr, nullptr);
    if (status.ok()) {
        return ResultCode::SUCCEEDED;
    } else {
        LOG(ERROR) << "CompactAll Failed: " << status.ToString();
        return ResultCode::ERR_UNKNOWN;
    }
}

}  // namespace kvstore
}  // namespace nebula

