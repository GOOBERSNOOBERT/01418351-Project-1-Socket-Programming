#ifndef RSWRAPPER_H
#define RSWRAPPER_H

#include "rs.h"

void rs_init(void);

/* Returns a lazily-created, process-wide reed_solomon instance
   configured for DATA_SHARDS data shards / PARITY_SHARDS parity
   shards (see Config.h). Calls rs_init() internally if needed.
   Returns NULL on allocation failure. */
reed_solomon* rs_get(void);

#endif