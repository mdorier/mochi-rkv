/*
 * (C) 2021 The University of Chicago
 *
 * See COPYRIGHT in top-level directory.
 */
#ifndef __RKV_BACKEND_H
#define __RKV_BACKEND_H

#include <limits>
#include <vector>
#include <string>
#include <unordered_map>
#include <functional>
#include <rkv/rkv-common.h>

template <typename T> class __RKVBackendRegistration;

namespace rkv {

/**
 * @brief Wrapper for user memory (equivalent to
 * some backend's notion of Slice, or to a C++17 std::string_view)
 *
 * @tparam T Type of data held.
 */
template<typename T>
struct BasicUserMem {
    T*     data = nullptr; /*!< Pointer to the data */
    size_t size = 0;       /*!< Number of elements of type T in the buffer */

    template<typename I>
    inline T& operator[](I index) {
        return data[index];
    }

    template<typename I>
    inline const T& operator[](I index) const {
        return data[index];
    }
};

/**
 * @brief UserMem is short for BasicUserMem<void>.
 * Its size field represents a number of bytes for
 * a buffer of unspecified type.
 */
using UserMem = BasicUserMem<char>;

/**
 * @brief The BitField class is used for the "exists" operations
 * to expose user memory with bitwise operations.
 */
struct BitField {
    uint8_t* data = nullptr; /*!< Pointer to the data */
    size_t   size = 0;       /*!< Number of bits in the bitfield */

    struct BitFieldAccessor {
        uint8_t* data;
        uint8_t  mask;

        operator bool() const {
            return *data & mask;
        }

        BitFieldAccessor& operator=(bool b) {
            if(b) *data |= mask;
            else *data &= ~mask;
            return *this;
        }
    };

    template<typename I>
    inline BitFieldAccessor operator[](I index) {
        uint8_t mask = 1 << (index % 8);
        return BitFieldAccessor{data + (index/8), mask};
    }
};

/**
 * @brief Status returned by all the backend functions.
 */
enum class Status : uint8_t {
    OK           = RKV_SUCCESS,
    InvalidType  = RKV_ERR_INVALID_BACKEND,
    InvalidConf  = RKV_ERR_INVALID_CONFIG,
    InvalidArg   = RKV_ERR_INVALID_ARGS,
    NotFound     = RKV_ERR_KEY_NOT_FOUND,
    SizeError    = RKV_ERR_BUFFER_SIZE,
    KeyExists    = RKV_ERR_KEY_EXISTS,
    NotSupported = RKV_ERR_OP_UNSUPPORTED
};

/**
 * @brief Size used for a UserMem value when the key was not found.
 */
constexpr auto KeyNotFound = RKV_KEY_NOT_FOUND;

/**
 * @brief Size used for a UserMem value when the provided buffer
 * was too small to hold the value.
 */
constexpr auto BufTooSmall = RKV_SIZE_TOO_SMALL;

/**
 * @brief Abstract embedded key/value storage object.
 *
 * Note: in the interest of forcing implementers to think about
 * optimizing their backends, all the methods are pure virtual,
 * even if some methods could be implemented in terms of other methods.
 */
class KeyValueStoreInterface {

    public:

    KeyValueStoreInterface() = default;
    virtual ~KeyValueStoreInterface() = default;
    KeyValueStoreInterface(const KeyValueStoreInterface&) = default;
    KeyValueStoreInterface(KeyValueStoreInterface&&) = default;
    KeyValueStoreInterface& operator=(const KeyValueStoreInterface&) = default;
    KeyValueStoreInterface& operator=(KeyValueStoreInterface&&) = default;

    /**
     * @brief Get the name of the backend (e.g. "map").
     *
     * @return The name of the backend.
     */
    virtual std::string name() const = 0;

    /**
     * @brief Get the internal configuration as a JSON-formatted string.
     *
     * @return Backend configuration.
     */
    virtual std::string config() const = 0;

    /**
     * @brief Destroy the resources (files, etc.) associated with the database.
     */
    virtual void destroy() = 0;

    /**
     * @brief Check if the provided keys exist. The keys are packed
     * into a single buffer. ksizes provides a pointer to the memory
     * holding the key sizes. The number of keys is conveyed by
     * ksizes.size and b.size, which should be equal (otherwise
     * Status::InvalidArg is returned).
     *
     * @param keys Packed keys.
     * @param ksizes Packed key sizes.
     * @param b Memory segment holding booleans indicating whether each key exists.
     *
     * @return Status.
     */
    virtual Status exists(const UserMem& keys,
                          const BasicUserMem<size_t>& ksizes,
                          BitField& b) const = 0;
    /**
     * @brief Get the size of values associated with the keys. The keys are packed
     * into a single buffer. ksizes provides a pointer to the memory holding the
     * key sizes. vsizes provides a pointer to the memory where the value sizes need
     * to be stored. The number of keys is conveyed by ksizes.size and vsizes.size,
     * which should be equal (otherwise Status::InvalidArg is returned).
     *
     * @param keys Packed keys.
     * @param ksizes Packed key sizes
     * @param b Memory segment to store value sizes.
     *
     * @return Status.
     */
    virtual Status length(const UserMem& keys,
                          const BasicUserMem<size_t>& ksizes,
                          BasicUserMem<size_t>& vsizes) const = 0;

