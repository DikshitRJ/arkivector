/*
 * version.h: version strings of the daemon, the API contract and the copied upstreams.
 */
#ifndef AV_VERSION_H
#define AV_VERSION_H

#ifndef AV_DAEMON_VERSION /* release builds pass -DAV_VERSION=<x.y.z> to CMake */
#define AV_DAEMON_VERSION "0.1.0-dev"
#endif
#define AV_API_VERSION  "1.1.0"     /* spec/VERSION */
#define AV_PGVECTOR_TAG "v0.8.7"    /* src/pgvector/PROVENANCE.md */
#define AV_POSTGRES_TAG "REL_17_11" /* src/pgcompat/STUBS.md */

/* Returns "arkivectord <version> (api <v>, pgvector <tag>)". Static storage. */
const char *av_version_string(void);

#endif /* AV_VERSION_H */
