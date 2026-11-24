/*
 * clifm_launch MODE INPUT — run the real clifm binary on one fuzz input, with a fresh config tree.
 *
 * clifm reads its startup files (config, bookmarks, keybindings, selection box, jump database,
 * actions, remotes, color schemes) from a per-user config directory that it creates and fills
 * with defaults on first run, and it rewrites some of them (jump db, dirhist) on exit. So that
 * each input's result depends only on that input, this launcher wipes a scratch directory under
 * $TMPDIR (default /tmp), stages INPUT at the file MODE names, points HOME and clifm's -D at the
 * scratch directory and calls clifm's own main() in this process: build.sh compiles src/main.c
 * with -Dmain=clifm_main and links it, with the rest of clifm, into this sanitized and
 * SanCov-instrumented binary. (An exec() of a separate clifm binary would hide clifm's coverage
 * from Mayhem, which traces the target image only: those runs recorded 0-57 edges.)
 * stdin is /dev/null, never a tty: clifm then runs every startup loader
 * and leaves through handle_stdin() without entering the interactive prompt.
 * Mode `stdin` instead puts INPUT itself on stdin: handle_stdin() parses it as a newline-separated
 * file list (`ls | clifm`) and links each entry into a virtual directory under clifm's tmp dir.
 * Mode `cmd` instead gives clifm a pty on stdin and runs one allow-listed command with --cmd.
 * CLIFM_TMPDIR points clifm's own tmp dir (selection box, virtual dirs, temp files) into the same
 * wiped scratch tree, so nothing an input writes there reaches the next input.
 *
 * No timers, and the launcher itself never forks or execs: Mayhem owns timeouts and sees clifm's
 * own crash. Pre-creating clifm's trash dirs keeps clifm's startup from running mkdir(1), so no
 * mode starts a child process except `stdin`: there clifm runs the fixed `mkdir -p` / `rm -r` of
 * its virtual dir, whose arguments are clifm's own paths, not chosen by the input.
 */
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef CLIFM_BIN
#define CLIFM_BIN "/mayhem/mayhem/out/clifm"
#endif

static void die(const char *what) { perror(what); exit(2); }

/* clifm's main(), renamed by build.sh (-Dmain=clifm_main on src/main.c only). */
int clifm_main(int argc, char *argv[]);

/* Call clifm's main() like the kernel would: argv strings are writable (clifm edits argv[0] in
 * place, args.c resolve_absolute_path) and argv[argc] is NULL. */
static int run_clifm(int argc, char **argv)
{
	for (int i = 0; i < argc; i++)
		if (!(argv[i] = strdup(argv[i]))) die("strdup");
	argv[argc] = NULL;
	return clifm_main(argc, argv);
}


static int rm_cb(const char *p, const struct stat *sb, int flag, struct FTW *ftw)
{
	(void)sb; (void)flag; (void)ftw;
	remove(p);
	return 0;
}

/* clifm leaves its virtual directory mode 0500: make every directory writable before the wipe */
static int chmod_cb(const char *p, const struct stat *sb, int flag, struct FTW *ftw)
{
	(void)ftw;
	if (flag == FTW_D || flag == FTW_DNR) chmod(p, (sb->st_mode & 07777) | S_IRWXU);
	return 0;
}

static void mkdir_p(const char *path)
{
	char buf[PATH_MAX];
	snprintf(buf, sizeof buf, "%s", path);
	for (char *p = buf + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			if (mkdir(buf, 0700) != 0 && errno != EEXIST) die(buf);
			*p = '/';
		}
	}
	if (mkdir(buf, 0700) != 0 && errno != EEXIST) die(buf);
}

static void copy_file(const char *src, const char *dst)
{
	int in = open(src, O_RDONLY), out;
	if (in < 0) die(src);
	out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (out < 0) die(dst);
	char buf[65536];
	ssize_t n;
	while ((n = read(in, buf, sizeof buf)) > 0)
		if (write(out, buf, (size_t)n) != n) die("write");
	if (n < 0) die("read");
	close(in);
	close(out);
}

