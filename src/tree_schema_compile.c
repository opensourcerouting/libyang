/**
 * @file tree_schema.c
 * @author Radek Krejci <rkrejci@cesnet.cz>
 * @brief Schema tree implementation
 *
 * Copyright (c) 2015 - 2018 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#define _GNU_SOURCE

#include <assert.h>
#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "compat.h"
#include "context.h"
#include "dict.h"
#include "log.h"
#include "parser.h"
#include "parser_schema.h"
#include "path.h"
#include "plugins_exts.h"
#include "plugins_exts_internal.h"
#include "plugins_types.h"
#include "set.h"
#include "tree.h"
#include "tree_data.h"
#include "tree_data_internal.h"
#include "tree_schema.h"
#include "tree_schema_internal.h"
#include "xpath.h"

static LY_ERR lys_compile_ext(struct lysc_ctx *ctx, struct lysp_ext_instance *ext_p, struct lysc_ext_instance *ext,
        void *parent, LYEXT_PARENT parent_type, const struct lys_module *ext_mod);

static LY_ERR lysp_qname_dup(const struct ly_ctx *ctx, struct lysp_qname *qname, const struct lysp_qname *orig_qname);

/**
 * @defgroup scflags Schema compile flags
 *
 * Flags are currently used only internally - the compilation process does not have a public interface and it is
 * integrated in the schema parsers. The current options set does not make sense for public used, but it can be a way
 * to modify behavior of the compilation process in future.
 *
 * @{
 */
#define LYS_COMPILE_RPC_INPUT  LYS_CONFIG_W       /**< Internal option when compiling schema tree of RPC/action input */
#define LYS_COMPILE_RPC_OUTPUT LYS_CONFIG_R       /**< Internal option when compiling schema tree of RPC/action output */
#define LYS_COMPILE_RPC_MASK   LYS_CONFIG_MASK    /**< mask for the internal RPC options */
#define LYS_COMPILE_NOTIFICATION 0x08             /**< Internal option when compiling schema tree of Notification */

#define LYS_COMPILE_GROUPING   0x10               /** Compiling (validation) of a non-instantiated grouping.
                                                      In this case not all the restrictions are checked since they can be valid only
                                                      in the real placement of the grouping. TODO - what specifically is not done */
/** @} scflags */

/**
 * @brief Duplicate string into dictionary
 * @param[in] CTX libyang context of the dictionary.
 * @param[in] ORIG String to duplicate.
 * @param[out] DUP Where to store the result.
 */
#define DUP_STRING(CTX, ORIG, DUP, RET) if (ORIG) {RET = lydict_insert(CTX, ORIG, 0, &DUP);}

#define DUP_STRING_GOTO(CTX, ORIG, DUP, RET, GOTO) if (ORIG) {LY_CHECK_GOTO(RET = lydict_insert(CTX, ORIG, 0, &DUP), GOTO);}

#define DUP_ARRAY(CTX, ORIG_ARRAY, NEW_ARRAY, DUP_FUNC) \
    if (ORIG_ARRAY) { \
        LY_ARRAY_COUNT_TYPE u; \
        LY_ARRAY_CREATE_RET(CTX, NEW_ARRAY, LY_ARRAY_COUNT(ORIG_ARRAY), LY_EMEM); \
        LY_ARRAY_FOR(ORIG_ARRAY, u) { \
            LY_ARRAY_INCREMENT(NEW_ARRAY); \
            LY_CHECK_RET(DUP_FUNC(CTX, &(NEW_ARRAY)[u], &(ORIG_ARRAY)[u])); \
        } \
    }

#define COMPILE_ARRAY_GOTO(CTX, ARRAY_P, ARRAY_C, ITER, FUNC, RET, GOTO) \
    if (ARRAY_P) { \
        LY_ARRAY_CREATE_GOTO((CTX)->ctx, ARRAY_C, LY_ARRAY_COUNT(ARRAY_P), RET, GOTO); \
        LY_ARRAY_COUNT_TYPE __array_offset = LY_ARRAY_COUNT(ARRAY_C); \
        for (ITER = 0; ITER < LY_ARRAY_COUNT(ARRAY_P); ++ITER) { \
            LY_ARRAY_INCREMENT(ARRAY_C); \
            RET = FUNC(CTX, &(ARRAY_P)[ITER], &(ARRAY_C)[ITER + __array_offset]); \
            LY_CHECK_GOTO(RET != LY_SUCCESS, GOTO); \
        } \
    }

#define COMPILE_OP_ARRAY_GOTO(CTX, ARRAY_P, ARRAY_C, PARENT, ITER, FUNC, USES_STATUS, RET, GOTO) \
    if (ARRAY_P) { \
        LY_ARRAY_CREATE_GOTO((CTX)->ctx, ARRAY_C, LY_ARRAY_COUNT(ARRAY_P), RET, GOTO); \
        LY_ARRAY_COUNT_TYPE __array_offset = LY_ARRAY_COUNT(ARRAY_C); \
        for (ITER = 0; ITER < LY_ARRAY_COUNT(ARRAY_P); ++ITER) { \
            LY_ARRAY_INCREMENT(ARRAY_C); \
            RET = FUNC(CTX, &(ARRAY_P)[ITER], PARENT, &(ARRAY_C)[ITER + __array_offset], USES_STATUS); \
            if (RET == LY_EDENIED) { \
                LY_ARRAY_DECREMENT(ARRAY_C); \
            } else if (RET != LY_SUCCESS) { \
                goto GOTO; \
            } \
        } \
    }

#define COMPILE_EXTS_GOTO(CTX, EXTS_P, EXT_C, PARENT, PARENT_TYPE, RET, GOTO) \
    if (EXTS_P) { \
        LY_ARRAY_CREATE_GOTO((CTX)->ctx, EXT_C, LY_ARRAY_COUNT(EXTS_P), RET, GOTO); \
        for (LY_ARRAY_COUNT_TYPE __exts_iter = 0, __array_offset = LY_ARRAY_COUNT(EXT_C); __exts_iter < LY_ARRAY_COUNT(EXTS_P); ++__exts_iter) { \
            LY_ARRAY_INCREMENT(EXT_C); \
            RET = lys_compile_ext(CTX, &(EXTS_P)[__exts_iter], &(EXT_C)[__exts_iter + __array_offset], PARENT, PARENT_TYPE, NULL); \
            LY_CHECK_GOTO(RET != LY_SUCCESS, GOTO); \
        } \
    }

#define COMPILE_ARRAY_UNIQUE_GOTO(CTX, ARRAY_P, ARRAY_C, ITER, FUNC, RET, GOTO) \
    if (ARRAY_P) { \
        LY_ARRAY_CREATE_GOTO((CTX)->ctx, ARRAY_C, LY_ARRAY_COUNT(ARRAY_P), RET, GOTO); \
        LY_ARRAY_COUNT_TYPE __array_offset = LY_ARRAY_COUNT(ARRAY_C); \
        for (ITER = 0; ITER < LY_ARRAY_COUNT(ARRAY_P); ++ITER) { \
            LY_ARRAY_INCREMENT(ARRAY_C); \
            RET = FUNC(CTX, &(ARRAY_P)[ITER], ARRAY_C, &(ARRAY_C)[ITER + __array_offset]); \
            LY_CHECK_GOTO(RET != LY_SUCCESS, GOTO); \
        } \
    }

#define COMPILE_MEMBER_GOTO(CTX, MEMBER_P, MEMBER_C, FUNC, RET, GOTO) \
    if (MEMBER_P) { \
        MEMBER_C = calloc(1, sizeof *(MEMBER_C)); \
        LY_CHECK_ERR_GOTO(!(MEMBER_C), LOGMEM((CTX)->ctx); RET = LY_EMEM, GOTO); \
        RET = FUNC(CTX, MEMBER_P, MEMBER_C); \
        LY_CHECK_GOTO(RET != LY_SUCCESS, GOTO); \
    }

#define COMPILE_MEMBER_ARRAY_GOTO(CTX, MEMBER_P, ARRAY_C, FUNC, RET, GOTO) \
    if (MEMBER_P) { \
        LY_ARRAY_CREATE_GOTO((CTX)->ctx, ARRAY_C, 1, RET, GOTO); \
        LY_ARRAY_COUNT_TYPE __array_offset = LY_ARRAY_COUNT(ARRAY_C); \
        LY_ARRAY_INCREMENT(ARRAY_C); \
        RET = FUNC(CTX, MEMBER_P, &(ARRAY_C)[__array_offset]); \
        LY_CHECK_GOTO(RET != LY_SUCCESS, GOTO); \
    }

#define COMPILE_CHECK_UNIQUENESS_ARRAY(CTX, ARRAY, MEMBER, EXCL, STMT, IDENT) \
    if (ARRAY) { \
        for (LY_ARRAY_COUNT_TYPE u__ = 0; u__ < LY_ARRAY_COUNT(ARRAY); ++u__) { \
            if (&(ARRAY)[u__] != EXCL && (void*)((ARRAY)[u__].MEMBER) == (void*)(IDENT)) { \
                LOGVAL((CTX)->ctx, LY_VLOG_STR, (CTX)->path, LY_VCODE_DUPIDENT, IDENT, STMT); \
                return LY_EVALID; \
            } \
        } \
    }

#define COMPILE_CHECK_UNIQUENESS_PARRAY(CTX, ARRAY, MEMBER, EXCL, STMT, IDENT) \
    if (ARRAY) { \
        for (LY_ARRAY_COUNT_TYPE u__ = 0; u__ < LY_ARRAY_COUNT(ARRAY); ++u__) { \
            if (&(ARRAY)[u__] != EXCL && (void*)((ARRAY)[u__]->MEMBER) == (void*)(IDENT)) { \
                LOGVAL((CTX)->ctx, LY_VLOG_STR, (CTX)->path, LY_VCODE_DUPIDENT, IDENT, STMT); \
                return LY_EVALID; \
            } \
        } \
    }

struct lysc_ext *
lysc_ext_dup(struct lysc_ext *orig)
{
    ++orig->refcount;
    return orig;
}

static struct lysc_ext_instance *
lysc_ext_instance_dup(struct ly_ctx *ctx, struct lysc_ext_instance *orig)
{
    /* TODO - extensions, increase refcount */
    (void) ctx;
    (void) orig;
    return NULL;
}

/**
 * @brief Add/replace a leaf default value in unres.
 * Can also be used for a single leaf-list default value.
 *
 * @param[in] ctx Compile context.
 * @param[in] leaf Leaf with the default value.
 * @param[in] dflt Default value to use.
 * @return LY_ERR value.
 */
static LY_ERR
lysc_unres_leaf_dflt_add(struct lysc_ctx *ctx, struct lysc_node_leaf *leaf, struct lysp_qname *dflt)
{
    struct lysc_unres_dflt *r = NULL;
    uint32_t i;

    for (i = 0; i < ctx->dflts.count; ++i) {
        if (((struct lysc_unres_dflt *)ctx->dflts.objs[i])->leaf == leaf) {
            /* just replace the default */
            r = ctx->dflts.objs[i];
            lysp_qname_free(ctx->ctx, r->dflt);
            free(r->dflt);
            break;
        }
    }
    if (!r) {
        /* add new unres item */
        r = calloc(1, sizeof *r);
        LY_CHECK_ERR_RET(!r, LOGMEM(ctx->ctx), LY_EMEM);
        r->leaf = leaf;

        LY_CHECK_RET(ly_set_add(&ctx->dflts, r, 1, NULL));
    }

    r->dflt = malloc(sizeof *r->dflt);
    LY_CHECK_GOTO(!r->dflt, error);
    LY_CHECK_GOTO(lysp_qname_dup(ctx->ctx, r->dflt, dflt), error);

    return LY_SUCCESS;

error:
    free(r->dflt);
    LOGMEM(ctx->ctx);
    return LY_EMEM;
}

/**
 * @brief Add/replace a leaf-list default value(s) in unres.
 *
 * @param[in] ctx Compile context.
 * @param[in] llist Leaf-list with the default value.
 * @param[in] dflts Sized array of the default values.
 * @return LY_ERR value.
 */
static LY_ERR
lysc_unres_llist_dflts_add(struct lysc_ctx *ctx, struct lysc_node_leaflist *llist, struct lysp_qname *dflts)
{
    struct lysc_unres_dflt *r = NULL;
    uint32_t i;

    for (i = 0; i < ctx->dflts.count; ++i) {
        if (((struct lysc_unres_dflt *)ctx->dflts.objs[i])->llist == llist) {
            /* just replace the defaults */
            r = ctx->dflts.objs[i];
            lysp_qname_free(ctx->ctx, r->dflt);
            free(r->dflt);
            r->dflt = NULL;
            FREE_ARRAY(ctx->ctx, r->dflts, lysp_qname_free);
            r->dflts = NULL;
            break;
        }
    }
    if (!r) {
        r = calloc(1, sizeof *r);
        LY_CHECK_ERR_RET(!r, LOGMEM(ctx->ctx), LY_EMEM);
        r->llist = llist;

        LY_CHECK_RET(ly_set_add(&ctx->dflts, r, 1, NULL));
    }

    DUP_ARRAY(ctx->ctx, dflts, r->dflts, lysp_qname_dup);

    return LY_SUCCESS;
}

static void
lysc_unres_dflt_free(const struct ly_ctx *ctx, struct lysc_unres_dflt *r)
{
    assert(!r->dflt || !r->dflts);
    if (r->dflt) {
        lysp_qname_free((struct ly_ctx *)ctx, r->dflt);
        free(r->dflt);
    } else {
        FREE_ARRAY((struct ly_ctx *)ctx, r->dflts, lysp_qname_free);
    }
    free(r);
}

void
lysc_update_path(struct lysc_ctx *ctx, struct lysc_node *parent, const char *name)
{
    int len;
    uint8_t nextlevel = 0; /* 0 - no starttag, 1 - '/' starttag, 2 - '=' starttag + '}' endtag */

    if (!name) {
        /* removing last path segment */
        if (ctx->path[ctx->path_len - 1] == '}') {
            for ( ; ctx->path[ctx->path_len] != '=' && ctx->path[ctx->path_len] != '{'; --ctx->path_len) {}
            if (ctx->path[ctx->path_len] == '=') {
                ctx->path[ctx->path_len++] = '}';
            } else {
                /* not a top-level special tag, remove also preceiding '/' */
                goto remove_nodelevel;
            }
        } else {
remove_nodelevel:
            for ( ; ctx->path[ctx->path_len] != '/'; --ctx->path_len) {}
            if (ctx->path_len == 0) {
                /* top-level (last segment) */
                ctx->path_len = 1;
            }
        }
        /* set new terminating NULL-byte */
        ctx->path[ctx->path_len] = '\0';
    } else {
        if (ctx->path_len > 1) {
            if (!parent && (ctx->path[ctx->path_len - 1] == '}') && (ctx->path[ctx->path_len - 2] != '\'')) {
                /* extension of the special tag */
                nextlevel = 2;
                --ctx->path_len;
            } else {
                /* there is already some path, so add next level */
                nextlevel = 1;
            }
        } /* else the path is just initiated with '/', so do not add additional slash in case of top-level nodes */

        if (nextlevel != 2) {
            if ((parent && (parent->module == ctx->mod)) || (!parent && (ctx->path_len > 1) && (name[0] == '{'))) {
                /* module not changed, print the name unprefixed */
                len = snprintf(&ctx->path[ctx->path_len], LYSC_CTX_BUFSIZE - ctx->path_len, "%s%s", nextlevel ? "/" : "", name);
            } else {
                len = snprintf(&ctx->path[ctx->path_len], LYSC_CTX_BUFSIZE - ctx->path_len, "%s%s:%s", nextlevel ? "/" : "", ctx->mod->name, name);
            }
        } else {
            len = snprintf(&ctx->path[ctx->path_len], LYSC_CTX_BUFSIZE - ctx->path_len, "='%s'}", name);
        }
        if (len >= LYSC_CTX_BUFSIZE - (int)ctx->path_len) {
            /* output truncated */
            ctx->path_len = LYSC_CTX_BUFSIZE - 1;
        } else {
            ctx->path_len += len;
        }
    }
}

/**
 * @brief Duplicate the compiled pattern structure.
 *
 * Instead of duplicating memory, the reference counter in the @p orig is increased.
 *
 * @param[in] orig The pattern structure to duplicate.
 * @return The duplicated structure to use.
 */
static struct lysc_pattern *
lysc_pattern_dup(struct lysc_pattern *orig)
{
    ++orig->refcount;
    return orig;
}

/**
 * @brief Duplicate the array of compiled patterns.
 *
 * The sized array itself is duplicated, but the pattern structures are just shadowed by increasing their reference counter.
 *
 * @param[in] ctx Libyang context for logging.
 * @param[in] orig The patterns sized array to duplicate.
 * @return New sized array as a copy of @p orig.
 * @return NULL in case of memory allocation error.
 */
static struct lysc_pattern **
lysc_patterns_dup(struct ly_ctx *ctx, struct lysc_pattern **orig)
{
    struct lysc_pattern **dup = NULL;
    LY_ARRAY_COUNT_TYPE u;

    assert(orig);

    LY_ARRAY_CREATE_RET(ctx, dup, LY_ARRAY_COUNT(orig), NULL);
    LY_ARRAY_FOR(orig, u) {
        dup[u] = lysc_pattern_dup(orig[u]);
        LY_ARRAY_INCREMENT(dup);
    }
    return dup;
}

/**
 * @brief Duplicate compiled range structure.
 *
 * @param[in] ctx Libyang context for logging.
 * @param[in] orig The range structure to be duplicated.
 * @return New compiled range structure as a copy of @p orig.
 * @return NULL in case of memory allocation error.
 */
struct lysc_range *
lysc_range_dup(struct ly_ctx *ctx, const struct lysc_range *orig)
{
    struct lysc_range *dup;
    LY_ERR ret;

    assert(orig);

    dup = calloc(1, sizeof *dup);
    LY_CHECK_ERR_RET(!dup, LOGMEM(ctx), NULL);
    if (orig->parts) {
        LY_ARRAY_CREATE_GOTO(ctx, dup->parts, LY_ARRAY_COUNT(orig->parts), ret, cleanup);
        LY_ARRAY_COUNT(dup->parts) = LY_ARRAY_COUNT(orig->parts);
        memcpy(dup->parts, orig->parts, LY_ARRAY_COUNT(dup->parts) * sizeof *dup->parts);
    }
    DUP_STRING_GOTO(ctx, orig->eapptag, dup->eapptag, ret, cleanup);
    DUP_STRING_GOTO(ctx, orig->emsg, dup->emsg, ret, cleanup);
    dup->exts = lysc_ext_instance_dup(ctx, orig->exts);

    return dup;
cleanup:
    free(dup);
    (void) ret; /* set but not used due to the return type */
    return NULL;
}

/**
 * @brief Stack for processing if-feature expressions.
 */
struct iff_stack {
    size_t size;    /**< number of items in the stack */
    size_t index;   /**< first empty item */
    uint8_t *stack; /**< stack - array of @ref ifftokens to create the if-feature expression in prefix format */
};

/**
 * @brief Add @ref ifftokens into the stack.
 * @param[in] stack The if-feature stack to use.
 * @param[in] value One of the @ref ifftokens to store in the stack.
 * @return LY_EMEM in case of memory allocation error
 * @return LY_ESUCCESS if the value successfully stored.
 */
static LY_ERR
iff_stack_push(struct iff_stack *stack, uint8_t value)
{
    if (stack->index == stack->size) {
        stack->size += 4;
        stack->stack = ly_realloc(stack->stack, stack->size * sizeof *stack->stack);
        LY_CHECK_ERR_RET(!stack->stack, LOGMEM(NULL); stack->size = 0, LY_EMEM);
    }
    stack->stack[stack->index++] = value;
    return LY_SUCCESS;
}

/**
 * @brief Get (and remove) the last item form the stack.
 * @param[in] stack The if-feature stack to use.
 * @return The value from the top of the stack.
 */
static uint8_t
iff_stack_pop(struct iff_stack *stack)
{
    assert(stack && stack->index);

    stack->index--;
    return stack->stack[stack->index];
}

/**
 * @brief Clean up the stack.
 * @param[in] stack The if-feature stack to use.
 */
static void
iff_stack_clean(struct iff_stack *stack)
{
    stack->size = 0;
    free(stack->stack);
}

/**
 * @brief Store the @ref ifftokens (@p op) on the given position in the 2bits array
 * (libyang format of the if-feature expression).
 * @param[in,out] list The 2bits array to modify.
 * @param[in] op The operand (@ref ifftokens) to store.
 * @param[in] pos Position (0-based) where to store the given @p op.
 */
static void
iff_setop(uint8_t *list, uint8_t op, size_t pos)
{
    uint8_t *item;
    uint8_t mask = 3;

    assert(op <= 3); /* max 2 bits */

    item = &list[pos / 4];
    mask = mask << 2 * (pos % 4);
    *item = (*item) & ~mask;
    *item = (*item) | (op << 2 * (pos % 4));
}

#define LYS_IFF_LP 0x04 /**< Additional, temporary, value of @ref ifftokens: ( */
#define LYS_IFF_RP 0x08 /**< Additional, temporary, value of @ref ifftokens: ) */

/**
 * @brief Find a feature of the given name and referenced in the given module.
 *
 * If the compiled schema is available (the schema is implemented), the feature from the compiled schema is
 * returned. Otherwise, the special array of pre-compiled features is used to search for the feature. Such
 * features are always disabled (feature from not implemented schema cannot be enabled), but in case the schema
 * will be made implemented in future (no matter if implicitly via augmenting/deviating it or explicitly via
 * ly_ctx_module_implement()), the compilation of these feature structure is finished, but the pointers
 * assigned till that time will be still valid.
 *
 * @param[in] mod Module where the feature was referenced (used to resolve prefix of the feature).
 * @param[in] name Name of the feature including possible prefix.
 * @param[in] len Length of the string representing the feature identifier in the name variable (mandatory!).
 * @return Pointer to the feature structure if found, NULL otherwise.
 */
static struct lysc_feature *
lys_feature_find(const struct lys_module *mod, const char *name, size_t len)
{
    size_t i;
    LY_ARRAY_COUNT_TYPE u;
    struct lysc_feature *f;

    assert(mod);

    for (i = 0; i < len; ++i) {
        if (name[i] == ':') {
            /* we have a prefixed feature */
            mod = lys_module_find_prefix(mod, name, i);
            LY_CHECK_RET(!mod, NULL);

            name = &name[i + 1];
            len = len - i - 1;
        }
    }

    /* we have the correct module, get the feature */
    LY_ARRAY_FOR(mod->features, u) {
        f = &mod->features[u];
        if (!ly_strncmp(f->name, name, len)) {
            return f;
        }
    }

    return NULL;
}

/**
 * @brief Fill in the prepared compiled extensions definition structure according to the parsed extension definition.
 */
static LY_ERR
lys_compile_extension(struct lysc_ctx *ctx, const struct lys_module *ext_mod, struct lysp_ext *ext_p, struct lysc_ext **ext)
{
    LY_ERR ret = LY_SUCCESS;

    if (!ext_p->compiled) {
        lysc_update_path(ctx, NULL, "{extension}");
        lysc_update_path(ctx, NULL, ext_p->name);

        /* compile the extension definition */
        ext_p->compiled = calloc(1, sizeof **ext);
        ext_p->compiled->refcount = 1;
        DUP_STRING_GOTO(ctx->ctx, ext_p->name, ext_p->compiled->name, ret, done);
        DUP_STRING_GOTO(ctx->ctx, ext_p->argument, ext_p->compiled->argument, ret, done);
        ext_p->compiled->module = (struct lys_module *)ext_mod;
        COMPILE_EXTS_GOTO(ctx, ext_p->exts, ext_p->compiled->exts, *ext, LYEXT_PAR_EXT, ret, done);

        lysc_update_path(ctx, NULL, NULL);
        lysc_update_path(ctx, NULL, NULL);

        /* find extension definition plugin */
        ext_p->compiled->plugin = lyext_get_plugin(ext_p->compiled);
    }

    *ext = lysc_ext_dup(ext_p->compiled);

done:
    return ret;
}

/**
 * @brief Fill in the prepared compiled extension instance structure according to the parsed extension instance.
 *
 * @param[in] ctx Compilation context.
 * @param[in] ext_p Parsed extension instance.
 * @param[in,out] ext Prepared compiled extension instance.
 * @param[in] parent Extension instance parent.
 * @param[in] parent_type Extension instance parent type.
 * @param[in] ext_mod Optional module with the extension instance extension definition, set only for internal annotations.
 */
static LY_ERR
lys_compile_ext(struct lysc_ctx *ctx, struct lysp_ext_instance *ext_p, struct lysc_ext_instance *ext, void *parent,
        LYEXT_PARENT parent_type, const struct lys_module *ext_mod)
{
    LY_ERR ret = LY_SUCCESS;
    const char *name;
    size_t u;
    LY_ARRAY_COUNT_TYPE v;
    const char *prefixed_name = NULL;

    DUP_STRING(ctx->ctx, ext_p->argument, ext->argument, ret);
    LY_CHECK_RET(ret);

    ext->insubstmt = ext_p->insubstmt;
    ext->insubstmt_index = ext_p->insubstmt_index;
    ext->module = ctx->mod;
    ext->parent = parent;
    ext->parent_type = parent_type;

    lysc_update_path(ctx, ext->parent_type == LYEXT_PAR_NODE ? (struct lysc_node *)ext->parent : NULL, "{extension}");

    /* get module where the extension definition should be placed */
    for (u = strlen(ext_p->name); u && ext_p->name[u - 1] != ':'; --u) {}
    if (ext_p->yin) {
        /* YIN parser has to replace prefixes by the namespace - XML namespace/prefix pairs may differs form the YANG schema's
         * namespace/prefix pair. YIN parser does not have the imports available, so mapping from XML namespace to the
         * YANG (import) prefix must be done here. */
        if (!ly_strncmp(ctx->mod_def->ns, ext_p->name, u - 1)) {
            LY_CHECK_GOTO(ret = lydict_insert(ctx->ctx, &ext_p->name[u], 0, &prefixed_name), cleanup);
            u = 0;
        } else {
            assert(ctx->mod_def->parsed);
            LY_ARRAY_FOR(ctx->mod_def->parsed->imports, v) {
                if (!ly_strncmp(ctx->mod_def->parsed->imports[v].module->ns, ext_p->name, u - 1)) {
                    char *s;
                    LY_CHECK_ERR_GOTO(asprintf(&s, "%s:%s", ctx->mod_def->parsed->imports[v].prefix, &ext_p->name[u]) == -1,
                            ret = LY_EMEM, cleanup);
                    LY_CHECK_GOTO(ret = lydict_insert_zc(ctx->ctx, s, &prefixed_name), cleanup);
                    u = strlen(ctx->mod_def->parsed->imports[v].prefix) + 1; /* add semicolon */
                    break;
                }
            }
        }
        if (!prefixed_name) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                    "Invalid XML prefix of \"%.*s\" namespace used for extension instance identifier.", u, ext_p->name);
            ret = LY_EVALID;
            goto cleanup;
        }
    } else {
        prefixed_name = ext_p->name;
    }
    lysc_update_path(ctx, NULL, prefixed_name);

    if (!ext_mod) {
        ext_mod = u ? lys_module_find_prefix(ctx->mod_def, prefixed_name, u - 1) : ctx->mod_def;
        if (!ext_mod) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                    "Invalid prefix \"%.*s\" used for extension instance identifier.", u, prefixed_name);
            ret = LY_EVALID;
            goto cleanup;
        } else if (!ext_mod->parsed->extensions) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                    "Extension instance \"%s\" refers \"%s\" module that does not contain extension definitions.",
                    prefixed_name, ext_mod->name);
            ret = LY_EVALID;
            goto cleanup;
        }
    }
    name = &prefixed_name[u];

    /* find the parsed extension definition there */
    LY_ARRAY_FOR(ext_mod->parsed->extensions, v) {
        if (!strcmp(name, ext_mod->parsed->extensions[v].name)) {
            /* compile extension definition and assign it */
            LY_CHECK_GOTO(ret = lys_compile_extension(ctx, ext_mod, &ext_mod->parsed->extensions[v], &ext->def), cleanup);
            break;
        }
    }
    if (!ext->def) {
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                "Extension definition of extension instance \"%s\" not found.", prefixed_name);
        ret = LY_EVALID;
        goto cleanup;
    }

    /* unify the parsed extension from YIN and YANG sources. Without extension definition, it is not possible
     * to get extension's argument from YIN source, so it is stored as one of the substatements. Here we have
     * to find it, mark it with LYS_YIN_ARGUMENT and store it in the compiled structure. */
    if (ext_p->yin && ext->def->argument && !ext->argument) {
        /* Schema was parsed from YIN and an argument is expected, ... */
        struct lysp_stmt *stmt = NULL;

        if (ext->def->flags & LYS_YINELEM_TRUE) {
            /* ... argument was the first XML child element */
            if (ext_p->child && !(ext_p->child->flags & LYS_YIN_ATTR)) {
                /* TODO check namespace of the statement */
                if (!strcmp(ext_p->child->stmt, ext->def->argument)) {
                    stmt = ext_p->child;
                }
            }
        } else {
            /* ... argument was one of the XML attributes which are represented as child stmt
             * with LYS_YIN_ATTR flag */
            for (stmt = ext_p->child; stmt && (stmt->flags & LYS_YIN_ATTR); stmt = stmt->next) {
                if (!strcmp(stmt->stmt, ext->def->argument)) {
                    /* this is the extension's argument */
                    break;
                }
            }
        }
        if (!stmt) {
            /* missing extension's argument */
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                    "Extension instance \"%s\" misses argument \"%s\".", prefixed_name, ext->def->argument);
            ret = LY_EVALID;
            goto cleanup;

        }
        LY_CHECK_GOTO(ret = lydict_insert(ctx->ctx, stmt->arg, 0, &ext->argument), cleanup);
        stmt->flags |= LYS_YIN_ARGUMENT;
    }
    if (prefixed_name != ext_p->name) {
        lydict_remove(ctx->ctx, ext_p->name);
        ext_p->name = prefixed_name;
        if (!ext_p->argument && ext->argument) {
            LY_CHECK_GOTO(ret = lydict_insert(ctx->ctx, ext->argument, 0, &ext_p->argument), cleanup);
        }
    }

    if (ext->def->plugin && ext->def->plugin->compile) {
        if (ext->argument) {
            lysc_update_path(ctx, (struct lysc_node *)ext, ext->argument);
        }
        LY_CHECK_GOTO(ret = ext->def->plugin->compile(ctx, ext_p, ext), cleanup);
        if (ext->argument) {
            lysc_update_path(ctx, NULL, NULL);
        }
    }
    ext_p->compiled = ext;

cleanup:
    if (prefixed_name && (prefixed_name != ext_p->name)) {
        lydict_remove(ctx->ctx, prefixed_name);
    }

    lysc_update_path(ctx, NULL, NULL);
    lysc_update_path(ctx, NULL, NULL);

    return ret;
}

/**
 * @brief Compile information from the if-feature statement
 * @param[in] ctx Compile context.
 * @param[in] qname The if-feature argument to process. It is pointer-to-qname just to unify the compile functions.
 * @param[in,out] iff Prepared (empty) compiled if-feature structure to fill.
 * @return LY_ERR value.
 */
static LY_ERR
lys_compile_iffeature(struct lysc_ctx *ctx, struct lysp_qname *qname, struct lysc_iffeature *iff)
{
    LY_ERR rc = LY_SUCCESS;
    const char *c = qname->str;
    int64_t i, j;
    int8_t op_len, last_not = 0, checkversion = 0;
    LY_ARRAY_COUNT_TYPE f_size = 0, expr_size = 0, f_exp = 1;
    uint8_t op;
    struct iff_stack stack = {0, 0, NULL};
    struct lysc_feature *f;

    assert(c);

    /* pre-parse the expression to get sizes for arrays, also do some syntax checks of the expression */
    for (i = j = 0; c[i]; i++) {
        if (c[i] == '(') {
            j++;
            checkversion = 1;
            continue;
        } else if (c[i] == ')') {
            j--;
            continue;
        } else if (isspace(c[i])) {
            checkversion = 1;
            continue;
        }

        if (!strncmp(&c[i], "not", op_len = 3) || !strncmp(&c[i], "and", op_len = 3) || !strncmp(&c[i], "or", op_len = 2)) {
            uint64_t spaces;
            for (spaces = 0; c[i + op_len + spaces] && isspace(c[i + op_len + spaces]); spaces++) {}
            if (c[i + op_len + spaces] == '\0') {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                        "Invalid value \"%s\" of if-feature - unexpected end of expression.", qname->str);
                return LY_EVALID;
            } else if (!isspace(c[i + op_len])) {
                /* feature name starting with the not/and/or */
                last_not = 0;
                f_size++;
            } else if (c[i] == 'n') { /* not operation */
                if (last_not) {
                    /* double not */
                    expr_size = expr_size - 2;
                    last_not = 0;
                } else {
                    last_not = 1;
                }
            } else { /* and, or */
                if (f_exp != f_size) {
                    LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                            "Invalid value \"%s\" of if-feature - missing feature/expression before \"%.*s\" operation.",
                            qname->str, op_len, &c[i]);
                    return LY_EVALID;
                }
                f_exp++;

                /* not a not operation */
                last_not = 0;
            }
            i += op_len;
        } else {
            f_size++;
            last_not = 0;
        }
        expr_size++;

        while (!isspace(c[i])) {
            if (!c[i] || (c[i] == ')') || (c[i] == '(')) {
                i--;
                break;
            }
            i++;
        }
    }
    if (j) {
        /* not matching count of ( and ) */
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                "Invalid value \"%s\" of if-feature - non-matching opening and closing parentheses.", qname->str);
        return LY_EVALID;
    }
    if (f_exp != f_size) {
        /* features do not match the needed arguments for the logical operations */
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                "Invalid value \"%s\" of if-feature - number of features in expression does not match "
                "the required number of operands for the operations.", qname->str);
        return LY_EVALID;
    }

    if (checkversion || (expr_size > 1)) {
        /* check that we have 1.1 module */
        if (qname->mod->version != LYS_VERSION_1_1) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                    "Invalid value \"%s\" of if-feature - YANG 1.1 expression in YANG 1.0 module.", qname->str);
            return LY_EVALID;
        }
    }

    /* allocate the memory */
    LY_ARRAY_CREATE_RET(ctx->ctx, iff->features, f_size, LY_EMEM);
    iff->expr = calloc((j = (expr_size / 4) + ((expr_size % 4) ? 1 : 0)), sizeof *iff->expr);
    stack.stack = malloc(expr_size * sizeof *stack.stack);
    LY_CHECK_ERR_GOTO(!stack.stack || !iff->expr, LOGMEM(ctx->ctx); rc = LY_EMEM, error);

    stack.size = expr_size;
    f_size--; expr_size--; /* used as indexes from now */

    for (i--; i >= 0; i--) {
        if (c[i] == ')') {
            /* push it on stack */
            iff_stack_push(&stack, LYS_IFF_RP);
            continue;
        } else if (c[i] == '(') {
            /* pop from the stack into result all operators until ) */
            while ((op = iff_stack_pop(&stack)) != LYS_IFF_RP) {
                iff_setop(iff->expr, op, expr_size--);
            }
            continue;
        } else if (isspace(c[i])) {
            continue;
        }

        /* end of operator or operand -> find beginning and get what is it */
        j = i + 1;
        while (i >= 0 && !isspace(c[i]) && c[i] != '(') {
            i--;
        }
        i++; /* go back by one step */

        if (!strncmp(&c[i], "not", 3) && isspace(c[i + 3])) {
            if (stack.index && (stack.stack[stack.index - 1] == LYS_IFF_NOT)) {
                /* double not */
                iff_stack_pop(&stack);
            } else {
                /* not has the highest priority, so do not pop from the stack
                 * as in case of AND and OR */
                iff_stack_push(&stack, LYS_IFF_NOT);
            }
        } else if (!strncmp(&c[i], "and", 3) && isspace(c[i + 3])) {
            /* as for OR - pop from the stack all operators with the same or higher
             * priority and store them to the result, then push the AND to the stack */
            while (stack.index && stack.stack[stack.index - 1] <= LYS_IFF_AND) {
                op = iff_stack_pop(&stack);
                iff_setop(iff->expr, op, expr_size--);
            }
            iff_stack_push(&stack, LYS_IFF_AND);
        } else if (!strncmp(&c[i], "or", 2) && isspace(c[i + 2])) {
            while (stack.index && stack.stack[stack.index - 1] <= LYS_IFF_OR) {
                op = iff_stack_pop(&stack);
                iff_setop(iff->expr, op, expr_size--);
            }
            iff_stack_push(&stack, LYS_IFF_OR);
        } else {
            /* feature name, length is j - i */

            /* add it to the expression */
            iff_setop(iff->expr, LYS_IFF_F, expr_size--);

            /* now get the link to the feature definition */
            f = lys_feature_find(qname->mod, &c[i], j - i);
            LY_CHECK_ERR_GOTO(!f, LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                    "Invalid value \"%s\" of if-feature - unable to find feature \"%.*s\".", qname->str, j - i, &c[i]);
                    rc = LY_EVALID, error)
            iff->features[f_size] = f;
            LY_ARRAY_INCREMENT(iff->features);
            f_size--;
        }
    }
    while (stack.index) {
        op = iff_stack_pop(&stack);
        iff_setop(iff->expr, op, expr_size--);
    }

    if (++expr_size || ++f_size) {
        /* not all expected operators and operands found */
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                "Invalid value \"%s\" of if-feature - processing error.", qname->str);
        rc = LY_EINT;
    } else {
        rc = LY_SUCCESS;
    }

error:
    /* cleanup */
    iff_stack_clean(&stack);

    return rc;
}

/**
 * @brief Get the XPath context node for the given schema node.
 * @param[in] start The schema node where the XPath expression appears.
 * @return The context node to evaluate XPath expression in given schema node.
 * @return NULL in case the context node is the root node.
 */
static struct lysc_node *
lysc_xpath_context(struct lysc_node *start)
{
    for (; start && !(start->nodetype & (LYS_CONTAINER | LYS_LEAF | LYS_LEAFLIST | LYS_LIST | LYS_ANYDATA | LYS_RPC | LYS_ACTION | LYS_NOTIF));
            start = start->parent) {}
    return start;
}

/**
 * @brief Compile information from the when statement
 *
 * @param[in] ctx Compile context.
 * @param[in] when_p Parsed when structure.
 * @param[in] flags Flags of the parsed node with the when statement.
 * @param[in] ctx_node Context node for the when statement.
 * @param[out] when Pointer where to store pointer to the created compiled when structure.
 * @return LY_ERR value.
 */
static LY_ERR
lys_compile_when_(struct lysc_ctx *ctx, struct lysp_when *when_p, uint16_t flags, const struct lysc_node *ctx_node,
        struct lysc_when **when)
{
    LY_ERR ret = LY_SUCCESS;

    *when = calloc(1, sizeof **when);
    LY_CHECK_ERR_RET(!(*when), LOGMEM(ctx->ctx), LY_EMEM);
    (*when)->refcount = 1;
    LY_CHECK_RET(lyxp_expr_parse(ctx->ctx, when_p->cond, 0, 1, &(*when)->cond));
    (*when)->module = ctx->mod_def;
    (*when)->context = (struct lysc_node *)ctx_node;
    DUP_STRING_GOTO(ctx->ctx, when_p->dsc, (*when)->dsc, ret, done);
    DUP_STRING_GOTO(ctx->ctx, when_p->ref, (*when)->ref, ret, done);
    COMPILE_EXTS_GOTO(ctx, when_p->exts, (*when)->exts, (*when), LYEXT_PAR_WHEN, ret, done);
    (*when)->flags = flags & LYS_STATUS_MASK;

done:
    return ret;
}

/**
 * @brief Compile information from the when statement by either standard compilation or by reusing
 * another compiled when structure.
 *
 * @param[in] ctx Compile context.
 * @param[in] when_p Parsed when structure.
 * @param[in] flags Flags of the parsed node with the when statement.
 * @param[in] ctx_node Context node for the when statement.
 * @param[in] node Compiled node to which add the compiled when.
 * @param[in,out] when_c Optional, pointer to the previously compiled @p when_p to be reused. Set to NULL
 * for the first call.
 * @return LY_ERR value.
 */
static LY_ERR
lys_compile_when(struct lysc_ctx *ctx, struct lysp_when *when_p, uint16_t flags, const struct lysc_node *ctx_node,
        struct lysc_node *node, struct lysc_when **when_c)
{
    struct lysc_when **new_when, ***node_when;

    assert(when_p);

    /* get the when array */
    if (node->nodetype & LYS_ACTION) {
        node_when = &((struct lysc_action *)node)->when;
    } else if (node->nodetype == LYS_NOTIF) {
        node_when = &((struct lysc_notif *)node)->when;
    } else {
        node_when = &node->when;
    }

    /* create new when pointer */
    LY_ARRAY_NEW_RET(ctx->ctx, *node_when, new_when, LY_EMEM);
    if (!when_c || !(*when_c)) {
        /* compile when */
        LY_CHECK_RET(lys_compile_when_(ctx, when_p, flags, ctx_node, new_when));

        if (!(ctx->options & LYS_COMPILE_GROUPING)) {
            /* do not check "when" semantics in a grouping */
            LY_CHECK_RET(ly_set_add(&ctx->xpath, node, 0, NULL));
        }

        /* remember the compiled when for sharing */
        if (when_c) {
            *when_c = *new_when;
        }
    } else {
        /* use the previously compiled when */
        ++(*when_c)->refcount;
        *new_when = *when_c;

        if (!(ctx->options & LYS_COMPILE_GROUPING)) {
            /* in this case check "when" again for all children because of dummy node check */
            LY_CHECK_RET(ly_set_add(&ctx->xpath, node, 0, NULL));
        }
    }

    return LY_SUCCESS;
}

/**
 * @brief Compile information from the must statement
 * @param[in] ctx Compile context.
 * @param[in] must_p The parsed must statement structure.
 * @param[in,out] must Prepared (empty) compiled must structure to fill.
 * @return LY_ERR value.
 */
static LY_ERR
lys_compile_must(struct lysc_ctx *ctx, struct lysp_restr *must_p, struct lysc_must *must)
{
    LY_ERR ret = LY_SUCCESS;

    LY_CHECK_RET(lyxp_expr_parse(ctx->ctx, must_p->arg.str, 0, 1, &must->cond));
    must->module = (struct lys_module *)must_p->arg.mod;
    DUP_STRING_GOTO(ctx->ctx, must_p->eapptag, must->eapptag, ret, done);
    DUP_STRING_GOTO(ctx->ctx, must_p->emsg, must->emsg, ret, done);
    DUP_STRING_GOTO(ctx->ctx, must_p->dsc, must->dsc, ret, done);
    DUP_STRING_GOTO(ctx->ctx, must_p->ref, must->ref, ret, done);
    COMPILE_EXTS_GOTO(ctx, must_p->exts, must->exts, must, LYEXT_PAR_MUST, ret, done);

done:
    return ret;
}

/**
 * @brief Compile information in the import statement - make sure there is the target module
 * @param[in] ctx Compile context.
 * @param[in] imp_p The parsed import statement structure to fill the module to.
 * @return LY_ERR value.
 */
static LY_ERR
lys_compile_import(struct lysc_ctx *ctx, struct lysp_import *imp_p)
{
    const struct lys_module *mod = NULL;
    LY_ERR ret = LY_SUCCESS;

    /* make sure that we have the parsed version (lysp_) of the imported module to import groupings or typedefs.
     * The compiled version is needed only for augments, deviates and leafrefs, so they are checked (and added,
     * if needed) when these nodes are finally being instantiated and validated at the end of schema compilation. */
    if (!imp_p->module->parsed) {
        /* try to use filepath if present */
        if (imp_p->module->filepath) {
            struct ly_in *in;
            if (ly_in_new_filepath(imp_p->module->filepath, 0, &in)) {
                LOGINT(ctx->ctx);
            } else {
                LY_CHECK_RET(lys_parse(ctx->ctx, in, !strcmp(&imp_p->module->filepath[strlen(imp_p->module->filepath - 4)],
                        ".yin") ? LYS_IN_YIN : LYS_IN_YANG, &mod));
                if (mod != imp_p->module) {
                    LOGERR(ctx->ctx, LY_EINT, "Filepath \"%s\" of the module \"%s\" does not match.",
                            imp_p->module->filepath, imp_p->module->name);
                    mod = NULL;
                }
            }
            ly_in_free(in, 1);
        }
        if (!mod) {
            if (lysp_load_module(ctx->ctx, imp_p->module->name, imp_p->module->revision, 0, 1, (struct lys_module **)&mod)) {
                LOGERR(ctx->ctx, LY_ENOTFOUND, "Unable to reload \"%s\" module to import it into \"%s\", source data not found.",
                        imp_p->module->name, ctx->mod->name);
                return LY_ENOTFOUND;
            }
        }
    }

    return ret;
}

LY_ERR
lys_identity_precompile(struct lysc_ctx *ctx_sc, struct ly_ctx *ctx, struct lys_module *module,
        struct lysp_ident *identities_p, struct lysc_ident **identities)
{
    LY_ARRAY_COUNT_TYPE offset = 0, u, v;
    struct lysc_ctx context = {0};
    LY_ERR ret = LY_SUCCESS;

    assert(ctx_sc || ctx);

    if (!ctx_sc) {
        context.ctx = ctx;
        context.mod = module;
        context.mod_def = module;
        context.path_len = 1;
        context.path[0] = '/';
        ctx_sc = &context;
    }

    if (!identities_p) {
        return LY_SUCCESS;
    }
    if (*identities) {
        offset = LY_ARRAY_COUNT(*identities);
    }

    lysc_update_path(ctx_sc, NULL, "{identity}");
    LY_ARRAY_CREATE_RET(ctx_sc->ctx, *identities, LY_ARRAY_COUNT(identities_p), LY_EMEM);
    LY_ARRAY_FOR(identities_p, u) {
        lysc_update_path(ctx_sc, NULL, identities_p[u].name);

        LY_ARRAY_INCREMENT(*identities);
        COMPILE_CHECK_UNIQUENESS_ARRAY(ctx_sc, *identities, name, &(*identities)[offset + u], "identity", identities_p[u].name);
        DUP_STRING_GOTO(ctx_sc->ctx, identities_p[u].name, (*identities)[offset + u].name, ret, done);
        DUP_STRING_GOTO(ctx_sc->ctx, identities_p[u].dsc, (*identities)[offset + u].dsc, ret, done);
        DUP_STRING_GOTO(ctx_sc->ctx, identities_p[u].ref, (*identities)[offset + u].ref, ret, done);
        (*identities)[offset + u].module = ctx_sc->mod;
        COMPILE_ARRAY_GOTO(ctx_sc, identities_p[u].iffeatures, (*identities)[offset + u].iffeatures, v,
                lys_compile_iffeature, ret, done);
        /* backlinks (derived) can be added no sooner than when all the identities in the current module are present */
        COMPILE_EXTS_GOTO(ctx_sc, identities_p[u].exts, (*identities)[offset + u].exts, &(*identities)[offset + u],
                LYEXT_PAR_IDENT, ret, done);
        (*identities)[offset + u].flags = identities_p[u].flags;

        lysc_update_path(ctx_sc, NULL, NULL);
    }
    lysc_update_path(ctx_sc, NULL, NULL);
done:
    return ret;
}

/**
 * @brief Check circular dependency of identities - identity MUST NOT reference itself (via their base statement).
 *
 * The function works in the same way as lys_compile_feature_circular_check() with different structures and error messages.
 *
 * @param[in] ctx Compile context for logging.
 * @param[in] ident The base identity (its derived list is being extended by the identity being currently processed).
 * @param[in] derived The list of derived identities of the identity being currently processed (not the one provided as @p ident)
 * @return LY_SUCCESS if everything is ok.
 * @return LY_EVALID if the identity is derived from itself.
 */
static LY_ERR
lys_compile_identity_circular_check(struct lysc_ctx *ctx, struct lysc_ident *ident, struct lysc_ident **derived)
{
    LY_ERR ret = LY_SUCCESS;
    LY_ARRAY_COUNT_TYPE u, v;
    struct ly_set recursion = {0};
    struct lysc_ident *drv;

    if (!derived) {
        return LY_SUCCESS;
    }

    for (u = 0; u < LY_ARRAY_COUNT(derived); ++u) {
        if (ident == derived[u]) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                    "Identity \"%s\" is indirectly derived from itself.", ident->name);
            ret = LY_EVALID;
            goto cleanup;
        }
        ret = ly_set_add(&recursion, derived[u], 0, NULL);
        LY_CHECK_GOTO(ret, cleanup);
    }

    for (v = 0; v < recursion.count; ++v) {
        drv = recursion.objs[v];
        if (!drv->derived) {
            continue;
        }
        for (u = 0; u < LY_ARRAY_COUNT(drv->derived); ++u) {
            if (ident == drv->derived[u]) {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                        "Identity \"%s\" is indirectly derived from itself.", ident->name);
                ret = LY_EVALID;
                goto cleanup;
            }
            ret = ly_set_add(&recursion, drv->derived[u], 0, NULL);
            LY_CHECK_GOTO(ret, cleanup);
        }
    }

cleanup:
    ly_set_erase(&recursion, NULL);
    return ret;
}

/**
 * @brief Find and process the referenced base identities from another identity or identityref
 *
 * For bases in identity set backlinks to them from the base identities. For identityref, store
 * the array of pointers to the base identities. So one of the ident or bases parameter must be set
 * to distinguish these two use cases.
 *
 * @param[in] ctx Compile context, not only for logging but also to get the current module to resolve prefixes.
 * @param[in] bases_p Array of names (including prefix if necessary) of base identities.
 * @param[in] ident Referencing identity to work with, NULL for identityref.
 * @param[in] bases Array of bases of identityref to fill in.
 * @return LY_ERR value.
 */
static LY_ERR
lys_compile_identity_bases(struct lysc_ctx *ctx, const struct lys_module *context_module, const char **bases_p,
        struct lysc_ident *ident, struct lysc_ident ***bases)
{
    LY_ARRAY_COUNT_TYPE u, v;
    const char *s, *name;
    const struct lys_module *mod;
    struct lysc_ident **idref;

    assert(ident || bases);

    if ((LY_ARRAY_COUNT(bases_p) > 1) && (ctx->mod_def->version < 2)) {
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                "Multiple bases in %s are allowed only in YANG 1.1 modules.", ident ? "identity" : "identityref type");
        return LY_EVALID;
    }

    LY_ARRAY_FOR(bases_p, u) {
        s = strchr(bases_p[u], ':');
        if (s) {
            /* prefixed identity */
            name = &s[1];
            mod = lys_module_find_prefix(context_module, bases_p[u], s - bases_p[u]);
        } else {
            name = bases_p[u];
            mod = context_module;
        }
        if (!mod) {
            if (ident) {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                        "Invalid prefix used for base (%s) of identity \"%s\".", bases_p[u], ident->name);
            } else {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                        "Invalid prefix used for base (%s) of identityref.", bases_p[u]);
            }
            return LY_EVALID;
        }

        idref = NULL;
        LY_ARRAY_FOR(mod->identities, v) {
            if (!strcmp(name, mod->identities[v].name)) {
                if (ident) {
                    if (ident == &mod->identities[v]) {
                        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                                "Identity \"%s\" is derived from itself.", ident->name);
                        return LY_EVALID;
                    }
                    LY_CHECK_RET(lys_compile_identity_circular_check(ctx, &mod->identities[v], ident->derived));
                    /* we have match! store the backlink */
                    LY_ARRAY_NEW_RET(ctx->ctx, mod->identities[v].derived, idref, LY_EMEM);
                    *idref = ident;
                } else {
                    /* we have match! store the found identity */
                    LY_ARRAY_NEW_RET(ctx->ctx, *bases, idref, LY_EMEM);
                    *idref = &mod->identities[v];
                }
                break;
            }
        }
        if (!idref || !(*idref)) {
            if (ident) {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                        "Unable to find base (%s) of identity \"%s\".", bases_p[u], ident->name);
            } else {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                        "Unable to find base (%s) of identityref.", bases_p[u]);
            }
            return LY_EVALID;
        }
    }
    return LY_SUCCESS;
}

/**
 * @brief For the given array of identities, set the backlinks from all their base identities.
 * @param[in] ctx Compile context, not only for logging but also to get the current module to resolve prefixes.
 * @param[in] idents_p Array of identities definitions from the parsed schema structure.
 * @param[in] idents Array of referencing identities to which the backlinks are supposed to be set.
 * @return LY_ERR value - LY_SUCCESS or LY_EVALID.
 */
static LY_ERR
lys_compile_identities_derived(struct lysc_ctx *ctx, struct lysp_ident *idents_p, struct lysc_ident *idents)
{
    LY_ARRAY_COUNT_TYPE u;

    lysc_update_path(ctx, NULL, "{identity}");
    for (u = 0; u < LY_ARRAY_COUNT(idents_p); ++u) {
        if (!idents_p[u].bases) {
            continue;
        }
        lysc_update_path(ctx, NULL, idents[u].name);
        LY_CHECK_RET(lys_compile_identity_bases(ctx, idents[u].module, idents_p[u].bases, &idents[u], NULL));
        lysc_update_path(ctx, NULL, NULL);
    }
    lysc_update_path(ctx, NULL, NULL);
    return LY_SUCCESS;
}

LY_ERR
lys_feature_precompile(struct lysc_ctx *ctx_sc, struct ly_ctx *ctx, struct lys_module *module,
        struct lysp_feature *features_p, struct lysc_feature **features)
{
    LY_ERR ret = LY_SUCCESS;
    LY_ARRAY_COUNT_TYPE offset = 0, u;
    struct lysc_ctx context = {0};

    assert(ctx_sc || ctx);

    if (!ctx_sc) {
        context.ctx = ctx;
        context.mod = module;
        context.path_len = 1;
        context.path[0] = '/';
        ctx_sc = &context;
    }

    if (!features_p) {
        return LY_SUCCESS;
    }
    if (*features) {
        offset = LY_ARRAY_COUNT(*features);
    }

    lysc_update_path(ctx_sc, NULL, "{feature}");
    LY_ARRAY_CREATE_RET(ctx_sc->ctx, *features, LY_ARRAY_COUNT(features_p), LY_EMEM);
    LY_ARRAY_FOR(features_p, u) {
        lysc_update_path(ctx_sc, NULL, features_p[u].name);

        LY_ARRAY_INCREMENT(*features);
        COMPILE_CHECK_UNIQUENESS_ARRAY(ctx_sc, *features, name, &(*features)[offset + u], "feature", features_p[u].name);
        DUP_STRING_GOTO(ctx_sc->ctx, features_p[u].name, (*features)[offset + u].name, ret, done);
        DUP_STRING_GOTO(ctx_sc->ctx, features_p[u].dsc, (*features)[offset + u].dsc, ret, done);
        DUP_STRING_GOTO(ctx_sc->ctx, features_p[u].ref, (*features)[offset + u].ref, ret, done);
        (*features)[offset + u].flags = features_p[u].flags;
        (*features)[offset + u].module = ctx_sc->mod;

        lysc_update_path(ctx_sc, NULL, NULL);
    }
    lysc_update_path(ctx_sc, NULL, NULL);

done:
    return ret;
}

/**
 * @brief Check circular dependency of features - feature MUST NOT reference itself (via their if-feature statement).
 *
 * The function works in the same way as lys_compile_identity_circular_check() with different structures and error messages.
 *
 * @param[in] ctx Compile context for logging.
 * @param[in] feature The feature referenced in if-feature statement (its depfeatures list is being extended by the feature
 *            being currently processed).
 * @param[in] depfeatures The list of depending features of the feature being currently processed (not the one provided as @p feature)
 * @return LY_SUCCESS if everything is ok.
 * @return LY_EVALID if the feature references indirectly itself.
 */
static LY_ERR
lys_compile_feature_circular_check(struct lysc_ctx *ctx, struct lysc_feature *feature, struct lysc_feature **depfeatures)
{
    LY_ERR ret = LY_SUCCESS;
    LY_ARRAY_COUNT_TYPE u, v;
    struct ly_set recursion = {0};
    struct lysc_feature *drv;

    if (!depfeatures) {
        return LY_SUCCESS;
    }

    for (u = 0; u < LY_ARRAY_COUNT(depfeatures); ++u) {
        if (feature == depfeatures[u]) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                    "Feature \"%s\" is indirectly referenced from itself.", feature->name);
            ret = LY_EVALID;
            goto cleanup;
        }
        ret = ly_set_add(&recursion, depfeatures[u], 0, NULL);
        LY_CHECK_GOTO(ret, cleanup);
    }

    for (v = 0; v < recursion.count; ++v) {
        drv = recursion.objs[v];
        if (!drv->depfeatures) {
            continue;
        }
        for (u = 0; u < LY_ARRAY_COUNT(drv->depfeatures); ++u) {
            if (feature == drv->depfeatures[u]) {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                        "Feature \"%s\" is indirectly referenced from itself.", feature->name);
                ret = LY_EVALID;
                goto cleanup;
            }
            ly_set_add(&recursion, drv->depfeatures[u], 0, NULL);
            LY_CHECK_GOTO(ret, cleanup);
        }
    }

cleanup:
    ly_set_erase(&recursion, NULL);
    return ret;
}

/**
 * @brief Create pre-compiled features array.
 *
 * See lys_feature_precompile() for more details.
 *
 * @param[in] ctx Compile context.
 * @param[in] feature_p Parsed feature definition to compile.
 * @param[in,out] features List of already (pre)compiled features to find the corresponding precompiled feature structure.
 * @return LY_ERR value.
 */
static LY_ERR
lys_feature_precompile_finish(struct lysc_ctx *ctx, struct lysp_feature *feature_p, struct lysc_feature *features)
{
    LY_ARRAY_COUNT_TYPE u, v, x;
    struct lysc_feature *feature, **df;
    LY_ERR ret = LY_SUCCESS;

    /* find the preprecompiled feature */
    LY_ARRAY_FOR(features, x) {
        if (strcmp(features[x].name, feature_p->name)) {
            continue;
        }
        feature = &features[x];
        lysc_update_path(ctx, NULL, "{feature}");
        lysc_update_path(ctx, NULL, feature_p->name);

        /* finish compilation started in lys_feature_precompile() */
        COMPILE_EXTS_GOTO(ctx, feature_p->exts, feature->exts, feature, LYEXT_PAR_FEATURE, ret, done);
        COMPILE_ARRAY_GOTO(ctx, feature_p->iffeatures, feature->iffeatures, u, lys_compile_iffeature, ret, done);
        if (feature->iffeatures) {
            for (u = 0; u < LY_ARRAY_COUNT(feature->iffeatures); ++u) {
                if (feature->iffeatures[u].features) {
                    for (v = 0; v < LY_ARRAY_COUNT(feature->iffeatures[u].features); ++v) {
                        /* check for circular dependency - direct reference first,... */
                        if (feature == feature->iffeatures[u].features[v]) {
                            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                                    "Feature \"%s\" is referenced from itself.", feature->name);
                            return LY_EVALID;
                        }
                        /* ... and indirect circular reference */
                        LY_CHECK_RET(lys_compile_feature_circular_check(ctx, feature->iffeatures[u].features[v], feature->depfeatures));

                        /* add itself into the dependants list */
                        LY_ARRAY_NEW_RET(ctx->ctx, feature->iffeatures[u].features[v]->depfeatures, df, LY_EMEM);
                        *df = feature;
                    }
                }
            }
        }
        lysc_update_path(ctx, NULL, NULL);
        lysc_update_path(ctx, NULL, NULL);
done:
        return ret;
    }

    LOGINT(ctx->ctx);
    return LY_EINT;
}

/**
 * @brief Revert compiled list of features back to the precompiled state.
 *
 * Function is needed in case the compilation failed and the schema is expected to revert back to the non-compiled status.
 *
 * @param[in] ctx Compilation context.
 * @param[in] mod The module structure with the features to decompile.
 */
static void
lys_feature_precompile_revert(struct lysc_ctx *ctx, struct lys_module *mod)
{
    LY_ARRAY_COUNT_TYPE u, v;

    /* in the dis_features list, remove all the parts (from finished compiling process)
     * which may points into the data being freed here */
    LY_ARRAY_FOR(mod->features, u) {
        LY_ARRAY_FOR(mod->features[u].iffeatures, v) {
            lysc_iffeature_free(ctx->ctx, &mod->features[u].iffeatures[v]);
        }
        LY_ARRAY_FREE(mod->features[u].iffeatures);
        mod->features[u].iffeatures = NULL;

        LY_ARRAY_FOR(mod->features[u].exts, v) {
            lysc_ext_instance_free(ctx->ctx, &(mod->features[u].exts)[v]);
        }
        LY_ARRAY_FREE(mod->features[u].exts);
        mod->features[u].exts = NULL;
    }
}

/**
 * @brief Validate and normalize numeric value from a range definition.
 * @param[in] ctx Compile context.
 * @param[in] basetype Base YANG built-in type of the node connected with the range restriction. Actually only LY_TYPE_DEC64 is important to
 * allow processing of the fractions. The fraction point is extracted from the value which is then normalize according to given frdigits into
 * valcopy to allow easy parsing and storing of the value. libyang stores decimal number without the decimal point which is always recovered from
 * the known fraction-digits value. So, with fraction-digits 2, number 3.14 is stored as 314 and number 1 is stored as 100.
 * @param[in] frdigits The fraction-digits of the type in case of LY_TYPE_DEC64.
 * @param[in] value String value of the range boundary.
 * @param[out] len Number of the processed bytes from the value. Processing stops on the first character which is not part of the number boundary.
 * @param[out] valcopy NULL-terminated string with the numeric value to parse and store.
 * @return LY_ERR value - LY_SUCCESS, LY_EMEM, LY_EVALID (no number) or LY_EINVAL (decimal64 not matching fraction-digits value).
 */
LY_ERR
range_part_check_value_syntax(struct lysc_ctx *ctx, LY_DATA_TYPE basetype, uint8_t frdigits, const char *value, size_t *len, char **valcopy)
{
    size_t fraction = 0, size;

    *len = 0;

    assert(value);
    /* parse value */
    if (!isdigit(value[*len]) && (value[*len] != '-') && (value[*len] != '+')) {
        return LY_EVALID;
    }

    if ((value[*len] == '-') || (value[*len] == '+')) {
        ++(*len);
    }

    while (isdigit(value[*len])) {
        ++(*len);
    }

    if ((basetype != LY_TYPE_DEC64) || (value[*len] != '.') || !isdigit(value[*len + 1])) {
        if (basetype == LY_TYPE_DEC64) {
            goto decimal;
        } else {
            *valcopy = strndup(value, *len);
            return LY_SUCCESS;
        }
    }
    fraction = *len;

    ++(*len);
    while (isdigit(value[*len])) {
        ++(*len);
    }

    if (basetype == LY_TYPE_DEC64) {
decimal:
        assert(frdigits);
        if (fraction && (*len - 1 - fraction > frdigits)) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                    "Range boundary \"%.*s\" of decimal64 type exceeds defined number (%u) of fraction digits.",
                    *len, value, frdigits);
            return LY_EINVAL;
        }
        if (fraction) {
            size = (*len) + (frdigits - ((*len) - 1 - fraction));
        } else {
            size = (*len) + frdigits + 1;
        }
        *valcopy = malloc(size * sizeof **valcopy);
        LY_CHECK_ERR_RET(!(*valcopy), LOGMEM(ctx->ctx), LY_EMEM);

        (*valcopy)[size - 1] = '\0';
        if (fraction) {
            memcpy(&(*valcopy)[0], &value[0], fraction);
            memcpy(&(*valcopy)[fraction], &value[fraction + 1], (*len) - 1 - (fraction));
            memset(&(*valcopy)[(*len) - 1], '0', frdigits - ((*len) - 1 - fraction));
        } else {
            memcpy(&(*valcopy)[0], &value[0], *len);
            memset(&(*valcopy)[*len], '0', frdigits);
        }
    }
    return LY_SUCCESS;
}

/**
 * @brief Check that values in range are in ascendant order.
 * @param[in] unsigned_value Flag to note that we are working with unsigned values.
 * @param[in] max Flag to distinguish if checking min or max value. min value must be strictly higher than previous,
 * max can be also equal.
 * @param[in] value Current value to check.
 * @param[in] prev_value The last seen value.
 * @return LY_SUCCESS or LY_EEXIST for invalid order.
 */
static LY_ERR
range_part_check_ascendancy(ly_bool unsigned_value, ly_bool max, int64_t value, int64_t prev_value)
{
    if (unsigned_value) {
        if ((max && ((uint64_t)prev_value > (uint64_t)value)) || (!max && ((uint64_t)prev_value >= (uint64_t)value))) {
            return LY_EEXIST;
        }
    } else {
        if ((max && (prev_value > value)) || (!max && (prev_value >= value))) {
            return LY_EEXIST;
        }
    }
    return LY_SUCCESS;
}

/**
 * @brief Set min/max value of the range part.
 * @param[in] ctx Compile context.
 * @param[in] part Range part structure to fill.
 * @param[in] max Flag to distinguish if storing min or max value.
 * @param[in] prev The last seen value to check that all values in range are specified in ascendant order.
 * @param[in] basetype Type of the value to get know implicit min/max values and other checking rules.
 * @param[in] first Flag for the first value of the range to avoid ascendancy order.
 * @param[in] length_restr Flag to distinguish between range and length restrictions. Only for logging.
 * @param[in] frdigits The fraction-digits value in case of LY_TYPE_DEC64 basetype.
 * @param[in] base_range Range from the type from which the current type is derived (if not built-in) to get type's min and max values.
 * @param[in,out] value Numeric range value to be stored, if not provided the type's min/max value is set.
 * @return LY_ERR value - LY_SUCCESS, LY_EDENIED (value brokes type's boundaries), LY_EVALID (not a number),
 * LY_EEXIST (value is smaller than the previous one), LY_EINVAL (decimal64 value does not corresponds with the
 * frdigits value), LY_EMEM.
 */
static LY_ERR
range_part_minmax(struct lysc_ctx *ctx, struct lysc_range_part *part, ly_bool max, int64_t prev, LY_DATA_TYPE basetype,
        ly_bool first, ly_bool length_restr, uint8_t frdigits, struct lysc_range *base_range, const char **value)
{
    LY_ERR ret = LY_SUCCESS;
    char *valcopy = NULL;
    size_t len;

    if (value) {
        ret = range_part_check_value_syntax(ctx, basetype, frdigits, *value, &len, &valcopy);
        LY_CHECK_GOTO(ret, finalize);
    }
    if (!valcopy && base_range) {
        if (max) {
            part->max_64 = base_range->parts[LY_ARRAY_COUNT(base_range->parts) - 1].max_64;
        } else {
            part->min_64 = base_range->parts[0].min_64;
        }
        if (!first) {
            ret = range_part_check_ascendancy(basetype <= LY_TYPE_STRING ? 1 : 0, max, max ? part->max_64 : part->min_64, prev);
        }
        goto finalize;
    }

    switch (basetype) {
    case LY_TYPE_INT8: /* range */
        if (valcopy) {
            ret = ly_parse_int(valcopy, strlen(valcopy), INT64_C(-128), INT64_C(127), 10, max ? &part->max_64 : &part->min_64);
        } else if (max) {
            part->max_64 = INT64_C(127);
        } else {
            part->min_64 = INT64_C(-128);
        }
        if (!ret && !first) {
            ret = range_part_check_ascendancy(0, max, max ? part->max_64 : part->min_64, prev);
        }
        break;
    case LY_TYPE_INT16: /* range */
        if (valcopy) {
            ret = ly_parse_int(valcopy, strlen(valcopy), INT64_C(-32768), INT64_C(32767), 10, max ? &part->max_64 : &part->min_64);
        } else if (max) {
            part->max_64 = INT64_C(32767);
        } else {
            part->min_64 = INT64_C(-32768);
        }
        if (!ret && !first) {
            ret = range_part_check_ascendancy(0, max, max ? part->max_64 : part->min_64, prev);
        }
        break;
    case LY_TYPE_INT32: /* range */
        if (valcopy) {
            ret = ly_parse_int(valcopy, strlen(valcopy), INT64_C(-2147483648), INT64_C(2147483647), 10, max ? &part->max_64 : &part->min_64);
        } else if (max) {
            part->max_64 = INT64_C(2147483647);
        } else {
            part->min_64 = INT64_C(-2147483648);
        }
        if (!ret && !first) {
            ret = range_part_check_ascendancy(0, max, max ? part->max_64 : part->min_64, prev);
        }
        break;
    case LY_TYPE_INT64: /* range */
    case LY_TYPE_DEC64: /* range */
        if (valcopy) {
            ret = ly_parse_int(valcopy, strlen(valcopy), INT64_C(-9223372036854775807) - INT64_C(1), INT64_C(9223372036854775807), 10,
                    max ? &part->max_64 : &part->min_64);
        } else if (max) {
            part->max_64 = INT64_C(9223372036854775807);
        } else {
            part->min_64 = INT64_C(-9223372036854775807) - INT64_C(1);
        }
        if (!ret && !first) {
            ret = range_part_check_ascendancy(0, max, max ? part->max_64 : part->min_64, prev);
        }
        break;
    case LY_TYPE_UINT8: /* range */
        if (valcopy) {
            ret = ly_parse_uint(valcopy, strlen(valcopy), UINT64_C(255), 10, max ? &part->max_u64 : &part->min_u64);
        } else if (max) {
            part->max_u64 = UINT64_C(255);
        } else {
            part->min_u64 = UINT64_C(0);
        }
        if (!ret && !first) {
            ret = range_part_check_ascendancy(1, max, max ? part->max_64 : part->min_64, prev);
        }
        break;
    case LY_TYPE_UINT16: /* range */
        if (valcopy) {
            ret = ly_parse_uint(valcopy, strlen(valcopy), UINT64_C(65535), 10, max ? &part->max_u64 : &part->min_u64);
        } else if (max) {
            part->max_u64 = UINT64_C(65535);
        } else {
            part->min_u64 = UINT64_C(0);
        }
        if (!ret && !first) {
            ret = range_part_check_ascendancy(1, max, max ? part->max_64 : part->min_64, prev);
        }
        break;
    case LY_TYPE_UINT32: /* range */
        if (valcopy) {
            ret = ly_parse_uint(valcopy, strlen(valcopy), UINT64_C(4294967295), 10, max ? &part->max_u64 : &part->min_u64);
        } else if (max) {
            part->max_u64 = UINT64_C(4294967295);
        } else {
            part->min_u64 = UINT64_C(0);
        }
        if (!ret && !first) {
            ret = range_part_check_ascendancy(1, max, max ? part->max_64 : part->min_64, prev);
        }
        break;
    case LY_TYPE_UINT64: /* range */
    case LY_TYPE_STRING: /* length */
    case LY_TYPE_BINARY: /* length */
        if (valcopy) {
            ret = ly_parse_uint(valcopy, strlen(valcopy), UINT64_C(18446744073709551615), 10, max ? &part->max_u64 : &part->min_u64);
        } else if (max) {
            part->max_u64 = UINT64_C(18446744073709551615);
        } else {
            part->min_u64 = UINT64_C(0);
        }
        if (!ret && !first) {
            ret = range_part_check_ascendancy(1, max, max ? part->max_64 : part->min_64, prev);
        }
        break;
    default:
        LOGINT(ctx->ctx);
        ret = LY_EINT;
    }

finalize:
    if (ret == LY_EDENIED) {
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                "Invalid %s restriction - value \"%s\" does not fit the type limitations.",
                length_restr ? "length" : "range", valcopy ? valcopy : *value);
    } else if (ret == LY_EVALID) {
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                "Invalid %s restriction - invalid value \"%s\".",
                length_restr ? "length" : "range", valcopy ? valcopy : *value);
    } else if (ret == LY_EEXIST) {
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                "Invalid %s restriction - values are not in ascending order (%s).",
                length_restr ? "length" : "range",
                (valcopy && basetype != LY_TYPE_DEC64) ? valcopy : value ? *value : max ? "max" : "min");
    } else if (!ret && value) {
        *value = *value + len;
    }
    free(valcopy);
    return ret;
}

/**
 * @brief Compile the parsed range restriction.
 * @param[in] ctx Compile context.
 * @param[in] range_p Parsed range structure to compile.
 * @param[in] basetype Base YANG built-in type of the node with the range restriction.
 * @param[in] length_restr Flag to distinguish between range and length restrictions. Only for logging.
 * @param[in] frdigits The fraction-digits value in case of LY_TYPE_DEC64 basetype.
 * @param[in] base_range Range restriction of the type from which the current type is derived. The current
 * range restriction must be more restrictive than the base_range.
 * @param[in,out] range Pointer to the created current range structure.
 * @return LY_ERR value.
 */
static LY_ERR
lys_compile_type_range(struct lysc_ctx *ctx, struct lysp_restr *range_p, LY_DATA_TYPE basetype, ly_bool length_restr,
        uint8_t frdigits, struct lysc_range *base_range, struct lysc_range **range)
{
    LY_ERR ret = LY_EVALID;
    const char *expr;
    struct lysc_range_part *parts = NULL, *part;
    ly_bool range_expected = 0, uns;
    LY_ARRAY_COUNT_TYPE parts_done = 0, u, v;

    assert(range);
    assert(range_p);

    expr = range_p->arg.str;
    while (1) {
        if (isspace(*expr)) {
            ++expr;
        } else if (*expr == '\0') {
            if (range_expected) {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                        "Invalid %s restriction - unexpected end of the expression after \"..\" (%s).",
                        length_restr ? "length" : "range", range_p->arg);
                goto cleanup;
            } else if (!parts || (parts_done == LY_ARRAY_COUNT(parts))) {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                        "Invalid %s restriction - unexpected end of the expression (%s).",
                        length_restr ? "length" : "range", range_p->arg);
                goto cleanup;
            }
            parts_done++;
            break;
        } else if (!strncmp(expr, "min", 3)) {
            if (parts) {
                /* min cannot be used elsewhere than in the first part */
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                        "Invalid %s restriction - unexpected data before min keyword (%.*s).", length_restr ? "length" : "range",
                        expr - range_p->arg.str, range_p->arg.str);
                goto cleanup;
            }
            expr += 3;

            LY_ARRAY_NEW_GOTO(ctx->ctx, parts, part, ret, cleanup);
            LY_CHECK_GOTO(range_part_minmax(ctx, part, 0, 0, basetype, 1, length_restr, frdigits, base_range, NULL), cleanup);
            part->max_64 = part->min_64;
        } else if (*expr == '|') {
            if (!parts || range_expected) {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                        "Invalid %s restriction - unexpected beginning of the expression (%s).", length_restr ? "length" : "range", expr);
                goto cleanup;
            }
            expr++;
            parts_done++;
            /* process next part of the expression */
        } else if (!strncmp(expr, "..", 2)) {
            expr += 2;
            while (isspace(*expr)) {
                expr++;
            }
            if (!parts || (LY_ARRAY_COUNT(parts) == parts_done)) {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                        "Invalid %s restriction - unexpected \"..\" without a lower bound.", length_restr ? "length" : "range");
                goto cleanup;
            }
            /* continue expecting the upper boundary */
            range_expected = 1;
        } else if (isdigit(*expr) || (*expr == '-') || (*expr == '+')) {
            /* number */
            if (range_expected) {
                part = &parts[LY_ARRAY_COUNT(parts) - 1];
                LY_CHECK_GOTO(range_part_minmax(ctx, part, 1, part->min_64, basetype, 0, length_restr, frdigits, NULL, &expr), cleanup);
                range_expected = 0;
            } else {
                LY_ARRAY_NEW_GOTO(ctx->ctx, parts, part, ret, cleanup);
                LY_CHECK_GOTO(range_part_minmax(ctx, part, 0, parts_done ? parts[LY_ARRAY_COUNT(parts) - 2].max_64 : 0,
                        basetype, parts_done ? 0 : 1, length_restr, frdigits, NULL, &expr), cleanup);
                part->max_64 = part->min_64;
            }

            /* continue with possible another expression part */
        } else if (!strncmp(expr, "max", 3)) {
            expr += 3;
            while (isspace(*expr)) {
                expr++;
            }
            if (*expr != '\0') {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG, "Invalid %s restriction - unexpected data after max keyword (%s).",
                        length_restr ? "length" : "range", expr);
                goto cleanup;
            }
            if (range_expected) {
                part = &parts[LY_ARRAY_COUNT(parts) - 1];
                LY_CHECK_GOTO(range_part_minmax(ctx, part, 1, part->min_64, basetype, 0, length_restr, frdigits, base_range, NULL), cleanup);
                range_expected = 0;
            } else {
                LY_ARRAY_NEW_GOTO(ctx->ctx, parts, part, ret, cleanup);
                LY_CHECK_GOTO(range_part_minmax(ctx, part, 1, parts_done ? parts[LY_ARRAY_COUNT(parts) - 2].max_64 : 0,
                        basetype, parts_done ? 0 : 1, length_restr, frdigits, base_range, NULL), cleanup);
                part->min_64 = part->max_64;
            }
        } else {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG, "Invalid %s restriction - unexpected data (%s).",
                    length_restr ? "length" : "range", expr);
            goto cleanup;
        }
    }

    /* check with the previous range/length restriction */
    if (base_range) {
        switch (basetype) {
        case LY_TYPE_BINARY:
        case LY_TYPE_UINT8:
        case LY_TYPE_UINT16:
        case LY_TYPE_UINT32:
        case LY_TYPE_UINT64:
        case LY_TYPE_STRING:
            uns = 1;
            break;
        case LY_TYPE_DEC64:
        case LY_TYPE_INT8:
        case LY_TYPE_INT16:
        case LY_TYPE_INT32:
        case LY_TYPE_INT64:
            uns = 0;
            break;
        default:
            LOGINT(ctx->ctx);
            ret = LY_EINT;
            goto cleanup;
        }
        for (u = v = 0; u < parts_done && v < LY_ARRAY_COUNT(base_range->parts); ++u) {
            if ((uns && (parts[u].min_u64 < base_range->parts[v].min_u64)) || (!uns && (parts[u].min_64 < base_range->parts[v].min_64))) {
                goto baseerror;
            }
            /* current lower bound is not lower than the base */
            if (base_range->parts[v].min_64 == base_range->parts[v].max_64) {
                /* base has single value */
                if (base_range->parts[v].min_64 == parts[u].min_64) {
                    /* both lower bounds are the same */
                    if (parts[u].min_64 != parts[u].max_64) {
                        /* current continues with a range */
                        goto baseerror;
                    } else {
                        /* equal single values, move both forward */
                        ++v;
                        continue;
                    }
                } else {
                    /* base is single value lower than current range, so the
                     * value from base range is removed in the current,
                     * move only base and repeat checking */
                    ++v;
                    --u;
                    continue;
                }
            } else {
                /* base is the range */
                if (parts[u].min_64 == parts[u].max_64) {
                    /* current is a single value */
                    if ((uns && (parts[u].max_u64 > base_range->parts[v].max_u64)) || (!uns && (parts[u].max_64 > base_range->parts[v].max_64))) {
                        /* current is behind the base range, so base range is omitted,
                         * move the base and keep the current for further check */
                        ++v;
                        --u;
                    } /* else it is within the base range, so move the current, but keep the base */
                    continue;
                } else {
                    /* both are ranges - check the higher bound, the lower was already checked */
                    if ((uns && (parts[u].max_u64 > base_range->parts[v].max_u64)) || (!uns && (parts[u].max_64 > base_range->parts[v].max_64))) {
                        /* higher bound is higher than the current higher bound */
                        if ((uns && (parts[u].min_u64 > base_range->parts[v].max_u64)) || (!uns && (parts[u].min_64 > base_range->parts[v].max_64))) {
                            /* but the current lower bound is also higher, so the base range is omitted,
                             * continue with the same current, but move the base */
                            --u;
                            ++v;
                            continue;
                        }
                        /* current range starts within the base range but end behind it */
                        goto baseerror;
                    } else {
                        /* current range is smaller than the base,
                         * move current, but stay with the base */
                        continue;
                    }
                }
            }
        }
        if (u != parts_done) {
baseerror:
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                    "Invalid %s restriction - the derived restriction (%s) is not equally or more limiting.",
                    length_restr ? "length" : "range", range_p->arg);
            goto cleanup;
        }
    }

    if (!(*range)) {
        *range = calloc(1, sizeof **range);
        LY_CHECK_ERR_RET(!(*range), LOGMEM(ctx->ctx), LY_EMEM);
    }

    /* we rewrite the following values as the types chain is being processed */
    if (range_p->eapptag) {
        lydict_remove(ctx->ctx, (*range)->eapptag);
        LY_CHECK_GOTO(ret = lydict_insert(ctx->ctx, range_p->eapptag, 0, &(*range)->eapptag), cleanup);
    }
    if (range_p->emsg) {
        lydict_remove(ctx->ctx, (*range)->emsg);
        LY_CHECK_GOTO(ret = lydict_insert(ctx->ctx, range_p->emsg, 0, &(*range)->emsg), cleanup);
    }
    if (range_p->dsc) {
        lydict_remove(ctx->ctx, (*range)->dsc);
        LY_CHECK_GOTO(ret = lydict_insert(ctx->ctx, range_p->dsc, 0, &(*range)->dsc), cleanup);
    }
    if (range_p->ref) {
        lydict_remove(ctx->ctx, (*range)->ref);
        LY_CHECK_GOTO(ret = lydict_insert(ctx->ctx, range_p->ref, 0, &(*range)->ref), cleanup);
    }
    /* extensions are taken only from the last range by the caller */

    (*range)->parts = parts;
    parts = NULL;
    ret = LY_SUCCESS;
cleanup:
    LY_ARRAY_FREE(parts);

    return ret;
}

/**
 * @brief Checks pattern syntax.
 *
 * @param[in] ctx Context.
 * @param[in] log_path Path for logging errors.
 * @param[in] pattern Pattern to check.
 * @param[in,out] pcre2_code Compiled PCRE2 pattern. If NULL, the compiled information used to validate pattern are freed.
 * @return LY_ERR value - LY_SUCCESS, LY_EMEM, LY_EVALID.
 */
LY_ERR
lys_compile_type_pattern_check(struct ly_ctx *ctx, const char *log_path, const char *pattern, pcre2_code **code)
{
    size_t idx, idx2, start, end, size, brack;
    char *perl_regex, *ptr;
    int err_code;
    const char *orig_ptr;
    PCRE2_SIZE err_offset;
    pcre2_code *code_local;

#define URANGE_LEN 19
    char *ublock2urange[][2] = {
        {"BasicLatin", "[\\x{0000}-\\x{007F}]"},
        {"Latin-1Supplement", "[\\x{0080}-\\x{00FF}]"},
        {"LatinExtended-A", "[\\x{0100}-\\x{017F}]"},
        {"LatinExtended-B", "[\\x{0180}-\\x{024F}]"},
        {"IPAExtensions", "[\\x{0250}-\\x{02AF}]"},
        {"SpacingModifierLetters", "[\\x{02B0}-\\x{02FF}]"},
        {"CombiningDiacriticalMarks", "[\\x{0300}-\\x{036F}]"},
        {"Greek", "[\\x{0370}-\\x{03FF}]"},
        {"Cyrillic", "[\\x{0400}-\\x{04FF}]"},
        {"Armenian", "[\\x{0530}-\\x{058F}]"},
        {"Hebrew", "[\\x{0590}-\\x{05FF}]"},
        {"Arabic", "[\\x{0600}-\\x{06FF}]"},
        {"Syriac", "[\\x{0700}-\\x{074F}]"},
        {"Thaana", "[\\x{0780}-\\x{07BF}]"},
        {"Devanagari", "[\\x{0900}-\\x{097F}]"},
        {"Bengali", "[\\x{0980}-\\x{09FF}]"},
        {"Gurmukhi", "[\\x{0A00}-\\x{0A7F}]"},
        {"Gujarati", "[\\x{0A80}-\\x{0AFF}]"},
        {"Oriya", "[\\x{0B00}-\\x{0B7F}]"},
        {"Tamil", "[\\x{0B80}-\\x{0BFF}]"},
        {"Telugu", "[\\x{0C00}-\\x{0C7F}]"},
        {"Kannada", "[\\x{0C80}-\\x{0CFF}]"},
        {"Malayalam", "[\\x{0D00}-\\x{0D7F}]"},
        {"Sinhala", "[\\x{0D80}-\\x{0DFF}]"},
        {"Thai", "[\\x{0E00}-\\x{0E7F}]"},
        {"Lao", "[\\x{0E80}-\\x{0EFF}]"},
        {"Tibetan", "[\\x{0F00}-\\x{0FFF}]"},
        {"Myanmar", "[\\x{1000}-\\x{109F}]"},
        {"Georgian", "[\\x{10A0}-\\x{10FF}]"},
        {"HangulJamo", "[\\x{1100}-\\x{11FF}]"},
        {"Ethiopic", "[\\x{1200}-\\x{137F}]"},
        {"Cherokee", "[\\x{13A0}-\\x{13FF}]"},
        {"UnifiedCanadianAboriginalSyllabics", "[\\x{1400}-\\x{167F}]"},
        {"Ogham", "[\\x{1680}-\\x{169F}]"},
        {"Runic", "[\\x{16A0}-\\x{16FF}]"},
        {"Khmer", "[\\x{1780}-\\x{17FF}]"},
        {"Mongolian", "[\\x{1800}-\\x{18AF}]"},
        {"LatinExtendedAdditional", "[\\x{1E00}-\\x{1EFF}]"},
        {"GreekExtended", "[\\x{1F00}-\\x{1FFF}]"},
        {"GeneralPunctuation", "[\\x{2000}-\\x{206F}]"},
        {"SuperscriptsandSubscripts", "[\\x{2070}-\\x{209F}]"},
        {"CurrencySymbols", "[\\x{20A0}-\\x{20CF}]"},
        {"CombiningMarksforSymbols", "[\\x{20D0}-\\x{20FF}]"},
        {"LetterlikeSymbols", "[\\x{2100}-\\x{214F}]"},
        {"NumberForms", "[\\x{2150}-\\x{218F}]"},
        {"Arrows", "[\\x{2190}-\\x{21FF}]"},
        {"MathematicalOperators", "[\\x{2200}-\\x{22FF}]"},
        {"MiscellaneousTechnical", "[\\x{2300}-\\x{23FF}]"},
        {"ControlPictures", "[\\x{2400}-\\x{243F}]"},
        {"OpticalCharacterRecognition", "[\\x{2440}-\\x{245F}]"},
        {"EnclosedAlphanumerics", "[\\x{2460}-\\x{24FF}]"},
        {"BoxDrawing", "[\\x{2500}-\\x{257F}]"},
        {"BlockElements", "[\\x{2580}-\\x{259F}]"},
        {"GeometricShapes", "[\\x{25A0}-\\x{25FF}]"},
        {"MiscellaneousSymbols", "[\\x{2600}-\\x{26FF}]"},
        {"Dingbats", "[\\x{2700}-\\x{27BF}]"},
        {"BraillePatterns", "[\\x{2800}-\\x{28FF}]"},
        {"CJKRadicalsSupplement", "[\\x{2E80}-\\x{2EFF}]"},
        {"KangxiRadicals", "[\\x{2F00}-\\x{2FDF}]"},
        {"IdeographicDescriptionCharacters", "[\\x{2FF0}-\\x{2FFF}]"},
        {"CJKSymbolsandPunctuation", "[\\x{3000}-\\x{303F}]"},
        {"Hiragana", "[\\x{3040}-\\x{309F}]"},
        {"Katakana", "[\\x{30A0}-\\x{30FF}]"},
        {"Bopomofo", "[\\x{3100}-\\x{312F}]"},
        {"HangulCompatibilityJamo", "[\\x{3130}-\\x{318F}]"},
        {"Kanbun", "[\\x{3190}-\\x{319F}]"},
        {"BopomofoExtended", "[\\x{31A0}-\\x{31BF}]"},
        {"EnclosedCJKLettersandMonths", "[\\x{3200}-\\x{32FF}]"},
        {"CJKCompatibility", "[\\x{3300}-\\x{33FF}]"},
        {"CJKUnifiedIdeographsExtensionA", "[\\x{3400}-\\x{4DB5}]"},
        {"CJKUnifiedIdeographs", "[\\x{4E00}-\\x{9FFF}]"},
        {"YiSyllables", "[\\x{A000}-\\x{A48F}]"},
        {"YiRadicals", "[\\x{A490}-\\x{A4CF}]"},
        {"HangulSyllables", "[\\x{AC00}-\\x{D7A3}]"},
        {"PrivateUse", "[\\x{E000}-\\x{F8FF}]"},
        {"CJKCompatibilityIdeographs", "[\\x{F900}-\\x{FAFF}]"},
        {"AlphabeticPresentationForms", "[\\x{FB00}-\\x{FB4F}]"},
        {"ArabicPresentationForms-A", "[\\x{FB50}-\\x{FDFF}]"},
        {"CombiningHalfMarks", "[\\x{FE20}-\\x{FE2F}]"},
        {"CJKCompatibilityForms", "[\\x{FE30}-\\x{FE4F}]"},
        {"SmallFormVariants", "[\\x{FE50}-\\x{FE6F}]"},
        {"ArabicPresentationForms-B", "[\\x{FE70}-\\x{FEFE}]"},
        {"HalfwidthandFullwidthForms", "[\\x{FF00}-\\x{FFEF}]"},
        {NULL, NULL}
    };

    /* adjust the expression to a Perl equivalent
     * http://www.w3.org/TR/2004/REC-xmlschema-2-20041028/#regexs */

    /* allocate space for the transformed pattern */
    size = strlen(pattern) + 1;
    perl_regex = malloc(size);
    LY_CHECK_ERR_RET(!perl_regex, LOGMEM(ctx), LY_EMEM);
    perl_regex[0] = '\0';

    /* we need to replace all "$" and "^" (that are not in "[]") with "\$" and "\^" */
    brack = 0;
    idx = 0;
    orig_ptr = pattern;
    while (orig_ptr[0]) {
        switch (orig_ptr[0]) {
        case '$':
        case '^':
            if (!brack) {
                /* make space for the extra character */
                ++size;
                perl_regex = ly_realloc(perl_regex, size);
                LY_CHECK_ERR_RET(!perl_regex, LOGMEM(ctx), LY_EMEM);

                /* print escape slash */
                perl_regex[idx] = '\\';
                ++idx;
            }
            break;
        case '[':
            /* must not be escaped */
            if ((orig_ptr == pattern) || (orig_ptr[-1] != '\\')) {
                ++brack;
            }
            break;
        case ']':
            if ((orig_ptr == pattern) || (orig_ptr[-1] != '\\')) {
                /* pattern was checked and compiled already */
                assert(brack);
                --brack;
            }
            break;
        default:
            break;
        }

        /* copy char */
        perl_regex[idx] = orig_ptr[0];

        ++idx;
        ++orig_ptr;
    }
    perl_regex[idx] = '\0';

    /* substitute Unicode Character Blocks with exact Character Ranges */
    while ((ptr = strstr(perl_regex, "\\p{Is"))) {
        start = ptr - perl_regex;

        ptr = strchr(ptr, '}');
        if (!ptr) {
            LOGVAL(ctx, LY_VLOG_STR, log_path, LY_VCODE_INREGEXP,
                    pattern, perl_regex + start + 2, "unterminated character property");
            free(perl_regex);
            return LY_EVALID;
        }
        end = (ptr - perl_regex) + 1;

        /* need more space */
        if (end - start < URANGE_LEN) {
            perl_regex = ly_realloc(perl_regex, strlen(perl_regex) + (URANGE_LEN - (end - start)) + 1);
            LY_CHECK_ERR_RET(!perl_regex, LOGMEM(ctx); free(perl_regex), LY_EMEM);
        }

        /* find our range */
        for (idx = 0; ublock2urange[idx][0]; ++idx) {
            if (!strncmp(perl_regex + start + 5, ublock2urange[idx][0], strlen(ublock2urange[idx][0]))) {
                break;
            }
        }
        if (!ublock2urange[idx][0]) {
            LOGVAL(ctx, LY_VLOG_STR, log_path, LY_VCODE_INREGEXP,
                    pattern, perl_regex + start + 5, "unknown block name");
            free(perl_regex);
            return LY_EVALID;
        }

        /* make the space in the string and replace the block (but we cannot include brackets if it was already enclosed in them) */
        for (idx2 = 0, idx = 0; idx2 < start; ++idx2) {
            if ((perl_regex[idx2] == '[') && (!idx2 || (perl_regex[idx2 - 1] != '\\'))) {
                ++idx;
            }
            if ((perl_regex[idx2] == ']') && (!idx2 || (perl_regex[idx2 - 1] != '\\'))) {
                --idx;
            }
        }
        if (idx) {
            /* skip brackets */
            memmove(perl_regex + start + (URANGE_LEN - 2), perl_regex + end, strlen(perl_regex + end) + 1);
            memcpy(perl_regex + start, ublock2urange[idx][1] + 1, URANGE_LEN - 2);
        } else {
            memmove(perl_regex + start + URANGE_LEN, perl_regex + end, strlen(perl_regex + end) + 1);
            memcpy(perl_regex + start, ublock2urange[idx][1], URANGE_LEN);
        }
    }

    /* must return 0, already checked during parsing */
    code_local = pcre2_compile((PCRE2_SPTR)perl_regex, PCRE2_ZERO_TERMINATED,
            PCRE2_UTF | PCRE2_ANCHORED | PCRE2_ENDANCHORED | PCRE2_DOLLAR_ENDONLY | PCRE2_NO_AUTO_CAPTURE,
            &err_code, &err_offset, NULL);
    if (!code_local) {
        PCRE2_UCHAR err_msg[256] = {0};
        pcre2_get_error_message(err_code, err_msg, 256);
        LOGVAL(ctx, LY_VLOG_STR, log_path, LY_VCODE_INREGEXP, pattern, perl_regex + err_offset, err_msg);
        free(perl_regex);
        return LY_EVALID;
    }
    free(perl_regex);

    if (code) {
        *code = code_local;
    } else {
        free(code_local);
    }

    return LY_SUCCESS;

#undef URANGE_LEN
}

/**
 * @brief Compile parsed pattern restriction in conjunction with the patterns from base type.
 * @param[in] ctx Compile context.
 * @param[in] patterns_p Array of parsed patterns from the current type to compile.
 * @param[in] base_patterns Compiled patterns from the type from which the current type is derived.
 * Patterns from the base type are inherited to have all the patterns that have to match at one place.
 * @param[out] patterns Pointer to the storage for the patterns of the current type.
 * @return LY_ERR LY_SUCCESS, LY_EMEM, LY_EVALID.
 */
static LY_ERR
lys_compile_type_patterns(struct lysc_ctx *ctx, struct lysp_restr *patterns_p,
        struct lysc_pattern **base_patterns, struct lysc_pattern ***patterns)
{
    struct lysc_pattern **pattern;
    LY_ARRAY_COUNT_TYPE u;
    LY_ERR ret = LY_SUCCESS;

    /* first, copy the patterns from the base type */
    if (base_patterns) {
        *patterns = lysc_patterns_dup(ctx->ctx, base_patterns);
        LY_CHECK_ERR_RET(!(*patterns), LOGMEM(ctx->ctx), LY_EMEM);
    }

    LY_ARRAY_FOR(patterns_p, u) {
        LY_ARRAY_NEW_RET(ctx->ctx, (*patterns), pattern, LY_EMEM);
        *pattern = calloc(1, sizeof **pattern);
        ++(*pattern)->refcount;

        ret = lys_compile_type_pattern_check(ctx->ctx, ctx->path, &patterns_p[u].arg.str[1], &(*pattern)->code);
        LY_CHECK_RET(ret);

        if (patterns_p[u].arg.str[0] == 0x15) {
            (*pattern)->inverted = 1;
        }
        DUP_STRING_GOTO(ctx->ctx, &patterns_p[u].arg.str[1], (*pattern)->expr, ret, done);
        DUP_STRING_GOTO(ctx->ctx, patterns_p[u].eapptag, (*pattern)->eapptag, ret, done);
        DUP_STRING_GOTO(ctx->ctx, patterns_p[u].emsg, (*pattern)->emsg, ret, done);
        DUP_STRING_GOTO(ctx->ctx, patterns_p[u].dsc, (*pattern)->dsc, ret, done);
        DUP_STRING_GOTO(ctx->ctx, patterns_p[u].ref, (*pattern)->ref, ret, done);
        COMPILE_EXTS_GOTO(ctx, patterns_p[u].exts, (*pattern)->exts, (*pattern), LYEXT_PAR_PATTERN, ret, done);
    }
done:
    return ret;
}

/**
 * @brief map of the possible restrictions combination for the specific built-in type.
 */
static uint16_t type_substmt_map[LY_DATA_TYPE_COUNT] = {
    0 /* LY_TYPE_UNKNOWN */,
    LYS_SET_LENGTH /* LY_TYPE_BINARY */,
    LYS_SET_RANGE /* LY_TYPE_UINT8 */,
    LYS_SET_RANGE /* LY_TYPE_UINT16 */,
    LYS_SET_RANGE /* LY_TYPE_UINT32 */,
    LYS_SET_RANGE /* LY_TYPE_UINT64 */,
    LYS_SET_LENGTH | LYS_SET_PATTERN /* LY_TYPE_STRING */,
    LYS_SET_BIT /* LY_TYPE_BITS */,
    0 /* LY_TYPE_BOOL */,
    LYS_SET_FRDIGITS | LYS_SET_RANGE /* LY_TYPE_DEC64 */,
    0 /* LY_TYPE_EMPTY */,
    LYS_SET_ENUM /* LY_TYPE_ENUM */,
    LYS_SET_BASE /* LY_TYPE_IDENT */,
    LYS_SET_REQINST /* LY_TYPE_INST */,
    LYS_SET_REQINST | LYS_SET_PATH /* LY_TYPE_LEAFREF */,
    LYS_SET_TYPE /* LY_TYPE_UNION */,
    LYS_SET_RANGE /* LY_TYPE_INT8 */,
    LYS_SET_RANGE /* LY_TYPE_INT16 */,
    LYS_SET_RANGE /* LY_TYPE_INT32 */,
    LYS_SET_RANGE /* LY_TYPE_INT64 */
};

/**
 * @brief stringification of the YANG built-in data types
 */
const char *ly_data_type2str[LY_DATA_TYPE_COUNT] = {
    "unknown", "binary", "8bit unsigned integer", "16bit unsigned integer",
    "32bit unsigned integer", "64bit unsigned integer", "string", "bits", "boolean", "decimal64", "empty", "enumeration",
    "identityref", "instance-identifier", "leafref", "union", "8bit integer", "16bit integer", "32bit integer", "64bit integer"
};

/**
 * @brief Compile parsed type's enum structures (for enumeration and bits types).
 * @param[in] ctx Compile context.
 * @param[in] enums_p Array of the parsed enum structures to compile.
 * @param[in] basetype Base YANG built-in type from which the current type is derived. Only LY_TYPE_ENUM and LY_TYPE_BITS are expected.
 * @param[in] base_enums Array of the compiled enums information from the (latest) base type to check if the current enums are compatible.
 * @param[out] enums Newly created array of the compiled enums information for the current type.
 * @return LY_ERR value - LY_SUCCESS or LY_EVALID.
 */
static LY_ERR
lys_compile_type_enums(struct lysc_ctx *ctx, struct lysp_type_enum *enums_p, LY_DATA_TYPE basetype,
        struct lysc_type_bitenum_item *base_enums, struct lysc_type_bitenum_item **enums)
{
    LY_ERR ret = LY_SUCCESS;
    LY_ARRAY_COUNT_TYPE u, v, match = 0;
    int32_t value = 0;
    uint32_t position = 0;
    struct lysc_type_bitenum_item *e, storage;

    if (base_enums && (ctx->mod_def->version < 2)) {
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG, "%s type can be subtyped only in YANG 1.1 modules.",
                basetype == LY_TYPE_ENUM ? "Enumeration" : "Bits");
        return LY_EVALID;
    }

    LY_ARRAY_FOR(enums_p, u) {
        LY_ARRAY_NEW_RET(ctx->ctx, *enums, e, LY_EMEM);
        DUP_STRING_GOTO(ctx->ctx, enums_p[u].name, e->name, ret, done);
        DUP_STRING_GOTO(ctx->ctx, enums_p[u].ref, e->dsc, ret, done);
        DUP_STRING_GOTO(ctx->ctx, enums_p[u].ref, e->ref, ret, done);
        e->flags = enums_p[u].flags & LYS_FLAGS_COMPILED_MASK;
        if (base_enums) {
            /* check the enum/bit presence in the base type - the set of enums/bits in the derived type must be a subset */
            LY_ARRAY_FOR(base_enums, v) {
                if (!strcmp(e->name, base_enums[v].name)) {
                    break;
                }
            }
            if (v == LY_ARRAY_COUNT(base_enums)) {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                        "Invalid %s - derived type adds new item \"%s\".",
                        basetype == LY_TYPE_ENUM ? "enumeration" : "bits", e->name);
                return LY_EVALID;
            }
            match = v;
        }

        if (basetype == LY_TYPE_ENUM) {
            e->flags |= LYS_ISENUM;
            if (enums_p[u].flags & LYS_SET_VALUE) {
                e->value = (int32_t)enums_p[u].value;
                if (!u || (e->value >= value)) {
                    value = e->value + 1;
                }
                /* check collision with other values */
                for (v = 0; v < LY_ARRAY_COUNT(*enums) - 1; ++v) {
                    if (e->value == (*enums)[v].value) {
                        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                                "Invalid enumeration - value %d collide in items \"%s\" and \"%s\".",
                                e->value, e->name, (*enums)[v].name);
                        return LY_EVALID;
                    }
                }
            } else if (base_enums) {
                /* inherit the assigned value */
                e->value = base_enums[match].value;
                if (!u || (e->value >= value)) {
                    value = e->value + 1;
                }
            } else {
                /* assign value automatically */
                if (u && (value == INT32_MIN)) {
                    /* counter overflow */
                    LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                            "Invalid enumeration - it is not possible to auto-assign enum value for "
                            "\"%s\" since the highest value is already 2147483647.", e->name);
                    return LY_EVALID;
                }
                e->value = value++;
            }
        } else { /* LY_TYPE_BITS */
            if (enums_p[u].flags & LYS_SET_VALUE) {
                e->value = (int32_t)enums_p[u].value;
                if (!u || ((uint32_t)e->value >= position)) {
                    position = (uint32_t)e->value + 1;
                }
                /* check collision with other values */
                for (v = 0; v < LY_ARRAY_COUNT(*enums) - 1; ++v) {
                    if (e->value == (*enums)[v].value) {
                        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                                "Invalid bits - position %u collide in items \"%s\" and \"%s\".",
                                (uint32_t)e->value, e->name, (*enums)[v].name);
                        return LY_EVALID;
                    }
                }
            } else if (base_enums) {
                /* inherit the assigned value */
                e->value = base_enums[match].value;
                if (!u || ((uint32_t)e->value >= position)) {
                    position = (uint32_t)e->value + 1;
                }
            } else {
                /* assign value automatically */
                if (u && (position == 0)) {
                    /* counter overflow */
                    LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                            "Invalid bits - it is not possible to auto-assign bit position for "
                            "\"%s\" since the highest value is already 4294967295.", e->name);
                    return LY_EVALID;
                }
                e->value = position++;
            }
        }

        if (base_enums) {
            /* the assigned values must not change from the derived type */
            if (e->value != base_enums[match].value) {
                if (basetype == LY_TYPE_ENUM) {
                    LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                            "Invalid enumeration - value of the item \"%s\" has changed from %d to %d in the derived type.",
                            e->name, base_enums[match].value, e->value);
                } else {
                    LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                            "Invalid bits - position of the item \"%s\" has changed from %u to %u in the derived type.",
                            e->name, (uint32_t)base_enums[match].value, (uint32_t)e->value);
                }
                return LY_EVALID;
            }
        }

        COMPILE_ARRAY_GOTO(ctx, enums_p[u].iffeatures, e->iffeatures, v, lys_compile_iffeature, ret, done);
        COMPILE_EXTS_GOTO(ctx, enums_p[u].exts, e->exts, e, basetype == LY_TYPE_ENUM ? LYEXT_PAR_TYPE_ENUM : LYEXT_PAR_TYPE_BIT, ret, done);

        if (basetype == LY_TYPE_BITS) {
            /* keep bits ordered by position */
            for (v = u; v && (*enums)[v - 1].value > e->value; --v) {}
            if (v != u) {
                memcpy(&storage, e, sizeof *e);
                memmove(&(*enums)[v + 1], &(*enums)[v], (u - v) * sizeof **enums);
                memcpy(&(*enums)[v], &storage, sizeof storage);
            }
        }
    }

done:
    return ret;
}

/**
 * @brief Parse path-arg (leafref). Get tokens of the path by repetitive calls of the function.
 *
 * path-arg            = absolute-path / relative-path
 * absolute-path       = 1*("/" (node-identifier *path-predicate))
 * relative-path       = 1*(".." "/") descendant-path
 *
 * @param[in,out] path Path to parse.
 * @param[out] prefix Prefix of the token, NULL if there is not any.
 * @param[out] pref_len Length of the prefix, 0 if there is not any.
 * @param[out] name Name of the token.
 * @param[out] nam_len Length of the name.
 * @param[out] parent_times Number of leading ".." in the path. Must be 0 on the first call,
 *                          must not be changed between consecutive calls. -1 if the
 *                          path is absolute.
 * @param[out] has_predicate Flag to mark whether there is a predicate specified.
 * @return LY_ERR value: LY_SUCCESS or LY_EINVAL in case of invalid character in the path.
 */
LY_ERR
lys_path_token(const char **path, const char **prefix, size_t *prefix_len, const char **name, size_t *name_len,
        int32_t *parent_times, ly_bool *has_predicate)
{
    int32_t par_times = 0;

    assert(path && *path);
    assert(parent_times);
    assert(prefix);
    assert(prefix_len);
    assert(name);
    assert(name_len);
    assert(has_predicate);

    *prefix = NULL;
    *prefix_len = 0;
    *name = NULL;
    *name_len = 0;
    *has_predicate = 0;

    if (!*parent_times) {
        if (!strncmp(*path, "..", 2)) {
            *path += 2;
            ++par_times;
            while (!strncmp(*path, "/..", 3)) {
                *path += 3;
                ++par_times;
            }
        }
        if (par_times) {
            *parent_times = par_times;
        } else {
            *parent_times = -1;
        }
    }

    if (**path != '/') {
        return LY_EINVAL;
    }
    /* skip '/' */
    ++(*path);

    /* node-identifier ([prefix:]name) */
    LY_CHECK_RET(ly_parse_nodeid(path, prefix, prefix_len, name, name_len));

    if (((**path == '/') && (*path)[1]) || !**path) {
        /* path continues by another token or this is the last token */
        return LY_SUCCESS;
    } else if ((*path)[0] != '[') {
        /* unexpected character */
        return LY_EINVAL;
    } else {
        /* predicate starting with [ */
        *has_predicate = 1;
        return LY_SUCCESS;
    }
}

/**
 * @brief Check the features used in if-feature statements applicable to the leafref and its target.
 *
 * The set of features used for target must be a subset of features used for the leafref.
 * This is not a perfect, we should compare the truth tables but it could require too much resources
 * and RFC 7950 does not require it explicitely, so we simplify that.
 *
 * @param[in] refnode The leafref node.
 * @param[in] target Tha target node of the leafref.
 * @return LY_SUCCESS or LY_EVALID;
 */
static LY_ERR
lys_compile_leafref_features_validate(const struct lysc_node *refnode, const struct lysc_node *target)
{
    LY_ERR ret = LY_EVALID;
    const struct lysc_node *iter;
    LY_ARRAY_COUNT_TYPE u, v;
    struct ly_set features = {0};

    for (iter = refnode; iter; iter = iter->parent) {
        if (iter->iffeatures) {
            LY_ARRAY_FOR(iter->iffeatures, u) {
                LY_ARRAY_FOR(iter->iffeatures[u].features, v) {
                    LY_CHECK_GOTO(ly_set_add(&features, iter->iffeatures[u].features[v], 0, NULL), cleanup);
                }
            }
        }
    }

    /* we should have, in features set, a superset of features applicable to the target node.
     * If the feature is not present, we don;t have a subset of features applicable
     * to the leafref itself. */
    for (iter = target; iter; iter = iter->parent) {
        if (iter->iffeatures) {
            LY_ARRAY_FOR(iter->iffeatures, u) {
                LY_ARRAY_FOR(iter->iffeatures[u].features, v) {
                    if (!ly_set_contains(&features, iter->iffeatures[u].features[v], NULL)) {
                        /* feature not present */
                        goto cleanup;
                    }
                }
            }
        }
    }
    ret = LY_SUCCESS;

cleanup:
    ly_set_erase(&features, NULL);
    return ret;
}

static LY_ERR lys_compile_type(struct lysc_ctx *ctx, struct lysp_node *context_pnode, uint16_t context_flags,
        struct lysp_module *context_mod, const char *context_name, struct lysp_type *type_p,
        struct lysc_type **type, const char **units, struct lysp_qname **dflt);

/**
 * @brief The core of the lys_compile_type() - compile information about the given type (from typedef or leaf/leaf-list).
 * @param[in] ctx Compile context.
 * @param[in] context_pnode Schema node where the type/typedef is placed to correctly find the base types.
 * @param[in] context_flags Flags of the context node or the referencing typedef to correctly check status of referencing and referenced objects.
 * @param[in] context_mod Module of the context node or the referencing typedef to correctly check status of referencing and referenced objects.
 * @param[in] context_name Name of the context node or referencing typedef for logging.
 * @param[in] type_p Parsed type to compile.
 * @param[in] basetype Base YANG built-in type of the type to compile.
 * @param[in] tpdfname Name of the type's typedef, serves as a flag - if it is leaf/leaf-list's type, it is NULL.
 * @param[in] base The latest base (compiled) type from which the current type is being derived.
 * @param[out] type Newly created type structure with the filled information about the type.
 * @return LY_ERR value.
 */
static LY_ERR
lys_compile_type_(struct lysc_ctx *ctx, struct lysp_node *context_pnode, uint16_t context_flags,
        struct lysp_module *context_mod, const char *context_name, struct lysp_type *type_p, LY_DATA_TYPE basetype,
        const char *tpdfname, struct lysc_type *base, struct lysc_type **type)
{
    LY_ERR ret = LY_SUCCESS;
    struct lysc_type_bin *bin;
    struct lysc_type_num *num;
    struct lysc_type_str *str;
    struct lysc_type_bits *bits;
    struct lysc_type_enum *enumeration;
    struct lysc_type_dec *dec;
    struct lysc_type_identityref *idref;
    struct lysc_type_leafref *lref;
    struct lysc_type_union *un, *un_aux;

    switch (basetype) {
    case LY_TYPE_BINARY:
        bin = (struct lysc_type_bin *)(*type);

        /* RFC 7950 9.8.1, 9.4.4 - length, number of octets it contains */
        if (type_p->length) {
            LY_CHECK_RET(lys_compile_type_range(ctx, type_p->length, basetype, 1, 0,
                    base ? ((struct lysc_type_bin *)base)->length : NULL, &bin->length));
            if (!tpdfname) {
                COMPILE_EXTS_GOTO(ctx, type_p->length->exts, bin->length->exts, bin->length, LYEXT_PAR_LENGTH, ret, cleanup);
            }
        }
        break;
    case LY_TYPE_BITS:
        /* RFC 7950 9.7 - bits */
        bits = (struct lysc_type_bits *)(*type);
        if (type_p->bits) {
            LY_CHECK_RET(lys_compile_type_enums(ctx, type_p->bits, basetype,
                    base ? (struct lysc_type_bitenum_item *)((struct lysc_type_bits *)base)->bits : NULL,
                    (struct lysc_type_bitenum_item **)&bits->bits));
        }

        if (!base && !type_p->flags) {
            /* type derived from bits built-in type must contain at least one bit */
            if (tpdfname) {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LY_VCODE_MISSCHILDSTMT, "bit", "bits type ", tpdfname);
            } else {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LY_VCODE_MISSCHILDSTMT, "bit", "bits type", "");
            }
            return LY_EVALID;
        }
        break;
    case LY_TYPE_DEC64:
        dec = (struct lysc_type_dec *)(*type);

        /* RFC 7950 9.3.4 - fraction-digits */
        if (!base) {
            if (!type_p->fraction_digits) {
                if (tpdfname) {
                    LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LY_VCODE_MISSCHILDSTMT, "fraction-digits", "decimal64 type ", tpdfname);
                } else {
                    LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LY_VCODE_MISSCHILDSTMT, "fraction-digits", "decimal64 type", "");
                }
                return LY_EVALID;
            }
            dec->fraction_digits = type_p->fraction_digits;
        } else {
            if (type_p->fraction_digits) {
                /* fraction digits is prohibited in types not directly derived from built-in decimal64 */
                if (tpdfname) {
                    LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                            "Invalid fraction-digits substatement for type \"%s\" not directly derived from decimal64 built-in type.",
                            tpdfname);
                } else {
                    LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                            "Invalid fraction-digits substatement for type not directly derived from decimal64 built-in type.");
                }
                return LY_EVALID;
            }
            dec->fraction_digits = ((struct lysc_type_dec *)base)->fraction_digits;
        }

        /* RFC 7950 9.2.4 - range */
        if (type_p->range) {
            LY_CHECK_RET(lys_compile_type_range(ctx, type_p->range, basetype, 0, dec->fraction_digits,
                    base ? ((struct lysc_type_dec *)base)->range : NULL, &dec->range));
            if (!tpdfname) {
                COMPILE_EXTS_GOTO(ctx, type_p->range->exts, dec->range->exts, dec->range, LYEXT_PAR_RANGE, ret, cleanup);
            }
        }
        break;
    case LY_TYPE_STRING:
        str = (struct lysc_type_str *)(*type);

        /* RFC 7950 9.4.4 - length */
        if (type_p->length) {
            LY_CHECK_RET(lys_compile_type_range(ctx, type_p->length, basetype, 1, 0,
                    base ? ((struct lysc_type_str *)base)->length : NULL, &str->length));
            if (!tpdfname) {
                COMPILE_EXTS_GOTO(ctx, type_p->length->exts, str->length->exts, str->length, LYEXT_PAR_LENGTH, ret, cleanup);
            }
        } else if (base && ((struct lysc_type_str *)base)->length) {
            str->length = lysc_range_dup(ctx->ctx, ((struct lysc_type_str *)base)->length);
        }

        /* RFC 7950 9.4.5 - pattern */
        if (type_p->patterns) {
            LY_CHECK_RET(lys_compile_type_patterns(ctx, type_p->patterns,
                    base ? ((struct lysc_type_str *)base)->patterns : NULL, &str->patterns));
        } else if (base && ((struct lysc_type_str *)base)->patterns) {
            str->patterns = lysc_patterns_dup(ctx->ctx, ((struct lysc_type_str *)base)->patterns);
        }
        break;
    case LY_TYPE_ENUM:
        enumeration = (struct lysc_type_enum *)(*type);

        /* RFC 7950 9.6 - enum */
        if (type_p->enums) {
            LY_CHECK_RET(lys_compile_type_enums(ctx, type_p->enums, basetype,
                    base ? ((struct lysc_type_enum *)base)->enums : NULL, &enumeration->enums));
        }

        if (!base && !type_p->flags) {
            /* type derived from enumerations built-in type must contain at least one enum */
            if (tpdfname) {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LY_VCODE_MISSCHILDSTMT, "enum", "enumeration type ", tpdfname);
            } else {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LY_VCODE_MISSCHILDSTMT, "enum", "enumeration type", "");
            }
            return LY_EVALID;
        }
        break;
    case LY_TYPE_INT8:
    case LY_TYPE_UINT8:
    case LY_TYPE_INT16:
    case LY_TYPE_UINT16:
    case LY_TYPE_INT32:
    case LY_TYPE_UINT32:
    case LY_TYPE_INT64:
    case LY_TYPE_UINT64:
        num = (struct lysc_type_num *)(*type);

        /* RFC 6020 9.2.4 - range */
        if (type_p->range) {
            LY_CHECK_RET(lys_compile_type_range(ctx, type_p->range, basetype, 0, 0,
                    base ? ((struct lysc_type_num *)base)->range : NULL, &num->range));
            if (!tpdfname) {
                COMPILE_EXTS_GOTO(ctx, type_p->range->exts, num->range->exts, num->range, LYEXT_PAR_RANGE, ret, cleanup);
            }
        }
        break;
    case LY_TYPE_IDENT:
        idref = (struct lysc_type_identityref *)(*type);

        /* RFC 7950 9.10.2 - base */
        if (type_p->bases) {
            if (base) {
                /* only the directly derived identityrefs can contain base specification */
                if (tpdfname) {
                    LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                            "Invalid base substatement for the type \"%s\" not directly derived from identityref built-in type.",
                            tpdfname);
                } else {
                    LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                            "Invalid base substatement for the type not directly derived from identityref built-in type.");
                }
                return LY_EVALID;
            }
            LY_CHECK_RET(lys_compile_identity_bases(ctx, type_p->mod, type_p->bases, NULL, &idref->bases));
        }

        if (!base && !type_p->flags) {
            /* type derived from identityref built-in type must contain at least one base */
            if (tpdfname) {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LY_VCODE_MISSCHILDSTMT, "base", "identityref type ", tpdfname);
            } else {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LY_VCODE_MISSCHILDSTMT, "base", "identityref type", "");
            }
            return LY_EVALID;
        }
        break;
    case LY_TYPE_LEAFREF:
        lref = (struct lysc_type_leafref *)*type;

        /* RFC 7950 9.9.3 - require-instance */
        if (type_p->flags & LYS_SET_REQINST) {
            if (context_mod->mod->version < LYS_VERSION_1_1) {
                if (tpdfname) {
                    LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SEMANTICS,
                            "Leafref type \"%s\" can be restricted by require-instance statement only in YANG 1.1 modules.", tpdfname);
                } else {
                    LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SEMANTICS,
                            "Leafref type can be restricted by require-instance statement only in YANG 1.1 modules.");
                }
                return LY_EVALID;
            }
            lref->require_instance = type_p->require_instance;
        } else if (base) {
            /* inherit */
            lref->require_instance = ((struct lysc_type_leafref *)base)->require_instance;
        } else {
            /* default is true */
            lref->require_instance = 1;
        }
        if (type_p->path) {
            LY_CHECK_RET(lyxp_expr_dup(ctx->ctx, type_p->path, &lref->path));
            lref->path_mod = type_p->mod;
        } else if (base) {
            LY_CHECK_RET(lyxp_expr_dup(ctx->ctx, ((struct lysc_type_leafref *)base)->path, &lref->path));
            lref->path_mod = ((struct lysc_type_leafref *)base)->path_mod;
        } else if (tpdfname) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LY_VCODE_MISSCHILDSTMT, "path", "leafref type ", tpdfname);
            return LY_EVALID;
        } else {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LY_VCODE_MISSCHILDSTMT, "path", "leafref type", "");
            return LY_EVALID;
        }
        break;
    case LY_TYPE_INST:
        /* RFC 7950 9.9.3 - require-instance */
        if (type_p->flags & LYS_SET_REQINST) {
            ((struct lysc_type_instanceid *)(*type))->require_instance = type_p->require_instance;
        } else {
            /* default is true */
            ((struct lysc_type_instanceid *)(*type))->require_instance = 1;
        }
        break;
    case LY_TYPE_UNION:
        un = (struct lysc_type_union *)(*type);

        /* RFC 7950 7.4 - type */
        if (type_p->types) {
            if (base) {
                /* only the directly derived union can contain types specification */
                if (tpdfname) {
                    LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                            "Invalid type substatement for the type \"%s\" not directly derived from union built-in type.",
                            tpdfname);
                } else {
                    LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG,
                            "Invalid type substatement for the type not directly derived from union built-in type.");
                }
                return LY_EVALID;
            }
            /* compile the type */
            LY_ARRAY_CREATE_RET(ctx->ctx, un->types, LY_ARRAY_COUNT(type_p->types), LY_EVALID);
            for (LY_ARRAY_COUNT_TYPE u = 0, additional = 0; u < LY_ARRAY_COUNT(type_p->types); ++u) {
                LY_CHECK_RET(lys_compile_type(ctx, context_pnode, context_flags, context_mod, context_name,
                        &type_p->types[u], &un->types[u + additional], NULL, NULL));
                if (un->types[u + additional]->basetype == LY_TYPE_UNION) {
                    /* add space for additional types from the union subtype */
                    un_aux = (struct lysc_type_union *)un->types[u + additional];
                    LY_ARRAY_RESIZE_ERR_RET(ctx->ctx, un->types, (*((uint64_t *)(type_p->types) - 1)) + additional + LY_ARRAY_COUNT(un_aux->types) - 1,
                            lysc_type_free(ctx->ctx, (struct lysc_type *)un_aux), LY_EMEM);

                    /* copy subtypes of the subtype union */
                    for (LY_ARRAY_COUNT_TYPE v = 0; v < LY_ARRAY_COUNT(un_aux->types); ++v) {
                        if (un_aux->types[v]->basetype == LY_TYPE_LEAFREF) {
                            /* duplicate the whole structure because of the instance-specific path resolving for realtype */
                            un->types[u + additional] = calloc(1, sizeof(struct lysc_type_leafref));
                            LY_CHECK_ERR_RET(!un->types[u + additional], LOGMEM(ctx->ctx); lysc_type_free(ctx->ctx, (struct lysc_type *)un_aux), LY_EMEM);
                            lref = (struct lysc_type_leafref *)un->types[u + additional];

                            lref->basetype = LY_TYPE_LEAFREF;
                            LY_CHECK_RET(lyxp_expr_dup(ctx->ctx, ((struct lysc_type_leafref *)un_aux->types[v])->path, &lref->path));
                            lref->refcount = 1;
                            lref->require_instance = ((struct lysc_type_leafref *)un_aux->types[v])->require_instance;
                            lref->path_mod = ((struct lysc_type_leafref *)un_aux->types[v])->path_mod;
                            /* TODO extensions */

                        } else {
                            un->types[u + additional] = un_aux->types[v];
                            ++un_aux->types[v]->refcount;
                        }
                        ++additional;
                        LY_ARRAY_INCREMENT(un->types);
                    }
                    /* compensate u increment in main loop */
                    --additional;

                    /* free the replaced union subtype */
                    lysc_type_free(ctx->ctx, (struct lysc_type *)un_aux);
                } else {
                    LY_ARRAY_INCREMENT(un->types);
                }
            }
        }

        if (!base && !type_p->flags) {
            /* type derived from union built-in type must contain at least one type */
            if (tpdfname) {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LY_VCODE_MISSCHILDSTMT, "type", "union type ", tpdfname);
            } else {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LY_VCODE_MISSCHILDSTMT, "type", "union type", "");
            }
            return LY_EVALID;
        }
        break;
    case LY_TYPE_BOOL:
    case LY_TYPE_EMPTY:
    case LY_TYPE_UNKNOWN: /* just to complete switch */
        break;
    }

    if (tpdfname) {
        switch (basetype) {
        case LY_TYPE_BINARY:
            type_p->compiled = *type;
            *type = calloc(1, sizeof(struct lysc_type_bin));
            break;
        case LY_TYPE_BITS:
            type_p->compiled = *type;
            *type = calloc(1, sizeof(struct lysc_type_bits));
            break;
        case LY_TYPE_DEC64:
            type_p->compiled = *type;
            *type = calloc(1, sizeof(struct lysc_type_dec));
            break;
        case LY_TYPE_STRING:
            type_p->compiled = *type;
            *type = calloc(1, sizeof(struct lysc_type_str));
            break;
        case LY_TYPE_ENUM:
            type_p->compiled = *type;
            *type = calloc(1, sizeof(struct lysc_type_enum));
            break;
        case LY_TYPE_INT8:
        case LY_TYPE_UINT8:
        case LY_TYPE_INT16:
        case LY_TYPE_UINT16:
        case LY_TYPE_INT32:
        case LY_TYPE_UINT32:
        case LY_TYPE_INT64:
        case LY_TYPE_UINT64:
            type_p->compiled = *type;
            *type = calloc(1, sizeof(struct lysc_type_num));
            break;
        case LY_TYPE_IDENT:
            type_p->compiled = *type;
            *type = calloc(1, sizeof(struct lysc_type_identityref));
            break;
        case LY_TYPE_LEAFREF:
            type_p->compiled = *type;
            *type = calloc(1, sizeof(struct lysc_type_leafref));
            break;
        case LY_TYPE_INST:
            type_p->compiled = *type;
            *type = calloc(1, sizeof(struct lysc_type_instanceid));
            break;
        case LY_TYPE_UNION:
            type_p->compiled = *type;
            *type = calloc(1, sizeof(struct lysc_type_union));
            break;
        case LY_TYPE_BOOL:
        case LY_TYPE_EMPTY:
        case LY_TYPE_UNKNOWN: /* just to complete switch */
            break;
        }
    }
    LY_CHECK_ERR_RET(!(*type), LOGMEM(ctx->ctx), LY_EMEM);

cleanup:
    return ret;
}

/**
 * @brief Compile information about the leaf/leaf-list's type.
 * @param[in] ctx Compile context.
 * @param[in] context_pnode Schema node where the type/typedef is placed to correctly find the base types.
 * @param[in] context_flags Flags of the context node or the referencing typedef to correctly check status of referencing and referenced objects.
 * @param[in] context_mod Module of the context node or the referencing typedef to correctly check status of referencing and referenced objects.
 * @param[in] context_name Name of the context node or referencing typedef for logging.
 * @param[in] type_p Parsed type to compile.
 * @param[out] type Newly created (or reused with increased refcount) type structure with the filled information about the type.
 * @param[out] units Storage for inheriting units value from the typedefs the current type derives from.
 * @param[out] dflt Default value for the type.
 * @return LY_ERR value.
 */
static LY_ERR
lys_compile_type(struct lysc_ctx *ctx, struct lysp_node *context_pnode, uint16_t context_flags,
        struct lysp_module *context_mod, const char *context_name, struct lysp_type *type_p,
        struct lysc_type **type, const char **units, struct lysp_qname **dflt)
{
    LY_ERR ret = LY_SUCCESS;
    ly_bool dummyloops = 0;
    struct type_context {
        const struct lysp_tpdf *tpdf;
        struct lysp_node *node;
        struct lysp_module *mod;
    } *tctx, *tctx_prev = NULL, *tctx_iter;
    LY_DATA_TYPE basetype = LY_TYPE_UNKNOWN;
    struct lysc_type *base = NULL, *prev_type;
    struct ly_set tpdf_chain = {0};

    (*type) = NULL;
    if (dflt) {
        *dflt = NULL;
    }

    tctx = calloc(1, sizeof *tctx);
    LY_CHECK_ERR_RET(!tctx, LOGMEM(ctx->ctx), LY_EMEM);
    for (ret = lysp_type_find(type_p->name, context_pnode, ctx->mod_def->parsed,
            &basetype, &tctx->tpdf, &tctx->node, &tctx->mod);
            ret == LY_SUCCESS;
            ret = lysp_type_find(tctx_prev->tpdf->type.name, tctx_prev->node, tctx_prev->mod,
                    &basetype, &tctx->tpdf, &tctx->node, &tctx->mod)) {
        if (basetype) {
            break;
        }

        /* check status */
        ret = lysc_check_status(ctx, context_flags, context_mod, context_name,
                tctx->tpdf->flags, tctx->mod, tctx->node ? tctx->node->name : tctx->tpdf->name);
        LY_CHECK_ERR_GOTO(ret, free(tctx), cleanup);

        if (units && !*units) {
            /* inherit units */
            DUP_STRING(ctx->ctx, tctx->tpdf->units, *units, ret);
            LY_CHECK_ERR_GOTO(ret, free(tctx), cleanup);
        }
        if (dflt && !*dflt && tctx->tpdf->dflt.str) {
            /* inherit default */
            *dflt = (struct lysp_qname *)&tctx->tpdf->dflt;
            assert((*dflt)->mod);
        }
        if (dummyloops && (!units || *units) && dflt && *dflt) {
            basetype = ((struct type_context *)tpdf_chain.objs[tpdf_chain.count - 1])->tpdf->type.compiled->basetype;
            break;
        }

        if (tctx->tpdf->type.compiled) {
            /* it is not necessary to continue, the rest of the chain was already compiled,
             * but we still may need to inherit default and units values, so start dummy loops */
            basetype = tctx->tpdf->type.compiled->basetype;
            ret = ly_set_add(&tpdf_chain, tctx, 1, NULL);
            LY_CHECK_ERR_GOTO(ret, free(tctx), cleanup);

            if ((units && !*units) || (dflt && !*dflt)) {
                dummyloops = 1;
                goto preparenext;
            } else {
                tctx = NULL;
                break;
            }
        }

        /* circular typedef reference detection */
        for (uint32_t u = 0; u < tpdf_chain.count; u++) {
            /* local part */
            tctx_iter = (struct type_context *)tpdf_chain.objs[u];
            if ((tctx_iter->mod == tctx->mod) && (tctx_iter->node == tctx->node) && (tctx_iter->tpdf == tctx->tpdf)) {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                        "Invalid \"%s\" type reference - circular chain of types detected.", tctx->tpdf->name);
                free(tctx);
                ret = LY_EVALID;
                goto cleanup;
            }
        }
        for (uint32_t u = 0; u < ctx->tpdf_chain.count; u++) {
            /* global part for unions corner case */
            tctx_iter = (struct type_context *)ctx->tpdf_chain.objs[u];
            if ((tctx_iter->mod == tctx->mod) && (tctx_iter->node == tctx->node) && (tctx_iter->tpdf == tctx->tpdf)) {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                        "Invalid \"%s\" type reference - circular chain of types detected.", tctx->tpdf->name);
                free(tctx);
                ret = LY_EVALID;
                goto cleanup;
            }
        }

        /* store information for the following processing */
        ret = ly_set_add(&tpdf_chain, tctx, 1, NULL);
        LY_CHECK_ERR_GOTO(ret, free(tctx), cleanup);

preparenext:
        /* prepare next loop */
        tctx_prev = tctx;
        tctx = calloc(1, sizeof *tctx);
        LY_CHECK_ERR_RET(!tctx, LOGMEM(ctx->ctx), LY_EMEM);
    }
    free(tctx);

    /* allocate type according to the basetype */
    switch (basetype) {
    case LY_TYPE_BINARY:
        *type = calloc(1, sizeof(struct lysc_type_bin));
        break;
    case LY_TYPE_BITS:
        *type = calloc(1, sizeof(struct lysc_type_bits));
        break;
    case LY_TYPE_BOOL:
    case LY_TYPE_EMPTY:
        *type = calloc(1, sizeof(struct lysc_type));
        break;
    case LY_TYPE_DEC64:
        *type = calloc(1, sizeof(struct lysc_type_dec));
        break;
    case LY_TYPE_ENUM:
        *type = calloc(1, sizeof(struct lysc_type_enum));
        break;
    case LY_TYPE_IDENT:
        *type = calloc(1, sizeof(struct lysc_type_identityref));
        break;
    case LY_TYPE_INST:
        *type = calloc(1, sizeof(struct lysc_type_instanceid));
        break;
    case LY_TYPE_LEAFREF:
        *type = calloc(1, sizeof(struct lysc_type_leafref));
        break;
    case LY_TYPE_STRING:
        *type = calloc(1, sizeof(struct lysc_type_str));
        break;
    case LY_TYPE_UNION:
        *type = calloc(1, sizeof(struct lysc_type_union));
        break;
    case LY_TYPE_INT8:
    case LY_TYPE_UINT8:
    case LY_TYPE_INT16:
    case LY_TYPE_UINT16:
    case LY_TYPE_INT32:
    case LY_TYPE_UINT32:
    case LY_TYPE_INT64:
    case LY_TYPE_UINT64:
        *type = calloc(1, sizeof(struct lysc_type_num));
        break;
    case LY_TYPE_UNKNOWN:
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                "Referenced type \"%s\" not found.", tctx_prev ? tctx_prev->tpdf->type.name : type_p->name);
        ret = LY_EVALID;
        goto cleanup;
    }
    LY_CHECK_ERR_GOTO(!(*type), LOGMEM(ctx->ctx), cleanup);
    if (~type_substmt_map[basetype] & type_p->flags) {
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG, "Invalid type restrictions for %s type.",
                ly_data_type2str[basetype]);
        free(*type);
        (*type) = NULL;
        ret = LY_EVALID;
        goto cleanup;
    }

    /* get restrictions from the referred typedefs */
    for (uint32_t u = tpdf_chain.count - 1; u + 1 > 0; --u) {
        tctx = (struct type_context *)tpdf_chain.objs[u];

        /* remember the typedef context for circular check */
        ret = ly_set_add(&ctx->tpdf_chain, tctx, 1, NULL);
        LY_CHECK_GOTO(ret, cleanup);

        if (tctx->tpdf->type.compiled) {
            base = tctx->tpdf->type.compiled;
            continue;
        } else if ((basetype != LY_TYPE_LEAFREF) && (u != tpdf_chain.count - 1) && !(tctx->tpdf->type.flags)) {
            /* no change, just use the type information from the base */
            base = ((struct lysp_tpdf *)tctx->tpdf)->type.compiled = ((struct type_context *)tpdf_chain.objs[u + 1])->tpdf->type.compiled;
            ++base->refcount;
            continue;
        }

        ++(*type)->refcount;
        if (~type_substmt_map[basetype] & tctx->tpdf->type.flags) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG, "Invalid type \"%s\" restriction(s) for %s type.",
                    tctx->tpdf->name, ly_data_type2str[basetype]);
            ret = LY_EVALID;
            goto cleanup;
        } else if ((basetype == LY_TYPE_EMPTY) && tctx->tpdf->dflt.str) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SEMANTICS,
                    "Invalid type \"%s\" - \"empty\" type must not have a default value (%s).",
                    tctx->tpdf->name, tctx->tpdf->dflt.str);
            ret = LY_EVALID;
            goto cleanup;
        }

        (*type)->basetype = basetype;
        /* TODO user type plugins */
        (*type)->plugin = &ly_builtin_type_plugins[basetype];
        prev_type = *type;
        ret = lys_compile_type_(ctx, tctx->node, tctx->tpdf->flags, tctx->mod, tctx->tpdf->name,
                &((struct lysp_tpdf *)tctx->tpdf)->type, basetype, tctx->tpdf->name, base, type);
        LY_CHECK_GOTO(ret, cleanup);
        base = prev_type;
    }
    /* remove the processed typedef contexts from the stack for circular check */
    ctx->tpdf_chain.count = ctx->tpdf_chain.count - tpdf_chain.count;

    /* process the type definition in leaf */
    if (type_p->flags || !base || (basetype == LY_TYPE_LEAFREF)) {
        /* get restrictions from the node itself */
        (*type)->basetype = basetype;
        /* TODO user type plugins */
        (*type)->plugin = &ly_builtin_type_plugins[basetype];
        ++(*type)->refcount;
        ret = lys_compile_type_(ctx, context_pnode, context_flags, context_mod, context_name, type_p, basetype, NULL,
                base, type);
        LY_CHECK_GOTO(ret, cleanup);
    } else if ((basetype != LY_TYPE_BOOL) && (basetype != LY_TYPE_EMPTY)) {
        /* no specific restriction in leaf's type definition, copy from the base */
        free(*type);
        (*type) = base;
        ++(*type)->refcount;
    }

    COMPILE_EXTS_GOTO(ctx, type_p->exts, (*type)->exts, (*type), LYEXT_PAR_TYPE, ret, cleanup);

cleanup:
    ly_set_erase(&tpdf_chain, free);
    return ret;
}

/**
 * @brief Compile status information of the given node.
 *
 * To simplify getting status of the node, the flags are set following inheritance rules, so all the nodes
 * has the status correctly set during the compilation.
 *
 * @param[in] ctx Compile context
 * @param[in,out] node_flags Flags of the compiled node which status is supposed to be resolved.
 * If the status was set explicitly on the node, it is already set in the flags value and we just check
 * the compatibility with the parent's status value.
 * @param[in] parent_flags Flags of the parent node to check/inherit the status value.
 * @return LY_ERR value.
 */
static LY_ERR
lys_compile_status(struct lysc_ctx *ctx, uint16_t *node_flags, uint16_t parent_flags)
{
    /* status - it is not inherited by specification, but it does not make sense to have
     * current in deprecated or deprecated in obsolete, so we do print warning and inherit status */
    if (!((*node_flags) & LYS_STATUS_MASK)) {
        if (parent_flags & (LYS_STATUS_DEPRC | LYS_STATUS_OBSLT)) {
            if ((parent_flags & 0x3) != 0x3) {
                /* do not print the warning when inheriting status from uses - the uses_status value has a special
                 * combination of bits (0x3) which marks the uses_status value */
                LOGWRN(ctx->ctx, "Missing explicit \"%s\" status that was already specified in parent, inheriting.",
                        (parent_flags & LYS_STATUS_DEPRC) ? "deprecated" : "obsolete");
            }
            (*node_flags) |= parent_flags & LYS_STATUS_MASK;
        } else {
            (*node_flags) |= LYS_STATUS_CURR;
        }
    } else if (parent_flags & LYS_STATUS_MASK) {
        /* check status compatibility with the parent */
        if ((parent_flags & LYS_STATUS_MASK) > ((*node_flags) & LYS_STATUS_MASK)) {
            if ((*node_flags) & LYS_STATUS_CURR) {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SEMANTICS,
                        "A \"current\" status is in conflict with the parent's \"%s\" status.",
                        (parent_flags & LYS_STATUS_DEPRC) ? "deprecated" : "obsolete");
            } else { /* LYS_STATUS_DEPRC */
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SEMANTICS,
                        "A \"deprecated\" status is in conflict with the parent's \"obsolete\" status.");
            }
            return LY_EVALID;
        }
    }
    return LY_SUCCESS;
}

/**
 * @brief Check uniqness of the node/action/notification name.
 *
 * Data nodes, actions/RPCs and Notifications are stored separately (in distinguish lists) in the schema
 * structures, but they share the namespace so we need to check their name collisions.
 *
 * @param[in] ctx Compile context.
 * @param[in] parent Parent of the nodes to check, can be NULL.
 * @param[in] name Name of the item to find in the given lists.
 * @param[in] exclude Node that was just added that should be excluded from the name checking.
 * @return LY_SUCCESS in case of unique name, LY_EEXIST otherwise.
 */
static LY_ERR
lys_compile_node_uniqness(struct lysc_ctx *ctx, const struct lysc_node *parent, const char *name,
        const struct lysc_node *exclude)
{
    const struct lysc_node *iter, *iter2;
    const struct lysc_action *actions;
    const struct lysc_notif *notifs;
    uint32_t getnext_flags;
    LY_ARRAY_COUNT_TYPE u;

#define CHECK_NODE(iter, exclude, name) (iter != (void *)exclude && (iter)->module == exclude->module && !strcmp(name, (iter)->name))

    if (exclude->nodetype == LYS_CASE) {
        /* check restricted only to all the cases */
        assert(parent->nodetype == LYS_CHOICE);
        LY_LIST_FOR(lysc_node_children(parent, 0), iter) {
            if (CHECK_NODE(iter, exclude, name)) {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LY_VCODE_DUPIDENT, name, "case");
                return LY_EEXIST;
            }
        }

        return LY_SUCCESS;
    }

    /* no reason for our parent to be choice anymore */
    assert(!parent || (parent->nodetype != LYS_CHOICE));

    if (parent && (parent->nodetype == LYS_CASE)) {
        /* move to the first data definition parent */
        parent = lysc_data_parent(parent);
    }

    getnext_flags = LYS_GETNEXT_NOSTATECHECK | LYS_GETNEXT_WITHCHOICE;
    if (parent && (parent->nodetype & (LYS_RPC | LYS_ACTION)) && (exclude->flags & LYS_CONFIG_R)) {
        getnext_flags |= LYS_GETNEXT_OUTPUT;
    }

    iter = NULL;
    while ((iter = lys_getnext(iter, parent, ctx->mod->compiled, getnext_flags))) {
        if (CHECK_NODE(iter, exclude, name)) {
            goto error;
        }

        /* we must compare with both the choice and all its nested data-definiition nodes (but not recursively) */
        if (iter->nodetype == LYS_CHOICE) {
            iter2 = NULL;
            while ((iter2 = lys_getnext(iter2, iter, NULL, LYS_GETNEXT_NOSTATECHECK))) {
                if (CHECK_NODE(iter2, exclude, name)) {
                    goto error;
                }
            }
        }
    }

    actions = parent ? lysc_node_actions(parent) : ctx->mod->compiled->rpcs;
    LY_ARRAY_FOR(actions, u) {
        if (CHECK_NODE(&actions[u], exclude, name)) {
            goto error;
        }
    }

    notifs = parent ? lysc_node_notifs(parent) : ctx->mod->compiled->notifs;
    LY_ARRAY_FOR(notifs, u) {
        if (CHECK_NODE(&notifs[u], exclude, name)) {
            goto error;
        }
    }
    return LY_SUCCESS;

error:
    LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LY_VCODE_DUPIDENT, name, "data definition/RPC/action/notification");
    return LY_EEXIST;

#undef CHECK_NODE
}

static LY_ERR lys_compile_node(struct lysc_ctx *ctx, struct lysp_node *pnode, struct lysc_node *parent,
        uint16_t uses_status, struct ly_set *child_set);

static LY_ERR lys_compile_node_deviations_refines(struct lysc_ctx *ctx, const struct lysp_node *pnode,
        const struct lysc_node *parent, struct lysp_node **dev_pnode, ly_bool *not_supported);

static LY_ERR lys_compile_node_augments(struct lysc_ctx *ctx, struct lysc_node *node);

static void lysp_dev_node_free(const struct ly_ctx *ctx, struct lysp_node *dev_pnode);

/**
 * @brief Compile parsed RPC/action schema node information.
 * @param[in] ctx Compile context
 * @param[in] action_p Parsed RPC/action schema node.
 * @param[in] parent Parent node of the action, NULL in case of RPC (top-level action)
 * @param[in,out] action Prepared (empty) compiled action structure to fill.
 * @param[in] uses_status If the RPC/action is being placed instead of uses, here we have the uses's status value (as node's flags).
 * Zero means no uses, non-zero value with no status bit set mean the default status.
 * @return LY_SUCCESS on success,
 * @return LY_EVALID on validation error,
 * @return LY_EDENIED on not-supported deviation.
 */
static LY_ERR
lys_compile_action(struct lysc_ctx *ctx, struct lysp_action *action_p,
        struct lysc_node *parent, struct lysc_action *action, uint16_t uses_status)
{
    LY_ERR ret = LY_SUCCESS;
    struct lysp_node *child_p, *dev_pnode = NULL, *dev_input_p = NULL, *dev_output_p = NULL;
    struct lysp_action *orig_action_p = action_p;
    struct lysp_action_inout *inout_p;
    LY_ARRAY_COUNT_TYPE u;
    ly_bool not_supported;
    uint32_t opt_prev = ctx->options;

    lysc_update_path(ctx, parent, action_p->name);

    /* apply deviation on the action/RPC */
    LY_CHECK_RET(lys_compile_node_deviations_refines(ctx, (struct lysp_node *)action_p, parent, &dev_pnode, &not_supported));
    if (not_supported) {
        lysc_update_path(ctx, NULL, NULL);
        return LY_EDENIED;
    } else if (dev_pnode) {
        action_p = (struct lysp_action *)dev_pnode;
    }

    /* member needed for uniqueness check lys_getnext() */
    action->nodetype = parent ? LYS_ACTION : LYS_RPC;
    action->module = ctx->mod;
    action->parent = parent;

    LY_CHECK_RET(lys_compile_node_uniqness(ctx, parent, action_p->name, (struct lysc_node *)action));

    if (ctx->options & (LYS_COMPILE_RPC_MASK | LYS_COMPILE_NOTIFICATION)) {
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SEMANTICS,
                "Action \"%s\" is placed inside %s.", action_p->name,
                ctx->options & LYS_COMPILE_RPC_MASK ? "another RPC/action" : "notification");
        return LY_EVALID;
    }

    action->sp = orig_action_p;
    action->flags = action_p->flags & LYS_FLAGS_COMPILED_MASK;

    /* status - it is not inherited by specification, but it does not make sense to have
     * current in deprecated or deprecated in obsolete, so we do print warning and inherit status */
    LY_CHECK_RET(lys_compile_status(ctx, &action->flags, uses_status ? uses_status : (parent ? parent->flags : 0)));

    DUP_STRING_GOTO(ctx->ctx, action_p->name, action->name, ret, cleanup);
    DUP_STRING_GOTO(ctx->ctx, action_p->dsc, action->dsc, ret, cleanup);
    DUP_STRING_GOTO(ctx->ctx, action_p->ref, action->ref, ret, cleanup);
    COMPILE_ARRAY_GOTO(ctx, action_p->iffeatures, action->iffeatures, u, lys_compile_iffeature, ret, cleanup);
    COMPILE_EXTS_GOTO(ctx, action_p->exts, action->exts, action, LYEXT_PAR_NODE, ret, cleanup);

    /* connect any action augments */
    LY_CHECK_RET(lys_compile_node_augments(ctx, (struct lysc_node *)action));

    /* input */
    lysc_update_path(ctx, (struct lysc_node *)action, "input");

    /* apply deviations on input */
    LY_CHECK_RET(lys_compile_node_deviations_refines(ctx, (struct lysp_node *)&action_p->input, (struct lysc_node *)action,
            &dev_input_p, &not_supported));
    if (not_supported) {
        inout_p = NULL;
    } else if (dev_input_p) {
        inout_p = (struct lysp_action_inout *)dev_input_p;
    } else {
        inout_p = &action_p->input;
    }

    if (inout_p) {
        action->input.nodetype = LYS_INPUT;
        COMPILE_ARRAY_GOTO(ctx, inout_p->musts, action->input.musts, u, lys_compile_must, ret, cleanup);
        COMPILE_EXTS_GOTO(ctx, inout_p->exts, action->input_exts, &action->input, LYEXT_PAR_INPUT, ret, cleanup);
        ctx->options |= LYS_COMPILE_RPC_INPUT;

        /* connect any input augments */
        LY_CHECK_RET(lys_compile_node_augments(ctx, (struct lysc_node *)&action->input));

        LY_LIST_FOR(inout_p->data, child_p) {
            LY_CHECK_RET(lys_compile_node(ctx, child_p, (struct lysc_node *)action, uses_status, NULL));
        }
        ctx->options = opt_prev;
    }

    lysc_update_path(ctx, NULL, NULL);

    /* output */
    lysc_update_path(ctx, (struct lysc_node *)action, "output");

    /* apply deviations on output */
    LY_CHECK_RET(lys_compile_node_deviations_refines(ctx, (struct lysp_node *)&action_p->output, (struct lysc_node *)action,
            &dev_output_p, &not_supported));
    if (not_supported) {
        inout_p = NULL;
    } else if (dev_output_p) {
        inout_p = (struct lysp_action_inout *)dev_output_p;
    } else {
        inout_p = &action_p->output;
    }

    if (inout_p) {
        action->output.nodetype = LYS_OUTPUT;
        COMPILE_ARRAY_GOTO(ctx, inout_p->musts, action->output.musts, u, lys_compile_must, ret, cleanup);
        COMPILE_EXTS_GOTO(ctx, inout_p->exts, action->output_exts, &action->output, LYEXT_PAR_OUTPUT, ret, cleanup);
        ctx->options |= LYS_COMPILE_RPC_OUTPUT;

        /* connect any output augments */
        LY_CHECK_RET(lys_compile_node_augments(ctx, (struct lysc_node *)&action->output));

        LY_LIST_FOR(inout_p->data, child_p) {
            LY_CHECK_RET(lys_compile_node(ctx, child_p, (struct lysc_node *)action, uses_status, NULL));
        }
        ctx->options = opt_prev;
    }

    lysc_update_path(ctx, NULL, NULL);

    if ((action->input.musts || action->output.musts) && !(ctx->options & LYS_COMPILE_GROUPING)) {
        /* do not check "must" semantics in a grouping */
        ret = ly_set_add(&ctx->xpath, action, 0, NULL);
        LY_CHECK_GOTO(ret, cleanup);
    }

    lysc_update_path(ctx, NULL, NULL);

cleanup:
    lysp_dev_node_free(ctx->ctx, dev_pnode);
    lysp_dev_node_free(ctx->ctx, dev_input_p);
    lysp_dev_node_free(ctx->ctx, dev_output_p);
    ctx->options = opt_prev;
    return ret;
}

/**
 * @brief Compile parsed Notification schema node information.
 * @param[in] ctx Compile context
 * @param[in] notif_p Parsed Notification schema node.
 * @param[in] parent Parent node of the Notification, NULL in case of top-level Notification
 * @param[in,out] notif Prepared (empty) compiled notification structure to fill.
 * @param[in] uses_status If the Notification is being placed instead of uses, here we have the uses's status value (as node's flags).
 * Zero means no uses, non-zero value with no status bit set mean the default status.
 * @return LY_SUCCESS on success,
 * @return LY_EVALID on validation error,
 * @return LY_EDENIED on not-supported deviation.
 */
static LY_ERR
lys_compile_notif(struct lysc_ctx *ctx, struct lysp_notif *notif_p,
        struct lysc_node *parent, struct lysc_notif *notif, uint16_t uses_status)
{
    LY_ERR ret = LY_SUCCESS;
    struct lysp_node *child_p, *dev_pnode = NULL;
    struct lysp_notif *orig_notif_p = notif_p;
    LY_ARRAY_COUNT_TYPE u;
    ly_bool not_supported;
    uint32_t opt_prev = ctx->options;

    lysc_update_path(ctx, parent, notif_p->name);

    LY_CHECK_RET(lys_compile_node_deviations_refines(ctx, (struct lysp_node *)notif_p, parent, &dev_pnode, &not_supported));
    if (not_supported) {
        lysc_update_path(ctx, NULL, NULL);
        return LY_EDENIED;
    } else if (dev_pnode) {
        notif_p = (struct lysp_notif *)dev_pnode;
    }

    /* member needed for uniqueness check lys_getnext() */
    notif->nodetype = LYS_NOTIF;
    notif->module = ctx->mod;
    notif->parent = parent;

    LY_CHECK_RET(lys_compile_node_uniqness(ctx, parent, notif_p->name, (struct lysc_node *)notif));

    if (ctx->options & (LYS_COMPILE_RPC_MASK | LYS_COMPILE_NOTIFICATION)) {
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SEMANTICS,
                "Notification \"%s\" is placed inside %s.", notif_p->name,
                ctx->options & LYS_COMPILE_RPC_MASK ? "RPC/action" : "another notification");
        return LY_EVALID;
    }

    notif->sp = orig_notif_p;
    notif->flags = notif_p->flags & LYS_FLAGS_COMPILED_MASK;

    /* status - it is not inherited by specification, but it does not make sense to have
     * current in deprecated or deprecated in obsolete, so we do print warning and inherit status */
    ret = lys_compile_status(ctx, &notif->flags, uses_status ? uses_status : (parent ? parent->flags : 0));
    LY_CHECK_GOTO(ret, cleanup);

    DUP_STRING_GOTO(ctx->ctx, notif_p->name, notif->name, ret, cleanup);
    DUP_STRING_GOTO(ctx->ctx, notif_p->dsc, notif->dsc, ret, cleanup);
    DUP_STRING_GOTO(ctx->ctx, notif_p->ref, notif->ref, ret, cleanup);
    COMPILE_ARRAY_GOTO(ctx, notif_p->iffeatures, notif->iffeatures, u, lys_compile_iffeature, ret, cleanup);
    COMPILE_ARRAY_GOTO(ctx, notif_p->musts, notif->musts, u, lys_compile_must, ret, cleanup);
    if (notif_p->musts && !(ctx->options & LYS_COMPILE_GROUPING)) {
        /* do not check "must" semantics in a grouping */
        ret = ly_set_add(&ctx->xpath, notif, 0, NULL);
        LY_CHECK_GOTO(ret, cleanup);
    }
    COMPILE_EXTS_GOTO(ctx, notif_p->exts, notif->exts, notif, LYEXT_PAR_NODE, ret, cleanup);

    ctx->options |= LYS_COMPILE_NOTIFICATION;

    /* connect any notification augments */
    LY_CHECK_RET(lys_compile_node_augments(ctx, (struct lysc_node *)notif));

    LY_LIST_FOR(notif_p->data, child_p) {
        ret = lys_compile_node(ctx, child_p, (struct lysc_node *)notif, uses_status, NULL);
        LY_CHECK_GOTO(ret, cleanup);
    }

    lysc_update_path(ctx, NULL, NULL);

cleanup:
    lysp_dev_node_free(ctx->ctx, dev_pnode);
    ctx->options = opt_prev;
    return ret;
}

/**
 * @brief Compile parsed container node information.
 * @param[in] ctx Compile context
 * @param[in] pnode Parsed container node.
 * @param[in,out] node Pre-prepared structure from lys_compile_node() with filled generic node information
 * is enriched with the container-specific information.
 * @return LY_ERR value - LY_SUCCESS or LY_EVALID.
 */
static LY_ERR
lys_compile_node_container(struct lysc_ctx *ctx, struct lysp_node *pnode, struct lysc_node *node)
{
    struct lysp_node_container *cont_p = (struct lysp_node_container *)pnode;
    struct lysc_node_container *cont = (struct lysc_node_container *)node;
    struct lysp_node *child_p;
    LY_ARRAY_COUNT_TYPE u;
    LY_ERR ret = LY_SUCCESS;

    if (cont_p->presence) {
        /* explicit presence */
        cont->flags |= LYS_PRESENCE;
    } else if (cont_p->musts) {
        /* container with a must condition */
        LOGWRN(ctx->ctx, "Container \"%s\" changed to presence because it has a meaning from its \"must\" condition.", cont_p->name);
        cont->flags |= LYS_PRESENCE;
    } else if (cont_p->when) {
        /* container with a when condition */
        LOGWRN(ctx->ctx, "Container \"%s\" changed to presence because it has a meaning from its \"when\" condition.", cont_p->name);
        cont->flags |= LYS_PRESENCE;
    } else if (cont_p->parent) {
        if (cont_p->parent->nodetype == LYS_CHOICE) {
            /* container is an implicit case, so its existence decides the existence of the whole case */
            LOGWRN(ctx->ctx, "Container \"%s\" changed to presence because it has a meaning as a case of choice \"%s\".",
                    cont_p->name, cont_p->parent->name);
            cont->flags |= LYS_PRESENCE;
        } else if ((cont_p->parent->nodetype == LYS_CASE) &&
                (((struct lysp_node_case *)cont_p->parent)->child == pnode) && !cont_p->next) {
            /* container is the only node in a case, so its existence decides the existence of the whole case */
            LOGWRN(ctx->ctx, "Container \"%s\" changed to presence because it has a meaning as a case of choice \"%s\".",
                    cont_p->name, cont_p->parent->name);
            cont->flags |= LYS_PRESENCE;
        }
    }

    /* more cases when the container has meaning but is kept NP for convenience:
     *   - when condition
     *   - direct child action/notification
     */

    LY_LIST_FOR(cont_p->child, child_p) {
        ret = lys_compile_node(ctx, child_p, node, 0, NULL);
        LY_CHECK_GOTO(ret, done);
    }

    COMPILE_ARRAY_GOTO(ctx, cont_p->musts, cont->musts, u, lys_compile_must, ret, done);
    if (cont_p->musts && !(ctx->options & LYS_COMPILE_GROUPING)) {
        /* do not check "must" semantics in a grouping */
        ret = ly_set_add(&ctx->xpath, cont, 0, NULL);
        LY_CHECK_GOTO(ret, done);
    }
    COMPILE_OP_ARRAY_GOTO(ctx, cont_p->actions, cont->actions, node, u, lys_compile_action, 0, ret, done);
    COMPILE_OP_ARRAY_GOTO(ctx, cont_p->notifs, cont->notifs, node, u, lys_compile_notif, 0, ret, done);

done:
    return ret;
}

/*
 * @brief Compile type in leaf/leaf-list node and do all the necessary checks.
 * @param[in] ctx Compile context.
 * @param[in] context_node Schema node where the type/typedef is placed to correctly find the base types.
 * @param[in] type_p Parsed type to compile.
 * @param[in,out] leaf Compiled leaf structure (possibly cast leaf-list) to provide node information and to store the compiled type information.
 * @return LY_ERR value.
 */
static LY_ERR
lys_compile_node_type(struct lysc_ctx *ctx, struct lysp_node *context_node, struct lysp_type *type_p,
        struct lysc_node_leaf *leaf)
{
    struct lysp_qname *dflt;

    LY_CHECK_RET(lys_compile_type(ctx, context_node, leaf->flags, ctx->mod_def->parsed, leaf->name, type_p, &leaf->type,
            leaf->units ? NULL : &leaf->units, &dflt));

    /* store default value, if any */
    if (dflt && !(leaf->flags & LYS_SET_DFLT)) {
        LY_CHECK_RET(lysc_unres_leaf_dflt_add(ctx, leaf, dflt));
    }

    if (leaf->type->basetype == LY_TYPE_LEAFREF) {
        /* store to validate the path in the current context at the end of schema compiling when all the nodes are present */
        LY_CHECK_RET(ly_set_add(&ctx->leafrefs, leaf, 0, NULL));
    } else if (leaf->type->basetype == LY_TYPE_UNION) {
        LY_ARRAY_COUNT_TYPE u;
        LY_ARRAY_FOR(((struct lysc_type_union *)leaf->type)->types, u) {
            if (((struct lysc_type_union *)leaf->type)->types[u]->basetype == LY_TYPE_LEAFREF) {
                /* store to validate the path in the current context at the end of schema compiling when all the nodes are present */
                LY_CHECK_RET(ly_set_add(&ctx->leafrefs, leaf, 0, NULL));
            }
        }
    } else if (leaf->type->basetype == LY_TYPE_EMPTY) {
        if ((leaf->nodetype == LYS_LEAFLIST) && (ctx->mod_def->version < LYS_VERSION_1_1)) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SEMANTICS,
                    "Leaf-list of type \"empty\" is allowed only in YANG 1.1 modules.");
            return LY_EVALID;
        }
    }

    return LY_SUCCESS;
}

/**
 * @brief Compile parsed leaf node information.
 * @param[in] ctx Compile context
 * @param[in] pnode Parsed leaf node.
 * @param[in,out] node Pre-prepared structure from lys_compile_node() with filled generic node information
 * is enriched with the leaf-specific information.
 * @return LY_ERR value - LY_SUCCESS or LY_EVALID.
 */
static LY_ERR
lys_compile_node_leaf(struct lysc_ctx *ctx, struct lysp_node *pnode, struct lysc_node *node)
{
    struct lysp_node_leaf *leaf_p = (struct lysp_node_leaf *)pnode;
    struct lysc_node_leaf *leaf = (struct lysc_node_leaf *)node;
    LY_ARRAY_COUNT_TYPE u;
    LY_ERR ret = LY_SUCCESS;

    COMPILE_ARRAY_GOTO(ctx, leaf_p->musts, leaf->musts, u, lys_compile_must, ret, done);
    if (leaf_p->musts && !(ctx->options & LYS_COMPILE_GROUPING)) {
        /* do not check "must" semantics in a grouping */
        ret = ly_set_add(&ctx->xpath, leaf, 0, NULL);
        LY_CHECK_GOTO(ret, done);
    }
    if (leaf_p->units) {
        LY_CHECK_GOTO(ret = lydict_insert(ctx->ctx, leaf_p->units, 0, &leaf->units), done);
        leaf->flags |= LYS_SET_UNITS;
    }

    /* compile type */
    ret = lys_compile_node_type(ctx, pnode, &leaf_p->type, leaf);
    LY_CHECK_GOTO(ret, done);

    /* store/update default value */
    if (leaf_p->dflt.str) {
        LY_CHECK_RET(lysc_unres_leaf_dflt_add(ctx, leaf, &leaf_p->dflt));
        leaf->flags |= LYS_SET_DFLT;
    }

    /* checks */
    if ((leaf->flags & LYS_SET_DFLT) && (leaf->flags & LYS_MAND_TRUE)) {
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SEMANTICS,
                "Invalid mandatory leaf with a default value.");
        return LY_EVALID;
    }

done:
    return ret;
}

/**
 * @brief Compile parsed leaf-list node information.
 * @param[in] ctx Compile context
 * @param[in] pnode Parsed leaf-list node.
 * @param[in,out] node Pre-prepared structure from lys_compile_node() with filled generic node information
 * is enriched with the leaf-list-specific information.
 * @return LY_ERR value - LY_SUCCESS or LY_EVALID.
 */
static LY_ERR
lys_compile_node_leaflist(struct lysc_ctx *ctx, struct lysp_node *pnode, struct lysc_node *node)
{
    struct lysp_node_leaflist *llist_p = (struct lysp_node_leaflist *)pnode;
    struct lysc_node_leaflist *llist = (struct lysc_node_leaflist *)node;
    LY_ARRAY_COUNT_TYPE u;
    LY_ERR ret = LY_SUCCESS;

    COMPILE_ARRAY_GOTO(ctx, llist_p->musts, llist->musts, u, lys_compile_must, ret, done);
    if (llist_p->musts && !(ctx->options & LYS_COMPILE_GROUPING)) {
        /* do not check "must" semantics in a grouping */
        ret = ly_set_add(&ctx->xpath, llist, 0, NULL);
        LY_CHECK_GOTO(ret, done);
    }
    if (llist_p->units) {
        LY_CHECK_GOTO(ret = lydict_insert(ctx->ctx, llist_p->units, 0, &llist->units), done);
        llist->flags |= LYS_SET_UNITS;
    }

    /* compile type */
    ret = lys_compile_node_type(ctx, pnode, &llist_p->type, (struct lysc_node_leaf *)llist);
    LY_CHECK_GOTO(ret, done);

    /* store/update default values */
    if (llist_p->dflts) {
        if (ctx->mod_def->version < LYS_VERSION_1_1) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SEMANTICS,
                    "Leaf-list default values are allowed only in YANG 1.1 modules.");
            return LY_EVALID;
        }

        LY_CHECK_GOTO(lysc_unres_llist_dflts_add(ctx, llist, llist_p->dflts), done);
        llist->flags |= LYS_SET_DFLT;
    }

    llist->min = llist_p->min;
    if (llist->min) {
        llist->flags |= LYS_MAND_TRUE;
    }
    llist->max = llist_p->max ? llist_p->max : (uint32_t)-1;

    /* checks */
    if ((llist->flags & LYS_SET_DFLT) && (llist->flags & LYS_MAND_TRUE)) {
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SEMANTICS,
                "Invalid mandatory leaf-list with default value(s).");
        return LY_EVALID;
    }

    if (llist->min > llist->max) {
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SEMANTICS, "Leaf-list min-elements %u is bigger than max-elements %u.",
                llist->min, llist->max);
        return LY_EVALID;
    }

done:
    return ret;
}

/**
 * @brief Find the node according to the given descendant/absolute schema nodeid.
 * Used in unique, refine and augment statements.
 *
 * @param[in] ctx Compile context
 * @param[in] nodeid Descendant-schema-nodeid (according to the YANG grammar)
 * @param[in] nodeid_len Length of the given nodeid, if it is not NULL-terminated string.
 * @param[in] context_node Node where the nodeid is specified to correctly resolve prefixes and to start searching.
 * If no context node is provided, the nodeid is actually expected to be the absolute schema node .
 * @param[in] context_module Explicit module to resolve prefixes in @nodeid.
 * @param[in] nodetype Optional (can be 0) restriction for target's nodetype. If target exists, but does not match
 * the given nodetype, LY_EDENIED is returned (and target is provided), but no error message is printed.
 * The value can be even an ORed value to allow multiple nodetypes.
 * @param[out] target Found target node if any.
 * @param[out] result_flag Output parameter to announce if the schema nodeid goes through the action's input/output or a Notification.
 * The LYSC_OPT_RPC_INPUT, LYSC_OPT_RPC_OUTPUT and LYSC_OPT_NOTIFICATION are used as flags.
 * @return LY_ERR values - LY_ENOTFOUND, LY_EVALID, LY_EDENIED or LY_SUCCESS.
 */
static LY_ERR
lysc_resolve_schema_nodeid(struct lysc_ctx *ctx, const char *nodeid, size_t nodeid_len, const struct lysc_node *context_node,
        const struct lys_module *context_module, uint16_t nodetype, const struct lysc_node **target, uint16_t *result_flag)
{
    LY_ERR ret = LY_EVALID;
    const char *name, *prefix, *id;
    size_t name_len, prefix_len;
    const struct lys_module *mod;
    const char *nodeid_type;
    uint32_t getnext_extra_flag = 0;
    uint16_t current_nodetype = 0;

    assert(nodeid);
    assert(target);
    assert(result_flag);
    *target = NULL;
    *result_flag = 0;

    id = nodeid;

    if (context_node) {
        /* descendant-schema-nodeid */
        nodeid_type = "descendant";

        if (*id == '/') {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                    "Invalid descendant-schema-nodeid value \"%.*s\" - absolute-schema-nodeid used.",
                    nodeid_len ? nodeid_len : strlen(nodeid), nodeid);
            return LY_EVALID;
        }
    } else {
        /* absolute-schema-nodeid */
        nodeid_type = "absolute";

        if (*id != '/') {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                    "Invalid absolute-schema-nodeid value \"%.*s\" - missing starting \"/\".",
                    nodeid_len ? nodeid_len : strlen(nodeid), nodeid);
            return LY_EVALID;
        }
        ++id;
    }

    while (*id && (ret = ly_parse_nodeid(&id, &prefix, &prefix_len, &name, &name_len)) == LY_SUCCESS) {
        if (prefix) {
            mod = lys_module_find_prefix(context_module, prefix, prefix_len);
            if (!mod) {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                        "Invalid %s-schema-nodeid value \"%.*s\" - prefix \"%.*s\" not defined in module \"%s\".",
                        nodeid_type, id - nodeid, nodeid, prefix_len, prefix, context_module->name);
                return LY_ENOTFOUND;
            }
        } else {
            mod = context_module;
        }
        if (context_node && (context_node->nodetype & (LYS_RPC | LYS_ACTION))) {
            /* move through input/output manually */
            if (mod != context_node->module) {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                        "Invalid %s-schema-nodeid value \"%.*s\" - target node not found.", nodeid_type, id - nodeid, nodeid);
                return LY_ENOTFOUND;
            }
            if (!ly_strncmp("input", name, name_len)) {
                context_node = (struct lysc_node *)&((struct lysc_action *)context_node)->input;
            } else if (!ly_strncmp("output", name, name_len)) {
                context_node = (struct lysc_node *)&((struct lysc_action *)context_node)->output;
                getnext_extra_flag = LYS_GETNEXT_OUTPUT;
            } else {
                /* only input or output is valid */
                context_node = NULL;
            }
        } else {
            context_node = lys_find_child(context_node, mod, name, name_len, 0,
                    getnext_extra_flag | LYS_GETNEXT_NOSTATECHECK | LYS_GETNEXT_WITHCHOICE | LYS_GETNEXT_WITHCASE);
            getnext_extra_flag = 0;
        }
        if (!context_node) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                    "Invalid %s-schema-nodeid value \"%.*s\" - target node not found.", nodeid_type, id - nodeid, nodeid);
            return LY_ENOTFOUND;
        }
        current_nodetype = context_node->nodetype;

        if (current_nodetype == LYS_NOTIF) {
            (*result_flag) |= LYS_COMPILE_NOTIFICATION;
        } else if (current_nodetype == LYS_INPUT) {
            (*result_flag) |= LYS_COMPILE_RPC_INPUT;
        } else if (current_nodetype == LYS_OUTPUT) {
            (*result_flag) |= LYS_COMPILE_RPC_OUTPUT;
        }

        if (!*id || (nodeid_len && ((size_t)(id - nodeid) >= nodeid_len))) {
            break;
        }
        if (*id != '/') {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                    "Invalid %s-schema-nodeid value \"%.*s\" - missing \"/\" as node-identifier separator.",
                    nodeid_type, id - nodeid + 1, nodeid);
            return LY_EVALID;
        }
        ++id;
    }

    if (ret == LY_SUCCESS) {
        *target = context_node;
        if (nodetype && !(current_nodetype & nodetype)) {
            return LY_EDENIED;
        }
    } else {
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                "Invalid %s-schema-nodeid value \"%.*s\" - unexpected end of expression.",
                nodeid_type, nodeid_len ? nodeid_len : strlen(nodeid), nodeid);
    }

    return ret;
}

/**
 * @brief Compile information about list's uniques.
 * @param[in] ctx Compile context.
 * @param[in] uniques Sized array list of unique statements.
 * @param[in] list Compiled list where the uniques are supposed to be resolved and stored.
 * @return LY_ERR value.
 */
static LY_ERR
lys_compile_node_list_unique(struct lysc_ctx *ctx, struct lysp_qname *uniques, struct lysc_node_list *list)
{
    LY_ERR ret = LY_SUCCESS;
    struct lysc_node_leaf **key, ***unique;
    struct lysc_node *parent;
    const char *keystr, *delim;
    size_t len;
    LY_ARRAY_COUNT_TYPE v;
    int8_t config; /* -1 - not yet seen; 0 - LYS_CONFIG_R; 1 - LYS_CONFIG_W */
    uint16_t flags;

    LY_ARRAY_FOR(uniques, v) {
        config = -1;
        LY_ARRAY_NEW_RET(ctx->ctx, list->uniques, unique, LY_EMEM);
        keystr = uniques[v].str;
        while (keystr) {
            delim = strpbrk(keystr, " \t\n");
            if (delim) {
                len = delim - keystr;
                while (isspace(*delim)) {
                    ++delim;
                }
            } else {
                len = strlen(keystr);
            }

            /* unique node must be present */
            LY_ARRAY_NEW_RET(ctx->ctx, *unique, key, LY_EMEM);
            ret = lysc_resolve_schema_nodeid(ctx, keystr, len, (struct lysc_node *)list, uniques[v].mod, LYS_LEAF,
                    (const struct lysc_node **)key, &flags);
            if (ret != LY_SUCCESS) {
                if (ret == LY_EDENIED) {
                    LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                            "Unique's descendant-schema-nodeid \"%.*s\" refers to %s node instead of a leaf.",
                            len, keystr, lys_nodetype2str((*key)->nodetype));
                }
                return LY_EVALID;
            } else if (flags) {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                        "Unique's descendant-schema-nodeid \"%.*s\" refers into %s node.",
                        len, keystr, flags & LYS_COMPILE_NOTIFICATION ? "notification" : "RPC/action");
                return LY_EVALID;
            }

            /* all referenced leafs must be of the same config type */
            if ((config != -1) && ((((*key)->flags & LYS_CONFIG_W) && (config == 0)) || (((*key)->flags & LYS_CONFIG_R) && (config == 1)))) {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SEMANTICS,
                        "Unique statement \"%s\" refers to leaves with different config type.", uniques[v].str);
                return LY_EVALID;
            } else if ((*key)->flags & LYS_CONFIG_W) {
                config = 1;
            } else { /* LYS_CONFIG_R */
                config = 0;
            }

            /* we forbid referencing nested lists because it is unspecified what instance of such a list to use */
            for (parent = (*key)->parent; parent != (struct lysc_node *)list; parent = parent->parent) {
                if (parent->nodetype == LYS_LIST) {
                    LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SEMANTICS,
                            "Unique statement \"%s\" refers to a leaf in nested list \"%s\".", uniques[v].str, parent->name);
                    return LY_EVALID;
                }
            }

            /* check status */
            LY_CHECK_RET(lysc_check_status(ctx, list->flags, list->module, list->name,
                    (*key)->flags, (*key)->module, (*key)->name));

            /* mark leaf as unique */
            (*key)->flags |= LYS_UNIQUE;

            /* next unique value in line */
            keystr = delim;
        }
        /* next unique definition */
    }

    return LY_SUCCESS;
}

/**
 * @brief Compile parsed list node information.
 * @param[in] ctx Compile context
 * @param[in] pnode Parsed list node.
 * @param[in,out] node Pre-prepared structure from lys_compile_node() with filled generic node information
 * is enriched with the list-specific information.
 * @return LY_ERR value - LY_SUCCESS or LY_EVALID.
 */
static LY_ERR
lys_compile_node_list(struct lysc_ctx *ctx, struct lysp_node *pnode, struct lysc_node *node)
{
    struct lysp_node_list *list_p = (struct lysp_node_list *)pnode;
    struct lysc_node_list *list = (struct lysc_node_list *)node;
    struct lysp_node *child_p;
    struct lysc_node_leaf *key, *prev_key = NULL;
    size_t len;
    LY_ARRAY_COUNT_TYPE u;
    const char *keystr, *delim;
    LY_ERR ret = LY_SUCCESS;

    list->min = list_p->min;
    if (list->min) {
        list->flags |= LYS_MAND_TRUE;
    }
    list->max = list_p->max ? list_p->max : (uint32_t)-1;

    LY_LIST_FOR(list_p->child, child_p) {
        LY_CHECK_RET(lys_compile_node(ctx, child_p, node, 0, NULL));
    }

    COMPILE_ARRAY_GOTO(ctx, list_p->musts, list->musts, u, lys_compile_must, ret, done);
    if (list_p->musts && !(ctx->options & LYS_COMPILE_GROUPING)) {
        /* do not check "must" semantics in a grouping */
        LY_CHECK_RET(ly_set_add(&ctx->xpath, list, 0, NULL));
    }

    /* keys */
    if ((list->flags & LYS_CONFIG_W) && (!list_p->key || !list_p->key[0])) {
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SEMANTICS, "Missing key in list representing configuration data.");
        return LY_EVALID;
    }

    /* find all the keys (must be direct children) */
    keystr = list_p->key;
    if (!keystr) {
        /* keyless list */
        list->flags |= LYS_KEYLESS;
    }
    while (keystr) {
        delim = strpbrk(keystr, " \t\n");
        if (delim) {
            len = delim - keystr;
            while (isspace(*delim)) {
                ++delim;
            }
        } else {
            len = strlen(keystr);
        }

        /* key node must be present */
        key = (struct lysc_node_leaf *)lys_find_child(node, node->module, keystr, len, LYS_LEAF, LYS_GETNEXT_NOCHOICE | LYS_GETNEXT_NOSTATECHECK);
        if (!(key)) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                    "The list's key \"%.*s\" not found.", len, keystr);
            return LY_EVALID;
        }
        /* keys must be unique */
        if (key->flags & LYS_KEY) {
            /* the node was already marked as a key */
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SEMANTICS,
                    "Duplicated key identifier \"%.*s\".", len, keystr);
            return LY_EVALID;
        }

        lysc_update_path(ctx, (struct lysc_node *)list, key->name);
        /* key must have the same config flag as the list itself */
        if ((list->flags & LYS_CONFIG_MASK) != (key->flags & LYS_CONFIG_MASK)) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SEMANTICS, "Key of the configuration list must not be status leaf.");
            return LY_EVALID;
        }
        if (ctx->mod_def->version < LYS_VERSION_1_1) {
            /* YANG 1.0 denies key to be of empty type */
            if (key->type->basetype == LY_TYPE_EMPTY) {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SEMANTICS,
                        "List's key cannot be of \"empty\" type until it is in YANG 1.1 module.");
                return LY_EVALID;
            }
        } else {
            /* when and if-feature are illegal on list keys */
            if (key->when) {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SEMANTICS,
                        "List's key must not have any \"when\" statement.");
                return LY_EVALID;
            }
            if (key->iffeatures) {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SEMANTICS,
                        "List's key must not have any \"if-feature\" statement.");
                return LY_EVALID;
            }
        }

        /* check status */
        LY_CHECK_RET(lysc_check_status(ctx, list->flags, list->module, list->name,
                key->flags, key->module, key->name));

        /* ignore default values of the key */
        if (key->dflt) {
            key->dflt->realtype->plugin->free(ctx->ctx, key->dflt);
            lysc_type_free(ctx->ctx, (struct lysc_type *)key->dflt->realtype);
            free(key->dflt);
            key->dflt = NULL;
        }
        /* mark leaf as key */
        key->flags |= LYS_KEY;

        /* move it to the correct position */
        if ((prev_key && ((struct lysc_node *)prev_key != key->prev)) || (!prev_key && key->prev->next)) {
            /* fix links in closest previous siblings of the key */
            if (key->next) {
                key->next->prev = key->prev;
            } else {
                /* last child */
                list->child->prev = key->prev;
            }
            if (key->prev->next) {
                key->prev->next = key->next;
            }
            /* fix links in the key */
            if (prev_key) {
                key->prev = (struct lysc_node *)prev_key;
                key->next = prev_key->next;
            } else {
                key->prev = list->child->prev;
                key->next = list->child;
            }
            /* fix links in closes future siblings of the key */
            if (prev_key) {
                if (prev_key->next) {
                    prev_key->next->prev = (struct lysc_node *)key;
                } else {
                    list->child->prev = (struct lysc_node *)key;
                }
                prev_key->next = (struct lysc_node *)key;
            } else {
                list->child->prev = (struct lysc_node *)key;
            }
            /* fix links in parent */
            if (!key->prev->next) {
                list->child = (struct lysc_node *)key;
            }
        }

        /* next key value */
        prev_key = key;
        keystr = delim;
        lysc_update_path(ctx, NULL, NULL);
    }

    /* uniques */
    if (list_p->uniques) {
        LY_CHECK_RET(lys_compile_node_list_unique(ctx, list_p->uniques, list));
    }

    COMPILE_OP_ARRAY_GOTO(ctx, list_p->actions, list->actions, node, u, lys_compile_action, 0, ret, done);
    COMPILE_OP_ARRAY_GOTO(ctx, list_p->notifs, list->notifs, node, u, lys_compile_notif, 0, ret, done);

    /* checks */
    if (list->min > list->max) {
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SEMANTICS, "List min-elements %u is bigger than max-elements %u.",
                list->min, list->max);
        return LY_EVALID;
    }

done:
    return ret;
}

/**
 * @brief Do some checks and set the default choice's case.
 *
 * Selects (and stores into ::lysc_node_choice#dflt) the default case and set LYS_SET_DFLT flag on it.
 *
 * @param[in] ctx Compile context.
 * @param[in] dflt Name of the default branch. Can even contain a prefix.
 * @param[in,out] ch The compiled choice node, its dflt member is filled to point to the default case node of the choice.
 * @return LY_ERR value.
 */
static LY_ERR
lys_compile_node_choice_dflt(struct lysc_ctx *ctx, struct lysp_qname *dflt, struct lysc_node_choice *ch)
{
    struct lysc_node *iter, *node = (struct lysc_node *)ch;
    const struct lys_module *mod;
    const char *prefix = NULL, *name;
    size_t prefix_len = 0;

    /* could use lys_parse_nodeid(), but it checks syntax which is already done in this case by the parsers */
    name = strchr(dflt->str, ':');
    if (name) {
        prefix = dflt->str;
        prefix_len = name - prefix;
        ++name;
    } else {
        name = dflt->str;
    }
    if (prefix) {
        mod = ly_resolve_prefix(ctx->ctx, prefix, prefix_len, LY_PREF_SCHEMA, (void *)dflt->mod);
        if (!mod) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                    "Default case prefix \"%.*s\" not found in imports of \"%s\".", prefix_len, prefix, dflt->mod->name);
            return LY_EVALID;
        }
    } else {
        mod = node->module;
    }

    ch->dflt = (struct lysc_node_case *)lys_find_child(node, mod, name, 0, LYS_CASE, LYS_GETNEXT_NOSTATECHECK | LYS_GETNEXT_WITHCASE);
    if (!ch->dflt) {
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SEMANTICS,
                "Default case \"%s\" not found.", dflt->str);
        return LY_EVALID;
    }

    /* no mandatory nodes directly under the default case */
    LY_LIST_FOR(ch->dflt->child, iter) {
        if (iter->parent != (struct lysc_node *)ch->dflt) {
            break;
        }
        if (iter->flags & LYS_MAND_TRUE) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SEMANTICS,
                    "Mandatory node \"%s\" under the default case \"%s\".", iter->name, dflt->str);
            return LY_EVALID;
        }
    }

    if (ch->flags & LYS_MAND_TRUE) {
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SEMANTICS, "Invalid mandatory choice with a default case.");
        return LY_EVALID;
    }

    ch->dflt->flags |= LYS_SET_DFLT;
    return LY_SUCCESS;
}

/**
 * @brief Compile choice children.
 *
 * @param[in] ctx Compile context
 * @param[in] child_p Parsed choice children nodes.
 * @param[in] node Compiled choice node to compile and add children to.
 * @return LY_ERR value - LY_SUCCESS or LY_EVALID.
 */
static LY_ERR
lys_compile_node_choice_child(struct lysc_ctx *ctx, struct lysp_node *child_p, struct lysc_node *node,
        struct ly_set *child_set)
{
    LY_ERR ret = LY_SUCCESS;
    struct lysp_node *child_p_next = child_p->next;
    struct lysp_node_case *cs_p;

    if (child_p->nodetype == LYS_CASE) {
        /* standard case under choice */
        ret = lys_compile_node(ctx, child_p, node, 0, child_set);
    } else {
        /* we need the implicit case first, so create a fake parsed case */
        cs_p = calloc(1, sizeof *cs_p);
        cs_p->nodetype = LYS_CASE;
        DUP_STRING_GOTO(ctx->ctx, child_p->name, cs_p->name, ret, free_fake_node);
        cs_p->child = child_p;

        /* make the child the only case child */
        child_p->next = NULL;

        /* compile it normally */
        ret = lys_compile_node(ctx, (struct lysp_node *)cs_p, node, 0, child_set);

free_fake_node:
        /* free the fake parsed node and correct pointers back */
        cs_p->child = NULL;
        lysp_node_free(ctx->ctx, (struct lysp_node *)cs_p);
        child_p->next = child_p_next;
    }

    return ret;
}

/**
 * @brief Compile parsed choice node information.
 *
 * @param[in] ctx Compile context
 * @param[in] pnode Parsed choice node.
 * @param[in,out] node Pre-prepared structure from lys_compile_node() with filled generic node information
 * is enriched with the choice-specific information.
 * @return LY_ERR value - LY_SUCCESS or LY_EVALID.
 */
static LY_ERR
lys_compile_node_choice(struct lysc_ctx *ctx, struct lysp_node *pnode, struct lysc_node *node)
{
    struct lysp_node_choice *ch_p = (struct lysp_node_choice *)pnode;
    struct lysc_node_choice *ch = (struct lysc_node_choice *)node;
    struct lysp_node *child_p;
    LY_ERR ret = LY_SUCCESS;

    assert(node->nodetype == LYS_CHOICE);

    LY_LIST_FOR(ch_p->child, child_p) {
        LY_CHECK_RET(lys_compile_node_choice_child(ctx, child_p, node, NULL));
    }

    /* default branch */
    if (ch_p->dflt.str) {
        LY_CHECK_RET(lys_compile_node_choice_dflt(ctx, &ch_p->dflt, ch));
    }

    return ret;
}

/**
 * @brief Compile parsed anydata or anyxml node information.
 * @param[in] ctx Compile context
 * @param[in] pnode Parsed anydata or anyxml node.
 * @param[in,out] node Pre-prepared structure from lys_compile_node() with filled generic node information
 * is enriched with the any-specific information.
 * @return LY_ERR value - LY_SUCCESS or LY_EVALID.
 */
static LY_ERR
lys_compile_node_any(struct lysc_ctx *ctx, struct lysp_node *pnode, struct lysc_node *node)
{
    struct lysp_node_anydata *any_p = (struct lysp_node_anydata *)pnode;
    struct lysc_node_anydata *any = (struct lysc_node_anydata *)node;
    LY_ARRAY_COUNT_TYPE u;
    LY_ERR ret = LY_SUCCESS;

    COMPILE_ARRAY_GOTO(ctx, any_p->musts, any->musts, u, lys_compile_must, ret, done);
    if (any_p->musts && !(ctx->options & LYS_COMPILE_GROUPING)) {
        /* do not check "must" semantics in a grouping */
        ret = ly_set_add(&ctx->xpath, any, 0, NULL);
        LY_CHECK_GOTO(ret, done);
    }

    if (any->flags & LYS_CONFIG_W) {
        LOGWRN(ctx->ctx, "Use of %s to define configuration data is not recommended. %s",
                ly_stmt2str(any->nodetype == LYS_ANYDATA ? LY_STMT_ANYDATA : LY_STMT_ANYXML), ctx->path);
    }
done:
    return ret;
}

/**
 * @brief Connect the node into the siblings list and check its name uniqueness. Also,
 * keep specific order of augments targetting the same node.
 *
 * @param[in] ctx Compile context
 * @param[in] parent Parent node holding the children list, in case of node from a choice's case,
 * the choice itself is expected instead of a specific case node.
 * @param[in] node Schema node to connect into the list.
 * @return LY_ERR value - LY_SUCCESS or LY_EEXIST.
 * In case of LY_EEXIST, the node is actually kept in the tree, so do not free it directly.
 */
static LY_ERR
lys_compile_node_connect(struct lysc_ctx *ctx, struct lysc_node *parent, struct lysc_node *node)
{
    struct lysc_node **children, *anchor = NULL;
    int insert_after = 0;

    node->parent = parent;

    if (parent) {
        if (parent->nodetype == LYS_CHOICE) {
            assert(node->nodetype == LYS_CASE);
            children = (struct lysc_node **)&((struct lysc_node_choice *)parent)->cases;
        } else {
            children = lysc_node_children_p(parent, ctx->options);
        }
        assert(children);

        if (!(*children)) {
            /* first child */
            *children = node;
        } else if (node->flags & LYS_KEY) {
            /* special handling of adding keys */
            assert(node->module == parent->module);
            anchor = *children;
            if (anchor->flags & LYS_KEY) {
                while ((anchor->flags & LYS_KEY) && anchor->next) {
                    anchor = anchor->next;
                }
                /* insert after the last key */
                insert_after = 1;
            } /* else insert before anchor (at the beginning) */
        } else if ((*children)->prev->module == node->module) {
            /* last child is from the same module, keep the order and insert at the end */
            anchor = (*children)->prev;
            insert_after = 1;
        } else if (parent->module == node->module) {
            /* adding module child after some augments were connected */
            for (anchor = *children; anchor->module == node->module; anchor = anchor->next) {}
        } else {
            /* some augments are already connected and we are connecting new ones,
             * keep module name order and insert the node into the children list */
            anchor = *children;
            do {
                anchor = anchor->prev;

                /* check that we have not found the last augment node from our module or
                 * the first augment node from a "smaller" module or
                 * the first node from a local module */
                if ((anchor->module == node->module) || (strcmp(anchor->module->name, node->module->name) < 0) ||
                        (anchor->module == parent->module)) {
                    /* insert after */
                    insert_after = 1;
                    break;
                }

                /* we have traversed all the nodes, insert before anchor (as the first node) */
            } while (anchor->prev->next);
        }

        /* insert */
        if (anchor) {
            if (insert_after) {
                node->next = anchor->next;
                node->prev = anchor;
                anchor->next = node;
                if (node->next) {
                    /* middle node */
                    node->next->prev = node;
                } else {
                    /* last node */
                    (*children)->prev = node;
                }
            } else {
                node->next = anchor;
                node->prev = anchor->prev;
                anchor->prev = node;
                if (anchor == *children) {
                    /* first node */
                    *children = node;
                } else {
                    /* middle node */
                    node->prev->next = node;
                }
            }
        }

        /* check the name uniqueness (even for an only child, it may be in case) */
        if (lys_compile_node_uniqness(ctx, parent, node->name, node)) {
            return LY_EEXIST;
        }
    } else {
        /* top-level element */
        if (!ctx->mod->compiled->data) {
            ctx->mod->compiled->data = node;
        } else {
            /* insert at the end of the module's top-level nodes list */
            ctx->mod->compiled->data->prev->next = node;
            node->prev = ctx->mod->compiled->data->prev;
            ctx->mod->compiled->data->prev = node;
        }

        /* check the name uniqueness on top-level */
        if (lys_compile_node_uniqness(ctx, NULL, node->name, node)) {
            return LY_EEXIST;
        }
    }

    return LY_SUCCESS;
}

/**
 * @brief Prepare the case structure in choice node for the new data node.
 *
 * It is able to handle implicit as well as explicit cases and the situation when the case has multiple data nodes and the case was already
 * created in the choice when the first child was processed.
 *
 * @param[in] ctx Compile context.
 * @param[in] pnode Node image from the parsed tree. If the case is explicit, it is the LYS_CASE node, but in case of implicit case,
 *                   it is the LYS_CHOICE, LYS_AUGMENT or LYS_GROUPING node.
 * @param[in] ch The compiled choice structure where the new case structures are created (if needed).
 * @param[in] child The new data node being part of a case (no matter if explicit or implicit).
 * @return The case structure where the child node belongs to, NULL in case of error. Note that the child is not connected into the siblings list,
 * it is linked from the case structure only in case it is its first child.
 */
static LY_ERR
lys_compile_node_case(struct lysc_ctx *ctx, struct lysp_node *pnode, struct lysc_node *node)
{
    struct lysp_node *child_p;
    struct lysp_node_case *cs_p = (struct lysp_node_case *)pnode;

    if (pnode->nodetype & (LYS_CHOICE | LYS_AUGMENT | LYS_GROUPING)) {
        /* we have to add an implicit case node into the parent choice */
    } else if (pnode->nodetype == LYS_CASE) {
        /* explicit parent case */
        LY_LIST_FOR(cs_p->child, child_p) {
            LY_CHECK_RET(lys_compile_node(ctx, child_p, node, 0, NULL));
        }
    } else {
        LOGINT_RET(ctx->ctx);
    }

    return LY_SUCCESS;
}

/**
 * @brief Set LYS_MAND_TRUE flag for the non-presence container parents.
 *
 * A non-presence container is mandatory in case it has at least one mandatory children. This function propagate
 * the flag to such parents from a mandatory children.
 *
 * @param[in] parent A schema node to be examined if the mandatory child make it also mandatory.
 * @param[in] add Flag to distinguish adding the mandatory flag (new mandatory children appeared) or removing the flag
 * (mandatory children was removed).
 */
static void
lys_compile_mandatory_parents(struct lysc_node *parent, ly_bool add)
{
    struct lysc_node *iter;

    if (add) { /* set flag */
        for ( ; parent && parent->nodetype == LYS_CONTAINER && !(parent->flags & LYS_MAND_TRUE) && !(parent->flags & LYS_PRESENCE);
                parent = parent->parent) {
            parent->flags |= LYS_MAND_TRUE;
        }
    } else { /* unset flag */
        for ( ; parent && parent->nodetype == LYS_CONTAINER && (parent->flags & LYS_MAND_TRUE); parent = parent->parent) {
            for (iter = (struct lysc_node *)lysc_node_children(parent, 0); iter; iter = iter->next) {
                if (iter->flags & LYS_MAND_TRUE) {
                    /* there is another mandatory node */
                    return;
                }
            }
            /* unset mandatory flag - there is no mandatory children in the non-presence container */
            parent->flags &= ~LYS_MAND_TRUE;
        }
    }
}

/**
 * @brief Compile the parsed augment connecting it into its target.
 *
 * It is expected that all the data referenced in path are present - augments are ordered so that augment B
 * targeting data from augment A is being compiled after augment A. Also the modules referenced in the path
 * are already implemented and compiled.
 *
 * @param[in] ctx Compile context.
 * @param[in] aug_p Parsed augment to compile.
 * @param[in] target Target node of the augment.
 * @return LY_SUCCESS on success.
 * @return LY_EVALID on failure.
 */
static LY_ERR
lys_compile_augment(struct lysc_ctx *ctx, struct lysp_augment *aug_p, struct lysc_node *target)
{
    LY_ERR ret = LY_SUCCESS;
    struct lysp_node *pnode;
    struct lysc_node *node;
    struct lysc_when *when_shared = NULL;
    struct lysc_action **actions;
    struct lysc_notif **notifs;
    ly_bool allow_mandatory = 0;
    LY_ARRAY_COUNT_TYPE u;
    struct ly_set child_set = {0};
    uint32_t i;

    if (!(target->nodetype & (LYS_CONTAINER | LYS_LIST | LYS_CHOICE | LYS_CASE | LYS_INPUT | LYS_OUTPUT | LYS_NOTIF))) {
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                "Augment's %s-schema-nodeid \"%s\" refers to a %s node which is not an allowed augment's target.",
                aug_p->nodeid[0] == '/' ? "absolute" : "descendant", aug_p->nodeid, lys_nodetype2str(target->nodetype));
        ret = LY_EVALID;
        goto cleanup;
    }

    /* check for mandatory nodes
     * - new cases augmenting some choice can have mandatory nodes
     * - mandatory nodes are allowed only in case the augmentation is made conditional with a when statement
     */
    if (aug_p->when || (target->nodetype == LYS_CHOICE) || (ctx->mod == target->module)) {
        allow_mandatory = 1;
    }

    LY_LIST_FOR(aug_p->child, pnode) {
        /* check if the subnode can be connected to the found target (e.g. case cannot be inserted into container) */
        if (((pnode->nodetype == LYS_CASE) && (target->nodetype != LYS_CHOICE)) ||
                ((pnode->nodetype & (LYS_RPC | LYS_ACTION | LYS_NOTIF)) && !(target->nodetype & (LYS_CONTAINER | LYS_LIST))) ||
                ((pnode->nodetype == LYS_USES) && (target->nodetype == LYS_CHOICE))) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                    "Invalid augment of %s node which is not allowed to contain %s node \"%s\".",
                    lys_nodetype2str(target->nodetype), lys_nodetype2str(pnode->nodetype), pnode->name);
            ret = LY_EVALID;
            goto cleanup;
        }

        /* compile the children */
        if (target->nodetype == LYS_CHOICE) {
            LY_CHECK_GOTO(ret = lys_compile_node_choice_child(ctx, pnode, target, &child_set), cleanup);
        } else {
            LY_CHECK_GOTO(ret = lys_compile_node(ctx, pnode, target, 0, &child_set), cleanup);
        }

        /* since the augment node is not present in the compiled tree, we need to pass some of its
         * statements to all its children */
        for (i = 0; i < child_set.count; ++i) {
            node = child_set.snodes[i];
            if (!allow_mandatory && (node->flags & LYS_CONFIG_W) && (node->flags & LYS_MAND_TRUE)) {
                node->flags &= ~LYS_MAND_TRUE;
                lys_compile_mandatory_parents(target, 0);
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SEMANTICS,
                        "Invalid augment adding mandatory node \"%s\" without making it conditional via when statement.", node->name);
                ret = LY_EVALID;
                goto cleanup;
            }

            if (aug_p->when) {
                /* pass augment's when to all the children */
                ret = lys_compile_when(ctx, aug_p->when, aug_p->flags, lysc_xpath_context(target), node, &when_shared);
                LY_CHECK_GOTO(ret, cleanup);
            }
        }
        ly_set_erase(&child_set, NULL);
    }

    switch (target->nodetype) {
    case LYS_CONTAINER:
        actions = &((struct lysc_node_container *)target)->actions;
        notifs = &((struct lysc_node_container *)target)->notifs;
        break;
    case LYS_LIST:
        actions = &((struct lysc_node_list *)target)->actions;
        notifs = &((struct lysc_node_list *)target)->notifs;
        break;
    default:
        actions = NULL;
        notifs = NULL;
        break;
    }

    if (aug_p->actions) {
        if (!actions) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                    "Invalid augment of %s node which is not allowed to contain RPC/action node \"%s\".",
                    lys_nodetype2str(target->nodetype), aug_p->actions[0].name);
            ret = LY_EVALID;
            goto cleanup;
        }

        /* compile actions into the target */
        COMPILE_OP_ARRAY_GOTO(ctx, aug_p->actions, *actions, target, u, lys_compile_action, 0, ret, cleanup);

        if (aug_p->when) {
            /* inherit when */
            LY_ARRAY_FOR(*actions, u) {
                ret = lys_compile_when(ctx, aug_p->when, aug_p->flags, lysc_xpath_context(target),
                        (struct lysc_node *)&(*actions)[u], &when_shared);
                LY_CHECK_GOTO(ret, cleanup);
            }
        }
    }
    if (aug_p->notifs) {
        if (!notifs) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                    "Invalid augment of %s node which is not allowed to contain notification node \"%s\".",
                    lys_nodetype2str(target->nodetype), aug_p->notifs[0].name);
            ret = LY_EVALID;
            goto cleanup;
        }

        /* compile notifications into the target */
        COMPILE_OP_ARRAY_GOTO(ctx, aug_p->notifs, *notifs, target, u, lys_compile_notif, 0, ret, cleanup);

        if (aug_p->when) {
            /* inherit when */
            LY_ARRAY_FOR(*notifs, u) {
                ret = lys_compile_when(ctx, aug_p->when, aug_p->flags, lysc_xpath_context(target),
                        (struct lysc_node *)&(*notifs)[u], &when_shared);
                LY_CHECK_GOTO(ret, cleanup);
            }
        }
    }

cleanup:
    ly_set_erase(&child_set, NULL);
    return ret;
}

/**
 * @brief Find grouping for a uses.
 *
 * @param[in] ctx Compile context.
 * @param[in] uses_p Parsed uses node.
 * @param[out] gpr_p Found grouping on success.
 * @param[out] grp_mod Module of @p grp_p on success.
 * @return LY_ERR value.
 */
static LY_ERR
lys_compile_uses_find_grouping(struct lysc_ctx *ctx, struct lysp_node_uses *uses_p, struct lysp_grp **grp_p,
        struct lys_module **grp_mod)
{
    struct lysp_node *pnode;
    struct lysp_grp *grp;
    LY_ARRAY_COUNT_TYPE u, v;
    ly_bool found = 0;
    const char *id, *name, *prefix;
    size_t prefix_len, name_len;
    struct lys_module *mod;

    *grp_p = NULL;
    *grp_mod = NULL;

    /* search for the grouping definition */
    id = uses_p->name;
    LY_CHECK_RET(ly_parse_nodeid(&id, &prefix, &prefix_len, &name, &name_len), LY_EVALID);
    if (prefix) {
        mod = lys_module_find_prefix(ctx->mod_def, prefix, prefix_len);
        if (!mod) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                    "Invalid prefix used for grouping reference.", uses_p->name);
            return LY_EVALID;
        }
    } else {
        mod = ctx->mod_def;
    }
    if (mod == ctx->mod_def) {
        for (pnode = uses_p->parent; !found && pnode; pnode = pnode->parent) {
            grp = (struct lysp_grp *)lysp_node_groupings(pnode);
            LY_ARRAY_FOR(grp, u) {
                if (!strcmp(grp[u].name, name)) {
                    grp = &grp[u];
                    found = 1;
                    break;
                }
            }
        }
    }
    if (!found) {
        /* search in top-level groupings of the main module ... */
        grp = mod->parsed->groupings;
        LY_ARRAY_FOR(grp, u) {
            if (!strcmp(grp[u].name, name)) {
                grp = &grp[u];
                found = 1;
                break;
            }
        }
        if (!found) {
            /* ... and all the submodules */
            LY_ARRAY_FOR(mod->parsed->includes, u) {
                grp = mod->parsed->includes[u].submodule->groupings;
                LY_ARRAY_FOR(grp, v) {
                    if (!strcmp(grp[v].name, name)) {
                        grp = &grp[v];
                        found = 1;
                        break;
                    }
                }
                if (found) {
                    break;
                }
            }
        }
    }
    if (!found) {
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SEMANTICS,
                "Grouping \"%s\" referenced by a uses statement not found.", uses_p->name);
        return LY_EVALID;
    }

    if (!(ctx->options & LYS_COMPILE_GROUPING)) {
        /* remember that the grouping is instantiated to avoid its standalone validation */
        grp->flags |= LYS_USED_GRP;
    }

    *grp_p = grp;
    *grp_mod = mod;
    return LY_SUCCESS;
}

static const struct lys_module *lys_schema_node_get_module(const struct ly_ctx *ctx, const char *nametest,
        size_t nametest_len, const struct lys_module *local_mod, const char **name, size_t *name_len);

static LY_ERR
lys_nodeid_check(struct lysc_ctx *ctx, const char *nodeid, ly_bool abs, struct lys_module **target_mod,
        struct lyxp_expr **expr)
{
    LY_ERR ret = LY_SUCCESS;
    struct lyxp_expr *e = NULL;
    struct lys_module *tmod = NULL, *mod;
    const char *nodeid_type = abs ? "absolute-schema-nodeid" : "descendant-schema-nodeid";
    uint32_t i;

    /* parse */
    ret = lyxp_expr_parse(ctx->ctx, nodeid, strlen(nodeid), 0, &e);
    if (ret) {
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG, "Invalid %s value \"%s\" - invalid syntax.",
                nodeid_type, nodeid);
        ret = LY_EVALID;
        goto cleanup;
    }

    if (abs) {
        /* absolute schema nodeid */
        i = 0;
    } else {
        /* descendant schema nodeid */
        if (e->tokens[0] != LYXP_TOKEN_NAMETEST) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE, "Invalid %s value \"%s\" - name test expected instead of \"%.*s\".",
                    nodeid_type, nodeid, e->tok_len[0], e->expr + e->tok_pos[0]);
            ret = LY_EVALID;
            goto cleanup;
        }
        i = 1;
    }

    /* check all the tokens */
    for ( ; i < e->used; i += 2) {
        if (e->tokens[i] != LYXP_TOKEN_OPER_PATH) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE, "Invalid %s value \"%s\" - \"/\" expected instead of \"%.*s\".",
                    nodeid_type, nodeid, e->tok_len[i], e->expr + e->tok_pos[i]);
            ret = LY_EVALID;
            goto cleanup;
        } else if (e->used == i + 1) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                    "Invalid %s value \"%s\" - unexpected end of expression.", nodeid_type, e->expr);
            ret = LY_EVALID;
            goto cleanup;
        } else if (e->tokens[i + 1] != LYXP_TOKEN_NAMETEST) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE, "Invalid %s value \"%s\" - name test expected instead of \"%.*s\".",
                    nodeid_type, nodeid, e->tok_len[i + 1], e->expr + e->tok_pos[i + 1]);
            ret = LY_EVALID;
            goto cleanup;
        } else if (abs) {
            mod = (struct lys_module *)lys_schema_node_get_module(ctx->ctx, e->expr + e->tok_pos[i + 1],
                    e->tok_len[i + 1], ctx->mod_def, NULL, NULL);
            LY_CHECK_ERR_GOTO(!mod, ret = LY_EVALID, cleanup);

            /* only keep the first module */
            if (!tmod) {
                tmod = mod;
            }

            /* all the modules must be implemented */
            if (!mod->implemented) {
                ret = lys_set_implemented(mod);
                LY_CHECK_GOTO(ret, cleanup);
            }
        }
    }

cleanup:
    if (ret || !expr) {
        lyxp_expr_free(ctx->ctx, e);
        e = NULL;
    }
    if (expr) {
        *expr = ret ? NULL : e;
    }
    if (target_mod) {
        *target_mod = ret ? NULL : tmod;
    }
    return ret;
}

/**
 * @brief Check whether 2 schema nodeids match.
 *
 * @param[in] ctx libyang context.
 * @param[in] exp1 First schema nodeid.
 * @param[in] exp1_mod Module of @p exp1 nodes without any prefix.
 * @param[in] exp2 Second schema nodeid.
 * @param[in] exp2_mod Module of @p exp2 nodes without any prefix.
 * @return Whether the schema nodeids match or not.
 */
static ly_bool
lys_abs_schema_nodeid_match(const struct ly_ctx *ctx, const struct lyxp_expr *exp1, const struct lys_module *exp1_mod,
        const struct lyxp_expr *exp2, const struct lys_module *exp2_mod)
{
    uint32_t i;
    const struct lys_module *mod1, *mod2;
    const char *name1, *name2;
    size_t name1_len, name2_len;

    if (exp1->used != exp2->used) {
        return 0;
    }

    for (i = 0; i < exp1->used; ++i) {
        assert(exp1->tokens[i] == exp2->tokens[i]);

        if (exp1->tokens[i] == LYXP_TOKEN_NAMETEST) {
            /* check modules of all the nodes in the node ID */
            mod1 = lys_schema_node_get_module(ctx, exp1->expr + exp1->tok_pos[i], exp1->tok_len[i], exp1_mod,
                    &name1, &name1_len);
            assert(mod1);
            mod2 = lys_schema_node_get_module(ctx, exp2->expr + exp2->tok_pos[i], exp2->tok_len[i], exp2_mod,
                    &name2, &name2_len);
            assert(mod2);

            /* compare modules */
            if (mod1 != mod2) {
                return 0;
            }

            /* compare names */
            if ((name1_len != name2_len) || strncmp(name1, name2, name1_len)) {
                return 0;
            }
        }
    }

    return 1;
}

/**
 * @brief Prepare any uses augments and refines in the context to be applied during uses descendant node compilation.
 *
 * @param[in] ctx Compile context.
 * @param[in] uses_p Parsed uses structure with augments and refines.
 * @param[in] ctx_node Context node of @p uses_p meaning its first data definiition parent.
 * @return LY_ERR value.
 */
static LY_ERR
lys_precompile_uses_augments_refines(struct lysc_ctx *ctx, struct lysp_node_uses *uses_p, const struct lysc_node *ctx_node)
{
    LY_ERR ret = LY_SUCCESS;
    LY_ARRAY_COUNT_TYPE u;
    struct lyxp_expr *exp = NULL;
    struct lysc_augment *aug;
    struct lysc_refine *rfn;
    struct lysp_refine **new_rfn;
    uint32_t i;

    LY_ARRAY_FOR(uses_p->augments, u) {
        lysc_update_path(ctx, NULL, "{augment}");
        lysc_update_path(ctx, NULL, uses_p->augments[u].nodeid);

        /* parse the nodeid */
        LY_CHECK_GOTO(ret = lys_nodeid_check(ctx, uses_p->augments[u].nodeid, 0, NULL, &exp), cleanup);

        /* allocate new compiled augment and store it in the set */
        aug = calloc(1, sizeof *aug);
        LY_CHECK_ERR_GOTO(!aug, LOGMEM(ctx->ctx); ret = LY_EMEM, cleanup);
        LY_CHECK_GOTO(ret = ly_set_add(&ctx->uses_augs, aug, 1, NULL), cleanup);

        aug->nodeid = exp;
        exp = NULL;
        aug->nodeid_ctx_node = ctx_node;
        aug->aug_p = &uses_p->augments[u];

        lysc_update_path(ctx, NULL, NULL);
        lysc_update_path(ctx, NULL, NULL);
    }

    LY_ARRAY_FOR(uses_p->refines, u) {
        lysc_update_path(ctx, NULL, "{refine}");
        lysc_update_path(ctx, NULL, uses_p->refines[u].nodeid);

        /* parse the nodeid */
        LY_CHECK_GOTO(ret = lys_nodeid_check(ctx, uses_p->refines[u].nodeid, 0, NULL, &exp), cleanup);

        /* try to find the node in already compiled refines */
        rfn = NULL;
        for (i = 0; i < ctx->uses_rfns.count; ++i) {
            if (lys_abs_schema_nodeid_match(ctx->ctx, exp, ctx->mod_def, ((struct lysc_refine *)ctx->uses_rfns.objs[i])->nodeid,
                    ctx->mod_def)) {
                rfn = ctx->uses_rfns.objs[i];
                break;
            }
        }

        if (!rfn) {
            /* allocate new compiled refine */
            rfn = calloc(1, sizeof *rfn);
            LY_CHECK_ERR_GOTO(!rfn, LOGMEM(ctx->ctx); ret = LY_EMEM, cleanup);
            LY_CHECK_GOTO(ret = ly_set_add(&ctx->uses_rfns, rfn, 1, NULL), cleanup);

            rfn->nodeid = exp;
            exp = NULL;
            rfn->nodeid_ctx_node = ctx_node;
        } else {
            /* just free exp */
            lyxp_expr_free(ctx->ctx, exp);
            exp = NULL;
        }

        /* add new parsed refine structure */
        LY_ARRAY_NEW_GOTO(ctx->ctx, rfn->rfns, new_rfn, ret, cleanup);
        *new_rfn = &uses_p->refines[u];

        lysc_update_path(ctx, NULL, NULL);
        lysc_update_path(ctx, NULL, NULL);
    }

cleanup:
    lyxp_expr_free(ctx->ctx, exp);
    return ret;
}

/**
 * @brief Compile parsed uses statement - resolve target grouping and connect its content into parent.
 * If present, also apply uses's modificators.
 *
 * @param[in] ctx Compile context
 * @param[in] uses_p Parsed uses schema node.
 * @param[in] parent Compiled parent node where the content of the referenced grouping is supposed to be connected. It is
 * NULL for top-level nodes, in such a case the module where the node will be connected is taken from
 * the compile context.
 * @return LY_ERR value - LY_SUCCESS or LY_EVALID.
 */
static LY_ERR
lys_compile_uses(struct lysc_ctx *ctx, struct lysp_node_uses *uses_p, struct lysc_node *parent, struct ly_set *child_set)
{
    struct lysp_node *pnode;
    struct lysc_node *child;
    struct lysp_grp *grp = NULL;
    uint32_t i, grp_stack_count;
    struct lys_module *grp_mod, *mod_old = ctx->mod_def;
    LY_ERR ret = LY_SUCCESS;
    struct lysc_when *when_shared = NULL;
    LY_ARRAY_COUNT_TYPE u;
    struct lysc_notif **notifs = NULL;
    struct lysc_action **actions = NULL;
    struct ly_set uses_child_set = {0};

    /* find the referenced grouping */
    LY_CHECK_RET(lys_compile_uses_find_grouping(ctx, uses_p, &grp, &grp_mod));

    /* grouping must not reference themselves - stack in ctx maintains list of groupings currently being applied */
    grp_stack_count = ctx->groupings.count;
    LY_CHECK_RET(ly_set_add(&ctx->groupings, (void *)grp, 0, NULL));
    if (grp_stack_count == ctx->groupings.count) {
        /* the target grouping is already in the stack, so we are already inside it -> circular dependency */
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                "Grouping \"%s\" references itself through a uses statement.", grp->name);
        return LY_EVALID;
    }

    /* check status */
    ret = lysc_check_status(ctx, uses_p->flags, ctx->mod_def, uses_p->name, grp->flags, grp_mod, grp->name);
    LY_CHECK_GOTO(ret, cleanup);

    /* compile any augments and refines so they can be applied during the grouping nodes compilation */
    ret = lys_precompile_uses_augments_refines(ctx, uses_p, parent);
    LY_CHECK_GOTO(ret, cleanup);

    /* switch context's mod_def */
    ctx->mod_def = grp_mod;

    /* compile data nodes */
    LY_LIST_FOR(grp->data, pnode) {
        /* 0x3 in uses_status is a special bits combination to be able to detect status flags from uses */
        ret = lys_compile_node(ctx, pnode, parent, (uses_p->flags & LYS_STATUS_MASK) | 0x3, &uses_child_set);
        LY_CHECK_GOTO(ret, cleanup);
    }

    if (child_set) {
        /* add these children to our compiled child_set as well since uses is a schema-only node */
        LY_CHECK_GOTO(ret = ly_set_merge(child_set, &uses_child_set, 1, NULL), cleanup);
    }

    if (uses_p->when) {
        /* pass uses's when to all the data children */
        for (i = 0; i < uses_child_set.count; ++i) {
            child = uses_child_set.snodes[i];

            ret = lys_compile_when(ctx, uses_p->when, uses_p->flags, lysc_xpath_context(parent), child, &when_shared);
            LY_CHECK_GOTO(ret, cleanup);
        }
    }

    /* compile actions */
    if (grp->actions) {
        actions = parent ? lysc_node_actions_p(parent) : &ctx->mod->compiled->rpcs;
        if (!actions) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE, "Invalid child %s \"%s\" of uses parent %s \"%s\" node.",
                    grp->actions[0].name, lys_nodetype2str(grp->actions[0].nodetype), parent->name,
                    lys_nodetype2str(parent->nodetype));
            ret = LY_EVALID;
            goto cleanup;
        }
        COMPILE_OP_ARRAY_GOTO(ctx, grp->actions, *actions, parent, u, lys_compile_action, 0, ret, cleanup);

        if (uses_p->when) {
            /* inherit when */
            LY_ARRAY_FOR(*actions, u) {
                ret = lys_compile_when(ctx, uses_p->when, uses_p->flags, lysc_xpath_context(parent),
                        (struct lysc_node *)&(*actions)[u], &when_shared);
                LY_CHECK_GOTO(ret, cleanup);
            }
        }
    }

    /* compile notifications */
    if (grp->notifs) {
        notifs = parent ? lysc_node_notifs_p(parent) : &ctx->mod->compiled->notifs;
        if (!notifs) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE, "Invalid child %s \"%s\" of uses parent %s \"%s\" node.",
                    grp->notifs[0].name, lys_nodetype2str(grp->notifs[0].nodetype), parent->name,
                    lys_nodetype2str(parent->nodetype));
            ret = LY_EVALID;
            goto cleanup;
        }
        COMPILE_OP_ARRAY_GOTO(ctx, grp->notifs, *notifs, parent, u, lys_compile_notif, 0, ret, cleanup);

        if (uses_p->when) {
            /* inherit when */
            LY_ARRAY_FOR(*notifs, u) {
                ret = lys_compile_when(ctx, uses_p->when, uses_p->flags, lysc_xpath_context(parent),
                        (struct lysc_node *)&(*notifs)[u], &when_shared);
                LY_CHECK_GOTO(ret, cleanup);
            }
        }
    }

    /* check that all augments were applied */
    for (i = 0; i < ctx->uses_augs.count; ++i) {
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                "Augment target node \"%s\" in grouping \"%s\" was not found.",
                ((struct lysc_augment *)ctx->uses_augs.objs[i])->nodeid->expr, grp->name);
        ret = LY_ENOTFOUND;
    }
    LY_CHECK_GOTO(ret, cleanup);

    /* check that all refines were applied */
    for (i = 0; i < ctx->uses_rfns.count; ++i) {
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                "Refine(s) target node \"%s\" in grouping \"%s\" was not found.",
                ((struct lysc_refine *)ctx->uses_rfns.objs[i])->nodeid->expr, grp->name);
        ret = LY_ENOTFOUND;
    }
    LY_CHECK_GOTO(ret, cleanup);

cleanup:
    /* reload previous context's mod_def */
    ctx->mod_def = mod_old;

    /* remove the grouping from the stack for circular groupings dependency check */
    ly_set_rm_index(&ctx->groupings, ctx->groupings.count - 1, NULL);
    assert(ctx->groupings.count == grp_stack_count);

    ly_set_erase(&uses_child_set, NULL);
    return ret;
}

static int
lys_compile_grouping_pathlog(struct lysc_ctx *ctx, struct lysp_node *node, char **path)
{
    struct lysp_node *iter;
    int len = 0;

    *path = NULL;
    for (iter = node; iter && len >= 0; iter = iter->parent) {
        char *s = *path;
        char *id;

        switch (iter->nodetype) {
        case LYS_USES:
            LY_CHECK_RET(asprintf(&id, "{uses='%s'}", iter->name) == -1, -1);
            break;
        case LYS_GROUPING:
            LY_CHECK_RET(asprintf(&id, "{grouping='%s'}", iter->name) == -1, -1);
            break;
        case LYS_AUGMENT:
            LY_CHECK_RET(asprintf(&id, "{augment='%s'}", iter->name) == -1, -1);
            break;
        default:
            id = strdup(iter->name);
            break;
        }

        if (!iter->parent) {
            /* print prefix */
            len = asprintf(path, "/%s:%s%s", ctx->mod->name, id, s ? s : "");
        } else {
            /* prefix is the same as in parent */
            len = asprintf(path, "/%s%s", id, s ? s : "");
        }
        free(s);
        free(id);
    }

    if (len < 0) {
        free(*path);
        *path = NULL;
    } else if (len == 0) {
        *path = strdup("/");
        len = 1;
    }
    return len;
}

/**
 * @brief Validate groupings that were defined but not directly used in the schema itself.
 *
 * The grouping does not need to be compiled (and it is compiled here, but the result is forgotten immediately),
 * but to have the complete result of the schema validity, even such groupings are supposed to be checked.
 */
static LY_ERR
lys_compile_grouping(struct lysc_ctx *ctx, struct lysp_node *pnode, struct lysp_grp *grp)
{
    LY_ERR ret;
    char *path;
    int len;

    struct lysp_node_uses fake_uses = {
        .parent = pnode,
        .nodetype = LYS_USES,
        .flags = 0, .next = NULL,
        .name = grp->name,
        .dsc = NULL, .ref = NULL, .when = NULL, .iffeatures = NULL, .exts = NULL,
        .refines = NULL, .augments = NULL
    };
    struct lysc_node_container fake_container = {
        .nodetype = LYS_CONTAINER,
        .flags = pnode ? (pnode->flags & LYS_FLAGS_COMPILED_MASK) : 0,
        .module = ctx->mod,
        .sp = NULL, .parent = NULL, .next = NULL,
        .prev = (struct lysc_node *)&fake_container,
        .name = "fake",
        .dsc = NULL, .ref = NULL, .exts = NULL, .iffeatures = NULL, .when = NULL,
        .child = NULL, .musts = NULL, .actions = NULL, .notifs = NULL
    };

    if (grp->parent) {
        LOGWRN(ctx->ctx, "Locally scoped grouping \"%s\" not used.", grp->name);
    }

    len = lys_compile_grouping_pathlog(ctx, grp->parent, &path);
    if (len < 0) {
        LOGMEM(ctx->ctx);
        return LY_EMEM;
    }
    strncpy(ctx->path, path, LYSC_CTX_BUFSIZE - 1);
    ctx->path_len = (uint32_t)len;
    free(path);

    lysc_update_path(ctx, NULL, "{grouping}");
    lysc_update_path(ctx, NULL, grp->name);
    ret = lys_compile_uses(ctx, &fake_uses, (struct lysc_node *)&fake_container, NULL);
    lysc_update_path(ctx, NULL, NULL);
    lysc_update_path(ctx, NULL, NULL);

    ctx->path_len = 1;
    ctx->path[1] = '\0';

    /* cleanup */
    lysc_node_container_free(ctx->ctx, &fake_container);

    return ret;
}

/**
 * @brief Set config flags for a node.
 *
 * @param[in] ctx Compile context.
 * @param[in] node Compiled node config to set.
 * @param[in] parent Parent of @p node.
 * @return LY_ERR value.
 */
static LY_ERR
lys_compile_config(struct lysc_ctx *ctx, struct lysc_node *node, struct lysc_node *parent)
{
    if (node->nodetype == LYS_CASE) {
        /* case never has any config */
        assert(!(node->flags & LYS_CONFIG_MASK));
        return LY_SUCCESS;
    }

    /* adjust parent to always get the ancestor with config */
    if (parent && (parent->nodetype == LYS_CASE)) {
        parent = parent->parent;
        assert(parent);
    }

    if (ctx->options & (LYS_COMPILE_RPC_INPUT | LYS_COMPILE_RPC_OUTPUT)) {
        /* ignore config statements inside RPC/action data */
        node->flags &= ~LYS_CONFIG_MASK;
        node->flags |= (ctx->options & LYS_COMPILE_RPC_INPUT) ? LYS_CONFIG_W : LYS_CONFIG_R;
    } else if (ctx->options & LYS_COMPILE_NOTIFICATION) {
        /* ignore config statements inside Notification data */
        node->flags &= ~LYS_CONFIG_MASK;
        node->flags |= LYS_CONFIG_R;
    } else if (!(node->flags & LYS_CONFIG_MASK)) {
        /* config not explicitely set, inherit it from parent */
        if (parent) {
            node->flags |= parent->flags & LYS_CONFIG_MASK;
        } else {
            /* default is config true */
            node->flags |= LYS_CONFIG_W;
        }
    } else {
        /* config set explicitely */
        node->flags |= LYS_SET_CONFIG;
    }

    if (parent && (parent->flags & LYS_CONFIG_R) && (node->flags & LYS_CONFIG_W)) {
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SEMANTICS,
                "Configuration node cannot be child of any state data node.");
        return LY_EVALID;
    }

    return LY_SUCCESS;
}

static LY_ERR
lysp_ext_dup(const struct ly_ctx *ctx, struct lysp_ext_instance *ext, const struct lysp_ext_instance *orig_ext)
{
    LY_ERR ret = LY_SUCCESS;

    *ext = *orig_ext;
    DUP_STRING(ctx, orig_ext->name, ext->name, ret);
    DUP_STRING(ctx, orig_ext->argument, ext->argument, ret);

    return ret;
}

static LY_ERR
lysp_restr_dup(const struct ly_ctx *ctx, struct lysp_restr *restr, const struct lysp_restr *orig_restr)
{
    LY_ERR ret = LY_SUCCESS;

    if (orig_restr) {
        DUP_STRING(ctx, orig_restr->arg.str, restr->arg.str, ret);
        restr->arg.mod = orig_restr->arg.mod;
        DUP_STRING(ctx, orig_restr->emsg, restr->emsg, ret);
        DUP_STRING(ctx, orig_restr->eapptag, restr->eapptag, ret);
        DUP_STRING(ctx, orig_restr->dsc, restr->dsc, ret);
        DUP_STRING(ctx, orig_restr->ref, restr->ref, ret);
        DUP_ARRAY(ctx, orig_restr->exts, restr->exts, lysp_ext_dup);
    }

    return ret;
}

static LY_ERR
lysp_string_dup(const struct ly_ctx *ctx, const char **str, const char **orig_str)
{
    LY_ERR ret = LY_SUCCESS;

    DUP_STRING(ctx, *orig_str, *str, ret);

    return ret;
}

static LY_ERR
lysp_qname_dup(const struct ly_ctx *ctx, struct lysp_qname *qname, const struct lysp_qname *orig_qname)
{
    LY_ERR ret = LY_SUCCESS;

    if (!orig_qname->str) {
        return LY_SUCCESS;
    }

    DUP_STRING(ctx, orig_qname->str, qname->str, ret);
    assert(orig_qname->mod);
    qname->mod = orig_qname->mod;

    return ret;
}

static LY_ERR
lysp_type_enum_dup(const struct ly_ctx *ctx, struct lysp_type_enum *enm, const struct lysp_type_enum *orig_enm)
{
    LY_ERR ret = LY_SUCCESS;

    DUP_STRING(ctx, orig_enm->name, enm->name, ret);
    DUP_STRING(ctx, orig_enm->dsc, enm->dsc, ret);
    DUP_STRING(ctx, orig_enm->ref, enm->ref, ret);
    enm->value = orig_enm->value;
    DUP_ARRAY(ctx, orig_enm->iffeatures, enm->iffeatures, lysp_qname_dup);
    DUP_ARRAY(ctx, orig_enm->exts, enm->exts, lysp_ext_dup);
    enm->flags = orig_enm->flags;

    return ret;
}

static LY_ERR
lysp_type_dup(const struct ly_ctx *ctx, struct lysp_type *type, const struct lysp_type *orig_type)
{
    LY_ERR ret = LY_SUCCESS;

    DUP_STRING_GOTO(ctx, orig_type->name, type->name, ret, done);

    if (orig_type->range) {
        type->range = calloc(1, sizeof *type->range);
        LY_CHECK_ERR_RET(!type->range, LOGMEM(ctx), LY_EMEM);
        LY_CHECK_RET(lysp_restr_dup(ctx, type->range, orig_type->range));
    }

    if (orig_type->length) {
        type->length = calloc(1, sizeof *type->length);
        LY_CHECK_ERR_RET(!type->length, LOGMEM(ctx), LY_EMEM);
        LY_CHECK_RET(lysp_restr_dup(ctx, type->length, orig_type->length));
    }

    DUP_ARRAY(ctx, orig_type->patterns, type->patterns, lysp_restr_dup);
    DUP_ARRAY(ctx, orig_type->enums, type->enums, lysp_type_enum_dup);
    DUP_ARRAY(ctx, orig_type->bits, type->bits, lysp_type_enum_dup);
    LY_CHECK_GOTO(ret = lyxp_expr_dup(ctx, orig_type->path, &type->path), done);
    DUP_ARRAY(ctx, orig_type->bases, type->bases, lysp_string_dup);
    DUP_ARRAY(ctx, orig_type->types, type->types, lysp_type_dup);
    DUP_ARRAY(ctx, orig_type->exts, type->exts, lysp_ext_dup);

    type->mod = orig_type->mod;
    type->compiled = orig_type->compiled;

    type->fraction_digits = orig_type->fraction_digits;
    type->require_instance = orig_type->require_instance;
    type->flags = orig_type->flags;

done:
    return ret;
}

static LY_ERR
lysp_when_dup(const struct ly_ctx *ctx, struct lysp_when *when, const struct lysp_when *orig_when)
{
    LY_ERR ret = LY_SUCCESS;

    DUP_STRING(ctx, orig_when->cond, when->cond, ret);
    DUP_STRING(ctx, orig_when->dsc, when->dsc, ret);
    DUP_STRING(ctx, orig_when->ref, when->ref, ret);
    DUP_ARRAY(ctx, orig_when->exts, when->exts, lysp_ext_dup);

    return ret;
}

static LY_ERR
lysp_node_common_dup(const struct ly_ctx *ctx, struct lysp_node *node, const struct lysp_node *orig)
{
    LY_ERR ret = LY_SUCCESS;

    node->parent = NULL;
    node->nodetype = orig->nodetype;
    node->flags = orig->flags;
    node->next = NULL;
    DUP_STRING(ctx, orig->name, node->name, ret);
    DUP_STRING(ctx, orig->dsc, node->dsc, ret);
    DUP_STRING(ctx, orig->ref, node->ref, ret);

    if (orig->when) {
        node->when = calloc(1, sizeof *node->when);
        LY_CHECK_ERR_RET(!node->when, LOGMEM(ctx), LY_EMEM);
        LY_CHECK_RET(lysp_when_dup(ctx, node->when, orig->when));
    }

    DUP_ARRAY(ctx, orig->iffeatures, node->iffeatures, lysp_qname_dup);
    DUP_ARRAY(ctx, orig->exts, node->exts, lysp_ext_dup);

    return ret;
}

static LY_ERR
lysp_node_dup(const struct ly_ctx *ctx, struct lysp_node *node, const struct lysp_node *orig)
{
    LY_ERR ret = LY_SUCCESS;
    struct lysp_node_container *cont;
    const struct lysp_node_container *orig_cont;
    struct lysp_node_leaf *leaf;
    const struct lysp_node_leaf *orig_leaf;
    struct lysp_node_leaflist *llist;
    const struct lysp_node_leaflist *orig_llist;
    struct lysp_node_list *list;
    const struct lysp_node_list *orig_list;
    struct lysp_node_choice *choice;
    const struct lysp_node_choice *orig_choice;
    struct lysp_node_case *cas;
    const struct lysp_node_case *orig_cas;
    struct lysp_node_anydata *any;
    const struct lysp_node_anydata *orig_any;

    assert(orig->nodetype & (LYS_CONTAINER | LYS_LEAF | LYS_LEAFLIST | LYS_LIST | LYS_CHOICE | LYS_CASE | LYS_ANYDATA));

    /* common part */
    LY_CHECK_RET(lysp_node_common_dup(ctx, node, orig));

    /* specific part */
    switch (node->nodetype) {
    case LYS_CONTAINER:
        cont = (struct lysp_node_container *)node;
        orig_cont = (const struct lysp_node_container *)orig;

        DUP_ARRAY(ctx, orig_cont->musts, cont->musts, lysp_restr_dup);
        DUP_STRING(ctx, orig_cont->presence, cont->presence, ret);
        /* we do not need the rest */
        break;
    case LYS_LEAF:
        leaf = (struct lysp_node_leaf *)node;
        orig_leaf = (const struct lysp_node_leaf *)orig;

        DUP_ARRAY(ctx, orig_leaf->musts, leaf->musts, lysp_restr_dup);
        LY_CHECK_RET(lysp_type_dup(ctx, &leaf->type, &orig_leaf->type));
        DUP_STRING(ctx, orig_leaf->units, leaf->units, ret);
        LY_CHECK_RET(lysp_qname_dup(ctx, &leaf->dflt, &orig_leaf->dflt));
        break;
    case LYS_LEAFLIST:
        llist = (struct lysp_node_leaflist *)node;
        orig_llist = (const struct lysp_node_leaflist *)orig;

        DUP_ARRAY(ctx, orig_llist->musts, llist->musts, lysp_restr_dup);
        LY_CHECK_RET(lysp_type_dup(ctx, &llist->type, &orig_llist->type));
        DUP_STRING(ctx, orig_llist->units, llist->units, ret);
        DUP_ARRAY(ctx, orig_llist->dflts, llist->dflts, lysp_qname_dup);
        llist->min = orig_llist->min;
        llist->max = orig_llist->max;
        break;
    case LYS_LIST:
        list = (struct lysp_node_list *)node;
        orig_list = (const struct lysp_node_list *)orig;

        DUP_ARRAY(ctx, orig_list->musts, list->musts, lysp_restr_dup);
        DUP_STRING(ctx, orig_list->key, list->key, ret);
        /* we do not need these arrays */
        DUP_ARRAY(ctx, orig_list->uniques, list->uniques, lysp_qname_dup);
        list->min = orig_list->min;
        list->max = orig_list->max;
        break;
    case LYS_CHOICE:
        choice = (struct lysp_node_choice *)node;
        orig_choice = (const struct lysp_node_choice *)orig;

        /* we do not need children */
        LY_CHECK_RET(lysp_qname_dup(ctx, &choice->dflt, &orig_choice->dflt));
        break;
    case LYS_CASE:
        cas = (struct lysp_node_case *)node;
        orig_cas = (const struct lysp_node_case *)orig;

        /* we do not need children */
        (void)cas;
        (void)orig_cas;
        break;
    case LYS_ANYDATA:
    case LYS_ANYXML:
        any = (struct lysp_node_anydata *)node;
        orig_any = (const struct lysp_node_anydata *)orig;

        DUP_ARRAY(ctx, orig_any->musts, any->musts, lysp_restr_dup);
        break;
    default:
        LOGINT_RET(ctx);
    }

    return ret;
}

static LY_ERR
lysp_action_inout_dup(const struct ly_ctx *ctx, struct lysp_action_inout *inout, const struct lysp_action_inout *orig)
{
    inout->parent = NULL;
    inout->nodetype = orig->nodetype;
    DUP_ARRAY(ctx, orig->musts, inout->musts, lysp_restr_dup);
    /* we dot need these arrays */
    DUP_ARRAY(ctx, orig->exts, inout->exts, lysp_ext_dup);

    return LY_SUCCESS;
}

static LY_ERR
lysp_action_dup(const struct ly_ctx *ctx, struct lysp_action *act, const struct lysp_action *orig)
{
    LY_ERR ret = LY_SUCCESS;

    act->parent = NULL;
    act->nodetype = orig->nodetype;
    act->flags = orig->flags;
    DUP_STRING(ctx, orig->name, act->name, ret);
    DUP_STRING(ctx, orig->dsc, act->dsc, ret);
    DUP_STRING(ctx, orig->ref, act->ref, ret);
    DUP_ARRAY(ctx, orig->iffeatures, act->iffeatures, lysp_qname_dup);

    act->input.nodetype = orig->input.nodetype;
    act->output.nodetype = orig->output.nodetype;
    /* we do not need choldren of in/out */
    DUP_ARRAY(ctx, orig->exts, act->exts, lysp_ext_dup);

    return ret;
}

static LY_ERR
lysp_notif_dup(const struct ly_ctx *ctx, struct lysp_notif *notif, const struct lysp_notif *orig)
{
    LY_ERR ret = LY_SUCCESS;

    notif->parent = NULL;
    notif->nodetype = orig->nodetype;
    notif->flags = orig->flags;
    DUP_STRING(ctx, orig->name, notif->name, ret);
    DUP_STRING(ctx, orig->dsc, notif->dsc, ret);
    DUP_STRING(ctx, orig->ref, notif->ref, ret);
    DUP_ARRAY(ctx, orig->iffeatures, notif->iffeatures, lysp_qname_dup);
    DUP_ARRAY(ctx, orig->musts, notif->musts, lysp_restr_dup);
    /* we do not need these arrays */
    DUP_ARRAY(ctx, orig->exts, notif->exts, lysp_ext_dup);

    return ret;
}

/**
 * @brief Duplicate a single parsed node. Only attributes that are used in compilation are copied.
 *
 * @param[in] ctx libyang context.
 * @param[in] pnode Node to duplicate.
 * @param[in] with_links Whether to also copy any links (child, parent pointers).
 * @param[out] dup_p Duplicated parsed node.
 * @return LY_ERR value.
 */
static LY_ERR
lysp_dup_single(const struct ly_ctx *ctx, const struct lysp_node *pnode, ly_bool with_links, struct lysp_node **dup_p)
{
    LY_ERR ret = LY_SUCCESS;
    void *mem = NULL;

    if (!pnode) {
        *dup_p = NULL;
        return LY_SUCCESS;
    }

    switch (pnode->nodetype) {
    case LYS_CONTAINER:
        mem = calloc(1, sizeof(struct lysp_node_container));
        LY_CHECK_ERR_GOTO(!mem, LOGMEM(ctx); ret = LY_EMEM, cleanup);
        LY_CHECK_GOTO(ret = lysp_node_dup(ctx, mem, pnode), cleanup);
        break;
    case LYS_LEAF:
        mem = calloc(1, sizeof(struct lysp_node_leaf));
        LY_CHECK_ERR_GOTO(!mem, LOGMEM(ctx); ret = LY_EMEM, cleanup);
        LY_CHECK_GOTO(ret = lysp_node_dup(ctx, mem, pnode), cleanup);
        break;
    case LYS_LEAFLIST:
        mem = calloc(1, sizeof(struct lysp_node_leaflist));
        LY_CHECK_ERR_GOTO(!mem, LOGMEM(ctx); ret = LY_EMEM, cleanup);
        LY_CHECK_GOTO(ret = lysp_node_dup(ctx, mem, pnode), cleanup);
        break;
    case LYS_LIST:
        mem = calloc(1, sizeof(struct lysp_node_list));
        LY_CHECK_ERR_GOTO(!mem, LOGMEM(ctx); ret = LY_EMEM, cleanup);
        LY_CHECK_GOTO(ret = lysp_node_dup(ctx, mem, pnode), cleanup);
        break;
    case LYS_CHOICE:
        mem = calloc(1, sizeof(struct lysp_node_choice));
        LY_CHECK_ERR_GOTO(!mem, LOGMEM(ctx); ret = LY_EMEM, cleanup);
        LY_CHECK_GOTO(ret = lysp_node_dup(ctx, mem, pnode), cleanup);
        break;
    case LYS_CASE:
        mem = calloc(1, sizeof(struct lysp_node_case));
        LY_CHECK_ERR_GOTO(!mem, LOGMEM(ctx); ret = LY_EMEM, cleanup);
        LY_CHECK_GOTO(ret = lysp_node_dup(ctx, mem, pnode), cleanup);
        break;
    case LYS_ANYDATA:
    case LYS_ANYXML:
        mem = calloc(1, sizeof(struct lysp_node_anydata));
        LY_CHECK_ERR_GOTO(!mem, LOGMEM(ctx); ret = LY_EMEM, cleanup);
        LY_CHECK_GOTO(ret = lysp_node_dup(ctx, mem, pnode), cleanup);
        break;
    case LYS_INPUT:
    case LYS_OUTPUT:
        mem = calloc(1, sizeof(struct lysp_action_inout));
        LY_CHECK_ERR_GOTO(!mem, LOGMEM(ctx); ret = LY_EMEM, cleanup);
        LY_CHECK_GOTO(ret = lysp_action_inout_dup(ctx, mem, (struct lysp_action_inout *)pnode), cleanup);
        break;
    case LYS_ACTION:
    case LYS_RPC:
        mem = calloc(1, sizeof(struct lysp_action));
        LY_CHECK_ERR_GOTO(!mem, LOGMEM(ctx); ret = LY_EMEM, cleanup);
        LY_CHECK_GOTO(ret = lysp_action_dup(ctx, mem, (struct lysp_action *)pnode), cleanup);
        break;
    case LYS_NOTIF:
        mem = calloc(1, sizeof(struct lysp_notif));
        LY_CHECK_ERR_GOTO(!mem, LOGMEM(ctx); ret = LY_EMEM, cleanup);
        LY_CHECK_GOTO(ret = lysp_notif_dup(ctx, mem, (struct lysp_notif *)pnode), cleanup);
        break;
    default:
        LOGINT_RET(ctx);
    }

    if (with_links) {
        /* copy also parent and child pointers */
        ((struct lysp_node *)mem)->parent = pnode->parent;
        switch (pnode->nodetype) {
        case LYS_CONTAINER:
            ((struct lysp_node_container *)mem)->child = ((struct lysp_node_container *)pnode)->child;
            break;
        case LYS_LIST:
            ((struct lysp_node_list *)mem)->child = ((struct lysp_node_list *)pnode)->child;
            break;
        case LYS_CHOICE:
            ((struct lysp_node_choice *)mem)->child = ((struct lysp_node_choice *)pnode)->child;
            break;
        case LYS_CASE:
            ((struct lysp_node_case *)mem)->child = ((struct lysp_node_case *)pnode)->child;
            break;
        default:
            break;
        }
    }

cleanup:
    if (ret) {
        free(mem);
    } else {
        *dup_p = mem;
    }
    return ret;
}

#define AMEND_WRONG_NODETYPE(AMEND_STR, OP_STR, PROPERTY) \
    LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE, "Invalid %s of %s node - it is not possible to %s \"%s\" property.", \
            AMEND_STR, lys_nodetype2str(target->nodetype), OP_STR, PROPERTY);\
    ret = LY_EVALID; \
    goto cleanup;

#define AMEND_CHECK_CARDINALITY(ARRAY, MAX, AMEND_STR, PROPERTY) \
    if (LY_ARRAY_COUNT(ARRAY) > MAX) { \
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SEMANTICS, "Invalid %s of %s with too many (%"LY_PRI_ARRAY_COUNT_TYPE") %s properties.", \
               AMEND_STR, lys_nodetype2str(target->nodetype), LY_ARRAY_COUNT(ARRAY), PROPERTY); \
        ret = LY_EVALID; \
        goto cleanup; \
    }

/**
 * @brief Apply refine.
 *
 * @param[in] ctx Compile context.
 * @param[in] rfn Refine to apply.
 * @param[in,out] target Refine target.
 * @return LY_ERR value.
 */
static LY_ERR
lys_apply_refine(struct lysc_ctx *ctx, struct lysp_refine *rfn, struct lysp_node *target)
{
    LY_ERR ret = LY_SUCCESS;
    LY_ARRAY_COUNT_TYPE u;
    struct lysp_qname *qname;
    struct lysp_restr **musts, *must;
    uint32_t *num;

    /* default value */
    if (rfn->dflts) {
        switch (target->nodetype) {
        case LYS_LEAF:
            AMEND_CHECK_CARDINALITY(rfn->dflts, 1, "refine", "default");

            FREE_STRING(ctx->ctx, ((struct lysp_node_leaf *)target)->dflt.str);
            DUP_STRING_GOTO(ctx->ctx, rfn->dflts[0], ((struct lysp_node_leaf *)target)->dflt.str, ret, cleanup);
            ((struct lysp_node_leaf *)target)->dflt.mod = ctx->mod;
            break;
        case LYS_LEAFLIST:
            if (ctx->mod->version < LYS_VERSION_1_1) {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SEMANTICS,
                        "Invalid refine of default in leaf-list - the default statement is allowed only in YANG 1.1 modules.");
                ret = LY_EVALID;
                goto cleanup;
            }

            FREE_ARRAY(ctx->ctx, ((struct lysp_node_leaflist *)target)->dflts, lysp_qname_free);
            ((struct lysp_node_leaflist *)target)->dflts = NULL;
            LY_ARRAY_FOR(rfn->dflts, u) {
                LY_ARRAY_NEW_GOTO(ctx->ctx, ((struct lysp_node_leaflist *)target)->dflts, qname, ret, cleanup);
                DUP_STRING_GOTO(ctx->ctx, rfn->dflts[u], qname->str, ret, cleanup);
                qname->mod = ctx->mod;
            }
            break;
        case LYS_CHOICE:
            AMEND_CHECK_CARDINALITY(rfn->dflts, 1, "refine", "default");

            FREE_STRING(ctx->ctx, ((struct lysp_node_choice *)target)->dflt.str);
            DUP_STRING_GOTO(ctx->ctx, rfn->dflts[0], ((struct lysp_node_choice *)target)->dflt.str, ret, cleanup);
            ((struct lysp_node_choice *)target)->dflt.mod = ctx->mod;
            break;
        default:
            AMEND_WRONG_NODETYPE("refine", "replace", "default");
        }
    }

    /* description */
    if (rfn->dsc) {
        FREE_STRING(ctx->ctx, target->dsc);
        DUP_STRING_GOTO(ctx->ctx, rfn->dsc, target->dsc, ret, cleanup);
    }

    /* reference */
    if (rfn->ref) {
        FREE_STRING(ctx->ctx, target->ref);
        DUP_STRING_GOTO(ctx->ctx, rfn->ref, target->ref, ret, cleanup);
    }

    /* config */
    if (rfn->flags & LYS_CONFIG_MASK) {
        if (ctx->options & (LYS_COMPILE_NOTIFICATION | LYS_COMPILE_RPC_INPUT | LYS_COMPILE_RPC_OUTPUT)) {
            LOGWRN(ctx->ctx, "Refining config inside %s has no effect (%s).",
                    ctx->options & LYS_COMPILE_NOTIFICATION ? "notification" : "RPC/action", ctx->path);
        } else {
            target->flags &= ~LYS_CONFIG_MASK;
            target->flags |= rfn->flags & LYS_CONFIG_MASK;
        }
    }

    /* mandatory */
    if (rfn->flags & LYS_MAND_MASK) {
        switch (target->nodetype) {
        case LYS_LEAF:
        case LYS_CHOICE:
        case LYS_ANYDATA:
        case LYS_ANYXML:
            break;
        default:
            AMEND_WRONG_NODETYPE("refine", "replace", "mandatory");
        }

        target->flags &= ~LYS_MAND_MASK;
        target->flags |= rfn->flags & LYS_MAND_MASK;
    }

    /* presence */
    if (rfn->presence) {
        switch (target->nodetype) {
        case LYS_CONTAINER:
            break;
        default:
            AMEND_WRONG_NODETYPE("refine", "replace", "presence");
        }

        FREE_STRING(ctx->ctx, ((struct lysp_node_container *)target)->presence);
        DUP_STRING_GOTO(ctx->ctx, rfn->presence, ((struct lysp_node_container *)target)->presence, ret, cleanup);
    }

    /* must */
    if (rfn->musts) {
        switch (target->nodetype) {
        case LYS_CONTAINER:
        case LYS_LIST:
        case LYS_LEAF:
        case LYS_LEAFLIST:
        case LYS_ANYDATA:
        case LYS_ANYXML:
            musts = &((struct lysp_node_container *)target)->musts;
            break;
        default:
            AMEND_WRONG_NODETYPE("refine", "add", "must");
        }

        LY_ARRAY_FOR(rfn->musts, u) {
            LY_ARRAY_NEW_GOTO(ctx->ctx, *musts, must, ret, cleanup);
            LY_CHECK_GOTO(ret = lysp_restr_dup(ctx->ctx, must, &rfn->musts[u]), cleanup);
        }
    }

    /* min-elements */
    if (rfn->flags & LYS_SET_MIN) {
        switch (target->nodetype) {
        case LYS_LEAFLIST:
            num = &((struct lysp_node_leaflist *)target)->min;
            break;
        case LYS_LIST:
            num = &((struct lysp_node_list *)target)->min;
            break;
        default:
            AMEND_WRONG_NODETYPE("refine", "replace", "min-elements");
        }

        *num = rfn->min;
    }

    /* max-elements */
    if (rfn->flags & LYS_SET_MAX) {
        switch (target->nodetype) {
        case LYS_LEAFLIST:
            num = &((struct lysp_node_leaflist *)target)->max;
            break;
        case LYS_LIST:
            num = &((struct lysp_node_list *)target)->max;
            break;
        default:
            AMEND_WRONG_NODETYPE("refine", "replace", "max-elements");
        }

        *num = rfn->max;
    }

    /* if-feature */
    if (rfn->iffeatures) {
        switch (target->nodetype) {
        case LYS_LEAF:
        case LYS_LEAFLIST:
        case LYS_LIST:
        case LYS_CONTAINER:
        case LYS_CHOICE:
        case LYS_CASE:
        case LYS_ANYDATA:
        case LYS_ANYXML:
            break;
        default:
            AMEND_WRONG_NODETYPE("refine", "add", "if-feature");
        }

        LY_ARRAY_FOR(rfn->iffeatures, u) {
            LY_ARRAY_NEW_GOTO(ctx->ctx, target->iffeatures, qname, ret, cleanup);
            DUP_STRING_GOTO(ctx->ctx, rfn->iffeatures[u].str, qname->str, ret, cleanup);
            qname->mod = ctx->mod;
        }
    }

    /* extension */
    /* TODO refine extensions */

cleanup:
    return ret;
}

/**
 * @brief Apply deviate add.
 *
 * @param[in] ctx Compile context.
 * @param[in] d Deviate add to apply.
 * @param[in,out] target Deviation target.
 * @return LY_ERR value.
 */
static LY_ERR
lys_apply_deviate_add(struct lysc_ctx *ctx, struct lysp_deviate_add *d, struct lysp_node *target)
{
    LY_ERR ret = LY_SUCCESS;
    LY_ARRAY_COUNT_TYPE u;
    struct lysp_qname *qname;
    uint32_t *num;
    struct lysp_restr **musts, *must;

#define DEV_CHECK_NONPRESENCE(TYPE, MEMBER, PROPERTY, VALUEMEMBER) \
    if (((TYPE)target)->MEMBER) { \
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE, \
                "Invalid deviation adding \"%s\" property which already exists (with value \"%s\").", \
                PROPERTY, ((TYPE)target)->VALUEMEMBER); \
        ret = LY_EVALID; \
        goto cleanup; \
    }

    /* [units-stmt] */
    if (d->units) {
        switch (target->nodetype) {
        case LYS_LEAF:
        case LYS_LEAFLIST:
            break;
        default:
            AMEND_WRONG_NODETYPE("deviation", "add", "units");
        }

        DEV_CHECK_NONPRESENCE(struct lysp_node_leaf *, units, "units", units);
        DUP_STRING_GOTO(ctx->ctx, d->units, ((struct lysp_node_leaf *)target)->units, ret, cleanup);
    }

    /* *must-stmt */
    if (d->musts) {
        switch (target->nodetype) {
        case LYS_CONTAINER:
        case LYS_LIST:
        case LYS_LEAF:
        case LYS_LEAFLIST:
        case LYS_ANYDATA:
        case LYS_ANYXML:
            musts = &((struct lysp_node_container *)target)->musts;
            break;
        case LYS_NOTIF:
            musts = &((struct lysp_notif *)target)->musts;
            break;
        case LYS_INPUT:
        case LYS_OUTPUT:
            musts = &((struct lysp_action_inout *)target)->musts;
            break;
        default:
            AMEND_WRONG_NODETYPE("deviation", "add", "must");
        }

        LY_ARRAY_FOR(d->musts, u) {
            LY_ARRAY_NEW_GOTO(ctx->ctx, *musts, must, ret, cleanup);
            LY_CHECK_GOTO(ret = lysp_restr_dup(ctx->ctx, must, &d->musts[u]), cleanup);
        }
    }

    /* *unique-stmt */
    if (d->uniques) {
        switch (target->nodetype) {
        case LYS_LIST:
            break;
        default:
            AMEND_WRONG_NODETYPE("deviation", "add", "unique");
        }

        LY_ARRAY_FOR(d->uniques, u) {
            LY_ARRAY_NEW_GOTO(ctx->ctx, ((struct lysp_node_list *)target)->uniques, qname, ret, cleanup);
            DUP_STRING_GOTO(ctx->ctx, d->uniques[u], qname->str, ret, cleanup);
            qname->mod = ctx->mod;
        }
    }

    /* *default-stmt */
    if (d->dflts) {
        switch (target->nodetype) {
        case LYS_LEAF:
            AMEND_CHECK_CARDINALITY(d->dflts, 1, "deviation", "default");
            DEV_CHECK_NONPRESENCE(struct lysp_node_leaf *, dflt.str, "default", dflt.str);

            DUP_STRING_GOTO(ctx->ctx, d->dflts[0], ((struct lysp_node_leaf *)target)->dflt.str, ret, cleanup);
            ((struct lysp_node_leaf *)target)->dflt.mod = ctx->mod;
            break;
        case LYS_LEAFLIST:
            LY_ARRAY_FOR(d->dflts, u) {
                LY_ARRAY_NEW_GOTO(ctx->ctx, ((struct lysp_node_leaflist *)target)->dflts, qname, ret, cleanup);
                DUP_STRING_GOTO(ctx->ctx, d->dflts[u], qname->str, ret, cleanup);
                qname->mod = ctx->mod;
            }
            break;
        case LYS_CHOICE:
            AMEND_CHECK_CARDINALITY(d->dflts, 1, "deviation", "default");
            DEV_CHECK_NONPRESENCE(struct lysp_node_choice *, dflt.str, "default", dflt.str);

            DUP_STRING_GOTO(ctx->ctx, d->dflts[0], ((struct lysp_node_choice *)target)->dflt.str, ret, cleanup);
            ((struct lysp_node_choice *)target)->dflt.mod = ctx->mod;
            break;
        default:
            AMEND_WRONG_NODETYPE("deviation", "add", "default");
        }
    }

    /* [config-stmt] */
    if (d->flags & LYS_CONFIG_MASK) {
        switch (target->nodetype) {
        case LYS_CONTAINER:
        case LYS_LEAF:
        case LYS_LEAFLIST:
        case LYS_LIST:
        case LYS_CHOICE:
        case LYS_ANYDATA:
        case LYS_ANYXML:
            break;
        default:
            AMEND_WRONG_NODETYPE("deviation", "add", "config");
        }

        if (target->flags & LYS_CONFIG_MASK) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                    "Invalid deviation adding \"config\" property which already exists (with value \"config %s\").",
                    target->flags & LYS_CONFIG_W ? "true" : "false");
            ret = LY_EVALID;
            goto cleanup;
        }

        target->flags |= d->flags & LYS_CONFIG_MASK;
    }

    /* [mandatory-stmt] */
    if (d->flags & LYS_MAND_MASK) {
        switch (target->nodetype) {
        case LYS_LEAF:
        case LYS_CHOICE:
        case LYS_ANYDATA:
        case LYS_ANYXML:
            break;
        default:
            AMEND_WRONG_NODETYPE("deviation", "add", "mandatory");
        }

        if (target->flags & LYS_MAND_MASK) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                    "Invalid deviation adding \"mandatory\" property which already exists (with value \"mandatory %s\").",
                    target->flags & LYS_MAND_TRUE ? "true" : "false");
            ret = LY_EVALID;
            goto cleanup;
        }

        target->flags |= d->flags & LYS_MAND_MASK;
    }

    /* [min-elements-stmt] */
    if (d->flags & LYS_SET_MIN) {
        switch (target->nodetype) {
        case LYS_LEAFLIST:
            num = &((struct lysp_node_leaflist *)target)->min;
            break;
        case LYS_LIST:
            num = &((struct lysp_node_list *)target)->min;
            break;
        default:
            AMEND_WRONG_NODETYPE("deviation", "add", "min-elements");
        }

        if (target->flags & LYS_SET_MIN) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                    "Invalid deviation adding \"min-elements\" property which already exists (with value \"%u\").", *num);
            ret = LY_EVALID;
            goto cleanup;
        }

        *num = d->min;
    }

    /* [max-elements-stmt] */
    if (d->flags & LYS_SET_MAX) {
        switch (target->nodetype) {
        case LYS_LEAFLIST:
            num = &((struct lysp_node_leaflist *)target)->max;
            break;
        case LYS_LIST:
            num = &((struct lysp_node_list *)target)->max;
            break;
        default:
            AMEND_WRONG_NODETYPE("deviation", "add", "max-elements");
        }

        if (target->flags & LYS_SET_MAX) {
            if (*num) {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                        "Invalid deviation adding \"max-elements\" property which already exists (with value \"%u\").",
                        *num);
            } else {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                        "Invalid deviation adding \"max-elements\" property which already exists (with value \"unbounded\").");
            }
            ret = LY_EVALID;
            goto cleanup;
        }

        *num = d->max;
    }

cleanup:
    return ret;
}

/**
 * @brief Apply deviate delete.
 *
 * @param[in] ctx Compile context.
 * @param[in] d Deviate delete to apply.
 * @param[in,out] target Deviation target.
 * @return LY_ERR value.
 */
static LY_ERR
lys_apply_deviate_delete(struct lysc_ctx *ctx, struct lysp_deviate_del *d, struct lysp_node *target)
{
    LY_ERR ret = LY_SUCCESS;
    struct lysp_restr **musts;
    LY_ARRAY_COUNT_TYPE u, v;
    struct lysp_qname **uniques, **dflts;

#define DEV_DEL_ARRAY(DEV_ARRAY, ORIG_ARRAY, DEV_MEMBER, ORIG_MEMBER, FREE_FUNC, PROPERTY) \
    LY_ARRAY_FOR(d->DEV_ARRAY, u) { \
        int found = 0; \
        LY_ARRAY_FOR(ORIG_ARRAY, v) { \
            if (!strcmp(d->DEV_ARRAY[u]DEV_MEMBER, (ORIG_ARRAY)[v]ORIG_MEMBER)) { \
                found = 1; \
                break; \
            } \
        } \
        if (!found) { \
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE, \
                    "Invalid deviation deleting \"%s\" property \"%s\" which does not match any of the target's property values.", \
                    PROPERTY, d->DEV_ARRAY[u]DEV_MEMBER); \
            ret = LY_EVALID; \
            goto cleanup; \
        } \
        LY_ARRAY_DECREMENT(ORIG_ARRAY); \
        FREE_FUNC(ctx->ctx, &(ORIG_ARRAY)[v]); \
        memmove(&(ORIG_ARRAY)[v], &(ORIG_ARRAY)[v + 1], (LY_ARRAY_COUNT(ORIG_ARRAY) - v) * sizeof *(ORIG_ARRAY)); \
    } \
    if (!LY_ARRAY_COUNT(ORIG_ARRAY)) { \
        LY_ARRAY_FREE(ORIG_ARRAY); \
        ORIG_ARRAY = NULL; \
    }

#define DEV_CHECK_PRESENCE_VALUE(TYPE, MEMBER, DEVTYPE, PROPERTY, VALUE) \
    if (!((TYPE)target)->MEMBER) { \
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LY_VCODE_DEV_NOT_PRESENT, DEVTYPE, PROPERTY, VALUE); \
        ret = LY_EVALID; \
        goto cleanup; \
    } else if (strcmp(((TYPE)target)->MEMBER, VALUE)) { \
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE, \
                "Invalid deviation deleting \"%s\" property \"%s\" which does not match the target's property value \"%s\".", \
                PROPERTY, VALUE, ((TYPE)target)->MEMBER); \
        ret = LY_EVALID; \
        goto cleanup; \
    }

    /* [units-stmt] */
    if (d->units) {
        switch (target->nodetype) {
        case LYS_LEAF:
        case LYS_LEAFLIST:
            break;
        default:
            AMEND_WRONG_NODETYPE("deviation", "delete", "units");
        }

        DEV_CHECK_PRESENCE_VALUE(struct lysp_node_leaf *, units, "deleting", "units", d->units);
        FREE_STRING(ctx->ctx, ((struct lysp_node_leaf *)target)->units);
        ((struct lysp_node_leaf *)target)->units = NULL;
    }

    /* *must-stmt */
    if (d->musts) {
        switch (target->nodetype) {
        case LYS_CONTAINER:
        case LYS_LIST:
        case LYS_LEAF:
        case LYS_LEAFLIST:
        case LYS_ANYDATA:
        case LYS_ANYXML:
            musts = &((struct lysp_node_container *)target)->musts;
            break;
        case LYS_NOTIF:
            musts = &((struct lysp_notif *)target)->musts;
            break;
        case LYS_INPUT:
        case LYS_OUTPUT:
            musts = &((struct lysp_action_inout *)target)->musts;
            break;
        default:
            AMEND_WRONG_NODETYPE("deviation", "delete", "must");
        }

        DEV_DEL_ARRAY(musts, *musts, .arg.str, .arg.str, lysp_restr_free, "must");
    }

    /* *unique-stmt */
    if (d->uniques) {
        switch (target->nodetype) {
        case LYS_LIST:
            break;
        default:
            AMEND_WRONG_NODETYPE("deviation", "delete", "unique");
        }

        uniques = &((struct lysp_node_list *)target)->uniques;
        DEV_DEL_ARRAY(uniques, *uniques, , .str, lysp_qname_free, "unique");
    }

    /* *default-stmt */
    if (d->dflts) {
        switch (target->nodetype) {
        case LYS_LEAF:
            AMEND_CHECK_CARDINALITY(d->dflts, 1, "deviation", "default");
            DEV_CHECK_PRESENCE_VALUE(struct lysp_node_leaf *, dflt.str, "deleting", "default", d->dflts[0]);

            FREE_STRING(ctx->ctx, ((struct lysp_node_leaf *)target)->dflt.str);
            ((struct lysp_node_leaf *)target)->dflt.str = NULL;
            break;
        case LYS_LEAFLIST:
            dflts = &((struct lysp_node_leaflist *)target)->dflts;
            DEV_DEL_ARRAY(dflts, *dflts, , .str, lysp_qname_free, "default");
            break;
        case LYS_CHOICE:
            AMEND_CHECK_CARDINALITY(d->dflts, 1, "deviation", "default");
            DEV_CHECK_PRESENCE_VALUE(struct lysp_node_choice *, dflt.str, "deleting", "default", d->dflts[0]);

            FREE_STRING(ctx->ctx, ((struct lysp_node_choice *)target)->dflt.str);
            ((struct lysp_node_choice *)target)->dflt.str = NULL;
            break;
        default:
            AMEND_WRONG_NODETYPE("deviation", "delete", "default");
        }
    }

cleanup:
    return ret;
}

/**
 * @brief Apply deviate replace.
 *
 * @param[in] ctx Compile context.
 * @param[in] d Deviate replace to apply.
 * @param[in,out] target Deviation target.
 * @return LY_ERR value.
 */
static LY_ERR
lys_apply_deviate_replace(struct lysc_ctx *ctx, struct lysp_deviate_rpl *d, struct lysp_node *target)
{
    LY_ERR ret = LY_SUCCESS;
    uint32_t *num;

#define DEV_CHECK_PRESENCE(TYPE, MEMBER, DEVTYPE, PROPERTY, VALUE) \
    if (!((TYPE)target)->MEMBER) { \
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LY_VCODE_DEV_NOT_PRESENT, DEVTYPE, PROPERTY, VALUE); \
        ret = LY_EVALID; \
        goto cleanup; \
    }

    /* [type-stmt] */
    if (d->type) {
        switch (target->nodetype) {
        case LYS_LEAF:
        case LYS_LEAFLIST:
            break;
        default:
            AMEND_WRONG_NODETYPE("deviation", "replace", "type");
        }

        lysp_type_free(ctx->ctx, &((struct lysp_node_leaf *)target)->type);
        lysp_type_dup(ctx->ctx, &((struct lysp_node_leaf *)target)->type, d->type);
    }

    /* [units-stmt] */
    if (d->units) {
        switch (target->nodetype) {
        case LYS_LEAF:
        case LYS_LEAFLIST:
            break;
        default:
            AMEND_WRONG_NODETYPE("deviation", "replace", "units");
        }

        DEV_CHECK_PRESENCE(struct lysp_node_leaf *, units, "replacing", "units", d->units);
        FREE_STRING(ctx->ctx, ((struct lysp_node_leaf *)target)->units);
        DUP_STRING_GOTO(ctx->ctx, d->units, ((struct lysp_node_leaf *)target)->units, ret, cleanup);
    }

    /* [default-stmt] */
    if (d->dflt) {
        switch (target->nodetype) {
        case LYS_LEAF:
            DEV_CHECK_PRESENCE(struct lysp_node_leaf *, dflt.str, "replacing", "default", d->dflt);

            FREE_STRING(ctx->ctx, ((struct lysp_node_leaf *)target)->dflt.str);
            DUP_STRING_GOTO(ctx->ctx, d->dflt, ((struct lysp_node_leaf *)target)->dflt.str, ret, cleanup);
            ((struct lysp_node_leaf *)target)->dflt.mod = ctx->mod;
            break;
        case LYS_CHOICE:
            DEV_CHECK_PRESENCE(struct lysp_node_choice *, dflt.str, "replacing", "default", d->dflt);

            FREE_STRING(ctx->ctx, ((struct lysp_node_choice *)target)->dflt.str);
            DUP_STRING_GOTO(ctx->ctx, d->dflt, ((struct lysp_node_choice *)target)->dflt.str, ret, cleanup);
            ((struct lysp_node_choice *)target)->dflt.mod = ctx->mod;
            break;
        default:
            AMEND_WRONG_NODETYPE("deviation", "replace", "default");
        }
    }

    /* [config-stmt] */
    if (d->flags & LYS_CONFIG_MASK) {
        switch (target->nodetype) {
        case LYS_CONTAINER:
        case LYS_LEAF:
        case LYS_LEAFLIST:
        case LYS_LIST:
        case LYS_CHOICE:
        case LYS_ANYDATA:
        case LYS_ANYXML:
            break;
        default:
            AMEND_WRONG_NODETYPE("deviation", "replace", "config");
        }

        if (!(target->flags & LYS_CONFIG_MASK)) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LY_VCODE_DEV_NOT_PRESENT,
                    "replacing", "config", d->flags & LYS_CONFIG_W ? "config true" : "config false");
            ret = LY_EVALID;
            goto cleanup;
        }

        target->flags &= ~LYS_CONFIG_MASK;
        target->flags |= d->flags & LYS_CONFIG_MASK;
    }

    /* [mandatory-stmt] */
    if (d->flags & LYS_MAND_MASK) {
        switch (target->nodetype) {
        case LYS_LEAF:
        case LYS_CHOICE:
        case LYS_ANYDATA:
        case LYS_ANYXML:
            break;
        default:
            AMEND_WRONG_NODETYPE("deviation", "replace", "mandatory");
        }

        if (!(target->flags & LYS_MAND_MASK)) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LY_VCODE_DEV_NOT_PRESENT,
                    "replacing", "mandatory", d->flags & LYS_MAND_TRUE ? "mandatory true" : "mandatory false");
            ret = LY_EVALID;
            goto cleanup;
        }

        target->flags &= ~LYS_MAND_MASK;
        target->flags |= d->flags & LYS_MAND_MASK;
    }

    /* [min-elements-stmt] */
    if (d->flags & LYS_SET_MIN) {
        switch (target->nodetype) {
        case LYS_LEAFLIST:
            num = &((struct lysp_node_leaflist *)target)->min;
            break;
        case LYS_LIST:
            num = &((struct lysp_node_list *)target)->min;
            break;
        default:
            AMEND_WRONG_NODETYPE("deviation", "replace", "min-elements");
        }

        if (!(target->flags & LYS_SET_MIN)) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                    "Invalid deviation replacing \"min-elements\" property which is not present.");
            ret = LY_EVALID;
            goto cleanup;
        }

        *num = d->min;
    }

    /* [max-elements-stmt] */
    if (d->flags & LYS_SET_MAX) {
        switch (target->nodetype) {
        case LYS_LEAFLIST:
            num = &((struct lysp_node_leaflist *)target)->max;
            break;
        case LYS_LIST:
            num = &((struct lysp_node_list *)target)->max;
            break;
        default:
            AMEND_WRONG_NODETYPE("deviation", "replace", "max-elements");
        }

        if (!(target->flags & LYS_SET_MAX)) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                    "Invalid deviation replacing \"max-elements\" property which is not present.");
            ret = LY_EVALID;
            goto cleanup;
        }

        *num = d->max;
    }

cleanup:
    return ret;
}

/**
 * @brief Get module of a single nodeid node name test.
 *
 * @param[in] ctx libyang context.
 * @param[in] nametest Nametest with an optional prefix.
 * @param[in] nametest_len Length of @p nametest.
 * @param[in] local_mod Module to return in case of no prefix.
 * @param[out] name Optional pointer to the name test without the prefix.
 * @param[out] name_len Length of @p name.
 * @return Resolved module.
 */
static const struct lys_module *
lys_schema_node_get_module(const struct ly_ctx *ctx, const char *nametest, size_t nametest_len,
        const struct lys_module *local_mod, const char **name, size_t *name_len)
{
    const struct lys_module *target_mod;
    const char *ptr;

    ptr = ly_strnchr(nametest, ':', nametest_len);
    if (ptr) {
        target_mod = ly_resolve_prefix(ctx, nametest, ptr - nametest, LY_PREF_SCHEMA, (void *)local_mod);
        if (!target_mod) {
            LOGVAL(ctx, LY_VLOG_NONE, NULL, LYVE_REFERENCE,
                    "Invalid absolute-schema-nodeid nametest \"%.*s\" - prefix \"%.*s\" not defined in module \"%s\".",
                    nametest_len, nametest, ptr - nametest, nametest, local_mod->name);
            return NULL;
        }

        if (name) {
            *name = ptr + 1;
            *name_len = nametest_len - ((ptr - nametest) + 1);
        }
    } else {
        target_mod = local_mod;
        if (name) {
            *name = nametest;
            *name_len = nametest_len;
        }
    }

    return target_mod;
}

/**
 * @brief Check whether a parsed node matches a single schema nodeid name test.
 *
 * @param[in] pnode Parsed node to consider.
 * @param[in] pnode_mod Compiled @p pnode to-be module.
 * @param[in] mod Expected module.
 * @param[in] name Expected name.
 * @param[in] name_len Length of @p name.
 * @return Whether it is a match or not.
 */
static ly_bool
lysp_schema_nodeid_match_pnode(const struct lysp_node *pnode, const struct lys_module *pnode_mod,
        const struct lys_module *mod, const char *name, size_t name_len)
{
    const char *pname;

    /* compare with the module of the node */
    if (pnode_mod != mod) {
        return 0;
    }

    /* compare names */
    if (pnode->nodetype & (LYS_ACTION | LYS_RPC)) {
        pname = ((struct lysp_action *)pnode)->name;
    } else if (pnode->nodetype & (LYS_INPUT | LYS_OUTPUT)) {
        pname = (pnode->nodetype & LYS_INPUT) ? "input" : "output";
    } else {
        pname = pnode->name;
    }
    if (ly_strncmp(pname, name, name_len)) {
        return 0;
    }

    return 1;
}

/**
 * @brief Check whether a compiled node matches a single schema nodeid name test.
 *
 * @param[in,out] node Compiled node to consider. On a match it is moved to its parent.
 * @param[in] mod Expected module.
 * @param[in] name Expected name.
 * @param[in] name_len Length of @p name.
 * @return Whether it is a match or not.
 */
static ly_bool
lysp_schema_nodeid_match_node(const struct lysc_node **node, const struct lys_module *mod, const char *name,
        size_t name_len)
{
    const struct lys_module *node_mod;
    const char *node_name;

    /* compare with the module of the node */
    if ((*node)->nodetype == LYS_INPUT) {
        node_mod = ((struct lysc_node *)(((char *)*node) - offsetof(struct lysc_action, input)))->module;
    } else if ((*node)->nodetype == LYS_OUTPUT) {
        node_mod = ((struct lysc_node *)(((char *)*node) - offsetof(struct lysc_action, output)))->module;
    } else {
        node_mod = (*node)->module;
    }
    if (node_mod != mod) {
        return 0;
    }

    /* compare names */
    if ((*node)->nodetype == LYS_INPUT) {
        node_name = "input";
    } else if ((*node)->nodetype == LYS_OUTPUT) {
        node_name = "output";
    } else {
        node_name = (*node)->name;
    }
    if (ly_strncmp(node_name, name, name_len)) {
        return 0;
    }

    if ((*node)->nodetype & (LYS_INPUT | LYS_OUTPUT)) {
        /* move up from input/output */
        if ((*node)->nodetype == LYS_INPUT) {
            (*node) = (struct lysc_node *)(((char *)*node) - offsetof(struct lysc_action, input));
        } else {
            (*node) = (struct lysc_node *)(((char *)*node) - offsetof(struct lysc_action, output));
        }
    } else if ((*node)->parent && ((*node)->parent->nodetype & (LYS_RPC | LYS_ACTION))) {
        /* move to the input/output */
        if ((*node)->flags & LYS_CONFIG_W) {
            *node = (struct lysc_node *)&((struct lysc_action *)(*node)->parent)->input;
        } else {
            *node = (struct lysc_node *)&((struct lysc_action *)(*node)->parent)->output;
        }
    } else {
        /* move to next parent */
        *node = (*node)->parent;
    }

    return 1;
}

/**
 * @brief Check whether a node matches specific schema nodeid.
 *
 * @param[in] exp Parsed nodeid to match.
 * @param[in] exp_mod Module to use for nodes in @p exp without a prefix.
 * @param[in] ctx_node Initial context node that should match, only for descendant paths.
 * @param[in] parent First compiled parent to consider. If @p pnode is NULL, it is condered the node to be matched.
 * @param[in] pnode Parsed node to be matched. May be NULL if the target node was already compiled.
 * @param[in] pnode_mod Compiled @p pnode to-be module.
 * @return Whether it is a match or not.
 */
static ly_bool
lysp_schema_nodeid_match(const struct lyxp_expr *exp, const struct lys_module *exp_mod, const struct lysc_node *ctx_node,
        const struct lysc_node *parent, const struct lysp_node *pnode, const struct lys_module *pnode_mod)
{
    uint32_t i;
    const struct lys_module *mod;
    const char *name;
    size_t name_len;

    /* compare last node in the node ID */
    i = exp->used - 1;

    /* get exp node ID module */
    mod = lys_schema_node_get_module(exp_mod->ctx, exp->expr + exp->tok_pos[i], exp->tok_len[i], exp_mod, &name, &name_len);
    assert(mod);

    if (pnode) {
        /* compare on the last parsed-only node */
        if (!lysp_schema_nodeid_match_pnode(pnode, pnode_mod, mod, name, name_len)) {
            return 0;
        }
    } else {
        /* using parent directly */
        if (!lysp_schema_nodeid_match_node(&parent, mod, name, name_len)) {
            return 0;
        }
    }

    /* now compare all the compiled parents */
    while (i > 1) {
        i -= 2;
        assert(exp->tokens[i] == LYXP_TOKEN_NAMETEST);

        if (!parent) {
            /* no more parents but path continues */
            return 0;
        }

        /* get exp node ID module */
        mod = lys_schema_node_get_module(exp_mod->ctx, exp->expr + exp->tok_pos[i], exp->tok_len[i], exp_mod, &name,
                &name_len);
        assert(mod);

        /* compare with the parent */
        if (!lysp_schema_nodeid_match_node(&parent, mod, name, name_len)) {
            return 0;
        }
    }

    if (ctx_node && (ctx_node != parent)) {
        /* descendant path has not finished in the context node */
        return 0;
    } else if (!ctx_node && parent) {
        /* some parent was not matched */
        return 0;
    }

    return 1;
}

static void
lysc_augment_free(const struct ly_ctx *ctx, struct lysc_augment *aug)
{
    if (aug) {
        lyxp_expr_free(ctx, aug->nodeid);

        free(aug);
    }
}

static void
lysc_deviation_free(const struct ly_ctx *ctx, struct lysc_deviation *dev)
{
    if (dev) {
        lyxp_expr_free(ctx, dev->nodeid);
        LY_ARRAY_FREE(dev->devs);
        LY_ARRAY_FREE(dev->dev_mods);

        free(dev);
    }
}

static void
lysc_refine_free(const struct ly_ctx *ctx, struct lysc_refine *rfn)
{
    if (rfn) {
        lyxp_expr_free(ctx, rfn->nodeid);
        LY_ARRAY_FREE(rfn->rfns);

        free(rfn);
    }
}

static void
lysp_dev_node_free(const struct ly_ctx *ctx, struct lysp_node *dev_pnode)
{
    if (!dev_pnode) {
        return;
    }

    switch (dev_pnode->nodetype) {
    case LYS_CONTAINER:
        ((struct lysp_node_container *)dev_pnode)->child = NULL;
        break;
    case LYS_LIST:
        ((struct lysp_node_list *)dev_pnode)->child = NULL;
        break;
    case LYS_CHOICE:
        ((struct lysp_node_choice *)dev_pnode)->child = NULL;
        break;
    case LYS_CASE:
        ((struct lysp_node_case *)dev_pnode)->child = NULL;
        break;
    case LYS_LEAF:
    case LYS_LEAFLIST:
    case LYS_ANYXML:
    case LYS_ANYDATA:
        /* no children */
        break;
    case LYS_NOTIF:
        ((struct lysp_notif *)dev_pnode)->data = NULL;
        lysp_notif_free((struct ly_ctx *)ctx, (struct lysp_notif *)dev_pnode);
        free(dev_pnode);
        return;
    case LYS_RPC:
    case LYS_ACTION:
        ((struct lysp_action *)dev_pnode)->input.data = NULL;
        ((struct lysp_action *)dev_pnode)->output.data = NULL;
        lysp_action_free((struct ly_ctx *)ctx, (struct lysp_action *)dev_pnode);
        free(dev_pnode);
        return;
    case LYS_INPUT:
    case LYS_OUTPUT:
        ((struct lysp_action_inout *)dev_pnode)->data = NULL;
        lysp_action_inout_free((struct ly_ctx *)ctx, (struct lysp_action_inout *)dev_pnode);
        free(dev_pnode);
        return;
    default:
        LOGINT(ctx);
        return;
    }

    lysp_node_free((struct ly_ctx *)ctx, dev_pnode);
}

/**
 * @brief Compile and apply any precompiled deviations and refines targetting a node.
 *
 * @param[in] ctx Compile context.
 * @param[in] pnode Parsed node to consider.
 * @param[in] parent First compiled parent of @p pnode.
 * @param[out] dev_pnode Copy of parsed node @p pnode with deviations and refines, if any. NULL if there are none.
 * @param[out] no_supported Whether a not-supported deviation is defined for the node.
 * @return LY_ERR value.
 */
static LY_ERR
lys_compile_node_deviations_refines(struct lysc_ctx *ctx, const struct lysp_node *pnode, const struct lysc_node *parent,
        struct lysp_node **dev_pnode, ly_bool *not_supported)
{
    LY_ERR ret = LY_SUCCESS;
    uint32_t i;
    LY_ARRAY_COUNT_TYPE u;
    struct lys_module *orig_mod = ctx->mod, *orig_mod_def = ctx->mod_def;
    char orig_path[LYSC_CTX_BUFSIZE];
    struct lysc_refine *rfn;
    struct lysc_deviation *dev;
    struct lysp_deviation *dev_p;
    struct lysp_deviate *d;

    *dev_pnode = NULL;
    *not_supported = 0;

    for (i = 0; i < ctx->uses_rfns.count; ++i) {
        rfn = ctx->uses_rfns.objs[i];

        if (!lysp_schema_nodeid_match(rfn->nodeid, ctx->mod, rfn->nodeid_ctx_node, parent, pnode, ctx->mod)) {
            /* not our target node */
            continue;
        }

        if (!*dev_pnode) {
            /* first refine on this node, create a copy first */
            LY_CHECK_GOTO(ret = lysp_dup_single(ctx->ctx, pnode, 1, dev_pnode), cleanup);
        }

        /* apply all the refines by changing (the copy of) the parsed node */
        LY_ARRAY_FOR(rfn->rfns, u) {
            /* apply refine, keep the current path and add to it */
            lysc_update_path(ctx, NULL, "{refine}");
            lysc_update_path(ctx, NULL, rfn->rfns[u]->nodeid);
            ret = lys_apply_refine(ctx, rfn->rfns[u], *dev_pnode);
            lysc_update_path(ctx, NULL, NULL);
            lysc_update_path(ctx, NULL, NULL);
            LY_CHECK_GOTO(ret, cleanup);
        }

        /* refine was applied, remove it */
        lysc_refine_free(ctx->ctx, rfn);
        ly_set_rm_index(&ctx->uses_rfns, i, NULL);

        /* all the refines for one target node are in one structure, we are done */
        break;
    }

    for (i = 0; i < ctx->devs.count; ++i) {
        dev = ctx->devs.objs[i];

        if (!lysp_schema_nodeid_match(dev->nodeid, dev->nodeid_mod, NULL, parent, pnode, ctx->mod_def)) {
            /* not our target node */
            continue;
        }

        if (dev->not_supported) {
            /* it is not supported, no more deviations */
            *not_supported = 1;
            goto dev_applied;
        }

        if (!*dev_pnode) {
            /* first deviation on this node, create a copy first */
            LY_CHECK_GOTO(ret = lysp_dup_single(ctx->ctx, pnode, 1, dev_pnode), cleanup);
        }

        /* apply all the deviates by changing (the copy of) the parsed node */
        LY_ARRAY_FOR(dev->devs, u) {
            dev_p = dev->devs[u];
            LY_LIST_FOR(dev_p->deviates, d) {
                /* generate correct path */
                strcpy(orig_path, ctx->path);
                ctx->path_len = 1;
                ctx->mod = (struct lys_module *)dev->dev_mods[u];
                ctx->mod_def = (struct lys_module *)dev->dev_mods[u];
                lysc_update_path(ctx, NULL, "{deviation}");
                lysc_update_path(ctx, NULL, dev_p->nodeid);

                switch (d->mod) {
                case LYS_DEV_ADD:
                    ret = lys_apply_deviate_add(ctx, (struct lysp_deviate_add *)d, *dev_pnode);
                    break;
                case LYS_DEV_DELETE:
                    ret = lys_apply_deviate_delete(ctx, (struct lysp_deviate_del *)d, *dev_pnode);
                    break;
                case LYS_DEV_REPLACE:
                    ret = lys_apply_deviate_replace(ctx, (struct lysp_deviate_rpl *)d, *dev_pnode);
                    break;
                default:
                    LOGINT(ctx->ctx);
                    ret = LY_EINT;
                }

                /* restore previous path */
                strcpy(ctx->path, orig_path);
                ctx->path_len = strlen(ctx->path);
                ctx->mod = orig_mod;
                ctx->mod_def = orig_mod_def;

                LY_CHECK_GOTO(ret, cleanup);
            }
        }

dev_applied:
        /* deviation was applied, remove it */
        lysc_deviation_free(ctx->ctx, dev);
        ly_set_rm_index(&ctx->devs, i, NULL);

        /* all the deviations for one target node are in one structure, we are done */
        break;
    }

cleanup:
    if (ret) {
        lysp_dev_node_free(ctx->ctx, *dev_pnode);
        *dev_pnode = NULL;
        *not_supported = 0;
    }
    return ret;
}

/**
 * @brief Compile and apply any precompiled top-level or uses augments targetting a node.
 *
 * @param[in] ctx Compile context.
 * @param[in] node Compiled node to consider.
 * @return LY_ERR value.
 */
static LY_ERR
lys_compile_node_augments(struct lysc_ctx *ctx, struct lysc_node *node)
{
    LY_ERR ret = LY_SUCCESS;
    struct lys_module *orig_mod = ctx->mod, *orig_mod_def = ctx->mod_def;
    uint32_t i;
    char orig_path[LYSC_CTX_BUFSIZE];
    struct lysc_augment *aug;

    /* uses augments */
    for (i = 0; i < ctx->uses_augs.count; ) {
        aug = ctx->uses_augs.objs[i];

        if (!lysp_schema_nodeid_match(aug->nodeid, ctx->mod, aug->nodeid_ctx_node, node, NULL, NULL)) {
            /* not our target node */
            ++i;
            continue;
        }

        /* apply augment, keep the current path and add to it */
        lysc_update_path(ctx, NULL, "{augment}");
        lysc_update_path(ctx, NULL, aug->aug_p->nodeid);
        ret = lys_compile_augment(ctx, aug->aug_p, node);
        lysc_update_path(ctx, NULL, NULL);
        lysc_update_path(ctx, NULL, NULL);
        LY_CHECK_GOTO(ret, cleanup);

        /* augment was applied, remove it (index may have changed because other augments could have been applied) */
        ly_set_rm(&ctx->uses_augs, aug, NULL);
        lysc_augment_free(ctx->ctx, aug);
    }

    /* top-level augments */
    for (i = 0; i < ctx->augs.count; ) {
        aug = ctx->augs.objs[i];

        if (!lysp_schema_nodeid_match(aug->nodeid, aug->nodeid_mod, NULL, node, NULL, NULL)) {
            /* not our target node */
            ++i;
            continue;
        }

        /* apply augment, use the path and modules from the augment */
        strcpy(orig_path, ctx->path);
        ctx->path_len = 1;
        lysc_update_path(ctx, NULL, "{augment}");
        lysc_update_path(ctx, NULL, aug->aug_p->nodeid);
        ctx->mod = (struct lys_module *)aug->nodeid_mod;
        ctx->mod_def = (struct lys_module *)aug->nodeid_mod;
        ret = lys_compile_augment(ctx, aug->aug_p, node);
        strcpy(ctx->path, orig_path);
        ctx->path_len = strlen(ctx->path);
        LY_CHECK_GOTO(ret, cleanup);

        /* augment was applied, remove it */
        ly_set_rm(&ctx->augs, aug, NULL);
        lysc_augment_free(ctx->ctx, aug);
    }

cleanup:
    ctx->mod = orig_mod;
    ctx->mod_def = orig_mod_def;
    return ret;
}

/**
 * @brief Prepare a top-level augment to be applied during data nodes compilation.
 *
 * @param[in] ctx Compile context.
 * @param[in] aug_p Parsed augment to be applied.
 * @param[in] mod_def Local module for @p aug_p.
 * @return LY_ERR value.
 */
static LY_ERR
lys_precompile_own_augment(struct lysc_ctx *ctx, struct lysp_augment *aug_p, const struct lys_module *mod_def)
{
    LY_ERR ret = LY_SUCCESS;
    struct lyxp_expr *exp = NULL;
    struct lysc_augment *aug;
    const struct lys_module *mod;

    /* parse its target, it was already parsed and fully checked (except for the existence of the nodes) */
    ret = lyxp_expr_parse(ctx->ctx, aug_p->nodeid, strlen(aug_p->nodeid), 0, &exp);
    LY_CHECK_GOTO(ret, cleanup);

    mod = lys_schema_node_get_module(ctx->ctx, exp->expr + exp->tok_pos[1], exp->tok_len[1], mod_def, NULL, NULL);
    LY_CHECK_ERR_GOTO(!mod, LOGINT(ctx->ctx); ret = LY_EINT, cleanup);
    if (mod != ctx->mod) {
        /* augment for another module, ignore */
        goto cleanup;
    }

    /* allocate new compiled augment and store it in the set */
    aug = calloc(1, sizeof *aug);
    LY_CHECK_ERR_GOTO(!aug, LOGMEM(ctx->ctx); ret = LY_EMEM, cleanup);
    LY_CHECK_GOTO(ret = ly_set_add(&ctx->augs, aug, 1, NULL), cleanup);

    aug->nodeid = exp;
    exp = NULL;
    aug->nodeid_mod = mod_def;
    aug->aug_p = aug_p;

cleanup:
    lyxp_expr_free(ctx->ctx, exp);
    return ret;
}

/**
 * @brief Prepare all top-level augments for the current module to be applied during data nodes compilation.
 *
 * @param[in] ctx Compile context.
 * @return LY_ERR value.
 */
static LY_ERR
lys_precompile_own_augments(struct lysc_ctx *ctx)
{
    LY_ARRAY_COUNT_TYPE u, v, w;
    const struct lys_module *aug_mod;

    LY_ARRAY_FOR(ctx->mod->augmented_by, u) {
        aug_mod = ctx->mod->augmented_by[u];

        /* collect all module augments */
        LY_ARRAY_FOR(aug_mod->parsed->augments, v) {
            LY_CHECK_RET(lys_precompile_own_augment(ctx, &aug_mod->parsed->augments[v], aug_mod));
        }

        /* collect all submodules augments */
        LY_ARRAY_FOR(aug_mod->parsed->includes, v) {
            LY_ARRAY_FOR(aug_mod->parsed->includes[v].submodule->augments, w) {
                LY_CHECK_RET(lys_precompile_own_augment(ctx, &aug_mod->parsed->includes[v].submodule->augments[w], aug_mod));
            }
        }
    }

    return LY_SUCCESS;
}

/**
 * @brief Prepare a deviation to be applied during data nodes compilation.
 *
 * @param[in] ctx Compile context.
 * @param[in] dev_p Parsed deviation to be applied.
 * @param[in] mod_def Local module for @p dev_p.
 * @return LY_ERR value.
 */
static LY_ERR
lys_precompile_own_deviation(struct lysc_ctx *ctx, struct lysp_deviation *dev_p, const struct lys_module *mod_def)
{
    LY_ERR ret = LY_SUCCESS;
    struct lysc_deviation *dev = NULL;
    struct lyxp_expr *exp = NULL;
    struct lysp_deviation **new_dev;
    const struct lys_module *mod, **new_dev_mod;
    uint32_t i;

    /* parse its target, it was already parsed and fully checked (except for the existence of the nodes) */
    ret = lyxp_expr_parse(ctx->ctx, dev_p->nodeid, strlen(dev_p->nodeid), 0, &exp);
    LY_CHECK_GOTO(ret, cleanup);

    mod = lys_schema_node_get_module(ctx->ctx, exp->expr + exp->tok_pos[1], exp->tok_len[1], mod_def, NULL, NULL);
    LY_CHECK_ERR_GOTO(!mod, LOGINT(ctx->ctx); ret = LY_EINT, cleanup);
    if (mod != ctx->mod) {
        /* deviation for another module, ignore */
        goto cleanup;
    }

    /* try to find the node in already compiled deviations */
    for (i = 0; i < ctx->devs.count; ++i) {
        if (lys_abs_schema_nodeid_match(ctx->ctx, exp, mod_def, ((struct lysc_deviation *)ctx->devs.objs[i])->nodeid,
                ((struct lysc_deviation *)ctx->devs.objs[i])->nodeid_mod)) {
            dev = ctx->devs.objs[i];
            break;
        }
    }

    if (!dev) {
        /* allocate new compiled deviation */
        dev = calloc(1, sizeof *dev);
        LY_CHECK_ERR_GOTO(!dev, LOGMEM(ctx->ctx); ret = LY_EMEM, cleanup);
        LY_CHECK_GOTO(ret = ly_set_add(&ctx->devs, dev, 1, NULL), cleanup);

        dev->nodeid = exp;
        exp = NULL;
        dev->nodeid_mod = mod_def;
    }

    /* add new parsed deviation structure */
    LY_ARRAY_NEW_GOTO(ctx->ctx, dev->devs, new_dev, ret, cleanup);
    *new_dev = dev_p;
    LY_ARRAY_NEW_GOTO(ctx->ctx, dev->dev_mods, new_dev_mod, ret, cleanup);
    *new_dev_mod = mod_def;

cleanup:
    lyxp_expr_free(ctx->ctx, exp);
    return ret;
}

/**
 * @brief Prepare all deviations for the current module to be applied during data nodes compilation.
 *
 * @param[in] ctx Compile context.
 * @return LY_ERR value.
 */
static LY_ERR
lys_precompile_own_deviations(struct lysc_ctx *ctx)
{
    LY_ARRAY_COUNT_TYPE u, v, w;
    const struct lys_module *dev_mod;
    struct lysc_deviation *dev;
    struct lysp_deviate *d;
    int not_supported;
    uint32_t i;

    LY_ARRAY_FOR(ctx->mod->deviated_by, u) {
        dev_mod = ctx->mod->deviated_by[u];

        /* compile all module deviations */
        LY_ARRAY_FOR(dev_mod->parsed->deviations, v) {
            LY_CHECK_RET(lys_precompile_own_deviation(ctx, &dev_mod->parsed->deviations[v], dev_mod));
        }

        /* compile all submodules deviations */
        LY_ARRAY_FOR(dev_mod->parsed->includes, v) {
            LY_ARRAY_FOR(dev_mod->parsed->includes[v].submodule->deviations, w) {
                LY_CHECK_RET(lys_precompile_own_deviation(ctx, &dev_mod->parsed->includes[v].submodule->deviations[w], dev_mod));
            }
        }
    }

    /* set not-supported flags for all the deviations */
    for (i = 0; i < ctx->devs.count; ++i) {
        dev = ctx->devs.objs[i];
        not_supported = 0;

        LY_ARRAY_FOR(dev->devs, u) {
            LY_LIST_FOR(dev->devs[u]->deviates, d) {
                if (d->mod == LYS_DEV_NOT_SUPPORTED) {
                    not_supported = 1;
                    break;
                }
            }
            if (not_supported) {
                break;
            }
        }
        if (not_supported && (LY_ARRAY_COUNT(dev->devs) > 1)) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SEMANTICS,
                    "Multiple deviations of \"%s\" with one of them being \"not-supported\".", dev->nodeid->expr);
            return LY_EVALID;
        }

        dev->not_supported = not_supported;
    }

    return LY_SUCCESS;
}

/**
 * @brief Compile parsed schema node information.
 * @param[in] ctx Compile context
 * @param[in] pnode Parsed schema node.
 * @param[in] parent Compiled parent node where the current node is supposed to be connected. It is
 * NULL for top-level nodes, in such a case the module where the node will be connected is taken from
 * the compile context.
 * @param[in] uses_status If the node is being placed instead of uses, here we have the uses's status value (as node's flags).
 * Zero means no uses, non-zero value with no status bit set mean the default status.
 * @return LY_ERR value - LY_SUCCESS or LY_EVALID.
 */
static LY_ERR
lys_compile_node(struct lysc_ctx *ctx, struct lysp_node *pnode, struct lysc_node *parent, uint16_t uses_status,
        struct ly_set *child_set)
{
    LY_ERR ret = LY_SUCCESS;
    struct lysc_node *node = NULL;
    struct lysp_node *dev_pnode = NULL, *orig_pnode = pnode;
    LY_ARRAY_COUNT_TYPE u;
    ly_bool not_supported;

    LY_ERR (*node_compile_spec)(struct lysc_ctx *, struct lysp_node *, struct lysc_node *);

    if (pnode->nodetype != LYS_USES) {
        lysc_update_path(ctx, parent, pnode->name);
    } else {
        lysc_update_path(ctx, NULL, "{uses}");
        lysc_update_path(ctx, NULL, pnode->name);
    }

    switch (pnode->nodetype) {
    case LYS_CONTAINER:
        node = (struct lysc_node *)calloc(1, sizeof(struct lysc_node_container));
        node_compile_spec = lys_compile_node_container;
        break;
    case LYS_LEAF:
        node = (struct lysc_node *)calloc(1, sizeof(struct lysc_node_leaf));
        node_compile_spec = lys_compile_node_leaf;
        break;
    case LYS_LIST:
        node = (struct lysc_node *)calloc(1, sizeof(struct lysc_node_list));
        node_compile_spec = lys_compile_node_list;
        break;
    case LYS_LEAFLIST:
        node = (struct lysc_node *)calloc(1, sizeof(struct lysc_node_leaflist));
        node_compile_spec = lys_compile_node_leaflist;
        break;
    case LYS_CHOICE:
        node = (struct lysc_node *)calloc(1, sizeof(struct lysc_node_choice));
        node_compile_spec = lys_compile_node_choice;
        break;
    case LYS_CASE:
        node = (struct lysc_node *)calloc(1, sizeof(struct lysc_node_case));
        node_compile_spec = lys_compile_node_case;
        break;
    case LYS_ANYXML:
    case LYS_ANYDATA:
        node = (struct lysc_node *)calloc(1, sizeof(struct lysc_node_anydata));
        node_compile_spec = lys_compile_node_any;
        break;
    case LYS_USES:
        ret = lys_compile_uses(ctx, (struct lysp_node_uses *)pnode, parent, child_set);
        lysc_update_path(ctx, NULL, NULL);
        lysc_update_path(ctx, NULL, NULL);
        return ret;
    default:
        LOGINT(ctx->ctx);
        return LY_EINT;
    }
    LY_CHECK_ERR_RET(!node, LOGMEM(ctx->ctx), LY_EMEM);

    /* compile any deviations for this node */
    LY_CHECK_ERR_RET(ret = lys_compile_node_deviations_refines(ctx, pnode, parent, &dev_pnode, &not_supported),
            free(node), ret);
    if (not_supported) {
        free(node);
        lysc_update_path(ctx, NULL, NULL);
        return LY_SUCCESS;
    } else if (dev_pnode) {
        pnode = dev_pnode;
    }

    node->nodetype = pnode->nodetype;
    node->module = ctx->mod;
    node->prev = node;
    node->flags = pnode->flags & LYS_FLAGS_COMPILED_MASK;

    /* config */
    ret = lys_compile_config(ctx, node, parent);
    LY_CHECK_GOTO(ret, error);

    /* list ordering */
    if (node->nodetype & (LYS_LIST | LYS_LEAFLIST)) {
        if ((node->flags & LYS_CONFIG_R) && (node->flags & LYS_ORDBY_MASK)) {
            LOGWRN(ctx->ctx, "The ordered-by statement is ignored in lists representing %s (%s).",
                    (ctx->options & LYS_COMPILE_RPC_OUTPUT) ? "RPC/action output parameters" :
                    (ctx->options & LYS_COMPILE_NOTIFICATION) ? "notification content" : "state data", ctx->path);
            node->flags &= ~LYS_ORDBY_MASK;
            node->flags |= LYS_ORDBY_SYSTEM;
        } else if (!(node->flags & LYS_ORDBY_MASK)) {
            /* default ordering is system */
            node->flags |= LYS_ORDBY_SYSTEM;
        }
    }

    /* status - it is not inherited by specification, but it does not make sense to have
     * current in deprecated or deprecated in obsolete, so we do print warning and inherit status */
    LY_CHECK_GOTO(ret = lys_compile_status(ctx, &node->flags, uses_status ? uses_status : (parent ? parent->flags : 0)), error);

    node->sp = orig_pnode;
    DUP_STRING_GOTO(ctx->ctx, pnode->name, node->name, ret, error);
    DUP_STRING_GOTO(ctx->ctx, pnode->dsc, node->dsc, ret, error);
    DUP_STRING_GOTO(ctx->ctx, pnode->ref, node->ref, ret, error);
    COMPILE_ARRAY_GOTO(ctx, pnode->iffeatures, node->iffeatures, u, lys_compile_iffeature, ret, error);

    /* insert into parent's children/compiled module (we can no longer free the node separately on error) */
    LY_CHECK_GOTO(ret = lys_compile_node_connect(ctx, parent, node), cleanup);

    if (pnode->when) {
        /* compile when */
        ret = lys_compile_when(ctx, pnode->when, pnode->flags, lysc_xpath_context(node), node, NULL);
        LY_CHECK_GOTO(ret, cleanup);
    }

    /* connect any augments */
    LY_CHECK_GOTO(ret = lys_compile_node_augments(ctx, node), cleanup);

    /* nodetype-specific part */
    LY_CHECK_GOTO(ret = node_compile_spec(ctx, pnode, node), cleanup);

    /* final compilation tasks that require the node to be connected */
    COMPILE_EXTS_GOTO(ctx, pnode->exts, node->exts, node, LYEXT_PAR_NODE, ret, cleanup);
    if (node->flags & LYS_MAND_TRUE) {
        /* inherit LYS_MAND_TRUE in parent containers */
        lys_compile_mandatory_parents(parent, 1);
    }

    if (child_set) {
        /* add the new node into set */
        LY_CHECK_GOTO(ret = ly_set_add(child_set, node, 1, NULL), cleanup);
    }

    lysc_update_path(ctx, NULL, NULL);
    lysp_dev_node_free(ctx->ctx, dev_pnode);
    return LY_SUCCESS;

error:
    lysc_node_free(ctx->ctx, node);
cleanup:
    if (dev_pnode) {
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_OTHER, "Compilation of a deviated and/or refined node failed.");
        lysp_dev_node_free(ctx->ctx, dev_pnode);
    }
    return ret;
}

/**
 * @brief Add a module reference into an array, checks for duplicities.
 *
 * @param[in] ctx Compile context.
 * @param[in] mod Module reference to add.
 * @param[in,out] mod_array Module sized array to add to.
 * @return LY_ERR value.
 */
static LY_ERR
lys_array_add_mod_ref(struct lysc_ctx *ctx, struct lys_module *mod, struct lys_module ***mod_array)
{
    LY_ARRAY_COUNT_TYPE u;
    struct lys_module **new_mod;

    LY_ARRAY_FOR(*mod_array, u) {
        if ((*mod_array)[u] == mod) {
            /* already there */
            return LY_EEXIST;
        }
    }

    /* add the new module ref */
    LY_ARRAY_NEW_RET(ctx->ctx, *mod_array, new_mod, LY_EMEM);
    *new_mod = mod;

    return LY_SUCCESS;
}

/**
 * @brief Compile top-level augments and deviations defined in the current module.
 * Generally, just add the module refence to the target modules. But in case
 * of foreign augments, they are directly applied.
 *
 * @param[in] ctx Compile context.
 * @return LY_ERR value.
 */
static LY_ERR
lys_precompile_augments_deviations(struct lysc_ctx *ctx)
{
    LY_ERR ret = LY_SUCCESS;
    LY_ARRAY_COUNT_TYPE u, v;
    const struct lysp_module *mod_p;
    const struct lysc_node *target;
    struct lys_module *mod;
    struct lysp_submodule *submod;
    ly_bool has_dev = 0;
    uint16_t flags;
    uint32_t idx, opt_prev = ctx->options;

    for (idx = 0; idx < ctx->ctx->implementing.count; ++idx) {
        if (ctx->mod == ctx->ctx->implementing.objs[idx]) {
            break;
        }
    }
    if (idx == ctx->ctx->implementing.count) {
        /* it was already implemented and all the augments and deviations fully applied */
        return LY_SUCCESS;
    }

    mod_p = ctx->mod->parsed;

    LY_ARRAY_FOR(mod_p->augments, u) {
        lysc_update_path(ctx, NULL, "{augment}");
        lysc_update_path(ctx, NULL, mod_p->augments[u].nodeid);

        /* get target module */
        ret = lys_nodeid_check(ctx, mod_p->augments[u].nodeid, 1, &mod, NULL);
        LY_CHECK_RET(ret);

        /* add this module into the target module augmented_by, if not there already from previous augments */
        lys_array_add_mod_ref(ctx, ctx->mod, &mod->augmented_by);

        /* if we are compiling this module, we cannot add augments to it yet */
        if (mod != ctx->mod) {
            /* apply the augment, find the target node first */
            flags = 0;
            ret = lysc_resolve_schema_nodeid(ctx, mod_p->augments[u].nodeid, 0, NULL, ctx->mod_def, 0, &target, &flags);
            LY_CHECK_RET(ret);

            /* apply the augment */
            ctx->options |= flags;
            ret = lys_compile_augment(ctx, &mod_p->augments[u], (struct lysc_node *)target);
            ctx->options = opt_prev;
            LY_CHECK_RET(ret);
        }

        lysc_update_path(ctx, NULL, NULL);
        lysc_update_path(ctx, NULL, NULL);
    }

    LY_ARRAY_FOR(mod_p->deviations, u) {
        /* get target module */
        lysc_update_path(ctx, NULL, "{deviation}");
        lysc_update_path(ctx, NULL, mod_p->deviations[u].nodeid);
        ret = lys_nodeid_check(ctx, mod_p->deviations[u].nodeid, 1, &mod, NULL);
        lysc_update_path(ctx, NULL, NULL);
        lysc_update_path(ctx, NULL, NULL);
        LY_CHECK_RET(ret);

        /* add this module into the target module deviated_by, if not there already from previous deviations */
        lys_array_add_mod_ref(ctx, ctx->mod, &mod->deviated_by);

        /* new deviation added to the target module */
        has_dev = 1;
    }

    /* the same for augments and deviations in submodules */
    LY_ARRAY_FOR(mod_p->includes, v) {
        submod = mod_p->includes[v].submodule;
        LY_ARRAY_FOR(submod->augments, u) {
            lysc_update_path(ctx, NULL, "{augment}");
            lysc_update_path(ctx, NULL, submod->augments[u].nodeid);

            ret = lys_nodeid_check(ctx, submod->augments[u].nodeid, 1, &mod, NULL);
            LY_CHECK_RET(ret);

            lys_array_add_mod_ref(ctx, ctx->mod, &mod->augmented_by);
            if (mod != ctx->mod) {
                flags = 0;
                ret = lysc_resolve_schema_nodeid(ctx, mod_p->augments[u].nodeid, 0, NULL, ctx->mod_def, 0, &target, &flags);
                LY_CHECK_RET(ret);

                ctx->options |= flags;
                ret = lys_compile_augment(ctx, &submod->augments[u], (struct lysc_node *)target);
                ctx->options = opt_prev;
                LY_CHECK_RET(ret);
            }

            lysc_update_path(ctx, NULL, NULL);
            lysc_update_path(ctx, NULL, NULL);
        }

        LY_ARRAY_FOR(submod->deviations, u) {
            lysc_update_path(ctx, NULL, "{deviation}");
            lysc_update_path(ctx, NULL, submod->deviations[u].nodeid);
            ret = lys_nodeid_check(ctx, submod->deviations[u].nodeid, 1, &mod, NULL);
            lysc_update_path(ctx, NULL, NULL);
            lysc_update_path(ctx, NULL, NULL);
            LY_CHECK_RET(ret);

            lys_array_add_mod_ref(ctx, ctx->mod, &mod->deviated_by);
            has_dev = 1;
        }
    }

    if (!has_dev) {
        /* no need to recompile any modules */
        return LY_SUCCESS;
    }

    /* free all the modules in descending order */
    idx = ctx->ctx->list.count;
    do {
        --idx;
        mod = ctx->ctx->list.objs[idx];
        /* skip this module */
        if (mod == mod_p->mod) {
            continue;
        }

        if (mod->implemented && mod->compiled) {
            /* keep information about features state in the module */
            lys_feature_precompile_revert(ctx, mod);

            /* free the module */
            lysc_module_free(mod->compiled, NULL);
            mod->compiled = NULL;
        }
    } while (idx);

    /* recompile all the modules in ascending order */
    for (idx = 0; idx < ctx->ctx->list.count; ++idx) {
        mod = ctx->ctx->list.objs[idx];

        /* skip this module */
        if (mod == mod_p->mod) {
            continue;
        }

        if (mod->implemented) {
            /* compile */
            LY_CHECK_GOTO(ret = lys_compile(mod, 0), cleanup);
        }
    }

cleanup:
    return ret;
}

static void *
lys_compile_extension_instance_storage(enum ly_stmt stmt, struct lysc_ext_substmt *substmts)
{
    for (LY_ARRAY_COUNT_TYPE u = 0; substmts[u].stmt; ++u) {
        if (substmts[u].stmt == stmt) {
            return substmts[u].storage;
        }
    }
    return NULL;
}

LY_ERR
lys_compile_extension_instance(struct lysc_ctx *ctx, const struct lysp_ext_instance *ext, struct lysc_ext_substmt *substmts)
{
    LY_ERR ret = LY_EVALID, r;
    LY_ARRAY_COUNT_TYPE u;
    struct lysp_stmt *stmt;
    struct lysp_qname qname;
    void *parsed = NULL, **compiled = NULL;

    /* check for invalid substatements */
    for (stmt = ext->child; stmt; stmt = stmt->next) {
        if (stmt->flags & (LYS_YIN_ATTR | LYS_YIN_ARGUMENT)) {
            continue;
        }
        for (u = 0; substmts[u].stmt; ++u) {
            if (substmts[u].stmt == stmt->kw) {
                break;
            }
        }
        if (!substmts[u].stmt) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG, "Invalid keyword \"%s\" as a child of \"%s%s%s\" extension instance.",
                    stmt->stmt, ext->name, ext->argument ? " " : "", ext->argument ? ext->argument : "");
            goto cleanup;
        }
    }

    /* TODO store inherited data, e.g. status first, but mark them somehow to allow to overwrite them and not detect duplicity */

    /* keep order of the processing the same as the order in the defined substmts,
     * the order is important for some of the statements depending on others (e.g. type needs status and units) */
    for (u = 0; substmts[u].stmt; ++u) {
        ly_bool stmt_present = 0;

        for (stmt = ext->child; stmt; stmt = stmt->next) {
            if (substmts[u].stmt != stmt->kw) {
                continue;
            }

            stmt_present = 1;
            if (substmts[u].storage) {
                switch (stmt->kw) {
                case LY_STMT_STATUS:
                    assert(substmts[u].cardinality < LY_STMT_CARD_SOME);
                    LY_CHECK_ERR_GOTO(r = lysp_stmt_parse(ctx, stmt, stmt->kw, &substmts[u].storage, /* TODO */ NULL), ret = r, cleanup);
                    break;
                case LY_STMT_UNITS: {
                    const char **units;

                    if (substmts[u].cardinality < LY_STMT_CARD_SOME) {
                        /* single item */
                        if (*((const char **)substmts[u].storage)) {
                            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LY_VCODE_DUPSTMT, stmt->stmt);
                            goto cleanup;
                        }
                        units = (const char **)substmts[u].storage;
                    } else {
                        /* sized array */
                        const char ***units_array = (const char ***)substmts[u].storage;
                        LY_ARRAY_NEW_GOTO(ctx->ctx, *units_array, units, ret, cleanup);
                    }
                    r = lydict_insert(ctx->ctx, stmt->arg, 0, units);
                    LY_CHECK_ERR_GOTO(r, ret = r, cleanup);
                    break;
                }
                case LY_STMT_TYPE: {
                    uint16_t *flags = lys_compile_extension_instance_storage(LY_STMT_STATUS, substmts);
                    const char **units = lys_compile_extension_instance_storage(LY_STMT_UNITS, substmts);

                    if (substmts[u].cardinality < LY_STMT_CARD_SOME) {
                        /* single item */
                        if (*(struct lysc_type **)substmts[u].storage) {
                            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LY_VCODE_DUPSTMT, stmt->stmt);
                            goto cleanup;
                        }
                        compiled = substmts[u].storage;
                    } else {
                        /* sized array */
                        struct lysc_type ***types = (struct lysc_type ***)substmts[u].storage, **type = NULL;
                        LY_ARRAY_NEW_GOTO(ctx->ctx, *types, type, ret, cleanup);
                        compiled = (void *)type;
                    }

                    LY_CHECK_ERR_GOTO(r = lysp_stmt_parse(ctx, stmt, stmt->kw, &parsed, NULL), ret = r, cleanup);
                    LY_CHECK_ERR_GOTO(r = lys_compile_type(ctx, ext->parent_type == LYEXT_PAR_NODE ? ((struct lysc_node *)ext->parent)->sp : NULL,
                            flags ? *flags : 0, ctx->mod_def->parsed, ext->name, parsed, (struct lysc_type **)compiled,
                            units && !*units ? units : NULL, NULL), lysp_type_free(ctx->ctx, parsed); free(parsed); ret = r, cleanup);
                    lysp_type_free(ctx->ctx, parsed);
                    free(parsed);
                    break;
                }
                case LY_STMT_IF_FEATURE: {
                    struct lysc_iffeature *iff = NULL;

                    if (substmts[u].cardinality < LY_STMT_CARD_SOME) {
                        /* single item */
                        if (((struct lysc_iffeature *)substmts[u].storage)->features) {
                            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LY_VCODE_DUPSTMT, stmt->stmt);
                            goto cleanup;
                        }
                        iff = (struct lysc_iffeature *)substmts[u].storage;
                    } else {
                        /* sized array */
                        struct lysc_iffeature **iffs = (struct lysc_iffeature **)substmts[u].storage;
                        LY_ARRAY_NEW_GOTO(ctx->ctx, *iffs, iff, ret, cleanup);
                    }
                    qname.str = stmt->arg;
                    qname.mod = ctx->mod_def;
                    LY_CHECK_ERR_GOTO(r = lys_compile_iffeature(ctx, &qname, iff), ret = r, cleanup);
                    break;
                }
                /* TODO support other substatements (parse stmt to lysp and then compile lysp to lysc),
                 * also note that in many statements their extensions are not taken into account  */
                default:
                    LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG, "Statement \"%s\" is not supported as an extension (found in \"%s%s%s\") substatement.",
                            stmt->stmt, ext->name, ext->argument ? " " : "", ext->argument ? ext->argument : "");
                    goto cleanup;
                }
            }
        }

        if (((substmts[u].cardinality == LY_STMT_CARD_MAND) || (substmts[u].cardinality == LY_STMT_CARD_SOME)) && !stmt_present) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SYNTAX_YANG, "Missing mandatory keyword \"%s\" as a child of \"%s%s%s\".",
                    ly_stmt2str(substmts[u].stmt), ext->name, ext->argument ? " " : "", ext->argument ? ext->argument : "");
            goto cleanup;
        }
    }

    ret = LY_SUCCESS;

cleanup:
    return ret;
}

/**
 * @brief Check when for cyclic dependencies.
 *
 * @param[in] set Set with all the referenced nodes.
 * @param[in] node Node whose "when" referenced nodes are in @p set.
 * @return LY_ERR value
 */
static LY_ERR
lys_compile_unres_when_cyclic(struct lyxp_set *set, const struct lysc_node *node)
{
    struct lyxp_set tmp_set;
    struct lyxp_set_scnode *xp_scnode;
    uint32_t i, j;
    LY_ARRAY_COUNT_TYPE u;
    struct lysc_when *when;
    LY_ERR ret = LY_SUCCESS;

    memset(&tmp_set, 0, sizeof tmp_set);

    /* prepare in_ctx of the set */
    for (i = 0; i < set->used; ++i) {
        xp_scnode = &set->val.scnodes[i];

        if (xp_scnode->in_ctx != -1) {
            /* check node when, skip the context node (it was just checked) */
            xp_scnode->in_ctx = 1;
        }
    }

    for (i = 0; i < set->used; ++i) {
        xp_scnode = &set->val.scnodes[i];
        if (xp_scnode->in_ctx != 1) {
            /* already checked */
            continue;
        }

        if ((xp_scnode->type != LYXP_NODE_ELEM) || (xp_scnode->scnode->nodetype & (LYS_RPC | LYS_ACTION | LYS_NOTIF)) ||
                !xp_scnode->scnode->when) {
            /* no when to check */
            xp_scnode->in_ctx = 0;
            continue;
        }

        node = xp_scnode->scnode;
        do {
            LY_ARRAY_FOR(node->when, u) {
                when = node->when[u];
                ret = lyxp_atomize(when->cond, LY_PREF_SCHEMA, when->module, when->context,
                        when->context ? LYXP_NODE_ELEM : LYXP_NODE_ROOT_CONFIG, &tmp_set, LYXP_SCNODE_SCHEMA);
                if (ret != LY_SUCCESS) {
                    LOGVAL(set->ctx, LY_VLOG_LYSC, node, LYVE_SEMANTICS, "Invalid when condition \"%s\".", when->cond->expr);
                    goto cleanup;
                }

                for (j = 0; j < tmp_set.used; ++j) {
                    /* skip roots'n'stuff */
                    if (tmp_set.val.scnodes[j].type == LYXP_NODE_ELEM) {
                        /* try to find this node in our set */
                        uint32_t idx;
                        if (lyxp_set_scnode_contains(set, tmp_set.val.scnodes[j].scnode, LYXP_NODE_ELEM, -1, &idx) && (set->val.scnodes[idx].in_ctx == -1)) {
                            LOGVAL(set->ctx, LY_VLOG_LYSC, node, LY_VCODE_CIRC_WHEN, node->name, set->val.scnodes[idx].scnode->name);
                            ret = LY_EVALID;
                            goto cleanup;
                        }

                        /* needs to be checked, if in both sets, will be ignored */
                        tmp_set.val.scnodes[j].in_ctx = 1;
                    } else {
                        /* no when, nothing to check */
                        tmp_set.val.scnodes[j].in_ctx = 0;
                    }
                }

                /* merge this set into the global when set */
                lyxp_set_scnode_merge(set, &tmp_set);
            }

            /* check when of non-data parents as well */
            node = node->parent;
        } while (node && (node->nodetype & (LYS_CASE | LYS_CHOICE)));

        /* this node when was checked (xp_scnode could have been reallocd) */
        set->val.scnodes[i].in_ctx = -1;
    }

cleanup:
    lyxp_set_free_content(&tmp_set);
    return ret;
}

/**
 * @brief Check when/must expressions of a node on a complete compiled schema tree.
 *
 * @param[in] ctx Compile context.
 * @param[in] node Node to check.
 * @return LY_ERR value
 */
static LY_ERR
lys_compile_unres_xpath(struct lysc_ctx *ctx, const struct lysc_node *node)
{
    struct lyxp_set tmp_set;
    uint32_t i;
    LY_ARRAY_COUNT_TYPE u;
    uint32_t opts;
    ly_bool input_done = 0;
    struct lysc_when **when = NULL;
    struct lysc_must *musts = NULL;
    LY_ERR ret = LY_SUCCESS;
    const struct lysc_node *op;

    memset(&tmp_set, 0, sizeof tmp_set);
    opts = LYXP_SCNODE_SCHEMA;
    if (node->flags & LYS_CONFIG_R) {
        for (op = node->parent; op && !(op->nodetype & (LYS_RPC | LYS_ACTION)); op = op->parent) {}
        if (op) {
            /* we are actually in output */
            opts = LYXP_SCNODE_OUTPUT;
        }
    }

    switch (node->nodetype) {
    case LYS_CONTAINER:
        when = ((struct lysc_node_container *)node)->when;
        musts = ((struct lysc_node_container *)node)->musts;
        break;
    case LYS_CHOICE:
        when = ((struct lysc_node_choice *)node)->when;
        break;
    case LYS_LEAF:
        when = ((struct lysc_node_leaf *)node)->when;
        musts = ((struct lysc_node_leaf *)node)->musts;
        break;
    case LYS_LEAFLIST:
        when = ((struct lysc_node_leaflist *)node)->when;
        musts = ((struct lysc_node_leaflist *)node)->musts;
        break;
    case LYS_LIST:
        when = ((struct lysc_node_list *)node)->when;
        musts = ((struct lysc_node_list *)node)->musts;
        break;
    case LYS_ANYXML:
    case LYS_ANYDATA:
        when = ((struct lysc_node_anydata *)node)->when;
        musts = ((struct lysc_node_anydata *)node)->musts;
        break;
    case LYS_CASE:
        when = ((struct lysc_node_case *)node)->when;
        break;
    case LYS_NOTIF:
        when = ((struct lysc_notif *)node)->when;
        musts = ((struct lysc_notif *)node)->musts;
        break;
    case LYS_RPC:
    case LYS_ACTION:
        /* first process when and input musts */
        when = ((struct lysc_action *)node)->when;
        musts = ((struct lysc_action *)node)->input.musts;
        break;
    default:
        /* nothing to check */
        break;
    }

    /* check "when" */
    LY_ARRAY_FOR(when, u) {
        ret = lyxp_atomize(when[u]->cond, LY_PREF_SCHEMA, when[u]->module, when[u]->context ? when[u]->context : node,
                when[u]->context ? LYXP_NODE_ELEM : LYXP_NODE_ROOT_CONFIG, &tmp_set, opts);
        if (ret != LY_SUCCESS) {
            LOGVAL(ctx->ctx, LY_VLOG_LYSC, node, LYVE_SEMANTICS, "Invalid when condition \"%s\".", when[u]->cond->expr);
            goto cleanup;
        }

        ctx->path[0] = '\0';
        lysc_path((struct lysc_node *)node, LYSC_PATH_LOG, ctx->path, LYSC_CTX_BUFSIZE);
        for (i = 0; i < tmp_set.used; ++i) {
            /* skip roots'n'stuff */
            if ((tmp_set.val.scnodes[i].type == LYXP_NODE_ELEM) && (tmp_set.val.scnodes[i].in_ctx != -1)) {
                struct lysc_node *schema = tmp_set.val.scnodes[i].scnode;

                /* XPath expression cannot reference "lower" status than the node that has the definition */
                ret = lysc_check_status(ctx, when[u]->flags, when[u]->module, node->name, schema->flags, schema->module,
                        schema->name);
                LY_CHECK_GOTO(ret, cleanup);

                /* check dummy node accessing */
                if (schema == node) {
                    LOGVAL(ctx->ctx, LY_VLOG_LYSC, node, LY_VCODE_DUMMY_WHEN, node->name);
                    ret = LY_EVALID;
                    goto cleanup;
                }
            }
        }

        /* check cyclic dependencies */
        ret = lys_compile_unres_when_cyclic(&tmp_set, node);
        LY_CHECK_GOTO(ret, cleanup);

        lyxp_set_free_content(&tmp_set);
    }

check_musts:
    /* check "must" */
    LY_ARRAY_FOR(musts, u) {
        ret = lyxp_atomize(musts[u].cond, LY_PREF_SCHEMA, musts[u].module, node, LYXP_NODE_ELEM, &tmp_set, opts);
        if (ret != LY_SUCCESS) {
            LOGVAL(ctx->ctx, LY_VLOG_LYSC, node, LYVE_SEMANTICS, "Invalid must restriction \"%s\".", musts[u].cond->expr);
            goto cleanup;
        }

        ctx->path[0] = '\0';
        lysc_path((struct lysc_node *)node, LYSC_PATH_LOG, ctx->path, LYSC_CTX_BUFSIZE);
        for (i = 0; i < tmp_set.used; ++i) {
            /* skip roots'n'stuff */
            if (tmp_set.val.scnodes[i].type == LYXP_NODE_ELEM) {
                /* XPath expression cannot reference "lower" status than the node that has the definition */
                ret = lysc_check_status(ctx, node->flags, musts[u].module, node->name, tmp_set.val.scnodes[i].scnode->flags,
                        tmp_set.val.scnodes[i].scnode->module, tmp_set.val.scnodes[i].scnode->name);
                LY_CHECK_GOTO(ret, cleanup);
            }
        }

        lyxp_set_free_content(&tmp_set);
    }

    if ((node->nodetype & (LYS_RPC | LYS_ACTION)) && !input_done) {
        /* now check output musts */
        input_done = 1;
        when = NULL;
        musts = ((struct lysc_action *)node)->output.musts;
        opts = LYXP_SCNODE_OUTPUT;
        goto check_musts;
    }

cleanup:
    lyxp_set_free_content(&tmp_set);
    return ret;
}

/**
 * @brief Check leafref for its target existence on a complete compiled schema tree.
 *
 * @param[in] ctx Compile context.
 * @param[in] node Context node for the leafref.
 * @param[in] lref Leafref to resolve.
 * @return LY_ERR value.
 */
static LY_ERR
lys_compile_unres_leafref(struct lysc_ctx *ctx, const struct lysc_node *node, struct lysc_type_leafref *lref)
{
    const struct lysc_node *target = NULL, *siter;
    struct ly_path *p;
    struct lysc_type *type;

    assert(node->nodetype & (LYS_LEAF | LYS_LEAFLIST));

    /* try to find the target */
    LY_CHECK_RET(ly_path_compile(ctx->ctx, node->module, node, lref->path, LY_PATH_LREF_TRUE,
            lysc_is_output(node) ? LY_PATH_OPER_OUTPUT : LY_PATH_OPER_INPUT, LY_PATH_TARGET_MANY,
            LY_PREF_SCHEMA, (void *)lref->path_mod, &p));

    /* get the target node */
    target = p[LY_ARRAY_COUNT(p) - 1].node;
    ly_path_free(node->module->ctx, p);

    if (!(target->nodetype & (LYS_LEAF | LYS_LEAFLIST))) {
        LOGVAL(ctx->ctx, LY_VLOG_LYSC, node, LYVE_REFERENCE,
                "Invalid leafref path \"%s\" - target node is %s instead of leaf or leaf-list.",
                lref->path->expr, lys_nodetype2str(target->nodetype));
        return LY_EVALID;
    }

    /* check status */
    ctx->path[0] = '\0';
    lysc_path(node, LYSC_PATH_LOG, ctx->path, LYSC_CTX_BUFSIZE);
    ctx->path_len = strlen(ctx->path);
    if (lysc_check_status(ctx, node->flags, node->module, node->name, target->flags, target->module, target->name)) {
        return LY_EVALID;
    }
    ctx->path_len = 1;
    ctx->path[1] = '\0';

    /* check config */
    if (lref->require_instance) {
        for (siter = node->parent; siter && !(siter->nodetype & (LYS_RPC | LYS_ACTION | LYS_NOTIF)); siter = siter->parent) {}
        if (!siter && (node->flags & LYS_CONFIG_W) && (target->flags & LYS_CONFIG_R)) {
            LOGVAL(ctx->ctx, LY_VLOG_LYSC, node, LYVE_REFERENCE, "Invalid leafref path \"%s\" - target is supposed"
                    " to represent configuration data (as the leafref does), but it does not.", lref->path->expr);
            return LY_EVALID;
        }
    }

    /* store the target's type and check for circular chain of leafrefs */
    lref->realtype = ((struct lysc_node_leaf *)target)->type;
    for (type = lref->realtype; type && type->basetype == LY_TYPE_LEAFREF; type = ((struct lysc_type_leafref *)type)->realtype) {
        if (type == (struct lysc_type *)lref) {
            /* circular chain detected */
            LOGVAL(ctx->ctx, LY_VLOG_LYSC, node, LYVE_REFERENCE,
                    "Invalid leafref path \"%s\" - circular chain of leafrefs detected.", lref->path->expr);
            return LY_EVALID;
        }
    }

    /* check if leafref and its target are under common if-features */
    if (lys_compile_leafref_features_validate(node, target)) {
        LOGVAL(ctx->ctx, LY_VLOG_LYSC, node, LYVE_REFERENCE,
                "Invalid leafref path \"%s\" - set of features applicable to the leafref target is not a subset of"
                " features applicable to the leafref itself.", lref->path->expr);
        return LY_EVALID;
    }

    return LY_SUCCESS;
}

static LY_ERR
lys_compile_ietf_netconf_wd_annotation(struct lysc_ctx *ctx, struct lys_module *mod)
{
    struct lysc_ext_instance *ext;
    struct lysp_ext_instance *ext_p = NULL;
    struct lysp_stmt *stmt;
    const struct lys_module *ext_mod;
    LY_ERR ret = LY_SUCCESS;

    /* create the parsed extension instance manually */
    ext_p = calloc(1, sizeof *ext_p);
    LY_CHECK_ERR_GOTO(!ext_p, LOGMEM(ctx->ctx); ret = LY_EMEM, cleanup);
    LY_CHECK_GOTO(ret = lydict_insert(ctx->ctx, "md:annotation", 0, &ext_p->name), cleanup);
    LY_CHECK_GOTO(ret = lydict_insert(ctx->ctx, "default", 0, &ext_p->argument), cleanup);
    ext_p->insubstmt = LYEXT_SUBSTMT_SELF;
    ext_p->insubstmt_index = 0;

    ext_p->child = stmt = calloc(1, sizeof *ext_p->child);
    LY_CHECK_ERR_GOTO(!stmt, LOGMEM(ctx->ctx); ret = LY_EMEM, cleanup);
    LY_CHECK_GOTO(ret = lydict_insert(ctx->ctx, "type", 0, &stmt->stmt), cleanup);
    LY_CHECK_GOTO(ret = lydict_insert(ctx->ctx, "boolean", 0, &stmt->arg), cleanup);
    stmt->kw = LY_STMT_TYPE;

    /* allocate new extension instance */
    LY_ARRAY_NEW_GOTO(mod->ctx, mod->compiled->exts, ext, ret, cleanup);

    /* manually get extension definition module */
    ext_mod = ly_ctx_get_module_latest(ctx->ctx, "ietf-yang-metadata");

    /* compile the extension instance */
    LY_CHECK_GOTO(ret = lys_compile_ext(ctx, ext_p, ext, mod->compiled, LYEXT_PAR_MODULE, ext_mod), cleanup);

cleanup:
    lysp_ext_instance_free(ctx->ctx, ext_p);
    free(ext_p);
    return ret;
}

/**
 * @brief Compile default value(s) for leaf or leaf-list expecting a complete compiled schema tree.
 *
 * @param[in] ctx Compile context.
 * @param[in] node Leaf or leaf-list to compile the default value(s) for.
 * @param[in] type Type of the default value.
 * @param[in] dflt Default value.
 * @param[in] dflt_mod Local module for @p dflt.
 * @param[in,out] storage Storage for the compiled default value.
 * @return LY_ERR value.
 */
static LY_ERR
lys_compile_unres_dflt(struct lysc_ctx *ctx, struct lysc_node *node, struct lysc_type *type, const char *dflt,
        const struct lys_module *dflt_mod, struct lyd_value *storage)
{
    LY_ERR ret;
    struct ly_err_item *err = NULL;

    ret = type->plugin->store(ctx->ctx, type, dflt, strlen(dflt), 0, LY_PREF_SCHEMA, (void *)dflt_mod, LYD_HINT_SCHEMA,
            node, storage, &err);
    if (ret == LY_EINCOMPLETE) {
        /* we have no data so we will not be resolving it */
        ret = LY_SUCCESS;
    }

    if (ret) {
        ctx->path[0] = '\0';
        lysc_path(node, LYSC_PATH_LOG, ctx->path, LYSC_CTX_BUFSIZE);
        if (err) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SEMANTICS,
                    "Invalid default - value does not fit the type (%s).", err->msg);
            ly_err_free(err);
        } else {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SEMANTICS,
                    "Invalid default - value does not fit the type.");
        }
        return ret;
    }

    ++((struct lysc_type *)storage->realtype)->refcount;
    return LY_SUCCESS;
}

/**
 * @brief Compile default value of a leaf expecting a complete compiled schema tree.
 *
 * @param[in] ctx Compile context.
 * @param[in] leaf Leaf that the default value is for.
 * @param[in] dflt Default value to compile.
 * @return LY_ERR value.
 */
static LY_ERR
lys_compile_unres_leaf_dlft(struct lysc_ctx *ctx, struct lysc_node_leaf *leaf, struct lysp_qname *dflt)
{
    LY_ERR ret;

    assert(!leaf->dflt);

    if (leaf->flags & (LYS_MAND_TRUE | LYS_KEY)) {
        /* ignore default values for keys and mandatory leaves */
        return LY_SUCCESS;
    }

    /* allocate the default value */
    leaf->dflt = calloc(1, sizeof *leaf->dflt);
    LY_CHECK_ERR_RET(!leaf->dflt, LOGMEM(ctx->ctx), LY_EMEM);

    /* store the default value */
    ret = lys_compile_unres_dflt(ctx, (struct lysc_node *)leaf, leaf->type, dflt->str, dflt->mod, leaf->dflt);
    if (ret) {
        free(leaf->dflt);
        leaf->dflt = NULL;
    }

    return ret;
}

/**
 * @brief Compile default values of a leaf-list expecting a complete compiled schema tree.
 *
 * @param[in] ctx Compile context.
 * @param[in] llist Leaf-list that the default value(s) are for.
 * @param[in] dflt Default value to compile, in case of a single value.
 * @param[in] dflts Sized array of default values, in case of more values.
 * @return LY_ERR value.
 */
static LY_ERR
lys_compile_unres_llist_dflts(struct lysc_ctx *ctx, struct lysc_node_leaflist *llist, struct lysp_qname *dflt,
        struct lysp_qname *dflts)
{
    LY_ERR ret;
    LY_ARRAY_COUNT_TYPE orig_count, u, v;

    assert(dflt || dflts);

    if (llist->dflts) {
        /* there were already some defaults and we are adding new by deviations */
        assert(dflts);
        orig_count = LY_ARRAY_COUNT(llist->dflts);
    } else {
        orig_count = 0;
    }

    /* allocate new items */
    if (dflts) {
        LY_ARRAY_CREATE_RET(ctx->ctx, llist->dflts, orig_count + LY_ARRAY_COUNT(dflts), LY_EMEM);
    } else {
        LY_ARRAY_CREATE_RET(ctx->ctx, llist->dflts, orig_count + 1, LY_EMEM);
    }

    /* fill each new default value */
    if (dflts) {
        LY_ARRAY_FOR(dflts, u) {
            llist->dflts[orig_count + u] = calloc(1, sizeof **llist->dflts);
            ret = lys_compile_unres_dflt(ctx, (struct lysc_node *)llist, llist->type, dflts[u].str, dflts[u].mod,
                    llist->dflts[orig_count + u]);
            LY_CHECK_ERR_RET(ret, free(llist->dflts[orig_count + u]), ret);
            LY_ARRAY_INCREMENT(llist->dflts);
        }
    } else {
        llist->dflts[orig_count] = calloc(1, sizeof **llist->dflts);
        ret = lys_compile_unres_dflt(ctx, (struct lysc_node *)llist, llist->type, dflt->str, dflt->mod,
                llist->dflts[orig_count]);
        LY_CHECK_ERR_RET(ret, free(llist->dflts[orig_count]), ret);
        LY_ARRAY_INCREMENT(llist->dflts);
    }

    /* check default value uniqueness */
    if (llist->flags & LYS_CONFIG_W) {
        /* configuration data values must be unique - so check the default values */
        for (u = orig_count; u < LY_ARRAY_COUNT(llist->dflts); ++u) {
            for (v = 0; v < u; ++v) {
                if (!llist->dflts[u]->realtype->plugin->compare(llist->dflts[u], llist->dflts[v])) {
                    lysc_update_path(ctx, llist->parent, llist->name);
                    LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_SEMANTICS,
                            "Configuration leaf-list has multiple defaults of the same value \"%s\".",
                            llist->dflts[u]->canonical);
                    lysc_update_path(ctx, NULL, NULL);
                    return LY_EVALID;
                }
            }
        }
    }

    return LY_SUCCESS;
}

/**
 * @brief Finish compilation of all the unres sets of a compile context.
 *
 * @param[in] ctx Compile context with unres sets.
 * @return LY_ERR value.
 */
static LY_ERR
lys_compile_unres(struct lysc_ctx *ctx)
{
    struct lysc_node *node;
    struct lysc_type *type, *typeiter;
    struct lysc_type_leafref *lref;
    struct lysc_augment *aug;
    struct lysc_deviation *dev;
    LY_ARRAY_COUNT_TYPE v;
    uint32_t i;

    /* for leafref, we need 2 rounds - first detects circular chain by storing the first referred type (which
     * can be also leafref, in case it is already resolved, go through the chain and check that it does not
     * point to the starting leafref type). The second round stores the first non-leafref type for later data validation. */
    for (i = 0; i < ctx->leafrefs.count; ++i) {
        node = ctx->leafrefs.objs[i];
        assert(node->nodetype & (LYS_LEAF | LYS_LEAFLIST));
        type = ((struct lysc_node_leaf *)node)->type;
        if (type->basetype == LY_TYPE_LEAFREF) {
            LY_CHECK_RET(lys_compile_unres_leafref(ctx, node, (struct lysc_type_leafref *)type));
        } else if (type->basetype == LY_TYPE_UNION) {
            LY_ARRAY_FOR(((struct lysc_type_union *)type)->types, v) {
                if (((struct lysc_type_union *)type)->types[v]->basetype == LY_TYPE_LEAFREF) {
                    lref = (struct lysc_type_leafref *)((struct lysc_type_union *)type)->types[v];
                    LY_CHECK_RET(lys_compile_unres_leafref(ctx, node, lref));
                }
            }
        }
    }
    for (i = 0; i < ctx->leafrefs.count; ++i) {
        /* store pointer to the real type */
        type = ((struct lysc_node_leaf *)ctx->leafrefs.objs[i])->type;
        if (type->basetype == LY_TYPE_LEAFREF) {
            for (typeiter = ((struct lysc_type_leafref *)type)->realtype;
                    typeiter->basetype == LY_TYPE_LEAFREF;
                    typeiter = ((struct lysc_type_leafref *)typeiter)->realtype) {}
            ((struct lysc_type_leafref *)type)->realtype = typeiter;
        } else if (type->basetype == LY_TYPE_UNION) {
            LY_ARRAY_FOR(((struct lysc_type_union *)type)->types, v) {
                if (((struct lysc_type_union *)type)->types[v]->basetype == LY_TYPE_LEAFREF) {
                    for (typeiter = ((struct lysc_type_leafref *)((struct lysc_type_union *)type)->types[v])->realtype;
                            typeiter->basetype == LY_TYPE_LEAFREF;
                            typeiter = ((struct lysc_type_leafref *)typeiter)->realtype) {}
                    ((struct lysc_type_leafref *)((struct lysc_type_union *)type)->types[v])->realtype = typeiter;
                }
            }
        }
    }

    /* check xpath */
    for (i = 0; i < ctx->xpath.count; ++i) {
        LY_CHECK_RET(lys_compile_unres_xpath(ctx, ctx->xpath.objs[i]));
    }

    /* finish incomplete default values compilation */
    for (i = 0; i < ctx->dflts.count; ++i) {
        struct lysc_unres_dflt *r = ctx->dflts.objs[i];
        if (r->leaf->nodetype == LYS_LEAF) {
            LY_CHECK_RET(lys_compile_unres_leaf_dlft(ctx, r->leaf, r->dflt));
        } else {
            LY_CHECK_RET(lys_compile_unres_llist_dflts(ctx, r->llist, r->dflt, r->dflts));
        }
    }

    /* check that all augments were applied */
    for (i = 0; i < ctx->augs.count; ++i) {
        aug = ctx->augs.objs[i];
        LOGVAL(ctx->ctx, LY_VLOG_NONE, NULL, LYVE_REFERENCE,
                "Augment target node \"%s\" from module \"%s\" was not found.", aug->nodeid->expr,
                aug->nodeid_mod->name);
    }
    if (ctx->augs.count) {
        return LY_ENOTFOUND;
    }

    /* check that all deviations were applied */
    for (i = 0; i < ctx->devs.count; ++i) {
        dev = ctx->devs.objs[i];
        LOGVAL(ctx->ctx, LY_VLOG_NONE, NULL, LYVE_REFERENCE,
                "Deviation(s) target node \"%s\" from module \"%s\" was not found.", dev->nodeid->expr,
                dev->nodeid_mod->name);
    }
    if (ctx->devs.count) {
        return LY_ENOTFOUND;
    }

    return LY_SUCCESS;
}

void
lys_precompile_augments_deviations_revert(struct ly_ctx *ctx, const struct lys_module *mod)
{
    uint32_t i;
    LY_ARRAY_COUNT_TYPE u, count;
    struct lys_module *m;

    for (i = 0; i < ctx->list.count; ++i) {
        m = ctx->list.objs[i];

        if (m->augmented_by) {
            count = LY_ARRAY_COUNT(m->augmented_by);
            for (u = 0; u < count; ++u) {
                if (m->augmented_by[u] == mod) {
                    /* keep the order */
                    if (u < count - 1) {
                        memmove(m->augmented_by + u, m->augmented_by + u + 1, (count - u) * sizeof *m->augmented_by);
                    }
                    LY_ARRAY_DECREMENT(m->augmented_by);
                    break;
                }
            }
            if (!LY_ARRAY_COUNT(m->augmented_by)) {
                LY_ARRAY_FREE(m->augmented_by);
                m->augmented_by = NULL;
            }
        }

        if (m->deviated_by) {
            count = LY_ARRAY_COUNT(m->deviated_by);
            for (u = 0; u < count; ++u) {
                if (m->deviated_by[u] == mod) {
                    /* keep the order */
                    if (u < count - 1) {
                        memmove(m->deviated_by + u, m->deviated_by + u + 1, (count - u) * sizeof *m->deviated_by);
                    }
                    LY_ARRAY_DECREMENT(m->deviated_by);
                    break;
                }
            }
            if (!LY_ARRAY_COUNT(m->deviated_by)) {
                LY_ARRAY_FREE(m->deviated_by);
                m->deviated_by = NULL;
            }
        }
    }
}

/**
 * @brief Compile features in the current module and all its submodules.
 *
 * @param[in] ctx Compile context.
 * @return LY_ERR value.
 */
static LY_ERR
lys_compile_features(struct lysc_ctx *ctx)
{
    struct lysp_submodule *submod;
    LY_ARRAY_COUNT_TYPE u, v;

    if (!ctx->mod->features) {
        /* features are compiled directly into the module structure,
         * but it must be done in two steps to allow forward references (via if-feature) between the features themselves */
        LY_CHECK_RET(lys_feature_precompile(ctx, NULL, NULL, ctx->mod->parsed->features, &ctx->mod->features));
        LY_ARRAY_FOR(ctx->mod->parsed->includes, v) {
            submod = ctx->mod->parsed->includes[v].submodule;
            LY_CHECK_RET(lys_feature_precompile(ctx, NULL, NULL, submod->features, &ctx->mod->features));
        }
    }

    /* finish feature compilation, not only for the main module, but also for the submodules.
     * Due to possible forward references, it must be done when all the features (including submodules)
     * are present. */
    LY_ARRAY_FOR(ctx->mod->parsed->features, u) {
        LY_CHECK_RET(lys_feature_precompile_finish(ctx, &ctx->mod->parsed->features[u], ctx->mod->features));
    }

    lysc_update_path(ctx, NULL, "{submodule}");
    LY_ARRAY_FOR(ctx->mod->parsed->includes, v) {
        submod = ctx->mod->parsed->includes[v].submodule;

        lysc_update_path(ctx, NULL, submod->name);
        LY_ARRAY_FOR(submod->features, u) {
            LY_CHECK_RET(lys_feature_precompile_finish(ctx, &submod->features[u], ctx->mod->features));
        }
        lysc_update_path(ctx, NULL, NULL);
    }
    lysc_update_path(ctx, NULL, NULL);

    return LY_SUCCESS;
}

/**
 * @brief Compile identites in the current module and all its submodules.
 *
 * @param[in] ctx Compile context.
 * @return LY_ERR value.
 */
static LY_ERR
lys_compile_identities(struct lysc_ctx *ctx)
{
    struct lysp_submodule *submod;
    LY_ARRAY_COUNT_TYPE u;

    if (!ctx->mod->identities) {
        LY_CHECK_RET(lys_identity_precompile(ctx, NULL, NULL, ctx->mod->parsed->identities, &ctx->mod->identities));
        LY_ARRAY_FOR(ctx->mod->parsed->includes, u) {
            submod = ctx->mod->parsed->includes[u].submodule;
            LY_CHECK_RET(lys_identity_precompile(ctx, NULL, NULL, submod->identities, &ctx->mod->identities));
        }
    }

    if (ctx->mod->parsed->identities) {
        LY_CHECK_RET(lys_compile_identities_derived(ctx, ctx->mod->parsed->identities, ctx->mod->identities));
    }
    lysc_update_path(ctx, NULL, "{submodule}");
    LY_ARRAY_FOR(ctx->mod->parsed->includes, u) {

        submod = ctx->mod->parsed->includes[u].submodule;
        if (submod->identities) {
            lysc_update_path(ctx, NULL, submod->name);
            LY_CHECK_RET(lys_compile_identities_derived(ctx, submod->identities, ctx->mod->identities));
            lysc_update_path(ctx, NULL, NULL);
        }
    }
    lysc_update_path(ctx, NULL, NULL);

    return LY_SUCCESS;
}

LY_ERR
lys_compile(struct lys_module *mod, uint32_t options)
{
    struct lysc_ctx ctx = {0};
    struct lysc_module *mod_c;
    struct lysp_module *sp;
    struct lysp_submodule *submod;
    struct lysp_node *pnode;
    struct lysp_grp *grps;
    LY_ARRAY_COUNT_TYPE u, v;
    uint32_t i;
    LY_ERR ret = LY_SUCCESS;

    LY_CHECK_ARG_RET(NULL, mod, mod->parsed, !mod->compiled, mod->ctx, LY_EINVAL);

    if (!mod->implemented) {
        /* just imported modules are not compiled */
        return LY_SUCCESS;
    }

    /* context will be changed */
    ++mod->ctx->module_set_id;

    sp = mod->parsed;

    ctx.ctx = mod->ctx;
    ctx.mod = mod;
    ctx.mod_def = mod;
    ctx.options = options;
    ctx.path_len = 1;
    ctx.path[0] = '/';

    mod->compiled = mod_c = calloc(1, sizeof *mod_c);
    LY_CHECK_ERR_RET(!mod_c, LOGMEM(mod->ctx), LY_EMEM);
    mod_c->mod = mod;

    /* process imports */
    LY_ARRAY_FOR(sp->imports, u) {
        LY_CHECK_GOTO(ret = lys_compile_import(&ctx, &sp->imports[u]), error);
    }

    /* features */
    LY_CHECK_GOTO(ret = lys_compile_features(&ctx), error);

    /* identities, work similarly to features with the precompilation */
    LY_CHECK_GOTO(ret = lys_compile_identities(&ctx), error);

    /* augments and deviations */
    LY_CHECK_GOTO(ret = lys_precompile_augments_deviations(&ctx), error);

    /* compile augments and deviations of our module from other modules so they can be applied during compilation */
    LY_CHECK_GOTO(ret = lys_precompile_own_augments(&ctx), error);
    LY_CHECK_GOTO(ret = lys_precompile_own_deviations(&ctx), error);

    /* data nodes */
    LY_LIST_FOR(sp->data, pnode) {
        LY_CHECK_GOTO(ret = lys_compile_node(&ctx, pnode, NULL, 0, NULL), error);
    }

    /* top-level RPCs and notifications */
    COMPILE_OP_ARRAY_GOTO(&ctx, sp->rpcs, mod_c->rpcs, NULL, u, lys_compile_action, 0, ret, error);
    COMPILE_OP_ARRAY_GOTO(&ctx, sp->notifs, mod_c->notifs, NULL, u, lys_compile_notif, 0, ret, error);

    /* extension instances */
    COMPILE_EXTS_GOTO(&ctx, sp->exts, mod_c->exts, mod_c, LYEXT_PAR_MODULE, ret, error);

    /* the same for submodules */
    LY_ARRAY_FOR(sp->includes, u) {
        submod = sp->includes[u].submodule;
        LY_LIST_FOR(submod->data, pnode) {
            ret = lys_compile_node(&ctx, pnode, NULL, 0, NULL);
            LY_CHECK_GOTO(ret, error);
        }

        COMPILE_OP_ARRAY_GOTO(&ctx, submod->rpcs, mod_c->rpcs, NULL, v, lys_compile_action, 0, ret, error);
        COMPILE_OP_ARRAY_GOTO(&ctx, submod->notifs, mod_c->notifs, NULL, v, lys_compile_notif, 0, ret, error);

        COMPILE_EXTS_GOTO(&ctx, submod->exts, mod_c->exts, mod_c, LYEXT_PAR_MODULE, ret, error);
    }

    /* finish compilation for all unresolved items in the context */
    LY_CHECK_GOTO(ret = lys_compile_unres(&ctx), error);

    /* validate non-instantiated groupings from the parsed schema,
     * without it we would accept even the schemas with invalid grouping specification */
    ctx.options |= LYS_COMPILE_GROUPING;
    LY_ARRAY_FOR(sp->groupings, u) {
        if (!(sp->groupings[u].flags & LYS_USED_GRP)) {
            LY_CHECK_GOTO(ret = lys_compile_grouping(&ctx, pnode, &sp->groupings[u]), error);
        }
    }
    LY_LIST_FOR(sp->data, pnode) {
        grps = (struct lysp_grp *)lysp_node_groupings(pnode);
        LY_ARRAY_FOR(grps, u) {
            if (!(grps[u].flags & LYS_USED_GRP)) {
                LY_CHECK_GOTO(ret = lys_compile_grouping(&ctx, pnode, &grps[u]), error);
            }
        }
    }
    LY_ARRAY_FOR(sp->includes, u) {
        submod = sp->includes[u].submodule;
        LY_ARRAY_FOR(submod->groupings, u) {
            if (!(submod->groupings[u].flags & LYS_USED_GRP)) {
                LY_CHECK_GOTO(ret = lys_compile_grouping(&ctx, pnode, &submod->groupings[u]), error);
            }
        }
        LY_LIST_FOR(submod->data, pnode) {
            grps = (struct lysp_grp *)lysp_node_groupings(pnode);
            LY_ARRAY_FOR(grps, u) {
                if (!(grps[u].flags & LYS_USED_GRP)) {
                    LY_CHECK_GOTO(ret = lys_compile_grouping(&ctx, pnode, &grps[u]), error);
                }
            }
        }
    }

#if 0
    /* hack for NETCONF's edit-config's operation attribute. It is not defined in the schema, but since libyang
     * implements YANG metadata (annotations), we need its definition. Because the ietf-netconf schema is not the
     * internal part of libyang, we cannot add the annotation into the schema source, but we do it here to have
     * the anotation definitions available in the internal schema structure. */
    if (ly_strequal(mod->name, "ietf-netconf", 0)) {
        if (lyp_add_ietf_netconf_annotations(mod)) {
            lys_free(mod, NULL, 1, 1);
            return NULL;
        }
    }
#endif

    /* add ietf-netconf-with-defaults "default" metadata to the compiled module */
    if (!strcmp(mod->name, "ietf-netconf-with-defaults")) {
        LY_CHECK_GOTO(ret = lys_compile_ietf_netconf_wd_annotation(&ctx, mod), error);
    }

    /* there can be no leftover deviations */
    LY_CHECK_ERR_GOTO(ctx.devs.count, LOGINT(ctx.ctx); ret = LY_EINT, error);

    for (i = 0; i < ctx.dflts.count; ++i) {
        lysc_unres_dflt_free(ctx.ctx, ctx.dflts.objs[i]);
    }
    ly_set_erase(&ctx.dflts, NULL);
    ly_set_erase(&ctx.xpath, NULL);
    ly_set_erase(&ctx.leafrefs, NULL);
    ly_set_erase(&ctx.groupings, NULL);
    ly_set_erase(&ctx.tpdf_chain, NULL);
    ly_set_erase(&ctx.augs, NULL);
    ly_set_erase(&ctx.devs, NULL);
    ly_set_erase(&ctx.uses_augs, NULL);
    ly_set_erase(&ctx.uses_rfns, NULL);

    return LY_SUCCESS;

error:
    lys_precompile_augments_deviations_revert(ctx.ctx, mod);
    lys_feature_precompile_revert(&ctx, mod);
    for (i = 0; i < ctx.dflts.count; ++i) {
        lysc_unres_dflt_free(ctx.ctx, ctx.dflts.objs[i]);
    }
    ly_set_erase(&ctx.dflts, NULL);
    ly_set_erase(&ctx.xpath, NULL);
    ly_set_erase(&ctx.leafrefs, NULL);
    ly_set_erase(&ctx.groupings, NULL);
    ly_set_erase(&ctx.tpdf_chain, NULL);
    for (i = 0; i < ctx.augs.count; ++i) {
        lysc_augment_free(ctx.ctx, ctx.augs.objs[i]);
    }
    ly_set_erase(&ctx.augs, NULL);
    for (i = 0; i < ctx.devs.count; ++i) {
        lysc_deviation_free(ctx.ctx, ctx.devs.objs[i]);
    }
    ly_set_erase(&ctx.devs, NULL);
    for (i = 0; i < ctx.uses_augs.count; ++i) {
        lysc_augment_free(ctx.ctx, ctx.uses_augs.objs[i]);
    }
    ly_set_erase(&ctx.uses_augs, NULL);
    for (i = 0; i < ctx.uses_rfns.count; ++i) {
        lysc_refine_free(ctx.ctx, ctx.uses_rfns.objs[i]);
    }
    ly_set_erase(&ctx.uses_rfns, NULL);
    lysc_module_free(mod_c, NULL);
    mod->compiled = NULL;

    return ret;
}