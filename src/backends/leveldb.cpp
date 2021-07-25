/*
 * (C) 2021 The University of Chicago
 *
 * See COPYRIGHT in top-level directory.
 */
#include "rkv/rkv-backend.hpp"
#include "../common/modes.hpp"
#include <nlohmann/json.hpp>
#include <abt.h>
#include <leveldb/db.h>
#include <leveldb/comparator.h>
#include <leveldb/env.h>
#include <leveldb/write_batch.h>
#include <string>
#include <cstring>
#include <iostream>
#if __cplusplus >= 201703L
#include <filesystem>
#else
#include <experimental/filesystem>
#endif

namespace rkv {

#if __cplusplus >= 201703L
namespace fs = std::filesystem;
#else
namespace fs = std::experimental::filesystem;
#endif

using json = nlohmann::json;

class LevelDBKeyValueStore : public KeyValueStoreInterface {

    public:

    static inline Status convertStatus(const leveldb::Status& s) {
        if(s.ok()) return Status::OK;
        if(s.IsNotFound()) return Status::NotFound;
        if(s.IsCorruption()) return Status::Corruption;
        if(s.IsIOError()) return Status::IOError;
        if(s.IsNotSupportedError()) return Status::NotSupported;
        if(s.IsInvalidArgument()) return Status::InvalidArg;
        return Status::Other;
    }

