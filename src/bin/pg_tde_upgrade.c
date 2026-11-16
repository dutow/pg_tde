#include "postgres_fe.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef WIN32
#include <sys/wait.h>
#else
#include <process.h>
#endif

#include "common/file_perm.h"
#include "common/file_utils.h"
#include "common/logging.h"
#include "getopt_long.h"

static char tmpdir[MAXPGPATH] = "/tmp/pg_tde_upgradeXXXXXX";

/* Copied from src/bin/pg_upgrade/option.c */
static void
check_required_directory(char **dirpath, const char *envVarName, bool useCwd,
						 const char *cmdLineOption, const char *description,
						 bool missingOk)
{
	if (*dirpath == NULL || strlen(*dirpath) == 0)
	{
		const char *envVar;

		if ((envVar = getenv(envVarName)) && strlen(envVar))
			*dirpath = pg_strdup(envVar);
		else if (useCwd)
		{
			char		cwd[MAXPGPATH];

			if (!getcwd(cwd, MAXPGPATH))
				pg_fatal("could not determine current directory");
			*dirpath = pg_strdup(cwd);
		}
		else if (missingOk)
			return;
		else
			pg_fatal("You must identify the directory where the %s.\n"
					 "Please use the %s command-line option or the %s environment variable.",
					 description, cmdLineOption, envVarName);
	}

	/*
	 * Clean up the path, in particular trimming any trailing path separators,
	 * because we construct paths by appending to this path.
	 */
	canonicalize_path(*dirpath);
}

/* Copied from bin/pg_basebackup/pg_createsubscriber.c */
static char *
get_exec_path(const char *argv0, const char *progname)
{
	char	   *versionstr;
	char	   *exec_path;
	int			ret;

	versionstr = psprintf("%s (PostgreSQL) %s\n", progname, PG_VERSION);
	exec_path = pg_malloc(MAXPGPATH);
	ret = find_other_exec(argv0, progname, versionstr, exec_path);
	pg_free(versionstr);

	if (ret < 0)
	{
		char		full_path[MAXPGPATH];

		if (find_my_exec(argv0, full_path) < 0)
			strlcpy(full_path, progname, sizeof(full_path));

		if (ret == -1)
			pg_fatal("program \"%s\" is needed by %s but was not found in the same directory as \"%s\"",
					 progname, "pg_tde_upgrade", full_path);
		else
			pg_fatal("program \"%s\" was found by \"%s\" but was not the same version as %s",
					 progname, full_path, "pg_tde_upgrade");
	}

	pg_log_debug("%s path is:  %s", progname, exec_path);

	return exec_path;
}

static void
setup_bin(const char *src, const char *dest)
{
	DIR		   *dir;
	struct dirent *de;

	dir = opendir(src);
	if (dir == NULL)
		pg_fatal("could not open directory \"%s\": %m", src);

	while (errno = 0, (de = readdir(dir)))
	{
		const char *targetbin;
		char		srcfile[MAXPGPATH];
		char		destfile[MAXPGPATH];
		char	   *abssrcfile;

		if (strcmp(de->d_name, ".") == 0 ||
			strcmp(de->d_name, "..") == 0)
			continue;

		targetbin = strcmp(de->d_name, "pg_resetwal") == 0 ? "pg_tde_resetwal" : de->d_name;

		snprintf(srcfile, sizeof(srcfile), "%s/%s", src, targetbin);
		snprintf(destfile, sizeof(destfile), "%s/%s", dest, de->d_name);

		abssrcfile = make_absolute_path(srcfile);

		if (symlink(abssrcfile, destfile) < 0)
			pg_fatal("could not create symlink \"%s\": %m", destfile);

		pg_free(abssrcfile);
	}

	closedir(dir);
}

