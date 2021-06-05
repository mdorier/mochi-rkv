/*
 * (C) 2020 The University of Chicago
 *
 * See COPYRIGHT in top-level directory.
 */
#include <stdio.h>
#include <margo.h>
#include <rkv/rkv-server.h>
#include <rkv/rkv-admin.h>
#include <rkv/rkv-client.h>
#include <rkv/rkv-database.h>
#include "munit/munit.h"

struct test_context {
    margo_instance_id   mid;
    hg_addr_t           addr;
    rkv_admin_t       admin;
    rkv_database_id_t id;
};

static const char* token = "ABCDEFGH";
static const uint16_t provider_id = 42;
static const char* backend_config = "{ \"foo\" : \"bar\" }";

static void* test_context_setup(const MunitParameter params[], void* user_data)
{
    (void) params;
    (void) user_data;
    rkv_return_t      ret;
    margo_instance_id mid;
    hg_addr_t         addr;
    rkv_admin_t       admin;
    rkv_database_id_t id;
    // create margo instance
    mid = margo_init("na+sm", MARGO_SERVER_MODE, 0, 0);
    munit_assert_not_null(mid);
    // set log level
    margo_set_global_log_level(MARGO_LOG_CRITICAL);
    margo_set_log_level(mid, MARGO_LOG_CRITICAL);
    // get address of current process
    hg_return_t hret = margo_addr_self(mid, &addr);
    munit_assert_int(hret, ==, HG_SUCCESS);
    // register rkv provider
    struct rkv_provider_args args = RKV_PROVIDER_ARGS_INIT;
    args.token = token;
    ret = rkv_provider_register(
            mid, provider_id, &args,
            RKV_PROVIDER_IGNORE);
    munit_assert_int(ret, ==, RKV_SUCCESS);
    // create an admin
    ret = rkv_admin_init(mid, &admin);
    munit_assert_int(ret, ==, RKV_SUCCESS);
    // open a database using the admin
    ret = rkv_open_database(admin, addr,
            provider_id, token, "map", backend_config, &id);
    munit_assert_int(ret, ==, RKV_SUCCESS);
    // create test context
    struct test_context* context = (struct test_context*)calloc(1, sizeof(*context));
    munit_assert_not_null(context);
    context->mid   = mid;
    context->addr  = addr;
    context->admin = admin;
    context->id    = id;
    return context;
}

static void test_context_tear_down(void* fixture)
{
    rkv_return_t ret;
    struct test_context* context = (struct test_context*)fixture;
    // destroy the database
    ret = rkv_destroy_database(context->admin,
            context->addr, provider_id, token, context->id);
    munit_assert_int(ret, ==, RKV_SUCCESS);
    // free the admin
    ret = rkv_admin_finalize(context->admin);
    munit_assert_int(ret, ==, RKV_SUCCESS);
    // free address
    margo_addr_free(context->mid, context->addr);
    // we are not checking the return value of the above function with
    // munit because we need margo_finalize to be called no matter what.
    margo_finalize(context->mid);
}

static MunitResult test_client(const MunitParameter params[], void* data)
{
    (void)params;
    (void)data;
    struct test_context* context = (struct test_context*)data;
    rkv_client_t client;
    rkv_return_t ret;
    // test that we can create a client object
    ret = rkv_client_init(context->mid, &client);
    munit_assert_int(ret, ==, RKV_SUCCESS);
    // test that we can free the client object
    ret = rkv_client_finalize(client);
    munit_assert_int(ret, ==, RKV_SUCCESS);

    return MUNIT_OK;
}

static MunitResult test_database(const MunitParameter params[], void* data)
{
    (void)params;
    (void)data;
    struct test_context* context = (struct test_context*)data;
    rkv_client_t client;
    rkv_database_handle_t rh;
    rkv_return_t ret;
    // test that we can create a client object
    ret = rkv_client_init(context->mid, &client);
    munit_assert_int(ret, ==, RKV_SUCCESS);
    // test that we can create a database handle
    ret = rkv_database_handle_create(client,
            context->addr, provider_id, context->id, &rh);
    munit_assert_int(ret, ==, RKV_SUCCESS);
    // test that we can increase the ref count
    ret = rkv_database_handle_ref_incr(rh);
    munit_assert_int(ret, ==, RKV_SUCCESS);
    // test that we can destroy the database handle
    ret = rkv_database_handle_release(rh);
    munit_assert_int(ret, ==, RKV_SUCCESS);
    // ... and a second time because of the increase ref 
    ret = rkv_database_handle_release(rh);
    munit_assert_int(ret, ==, RKV_SUCCESS);
    // test that we can free the client object
    ret = rkv_client_finalize(client);
    munit_assert_int(ret, ==, RKV_SUCCESS);

    return MUNIT_OK;
}


