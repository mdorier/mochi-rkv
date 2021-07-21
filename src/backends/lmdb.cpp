/*
 * (C) 2021 The University of Chicago
 *
 * See COPYRIGHT in top-level directory.
 */
#include "rkv/rkv-backend.hpp"
#include <nlohmann/json.hpp>
#include <abt.h>
#include <lmdb.h>
#include <string>
#include <cstring>
#include <iostream>
#include <experimental/filesystem>

namespace rkv {

using json = nlohmann::json;

class LMDBKeyValueStore : public KeyValueStoreInterface {

    public:

    static inline Status convertStatus(int s) {
        switch(s) {
            case MDB_SUCCESS:
                return Status::OK;
            case MDB_KEYEXIST:
                return Status::KeyExists;
            case MDB_NOTFOUND:
                return Status::NotFound;
            case MDB_CORRUPTED:
                return Status::Corruption;
            case MDB_INVALID:
                return Status::InvalidArg;
            case MDB_PAGE_NOTFOUND:
            case MDB_PANIC:
            case MDB_VERSION_MISMATCH:
            case MDB_MAP_FULL:
            case MDB_DBS_FULL:
            case MDB_READERS_FULL:
            case MDB_TLS_FULL:
            case MDB_TXN_FULL:
            case MDB_CURSOR_FULL:
            case MDB_PAGE_FULL:
            case MDB_MAP_RESIZED:
            case MDB_INCOMPATIBLE:
            case MDB_BAD_RSLOT:
            case MDB_BAD_TXN:
            case MDB_BAD_VALSIZE:
            case MDB_BAD_DBI:
            default:
                return Status::Other;
        };
        return Status::Other;
    }

    static Status create(const std::string& config, KeyValueStoreInterface** kvs) {
        json cfg;
        try {
            cfg = json::parse(config);
        } catch(...) {
            return Status::InvalidConf;
        }

#define CHECK_AND_ADD_MISSING(__json__, __field__, __type__, __default__, __required__) \
        do {                                                                            \
            if(!__json__.contains(__field__)) {                                         \
                if(__required__) {                                                      \
                    return Status::InvalidConf;                                         \
                } else {                                                                \
                    __json__[__field__] = __default__;                                  \
                }                                                                       \
            } else if(!__json__[__field__].is_##__type__()) {                           \
                return Status::InvalidConf;                                             \
            }                                                                           \
        } while(0)

        CHECK_AND_ADD_MISSING(cfg, "path", string, "", true);

        auto path = cfg["path"].get<std::string>();
        std::error_code ec;
        std::experimental::filesystem::create_directories(path, ec);
        int ret;
        MDB_env* env = nullptr;
        MDB_txn *txn = nullptr;
        MDB_dbi db;

        ret = mdb_env_create(&env);
        if(ret != MDB_SUCCESS) return convertStatus(ret);
        // TODO add MDB_NOLOCK flag if requested, and maybe other flags
        ret = mdb_env_open(env, path.c_str(), MDB_WRITEMAP, 0644);
        if(ret != MDB_SUCCESS) {
            mdb_env_close(env);
            return convertStatus(ret);
        }
        ret = mdb_txn_begin(env, nullptr, 0, &txn);
        if(ret != MDB_SUCCESS) {
            mdb_env_close(env);
            return convertStatus(ret);
        }
        ret = mdb_dbi_open(txn, nullptr, MDB_CREATE, &db);
        if(ret != MDB_SUCCESS) {
            mdb_txn_abort(txn);
            mdb_env_close(env);
            return convertStatus(ret);
        }
        ret = mdb_txn_commit(txn);
        if(ret != MDB_SUCCESS) {
            mdb_env_close(env);
            return convertStatus(ret);
        }

        *kvs = new LMDBKeyValueStore(std::move(cfg), env, db);

        return Status::OK;
    }

    // LCOV_EXCL_START
    virtual std::string name() const override {
        return "lmdb";
    }
    // LCOV_EXCL_STOP

    // LCOV_EXCL_START
    virtual std::string config() const override {
        return m_config.dump();
    }
    // LCOV_EXCL_STOP

    virtual void destroy() override {
        if(m_env) {
            mdb_dbi_close(m_env, m_db);
            mdb_env_close(m_env);
        }
        m_env = nullptr;
        auto path = m_config["path"].get<std::string>();
        std::experimental::filesystem::remove_all(path);
    }

