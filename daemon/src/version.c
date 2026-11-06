/*
 * version.c: version string helper. See version.h.
 */
#include "version.h"

const char *av_version_string(void)
{
    return "arkivectord " AV_DAEMON_VERSION " (api " AV_API_VERSION ", pgvector " AV_PGVECTOR_TAG ")";
}