static MunitResult test_hello(const MunitParameter params[], void* data)
{
    (void)params;
    (void)data;
    struct test_context* context = (struct test_context*)data;
    rkv_client_t client;
    rkv_database_handle_t rh;
    rkv_return_t ret;
    // test that we can create a client object
    ret = rkv_client_init(context->mid, &client);
    munit_assert_int(ret, ==, RKV_SUCCESS);
    // test that we can create a database handle
    ret = rkv_database_handle_create(client,
            context->addr, provider_id, context->id, &rh);
    munit_assert_int(ret, ==, RKV_SUCCESS);
    // test that we can send a hello RPC to the database
    ret = rkv_say_hello(rh);
    munit_assert_int(ret, ==, RKV_SUCCESS);
    // test that we can destroy the database handle
    ret = rkv_database_handle_release(rh);
    munit_assert_int(ret, ==, RKV_SUCCESS);
    // test that we can free the client object
    ret = rkv_client_finalize(client);
    munit_assert_int(ret, ==, RKV_SUCCESS);

    return MUNIT_OK;
}

static MunitResult test_sum(const MunitParameter params[], void* data)
{
    (void)params;
    (void)data;
    struct test_context* context = (struct test_context*)data;
    rkv_client_t client;
    rkv_database_handle_t rh;
    rkv_return_t ret;
    // test that we can create a client object
    ret = rkv_client_init(context->mid, &client);
    munit_assert_int(ret, ==, RKV_SUCCESS);
    // test that we can create a database handle
    ret = rkv_database_handle_create(client,
            context->addr, provider_id, context->id, &rh);
    munit_assert_int(ret, ==, RKV_SUCCESS);
    // test that we can send a sum RPC to the database
    int32_t result = 0;
    ret = rkv_compute_sum(rh, 45, 55, &result);
    munit_assert_int(ret, ==, RKV_SUCCESS);
    munit_assert_int(result, ==, 100);
    // test that we can destroy the database handle
    ret = rkv_database_handle_release(rh);
    munit_assert_int(ret, ==, RKV_SUCCESS);
    // test that we can free the client object
    ret = rkv_client_finalize(client);
    munit_assert_int(ret, ==, RKV_SUCCESS);

    return MUNIT_OK;
}

static MunitResult test_invalid(const MunitParameter params[], void* data)
{
    (void)params;
    (void)data;
    struct test_context* context = (struct test_context*)data;
    rkv_client_t client;
    rkv_database_handle_t rh1, rh2;
    rkv_database_id_t invalid_id;
    rkv_return_t ret;
    // test that we can create a client object
    ret = rkv_client_init(context->mid, &client);
    munit_assert_int(ret, ==, RKV_SUCCESS);
    // create a database handle for a wrong database id
    ret = rkv_database_handle_create(client,
            context->addr, provider_id, invalid_id, &rh1);
    munit_assert_int(ret, ==, RKV_SUCCESS);
    // create a database handle for a wrong provider id
    ret = rkv_database_handle_create(client,
            context->addr, provider_id + 1, context->id, &rh2);
    munit_assert_int(ret, ==, RKV_SUCCESS);
    // test sending to the invalid database id
    int32_t result;
    ret = rkv_compute_sum(rh1, 45, 55, &result);
    munit_assert_int(ret, ==, RKV_ERR_INVALID_DATABASE);
    // test sending to the invalid provider id
    ret = rkv_compute_sum(rh2, 45, 55, &result);
    munit_assert_int(ret, ==, RKV_ERR_FROM_MERCURY);
    // test that we can destroy the database handle
    ret = rkv_database_handle_release(rh1);
    munit_assert_int(ret, ==, RKV_SUCCESS);
    // test that we can destroy the database handle
    ret = rkv_database_handle_release(rh2);
    munit_assert_int(ret, ==, RKV_SUCCESS);
    // test that we can free the client object
    ret = rkv_client_finalize(client);
    munit_assert_int(ret, ==, RKV_SUCCESS);

    return MUNIT_OK;
}

static MunitTest test_suite_tests[] = {
    { (char*) "/client",   test_client,   test_context_setup, test_context_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
    { (char*) "/database", test_database, test_context_setup, test_context_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
    { (char*) "/hello",    test_hello,    test_context_setup, test_context_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
    { (char*) "/sum",      test_sum,      test_context_setup, test_context_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
    { (char*) "/invalid",  test_invalid,  test_context_setup, test_context_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite test_suite = { 
    (char*) "/rkv/admin", test_suite_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int main(int argc, char* argv[MUNIT_ARRAY_PARAM(argc + 1)]) {
    return munit_suite_main(&test_suite, (void*) "rkv", argc, argv);
}