    virtual Status exists(const UserMem& keys,
                          const BasicUserMem<size_t>& ksizes,
                          BitField& flags) const override {
        if(ksizes.size > flags.size) return Status::InvalidArg;
        auto count = ksizes.size;
        size_t offset = 0;
        MDB_txn* txn = nullptr;
        int ret = mdb_txn_begin(m_env, nullptr, MDB_RDONLY, &txn);
        if(ret != MDB_SUCCESS) return convertStatus(ret);
        for(size_t i = 0; i < count; i++) {
            if(offset + ksizes[i] > keys.size) return Status::InvalidArg;
            MDB_val key{ ksizes[i], keys.data + offset };
            MDB_val val { 0, nullptr };
            ret = mdb_get(txn, m_db, &key, &val);
            if(ret == MDB_NOTFOUND) flags[i] = false;
            else if(ret == MDB_SUCCESS) flags[i] = true;
            else {
                mdb_txn_abort(txn);
                return convertStatus(ret);
            }
            offset += ksizes[i];
        }
        mdb_txn_abort(txn);
        return Status::OK;
    }

    virtual Status length(const UserMem& keys,
                          const BasicUserMem<size_t>& ksizes,
                          BasicUserMem<size_t>& vsizes) const override {
        if(ksizes.size > vsizes.size) return Status::InvalidArg;
        auto count = ksizes.size;
        size_t offset = 0;
        MDB_txn* txn = nullptr;
        int ret = mdb_txn_begin(m_env, nullptr, MDB_RDONLY, &txn);
        if(ret != MDB_SUCCESS) return convertStatus(ret);
        for(size_t i = 0; i < count; i++) {
            if(offset + ksizes[i] > keys.size) return Status::InvalidArg;
            MDB_val key{ ksizes[i], keys.data + offset };
            MDB_val val { 0, nullptr };
            ret = mdb_get(txn, m_db, &key, &val);
            if(ret == MDB_NOTFOUND) {
                vsizes[i] = KeyNotFound;
            } else if(ret == MDB_SUCCESS) {
                vsizes[i] = val.mv_size;
            } else {
                mdb_txn_abort(txn);
                return convertStatus(ret);
            }
            offset += ksizes[i];
        }
        mdb_txn_abort(txn);
        return Status::OK;
    }

    virtual Status put(const UserMem& keys,
                       const BasicUserMem<size_t>& ksizes,
                       const UserMem& vals,
                       const BasicUserMem<size_t>& vsizes) override {
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

        MDB_txn* txn = nullptr;
        int ret = mdb_txn_begin(m_env, nullptr, 0, &txn);
        if(ret != MDB_SUCCESS) return convertStatus(ret);
        for(size_t i = 0; i < ksizes.size; i++) {
            MDB_val key{ ksizes[i], keys.data + key_offset};
            MDB_val val{ vsizes[i], vals.data + val_offset };
            ret = mdb_put(txn, m_db, &key, &val, 0);
            key_offset += ksizes[i];
            val_offset += vsizes[i];
            if(ret != 0) {
                mdb_txn_abort(txn);
                return convertStatus(ret);
            }
        }
        ret = mdb_txn_commit(txn);
        return convertStatus(ret);
    }

