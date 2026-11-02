#ifndef AMATRIX_CANONJSON_H
#define AMATRIX_CANONJSON_H

#include "cJSON.h"

/* Serialise as Matrix canonical JSON. Caller frees. NULL on error. */
char *canonical_json(const cJSON *v);

/* Canonical JSON of obj without its "signatures" and "unsigned" members,
 * i.e. the exact bytes that get signed. Caller frees. */
char *canonical_json_for_signing(const cJSON *obj);

#endif
