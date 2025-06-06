/**
 * @file tree_data.h
 * @author Radek Krejci <rkrejci@cesnet.cz>
 * @author Michal Vasko <mvasko@cesnet.cz>
 * @brief libyang representation of YANG data trees.
 *
 * Copyright (c) 2015 - 2025 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#ifndef LY_TREE_DATA_H_
#define LY_TREE_DATA_H_

#ifdef _WIN32
# include <winsock2.h>
# include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  if defined (__FreeBSD__) || defined (__NetBSD__) || defined (__OpenBSD__)
#    include <netinet/in.h>
#    include <sys/socket.h>
#  endif
#endif

#define PCRE2_CODE_UNIT_WIDTH 8

#include <pcre2.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "log.h"
#include "ly_config.h"
#include "tree.h"
#include "tree_schema.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ly_ctx;
struct ly_path;
struct ly_set;
struct lyd_node;
struct lyd_node_opaq;
struct lyd_node_term;
struct timespec;
struct lyxp_var;
struct rb_node;

/**
 * @page howtoData Data Instances
 *
 * All the nodes in data tree comes are based on ::lyd_node structure. According to the content of the ::lyd_node.schema
 * it can be cast to several other structures.
 *
 * In case the ::lyd_node.schema pointer is NULL, the node is actually __opaq__ and can be safely cast to ::lyd_node_opaq.
 * The opaq node represents an **unknown node** which wasn't mapped to any [(compiled) schema](@ref howtoSchema) node in the
 * context. But it may represent a schema node data instance which is invalid. That may happen if the value (leaf/leaf-list)
 * is invalid or there are invalid/missing keys of a list instance. Such a node can appear in several places in the data tree.
 * - As a part of the tree structure, but only in the case the ::LYD_PARSE_OPAQ option was used when input data were
 *   [parsed](@ref howtoDataParsers), because unknown data instances are ignored and invalid data produce errors by default.
 * - As a representation of YANG anydata/anyxml content.
 * - As envelopes of standard data tree instances (RPCs, actions or Notifications).
 *
 * In case the data node has its definition in a [compiled schema tree](@ref howtoSchema), the structure of the data node is
 * actually one of the followings according to the schema node's nodetype (::lysc_node.nodetype).
 * - ::lyd_node_inner - represents data nodes corresponding to schema nodes matching ::LYD_NODE_INNER nodetypes. They provide
 * structure of the tree by having children nodes.
 * - ::lyd_node_term - represents data nodes corresponding to schema nodes matching ::LYD_NODE_TERM nodetypes. The terminal
 * nodes provide values of the particular configuration/status information. The values are represented as ::lyd_value
 * structure with string representation of the value (retrieved by ::lyd_get_value() and ::lyd_get_meta_value()) and
 * the type specific data stored in the structure's union according to the real type of the value (::lyd_value.realtype).
 * The string representation provides canonical representation of the value in case the type has the canonical
 * representation specified. Otherwise, it is the original value or, in case the value can contain prefixes, the JSON
 * format is used to make the value unambiguous.
 * - ::lyd_node_any - represents data nodes corresponding to schema nodes matching ::LYD_NODE_ANY nodetypes.
 *
 * Despite all the aforementioned structures and their members are available as part of the libyang API and callers can use
 * it to navigate through the data tree structure or to obtain various information, we recommend to use the following macros
 * and functions.
 * - ::lyd_child() (or ::lyd_child_no_keys()) and ::lyd_parent() to get the node's child/parent node.
 * - ::LYD_CTX to get libyang context from a data node.
 * - ::lyd_get_value()/::lyd_get_meta_value() to get canonical string value from a terminal node/metadata instance.
 * - ::LYD_TREE_DFS_BEGIN and ::LYD_TREE_DFS_END to traverse the data tree (depth-first).
 * - ::LY_LIST_FOR and ::LY_ARRAY_FOR as described on @ref howtoStructures page.
 *
 * Instead of going through the data tree on your own, a specific data node can be also located using a wide set of
 * \b lyd_find_*() functions.
 *
 * More information about specific operations with data instances can be found on the following pages:
 * - @subpage howtoDataParsers
 * - @subpage howtoDataValidation
 * - @subpage howtoDataWD
 * - @subpage howtoDataManipulation
 * - @subpage howtoDataPrinters
 * - @subpage howtoDataLYB
 *
 * \note API for this group of functions is described in the [Data Instances module](@ref datatree).
 *
 * Functions List (not assigned to above subsections)
 * --------------------------------------------------
 * - ::lyd_child()
 * - ::lyd_child_no_keys()
 * - ::lyd_parent()
 * - ::lyd_owner_module()
 * - ::lyd_get_value()
 * - ::lyd_get_meta_value()
 * - ::lyd_find_xpath()
 * - ::lyd_find_path()
 * - ::lyd_find_target()
 * - ::lyd_find_sibling_val()
 * - ::lyd_find_sibling_first()
 * - ::lyd_find_sibling_opaq_next()
 * - ::lyd_find_meta()
 *
 * - ::lyd_path()
 * - ::lyd_find_target()
 *
 * - ::lyd_lyb_data_length()
 *
 *
 * @section howtoDataMetadata Metadata Support
 *
 * YANG Metadata annotations are defined in [RFC 7952](https://tools.ietf.org/html/rfc7952) as YANG extension (and libyang
 * [implements them as internal extension plugin](@ref howtoPluginsExtensions)). In practice, it allows to have XML
 * attributes (there is also a special encoding for JSON) in YANG modeled data. libyang does not allow to have any XML
 * attribute without the appropriate annotation definition describing the data as it is done e.g. for leafs. When an
 * attribute without a matching annotation definition is found in the input data, it is:
 * - silently dropped (with warning) or
 * - an error is reported in case the ::LYD_PARSE_STRICT parser option is provided to the
 *   [parser function](@ref howtoDataParsers) or
 * - stored into a generic ::lyd_attr structure without a connection with any YANG module in case the ::LYD_PARSE_OPAQ
 *   parser options is provided to the [parser function](@ref howtoDataParsers).
 *
 * There are some XML attributes, described by [YANG](https://tools.ietf.org/html/rfc7950) and
 * [NETCONF](https://tools.ietf.org/html/rfc6241) specifications, which are not defined as annotations, but libyang
 * implements them this way. In case of attributes in the YANG namespace (`insert`, `value` and `key` attributes
 * for the NETCONF edit-config operation), they are defined in special libyang's internal module `yang`, which is
 * available in each context and the content of this schema can be printed via
 * [schema printers](@ref howtoSchemaPrinters).
 *
 * In case of the attributes described in [NETCONF specification](https://tools.ietf.org/html/rfc6241), the libyang's
 * annotations structures are hidden and cannot be printed despite, internally, they are part of the `ietf-netconf`'s
 * schema structure. Therefore, these attributes are available only when the `ietf-netconf` schema is loaded in the
 * context. The definitions of these annotations are as follows:
 *
 *     md:annotation operation {
 *       type enumeration {
 *         enum merge;
 *         enum replace;
 *         enum create;
 *         enum delete;
 *         enum remove;
 *       }
 *     }
 *
 *     md:annotation type {
 *       type enumeration {
 *         enum subtree;
 *         enum xpath {
 *           if-feature "nc:xpath";
 *         }
 *       }
 *     }
 *
 *     md:annotation select {
 *       type string;
 *     }
 *
 * Note, that, following the specification,
 * - the `type` and `select` XML attributes are supposed to be unqualified (without namespace) and that
 * - the `select`'s content is XPath and it is internally transformed by libyang into the format where the
 *   XML namespace prefixes are replaced by the YANG module names.
 *
 *
 * @section howtoDataYangdata yang-data Support
 *
 * [RFC 8040](https://tools.ietf.org/html/rfc8040) defines ietf-restconf module, which includes yang-data extension. Despite
 * the definition in the RESTCONF YANG module, the yang-data concept is quite generic and used even in modules without a
 * connection to RESTCONF protocol. The extension allows to define a separated YANG trees usable separately from any
 * datastore.
 *
 * libyang implements support for yang-data internally as an [extension plugin](@ref howtoPluginsExtensions). To ease the
 * use of yang-data with libyang, there are several generic functions, which are usable for yang-data:
 *
 * - ::lyd_parse_ext_data()
 * - ::lyd_parse_ext_op()
 *
 * - ::lys_getnext_ext()
 *
 * - ::lyd_new_ext_inner()
 * - ::lyd_new_ext_list()
 * - ::lyd_new_ext_term()
 * - ::lyd_new_ext_any()
 * - ::lyd_new_ext_path()
 *
 * @section howtoDataMountpoint mount-point Support
 *
 * [RFC 8528](https://tools.ietf.org/html/rfc8528) defines mount-point extension in ietf-yang-schema-mount YANG module.
 * This extension is supported out-of-the-box but to be able to parse data in a mount point, additional run-time data
 * need to be provided by a callback:
 *
 * - ::ly_ctx_set_ext_data_clb()
 *
 * The mounted data can be parsed directly from data files or created manually using the standard functions. However,
 * note that the mounted data use **their own context** created as needed. For *inline* data this means that any new
 * request for a mount-point schema node results in a new context creation because it is impossible to determine
 * whether any existing context can be used. Also, all these contexts created for the mounted data are **never**
 * freed automatically except when the parent context is being freed. So, to avoid redundant context creation, it is
 * always advised to use *shared-schema* for mount-points.
 *
 * In case it is not possible and *inline* mount point must be defined, it is still possible to avoid creating
 * additional contexts. When the top-level node right under a schema node with a mount-point is created, always use
 * this node for creation of any descendants. So, when using ::lyd_new_path(), use the node as `parent` and specify
 * relative `path`.
 */

/**
 * @page howtoDataManipulation Manipulating Data
 *
 * There are many functions to create or modify an existing data tree. You can add new nodes, reconnect nodes from
 * one tree to another (or e.g. from one list instance to another) or remove nodes. The functions doesn't allow you
 * to put a node to a wrong place (by checking the YANG module structure), but not all validation checks can be made directly
 * (or you have to make a valid change by multiple tree modifications) when the tree is being changed. Therefore,
 * the [validation process](@ref howtoDataValidation) is expected to be invoked after changing the data tree to make sure
 * that the changed data tree is valid.
 *
 * When inserting a node into data tree (no matter if the node already exists, via ::lyd_insert_child() and
 * ::lyd_insert_sibling(), or a new node is being created), the node is automatically inserted to the place respecting the
 * nodes order from the YANG schema. A leaf-list instances are sorted based on the value and the ::lyplg_type_sort_clb
 * function defined in the given datatype. A list instances are ordered similarly based on keys. In case the node is opaq
 * (it is not connected with any schema node), it is placed to the end of the sibling node in the order they are inserted in.
 * The only situation when it is possible to influence the order of the nodes is the order of user-ordered list/leaf-list
 * instances. In such a case the ::lyd_insert_after(), ::lyd_insert_before() can be used and ::lyd_insert_child(),
 * ::lyd_insert_sibling() adds the node after the existing instance of the closest preceding sibling node from the schema.
 *
 * Creating data is generally possible in two ways, they can be combined. You can add nodes one-by-one based on
 * the node name and/or its parent (::lyd_new_inner(), ::lyd_new_term(), ::lyd_new_any(), ::lyd_new_list(), ::lyd_new_list2()
 * and ::lyd_new_opaq()) or address the nodes using a [simple XPath addressing](@ref howtoXPath) (::lyd_new_path() and
 * ::lyd_new_path2()). The latter enables to create a whole path of nodes, requires less information
 * about the modified data, and is generally simpler to use. Actually the third way is duplicating the existing data using
 * ::lyd_dup_single(), ::lyd_dup_siblings() and ::lyd_dup_meta_single().
 *
 * Note, that in case the node is defined in an extension instance, the functions mentioned above do not work until you
 * provide parent where the new node is supposed to be inserted. The reason is that all the functions searches for the
 * top-level nodes directly inside modules. To create a top-level node defined in an extension instance, use
 * ::lyd_new_ext_inner(), ::lyd_new_ext_term(), ::lyd_new_ext_any(), ::lyd_new_ext_list() and ::lyd_new_ext_path()
 * functions.
 *
 * The [metadata](@ref howtoDataMetadata) (and attributes in opaq nodes) can be created with ::lyd_new_meta()
 * and ::lyd_new_attr().
 *
 * Changing value of a terminal node (leaf, leaf-list) is possible with ::lyd_change_term(). Similarly, the metadata value
 * can be changed with ::lyd_change_meta(). Before changing the value, it might be useful to compare the node's value
 * with a string value (::lyd_value_compare()) or verify that the new string value is correct for the specific data node
 * (::lyd_value_validate()).
 *
 * Working with two existing subtrees can also be performed two ways. Usually, you would use lyd_insert*() functions.
 * They are generally meant for simple inserts of a node into a data tree. For more complicated inserts and when
 * merging 2 trees use ::lyd_merge_tree() or ::lyd_merge_siblings(). It offers additional options and is basically a more
 * powerful insert.
 *
 * Besides merging, libyang is also capable to provide information about differences between two data trees. For this purpose,
 * ::lyd_diff_tree() and ::lyd_diff_siblings() generates annotated data trees which can be, in addition, used to change one
 * data tree to another one using ::lyd_diff_apply_all(), ::lyd_diff_apply_module() and ::lyd_diff_reverse_all(). Multiple
 * diff data trees can be also put together for further work using ::lyd_diff_merge_all(), ::lyd_diff_merge_module() and
 * ::lyd_diff_merge_tree() functions. To just check equivalence of the data nodes, ::lyd_compare_single(),
 * ::lyd_compare_siblings() and ::lyd_compare_meta() can be used.
 *
 * To remove a node or subtree from a data tree, use ::lyd_unlink_tree() and then free the unwanted data using
 * ::lyd_free_all() (or other \b lyd_free_*() functions).
 *
 * Also remember, that when you are creating/inserting a node, all the objects in that operation must belong to the
 * same context.
 *
 * Modifying the single data tree in multiple threads is not safe.
 *
 * Functions List
 * --------------
 * - ::lyd_new_inner()
 * - ::lyd_new_term()
 * - ::lyd_new_term_bin()
 * - ::lyd_new_list()
 * - ::lyd_new_list2()
 * - ::lyd_new_list3()
 * - ::lyd_new_any()
 * - ::lyd_new_opaq()
 * - ::lyd_new_opaq2()
 * - ::lyd_new_attr()
 * - ::lyd_new_attr2()
 * - ::lyd_new_meta()
 * - ::lyd_new_path()
 * - ::lyd_new_path2()
 *
 * - ::lyd_new_ext_inner()
 * - ::lyd_new_ext_term()
 * - ::lyd_new_ext_list()
 * - ::lyd_new_ext_any()
 * - ::lyd_new_ext_path()
 *
 * - ::lyd_dup_single()
 * - ::lyd_dup_siblings()
 * - ::lyd_dup_meta_single()
 *
 * - ::lyd_insert_child()
 * - ::lyd_insert_sibling()
 * - ::lyd_insert_after()
 * - ::lyd_insert_before()
 *
 * - ::lyd_value_compare()
 * - ::lyd_value_validate()
 *
 * - ::lyd_change_term()
 * - ::lyd_change_term_bin()
 * - ::lyd_change_term_canon()
 * - ::lyd_change_meta()
 *
 * - ::lyd_compare_single()
 * - ::lyd_compare_siblings()
 * - ::lyd_compare_meta()
 * - ::lyd_diff_tree()
 * - ::lyd_diff_siblings()
 * - ::lyd_diff_apply_all()
 * - ::lyd_diff_apply_module()
 * - ::lyd_diff_reverse_all()
 * - ::lyd_diff_merge_all()
 * - ::lyd_diff_merge_module()
 * - ::lyd_diff_merge_tree()
 *
 * - ::lyd_merge_tree()
 * - ::lyd_merge_siblings()
 * - ::lyd_merge_module()
 *
 * - ::lyd_unlink_tree()
 *
 * - ::lyd_free_all()
 * - ::lyd_free_siblings()
 * - ::lyd_free_tree()
 * - ::lyd_free_meta_single()
 * - ::lyd_free_meta_siblings()
 * - ::lyd_free_attr_single()
 * - ::lyd_free_attr_siblings()
 *
 * - ::lyd_any_value_str()
 * - ::lyd_any_copy_value()
 */

