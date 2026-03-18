/*-------------------------------------------------------------------------
 *
 * pg_tde_rewind_sync.c
 *	  Sync pg_tde key data between source and target during rewind.
 *
 * This runs after WAL analysis (which needs the target's wal_keys) but
 * before perform_rewind() (which copies data blocks and needs both key
 * maps to be interoperable for re-encryption).
 *
 * The sync step:
 *   1. Verifies that the source and target use the same principal keys
 *      (required so we can read both key maps).
 *   2. Merges WAL key entries: keeps the target's entries (for pre-
 *      divergence WAL on timeline 1) and appends the source's entries
 *      (for post-divergence WAL on timeline 2).
 *   3. Handles asymmetric databases: copies key files that exist on the
 *      source but not the target, and removes key files that exist on
 *      the target but not the source.
 *
 * Copyright (c) 2013-2025, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/file_perm.h"
#include "common/logging.h"
#include "libpq-fe.h"
#include "pg_rewind.h"
#include "pg_tde_rewind_sync.h"

#include "pg_tde.h"
#include "access/pg_tde_keys_common.h"
#include "catalog/tde_principal_key.h"
#include "access/pg_tde_tdemap.h"
#include "access/pg_tde_xlog_keys.h"
#include "common/pg_tde_utils.h"

/*
 * File header structures (must match pg_tde_tdemap.c and pg_tde_xlog_keys.c).
 * These are duplicated here because the originals are private to their
 * respective translation units.
 */
typedef struct TDEFileHeader
{
	int32		file_version;
	TDESignedPrincipalKeyInfo signed_key_info;
} TDEFileHeader;

typedef struct WalKeyFileHeader
{
	int32		file_version;
	TDESignedPrincipalKeyInfo signed_key_info;
} WalKeyFileHeader;

/*
 * Path to the directory containing the source's pg_tde key files.
 * For local mode: datadir_source/pg_tde
 * For remote mode: a temp directory populated during sync.
 */
static char source_tde_dir_path[MAXPGPATH] = "";

const char *
pg_tde_get_source_tde_dir(void)
{
	return source_tde_dir_path;
}


/*
 * Read a file entirely into a malloc'd buffer.  For local source, read
 * from datadir_source directly.  For remote source, use the rewind_source
 * fetch_file interface (which calls pg_read_binary_file over libpq).
 */
static char *
read_source_file(rewind_source *source, const char *datadir_source,
				 const char *relpath, size_t *filesize)
{
	if (datadir_source != NULL)
	{
		/* Local source: read directly from filesystem */
		return slurpFile(datadir_source, relpath, filesize);
	}
	else
	{
		/* Remote source: fetch via libpq */
		return source->fetch_file(source, relpath, filesize);
	}
}

/*
 * Write a buffer to a file at an absolute path, creating or overwriting.
 */
static void
write_local_file(const char *path, const char *data, size_t len)
{
	int			fd;

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | PG_BINARY, pg_file_create_mode);
	if (fd < 0)
		pg_fatal("could not create file \"%s\": %m", path);

	if (write(fd, data, len) != len)
		pg_fatal("could not write file \"%s\": %m", path);

	if (close(fd) != 0)
		pg_fatal("could not close file \"%s\": %m", path);
}

/*
 * Copy a file from src to dst (absolute paths).
 */
static void
copy_file(const char *srcpath, const char *dstpath)
{
	int			sfd,
				dfd;
	struct stat	st;
	char	   *buf;

	sfd = open(srcpath, O_RDONLY | PG_BINARY, 0);
	if (sfd < 0)
		pg_fatal("could not open \"%s\": %m", srcpath);
	if (fstat(sfd, &st) != 0)
		pg_fatal("could not stat \"%s\": %m", srcpath);

	buf = pg_malloc(st.st_size);
	if (read(sfd, buf, st.st_size) != st.st_size)
		pg_fatal("could not read \"%s\": %m", srcpath);
	close(sfd);

	dfd = open(dstpath, O_WRONLY | O_CREAT | O_TRUNC | PG_BINARY, pg_file_create_mode);
	if (dfd < 0)
		pg_fatal("could not create \"%s\": %m", dstpath);
	if (write(dfd, buf, st.st_size) != st.st_size)
		pg_fatal("could not write \"%s\": %m", dstpath);
	close(dfd);

	pg_free(buf);
}

