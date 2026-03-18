/*-------------------------------------------------------------------------
 *
 * pg_tde_rewind_sync.h
 *	  Sync pg_tde key data between source and target during rewind.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_TDE_REWIND_SYNC_H
#define PG_TDE_REWIND_SYNC_H

#include "libpq-fe.h"
#include "rewind_source.h"

extern void pg_tde_rewind_sync(rewind_source *source,
							   PGconn *conn,
							   const char *datadir_source,
							   const char *datadir_target);

/*
 * Returns the path to the directory containing the source's pg_tde key
 * files.  For local mode this is datadir_source/pg_tde; for remote mode
 * this is a temp directory populated during pg_tde_rewind_sync().
 *
 * Only valid after pg_tde_rewind_sync() has been called.
 */
extern const char *pg_tde_get_source_tde_dir(void);

#endif							/* PG_TDE_REWIND_SYNC_H */