/**
 * @page howtoDataWD Default Values
 *
 * libyang provides support for work with default values as defined in [RFC 6243](https://tools.ietf.org/html/rfc6243).
 * However, libyang context do not contains the *ietf-netconf-with-defaults* module on its own and caller is supposed to
 * add this YANG module to enable full support of the *with-defaults* features described below. Without presence of the
 * mentioned module in the context, the default nodes are still present and handled in the data trees, but the metadata
 * providing the information about the default values cannot be used. It means that when parsing data, the default nodes
 * marked with the metadata as implicit default nodes are handled as explicit data and when printing data tree, the expected
 * nodes are printed without the ietf-netconf-with-defaults metadata.
 *
 * The RFC document defines 4 modes for handling default nodes in a data tree, libyang adds the fifth mode and use them
 * via @ref dataprinterflags when printing data trees.
 * - \b explicit - Only the explicitly set configuration data. But in the case of status data, missing default
 *                 data are added into the tree. In libyang, this mode is represented by ::LYD_PRINT_WD_EXPLICIT option.
 *                 This is the default with-defaults mode of the printer. The data nodes do not contain any additional
 *                 metadata information.
 * - \b trim - Data nodes containing the default value are removed. This mode is applied with ::LYD_PRINT_WD_TRIM option.
 * - \b report-all - This mode provides all the default data nodes despite they were explicitly present in source data or
 *                 they were added by libyang's [validation process](@ref howtoDataValidation). This mode is activated by
 *                 ::LYD_PRINT_WD_ALL option.
 * - \b report-all-tagged - In this case, all the data nodes (implicit as well the explicit) containing the default value
 *                 are printed and tagged (see the note below). Printers accept ::LYD_PRINT_WD_ALL_TAG option for this mode.
 * - \b report-implicit-tagged - The last mode is similar to the previous one, except only the implicitly added nodes
 *                 are tagged. This is the libyang's extension and it is activated by ::LYD_PRINT_WD_IMPL_TAG option.
 *
 * Internally, libyang adds the default nodes into the data tree as part of the [validation process](@ref howtoDataValidation).
 * When [parsing data](@ref howtoDataParsers) from an input source, adding default nodes can be avoided only by avoiding
 * the whole [validation process](@ref howtoDataValidation). In case the ietf-netconf-with-defaults module is present in the
 * context, the [parser process](@ref howtoDataParsers) also supports to recognize the implicit default nodes marked with the
 * appropriate metadata.
 *
 * Note, that in a modified data tree (via e.g. \b lyd_insert_*() or \b lyd_free_*() functions), some of the default nodes
 * can be missing or they can be present by mistake. Such a data tree is again corrected during the next run of the
 * [validation process](@ref howtoDataValidation) or manualy using \b lyd_new_implicit_*() functions.
 *
 * The implicit (default) nodes, created by libyang, are marked with the ::LYD_DEFAULT flag in ::lyd_node.flags member
 * Note, that besides leafs and leaf-lists, the flag can appear also in containers, where it means that the container
 * holds only a default node(s) or it is implicitly added empty container (according to YANG 1.1 spec, all such containers are part of
 * the accessible data tree). When printing data trees, the presence of empty containers (despite they were added
 * explicitly or implicitly as part of accessible data tree) depends on ::LYD_PRINT_KEEPEMPTYCONT option.
 *
 * To get know if the particular leaf or leaf-list node contains default value (despite implicit or explicit), you can
 * use ::lyd_is_default() function.
 *
 * Functions List
 * --------------
 * - ::lyd_is_default()
 * - ::lyd_new_implicit_all()
 * - ::lyd_new_implicit_module()
 * - ::lyd_new_implicit_tree()
 */

/**
 * @page howtoDataLYB LYB Binary Format
 *
 * LYB (LibYang Binary) is a proprietary libyang binary data and file format. Its primary purpose is efficient
 * serialization (printing) and deserialization (parsing). With this goal in mind, every term node value is stored
 * in its new binary format specification according to its type. Following is the format for all types with explicit
 * support out-of-the-box (meaning that have a special type plugin). Any derived types inherit the format of its
 * closest type with explicit support (up to a built-in type).
 *
 * @section howtoDataLYBTypes Format of specific data type values
 */

/**
 * @ingroup trees
 * @defgroup datatree Data Tree
 * @{
 *
 * Data structures and functions to manipulate and access instance data tree.
 */

/* *INDENT-OFF* */

/**
 * @brief Macro to iterate via all elements in a data tree. This is the opening part
 * to the #LYD_TREE_DFS_END - they always have to be used together.
 *
 * The function follows deep-first search algorithm:
 * <pre>
 *     1
 *    / \
 *   2   4
 *  /   / \
 * 3   5   6
 * </pre>
 *
 * Use the same parameters for #LYD_TREE_DFS_BEGIN and #LYD_TREE_DFS_END. While
 * START can be any of the lyd_node* types, ELEM variable must be a pointer to
 * the generic struct lyd_node.
 *
 * To skip a particular subtree, instead of the continue statement, set LYD_TREE_DFS_continue
 * variable to non-zero value.
 *
 * Use with opening curly bracket '{' after the macro.
 *
 * @param START Pointer to the starting element processed first.
 * @param ELEM Iterator intended for use in the block.
 */
#define LYD_TREE_DFS_BEGIN(START, ELEM) \
    { ly_bool LYD_TREE_DFS_continue = 0; struct lyd_node *LYD_TREE_DFS_next; \
    for ((ELEM) = (LYD_TREE_DFS_next) = (struct lyd_node *)(START); \
         (ELEM); \
         (ELEM) = (LYD_TREE_DFS_next), LYD_TREE_DFS_continue = 0)

/**
 * @brief Macro to iterate via all elements in a tree. This is the closing part
 * to the #LYD_TREE_DFS_BEGIN - they always have to be used together.
 *
 * Use the same parameters for #LYD_TREE_DFS_BEGIN and #LYD_TREE_DFS_END. While
 * START can be any of the lyd_node* types, ELEM variable must be a pointer
 * to the generic struct lyd_node.
 *
 * Use with closing curly bracket '}' after the macro.
 *
 * @param START Pointer to the starting element processed first.
 * @param ELEM Iterator intended for use in the block.
 */

#define LYD_TREE_DFS_END(START, ELEM) \
    /* select element for the next run - children first */ \
    if (LYD_TREE_DFS_continue) { \
        (LYD_TREE_DFS_next) = NULL; \
    } else { \
        (LYD_TREE_DFS_next) = lyd_child(ELEM); \
    }\
    if (!(LYD_TREE_DFS_next)) { \
        /* no children */ \
        if ((ELEM) == (struct lyd_node *)(START)) { \
            /* we are done, (START) has no children */ \
            break; \
        } \
        /* try siblings */ \
        (LYD_TREE_DFS_next) = (ELEM)->next; \
    } \
    while (!(LYD_TREE_DFS_next)) { \
        /* parent is already processed, go to its sibling */ \
        (ELEM) = (struct lyd_node *)(ELEM)->parent; \
        /* no siblings, go back through parents */ \
        if ((ELEM)->parent == (START)->parent) { \
            /* we are done, no next element to process */ \
            break; \
        } \
        (LYD_TREE_DFS_next) = (ELEM)->next; \
    } }

/**
 * @brief Macro to iterate via all schema node data instances in data siblings.
 *
 * @param START Pointer to the starting sibling. Even if it is not first, all the siblings are searched.
 * @param SCHEMA Schema node of the searched instances.
 * @param ELEM Iterator.
 */
#define LYD_LIST_FOR_INST(START, SCHEMA, ELEM) \
    for (lyd_find_sibling_val(START, SCHEMA, NULL, 0, &(ELEM)); \
         (ELEM) && ((ELEM)->schema == (SCHEMA)); \
         (ELEM) = (ELEM)->next)

/**
 * @brief Macro to iterate via all schema node data instances in data siblings allowing to modify the list itself.
 *
 * @param START Pointer to the starting sibling. Even if it is not first, all the siblings are searched.
 * @param SCHEMA Schema node of the searched instances.
 * @param NEXT Temporary storage to allow removing of the current iterator content.
 * @param ELEM Iterator.
 */
#define LYD_LIST_FOR_INST_SAFE(START, SCHEMA, NEXT, ELEM) \
    for ((NEXT) = (ELEM) = NULL, lyd_find_sibling_val(START, SCHEMA, NULL, 0, &(ELEM)); \
         (ELEM) && ((ELEM)->schema == (SCHEMA)) ? ((NEXT) = (ELEM)->next, 1) : 0; \
         (ELEM) = (NEXT))

/* *INDENT-ON* */

/**
 * @brief Macro to get context from a data tree node.
 */
#define LYD_CTX(node) ((node)->schema ? (node)->schema->module->ctx : ((const struct lyd_node_opaq *)(node))->ctx)

/**
 * @brief Find out if the node is the only instance, i.e. it has no siblings with the same schema.
 *
 * @param[in] NODE Pointer to the struct lyd_node.
 * @return 1 @p NODE is a single instance (is alone).
 * @return 0 @p NODE is not alone.
 */
#define LYD_NODE_IS_ALONE(NODE) \
    (((NODE)->prev == NODE) || \
        (((NODE)->prev->schema != (NODE)->schema) && (!(NODE)->next || ((NODE)->schema != (NODE)->next->schema))))

/**
 * @brief Data input/output formats supported by libyang [parser](@ref howtoDataParsers) and
 * [printer](@ref howtoDataPrinters) functions.
 */
typedef enum {
    LYD_UNKNOWN = 0,     /**< unknown data format, invalid value */
    LYD_XML,             /**< XML instance data format */
    LYD_JSON,            /**< JSON instance data format */
    LYD_LYB              /**< LYB instance data format */
} LYD_FORMAT;

/**
 * @brief List of possible value types stored in ::lyd_node_any.
 */
typedef enum {
    LYD_ANYDATA_DATATREE,   /**< Value is a pointer to ::lyd_node structure (first sibling). When provided as input
                                 parameter, the pointer is directly connected into the anydata node without duplication,
                                 caller is supposed to not manipulate with the data after a successful call (including
                                 calling ::lyd_free_all() on the provided data) */
    LYD_ANYDATA_STRING,     /**< Value is a generic string without any knowledge about its format (e.g. anyxml value in
                                 JSON encoded as string). XML sensitive characters (such as & or \>) are automatically
                                 escaped when the anydata is printed in XML format. */
    LYD_ANYDATA_XML,        /**< Value is a string containing the serialized XML data. */
    LYD_ANYDATA_JSON,       /**< Value is a string containing the data modeled by YANG and encoded as I-JSON. */
    LYD_ANYDATA_LYB         /**< Value is a memory chunk with the serialized data tree in LYB format. */
} LYD_ANYDATA_VALUETYPE;

/** @} */

/**
 * @brief YANG data representation
 */
struct lyd_value {
    const char *_canonical;          /**< Should never be accessed directly, instead ::lyd_get_value() and ::lyd_get_meta_value()
                                          should be used. Serves as a cache for the canonical value or the JSON
                                          representation if no canonical value is defined. */
    const struct lysc_type *realtype; /**< pointer to the real type of the data stored in the value structure. This type can differ from the type
                                          in the schema node of the data node since the type's store plugin can use other types/plugins for
                                          storing data. Speaking about built-in types, this is the case of leafref which stores data as its
                                          target type. In contrast, union type also uses its subtype's callbacks, but inside an internal data
                                          stored in subvalue member of ::lyd_value structure, so here is the pointer to the union type.
                                          In general, this type is used to get free callback for this lyd_value structure, so it must reflect
                                          the type used to store data directly in the same lyd_value instance. */

    union {
        int8_t boolean;              /**< 0 as false, 1 as true */
        int64_t dec64;               /**< decimal64: value = dec64 / 10^fraction-digits  */
        int8_t int8;                 /**< 8-bit signed integer */
        int16_t int16;               /**< 16-bit signed integer */
        int32_t int32;               /**< 32-bit signed integer */
        int64_t int64;               /**< 64-bit signed integer */
        uint8_t uint8;               /**< 8-bit unsigned integer */
        uint16_t uint16;             /**< 16-bit unsigned integer */
        uint32_t uint32;             /**< 32-bit unsigned integer */
        uint64_t uint64;             /**< 64-bit unsigned integer */
        struct lysc_type_bitenum_item *enum_item;  /**< pointer to the definition of the enumeration value */
        struct lysc_ident *ident;    /**< pointer to the schema definition of the identityref value */
        struct ly_path *target;      /**< Instance-identifier target path, use ::lyd_find_target() to evaluate
                                        it on data. */
        struct lyd_value_union *subvalue; /** Union value with some metadata. */

        void *dyn_mem;               /**< pointer to generic data type value stored in dynamic memory */
        uint8_t fixed_mem[LYD_VALUE_FIXED_MEM_SIZE]; /**< fixed-size buffer for a generic data type value */
    };  /**< The union is just a list of shorthands to possible values stored by a type's plugin. libyang itself uses the ::lyd_value.realtype
             plugin's callbacks to work with the data.*/
};

/**
 * @brief Get the value in format specific to the type.
 *
 * Should be used for any types that do not have their specific representation in the ::lyd_value union.
 *
 * @param[in] value Pointer to the value structure to read from (struct ::lyd_value *).
 * @param[out] type_val Pointer to the type-specific value structure.
 */
#define LYD_VALUE_GET(value, type_val) \
    ((sizeof *(type_val) > LYD_VALUE_FIXED_MEM_SIZE) \
     ? ((type_val) = (((value)->dyn_mem))) \
     : ((type_val) = ((void *)((value)->fixed_mem))))

/**
 * @brief Special lyd_value structure for built-in union values.
 *
 * Represents data with multiple types (union). The ::lyd_value_union.value contains representation according to
 * one of the union's types. The ::lyd_value_union.prefix_data provides (possible) mappings from prefixes in
 * the original value to YANG modules. These prefixes are necessary to parse original value to the union's subtypes.
 */
struct lyd_value_union {
    struct lyd_value value;      /**< representation of the value according to the selected union's subtype
                                      (stored as ::lyd_value.realtype here) */
    void *original;              /**< Original value. */
    size_t orig_len;             /**< Original value length. */
    uint32_t hints;              /**< [Value hints](@ref lydvalhints) from the parser */
    LY_VALUE_FORMAT format;      /**< Prefix format of the value. However, this information is also used to decide
                                      whether a value is valid for the specific format or not on later validations
                                      (instance-identifier in XML looks different than in JSON). */
    void *prefix_data;           /**< Format-specific data for prefix resolution (see ly_resolve_prefix()) */
    const struct lysc_node *ctx_node;   /**< Context schema node. */
};

/**
 * @brief Special lyd_value structure for built-in bits values.
 */
struct lyd_value_bits {
    char *bitmap;                           /**< bitmap of size ::lyplg_type_bits_bitmap_size(), if its value is
                                                cast to an integer type of the corresponding size, can be used
                                                directly as a bitmap */
    struct lysc_type_bitenum_item **items;  /**< list of set pointers to the specification of the set
                                                bits ([sized array](@ref sizedarrays)) */
};

/**
 * @brief Special lyd_value structure for built-in binary values.
 */
struct lyd_value_binary {
    void *data;     /**< pointer to the binary value */
    size_t size;    /**< size of @p data value in bytes */
};

/**
 * @brief Special lyd_value structure for ietf-inet-types ipv4-address-no-zone values.
 */
struct lyd_value_ipv4_address_no_zone {
    struct in_addr addr;    /**< IPv4 address in binary */
};

/**
 * @brief Special lyd_value structure for ietf-inet-types ipv4-address values.
 */
struct lyd_value_ipv4_address {
    struct in_addr addr;    /**< IPv4 address in binary */
    const char *zone;       /**< Optional address zone */
};

/**
 * @brief Special lyd_value structure for ietf-inet-types ipv4-prefix values.
 */
struct lyd_value_ipv4_prefix {
    struct in_addr addr;    /**< IPv4 host address in binary */
    uint8_t prefix;         /**< prefix length (0 - 32) */
};

/**
 * @brief Special lyd_value structure for ietf-inet-types ipv6-address-no-zone values.
 */
struct lyd_value_ipv6_address_no_zone {
    struct in6_addr addr;   /**< IPv6 address in binary */
};