/* A small fixture in DIR: plain files, a dotfile (hidden files are off by default), a subdir. */
static void make_fixture(const char *dir)
{
	static const char *const fixture[] = {"a.txt", "b.c", ".hidden", "sub/c.png", "sub/.d", NULL};
	char sub[PATH_MAX];
	snprintf(sub, sizeof sub, "%s/sub", dir);
	mkdir_p(sub);
	for (int i = 0; fixture[i]; i++) {
		char f[PATH_MAX];
		snprintf(f, sizeof f, "%s/%s", dir, fixture[i]);
		int fd = open(f, O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (fd < 0) die(f);
		if (write(fd, fixture[i], strlen(fixture[i])) < 0) die("write");
		close(fd);
	}
}

/*
 * MODE cmd: INPUT is one clifm command line, run with --cmd. clifm's command language can move,
 * trash and create files and run shell code, so (common.md: allow-list commands for an interpreter)
 * only read-only builtins run, and inputs with shell metacharacters are skipped before clifm sees
 * them, so clifm cannot split, substitute or redirect: the first word must be one of ALLOWED, or a
 * '/'-prefixed search pattern or path (search, or autocd into a directory). `-x` additionally blocks
 * external commands, and `-e` (--no-open-auto) plus refusing a '/'-word naming an existing
 * non-directory keep clifm from auto-opening a file through mime_open. -x does not stop
 * clifm's own child processes, so builtins that spawn one are left out: ac/ad (tar, atool, zstd),
 * bl (writes symlinks), and a search with the `-x`/`-r` file type (runs find(1)).
 */
static const char *const ALLOWED[] = {
	"p", "pp", "prop", "cd", "j", "jc", "jp", "jo", "st", "ft", "s", "sel", "sb", "ds", "mm", "hf",
	"hh", "bm", "ws", "pwd", "ver", "colors", "fc", "ff", "hidden", NULL
};

static int read_cmd(const char *path, char *buf, size_t sz)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) die(path);
	ssize_t n = read(fd, buf, sz - 1);
	close(fd);
	if (n <= 0) return -1;
	buf[n] = '\0';
	if (memchr(buf, '\0', (size_t)n)) return -1;
	while (n > 0 && buf[n - 1] == '\n') buf[--n] = '\0';
	if (n == 0 || strpbrk(buf, ";&|$`<>()\\\n!{}")) return -1;
	const char *w = buf;
	size_t wl = strcspn(w, " \t");
	if (*w == '/') {
		/* A search whose file type is -x or -r runs find(1) (search.c run_find): refuse any word
		 * starting with -x/-r, and quotes, which clifm would strip to form one. */
		if (strpbrk(w, "'\"")) return -1;
		/* An existing non-directory path is not a search: clifm would auto-open it
		 * (exec.c check_auto_second -> mime_open, which spawns an opener or archiver and is
		 * not blocked by -x). -e (--no-open-auto) below disables that too. */
		char first[PATH_MAX];
		struct stat st;
		if (wl >= sizeof first) return -1;
		memcpy(first, w, wl);
		first[wl] = '\0';
		if (stat(first, &st) == 0 && !S_ISDIR(st.st_mode)) return -1;
		for (const char *a = w; (a = strchr(a, '-')); a++)
			if ((a[-1] == ' ' || a[-1] == '\t') && (a[1] == 'x' || a[1] == 'r'))
				return -1;
		return 0;
	}
	for (int i = 0; ALLOWED[i]; i++)
		if (strlen(ALLOWED[i]) == wl && strncmp(ALLOWED[i], w, wl) == 0) {
			/* "bm" and "mm" have editing/opening subcommands: allow only their listing forms */
			if (strcmp(ALLOWED[i], "mm") == 0)
				return strncmp(w + wl, " info ", 6) == 0 ? 0 : -1;
			if (strcmp(ALLOWED[i], "bm") == 0)
				return w[wl] == '\0' || strncmp(w + wl, " list", 5) == 0 ? 0 : -1;
			if (strcmp(ALLOWED[i], "colors") == 0 || strcmp(ALLOWED[i], "ver") == 0 ||
			    strcmp(ALLOWED[i], "pwd") == 0)
				return w[wl] == '\0' ? 0 : -1;
			return 0;
		}
	return -1;
}

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s config|bookmarks|keybinds|selfile|colors|jump|actions|stdin|cmd INPUT\n", argv[0]);
		return 2;
	}
	const char *mode = argv[1], *input = argv[2];

	char inabs[PATH_MAX];
	if (!realpath(input, inabs)) die(input);

	const char *tmp = getenv("TMPDIR");
	if (!tmp || !*tmp) tmp = "/tmp";
	char root[PATH_MAX], cfg[PATH_MAX], prof[PATH_MAX], staged[PATH_MAX];
	snprintf(root, sizeof root, "%s/clifm-fuzz-%s", tmp, mode);
	nftw(root, chmod_cb, 16, FTW_PHYS);
	nftw(root, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
	snprintf(cfg, sizeof cfg, "%s/cfg", root);
	snprintf(prof, sizeof prof, "%s/profiles/default", cfg);
	mkdir_p(prof);
	char colors[PATH_MAX];
	snprintf(colors, sizeof colors, "%s/colors", cfg);
	mkdir_p(colors);
	char start[PATH_MAX];
	snprintf(start, sizeof start, "%s/start", root);
	mkdir_p(start);

	/* clifm creates ~/.local/share/Trash/{files,info} with a mkdir(1) child when they are
	 * missing (config.c create_trash_dirs): create them here so startup runs no child. */
	char trash[PATH_MAX];
	snprintf(trash, sizeof trash, "%s/.local/share/Trash/files", root);
	mkdir_p(trash);
	snprintf(trash, sizeof trash, "%s/.local/share/Trash/info", root);
	mkdir_p(trash);

	char ctmp[PATH_MAX];
	snprintf(ctmp, sizeof ctmp, "%s/tmp", root);
	mkdir_p(ctmp);

	if (setenv("HOME", root, 1) != 0 || setenv("CLIFM_TMPDIR", ctmp, 1) != 0) die("setenv");
	if (chdir(start) != 0) die(start);

	char *args[16];
	int n = 0;
	args[n++] = (char *)CLIFM_BIN;
	args[n++] = (char *)"-D";
	args[n++] = cfg;

	if (strcmp(mode, "config") == 0) {
		args[n++] = (char *)"-c"; args[n++] = inabs;
	} else if (strcmp(mode, "bookmarks") == 0) {
		args[n++] = (char *)"-b"; args[n++] = inabs;
	} else if (strcmp(mode, "keybinds") == 0) {
		args[n++] = (char *)"-k"; args[n++] = inabs;
	} else if (strcmp(mode, "selfile") == 0) {
		/* clifm rewrites the selection file on exit: give it a copy, not Mayhem's testcase */
		snprintf(staged, sizeof staged, "%s/sel.clifm", cfg);
		copy_file(inabs, staged);
		args[n++] = (char *)"--sel-file"; args[n++] = staged;
	} else if (strcmp(mode, "colors") == 0) {
		snprintf(staged, sizeof staged, "%s/fuzz.clifm", colors);
		copy_file(inabs, staged);
		args[n++] = (char *)"--color-scheme"; args[n++] = (char *)"fuzz";
	} else if (strcmp(mode, "jump") == 0 || strcmp(mode, "actions") == 0) {
		snprintf(staged, sizeof staged, "%s/%s.clifm", prof, mode);
		copy_file(inabs, staged);
	} else if (strcmp(mode, "stdin") == 0) {
		/* The file list is read from stdin, below; give its relative names something to match.
		 * After handle_stdin() clifm reads commands from stdout and would loop on EOF forever:
		 * --cmd pwd makes it run that one fixed builtin and exit instead. */
		make_fixture(start);
		args[n++] = (char *)"--no-color";
		args[n++] = (char *)"--cmd";
		args[n++] = (char *)"pwd";
	} else if (strcmp(mode, "cmd") == 0) {
		static char cmdline[4096];
		if (read_cmd(inabs, cmdline, sizeof cmdline) != 0)
			return 0; /* not an allowed read-only command: nothing to run */
		make_fixture(start);
		/* --cmd needs a terminal on stdin; give it a pty whose master stays open (inherited). */
		int m = posix_openpt(O_RDWR | O_NOCTTY);
		if (m < 0 || grantpt(m) != 0 || unlockpt(m) != 0) die("pty");
		const char *sname = ptsname(m);
		int sl = sname ? open(sname, O_RDWR | O_NOCTTY) : -1;
		if (sl < 0 || dup2(sl, STDIN_FILENO) < 0) die("pty slave");
		if (sl != STDIN_FILENO) close(sl);
		/* Answer any confirmation prompt with "n" so it cannot wait forever. */
		for (int i = 0; i < 16; i++)
			if (write(m, "n\n", 2) < 0) break;
		args[n++] = (char *)"-x";
		args[n++] = (char *)"-e"; /* --no-open-auto: a file path never reaches mime_open */
		args[n++] = (char *)"--no-color";
		args[n++] = (char *)"--cmd";
		args[n++] = cmdline;
		args[n++] = start;
		args[n] = NULL;
		return run_clifm(n, args);
	} else {
		fprintf(stderr, "unknown mode %s\n", mode);
		return 2;
	}
	args[n++] = start;
	args[n] = NULL;

	int fd = open(strcmp(mode, "stdin") == 0 ? inabs : "/dev/null", O_RDONLY);
	if (fd < 0 || dup2(fd, STDIN_FILENO) < 0) die("stdin");
	if (fd != STDIN_FILENO) close(fd);

	return run_clifm(n, args);
}