static void
copy_file(const char *src, const char *dst)
{
	int			src_fd;
	int			dst_fd;
	char		buf[8192];
	ssize_t		nread;

	if ((src_fd = open(src, O_RDONLY | PG_BINARY, 0)) < 0)
		pg_fatal("could not open file \"%s\": %m", src);

	if ((dst_fd = open(dst, O_WRONLY | O_CREAT | O_EXCL | PG_BINARY, pg_file_create_mode)) < 0)
		pg_fatal("could not create file \"%s\": %m", dst);

	while ((nread = read(src_fd, buf, sizeof(buf))) > 0)
	{
		errno = 0;
		if (write(dst_fd, buf, nread) != nread)
		{
			if (errno == 0)
				errno = ENOSPC;
			pg_fatal("could not write file \"%s\": %m", dst);
		}
	}

	if (nread < 0)
		pg_fatal("could not read file \"%s\": %m", src);

	if (close(dst_fd) != 0)
		pg_fatal("could not close file \"%s\": %m", dst);

	close(src_fd);
}

static void
copy_dir(const char *src, const char *dst)
{
	DIR		   *dir;
	struct dirent *de;

	if (mkdir(dst, pg_dir_create_mode) < 0)
		pg_fatal("could not create directory \"%s\": %m", dst);

	dir = opendir(src);
	if (dir == NULL)
		pg_fatal("could not open directory \"%s\": %m", src);

	while (errno = 0, (de = readdir(dir)))
	{
		char		srcfile[MAXPGPATH];
		char		dstfile[MAXPGPATH];
		struct stat st;

		if (strcmp(de->d_name, ".") == 0 ||
			strcmp(de->d_name, "..") == 0)
			continue;

		snprintf(srcfile, sizeof(srcfile), "%s/%s", src, de->d_name);
		snprintf(dstfile, sizeof(dstfile), "%s/%s", dst, de->d_name);

		if (stat(srcfile, &st) < 0)
			pg_fatal("could not stat file \"%s\": %m", srcfile);

		if (S_ISDIR(st.st_mode))
			copy_dir(srcfile, dstfile);
		else if (S_ISREG(st.st_mode))
			copy_file(srcfile, dstfile);
		else
			pg_fatal("unexpected file type for \"%s\"", srcfile);
	}

	if (errno)
		pg_fatal("could not read directory \"%s\": %m", src);

	closedir(dir);
}

static void
copy_pg_tde(const char *old_pgdata, const char *new_pgdata)
{
	char		old_pg_tde[MAXPGPATH];
	char		new_pg_tde[MAXPGPATH];
	char		tmp_pg_tde[MAXPGPATH];
	struct stat statBuf;

	snprintf(old_pg_tde, sizeof(old_pg_tde), "%s/pg_tde", old_pgdata);
	snprintf(new_pg_tde, sizeof(new_pg_tde), "%s/pg_tde", new_pgdata);
	snprintf(tmp_pg_tde, sizeof(tmp_pg_tde), "%s/pg_tde.tmp", new_pgdata);

	if (stat(old_pg_tde, &statBuf) < 0)
	{
		if (errno == ENOENT)
			return;
		else
			pg_fatal("could not stat directory \"%s\": %m", old_pg_tde);
	}

	if (stat(tmp_pg_tde, &statBuf) == 0)
	{
		if (!rmtree(tmp_pg_tde, true))
			pg_fatal("could not delete directory \"%s\"", tmp_pg_tde);
	}
	else if (errno != ENOENT)
		pg_fatal("could not stat directory \"%s\": %m", tmp_pg_tde);

	/* Keep the old destination until we have a complete copy */
	copy_dir(old_pg_tde, tmp_pg_tde);

	if (stat(new_pg_tde, &statBuf) == 0)
	{
		if (!rmtree(new_pg_tde, true))
			pg_fatal("could not delete directory \"%s\"", new_pg_tde);
	}
	else if (errno != ENOENT)
		pg_fatal("could not stat directory \"%s\": %m", new_pg_tde);

	if (rename(tmp_pg_tde, new_pg_tde) < 0)
		pg_fatal("could not rename directory \"%s\" to \"%s\": %m", tmp_pg_tde, new_pg_tde);
}

static void
cleanup_tmpdir(void)
{
	rmtree(tmpdir, true);
}

/* Clean up temporary files and dirs on SIGINT/SIGTERM */
static void
trapsig(int signum)
{
	cleanup_tmpdir();
	signal(signum, SIG_DFL);
	kill(getpid(), signum);
}

static void
usage(const char *progname)
{
	printf(_("%s wraps pg_upgrade to support upgrade with pg_tde\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  See pg_upgrade\n"));
}