/**
 * @brief Special lyd_value structure for ietf-inet-types ipv6-address values.
 */
struct lyd_value_ipv6_address {
    struct in6_addr addr;   /**< IPv6 address in binary */
    const char *zone;       /**< Optional address zone */
};

/**
 * @brief Special lyd_value structure for ietf-inet-types ipv6-prefix values.
 */
struct lyd_value_ipv6_prefix {
    struct in6_addr addr;   /**< IPv6 host address in binary */
    uint8_t prefix;         /**< prefix length (0 - 128) */
};

/**
 * @brief Special lyd_value structure for ietf-yang-types date-and-time values.
 */
struct lyd_value_date_and_time {
    time_t time;        /**< UNIX timestamp */
    char *fractions_s;  /**< Optional fractions of a second */
    ly_bool unknown_tz; /**< Whether the value is in the special -00:00 timezone. */
};

/**
 * @brief Special lyd_value structure for ietf-yang-types xpath1.0 values.
 */
struct lyd_value_xpath10 {
    struct lyxp_expr *exp;
    const struct ly_ctx *ctx;
    void *prefix_data;
    LY_VALUE_FORMAT format;
};

/**
 * @brief Special lyd_value structure for lyds tree value.
 */
struct lyd_value_lyds_tree {
    struct rb_node *rbt;        /**< Root of the Red-black tree. */
};

/**
 * @brief Generic prefix and namespace mapping, meaning depends on the format.
 *
 * The union is used as a reference to the data's module and according to the format, it can be used as a key for
 * ::ly_ctx_get_module_implemented_ns() or ::ly_ctx_get_module_implemented(). While the module reference is always present,
 * the prefix member can be omitted in case it is not present in the source data as a reference to the default module/namespace.
 */
struct ly_opaq_name {
    const char *name;             /**< node name, without prefix if any was defined */
    const char *prefix;           /**< identifier used in the qualified name as the prefix, can be NULL */

    union {
        const char *module_ns;    /**< format ::LY_VALUE_XML - XML namespace of the node element */
        const char *module_name;  /**< format ::LY_VALUE_JSON - (inherited) name of the module of the element */
    };
};

/**
 * @brief Generic attribute structure.
 */
struct lyd_attr {
    struct lyd_node_opaq *parent;   /**< data node where the attribute is placed */
    struct lyd_attr *next;          /**< pointer to the next attribute */
    struct ly_opaq_name name;       /**< attribute name with module information */
    const char *value;              /**< attribute value */
    uint32_t hints;                 /**< additional information about from the data source, see the [hints list](@ref lydhints) */
    LY_VALUE_FORMAT format;         /**< format of the attribute and any prefixes, ::LY_VALUE_XML or ::LY_VALUE_JSON */
    void *val_prefix_data;          /**< format-specific prefix data */
};

#define LYD_NODE_INNER (LYS_CONTAINER|LYS_LIST|LYS_RPC|LYS_ACTION|LYS_NOTIF) /**< Schema nodetype mask for lyd_node_inner */
#define LYD_NODE_TERM (LYS_LEAF|LYS_LEAFLIST)   /**< Schema nodetype mask for lyd_node_term */
#define LYD_NODE_ANY (LYS_ANYDATA)   /**< Schema nodetype mask for lyd_node_any */

/**
 * @ingroup datatree
 * @defgroup dnodeflags Data node flags
 * @{
 *
 * Various flags of data nodes.
 *
 *     1 - container    5 - anydata/anyxml
 *     2 - list         6 - rpc/action
 *     3 - leaf         7 - notification
 *     4 - leaflist
 *
 *     bit name              1 2 3 4 5 6 7
 *     ---------------------+-+-+-+-+-+-+-+
 *       1 LYD_DEFAULT      |x| |x|x| | | |
 *                          +-+-+-+-+-+-+-+
 *       2 LYD_WHEN_TRUE    |x|x|x|x|x| | |
 *                          +-+-+-+-+-+-+-+
 *       3 LYD_NEW          |x|x|x|x|x|x|x|
 *                          +-+-+-+-+-+-+-+
 *       4 LYD_EXT          |x|x|x|x|x|x|x|
 *     ---------------------+-+-+-+-+-+-+-+
 *
 */

#define LYD_DEFAULT     0x01        /**< default (implicit) node */
#define LYD_WHEN_TRUE   0x02        /**< all when conditions of this node were evaluated to true */
#define LYD_NEW         0x04        /**< node was created after the last validation, is needed for the next validation */
#define LYD_EXT         0x08        /**< node is the first sibling parsed as extension instance data */

/** @} */

/**
 * @brief Generic structure for a data node.
 */
struct lyd_node {
    uint32_t hash;                   /**< hash of this particular node (module name + schema name + key string values if list or
                                          hashes of all nodes of subtree in case of keyless list). Note that while hash can be
                                          used to get know that nodes are not equal, it cannot be used to decide that the
                                          nodes are equal due to possible collisions. */
    uint32_t flags;                  /**< [data node flags](@ref dnodeflags) */
    const struct lysc_node *schema;  /**< pointer to the schema definition of this node */
    struct lyd_node_inner *parent;   /**< pointer to the parent node, NULL in case of root node */
    struct lyd_node *next;           /**< pointer to the next sibling node (NULL if there is no one) */
    struct lyd_node *prev;           /**< pointer to the previous sibling node \note Note that this pointer is
                                          never NULL. If there is no sibling node, pointer points to the node
                                          itself. In case of the first node, this pointer points to the last
                                          node in the list. */
    struct lyd_meta *meta;           /**< pointer to the list of metadata of this node */
    void *priv;                      /**< private user data, not used by libyang */
};

/**
 * @brief Data node structure for the inner data tree nodes - containers, lists, RPCs, actions and Notifications.
 */
struct lyd_node_inner {
    union {
        struct lyd_node node;               /**< implicit cast for the members compatible with ::lyd_node */

        struct {
            uint32_t hash;                  /**< hash of this particular node (module name + schema name + key string
                                                 values if list or hashes of all nodes of subtree in case of keyless
                                                 list). Note that while hash can be used to get know that nodes are
                                                 not equal, it cannot be used to decide that the nodes are equal due
                                                 to possible collisions. */
            uint32_t flags;                 /**< [data node flags](@ref dnodeflags) */
            const struct lysc_node *schema; /**< pointer to the schema definition of this node */
            struct lyd_node_inner *parent;  /**< pointer to the parent node, NULL in case of root node */
            struct lyd_node *next;          /**< pointer to the next sibling node (NULL if there is no one) */
            struct lyd_node *prev;          /**< pointer to the previous sibling node \note Note that this pointer is
                                                 never NULL. If there is no sibling node, pointer points to the node
                                                 itself. In case of the first node, this pointer points to the last
                                                 node in the list. */
            struct lyd_meta *meta;          /**< pointer to the list of metadata of this node */
            void *priv;                     /**< private user data, not used by libyang */
        };
    };                                      /**< common part corresponding to ::lyd_node */

    struct lyd_node *child;          /**< pointer to the first child node. */
    struct ly_ht *children_ht;  /**< hash table with all the direct children (except keys for a list, lists without keys) */

#define LYD_HT_MIN_ITEMS 4           /**< minimal number of children to create ::lyd_node_inner.children_ht hash table. */
};

/**
 * @brief Data node structure for the terminal data tree nodes - leaves and leaf-lists.
 */
struct lyd_node_term {
    union {
        struct lyd_node node;               /**< implicit cast for the members compatible with ::lyd_node */

        struct {
            uint32_t hash;                  /**< hash of this particular node (module name + schema name + key string
                                                 values if list or hashes of all nodes of subtree in case of keyless
                                                 list). Note that while hash can be used to get know that nodes are
                                                 not equal, it cannot be used to decide that the nodes are equal due
                                                 to possible collisions. */
            uint32_t flags;                 /**< [data node flags](@ref dnodeflags) */
            const struct lysc_node *schema; /**< pointer to the schema definition of this node */
            struct lyd_node_inner *parent;  /**< pointer to the parent node, NULL in case of root node */
            struct lyd_node *next;          /**< pointer to the next sibling node (NULL if there is no one) */
            struct lyd_node *prev;          /**< pointer to the previous sibling node \note Note that this pointer is
                                                 never NULL. If there is no sibling node, pointer points to the node
                                                 itself. In case of the first node, this pointer points to the last
                                                 node in the list. */
            struct lyd_meta *meta;          /**< pointer to the list of metadata of this node */
            void *priv;                     /**< private user data, not used by libyang */
        };
    };                                      /**< common part corresponding to ::lyd_node */

    struct lyd_value value;          /**< node's value representation */
};

/**
 * @brief union for anydata/anyxml value representation.
 */
union lyd_any_value {
    struct lyd_node *tree; /**< data tree */
    const char *str;       /**< Generic string data */
    const char *xml;       /**< Serialized XML data */
    const char *json;      /**< I-JSON encoded string */
    char *mem;             /**< LYD_ANYDATA_LYB memory chunk */
};

/**
 * @brief Data node structure for the anydata data tree nodes - anydata or
 * anyxml.
 */
struct lyd_node_any {
    union {
        struct lyd_node node;               /**< implicit cast for the members compatible with ::lyd_node */

        struct {
            uint32_t hash;                  /**< hash of this particular node (module name + schema name + key string
                                                 values if list or hashes of all nodes of subtree in case of keyless
                                                 list). Note that while hash can be used to get know that nodes are
                                                 not equal, it cannot be used to decide that the nodes are equal due
                                                 to possible collisions. */
            uint32_t flags;                 /**< [data node flags](@ref dnodeflags) */
            const struct lysc_node *schema; /**< pointer to the schema definition of this node */
            struct lyd_node_inner *parent;  /**< pointer to the parent node, NULL in case of root node */
            struct lyd_node *next;          /**< pointer to the next sibling node (NULL if there is no one) */
            struct lyd_node *prev;          /**< pointer to the previous sibling node \note Note that this pointer is
                                                 never NULL. If there is no sibling node, pointer points to the node
                                                 itself. In case of the first node, this pointer points to the last
                                                 node in the list. */
            struct lyd_meta *meta;          /**< pointer to the list of metadata of this node */
            void *priv;                     /**< private user data, not used by libyang */
        };
    };                                      /**< common part corresponding to ::lyd_node */

    union lyd_any_value value;          /**< pointer to the stored value representation of the anydata/anyxml node */
    LYD_ANYDATA_VALUETYPE value_type;   /**< type of the data stored as ::lyd_node_any.value */
};

/**
 * @brief Get the name (associated with) of a data node. Works for opaque nodes as well.
 *
 * @param[in] node Node to examine.
 * @return Data node name.
 */
#define LYD_NAME(node) ((node)->schema ? (node)->schema->name : ((struct lyd_node_opaq *)node)->name.name)

/**
 * @ingroup datatree
 * @defgroup lydvalhints Value format hints.
 * @{
 *
 * Hints for the type of the data value.
 *
 * Any information about value types encoded in the format is hinted by these values.
 */
#define LYD_VALHINT_STRING     0x0001 /**< value is allowed to be a string */
#define LYD_VALHINT_DECNUM     0x0002 /**< value is allowed to be a decimal number */
#define LYD_VALHINT_OCTNUM     0x0004 /**< value is allowed to be an octal number */
#define LYD_VALHINT_HEXNUM     0x0008 /**< value is allowed to be a hexadecimal number */
#define LYD_VALHINT_NUM64      0x0010 /**< value is allowed to be an int64 or uint64 */
#define LYD_VALHINT_BOOLEAN    0x0020 /**< value is allowed to be a boolean */
#define LYD_VALHINT_EMPTY      0x0040 /**< value is allowed to be empty */
#define LYD_VALHINT_STRING_DATATYPES 0x0080 /**< boolean and numeric fields are allowed to be quoted */
/**
 * @} lydvalhints
 */

/**
 * @ingroup datatree
 * @defgroup lydnodehints Node type format hints
 * @{
 *
 * Hints for the type of the data node.
 *
 * Any information about node types encoded in the format is hinted by these values.
 */
#define LYD_NODEHINT_LIST       0x0080 /**< node is allowed to be a list instance */
#define LYD_NODEHINT_LEAFLIST   0x0100 /**< node is allowed to be a leaf-list instance */
#define LYD_NODEHINT_CONTAINER  0x0200 /**< node is allowed to be a container instance */
/**
 * @} lydnodehints
 */

/**
 * @ingroup datatree
 * @defgroup lydhints Value and node type format hints
 * @{
 *
 * Hints for the types of data node and its value.
 *
 * Any information about value and node types encoded in the format is hinted by these values.
 * It combines [value hints](@ref lydvalhints) and [node hints](@ref lydnodehints).
 */
#define LYD_HINT_DATA       0x03F3 /**< special node/value hint to be used for generic data node/value (for cases when
                                        there is no encoding or it does not provide any additional information about
                                        a node/value type); do not combine with specific [value hints](@ref lydvalhints)
                                        or [node hints](@ref lydnodehints). */
#define LYD_HINT_SCHEMA     0x03FF /**< special node/value hint to be used for generic schema node/value(for cases when
                                        there is no encoding or it does not provide any additional information about
                                        a node/value type); do not combine with specific [value hints](@ref lydvalhints)
                                        or [node hints](@ref lydnodehints). */
/**
 * @} lydhints
 */

/**
 * @brief Data node structure for unparsed (opaque) nodes.
 */
struct lyd_node_opaq {
    union {
        struct lyd_node node;               /**< implicit cast for the members compatible with ::lyd_node */

        struct {
            uint32_t hash;                  /**< always 0 */
            uint32_t flags;                 /**< always 0 */
            const struct lysc_node *schema; /**< always NULL */
            struct lyd_node_inner *parent;  /**< pointer to the parent node, NULL in case of root node */
            struct lyd_node *next;          /**< pointer to the next sibling node (NULL if there is no one) */
            struct lyd_node *prev;          /**< pointer to the previous sibling node \note Note that this pointer is
                                                 never NULL. If there is no sibling node, pointer points to the node
                                                 itself. In case of the first node, this pointer points to the last
                                                 node in the list. */
            struct lyd_meta *meta;          /**< always NULL */
            void *priv;                     /**< private user data, not used by libyang */
        };
    };                                      /**< common part corresponding to ::lyd_node */

    struct lyd_node *child;         /**< pointer to the child node (compatible with ::lyd_node_inner) */

    struct ly_opaq_name name;       /**< node name with module information */
    const char *value;              /**< original value */
    uint32_t hints;                 /**< additional information about from the data source, see the [hints list](@ref lydhints) */
    LY_VALUE_FORMAT format;        /**< format of the node and any prefixes, ::LY_VALUE_XML or ::LY_VALUE_JSON */
    void *val_prefix_data;          /**< format-specific prefix data */

    struct lyd_attr *attr;          /**< pointer to the list of generic attributes of this node */
    const struct ly_ctx *ctx;       /**< libyang context */
};

/**
 * @brief Structure of leafref links record.
 */
struct lyd_leafref_links_rec {
    const struct lyd_node_term *node;           /** pointer to the data node itself */
    const struct lyd_node_term **leafref_nodes; /** list of the leafref pointing to this data node [sized array](@ref sizedarrays)),
                                                    By default it is empty. It is filled automatically by validation function of
                                                    leafref nodes, which are valid and are not using 'require-instance false;'.
                                                    It can also be populated based on manual request using
                                                    [link api](@ref lyd_leafref_link_node_tree). Freeing of the resources is
                                                    automatic. */
    const struct lyd_node_term **target_nodes;  /** list of leafref target data nodes [sized array](@ref sizedarrays)). Byt default
                                                    it is empty. The logic is the same as for [leafref_nodes](@ref leafref_nodes) and
                                                    is filled only for leafrefs */
};

/**
 * @brief Get the generic parent pointer of a data node.
 *
 * @param[in] node Node whose parent pointer to get.
 * @return Pointer to the parent node of the @p node.
 * @return NULL in case of the top-level node or if the @p node is NULL itself.
 */
static inline struct lyd_node *
lyd_parent(const struct lyd_node *node)
{
    return (node && node->parent) ? &node->parent->node : NULL;
}