    virtual Status get(bool packed, const UserMem& keys,
                       const BasicUserMem<size_t>& ksizes,
                       UserMem& vals,
                       BasicUserMem<size_t>& vsizes) const override {
        if(ksizes.size != vsizes.size) return Status::InvalidArg;

        size_t key_offset = 0;
        size_t val_offset = 0;

        if(!packed) {

            MDB_txn* txn = nullptr;
            int ret = mdb_txn_begin(m_env, nullptr, MDB_RDONLY, &txn);
            if(ret != MDB_SUCCESS) return convertStatus(ret);
            for(size_t i = 0; i < ksizes.size; i++) {
                MDB_val key{ ksizes[i], keys.data + key_offset };
                MDB_val val{ 0, nullptr };
                ret = mdb_get(txn, m_db, &key, &val);
                size_t original_vsize = vsizes[i];
                if(ret == MDB_NOTFOUND) {
                    vsizes[i] = KeyNotFound;
                } else if(ret == MDB_SUCCESS) {
                    if(val.mv_size > vsizes[i]) {
                        vsizes[i] = BufTooSmall;
                    }else {
                        vsizes[i] = val.mv_size;
                        std::memcpy(vals.data + val_offset, val.mv_data, val.mv_size);
                    }
                } else {
                    mdb_txn_abort(txn);
                    return convertStatus(ret);
                }
                key_offset += ksizes[i];
                val_offset += original_vsize;
            }
            mdb_txn_abort(txn);

        } else { // if packed

            size_t val_remaining_size = vals.size;
            MDB_txn* txn = nullptr;
            int ret = mdb_txn_begin(m_env, nullptr, MDB_RDONLY, &txn);
            if(ret != MDB_SUCCESS) return convertStatus(ret);
            for(size_t i = 0; i < ksizes.size; i++) {
                MDB_val key{ ksizes[i], keys.data + key_offset };
                MDB_val val{ 0, nullptr };
                ret = mdb_get(txn, m_db, &key, &val);
                if(ret == MDB_NOTFOUND) {
                    vsizes[i] = KeyNotFound;
                } else if(ret == MDB_SUCCESS) {
                    if(val.mv_size > val_remaining_size) {
                        for(; i < ksizes.size; i++) {
                            vsizes[i] = BufTooSmall;
                        }
                    }else {
                        vsizes[i] = val.mv_size;
                        std::memcpy(vals.data + val_offset, val.mv_data, val.mv_size);
                        val_remaining_size -= vsizes[i];
                        val_offset += vsizes[i];
                    }
                } else {
                    mdb_txn_abort(txn);
                    return convertStatus(ret);
                }
                key_offset += ksizes[i];
            }
            mdb_txn_abort(txn);
            vals.size = vals.size - val_remaining_size;
        }
        return Status::OK;
    }

    virtual Status erase(const UserMem& keys,
                         const BasicUserMem<size_t>& ksizes) override {
        size_t key_offset = 0;
        size_t total_ksizes = std::accumulate(ksizes.data,
                                              ksizes.data + ksizes.size,
                                              0);
        if(total_ksizes > keys.size) return Status::InvalidArg;

        MDB_txn* txn = nullptr;
        int ret = mdb_txn_begin(m_env, nullptr, 0, &txn);
        if(ret != MDB_SUCCESS) return convertStatus(ret);
        for(size_t i = 0; i < ksizes.size; i++) {
            MDB_val key{ ksizes[i], keys.data + key_offset};
            MDB_val val{ 0, nullptr };
            ret = mdb_del(txn, m_db, &key, &val);
            key_offset += ksizes[i];
            if(ret != MDB_SUCCESS && ret != MDB_NOTFOUND) {
                mdb_txn_abort(txn);
                return convertStatus(ret);
            }
        }
        ret = mdb_txn_commit(txn);
        return convertStatus(ret);
    }

    virtual Status listKeys(bool packed, const UserMem& fromKey,
                            bool inclusive, const UserMem& prefix,
                            UserMem& keys, BasicUserMem<size_t>& keySizes) const override {
#if 0
        auto fromKeySlice = rocksdb::Slice{ fromKey.data, fromKey.size };
        auto prefixSlice = rocksdb::Slice { prefix.data, prefix.size };

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
            if(!key.starts_with(prefixSlice)) {
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
#endif
        return Status::NotSupported;
    }

    virtual Status listKeyValues(bool packed,
                                 const UserMem& fromKey,
                                 bool inclusive, const UserMem& prefix,
                                 UserMem& keys,
                                 BasicUserMem<size_t>& keySizes,
                                 UserMem& vals,
                                 BasicUserMem<size_t>& valSizes) const override {
#if 0
        auto fromKeySlice = rocksdb::Slice{ fromKey.data, fromKey.size };
        auto prefixSlice = rocksdb::Slice { prefix.data, prefix.size };

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
            if(!key.starts_with(prefixSlice)) {
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
#endif
        return Status::NotSupported;
    }

    ~LMDBKeyValueStore() {
        if(m_env) {
            mdb_dbi_close(m_env, m_db);
            mdb_env_close(m_env);
        }
        m_env = nullptr;
    }

    private:

    LMDBKeyValueStore(json&& cfg, MDB_env* env, MDB_dbi db)
    : m_config(std::move(cfg))
    , m_env(env)
    , m_db(db) {}

    json     m_config;
    MDB_env* m_env = nullptr;
    MDB_dbi  m_db;
};

}

RKV_REGISTER_BACKEND(lmdb, rkv::LMDBKeyValueStore);