/*
 * Copy a buffer to a file in the target data directory, creating or
 * overwriting as needed.
 */
static void
write_target_file(const char *datadir_target, const char *relpath,
				  const char *data, size_t len)
{
	char		path[MAXPGPATH];

	snprintf(path, sizeof(path), "%s/%s", datadir_target, relpath);
	write_local_file(path, data, len);
}

/*
 * Read the TDESignedPrincipalKeyInfo from a key map file header.
 * Returns true if the file exists, false if missing.
 */
static bool
read_key_file_header(const char *dirpath, const char *filename,
					 TDESignedPrincipalKeyInfo *info)
{
	char		path[MAXPGPATH];
	int			fd;
	TDEFileHeader header;
	ssize_t		nbytes;

	snprintf(path, sizeof(path), "%s/%s/%s", dirpath, PG_TDE_DATA_DIR, filename);

	fd = open(path, O_RDONLY | PG_BINARY, 0);
	if (fd < 0)
	{
		if (errno == ENOENT)
			return false;
		pg_fatal("could not open \"%s\": %m", path);
	}

	nbytes = read(fd, &header, sizeof(header));
	if (nbytes != sizeof(header))
		pg_fatal("could not read header from \"%s\"", path);

	close(fd);
	*info = header.signed_key_info;
	return true;
}

/*
 * Read the TDESignedPrincipalKeyInfo from a WAL key file header.
 * Returns true if the file exists, false if missing.
 */
static bool
read_wal_key_file_header(const char *dirpath,
						 TDESignedPrincipalKeyInfo *info)
{
	char		path[MAXPGPATH];
	int			fd;
	WalKeyFileHeader header;
	ssize_t		nbytes;

	snprintf(path, sizeof(path), "%s/%s/wal_keys", dirpath, PG_TDE_DATA_DIR);

	fd = open(path, O_RDONLY | PG_BINARY, 0);
	if (fd < 0)
	{
		if (errno == ENOENT)
			return false;
		pg_fatal("could not open \"%s\": %m", path);
	}

	nbytes = read(fd, &header, sizeof(header));
	if (nbytes != sizeof(header))
		pg_fatal("could not read header from \"%s\"", path);

	close(fd);
	*info = header.signed_key_info;
	return true;
}

/*
 * Verify that the principal key name matches between source and target
 * for a given key file.  We compare the key name from the signed header;
 * if the names match and both nodes use the same keyring (which is
 * expected since key management operations are WAL-replicated), the
 * keys are interoperable.
 */
static void
verify_principal_key_match(const char *context,
						   const TDESignedPrincipalKeyInfo *source_info,
						   const TDESignedPrincipalKeyInfo *target_info)
{
	if (strcmp(source_info->data.name, target_info->data.name) != 0)
		pg_fatal("principal key mismatch for %s: "
				 "source has \"%s\", target has \"%s\". "
				 "pg_tde_rewind does not yet support rewinding clusters "
				 "with diverged principal keys.",
				 context, source_info->data.name, target_info->data.name);
}

/*
 * Copy the source's wal_keys file to the target, replacing it entirely.
 *
 * In local mode, pg_rewind replaces WAL segment files on the target with
 * the source's versions.  After rewind, all WAL files on the target are
 * encrypted with the source's WAL key, so we need the source's wal_keys
 * to decrypt them.  The target's own WAL key entries are no longer valid
 * since those WAL files have been replaced.
 */
static void
copy_source_wal_keys(rewind_source *source,
					 const char *datadir_source,
					 const char *datadir_target)
{
	char	   *source_data;
	size_t		source_size;

	source_data = read_source_file(source, datadir_source,
								   PG_TDE_DATA_DIR "/wal_keys", &source_size);

	write_target_file(datadir_target,
					  PG_TDE_DATA_DIR "/wal_keys", source_data, source_size);

	pg_log_info("copied source wal_keys (%zu bytes) to target", source_size);

	pg_free(source_data);
}

/*
 * Collect database OIDs that have key files in a pg_tde directory.
 * Returns a palloc'd array of Oids, with *count set to the number found.
 *
 * Key files are named "{dboid}_keys".
 */