/**
 * @brief Get the child pointer of a generic data node.
 *
 * Decides the node's type and in case it has a children list, returns it. Supports even the opaq nodes (::lyd_node_opaq).
 *
 * If you need to skip key children, use ::lyd_child_no_keys().
 *
 * @param[in] node Node to use.
 * @return Pointer to the first child node (if any) of the @p node.
 */
static inline struct lyd_node *
lyd_child(const struct lyd_node *node)
{
    if (!node) {
        return NULL;
    }

    if (!node->schema) {
        /* opaq node */
        return ((const struct lyd_node_opaq *)node)->child;
    }

    if (node->schema->nodetype & (LYS_CONTAINER | LYS_LIST | LYS_RPC | LYS_ACTION | LYS_NOTIF)) {
        return ((const struct lyd_node_inner *)node)->child;
    }

    return NULL;
}

/**
 * @brief Get the child pointer of a generic data node but skip its keys in case it is ::LYS_LIST.
 *
 * Decides the node's type and in case it has a children list, returns it. Supports even the opaq nodes (::lyd_node_opaq).
 *
 * If you need to take key children into account, use ::lyd_child().
 *
 * @param[in] node Node to use.
 * @return Pointer to the first child node (if any) of the @p node.
 */
LIBYANG_API_DECL struct lyd_node *lyd_child_no_keys(const struct lyd_node *node);

/**
 * @brief Get the owner module of the data node. It is the module of the top-level schema node. Generally,
 * in case of augments it is the target module, recursively, otherwise it is the module where the data node is defined.
 *
 * Also works for opaque nodes, if it is possible to resolve the module.
 *
 * @param[in] node Data node to examine.
 * @return Module owner of the node.
 */
LIBYANG_API_DECL const struct lys_module *lyd_owner_module(const struct lyd_node *node);

/**
 * @brief Get the module of a node. Useful mainly for opaque nodes.
 *
 * @param[in] node Node to examine.
 * @return Module of the node.
 */
LIBYANG_API_DECL const struct lys_module *lyd_node_module(const struct lyd_node *node);

/**
 * @brief Check whether a node value equals to its default one.
 *
 * @param[in] node Term node to test.
 * @return false (no, it is not a default node) or true (yes, it is default)
 */
LIBYANG_API_DECL ly_bool lyd_is_default(const struct lyd_node *node);

/**
 * @brief Learn the relative position of a list or leaf-list instance within other instances of the same schema node.
 *
 * @param[in] instance List or leaf-list instance to get the position of.
 * @return 0 on error.
 * @return Positive integer of the @p instance position.
 */
LIBYANG_API_DECL uint32_t lyd_list_pos(const struct lyd_node *instance);

/**
 * @brief Get the first sibling of the given node.
 *
 * @param[in] node Node which first sibling is going to be the result.
 * @return The first sibling of the given node or the node itself if it is the first child of the parent.
 */
LIBYANG_API_DECL struct lyd_node *lyd_first_sibling(const struct lyd_node *node);

/**
 * @brief Learn the length of LYB data.
 *
 * @param[in] data LYB data to examine.
 * @return Length of the LYB data chunk,
 * @return -1 on error.
 */
LIBYANG_API_DECL int lyd_lyb_data_length(const char *data);

/**
 * @brief Check node parsed into an opaque node for the reason (error) why it could not be parsed as data node.
 *
 * The node is expected to be produced by a parser and must either have no parent or a data node parent (not opaque).
 *
 * @param[in] node Opaque node to check.
 * @return LY_EINVAL if @p node is in some way unexpected (even valid);
 * @return LY_ERR value of the reason.
 */
LIBYANG_API_DECL LY_ERR lyd_parse_opaq_error(const struct lyd_node *node);

/**
 * @brief Get the (canonical) value of a lyd_value.
 *
 * Whenever possible, ::lyd_get_value() or ::lyd_get_meta_value() should be used instead.
 *
 * @param[in] ctx Context for the value
 * @param[in] value Value structure to use.
 * @return Canonical value.
 */
LIBYANG_API_DECL const char *lyd_value_get_canonical(const struct ly_ctx *ctx, const struct lyd_value *value);

/**
 * @brief Get the (canonical) value of a data node.
 *
 * @param[in] node Data node to use.
 * @return Canonical value.
 */
static inline const char *
lyd_get_value(const struct lyd_node *node)
{
    if (!node) {
        return NULL;
    }

    if (!node->schema) {
        return ((const struct lyd_node_opaq *)node)->value;
    } else if (node->schema->nodetype & LYD_NODE_TERM) {
        const struct lyd_value *value = &((const struct lyd_node_term *)node)->value;

        return value->_canonical ? value->_canonical : lyd_value_get_canonical(LYD_CTX(node), value);
    }

    return NULL;
}

/**
 * @brief Get anydata string value.
 *
 * @param[in] any Anyxml/anydata node to read from.
 * @param[out] value_str String representation of the value.
 * @return LY_ERR value.
 */
LIBYANG_API_DECL LY_ERR lyd_any_value_str(const struct lyd_node *any, char **value_str);

/**
 * @brief Copy anydata value from one node to another. Target value is freed first.
 *
 * @param[in,out] trg Target node.
 * @param[in] value Source value, may be NULL when the target value is only freed.
 * @param[in] value_type Source value type.
 * @return LY_ERR value.
 */
LIBYANG_API_DECL LY_ERR lyd_any_copy_value(struct lyd_node *trg, const union lyd_any_value *value,
        LYD_ANYDATA_VALUETYPE value_type);

/**
 * @brief Get schema node of a data node. Useful especially for opaque nodes.
 *
 * @param[in] node Data node to use.
 * @return Schema node represented by data @p node, NULL if there is none.
 */
LIBYANG_API_DECL const struct lysc_node *lyd_node_schema(const struct lyd_node *node);

/**
 * @brief Check whether metadata are not an instance of internal metadata.
 *
 * @param[in] meta Metadata to check.
 * @return 1 if @p meta are internal.
 * @return 0 if @p meta are not internal.
 */
LIBYANG_API_DECL ly_bool lyd_meta_is_internal(const struct lyd_meta *meta);

/**
 * @brief Create a new inner node in the data tree.
 *
 * To create list, use ::lyd_new_list() or ::lyd_new_list2().
 *
 * To create a top-level inner node defined in an extension instance, use ::lyd_new_ext_inner().
 *
 * @param[in] parent Parent node for the node being created. NULL in case of creating a top level element.
 * @param[in] module Module of the node being created. If NULL, @p parent module will be used.
 * @param[in] name Schema node name of the new data node. The node can be #LYS_CONTAINER, #LYS_NOTIF, #LYS_RPC, or #LYS_ACTION.
 * @param[in] output Flag in case the @p parent is RPC/Action. If value is 0, the input's data nodes of the RPC/Action are
 * taken into consideration. Otherwise, the output's data node is going to be created.
 * @param[out] node Optional created node.
 * @return LY_ERR value.
 */
LIBYANG_API_DECL LY_ERR lyd_new_inner(struct lyd_node *parent, const struct lys_module *module, const char *name,
        ly_bool output, struct lyd_node **node);

/**
 * @brief Create a new top-level inner node defined in the given extension instance.
 *
 * To create list, use ::lyd_new_list() or ::lyd_new_list2().
 *
 * To create an inner node with parent (no matter if defined inside extension instance or a standard tree) or a top-level
 * node of a standard module's tree, use ::lyd_new_inner().
 *
 * @param[in] ext Extension instance where the inner node being created is defined.
 * @param[in] name Schema node name of the new data node. The node can be #LYS_CONTAINER, #LYS_NOTIF, #LYS_RPC, or #LYS_ACTION.
 * @param[out] node The created node.
 * @return LY_ERR value.
 */
LIBYANG_API_DECL LY_ERR lyd_new_ext_inner(const struct lysc_ext_instance *ext, const char *name, struct lyd_node **node);

/**
 * @ingroup datatree
 * @defgroup newvaloptions New value creation options
 *
 * Various options to change lyd_new_*() behavior. The LYD_NEW_VAL* can be used within any API, others
 * are API specific
 *
 * Default behavior:
 * - the input data nodes or RPC/Action is taken into account
 * - the value is being validated with all possible validations, which doesn't require existence of any other data nodes
 * - the input value is expected to be in JSON format
 * - RPC output schema children are completely ignored in all modules. Input is searched and nodes created normally.
 *
 * Default behavior specific for lyd_new_path*() functions:
 * - if the target node already exists (and is not default), an error is returned.
 * - the whole path to the target node is created (with any missing parents) if necessary.
 * - during creation of new metadata, the nodes will have default flag set
 * - string value is copied and stored internally during any node creation
 * @{
 */

#define LYD_NEW_VAL_OUTPUT 0x01      /**< Flag in case the @p parent is RPC/Action. If value is 0, the input's data nodes of the RPC/Action are
                                          taken into consideration. Otherwise, the output's data node is going to be created. */
#define LYD_NEW_VAL_STORE_ONLY 0x02  /**< Whether to perform only storing operation with no or minimum valitions */
#define LYD_NEW_VAL_BIN 0x04         /**< Interpret the provided leaf/leaf-list @p value as being in the binary
                                          ::LY_VALUE_LYB format, to learn what exactly is expected see @ref howtoDataLYB. */
#define LYD_NEW_VAL_CANON 0x08       /**< Interpret the provided leaf/leaf-list @p value as being in the canonical
                                          (or JSON if no defined) ::LY_VALUE_CANON format. If it is not, it may lead
                                          to unexpected behavior. */
#define LYD_NEW_META_CLEAR_DFLT 0x10 /**< Whether to clear the default flag starting from @p parent, recursively all NP containers. */
#define LYD_NEW_PATH_UPDATE 0x20     /**< If the target node exists, is a leaf, and it is updated with a new value or its
                                          default flag is changed, it is returned. If the target node exists and is not
                                          a leaf or generally no change occurs in the @p parent tree, NULL is returned and
                                          no error set. */
#define LYD_NEW_PATH_OPAQ 0x40       /**< Enables the creation of opaque nodes with some specific rules. If the __last node__
                                          in the path is not uniquely defined ((leaf-)list without a predicate) or has an
                                          invalid value (leaf/leaf-list), it is created as opaque. Otherwise a regular node
                                          is created. */
#define LYD_NEW_PATH_WITH_OPAQ 0x80  /**< Consider opaque nodes normally when searching for existing nodes. */
#define LYD_NEW_ANY_USE_VALUE 0x100  /**< Whether to use dynamic @p value or make a copy. */

/** @} newvaloptions */

/**
 * @brief Create a new list node in the data tree.
 *
 * @param[in] parent Parent node for the node being created. NULL in case of creating a top level element.
 * @param[in] module Module of the node being created. If NULL, @p parent module will be used.
 * @param[in] name Schema node name of the new data node. The node must be #LYS_LIST.
 * @param[in] options Bitmask of options, see @ref newvaloptions.
 * @param[out] node Optional created node.
 * @param[in] ... Ordered key values of the new list instance, all must be set. In case of an instance-identifier
 * or identityref value, the JSON format is expected (module names instead of prefixes). No keys are expected for key-less lists.
 * In case options include ::LYD_NEW_VAL_BIN, every key value must be followed by its length.
 * @return LY_ERR value.
 */
LIBYANG_API_DECL LY_ERR lyd_new_list(struct lyd_node *parent, const struct lys_module *module, const char *name,
        uint32_t options, struct lyd_node **node, ...);

/**
 * @brief Create a new top-level list node defined in the given extension instance.
 *
 * To create a list node with parent (no matter if defined inside extension instance or a standard tree) or a top-level
 * list node of a standard module's tree, use ::lyd_new_list() or ::lyd_new_list2().
 *
 * @param[in] ext Extension instance where the list node being created is defined.
 * @param[in] name Schema node name of the new data node. The node must be #LYS_LIST.
 * @param[in] options Bitmask of options, see @ref newvaloptions.
 * @param[out] node The created node.
 * @param[in] ... Ordered key values of the new list instance, all must be set. In case of an instance-identifier
 * or identityref value, the JSON format is expected (module names instead of prefixes). No keys are expected for key-less lists.
 * In case options include ::LYD_NEW_VAL_BIN, every key value must be followed by its length.
 * @return LY_ERR value.
 */
LIBYANG_API_DECL LY_ERR lyd_new_ext_list(const struct lysc_ext_instance *ext, const char *name, uint32_t options,
        struct lyd_node **node, ...);

/**
 * @brief Create a new list node in the data tree.
 *
 * @param[in] parent Parent node for the node being created. NULL in case of creating a top level element.
 * @param[in] module Module of the node being created. If NULL, @p parent module will be used.
 * @param[in] name Schema node name of the new data node. The node must be #LYS_LIST.
 * @param[in] keys All key values predicate in the form of "[key1='val1'][key2='val2']...", they do not have to be ordered.
 * In case of an instance-identifier or identityref value, the JSON format is expected (module names instead of prefixes).
 * Use NULL or string of length 0 in case of key-less list.
 * @param[in] options Bitmask of options, see @ref newvaloptions.
 * @param[out] node Optional created node.
 * @return LY_ERR value.
 */
LIBYANG_API_DECL LY_ERR lyd_new_list2(struct lyd_node *parent, const struct lys_module *module, const char *name,
        const char *keys, uint32_t options, struct lyd_node **node);

/**
 * @brief Create a new list node in the data tree.
 *
 * @param[in] parent Parent node for the node being created. NULL in case of creating a top level element.
 * @param[in] module Module of the node being created. If NULL, @p parent module will be used.
 * @param[in] name Schema node name of the new data node. The node must be #LYS_LIST.
 * @param[in] key_values Ordered key string values of the new list instance, all must be set.
 * @param[in] value_lengths Array of lengths of each @p key_values, may be NULL if @p key_values are 0-terminated strings.
 * @param[in] options Bitmask of options, see @ref newvaloptions.
 * @param[out] node Optional created node.
 * @return LY_ERR value.
 */
LIBYANG_API_DECL LY_ERR lyd_new_list3(struct lyd_node *parent, const struct lys_module *module, const char *name,
        const char **key_values, uint32_t *value_lengths, uint32_t options, struct lyd_node **node);

/**
 * @brief Create a new term node in the data tree.
 *
 * To create a top-level term node defined in an extension instance, use ::lyd_new_ext_term().
 * To create a term node based on binary value, use ::lyd_new_term_bin().
 *
 * @param[in] parent Parent node for the node being created. NULL in case of creating a top level element.
 * @param[in] module Module of the node being created. If NULL, @p parent module will be used.
 * @param[in] name Schema node name of the new data node. The node can be #LYS_LEAF or #LYS_LEAFLIST.
 * @param[in] value Value of the node in JSON format unless changed by @p options.
 * @param[in] options Bitmask of options, see @ref newvaloptions.
 * @param[out] node Optional created node.
 * @return LY_ERR value.
 */
LIBYANG_API_DECL LY_ERR lyd_new_term(struct lyd_node *parent, const struct lys_module *module, const char *name,
        const char *value, uint32_t options, struct lyd_node **node);

/**
 * @brief Create a new term node in the data tree based on binary value.
 *
 * @param[in] parent Parent node for the node being created. NULL in case of creating a top level element.
 * @param[in] module Module of the node being created. If NULL, @p parent module will be used.
 * @param[in] name Schema node name of the new data node. The node can be #LYS_LEAF or #LYS_LEAFLIST.
 * @param[in] value Binary value of the node. To learn what exactly is expected see @ref howtoDataLYB.
 * @param[in] value_len Length of @p value.
 * @param[in] options Bitmask of options, see @ref newvaloptions.
 * @param[out] node Optional created node.
 * @return LY_ERR value.
 */
LIBYANG_API_DECL LY_ERR lyd_new_term_bin(struct lyd_node *parent, const struct lys_module *module, const char *name,
        const void *value, size_t value_len, uint32_t options, struct lyd_node **node);

/**
 * @brief Create a new top-level term node defined in the given extension instance.
 *
 * To create a term node with parent (no matter if defined inside extension instance or a standard tree) or a top-level
 * node of a standard module's tree, use ::lyd_new_term().
 *
 * @param[in] ext Extension instance where the term node being created is defined.
 * @param[in] name Schema node name of the new data node. The node can be #LYS_LEAF or #LYS_LEAFLIST.
 * @param[in] value Value of the node in JSON format unless changed by @p options.
 * @param[in] value_len Length of @p value.
 * @param[in] options Bitmask of options, see @ref newvaloptions.
 * @param[out] node The created node.
 * @return LY_ERR value.
 */
