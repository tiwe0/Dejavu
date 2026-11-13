#ifndef DEJAVU_JSON_H
#define DEJAVU_JSON_H

#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* Internal printing support. cJSON's tolerant 15-digit formatting can change
 * even exactly representable integers. Keep local trees intact while printing
 * safe integral numbers as exact decimal tokens. Trees must not be shared with
 * another thread while this function runs. The returned string has cJSON's
 * usual ownership; the caller releases it with its normal JSON allocator. */
struct dejavu_json_number_restore {
    cJSON *node;
    int type;
    char *valuestring;
    char decimal[32];
    struct dejavu_json_number_restore *next;
};

static inline int dejavu_json_prepare_numbers(cJSON *node,
                                              struct dejavu_json_number_restore **restores) {
    cJSON *child;
    if (cJSON_IsNumber(node) && isfinite(node->valuedouble) &&
        fabs(node->valuedouble) <= 9007199254740991.0 &&
        trunc(node->valuedouble) == node->valuedouble) {
        struct dejavu_json_number_restore *restore =
            (struct dejavu_json_number_restore *)cJSON_malloc(sizeof(*restore));
        if (!restore)
            return 0;
        restore->node = node;
        restore->type = node->type;
        restore->valuestring = node->valuestring;
        snprintf(restore->decimal, sizeof(restore->decimal), "%.0f", node->valuedouble);
        restore->next = *restores;
        *restores = restore;
        node->type = (node->type & ~0xff) | cJSON_Raw;
        node->valuestring = restore->decimal;
    }
    for (child = node ? node->child : NULL; child; child = child->next)
        if (!dejavu_json_prepare_numbers(child, restores))
            return 0;
    return 1;
}

static inline char *dejavu_json_print(cJSON *root) {
    struct dejavu_json_number_restore *restores = NULL;
    char *output = NULL;
    if (dejavu_json_prepare_numbers(root, &restores))
        output = cJSON_PrintUnformatted(root);
    while (restores) {
        struct dejavu_json_number_restore *restore = restores;
        restores = restore->next;
        restore->node->type = restore->type;
        restore->node->valuestring = restore->valuestring;
        cJSON_free(restore);
    }
    return output;
}

#endif