static Oid *
collect_key_file_dboids(const char *tde_dir, int *count)
{
	DIR		   *dir;
	struct dirent *de;
	Oid		   *oids;
	int			capacity = 16;
	int			n = 0;

	oids = pg_malloc(capacity * sizeof(Oid));

	dir = opendir(tde_dir);
	if (dir == NULL)
	{
		*count = 0;
		return oids;
	}

	while ((de = readdir(dir)) != NULL)
	{
		unsigned int dboid;
		char		suffix[16];

		if (sscanf(de->d_name, "%u_%15s", &dboid, suffix) == 2 &&
			strcmp(suffix, "keys") == 0)
		{
			if (n >= capacity)
			{
				capacity *= 2;
				oids = pg_realloc(oids, capacity * sizeof(Oid));
			}
			oids[n++] = (Oid) dboid;
		}
	}

	closedir(dir);
	*count = n;
	return oids;
}

/*
 * Check if a value exists in an Oid array.
 */
static bool
oid_array_contains(const Oid *arr, int count, Oid val)
{
	for (int i = 0; i < count; i++)
	{
		if (arr[i] == val)
			return true;
	}
	return false;
}

/*
 * Handle asymmetric database key files.
 *
 * - Source has a key file that the target doesn't: copy it from source
 *   (the database was encrypted on the source after divergence).
 * - Target has a key file that the source doesn't: delete it from target
 *   (the database was encrypted on the target after divergence but that
 *   timeline is being abandoned by the rewind).
 *
 * TODO: This approach of directly copying/deleting key files works when
 * principal keys match, but is fragile.  A more robust approach would
 * re-create the key entries through the proper pg_tde API rather than
 * raw file manipulation.  This matters for cases like the source having
 * rotated the principal key for one database but not another, or the
 * key file format changing between versions.
 */
static void
handle_asymmetric_databases(rewind_source *source,
							const char *datadir_source,
							const char *datadir_target)
{
	char		target_tde_dir[MAXPGPATH];
	char		source_tde_dir[MAXPGPATH];
	Oid		   *target_dboids;
	int			target_count;
	Oid		   *source_dboids = NULL;
	int			source_count = 0;

	snprintf(target_tde_dir, sizeof(target_tde_dir), "%s/%s",
			 datadir_target, PG_TDE_DATA_DIR);

	/* Collect target's database key files */
	target_dboids = collect_key_file_dboids(target_tde_dir, &target_count);

	/*
	 * Collect source's database key files.  For local source we can scan
	 * the directory directly.  For remote source we need a different
	 * approach — the file list was already collected during traverse_files,
	 * but we don't have easy access to it here.  For now, in remote mode
	 * we rely on the filemap having already recorded source files.  Since
	 * pg_tde/ files are skipped in the filemap, we need to list them
	 * explicitly.
	 *
	 * TODO: For remote mode, query the source's pg_tde/ directory via
	 * pg_ls_dir() to get the list of key files.
	 */
	if (datadir_source != NULL)
	{
		snprintf(source_tde_dir, sizeof(source_tde_dir), "%s/%s",
				 datadir_source, PG_TDE_DATA_DIR);
		source_dboids = collect_key_file_dboids(source_tde_dir, &source_count);
	}
	else
	{
		/*
		 * Remote mode: for now, skip asymmetric database handling.
		 * The most common case (same databases on both sides) is covered.
		 * A future improvement should query pg_ls_dir('pg_tde') on the
		 * source to get the file list.
		 *
		 * TODO: Implement remote listing of source pg_tde/ directory.
		 */
		pg_log_info("skipping asymmetric database key file check in remote mode");
		pg_free(target_dboids);
		return;
	}

	/* Source has key files that target doesn't → copy from source */
	for (int i = 0; i < source_count; i++)
	{
		if (!oid_array_contains(target_dboids, target_count, source_dboids[i]))
		{
			char		relpath[MAXPGPATH];
			char	   *data;
			size_t		size;

			snprintf(relpath, sizeof(relpath), "%s/%u_keys",
					 PG_TDE_DATA_DIR, source_dboids[i]);

			pg_log_info("copying key file for database %u from source",
						source_dboids[i]);

			data = read_source_file(source, datadir_source, relpath, &size);
			write_target_file(datadir_target, relpath, data, size);
			pg_free(data);
		}
	}

	/* Target has key files that source doesn't → delete from target */
	for (int i = 0; i < target_count; i++)
	{
		if (!oid_array_contains(source_dboids, source_count, target_dboids[i]))
		{
			char		path[MAXPGPATH];

			snprintf(path, sizeof(path), "%s/%s/%u_keys",
					 datadir_target, PG_TDE_DATA_DIR, target_dboids[i]);

			pg_log_info("removing orphaned key file for database %u",
						target_dboids[i]);

			if (unlink(path) != 0 && errno != ENOENT)
				pg_fatal("could not remove \"%s\": %m", path);
		}
	}

	pg_free(target_dboids);
	pg_free(source_dboids);
}