LIBYANG_API_DECL LY_ERR lyd_new_ext_term(const struct lysc_ext_instance *ext, const char *name, const void *value,
        size_t value_len, uint32_t options, struct lyd_node **node);

/**
 * @brief Create a new any node in the data tree.
 *
 * To create a top-level any node defined in an extension instance, use ::lyd_new_ext_any().
 *
 * @param[in] parent Parent node for the node being created. NULL in case of creating a top level element.
 * @param[in] module Module of the node being created. If NULL, @p parent module will be used.
 * @param[in] name Schema node name of the new data node. The node can be #LYS_ANYDATA or #LYS_ANYXML.
 * @param[in] value Value for the node. Expected type is determined by @p value_type.
 * @param[in] value_type Type of the provided value in @p value.
 * @param[in] options Bitmask of options, see @ref newvaloptions.
 * @param[out] node Optional created node.
 * @return LY_ERR value.
 */
LIBYANG_API_DECL LY_ERR lyd_new_any(struct lyd_node *parent, const struct lys_module *module, const char *name,
        const void *value, LYD_ANYDATA_VALUETYPE value_type, uint32_t options, struct lyd_node **node);

/**
 * @brief Create a new top-level any node defined in the given extension instance.
 *
 * To create an any node with parent (no matter if defined inside extension instance or a standard tree) or a top-level
 * any node of a standard module's tree, use ::lyd_new_any().
 *
 * @param[in] ext Extension instance where the any node being created is defined.
 * @param[in] name Schema node name of the new data node. The node can be #LYS_ANYDATA or #LYS_ANYXML.
 * @param[in] value Value for the node. Expected type is determined by @p value_type.
 * @param[in] value_type Type of the provided value in @p value.
 * @param[in] options Bitmask of options, see @ref newvaloptions.
 * @param[out] node The created node.
 * @return LY_ERR value.
 */
LIBYANG_API_DECL LY_ERR lyd_new_ext_any(const struct lysc_ext_instance *ext, const char *name, const void *value,
        LYD_ANYDATA_VALUETYPE value_type, uint32_t options, struct lyd_node **node);

/**
 * @brief Create a new metadata.
 *
 * @param[in] ctx libyang context.
 * @param[in] parent Optional parent node for the metadata being created. Must be set if @p meta is NULL.
 * @param[in] module Module of the metadata being created. If NULL, @p name must include module name as the prefix.
 * @param[in] name Annotation name of the new metadata. It can include the annotation module as the prefix.
 * If the prefix is specified it is always used but if not specified, @p module must be set.
 * @param[in] val_str String form of the value of the metadata. In case of an instance-identifier or identityref
 * value, the JSON format is expected (module names instead of prefixes).
 * @param[in] options Bitmask of options, see @ref newvaloptions.
 * @param[out] meta Optional created metadata. Must be set if @p parent is NULL.
 * @return LY_ERR value.
 */
LIBYANG_API_DECL LY_ERR lyd_new_meta(const struct ly_ctx *ctx, struct lyd_node *parent, const struct lys_module *module,
        const char *name, const char *val_str, uint32_t options, struct lyd_meta **meta);

/**
 * @brief Create new metadata from an opaque node attribute if possible.
 *
 * @param[in] ctx libyang context.
 * @param[in] parent Optional parent node for the metadata being created. Must be set if @p meta is NULL.
 * @param[in] options Bitmask of options, see @ref newvaloptions.
 * @param[in] attr Opaque node attribute to parse into metadata.
 * @param[out] meta Optional created metadata. Must be set if @p parent is NULL.
 * @return LY_SUCCESS on success.
 * @return LY_ENOT if the attribute could not be parsed into any metadata.
 * @return LY_ERR on error.
 */
LIBYANG_API_DECL LY_ERR lyd_new_meta2(const struct ly_ctx *ctx, struct lyd_node *parent, uint32_t options,
        const struct lyd_attr *attr, struct lyd_meta **meta);

/**
 * @brief Create a new JSON opaque node in the data tree. To create an XML opaque node, use ::lyd_new_opaq2().
 *
 * @param[in] parent Parent node for the node being created. NULL in case of creating a top level element.
 * @param[in] ctx libyang context. If NULL, @p parent context will be used.
 * @param[in] name Node name.
 * @param[in] value Optional node value.
 * @param[in] prefix Optional node prefix, must be equal to @p module_name if set.
 * @param[in] module_name Node module name.
 * @param[out] node Optional created node.
 * @return LY_ERR value.
 */
LIBYANG_API_DECL LY_ERR lyd_new_opaq(struct lyd_node *parent, const struct ly_ctx *ctx, const char *name, const char *value,
        const char *prefix, const char *module_name, struct lyd_node **node);

/**
 * @brief Create a new XML opaque node in the data tree. To create a JSON opaque node, use ::lyd_new_opaq().
 *
 * @param[in] parent Parent node for the node being created. NULL in case of creating a top level element.
 * @param[in] ctx libyang context. If NULL, @p parent context will be used.
 * @param[in] name Node name.
 * @param[in] value Optional node value.
 * @param[in] prefix Optional node prefix.
 * @param[in] module_ns Node module namespace.
 * @param[out] node Optional created node.
 * @return LY_ERR value.
 */
LIBYANG_API_DECL LY_ERR lyd_new_opaq2(struct lyd_node *parent, const struct ly_ctx *ctx, const char *name, const char *value,
        const char *prefix, const char *module_ns, struct lyd_node **node);

/**
 * @brief Create new JSON attribute for an opaque data node. To create an XML attribute, use ::lyd_new_attr2().
 *
 * Note that for an attribute to be later resolved as YANG metadata, it needs @p module_nane and a prefix in @p name.
 *
 * @param[in] parent Parent opaque node for the attribute.
 * @param[in] module_name Optional name of the module of the attribute.
 * @param[in] name Attribute name with optional prefix, which is a module name. If the prefix is set, it is also stored
 * as the explicit module name if @p module_name is not set.
 * @param[in] value Optional attribute value.
 * @param[out] attr Optional created attribute.
 * @return LY_ERR value.
 */
LIBYANG_API_DECL LY_ERR lyd_new_attr(struct lyd_node *parent, const char *module_name, const char *name, const char *value,
        struct lyd_attr **attr);

/**
 * @brief Create new XML attribute for an opaque data node. To create a JSON attribute, use ::lyd_new_attr().
 *
 * Note that for an attribute to be later resolved as YANG metadata, it needs @p module_ns and a prefix in @p name.
 *
 * @param[in] parent Parent opaque node for the attribute being created.
 * @param[in] module_ns Optional namespace of the module of the attribute.
 * @param[in] name Attribute name with optional prefix, which is an XML prefix.
 * @param[in] value Optional attribute value.
 * @param[out] attr Optional created attribute.
 * @return LY_ERR value.
 */
LIBYANG_API_DECL LY_ERR lyd_new_attr2(struct lyd_node *parent, const char *module_ns, const char *name, const char *value,
        struct lyd_attr **attr);

/**
 * @brief Create a new node in the data tree based on a path. If creating anyxml/anydata nodes, ::lyd_new_path2
 * should be used instead, this function expects the value as string.
 *
 * If creating data nodes defined inside an extension instance, use ::lyd_new_ext_path().
 *
 * If @p path points to a list key, the key value from the predicate is used and @p value is ignored.
 * Also, if a leaf-list is being created and both a predicate is defined in @p path
 * and @p value is set, the predicate is preferred.
 *
 * For key-less lists, positional predicates must be used (indices starting from 1). For non-configuration leaf-lists
 * (or lists) either positional predicate can be used or leaf-list (or key) predicate, when an instance is always
 * created at the end. If no predicate is used for these nodes, they are always created.
 *
 * @param[in] parent Data parent to add to/modify, can be NULL. Note that in case a first top-level sibling is used,
 * it may no longer be first if @p path is absolute and starts with a non-existing top-level node inserted
 * before @p parent. Use ::lyd_first_sibling() to adjust @p parent in these cases.
 * @param[in] ctx libyang context, must be set if @p parent is NULL.
 * @param[in] path [Path](@ref howtoXPath) to create.
 * @param[in] value String value of the new leaf/leaf-list in JSON format. For other node types it should be NULL.
 * @param[in] options Bitmask of options, see @ref newvaloptions.
 * @param[out] node Optional first created node.
 * @return LY_SUCCESS on success.
 * @return LY_EEXIST if the final node to create exists (unless ::LYD_NEW_PATH_UPDATE is used).
 * @return LY_EINVAL on invalid arguments including invalid @p path.
 * @return LY_EVALID on invalid @p value.
 * @return LY_ERR on other errors.
 */
LIBYANG_API_DECL LY_ERR lyd_new_path(struct lyd_node *parent, const struct ly_ctx *ctx, const char *path, const char *value,
        uint32_t options, struct lyd_node **node);

/**
 * @brief Create a new node in the data tree based on a path. All node types can be created.
 *
 * Details are mentioned in ::lyd_new_path().
 *
 * @param[in] parent Data parent to add to/modify, can be NULL. Note that in case a first top-level sibling is used,
 * it may no longer be first if @p path is absolute and starts with a non-existing top-level node inserted
 * before @p parent. Use ::lyd_first_sibling() to adjust @p parent in these cases.
 * @param[in] ctx libyang context, must be set if @p parent is NULL.
 * @param[in] path [Path](@ref howtoXPath) to create.
 * @param[in] value Value of the new leaf/leaf-list (const char *) in ::LY_VALUE_JSON format. If creating an
 * anyxml/anydata node, the expected type depends on @p value_type. For other node types, it should be NULL.
 * @param[in] value_len Length of @p value in bytes. May be 0 if @p value is a zero-terminated string. Ignored when
 * creating anyxml/anydata nodes.
 * @param[in] value_type Anyxml/anydata node @p value type.
 * @param[in] options Bitmask of options, see @ref newvaloptions.
 * @param[out] new_parent Optional first parent node created. If only one node was created, equals to @p new_node.
 * @param[out] new_node Optional target node of @p path (the last created node, the list instance in case of a list).
 * @return LY_SUCCESS on success.
 * @return LY_EEXIST if the final node to create exists (unless ::LYD_NEW_PATH_UPDATE is used).
 * @return LY_EINVAL on invalid arguments including invalid @p path.
 * @return LY_EVALID on invalid @p value.
 * @return LY_ERR on other errors.
 */
LIBYANG_API_DECL LY_ERR lyd_new_path2(struct lyd_node *parent, const struct ly_ctx *ctx, const char *path, const void *value,
        size_t value_len, LYD_ANYDATA_VALUETYPE value_type, uint32_t options, struct lyd_node **new_parent,
        struct lyd_node **new_node);

/**
 * @brief Create a new node defined in the given extension instance. In case of anyxml/anydata nodes, this function expects
 * the @p value as string.
 *
 * If creating data nodes defined in a module's standard tree, use ::lyd_new_path() or ::lyd_new_path2().
 *
 * Details are mentioned in ::lyd_new_path().
 *
 * @param[in] parent Data parent to add to/modify, can be NULL. Note that in case a first top-level sibling is used,
 * it may no longer be first if @p path is absolute and starts with a non-existing top-level node inserted
 * before @p parent. Use ::lyd_first_sibling() to adjust @p parent in these cases.
 * @param[in] ext Extension instance where the node being created is defined.
 * @param[in] path [Path](@ref howtoXPath) to create.
 * @param[in] value Value of the new leaf/leaf-list. For other node types, it should be NULL.
 * @param[in] options Bitmask of options, see @ref newvaloptions.
 * @param[out] node Optional first created node.
 * @return LY_SUCCESS on success.
 * @return LY_EEXIST if the final node to create exists (unless ::LYD_NEW_PATH_UPDATE is used).
 * @return LY_EINVAL on invalid arguments including invalid @p path.
 * @return LY_EVALID on invalid @p value.
 * @return LY_ERR on other errors.
 */
LIBYANG_API_DECL LY_ERR lyd_new_ext_path(struct lyd_node *parent, const struct lysc_ext_instance *ext, const char *path,
        const void *value, uint32_t options, struct lyd_node **node);

/**
 * @ingroup datatree
 * @defgroup implicitoptions Implicit node creation options
 *
 * Various options to change lyd_new_implicit*() behavior.
 *
 * Default behavior:
 * - both configuration and state missing implicit nodes are added.
 * - for existing RPC/action nodes, input implicit nodes are added.
 * - all implicit node types are added (non-presence containers, default leaves, and default leaf-lists).
 * @{
 */

#define LYD_IMPLICIT_NO_STATE    0x01   /**< Do not add any implicit state nodes. */
#define LYD_IMPLICIT_NO_CONFIG   0x02   /**< Do not add any implicit config nodes. */
#define LYD_IMPLICIT_OUTPUT      0x04   /**< For RPC/action nodes, add output implicit nodes instead of input. */
#define LYD_IMPLICIT_NO_DEFAULTS 0x08   /**< Do not add any default nodes (leaves/leaf-lists), only non-presence
                                             containers. */

/** @} implicitoptions */

/**
 * @brief Add any missing implicit nodes into a data subtree. Default nodes with a false "when" are not added.
 *
 * @param[in] tree Tree to add implicit nodes into.
 * @param[in] implicit_options Options for implicit node creation, see @ref implicitoptions.
 * @param[out] diff Optional diff with any created nodes.
 * @return LY_ERR value.
 */
LIBYANG_API_DECL LY_ERR lyd_new_implicit_tree(struct lyd_node *tree, uint32_t implicit_options, struct lyd_node **diff);

/**
 * @brief Add any missing implicit nodes. Default nodes with a false "when" are not added.
 *
 * @param[in,out] tree Tree to add implicit nodes into. Note that in case a first top-level sibling is used,
 * it may no longer be first if an implicit node was inserted before @p tree. Use ::lyd_first_sibling() to
 * adjust @p tree in these cases.
 * @param[in] ctx libyang context, must be set only if @p tree is an empty tree.
 * @param[in] implicit_options Options for implicit node creation, see @ref implicitoptions.
 * @param[out] diff Optional diff with any created nodes.
 * @return LY_ERR value.
 */
LIBYANG_API_DECL LY_ERR lyd_new_implicit_all(struct lyd_node **tree, const struct ly_ctx *ctx, uint32_t implicit_options,
        struct lyd_node **diff);

/**
 * @brief Add any missing implicit nodes of one module. Default nodes with a false "when" are not added.
 *
 * @param[in,out] tree Tree to add implicit nodes into. Note that in case a first top-level sibling is used,
 * it may no longer be first if an implicit node was inserted before @p tree. Use ::lyd_first_sibling() to
 * adjust @p tree in these cases.
 * @param[in] module Module whose implicit nodes to create.
 * @param[in] implicit_options Options for implicit node creation, see @ref implicitoptions.
 * @param[out] diff Optional diff with any created nodes.
 * @return LY_ERR value.
 */
LIBYANG_API_DECL LY_ERR lyd_new_implicit_module(struct lyd_node **tree, const struct lys_module *module,
        uint32_t implicit_options, struct lyd_node **diff);

/**
 * @brief Change the value of a term (leaf or leaf-list) node to a string value.
 *
 * Node changed this way is always considered explicitly set, meaning its default flag
 * is always cleared.
 *
 * @param[in] term Term node to change.
 * @param[in] val_str New value to set, any prefixes are expected in JSON format.
 * @return LY_SUCCESS if value was changed,
 * @return LY_EEXIST if value was the same and only the default flag was cleared,
 * @return LY_ENOT if the values were equal and no change occurred,
 * @return LY_ERR value on other errors.
 */
LIBYANG_API_DECL LY_ERR lyd_change_term(struct lyd_node *term, const char *val_str);

/**
 * @brief Change the value of a term (leaf or leaf-list) node to a binary value.
 *
 * Node changed this way is always considered explicitly set, meaning its default flag
 * is always cleared.
 *
 * @param[in] term Term node to change.
 * @param[in] value New value to set in binary format (usually a pointer), see @ref howtoDataLYB.
 * @param[in] value_len Length of @p value.
 * @return LY_SUCCESS if value was changed,
 * @return LY_EEXIST if value was the same and only the default flag was cleared,
 * @return LY_ENOT if the values were equal and no change occurred,
 * @return LY_ERR value on other errors.
 */