    static Status create(const std::string& config, KeyValueStoreInterface** kvs) {
        json cfg;
        try {
            cfg = json::parse(config);
        } catch(...) {
            return Status::InvalidConf;
        }
        // fill options and complete configuration
        leveldb::Options options;

#define SET_AND_COMPLETE(__json__, __field__, __value__)               \
        do { try {                                                     \
            options.__field__ = __json__.value(#__field__, __value__); \
            __json__[#__field__] = options.__field__;                  \
        } catch(...) {                                                 \
            return Status::InvalidConf;                                \
        } } while(0)

#define CHECK_AND_ADD_MISSING(__json__, __field__, __type__, __default__) \
        do { if(!__json__.contains(__field__)) {                          \
            __json__[__field__] = __default__;                            \
        } else if(!__json__[__field__].is_##__type__()) {                 \
            return Status::InvalidConf;                                   \
        } } while(0)

        SET_AND_COMPLETE(cfg, create_if_missing, false);
        SET_AND_COMPLETE(cfg, error_if_exists, false);
        SET_AND_COMPLETE(cfg, paranoid_checks, false);
        SET_AND_COMPLETE(cfg, write_buffer_size, (size_t)(4*1024*1024));
        SET_AND_COMPLETE(cfg, max_open_files, 1000);
        SET_AND_COMPLETE(cfg, block_size, (size_t)(4*1024));
        SET_AND_COMPLETE(cfg, block_restart_interval, 16);
        SET_AND_COMPLETE(cfg, max_file_size, (size_t)(2*1024*1024));
        SET_AND_COMPLETE(cfg, reuse_logs, false);
        try {
            options.compression = cfg.value("compression", true) ?
                leveldb::kSnappyCompression : leveldb::kNoCompression;
            cfg["compression"] = options.compression == leveldb::kSnappyCompression;
        } catch(...) {
            return Status::InvalidConf;
        }
        CHECK_AND_ADD_MISSING(cfg, "read_options", object, json::object());
        CHECK_AND_ADD_MISSING(cfg["read_options"], "verify_checksums", boolean, false);
        CHECK_AND_ADD_MISSING(cfg["read_options"], "fill_cache", boolean, true);
        CHECK_AND_ADD_MISSING(cfg, "write_options", object, json::object());
        CHECK_AND_ADD_MISSING(cfg["write_options"], "sync", boolean, false);
        CHECK_AND_ADD_MISSING(cfg["write_options"], "use_write_batch", boolean, false);
        // TODO set logger, env, block_cache, and filter_policy
        std::string path = cfg.value("path", "");
        if(path.empty()) {
            return Status::InvalidConf;
        }
        leveldb::Status status;
        leveldb::DB* db = nullptr;
        status = leveldb::DB::Open(options, path, &db);
        if(!status.ok())
            return convertStatus(status);

        *kvs = new LevelDBKeyValueStore(db, std::move(cfg));

        return Status::OK;
    }

    // LCOV_EXCL_START
    virtual std::string name() const override {
        return "leveldb";
    }
    // LCOV_EXCL_STOP

    // LCOV_EXCL_START
    virtual std::string config() const override {
        return m_config.dump();
    }
    // LCOV_EXCL_STOP

    virtual bool supportsMode(int32_t mode) const override {
        return mode ==
            (mode & (
                     RKV_MODE_INCLUSIVE
        //            |RKV_MODE_APPEND
                    |RKV_MODE_CONSUME
        //            |RKV_MODE_WAIT
        //            |RKV_MODE_NOTIFY
        //            |RKV_MODE_NEW_ONLY
        //            |RKV_MODE_EXIST_ONLY
        //            |RKV_MODE_NO_PREFIX
        //            |RKV_MODE_IGNORE_KEYS
        //            |RKV_MODE_KEEP_LAST
                    |RKV_MODE_SUFFIX
                    )
            );
    }

    virtual void destroy() override {
        auto path = m_config["path"].get<std::string>();
        fs::remove_all(path);
    }

    virtual Status exists(int32_t mode, const UserMem& keys,
                          const BasicUserMem<size_t>& ksizes,
                          BitField& flags) const override {
        (void)mode;
        if(ksizes.size > flags.size) return Status::InvalidArg;
        size_t offset = 0;
        std::string value;
        for(size_t i = 0; i < ksizes.size; i++) {
            if(offset + ksizes[i] > keys.size) return Status::InvalidArg;
            const leveldb::Slice key{ keys.data + offset, ksizes[i] };
            flags[i] = m_db->Get(m_read_options, key, &value).ok();
            offset += ksizes[i];
        }
        return Status::OK;
    }

    virtual Status length(int32_t mode, const UserMem& keys,
                          const BasicUserMem<size_t>& ksizes,
                          BasicUserMem<size_t>& vsizes) const override {
        (void)mode;
        if(ksizes.size > vsizes.size) return Status::InvalidArg;
        size_t offset = 0;
        std::string value;
        for(size_t i = 0; i < ksizes.size; i++) {
            if(offset + ksizes[i] > keys.size) return Status::InvalidArg;
            const leveldb::Slice key{ keys.data + offset, ksizes[i] };
            auto status = m_db->Get(m_read_options, key, &value);
            if(status.ok()) {
                vsizes[i] = value.size();
            } else if(status.IsNotFound()) {
                vsizes[i] = KeyNotFound;
            } else {
                return convertStatus(status);
            }
            offset += ksizes[i];
        }
        return Status::OK;
    }

    virtual Status put(int32_t mode,
                       const UserMem& keys,
                       const BasicUserMem<size_t>& ksizes,
                       const UserMem& vals,
                       const BasicUserMem<size_t>& vsizes) override {
        (void)mode;
        if(ksizes.size != vsizes.size) return Status::InvalidArg;

        size_t key_offset = 0;
        size_t val_offset = 0;

        size_t total_ksizes = std::accumulate(ksizes.data,
                                              ksizes.data + ksizes.size,
                                              0);
        if(total_ksizes > keys.size) return Status::InvalidArg;

        size_t total_vsizes = std::accumulate(vsizes.data,
                                              vsizes.data + vsizes.size,
                                              0);
        if(total_vsizes > vals.size) return Status::InvalidArg;

        if(m_use_write_batch) {
            leveldb::WriteBatch wb;

            for(size_t i = 0; i < ksizes.size; i++) {
                wb.Put(leveldb::Slice{ keys.data + key_offset, ksizes[i] },
                       leveldb::Slice{ vals.data + val_offset, vsizes[i] });
                key_offset += ksizes[i];
                val_offset += vsizes[i];
            }
            auto status = m_db->Write(m_write_options, &wb);
            return convertStatus(status);

        } else {
            for(size_t i = 0; i < ksizes.size; i++) {
                auto status = m_db->Put(m_write_options,
                          leveldb::Slice{ keys.data + key_offset, ksizes[i] },
                          leveldb::Slice{ vals.data + val_offset, vsizes[i] });
                key_offset += ksizes[i];
                val_offset += vsizes[i];
                if(!status.ok())
                    return convertStatus(status);
            }
        }
        return Status::OK;;
    }

    virtual Status get(int32_t mode, bool packed, const UserMem& keys,
                       const BasicUserMem<size_t>& ksizes,
                       UserMem& vals,
                       BasicUserMem<size_t>& vsizes) override {
        (void)mode;
        if(ksizes.size != vsizes.size) return Status::InvalidArg;

        size_t key_offset = 0;
        size_t val_offset = 0;

        std::string value;

        if(!packed) {

            for(size_t i = 0; i < ksizes.size; i++) {
                const leveldb::Slice key{ keys.data + key_offset, ksizes[i] };
                auto status = m_db->Get(m_read_options, key, &value);
                const auto original_vsize = vsizes[i];
                if(status.IsNotFound()) {
                    vsizes[i] = KeyNotFound;
                } else if(status.ok()) {
                    if(value.size() > vsizes[i]) {
                        vsizes[i] = BufTooSmall;
                    } else {
                        std::memcpy(vals.data + val_offset, value.data(), value.size());
                        vsizes[i] = value.size();
                    }
                } else {
                    return convertStatus(status);
                }
                key_offset += ksizes[i];
                val_offset += original_vsize;
            }

        } else { // if packed

            size_t val_remaining_size = vals.size;

            for(size_t i = 0; i < ksizes.size; i++) {
                const leveldb::Slice key{ keys.data + key_offset, ksizes[i] };
                auto status = m_db->Get(m_read_options, key, &value);
                if(status.IsNotFound()) {
                    vsizes[i] = KeyNotFound;
                } else if(status.ok()) {
                    if(value.size() > val_remaining_size) {
                        for(; i < ksizes.size; i++) {
                            vsizes[i] = BufTooSmall;
                        }
                    } else {
                        std::memcpy(vals.data + val_offset, value.data(), value.size());
                        vsizes[i] = value.size();
                        val_remaining_size -= vsizes[i];
                        val_offset += vsizes[i];
                    }
                } else {
                    convertStatus(status);
                }
                key_offset += ksizes[i];
            }
            vals.size = vals.size - val_remaining_size;
        }
        if(mode & RKV_MODE_CONSUME) {
            return erase(mode, keys, ksizes);
        }
        return Status::OK;
    }

    virtual Status erase(int32_t mode, const UserMem& keys,
                         const BasicUserMem<size_t>& ksizes) override {
        (void)mode;
        size_t offset = 0;
        leveldb::WriteBatch wb;
        for(size_t i = 0; i < ksizes.size; i++) {
            const auto key = leveldb::Slice{ keys.data + offset, ksizes[i] };
            if(offset + ksizes[i] > keys.size) return Status::InvalidArg;
            wb.Delete(key);
            offset += ksizes[i];
        }
        auto status = m_db->Write(m_write_options, &wb);
        return convertStatus(status);
    }

    virtual Status listKeys(int32_t mode, bool packed, const UserMem& fromKey,
                            const UserMem& prefix,
                            UserMem& keys, BasicUserMem<size_t>& keySizes) const override {

        auto inclusive = mode & RKV_MODE_INCLUSIVE;
        auto fromKeySlice = leveldb::Slice{ fromKey.data, fromKey.size };

        auto iterator = m_db->NewIterator(m_read_options);
        if(fromKey.size == 0) {
            iterator->SeekToFirst();
        } else {
            iterator->Seek(fromKeySlice);
            if(!inclusive) {
                if(iterator->key().compare(fromKeySlice) == 0) {
                    iterator->Next();
                }
            }
        }

        auto max = keySizes.size;
        size_t i = 0;
        size_t offset = 0;
        bool buf_too_small = false;

        while(iterator->Valid() && i < max) {
            auto key = iterator->key();
            if(!checkPrefix(mode, key.data(), key.size(),
                                  prefix.data, prefix.size)) {
                iterator->Next();
                continue;
            }
            size_t usize = keySizes[i];
            auto umem = static_cast<char*>(keys.data) + offset;
            if(packed) {
                if(keys.size - offset < key.size() || buf_too_small) {
                    keySizes[i] = RKV_SIZE_TOO_SMALL;
                    buf_too_small = true;
                } else {
                    std::memcpy(umem, key.data(), key.size());
                    keySizes[i] = key.size();
                    offset += key.size();
                }
            } else {
                if(usize < key.size()) {
                    keySizes[i] = RKV_SIZE_TOO_SMALL;
                    offset += usize;
                } else {
                    std::memcpy(umem, key.data(), key.size());
                    keySizes[i] = key.size();
                    offset += usize;
                }
            }
            i += 1;
            iterator->Next();
        }
        keys.size = offset;
        for(; i < max; i++) {
            keySizes[i] = RKV_NO_MORE_KEYS;
        }
        delete iterator;
        return Status::OK;
    }

    virtual Status listKeyValues(int32_t mode,
                                 bool packed,
                                 const UserMem& fromKey,
                                 const UserMem& prefix,
                                 UserMem& keys,
                                 BasicUserMem<size_t>& keySizes,
                                 UserMem& vals,
                                 BasicUserMem<size_t>& valSizes) const override {

        auto inclusive = mode & RKV_MODE_INCLUSIVE;
        auto fromKeySlice = leveldb::Slice{ fromKey.data, fromKey.size };

        auto iterator = m_db->NewIterator(m_read_options);
        if(fromKey.size == 0) {
            iterator->SeekToFirst();
        } else {
            iterator->Seek(fromKeySlice);
            if(!inclusive) {
                if(iterator->key().compare(fromKeySlice) == 0) {
                    iterator->Next();
                }
            }
        }

        auto max = keySizes.size;
        size_t i = 0;
        size_t key_offset = 0;
        size_t val_offset = 0;
        bool key_buf_too_small = false;
        bool val_buf_too_small = false;

        while(iterator->Valid() && i < max) {
            auto key = iterator->key();
            if(!checkPrefix(mode, key.data(), key.size(),
                                  prefix.data, prefix.size)) {
                iterator->Next();
                continue;
            }
            auto val = iterator->value();
            size_t key_usize = keySizes[i];
            size_t val_usize = valSizes[i];
            auto key_umem = static_cast<char*>(keys.data) + key_offset;
            auto val_umem = static_cast<char*>(vals.data) + val_offset;
            if(packed) {
                if(keys.size - key_offset < key.size() || key_buf_too_small) {
                    keySizes[i] = RKV_SIZE_TOO_SMALL;
                    key_buf_too_small = true;
                } else {
                    std::memcpy(key_umem, key.data(), key.size());
                    keySizes[i] = key.size();
                    key_offset += key.size();
                }
                if(vals.size - val_offset < val.size() || val_buf_too_small) {
                    valSizes[i] = RKV_SIZE_TOO_SMALL;
                    val_buf_too_small = true;
                } else {
                    std::memcpy(val_umem, val.data(), val.size());
                    valSizes[i] = val.size();
                    val_offset += val.size();
                }
            } else {
                if(key_usize < key.size()) {
                    keySizes[i] = RKV_SIZE_TOO_SMALL;
                    key_offset += key_usize;
                } else {
                    std::memcpy(key_umem, key.data(), key.size());
                    keySizes[i] = key.size();
                    key_offset += key_usize;
                }
                if(val_usize < val.size()) {
                    valSizes[i] = RKV_SIZE_TOO_SMALL;
                    val_offset += val_usize;
                } else {
                    std::memcpy(val_umem, val.data(), val.size());
                    valSizes[i] = val.size();
                    val_offset += val_usize;
                }
            }
            i += 1;
            iterator->Next();
        }
        keys.size = key_offset;
        vals.size = val_offset;
        for(; i < max; i++) {
            keySizes[i] = RKV_NO_MORE_KEYS;
            valSizes[i] = RKV_NO_MORE_KEYS;
        }
        delete iterator;
        return Status::OK;
    }

    ~LevelDBKeyValueStore() {
        delete m_db;
    }

    private:

    LevelDBKeyValueStore(leveldb::DB* db, json&& cfg)
    : m_db(db)
    , m_config(std::move(cfg)) {
        m_read_options.verify_checksums = m_config["read_options"]["verify_checksums"].get<bool>();
        m_read_options.fill_cache = m_config["read_options"]["fill_cache"].get<bool>();
        m_write_options.sync = m_config["write_options"]["sync"].get<bool>();
        m_use_write_batch = m_config["write_options"]["use_write_batch"].get<bool>();
    }

    leveldb::DB*          m_db;
    json                  m_config;
    leveldb::ReadOptions  m_read_options;
    leveldb::WriteOptions m_write_options;
    bool                  m_use_write_batch;
};

}

RKV_REGISTER_BACKEND(leveldb, rkv::LevelDBKeyValueStore);