int
main(int argc, char *argv[])
{
	static struct option long_options[] = {
		{"old-datadir", required_argument, NULL, 'd'},
		{"new-datadir", required_argument, NULL, 'D'},
		{"new-bindir", required_argument, NULL, 'B'},
		{NULL, 0, NULL, 0}
	};

	const char *progname;
	int			c;
	int			option_index;
	char	   *new_bindir = NULL;
	char	   *old_pgdata = NULL;
	char	   *new_pgdata = NULL;
	char	   *pg_upgrade_path;
	char	  **pg_upgrade_argv;
#ifndef WIN32
	pid_t		child;
#endif
	int			exitstatus;

	/* Copy argv before getopt_long() modifies it. */
	pg_upgrade_argv = pg_malloc_array(char *, argc + 3);
	for (int i = 1; i < argc; i++)
		pg_upgrade_argv[i] = argv[i];

	pg_logging_init(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_tde_upgrade"));
	progname = get_progname(argv[0]);

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage(progname);
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pg_tde_upgrade (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	/* Silence errors, pg_upgrade will verify the arguments */
	opterr = 0;

	while ((c = getopt_long(argc, argv, "B:d:D:", long_options, &option_index)) != -1)
	{
		switch (c)
		{
			case 'B':
				new_bindir = optarg;
				break;
			case 'd':
				old_pgdata = optarg;
				break;
			case 'D':
				new_pgdata = optarg;
				break;
		}
	}

	check_required_directory(&new_bindir, "PGBINNEW", false,
							 "-B", _("new cluster binaries reside"), true);
	check_required_directory(&old_pgdata, "PGDATAOLD", false,
							 "-d", _("old cluster data resides"), false);
	check_required_directory(&new_pgdata, "PGDATANEW", false,
							 "-D", _("new cluster data resides"), false);

	if (new_bindir == NULL)
	{
		char		exec_path[MAXPGPATH];

		if (find_my_exec(argv[0], exec_path) < 0)
			pg_fatal("%s: could not find own program executable", argv[0]);
		/* Trim off program name and keep just path */
		*last_dir_separator(exec_path) = '\0';
		canonicalize_path(exec_path);
		new_bindir = pg_strdup(exec_path);
	}

	pg_upgrade_path = get_exec_path(argv[0], "pg_upgrade");

	if (mkdtemp(tmpdir) == NULL)
		pg_fatal("could not create temporary directory \"%s\": %m", tmpdir);

	setup_bin(new_bindir, tmpdir);
	copy_pg_tde(old_pgdata, new_pgdata);

	/* We override any possible -B/--new-bindir by adding it at the end */
	pg_upgrade_argv[0] = pg_upgrade_path;
	pg_upgrade_argv[argc + 0] = "-B";
	pg_upgrade_argv[argc + 1] = tmpdir;
	pg_upgrade_argv[argc + 2] = NULL;

#ifndef WIN32
	child = fork();
	if (child == 0)
	{
		if (execv(pg_upgrade_path, pg_upgrade_argv) < 0)
			pg_fatal("could not run pg_upgrade executable: %m");
	}
	else if (child < 0)
		pg_fatal("could not spawn child process: %m");

	signal(SIGTERM, trapsig);
	signal(SIGINT, trapsig);
	atexit(cleanup_tmpdir);

	if (waitpid(child, &exitstatus, 0) < 0)
		pg_fatal("could not wait for child process: %m");

	if (WIFEXITED(exitstatus))
		exit(WEXITSTATUS(exitstatus));
	else if (WIFSIGNALED(exitstatus))
		pg_fatal("pg_upgrade (PID %d) was terminated by signal %d: %s",
				 child, WTERMSIG(exitstatus), pg_strsignal(WTERMSIG(exitstatus)));
#else
	signal(SIGTERM, trapsig);
	signal(SIGINT, trapsig);
	atexit(cleanup_tmpdir);

	exitstatus = (int) _spawnv(_P_WAIT, pg_upgrade_path, pg_upgrade_argv);
	if (exitstatus < 0)
		pg_fatal("could not run pg_upgrade executable: %m");
	exit(exitstatus);
#endif
}