LIBYANG_API_DECL LY_ERR lyd_change_term_bin(struct lyd_node *term, const void *value, size_t value_len);

/**
 * @brief Change the value of a term (leaf or leaf-list) node to a canonical string value.
 *
 * Node changed this way is always considered explicitly set, meaning its default flag
 * is always cleared.
 *
 * @param[in] term Term node to change.
 * @param[in] val_str New value to set in canonical (or JSON if no defined) format. If the value is not
 * canonical, it may lead to unexpected behavior.
 * @return LY_SUCCESS if value was changed,
 * @return LY_EEXIST if value was the same and only the default flag was cleared,
 * @return LY_ENOT if the values were equal and no change occurred,
 * @return LY_ERR value on other errors.
 */
LIBYANG_API_DECL LY_ERR lyd_change_term_canon(struct lyd_node *term, const char *val_str);

/**
 * @brief Change the value of a metadata instance.
 *
 * @param[in] meta Metadata to change.
 * @param[in] val_str New value to set, any prefixes are expected in JSON format.
 * @return LY_SUCCESS if value was changed,
 * @return LY_ENOT if the values were equal and no change occurred,
 * @return LY_ERR value on other errors.
 */
LIBYANG_API_DECL LY_ERR lyd_change_meta(struct lyd_meta *meta, const char *val_str);

/**
 * @brief Insert a child into a parent.
 *
 * - if the node is part of some other tree, it is automatically unlinked.
 * - if the node is the first node of a node list (with no parent), all the subsequent nodes are also inserted.
 *
 * @param[in] parent Parent node to insert into.
 * @param[in] node Node to insert.
 * @return LY_SUCCESS on success.
 * @return LY_ERR error on error.
 */
LIBYANG_API_DECL LY_ERR lyd_insert_child(struct lyd_node *parent, struct lyd_node *node);

/**
 * @brief Insert a node into siblings.
 *
 * - if the node is part of some other tree, it is automatically unlinked.
 * - if the node is the first node of a node list (with no parent), all the subsequent nodes are also inserted.
 *
 * @param[in] sibling Siblings to insert into, can even be NULL.
 * @param[in] node Node to insert.
 * @param[out] first Optionally return the first sibling after insertion. Can be the address of @p sibling.
 * @return LY_SUCCESS on success.
 * @return LY_ERR error on error.
 */
LIBYANG_API_DECL LY_ERR lyd_insert_sibling(struct lyd_node *sibling, struct lyd_node *node, struct lyd_node **first);

/**
 * @brief Insert a node before another node, can be used only for user-ordered nodes.
 * If inserting several siblings, each of them must be inserted individually.
 *
 * - if the node is part of some other tree, it is automatically unlinked.
 *
 * @param[in] sibling Sibling node to insert before.
 * @param[in] node Node to insert.
 * @return LY_SUCCESS on success.
 * @return LY_ERR error on error.
 */
LIBYANG_API_DECL LY_ERR lyd_insert_before(struct lyd_node *sibling, struct lyd_node *node);

/**
 * @brief Insert a node after another node, can be used only for user-ordered nodes.
 * If inserting several siblings, each of them must be inserted individually.
 *
 * - if the node is part of some other tree, it is automatically unlinked.
 *
 * @param[in] sibling Sibling node to insert after.
 * @param[in] node Node to insert.
 * @return LY_SUCCESS on success.
 * @return LY_ERR error on error.
 */
LIBYANG_API_DECL LY_ERR lyd_insert_after(struct lyd_node *sibling, struct lyd_node *node);

/**
 * @brief Unlink the specified node with all the following siblings.
 *
 * @param[in] node Data tree node to be unlinked (together with all the children and following siblings).
 * @return LYS_SUCCESS on success.
 * @return LY_ERR error on error.
 */
LIBYANG_API_DECL LY_ERR lyd_unlink_siblings(struct lyd_node *node);

/**
 * @brief Unlink the specified data subtree.
 *
 * @param[in] node Data tree node to be unlinked (together with all the children).
 * @return LYS_SUCCESS on success.
 * @return LY_ERR error on error.
 */
LIBYANG_API_DECL LY_ERR lyd_unlink_tree(struct lyd_node *node);

/**
 * @brief Free all the nodes (even parents of the node) in the data tree.
 *
 * @param[in] node Any of the nodes inside the tree.
 */
LIBYANG_API_DECL void lyd_free_all(struct lyd_node *node);

/**
 * @brief Free all the sibling nodes (preceding as well as succeeding).
 *
 * @param[in] node Any of the sibling nodes to free.
 */
LIBYANG_API_DECL void lyd_free_siblings(struct lyd_node *node);

/**
 * @brief Free (and unlink) the specified data (sub)tree.
 *
 * @param[in] node Root of the (sub)tree to be freed.
 */
LIBYANG_API_DECL void lyd_free_tree(struct lyd_node *node);

/**
 * @brief Free a single metadata instance.
 *
 * @param[in] meta Metadata to free.
 */
LIBYANG_API_DECL void lyd_free_meta_single(struct lyd_meta *meta);

/**
 * @brief Free the metadata instance with any following instances.
 *
 * @param[in] meta Metadata to free.
 */
LIBYANG_API_DECL void lyd_free_meta_siblings(struct lyd_meta *meta);

/**
 * @brief Free a single attribute.
 *
 * @param[in] ctx Context where the attributes were created.
 * @param[in] attr Attribute to free.
 */
LIBYANG_API_DECL void lyd_free_attr_single(const struct ly_ctx *ctx, struct lyd_attr *attr);

/**
 * @brief Free the attribute with any following attributes.
 *
 * @param[in] ctx Context where the attributes were created.
 * @param[in] attr First attribute to free.
 */
LIBYANG_API_DECL void lyd_free_attr_siblings(const struct ly_ctx *ctx, struct lyd_attr *attr);

/**
 * @brief Check type restrictions applicable to the particular leaf/leaf-list with the given string @p value.
 *
 * The given node is not modified in any way - it is just checked if the @p value can be set to the node.
 *
 * @param[in] ctx libyang context for logging (function does not log errors when @p ctx is NULL)
 * @param[in] schema Schema node of the @p value.
 * @param[in] value String value to be checked, it is expected to be in JSON format.
 * @param[in] value_len Length of the given @p value (mandatory).
 * @param[in] ctx_node Optional data tree context node for the value (leafref target, instance-identifier).
 * If not set and is required for the validation to complete, ::LY_EINCOMPLETE is be returned.
 * @param[out] realtype Optional real type of @p value.
 * @param[out] canonical Optional canonical value of @p value (in the dictionary).
 * @return LY_SUCCESS on success
 * @return LY_EINCOMPLETE in case the @p ctx_node is not provided and it was needed to finish the validation
 * (e.g. due to require-instance).
 * @return LY_ERR value if an error occurred.
 */
LIBYANG_API_DECL LY_ERR lyd_value_validate(const struct ly_ctx *ctx, const struct lysc_node *schema, const char *value,
        size_t value_len, const struct lyd_node *ctx_node, const struct lysc_type **realtype, const char **canonical);

/**
 * @brief Compare the node's value with the given string value. The string value is first validated according to
 * the (current) node's type.
 *
 * @param[in] node Data node to compare.
 * @param[in] value String value to be compared. It does not need to be in a canonical form - as part of the process,
 * it is validated and canonized if possible. But it is expected to be in JSON format.
 * @param[in] value_len Length of the given @p value (mandatory).
 * @return LY_SUCCESS on success,
 * @return LY_ENOT if the values do not match,
 * @return LY_ERR value if an error occurred.
 */
LIBYANG_API_DECL LY_ERR lyd_value_compare(const struct lyd_node_term *node, const char *value, size_t value_len);

/**
 * @ingroup datatree
 * @defgroup datacompareoptions Data compare options
 * @{
 * Various options to change the ::lyd_compare_single() and ::lyd_compare_siblings() behavior.
 */
#define LYD_COMPARE_FULL_RECURSION 0x01 /* Lists and containers are the same only in case all they children
                                           (subtree, so direct as well as indirect children) are the same. By default,
                                           containers are the same in case of the same schema node and lists are the same
                                           in case of equal keys (keyless lists do the full recursion comparison all the time). */
#define LYD_COMPARE_DEFAULTS 0x02       /* By default, implicit and explicit default nodes are considered to be equal. This flag
                                           changes this behavior and implicit (automatically created default node) and explicit
                                           (explicitly created node with the default value) default nodes are considered different. */
#define LYD_COMPARE_OPAQ 0x04           /* Opaque nodes can normally be never equal to data nodes. Using this flag even
                                           opaque nodes members are compared to data node schema and value and can result
                                           in a match. */
/** @} datacompareoptions */

/**
 * @brief Compare 2 data nodes if they are equivalent.
 *
 * Works correctly even if @p node1 and @p node2 have different contexts.
 *
 * @param[in] node1 The first node to compare.
 * @param[in] node2 The second node to compare.
 * @param[in] options Various @ref datacompareoptions.
 * @return LY_SUCCESS if the nodes are equivalent.
 * @return LY_ENOT if the nodes are not equivalent.
 */
LIBYANG_API_DECL LY_ERR lyd_compare_single(const struct lyd_node *node1, const struct lyd_node *node2, uint32_t options);

/**
 * @brief Compare 2 lists of siblings if they are equivalent.
 *
 * Works correctly even if @p node1 and @p node2 have different contexts.
 *
 * @param[in] node1 The first sibling list to compare.
 * @param[in] node2 The second sibling list to compare.
 * @param[in] options Various @ref datacompareoptions.
 * @return LY_SUCCESS if all the siblings are equivalent.
 * @return LY_ENOT if the siblings are not equivalent.
 */
LIBYANG_API_DECL LY_ERR lyd_compare_siblings(const struct lyd_node *node1, const struct lyd_node *node2, uint32_t options);

/**
 * @brief Compare 2 metadata.
 *
 * If @p meta1 and @p meta2 have different contexts, they are never equivalent.
 *
 * @param[in] meta1 First metadata.
 * @param[in] meta2 Second metadata.
 * @return LY_SUCCESS if the metadata are equivalent.
 * @return LY_ENOT if not.
 */
LIBYANG_API_DECL LY_ERR lyd_compare_meta(const struct lyd_meta *meta1, const struct lyd_meta *meta2);

/**
 * @ingroup datatree
 * @defgroup dupoptions Data duplication options
 *
 * Various options to change ::lyd_dup_single() and ::lyd_dup_siblings() behavior.
 *
 * Default behavior:
 * - only the specified node is duplicated without siblings, parents, or children.
 * - all the metadata of the duplicated nodes are also duplicated.
 * @{
 */

#define LYD_DUP_RECURSIVE    0x01  /**< Duplicate not just the node but also all the children. Note that
                                        list's keys are always duplicated. */
#define LYD_DUP_NO_META      0x02  /**< Do not duplicate metadata (or attributes) of any node. Flag has no effect
                                        on 'lyds_tree' metadata. */
#define LYD_DUP_WITH_PARENTS 0x04  /**< If a nested node is being duplicated, duplicate also all the parents.
                                        Keys are also duplicated for lists. Return value does not change! */
#define LYD_DUP_WITH_FLAGS   0x08  /**< Also copy any data node flags. That will cause the duplicated data to preserve
                                        its validation/default node state. */
#define LYD_DUP_NO_EXT       0x10  /**< Do not duplicate nodes with the ::LYD_EXT flag (nested extension instance data). */
#define LYD_DUP_WITH_PRIV    0x20  /**< Also copy data node private pointer. Only the pointer is copied, it still points
                                        to the same data. */
#define LYD_DUP_NO_LYDS      0x40  /**< The order of nodes is used the same as for copied nodes and a 'lyds_tree' is not
                                        created, so the flag is suitable for optimization. If a new node is inserted into
                                        such a (leaf-)list by default, the 'lyds_tree' will be created additionally and
                                        the sorting will work. */

/** @} dupoptions */

/**
 * @brief Create a copy of the specified data tree @p node. Schema references are kept the same.
 *
 * @param[in] node Data tree node to be duplicated.
 * @param[in] parent Optional parent node where to connect the duplicated node(s). If set in combination with
 * ::LYD_DUP_WITH_PARENTS, the missing parents' chain is duplicated and connected with @p parent.
 * @param[in] options Bitmask of options flags, see @ref dupoptions.
 * @param[out] dup Optional created copy of the node. Note that in case the parents chain is duplicated for the duplicated
 * node(s) (when ::LYD_DUP_WITH_PARENTS used), the first duplicated node is still returned.
 * @return LY_ERR value.
 */
LIBYANG_API_DECL LY_ERR lyd_dup_single(const struct lyd_node *node, struct lyd_node_inner *parent, uint32_t options,
        struct lyd_node **dup);

/**
 * @brief Create a copy of the specified data tree @p node. Schema references are assigned from @p trg_ctx.
 *
 * @param[in] node Data tree node to be duplicated.
 * @param[in] trg_ctx Target context for duplicated nodes. In case of mixed contexts in @p node subtree or parents
 * (schema mount data), this is the context of top-level nodes.
 * @param[in] parent Optional parent node where to connect the duplicated node(s). If set in combination with
 * ::LYD_DUP_WITH_PARENTS, the missing parents' chain is duplicated and connected with @p parent.
 * @param[in] options Bitmask of options flags, see @ref dupoptions.
 * @param[out] dup Optional created copy of the node. Note that in case the parents chain is duplicated for the duplicated
 * node(s) (when ::LYD_DUP_WITH_PARENTS used), the first duplicated node is still returned.
 * @return LY_ERR value.
 */
LIBYANG_API_DECL LY_ERR lyd_dup_single_to_ctx(const struct lyd_node *node, const struct ly_ctx *trg_ctx,
        struct lyd_node_inner *parent, uint32_t options, struct lyd_node **dup);

/**
 * @brief Create a copy of the specified data tree @p node with any following siblings. Schema references are kept the same.
 *
 * @param[in] node Data tree node to be duplicated.
 * @param[in] parent Optional parent node where to connect the duplicated node(s). If set in combination with
 * ::LYD_DUP_WITH_PARENTS, the missing parents' chain is duplicated and connected with @p parent.
 * @param[in] options Bitmask of options flags, see @ref dupoptions.
 * @param[out] dup Optional created copy of the node. Note that in case the parents chain is duplicated for the duplicated
 * node(s) (when ::LYD_DUP_WITH_PARENTS used), the first duplicated node is still returned.
 * @return LY_ERR value.
 */
LIBYANG_API_DECL LY_ERR lyd_dup_siblings(const struct lyd_node *node, struct lyd_node_inner *parent, uint32_t options,
        struct lyd_node **dup);

/**
 * @brief Create a copy of the specified data tree @p node with any following siblings. Schema references are assigned
 * from @p trg_ctx.
 *
 * @param[in] node Data tree node to be duplicated.
 * @param[in] trg_ctx Target context for duplicated nodes. In case of mixed contexts in @p node subtree or parents
 * (schema mount data), this is the context of top-level nodes.
 * @param[in] parent Optional parent node where to connect the duplicated node(s). If set in combination with
 * ::LYD_DUP_WITH_PARENTS, the missing parents' chain is duplicated and connected with @p parent.
 * @param[in] options Bitmask of options flags, see @ref dupoptions.
 * @param[out] dup Optional created copy of the node. Note that in case the parents chain is duplicated for the duplicated
 * node(s) (when ::LYD_DUP_WITH_PARENTS used), the first duplicated node is still returned.
 * @return LY_ERR value.
 */
LIBYANG_API_DECL LY_ERR lyd_dup_siblings_to_ctx(const struct lyd_node *node, const struct ly_ctx *trg_ctx,
        struct lyd_node_inner *parent, uint32_t options, struct lyd_node **dup);

/**
 * @brief Create a copy of the metadata.
 *
 * @param[in] meta Metadata to copy.
 * @param[in] parent Node where to append the new metadata.
 * @param[out] dup Optional created metadata copy.
 * @return LY_ERR value.
 */
LIBYANG_API_DECL LY_ERR lyd_dup_meta_single(const struct lyd_meta *meta, struct lyd_node *parent, struct lyd_meta **dup);