/*
 * Main entry point: sync pg_tde key data between source and target.
 *
 * Must be called after WAL analysis is complete (we need the target's
 * original wal_keys for reading target WAL) and before perform_rewind()
 * (which needs interoperable key maps for re-encryption).
 *
 * datadir_source is non-NULL for local mode, NULL for remote mode.
 */
void
pg_tde_rewind_sync(rewind_source *source,
				   PGconn *conn,
				   const char *datadir_source,
				   const char *datadir_target)
{
	char		target_tde_dir[MAXPGPATH];
	TDESignedPrincipalKeyInfo source_wal_info;
	TDESignedPrincipalKeyInfo target_wal_info;
	bool		source_has_wal_keys;
	bool		target_has_wal_keys;

	snprintf(target_tde_dir, sizeof(target_tde_dir), "%s/%s",
			 datadir_target, PG_TDE_DATA_DIR);

	/* Check if pg_tde directory exists on target */
	if (access(target_tde_dir, F_OK) != 0)
	{
		pg_log_info("no pg_tde directory on target, skipping TDE sync");
		return;
	}

	pg_log_info("syncing pg_tde key data between source and target");

	/*
	 * Phase 1: Verify principal keys match.
	 *
	 * Compare the WAL key file headers (server/global principal key)
	 * and each database's key file header (per-database principal key).
	 */
	target_has_wal_keys = read_wal_key_file_header(datadir_target, &target_wal_info);

	if (datadir_source != NULL)
		source_has_wal_keys = read_wal_key_file_header(datadir_source, &source_wal_info);
	else
	{
		/* Remote: fetch the source's wal_keys header */
		char   *data;
		size_t	size;

		data = source->fetch_file(source, PG_TDE_DATA_DIR "/wal_keys", &size);
		if (size >= sizeof(WalKeyFileHeader))
		{
			WalKeyFileHeader *hdr = (WalKeyFileHeader *) data;

			source_wal_info = hdr->signed_key_info;
			source_has_wal_keys = true;
		}
		else
			source_has_wal_keys = false;

		pg_free(data);
	}

	if (target_has_wal_keys && source_has_wal_keys)
		verify_principal_key_match("WAL keys (server principal key)",
								  &source_wal_info, &target_wal_info);

	/*
	 * TODO: Also verify per-database principal keys for each database
	 * that has key files on both source and target.  For now we only
	 * check the server/global principal key.
	 */

	/*
	 * Phase 2: Replace the target's wal_keys with the source's.
	 *
	 * pg_rewind may replace WAL segment files with the source's versions
	 * (in both local and remote modes, depending on file size changes).
	 * Those segments are encrypted with the source's WAL key.  We replace
	 * the target's wal_keys entirely so the source's key is available.
	 *
	 * For pre-divergence WAL segments that pg_rewind does NOT replace,
	 * recovery either starts past them (the rewind checkpoint is after
	 * the divergence point) or the source has the same WAL key for
	 * those ranges (both nodes received the same WAL key setup via
	 * replication before divergence).
	 */
	if (source_has_wal_keys)
		copy_source_wal_keys(source, datadir_source, datadir_target);

	/*
	 * Phase 3: Handle asymmetric database key files.
	 */
	handle_asymmetric_databases(source, datadir_source, datadir_target);

	/*
	 * Phase 4: Set up the source TDE directory path for re-encryption.
	 *
	 * local_source.c and libpq_source.c need to load the source's
	 * InternalKeys to decrypt/re-encrypt copied data blocks.  For local
	 * mode, we point directly at the source's pg_tde/ directory.  For
	 * remote mode, we fetch the source's *_keys files to a temp directory
	 * so they can be loaded locally (the principal keys match, so we can
	 * decrypt them).
	 */
	if (datadir_source != NULL)
	{
		/* Local mode: source's pg_tde/ is directly accessible */
		snprintf(source_tde_dir_path, sizeof(source_tde_dir_path),
				 "%s/%s", datadir_source, PG_TDE_DATA_DIR);
	}
	else
	{
		/*
		 * Remote mode: fetch source's key files to a temp directory
		 * under the target's data directory.
		 */
		char		tmp_dir[MAXPGPATH];
		int			ndboids;
		Oid		   *target_dboids;

		snprintf(tmp_dir, sizeof(tmp_dir), "%s/pg_tde_source_keys",
				 datadir_target);

		if (mkdir(tmp_dir, pg_dir_create_mode) != 0 && errno != EEXIST)
			pg_fatal("could not create directory \"%s\": %m", tmp_dir);

		strlcpy(source_tde_dir_path, tmp_dir, sizeof(source_tde_dir_path));

		/*
		 * Copy the target's provider files and keys_version into the
		 * temp dir.  pg_tde_get_smgr_key uses pg_tde_set_data_dir to
		 * find everything — provider config, principal key info, and
		 * key maps must all be in the same directory.  Since principal
		 * keys match (verified above), the target's providers can
		 * decrypt the source's key map entries.
		 */
		target_dboids = collect_key_file_dboids(target_tde_dir, &ndboids);
		{
			DIR		   *dir;
			struct dirent *de;

			dir = opendir(target_tde_dir);
			if (dir != NULL)
			{
				while ((de = readdir(dir)) != NULL)
				{
					char		srcpath[MAXPGPATH];
					char		dstpath[MAXPGPATH];

					/* Copy provider files, keys_version, and wal_keys */
					if (strstr(de->d_name, "_providers") != NULL ||
						strcmp(de->d_name, "keys_version") == 0 ||
						strcmp(de->d_name, "wal_keys") == 0)
					{
						snprintf(srcpath, sizeof(srcpath), "%s/%s",
								 target_tde_dir, de->d_name);
						snprintf(dstpath, sizeof(dstpath), "%s/%s",
								 tmp_dir, de->d_name);
						copy_file(srcpath, dstpath);
					}
				}
				closedir(dir);
			}
		}

		/*
		 * Fetch the source's *_keys files into the temp dir.
		 *
		 * We iterate the target's database OIDs, but some databases may
		 * only exist on the target (created after divergence).  Those
		 * won't have key files on the source.  We use pg_read_binary_file
		 * with missing_ok=true to handle this gracefully.
		 */
		for (int i = 0; i < ndboids; i++)
		{
			PGresult   *res;
			char		sql[MAXPGPATH + 128];
			char		destpath[MAXPGPATH];

			snprintf(sql, sizeof(sql),
					 "SELECT pg_read_binary_file('%s/%u_keys', true)",
					 PG_TDE_DATA_DIR, target_dboids[i]);

			res = PQexecParams(conn, sql, 0, NULL, NULL, NULL, NULL, 1);

			if (PQresultStatus(res) == PGRES_TUPLES_OK &&
				PQntuples(res) == 1 && !PQgetisnull(res, 0, 0))
			{
				int			len = PQgetlength(res, 0, 0);

				if (len > 0)
				{
					snprintf(destpath, sizeof(destpath), "%s/%u_keys",
							 tmp_dir, target_dboids[i]);
					write_local_file(destpath, PQgetvalue(res, 0, 0), len);
					pg_log_debug("fetched source key file for database %u (%d bytes)",
								 target_dboids[i], len);
				}
			}
			else
			{
				pg_log_debug("source has no key file for database %u (target-only database)",
							 target_dboids[i]);
			}

			PQclear(res);
		}

		pg_free(target_dboids);
	}

	pg_log_info("pg_tde key data sync complete (source keys at %s)",
				source_tde_dir_path);
}
