#include "config.h"

static const char* available_backends[] = {
    "map",
    "unordered_map",
    "set",
    "unordered_set",
#ifdef HAS_LEVELDB
    "leveldb",
#endif
#ifdef HAS_LMDB
    "lmdb",
#endif
#ifdef HAS_BERKELEYDB
    "berkeleydb",
#endif
#ifdef HAS_ROCKSDB
    "rocksdb",
#endif
#ifdef HAS_GDBM
    "gdbm",
#endif
#ifdef HAS_PMEMKV
    "pmemkv",
#endif
#ifdef HAS_TKRZW
    "tkrzw",
#endif
#ifdef HAS_UNQLITE
    "unqlite",
#endif
    NULL
};

static const char* backend_configs[] = {
    "{}",
    "{}",
    "{}",
    "{}",
#ifdef HAS_LEVELDB
    "{\"path\":\"/tmp/leveldb-test\","
    " \"create_if_missing\":true}",
#endif
#ifdef HAS_LMDB
    "{\"path\":\"/tmp/lmdb-test\","
    " \"create_if_missing\":true}",
#endif
#ifdef HAS_BERKELEYDB
    "{\"home\":\"/tmp/berkeleydb-test\","
    " \"file\":\"my-bdb\","
    " \"create_if_missing\":true,"
    " \"type\":\"btree\"}",
#endif
#ifdef HAS_ROCKSDB
    "{\"path\":\"/tmp/rocksdb-test\","
    " \"create_if_missing\":true}",
#endif
#ifdef HAS_GDBM
    "{\"path\":\"/tmp/gdbm-test\","
    " \"create_if_missing\":true}",
#endif
#ifdef HAS_PMEMKV
    "{}",
#endif
#ifdef HAS_TKRZW
    "{\"path\":\"/tmp/trkzw-test\","
    " \"type\":\"tree\"}",
#endif
#ifdef HAS_UNQLITE
    "{\"path\":\":mem:\","
    "\"mode\":\"memory\"}",
#endif
    NULL
};

inline static const char* find_backend_config_for(const char* backend) {
    unsigned i=0;
    while(backend_configs[i] != NULL) {
        if(strcmp(backend, available_backends[i]) == 0)
            return backend_configs[i];
        i += 1;
    }
    return NULL;
}

#define SKIP_IF_NOT_IMPLEMENTED(__ret__) \
    do { if(__ret__ == YOKAN_ERR_OP_UNSUPPORTED || __ret__ == YOKAN_ERR_MODE) return MUNIT_SKIP; } while(0)