/**
 * @ingroup datatree
 * @defgroup mergeoptions Data merge options.
 *
 * Various options to change ::lyd_merge_tree(), ::lyd_merge_siblings(), and ::lyd_merge_module() behavior.
 *
 * Default behavior:
 * - source data tree is not modified in any way,
 * - any default nodes in the source are ignored if there are explicit nodes in the target,
 * - any metadata are ignored - those present in the target are kept, those in the source are not merged.
 * - any merged nodes flags are set as non-validated.
 * @{
 */

#define LYD_MERGE_DESTRUCT      0x01 /**< Spend source data tree in the function, it cannot be used afterwards! */
#define LYD_MERGE_DEFAULTS      0x02 /**< Default nodes in the source tree replace even explicit nodes in the target. */
#define LYD_MERGE_WITH_FLAGS    0x04 /**< Merged nodes (those missing in the source) keep their exact flags. */

/** @} mergeoptions */

/**
 * @brief Merge the source data subtree into the target data tree. Merge may not be complete until validation
 * is called on the resulting data tree (data from more cases may be present, default and non-default values).
 *
 * Example input:
 *
 * source   (A1) - A2 - A3    target   (B1) - B2 - B3
 *           /\    /\   /\              /\    /\   /\
 *          ....  .... ....            ....  .... ....
 *
 * result target  (A1) - B1 - B2 - B3
 *                 /\    /\   /\   /\
 *                ....  .... .... ....
 *
 * @param[in,out] target Target data tree to merge into, must be a top-level tree. Always points to the first sibling.
 * @param[in] source Source data tree to merge, must be a top-level tree.
 * @param[in] options Bitmask of option flags, see @ref mergeoptions.
 * @return LY_SUCCESS on success,
 * @return LY_ERR value on error.
 */
LIBYANG_API_DECL LY_ERR lyd_merge_tree(struct lyd_node **target, const struct lyd_node *source, uint16_t options);

/**
 * @brief Merge the source data tree with any following siblings into the target data tree. Merge may not be
 * complete until validation called on the resulting data tree (data from more cases may be present, default
 * and non-default values).
 *
 * Example input:
 *
 * source   (A1) - A2 - A3    target   (B1) - B2 - B3
 *           /\    /\   /\              /\    /\   /\
 *          ....  .... ....            ....  .... ....
 *
 * result target  (A1) - A2 - A3 - B1 - B2 - B3
 *                 /\    /\   /\   /\   /\   /\
 *                ....  .... .... .... .... ....
 *
 * @param[in,out] target Target data tree to merge into, must be a top-level tree. Always points to the first sibling.
 * @param[in] source Source data tree to merge, must be a top-level tree.
 * @param[in] options Bitmask of option flags, see @ref mergeoptions.
 * @return LY_SUCCESS on success,
 * @return LY_ERR value on error.
 */
LIBYANG_API_DECL LY_ERR lyd_merge_siblings(struct lyd_node **target, const struct lyd_node *source, uint16_t options);

/**
 * @brief Callback for matching merge nodes.
 *
 * @param[in] trg_node Target data node.
 * @param[in] src_node Source data node, is NULL if it was actually duplicated (no target node found) and
 * its copy is @p trg_node.
 * @param[in] cb_data Arbitrary callback data.
 * @return LY_ERR value.
 */
typedef LY_ERR (*lyd_merge_cb)(struct lyd_node *trg_node, const struct lyd_node *src_node, void *cb_data);

/**
 * @brief Merge all the nodes of a module from source data tree into the target data tree. Merge may not be
 * complete until validation called on the resulting data tree (data from more cases may be present, default
 * and non-default values).
 *
 * @param[in,out] target Target data tree to merge into, must be a top-level tree. Always points to the first sibling.
 * @param[in] source Source data tree to merge, must be a top-level tree.
 * @param[in] mod Module, whose source data only to consider, NULL for all modules.
 * @param[in] merge_cb Optional merge callback that will be called for every merged node, before merging its descendants.
 * If a subtree is being added into target (no matching node found), callback is called only once with the subtree root.
 * @param[in] cb_data Arbitrary callback data.
 * @param[in] options Bitmask of option flags, see @ref mergeoptions.
 * @return LY_SUCCESS on success,
 * @return LY_ERR value on error.
 */
LIBYANG_API_DECL LY_ERR lyd_merge_module(struct lyd_node **target, const struct lyd_node *source, const struct lys_module *mod,
        lyd_merge_cb merge_cb, void *cb_data, uint16_t options);

/**
 * @ingroup datatree
 * @defgroup diffoptions Data diff options.
 *
 * Various options to change ::lyd_diff_tree() and ::lyd_diff_siblings() behavior.
 *
 * Default behavior:
 * - any default nodes are treated as non-existent and ignored.
 * - nodes with 'none' operation can appear only in case a leaf node has not changed its value and only its
 *   default flag.
 * - metadata differences are not included in the diff.
 * @{
 */

#define LYD_DIFF_DEFAULTS   0x01 /**< Default nodes in the trees are not ignored but treated similarly to explicit
                                      nodes. Also, leaves and leaf-lists are added into diff even in case only their
                                      default flag (state) was changed. */
#define LYD_DIFF_META       0x02 /**< All metadata are compared and the full difference reported in the diff always in
                                      the form of 'yang:meta-<operation>' metadata. Also, equal nodes with only changes
                                      in their metadata will be present in the diff with the 'none' operation. */

/** @} diffoptions */

/**
 * @brief Learn the differences between 2 data trees.
 *
 * The resulting diff is represented as a data tree with specific metadata from the internal 'yang'
 * module. Most importantly, every node has an effective 'operation' metadata. If there is none
 * defined on the node, it inherits the operation from the nearest parent. Top-level nodes must
 * always have the 'operation' metadata defined. Additional metadata ('orig-default', 'value',
 * 'orig-value', 'key', 'orig-key') are used for storing more information about the value in the first
 * or the second tree.
 *
 * The diff tree is completely independent on the @p first and @p second trees, meaning all
 * the information about the change is stored in the diff and the trees are not needed.
 *
 * __!! Caution !!__
 * The diff tree should never be validated because it may easily not be valid! For example,
 * when data from one case branch are deleted and data from another branch created - data from both
 * branches are then stored in the diff tree simultaneously.
 *
 * @param[in] first First data tree.
 * @param[in] second Second data tree.
 * @param[in] options Bitmask of options flags, see @ref diffoptions.
 * @param[out] diff Generated diff, NULL if there are no differences.
 * @return LY_SUCCESS on success,
 * @return LY_ERR on error.
 */
LIBYANG_API_DECL LY_ERR lyd_diff_tree(const struct lyd_node *first, const struct lyd_node *second, uint16_t options,
        struct lyd_node **diff);

/**
 * @brief Learn the differences between 2 data trees including all the following siblings.
 *
 * Details are mentioned in ::lyd_diff_tree().
 *
 * @param[in] first First data tree.
 * @param[in] second Second data tree.
 * @param[in] options Bitmask of options flags, see @ref diffoptions.
 * @param[out] diff Generated diff, NULL if there are no differences.
 * @return LY_SUCCESS on success,
 * @return LY_ERR on error.
 */
LIBYANG_API_DECL LY_ERR lyd_diff_siblings(const struct lyd_node *first, const struct lyd_node *second, uint16_t options,
        struct lyd_node **diff);

/**
 * @brief Callback for diff nodes.
 *
 * @param[in] diff_node Diff node.
 * @param[in] data_node Matching node in data.
 * @param[in] cb_data Arbitrary callback data.
 * @return LY_ERR value.
 */
typedef LY_ERR (*lyd_diff_cb)(const struct lyd_node *diff_node, struct lyd_node *data_node, void *cb_data);

/**
 * @brief Apply the whole diff on a data tree but restrict the operation to one module.
 *
 * __!! Caution !!__
 * If applying a diff that was created __without__ the ::LYD_DIFF_DEFAULTS flag, there may be some duplicate values
 * created. Unless the resulting tree is validated (and default values thus consolidated), using it further
 * (such as applying another diff) may cause unexpected results or errors.
 *
 * @param[in,out] data Data to apply the diff on.
 * @param[in] diff Diff to apply.
 * @param[in] mod Module, whose diff/data only to consider, NULL for all modules.
 * @param[in] diff_cb Optional diff callback that will be called for every changed node.
 * @param[in] cb_data Arbitrary callback data.
 * @return LY_SUCCESS on success,
 * @return LY_ERR on error.
 */
LIBYANG_API_DECL LY_ERR lyd_diff_apply_module(struct lyd_node **data, const struct lyd_node *diff,
        const struct lys_module *mod, lyd_diff_cb diff_cb, void *cb_data);

/**
 * @brief Apply the whole diff tree on a data tree.
 *
 * Details are mentioned in ::lyd_diff_apply_module().
 *
 * @param[in,out] data Data to apply the diff on.
 * @param[in] diff Diff to apply.
 * @return LY_SUCCESS on success,
 * @return LY_ERR on error.
 */
LIBYANG_API_DECL LY_ERR lyd_diff_apply_all(struct lyd_node **data, const struct lyd_node *diff);

/**
 * @ingroup datatree
 * @defgroup diffmergeoptions Data diff merge options.
 *
 * Various options to change ::lyd_diff_merge_module(), ::lyd_diff_merge_tree(), and ::lyd_diff_merge_all() behavior.
 *
 * Default behavior:
 * - any default nodes are expected to be a result of validation corrections and not explicitly modified.
 * @{
 */

#define LYD_DIFF_MERGE_DEFAULTS   0x01 /**< Default nodes in the diffs are treated as possibly explicitly modified. */

/** @} diffmergeoptions */

/**
 * @brief Merge 2 diffs into each other but restrict the operation to one module.
 *
 * The diffs must be possible to be merged, which is guaranteed only if the source diff was
 * created on data that had the target diff applied on them. In other words, this sequence is legal
 *
 * 1) get diff1 from data1 and data2 -> get data11 from apply diff1 on data1 -> get diff2 from data11 and data3 ->
 *    -> get data 33 from apply diff2 on data1
 *
 * and reusing these diffs
 *
 * 2) get diff11 from merge diff1 and diff2 -> get data33 from apply diff11 on data1
 *
 * @param[in,out] diff Target diff to merge into.
 * @param[in] src_diff Source diff.
 * @param[in] mod Module, whose diff only to consider, NULL for all modules.
 * @param[in] diff_cb Optional diff callback that will be called for every merged node. Param @p diff_node is the source
 * diff node while @p data_node is the updated target diff node. In case a whole subtree is added, the callback is
 * called on the root with @p diff_node being NULL.
 * @param[in] cb_data Arbitrary callback data.
 * @param[in] options Bitmask of options flags, see @ref diffmergeoptions.
 * @return LY_SUCCESS on success,
 * @return LY_ERR on error.
 */
LIBYANG_API_DECL LY_ERR lyd_diff_merge_module(struct lyd_node **diff, const struct lyd_node *src_diff,
        const struct lys_module *mod, lyd_diff_cb diff_cb, void *cb_data, uint16_t options);

/**
 * @brief Merge 2 diff trees into each other.
 *
 * Details are mentioned in ::lyd_diff_merge_module().
 *
 * @param[in,out] diff_first Target diff first sibling to merge into.
 * @param[in] diff_parent Target diff parent to merge into.
 * @param[in] src_sibling Source diff sibling to merge.
 * @param[in] diff_cb Optional diff callback that will be called for every merged node. Param @p diff_node is the source
 * diff node while @p data_node is the updated target diff node. In case a whole subtree is added, the callback is
 * called on the root with @p diff_node being NULL.
 * @param[in] cb_data Arbitrary callback data.
 * @param[in] options Bitmask of options flags, see @ref diffmergeoptions.
 * @return LY_SUCCESS on success,
 * @return LY_ERR on error.
 */
LIBYANG_API_DECL LY_ERR lyd_diff_merge_tree(struct lyd_node **diff_first, struct lyd_node *diff_parent,
        const struct lyd_node *src_sibling, lyd_diff_cb diff_cb, void *cb_data, uint16_t options);

/**
 * @brief Merge 2 diffs into each other.
 *
 * Details are mentioned in ::lyd_diff_merge_module().
 *
 * @param[in,out] diff Target diff to merge into.
 * @param[in] src_diff Source diff.
 * @param[in] options Bitmask of options flags, see @ref diffmergeoptions.
 * @return LY_SUCCESS on success,
 * @return LY_ERR on error.
 */
LIBYANG_API_DECL LY_ERR lyd_diff_merge_all(struct lyd_node **diff, const struct lyd_node *src_diff, uint16_t options);

/**
 * @brief Reverse a diff and make the opposite changes. Meaning change create to delete, delete to create,
 * or move from place A to B to move from B to A and so on.
 *
 * @param[in] src_diff Diff to reverse.
 * @param[out] diff Reversed diff.
 * @return LY_SUCCESS on success.
 * @return LY_ERR on error.
 */
LIBYANG_API_DECL LY_ERR lyd_diff_reverse_all(const struct lyd_node *src_diff, struct lyd_node **diff);

/**
 * @brief Types of the different data paths.
 */
typedef enum {
    LYD_PATH_STD, /**< Generic data path used for logging, node searching (::lyd_find_xpath(), ::lys_find_path()) as well as
                       creating new nodes (::lyd_new_path(), ::lyd_new_path2(), ::lyd_new_ext_path()). */
    LYD_PATH_STD_NO_LAST_PRED  /**< Similar to ::LYD_PATH_STD except there is never a predicate on the last node. While it
                                    can be used to search for nodes, do not use it to create new data nodes (lists). */
} LYD_PATH_TYPE;

/**
 * @brief Generate path of the given node in the requested format.
 *
 * The path is constructed based on the parent node(s) of this node. When run on a node which is disconnected
 * from its parent(s), this function might yield unexpected results such as `/example:b` instead of the expected
 * `/example:a/b`.
 *
 * @param[in] node Data path of this node will be generated.
 * @param[in] pathtype Format of the path to generate.
 * @param[in,out] buffer Prepared buffer of the @p buflen length to store the generated path.
 *                If NULL, memory for the complete path is allocated.
 * @param[in] buflen Size of the provided @p buffer.
 * @return NULL in case of memory allocation error, path of the node otherwise.
 * In case the @p buffer is NULL, the returned string is dynamically allocated and caller is responsible to free it.
 */
LIBYANG_API_DECL char *lyd_path(const struct lyd_node *node, LYD_PATH_TYPE pathtype, char *buffer, size_t buflen);

/**
 * @brief Find a specific metadata.
 *
 * @param[in] first First metadata to consider.
 * @param[in] module Module of the metadata definition, may be NULL if @p name includes a prefix.
 * @param[in] name Name of the metadata to find, may not include a prefix (module name) if @p module is set.
 * @return Found metadata,
 * @return NULL if not found.
 */
LIBYANG_API_DECL struct lyd_meta *lyd_find_meta(const struct lyd_meta *first, const struct lys_module *module,
        const char *name);

/**
 * @brief Search in the given siblings (NOT recursively) for the first target instance with the same value.
 * Uses hashes - should be used whenever possible for best performance.
 *
 * @param[in] siblings Siblings to search in including preceding and succeeding nodes.
 * @param[in] target Target node to find.
 * @param[out] match Can be NULL, otherwise the found data node.
 * @return LY_SUCCESS on success, @p match set.
 * @return LY_ENOTFOUND if not found, @p match set to NULL.
 * @return LY_ERR value if another error occurred.
 */
LIBYANG_API_DECL LY_ERR lyd_find_sibling_first(const struct lyd_node *siblings, const struct lyd_node *target,
        struct lyd_node **match);

/**
 * @brief Search in the given siblings for the first schema instance.
 * Uses hashes - should be used whenever possible for best performance.
 *
 * @param[in] siblings Siblings to search in including preceding and succeeding nodes.
 * @param[in] schema Schema node of the data node to find.
 * @param[in] key_or_value If it is NULL, the first schema node data instance is found. For nodes with many
 * instances, it can be set based on the type of @p schema:
 *              LYS_LEAFLIST:
 *                  Searched instance value.
 *              LYS_LIST:
 *                  Searched instance key values in the form of "[key1='val1'][key2='val2']...".
 *                  The keys do not have to be ordered but all of them must be set.
 *
 *              Note that any explicit values (leaf-list or list key values) will be canonized first
 *              before comparison. But values that do not have a canonical value are expected to be in the
 *              JSON format!
 * @param[in] val_len Optional length of @p key_or_value in case it is not 0-terminated.
 * @param[out] match Can be NULL, otherwise the found data node.
 * @return LY_SUCCESS on success, @p match set.
 * @return LY_ENOTFOUND if not found, @p match set to NULL.
 * @return LY_EINVAL if @p schema is a key-less list.
 * @return LY_ERR value if another error occurred.
 */