    /**
     * @brief Put multiple key/value pairs into the database.
     * The keys, ksizes, values, and vsizes are packed into user-provided
     * memory segments. The number of key/value pairs is conveyed by
     * ksizes.size and vsizes.size, which should be equal.
     *
     * @param [in] keys Keys to put.
     * @param [in] ksizes Key sizes.
     * @param [in] vals Values to put.
     * @param [in] vsizes Value sizes.
     *
     * @return Status.
     */
    virtual Status put(const UserMem& keys,
                       const BasicUserMem<size_t>& ksizes,
                       const UserMem& vals,
                       const BasicUserMem<size_t>& vsizes) = 0;

    /**
     * @brief This verion of getMulti uses the user-provided memory.
     * vsizes is used both as input (to know where to place data in vals
     * and how much is available to each value) and as output (to store
     * the actual size of each value).
     *
     * This function expects (and will not check) that
     * - ksizes.size == vsizes.size
     * - the sum of ksizes <= keys.size
     * - the sum of vsizes <= vals.size
     *
     * @param keys Keys to get.
     * @param ksizes Size of the keys.
     * @param vals Values to get.
     * @param vsizes Size of the values.
     *
     * @return Status.
     */
    virtual Status get(bool packed, const UserMem& keys,
                       const BasicUserMem<size_t>& ksizes,
                       UserMem& vals,
                       BasicUserMem<size_t>& vsizes) const = 0;

    /**
     * @brief Erase a set of key/value pairs. Keys are packed into
     * a single buffer. The number of keys is conveyed by ksizes.size.
     *
     * @param [in] keys Keys to erase.
     * @param [in] ksizes Size of the keys.
     *
     * @return Status.
     */
    virtual Status erase(const UserMem& keys,
                         const BasicUserMem<size_t>& ksizes) = 0;

    /**
     * @brief This version of listKeys uses a single contiguous buffer
     * to hold all the keys. Their size is stored in the keySizes user-allocated
     * buffer. After a successful call, keySizes.size holds the number of keys read.
     * The function will try to read up to keySizes.size keys.
     *
     * keySizes is considered an input and an output. As input, it provides the
     * size that should be used for each keys in the keys buffer. As an output,
     * it stores the actual size of each key.
     *
     * @param [in] fromKey Starting key.
     * @param [in] inclusive Whether to include the start key if found.
     * @param [in] prefix Prefix to filter with.
     * @param [out] keys Resulting keys.
     * @param [inout] keySizes Resulting key sizes.
     *
     * @return Status.
     */
    virtual Status listKeys(bool packed, const UserMem& fromKey,
                            bool inclusive, const UserMem& prefix,
                            UserMem& keys, BasicUserMem<size_t>& keySizes) const = 0;

    /**
     * @brief Same as listKeys but also returns the values.
     */
    virtual Status listKeyValues(bool packed,
                                 const UserMem& fromKey,
                                 bool inclusive, const UserMem& prefix,
                                 UserMem& keys,
                                 BasicUserMem<size_t>& keySizes,
                                 UserMem& vals,
                                 BasicUserMem<size_t>& valSizes) const = 0;
};

/**
 * @brief The KeyValueStoreFactory is used by the provider to build
 * key/value store instances of various types.
 */
class KeyValueStoreFactory {

    template <typename T> friend class ::__RKVBackendRegistration;

    public:

    KeyValueStoreFactory() = delete;

    /**
     * @brief Create a KeyValueStoreInterface object of a specified
     * type and return a pointer to it. It is the respondibility of
     * the user to call delete on this pointer.
     *
     * If the function fails, it will return a Status error.
     *
     * @param backendType Name of the backend.
     * @param jsonConfig Json-formatted configuration string.
     * @param kvs Created KeyValueStoreInterface pointer.
     *
     * @return Status.
     */
    static Status makeKeyValueStore(
            const std::string& backendType,
            const std::string& jsonConfig,
            KeyValueStoreInterface** kvs) {
        if(!hasBackendType(backendType)) return Status::InvalidType;
        return make_fn[backendType](jsonConfig, kvs);
    }

    /**
     * @brief Check if the backend type is available in the factory.
     *
     * @param backendType Name of the backend.
     *
     * @return whether the backend type is available.
     */
    static inline bool hasBackendType(const std::string& backendType) {
        return make_fn.count(backendType) != 0;
    }

    private:

    static std::unordered_map<
                std::string,
                std::function<Status(const std::string&,KeyValueStoreInterface**)>>
        make_fn; /*!< Map of factory functions for each backend type */
};

}

/**
 * @brief This macro should be used to register new backend types.
 */
#define RKV_REGISTER_BACKEND(__backend_name, __backend_type) \
    static __RKVBackendRegistration<__backend_type>          \
    __rkv##__backend_name##_backend(#__backend_name)

template <typename T> class __RKVBackendRegistration {

    public:

    __RKVBackendRegistration(const std::string &backend_name) {
        rkv::KeyValueStoreFactory::make_fn[backend_name] =
            [](const std::string &config, rkv::KeyValueStoreInterface** kvs) {
                return T::create(config, kvs);
            };
    }
};

using rkv_database = rkv::KeyValueStoreInterface;
using rkv_database_t = rkv::KeyValueStoreInterface*;

#endif
