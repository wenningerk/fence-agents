/*
 * Copyright 2024-2025 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU General Public License version 2
 * or later (GPLv2+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <sys/param.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

#include <stdlib.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <glib.h>

#include <crm/crm.h>
#include <crm/common/ipc.h>
#include <crm/stonith-ng.h>
#include <crm/fencing/internal.h>
#include <pacemaker-internal.h>

#define SUMMARY "stonith_async - Trigger asynchronous fencing operations without blocking"

// Structure to track a fence operation
struct fence_op {
    char *node;
    char *action;
    int call_id;
    int rc;
};

// Global options
static struct {
    GList *fence_list;      // nodes to fence (off)
    GList *unfence_list;    // nodes to unfence (on)
    GList *reboot_list;     // nodes to reboot
    int timeout;
    int delay;
    unsigned int tolerance_ms;
    gboolean quiet;
    char *tag;
} options = {
    .fence_list = NULL,
    .unfence_list = NULL,
    .reboot_list = NULL,
    .timeout = 120,
    .delay = 0,
    .tolerance_ms = 0,
    .quiet = FALSE,
    .tag = NULL
};

// Function prototypes
static gboolean add_fence_node(const gchar *option_name, const gchar *optarg,
                               gpointer data, GError **error);
static gboolean add_unfence_node(const gchar *option_name, const gchar *optarg,
                                 gpointer data, GError **error);
static gboolean add_reboot_node(const gchar *option_name, const gchar *optarg,
                                gpointer data, GError **error);
static gboolean add_tolerance(const gchar *option_name, const gchar *optarg,
                              gpointer data, GError **error);
static gboolean set_tag(const gchar *option_name, const gchar *optarg,
                        gpointer data, GError **error);

/* *INDENT-OFF* */
static GOptionEntry main_entries[] = {
    { "fence", 'F', 0, G_OPTION_ARG_CALLBACK, add_fence_node,
      "Fence (power off) the specified node asynchronously",
      "NODE" },
    { "unfence", 'U', 0, G_OPTION_ARG_CALLBACK, add_unfence_node,
      "Unfence (power on) the specified node asynchronously",
      "NODE" },
    { "reboot", 'B', 0, G_OPTION_ARG_CALLBACK, add_reboot_node,
      "Reboot the specified node asynchronously",
      "NODE" },
    { "timeout", 't', 0, G_OPTION_ARG_INT, &options.timeout,
      "Timeout in seconds (default: 120)",
      "SECONDS" },
    { "delay", 'y', 0, G_OPTION_ARG_INT, &options.delay,
      "Apply fencing delay in seconds (default: 0)",
      "SECONDS" },
    { "tolerance", 0, 0, G_OPTION_ARG_CALLBACK, add_tolerance,
      "Do nothing if an equivalent operation succeeded within this many seconds\n"
      "                                   (default: 0)",
      "SECONDS" },
    { "tag", 'T', 0, G_OPTION_ARG_CALLBACK, set_tag,
      "Tag for log identification",
      "TAG" },
    { "quiet", 'q', 0, G_OPTION_ARG_NONE, &options.quiet,
      "Print only call IDs, no descriptive text",
      NULL },

    { NULL }
};
/* *INDENT-ON* */

static gboolean
add_fence_node(const gchar *option_name, const gchar *optarg, gpointer data,
               GError **error)
{
    options.fence_list = g_list_append(options.fence_list, g_strdup(optarg));
    return TRUE;
}

static gboolean
add_unfence_node(const gchar *option_name, const gchar *optarg, gpointer data,
                 GError **error)
{
    options.unfence_list = g_list_append(options.unfence_list, g_strdup(optarg));
    return TRUE;
}

static gboolean
add_reboot_node(const gchar *option_name, const gchar *optarg, gpointer data,
                GError **error)
{
    options.reboot_list = g_list_append(options.reboot_list, g_strdup(optarg));
    return TRUE;
}

static gboolean
add_tolerance(const gchar *option_name, const gchar *optarg, gpointer data,
              GError **error)
{
    options.tolerance_ms = (unsigned int) (crm_parse_interval_spec(optarg) / 1000);
    return TRUE;
}

static gboolean
set_tag(const gchar *option_name, const gchar *optarg, gpointer data,
        GError **error)
{
    pcmk__str_update(&options.tag, optarg);
    return TRUE;
}

static struct fence_op *
create_fence_op(const char *node, const char *action)
{
    struct fence_op *op = calloc(1, sizeof(struct fence_op));

    if (op != NULL) {
        op->node = strdup(node);
        op->action = strdup(action);
        op->call_id = -1;
        op->rc = pcmk_rc_ok;
    }
    return op;
}

static void
free_fence_op(gpointer data)
{
    struct fence_op *op = (struct fence_op *) data;

    if (op != NULL) {
        free(op->node);
        free(op->action);
        free(op);
    }
}

static int
submit_fence_op(stonith_t *st, struct fence_op *op)
{
    int call_options = st_opt_allow_self_fencing;
    int call_id;

    // Set tag if provided
    if (options.tag != NULL) {
        stonith__set_call_options(call_options, options.tag, st_opt_verbose);
    }

    // Critical: NO st_opt_sync_call flag - this makes the operation async
    call_id = st->cmds->fence_with_delay(st, call_options, op->node, op->action,
                                         options.timeout,
                                         options.tolerance_ms / 1000,
                                         options.delay);

    return call_id;
}