LIBYANG_API_DECL LY_ERR lyd_find_sibling_val(const struct lyd_node *siblings, const struct lysc_node *schema,
        const char *key_or_value, size_t val_len, struct lyd_node **match);

/**
 * @brief Search the given siblings for all the exact same instances of a specific node instance.
 * Uses hashes to whatever extent possible.
 *
 * @param[in] siblings Siblings to search in including preceding and succeeding nodes.
 * @param[in] target Target node instance to find.
 * @param[out] set Set with all the found instances. The first item is always the first instance.
 * @return LY_SUCCESS on success, @p set returned.
 * @return LY_ENOTFOUND if not found, empty @p set returned.
 * @return LY_ERR value if another error occurred.
 */
LIBYANG_API_DECL LY_ERR lyd_find_sibling_dup_inst_set(const struct lyd_node *siblings, const struct lyd_node *target,
        struct ly_set **set);

/**
 * @brief Search the given siblings for an opaque node with a specific name.
 *
 * @param[in] first First sibling to consider.
 * @param[in] name Opaque node name to find.
 * @param[out] match Can be NULL, otherwise the found data node.
 * @return LY_SUCCESS on success, @p match set.
 * @return LY_ENOTFOUND if not found, @p match set to NULL.
 * @return LY_ERR value is an error occurred.
 */
LIBYANG_API_DECL LY_ERR lyd_find_sibling_opaq_next(const struct lyd_node *first, const char *name, struct lyd_node **match);

/**
 * @brief Set a new XPath variable to @p vars.
 *
 * @param[in,out] vars Pointer to [sized array](@ref sizedarrays) of XPath variables.
 * To create a new array, set the @p vars target pointer to NULL.
 * Otherwise variable named @p name with a value @p value will be added to the @p vars
 * or its value will be changed if the variable is already defined.
 * @param[in] name Name of the added/edited variable.
 * @param[in] value Value of the variable.
 * @return LY_ERR value.
 */
LIBYANG_API_DECL LY_ERR lyxp_vars_set(struct lyxp_var **vars, const char *name, const char *value);

/**
 * @brief Free the XPath variables.
 *
 * @param[in] vars [Sized array](@ref sizedarrays) of XPath variables.
 */
LIBYANG_API_DECL void lyxp_vars_free(struct lyxp_var *vars);

/**
 * @brief Search in the given data for instances of nodes matching the provided XPath.
 *
 * If a list instance is being selected with all its key values specified and ordered
 * in the form `list[key1=...][key2=...][key3=...]` or a leaf-list instance in the form
 * `leaf-list[.=...]`, these instances are found using hashes with constant (*O(1)*) complexity
 * (unless they are defined in top-level). Other predicates can still follow the aforementioned ones.
 *
 * Opaque nodes are part of the evaluation.
 *
 * @param[in] ctx_node XPath context node.
 * @param[in] xpath [XPath](@ref howtoXPath) to select in JSON format. It must evaluate into a node set.
 * @param[out] set Set of found data nodes. In case the result is a number, a string, or a boolean,
 * the returned set is empty.
 * @return LY_SUCCESS on success, @p set is returned.
 * @return LY_ERR value if an error occurred.
 */
LIBYANG_API_DECL LY_ERR lyd_find_xpath(const struct lyd_node *ctx_node, const char *xpath, struct ly_set **set);

/**
 * @brief Search in the given data for instances of nodes matching the provided XPath.
 *
 * It is ::lyd_find_xpath() with @p vars added.
 *
 * @param[in] ctx_node XPath context node.
 * @param[in] xpath [XPath](@ref howtoXPath) to select in JSON format.
 * @param[in] vars [Sized array](@ref sizedarrays) of XPath variables.
 * @param[out] set Set of found data nodes. In case the result is a number, a string, or a boolean,
 * the returned set is empty.
 * @return LY_SUCCESS on success, @p set is returned.
 * @return LY_ERR value if an error occurred.
 */
LIBYANG_API_DECL LY_ERR lyd_find_xpath2(const struct lyd_node *ctx_node, const char *xpath, const struct lyxp_var *vars,
        struct ly_set **set);

/**
 * @brief Search in the given data for instances of nodes matching the provided XPath.
 *
 * It is ::lyd_find_xpath2() with @p tree added so that @p ctx_node may be the root and
 * also @p format and @p prefix_data added for expressions in different formats than JSON.
 *
 * @param[in] ctx_node XPath context node, NULL for the root node.
 * @param[in] tree Data tree to evaluate on.
 * @param[in] xpath [XPath](@ref howtoXPath) to select with prefixes in @p format.
 * @param[in] format Format of any prefixes in @p xpath.
 * @param[in] prefix_data Format-specific prefix data.
 * @param[in] vars [Sized array](@ref sizedarrays) of XPath variables.
 * @param[out] set Set of found data nodes. In case the result is a number, a string, or a boolean,
 * the returned set is empty.
 * @return LY_SUCCESS on success, @p set is returned.
 * @return LY_ERR value if an error occurred.
 */
LIBYANG_API_DECL LY_ERR lyd_find_xpath3(const struct lyd_node *ctx_node, const struct lyd_node *tree, const char *xpath,
        LY_VALUE_FORMAT format, void *prefix_data, const struct lyxp_var *vars, struct ly_set **set);

/**
 * @brief Evaluate an XPath on data and return the result converted to boolean.
 *
 * Optimizations similar as in ::lyd_find_xpath().
 *
 * @param[in] ctx_node XPath context node.
 * @param[in] xpath [XPath](@ref howtoXPath) to select in JSON format.
 * @param[out] result Expression result converted to boolean.
 * @return LY_SUCCESS on success, @p result is returned.
 * @return LY_ERR value if an error occurred.
 */
LIBYANG_API_DECL LY_ERR lyd_eval_xpath(const struct lyd_node *ctx_node, const char *xpath, ly_bool *result);

/**
 * @brief Evaluate an XPath on data and return the result converted to boolean.
 *
 * It is ::lyd_eval_xpath() with @p vars added.
 *
 * @param[in] ctx_node XPath context node.
 * @param[in] xpath [XPath](@ref howtoXPath) to select in JSON format.
 * @param[in] vars [Sized array](@ref sizedarrays) of XPath variables.
 * @param[out] result Expression result converted to boolean.
 * @return LY_SUCCESS on success, @p result is returned.
 * @return LY_ERR value if an error occurred.
 */
LIBYANG_API_DECL LY_ERR lyd_eval_xpath2(const struct lyd_node *ctx_node, const char *xpath,
        const struct lyxp_var *vars, ly_bool *result);

/**
 * @brief Evaluate an XPath on data and return the result converted to boolean.
 *
 * It is ::lyd_eval_xpath2() with @p format and @p prefix_data added for special use-cases.
 *
 * @param[in] ctx_node XPath context node.
 * @param[in] cur_mod Current module of @p xpath, needed for some kinds of @p format.
 * @param[in] xpath [XPath](@ref howtoXPath) to select with prefixes in in @p format.
 * @param[in] format Format of any prefixes in @p xpath.
 * @param[in] prefix_data Format-specific prefix data.
 * @param[in] vars [Sized array](@ref sizedarrays) of XPath variables.
 * @param[out] result Expression result converted to boolean.
 * @return LY_SUCCESS on success, @p result is returned.
 * @return LY_ERR value if an error occurred.
 */
LIBYANG_API_DECL LY_ERR lyd_eval_xpath3(const struct lyd_node *ctx_node, const struct lys_module *cur_mod,
        const char *xpath, LY_VALUE_FORMAT format, void *prefix_data, const struct lyxp_var *vars, ly_bool *result);

/**
 * @brief XPath result type.
 */
typedef enum {
    LY_XPATH_NODE_SET,  /**< XPath node set */
    LY_XPATH_STRING,    /**< XPath string */
    LY_XPATH_NUMBER,    /**< XPath number */
    LY_XPATH_BOOLEAN    /**< XPath boolean */
} LY_XPATH_TYPE;

/**
 * @brief Evaluate an XPath on data and return the result or convert it first to an expected result type.
 *
 * Either all return type parameters @p node_set, @p string, @p number, and @p boolean with @p ret_type
 * are provided or exactly one of @p node_set, @p string, @p number, and @p boolean is provided with @p ret_type
 * being obvious and hence optional.
 *
 * @param[in] ctx_node XPath context node, NULL for the root node.
 * @param[in] tree Data tree to evaluate on.
 * @param[in] cur_mod Current module of @p xpath, needed for some kinds of @p format.
 * @param[in] xpath [XPath](@ref howtoXPath) to select.
 * @param[in] format Format of any prefixes in @p xpath.
 * @param[in] prefix_data Format-specific prefix data.
 * @param[in] vars Optional [sized array](@ref sizedarrays) of XPath variables.
 * @param[out] ret_type XPath type of the result selecting which of @p node_set, @p string, @p number, and @p boolean to use.
 * @param[out] node_set XPath node set result.
 * @param[out] string XPath string result.
 * @param[out] number XPath number result.
 * @param[out] boolean XPath boolean result.
 * @return LY_SUCCESS on success.
 * @return LY_ERR value on error.
 */
LIBYANG_API_DECL LY_ERR lyd_eval_xpath4(const struct lyd_node *ctx_node, const struct lyd_node *tree,
        const struct lys_module *cur_mod, const char *xpath, LY_VALUE_FORMAT format, void *prefix_data,
        const struct lyxp_var *vars, LY_XPATH_TYPE *ret_type, struct ly_set **node_set, char **string,
        long double *number, ly_bool *boolean);

/**
 * @brief Evaluate an XPath on data and free all the nodes except the subtrees selected by the expression.
 *
 * @param[in,out] tree Data tree to evaluate on and trim.
 * @param[in] xpath [XPath](@ref howtoXPath) to select in JSON format.
 * @param[in] vars Optional [sized array](@ref sizedarrays) of XPath variables.
 * @return LY_SUCCESS on success.
 * @return LY_ERR value on error.
 */
LIBYANG_API_DEF LY_ERR lyd_trim_xpath(struct lyd_node **tree, const char *xpath, const struct lyxp_var *vars);

/**
 * @brief Search in given data for a node uniquely identified by a path.
 *
 * Always works in constant (*O(1)*) complexity. To be exact, it is *O(n)* where *n* is the depth
 * of the path used.
 *
 * Opaque nodes are NEVER found/traversed.
 *
 * @param[in] ctx_node Path context node.
 * @param[in] path [Path](@ref howtoXPath) to find.
 * @param[in] output Whether to search in RPC/action output nodes or in input nodes.
 * @param[out] match Can be NULL, otherwise the found data node.
 * @return LY_SUCCESS on success, @p match is set to the found node.
 * @return LY_EINCOMPLETE if only a parent of the node was found, @p match is set to this parent node.
 * @return LY_ENOTFOUND if no nodes in the path were found.
 * @return LY_ERR on other errors.
 */
LIBYANG_API_DECL LY_ERR lyd_find_path(const struct lyd_node *ctx_node, const char *path, ly_bool output,
        struct lyd_node **match);

/**
 * @brief Find the target node of a compiled path (::lyd_value instance-identifier).
 *
 * @param[in] path Compiled path structure.
 * @param[in] tree Data tree to be searched.
 * @param[out] match Can be NULL, otherwise the found data node.
 * @return LY_SUCCESS on success, @p match is set to the found node.
 * @return LY_ENOTFOUND if no match was found.
 * @return LY_ERR on other errors.
 */
LIBYANG_API_DECL LY_ERR lyd_find_target(const struct ly_path *path, const struct lyd_node *tree, struct lyd_node **match);

/**
 * @brief Get current timezone (including DST setting) UTC (GMT) time offset in seconds.
 *
 * @return Timezone shift in seconds.
 */
LIBYANG_API_DECL int ly_time_tz_offset(void);

/**
 * @brief Get UTC (GMT) timezone offset in seconds at a specific timestamp (including DST setting).
 *
 * @param[in] time Timestamp to get the offset at.
 * @return Timezone shift in seconds.
 */
LIBYANG_API_DECL int ly_time_tz_offset_at(time_t time);

/**
 * @brief Convert date-and-time from string to UNIX timestamp and fractions of a second.
 *
 * @param[in] value Valid string date-and-time value, the string may continue after the value (be longer).
 * @param[out] time UNIX timestamp.
 * @param[out] fractions_s Optional fractions of a second, set to NULL if none.
 * @return LY_ERR value.
 */
LIBYANG_API_DECL LY_ERR ly_time_str2time(const char *value, time_t *time, char **fractions_s);

/**
 * @brief Convert UNIX timestamp and fractions of a second into canonical date-and-time string value.
 *
 * @param[in] time UNIX timestamp.
 * @param[in] fractions_s Fractions of a second, if any.
 * @param[out] str String date-and-time value in the local timezone.
 * @return LY_ERR value.
 */
LIBYANG_API_DECL LY_ERR ly_time_time2str(time_t time, const char *fractions_s, char **str);

/**
 * @brief Convert date-and-time from string to timespec.
 *
 * @param[in] value Valid string date-and-time value, the string may continue after the value (be longer).
 * @param[out] ts Timespec.
 * @return LY_ERR value.
 */
LIBYANG_API_DECL LY_ERR ly_time_str2ts(const char *value, struct timespec *ts);

/**
 * @brief Convert timespec into date-and-time string value.
 *
 * @param[in] ts Timespec.
 * @param[out] str String date-and-time value in the local timezone.
 * @return LY_ERR value.
 */
LIBYANG_API_DECL LY_ERR ly_time_ts2str(const struct timespec *ts, char **str);

/**
 * @brief Gets the leafref links record for given node
 *
 * This API requires usage of ::LY_CTX_LEAFREF_LINKING context flag.
 *
 * @param[in] node The term data node.
 * @param[out] record The leafref links record
 * @return LY_SUCCESS on success.
 * @return LY_ERR value on error.
 */
LIBYANG_API_DECL LY_ERR lyd_leafref_get_links(const struct lyd_node_term *node, const struct lyd_leafref_links_rec **record);

/**
 * @brief Traverse through data tree including root node siblings and adds leafrefs links to the given nodes
 *
 * This API requires usage of ::LY_CTX_LEAFREF_LINKING context flag.
 *
 * @param[in] tree The data tree root node.
 * @return LY_SUCCESS on success.
 * @return LY_ERR value on error.
 */
LIBYANG_API_DECL LY_ERR lyd_leafref_link_node_tree(const struct lyd_node *tree);

/**
 * @brief Check a string matches an XML Schema regex used in YANG.
 *
 * @param[in] ctx Optional context for storing errors.
 * @param[in] pattern Regular expression pattern to use.
 * @param[in] string String to match.
 * @param[in] str_len Length of @p string, may be 0 if string is 0-terminated.
 * @param[in,out] pcode Optional pointer to PCRE2 code. If set and NULL, it is returned. If set and non-NULL, it is
 * used directly for matching instead of compiling @p pattern. Free it using pcre2_code_free().
 * @return LY_SUCCESS on a match;
 * @return LY_ENOT if the string does not match;
 * @return LY_ERR on error.
 */
LIBYANG_API_DECL LY_ERR ly_pattern_match(const struct ly_ctx *ctx, const char *pattern, const char *string,
        uint32_t str_len, pcre2_code **pcode);

/**
 * @brief Compile an XML Schema regex pattern prior to matching.
 *
 * @param[in] ctx Optional context for storing errors.
 * @param[in] pattern Regular expression pattern to use.
 * @param[out] pcode Compiled @p pattern to be used by ::ly_pattern_match(). Free it using pcre2_code_free().
 * @return LY_SUCCESS on success;
 * @return LY_ERR on error.
 */
LIBYANG_API_DECL LY_ERR ly_pattern_compile(const struct ly_ctx *ctx, const char *pattern, pcre2_code **pcode);

#ifdef __cplusplus
}
#endif

#endif /* LY_TREE_DATA_H_ */
