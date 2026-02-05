/**
 * @file schema_mount_example.c
 * @author Michal Vasko <mvasko@cesnet.cz>
 * @brief Example of parsing schema-mount data
 *
 * Copyright (c) 2026 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <libyang/libyang.h>

static LY_ERR
sm_ext_data_clb(const struct lysc_ext_instance *ext, const struct lyd_node *parent, void *user_data, void **ext_data,
        ly_bool *ext_data_free)
{
    static int recursive_call = 0;
    LY_ERR r;

    *ext_data = NULL;
    *ext_data_free = 0;

    if (recursive_call) {
        /* called recursively, when processing only `ietf-yang-library` and `ietf-yang-schema-mount` data,
         * NULL ext_data can be returned */
        return LY_SUCCESS;
    }

    /* the required operational data include mounted data so we must prevent this callback from being called
     * recursively again, infinitely */
    recursive_call = 1;
    r = lyd_parse_data_path(ext->module->ctx, "sm_oper_data.xml", LYD_XML, LYD_PARSE_STRICT, LYD_VALIDATE_PRESENT,
            (struct lyd_node **)ext_data);
    recursive_call = 0;

    if (r) {
        return r;
    }

    *ext_data_free = 1;
    return LY_SUCCESS;
}

int
main(int argc, char **argv)
{
    struct ly_ctx *ctx;
    struct lyd_node *data;
    const struct lys_module *mod;

    /* create basic context */
    ly_ctx_new(".", 0, &ctx);
    mod = ly_ctx_load_module(ctx, "ietf-interfaces", NULL, NULL);
    assert(mod);
    mod = ly_ctx_load_module(ctx, "ietf-ip", NULL, NULL);
    assert(mod);
    mod = ly_ctx_load_module(ctx, "ietf-network-instance", NULL, NULL);
    assert(mod);

    /* set schema-mount callback */
    ly_ctx_set_ext_data_clb(ctx, sm_ext_data_clb, NULL);

    /* parse configuration data with mounted data normally */
    lyd_parse_data_path(ctx, "config_data.xml", LYD_XML, LYD_PARSE_STRICT, LYD_VALIDATE_PRESENT, &data);
    assert(data);

    /* print the parsed data */
    lyd_print_file(stdout, data, LYD_XML, 0);

    /* cleanup */
    lyd_free_siblings(data);
    ly_ctx_destroy(ctx);
    return 0;
}