static void
print_fence_result(struct fence_op *op)
{
    if (op->call_id < 0) {
        fprintf(stderr, "Error: Failed to submit %s operation for node %s: %s\n",
                op->action, op->node, pcmk_rc_str(pcmk_legacy2rc(op->call_id)));
    } else if (options.quiet) {
        printf("%d\n", op->call_id);
    } else {
        printf("%d:%s:%s\n", op->call_id, op->node, op->action);
    }
}

static GList *
build_operation_list(void)
{
    GList *operations = NULL;
    GList *iter;

    // Add fence operations
    for (iter = options.fence_list; iter != NULL; iter = iter->next) {
        char *node = (char *) iter->data;
        operations = g_list_append(operations, create_fence_op(node, PCMK_ACTION_OFF));
    }

    // Add unfence operations
    for (iter = options.unfence_list; iter != NULL; iter = iter->next) {
        char *node = (char *) iter->data;
        operations = g_list_append(operations, create_fence_op(node, PCMK_ACTION_ON));
    }

    // Add reboot operations
    for (iter = options.reboot_list; iter != NULL; iter = iter->next) {
        char *node = (char *) iter->data;
        operations = g_list_append(operations, create_fence_op(node, PCMK_ACTION_REBOOT));
    }

    return operations;
}

static crm_exit_t
determine_exit_code(GList *operations)
{
    gboolean any_failed = FALSE;
    GList *iter;

    for (iter = operations; iter != NULL; iter = iter->next) {
        struct fence_op *op = (struct fence_op *) iter->data;
        if (op->call_id < 0) {
            any_failed = TRUE;
            break;
        }
    }

    return any_failed ? CRM_EX_ERROR : CRM_EX_OK;
}

static GOptionContext *
build_arg_context(pcmk__common_args_t *args, GOptionGroup **group)
{
    GOptionContext *context = NULL;

    context = pcmk__build_arg_context(args, NULL, group, NULL);
    pcmk__add_main_args(context, main_entries);
    return context;
}

int
main(int argc, char **argv)
{
    stonith_t *st = NULL;
    GList *operations = NULL;
    GList *iter;
    int rc;
    crm_exit_t exit_code = CRM_EX_OK;
    GError *error = NULL;

    pcmk__common_args_t *args = pcmk__new_common_args(SUMMARY);
    gchar **processed_args = pcmk__cmdline_preproc(argv, "BFtTUy");
    GOptionContext *context = build_arg_context(args, NULL);

    // Parse command line arguments
    if (!g_option_context_parse_strv(context, &processed_args, &error)) {
        exit_code = CRM_EX_USAGE;
        goto done;
    }

    // Initialize logging
    pcmk__cli_init_logging("stonith_async", args->verbosity);

    for (int i = 0; i < args->verbosity; i++) {
        crm_bump_log_level(argc, argv);
    }

    // Register STONITH messages
    stonith__register_messages(pcmk__output_new(&args->output, "text", "-",
                                                argv));

    // Check if we have any operations to perform
    if (options.fence_list == NULL && options.unfence_list == NULL &&
        options.reboot_list == NULL) {
        fprintf(stderr, "Error: No fence operations specified\n");
        fprintf(stderr, "Use --fence, --unfence, or --reboot to specify target nodes\n");
        exit_code = CRM_EX_USAGE;
        goto done;
    }

    // Build operation list
    operations = build_operation_list();

    // Create stonith API object
    st = stonith__api_new();
    if (st == NULL) {
        fprintf(stderr, "Error: Could not create stonith API object\n");
        exit_code = CRM_EX_ERROR;
        goto done;
    }

    // Connect to pacemaker-fenced with retry
    rc = stonith__api_connect_retry(st, "stonith_async", 10);
    if (rc != pcmk_rc_ok) {
        fprintf(stderr, "Error: Could not connect to pacemaker-fenced: %s\n",
                pcmk_rc_str(rc));
        exit_code = CRM_EX_DISCONNECT;
        goto done;
    }

    // Submit all fence operations
    for (iter = operations; iter != NULL; iter = iter->next) {
        struct fence_op *op = (struct fence_op *) iter->data;

        op->call_id = submit_fence_op(st, op);

        if (op->call_id < 0) {
            op->rc = pcmk_legacy2rc(op->call_id);
        }

        print_fence_result(op);
    }

    // Determine final exit code
    exit_code = determine_exit_code(operations);

    // Disconnect and cleanup
    if (st != NULL) {
        st->cmds->disconnect(st);
        stonith__api_free(st);
    }

done:
    // Cleanup
    g_strfreev(processed_args);
    pcmk__free_arg_context(context);

    if (operations != NULL) {
        g_list_free_full(operations, free_fence_op);
    }

    if (options.fence_list != NULL) {
        g_list_free_full(options.fence_list, free);
    }

    if (options.unfence_list != NULL) {
        g_list_free_full(options.unfence_list, free);
    }

    if (options.reboot_list != NULL) {
        g_list_free_full(options.reboot_list, free);
    }

    free(options.tag);

    pcmk__output_and_clear_error(&error, NULL);

    return crm_exit(exit_code);
}
