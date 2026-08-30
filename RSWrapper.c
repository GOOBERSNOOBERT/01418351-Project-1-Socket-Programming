#include "include/RSWrapper.h"
#include "include/rs.h"
#include "include/Config.h"

#include <stddef.h>

void rs_init(void)
{
    static int initialized = 0;

    if (!initialized)
    {
        fec_init();
        initialized = 1;
    }
}

reed_solomon* rs_get(void)
{
    static reed_solomon* rs = NULL;

    if (!rs)
    {
        rs_init();

        rs = reed_solomon_new(
            DATA_SHARDS,
            PARITY_SHARDS);
    }

    return rs;
}