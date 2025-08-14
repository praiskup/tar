/* System-dependent calls for tar.

   Copyright 2003-2025 Free Software Foundation, Inc.

   This program is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the
   Free Software Foundation; either version 3, or (at your option) any later
   version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
   Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include <system.h>

#if HAVE_SYS_MTIO_H
# include <sys/ioctl.h>
# include <sys/mtio.h>
#endif

#include "common.h"
#include <priv-set.h>
#include <rmt.h>
#include <same-inode.h>
#include <signal.h>
#include <wordsplit.h>
#include <poll.h>
#include <parse-datetime.h>

bool dev_null_output;

static _Noreturn void
xexec (const char *cmd)
{
  char *argv[4];

  argv[0] = (char *) "/bin/sh";
  argv[1] = (char *) "-c";
  argv[2] = (char *) cmd;
  argv[3] = NULL;

  execv ("/bin/sh", argv);
  exec_fatal (cmd);
}

/* True if the archive is seekable via ioctl and MTIOCTOP,
   or if it is not known whether it is seekable.
   False if it is known to be not seekable.  */
static bool mtioseekable_archive;

bool
mtioseek (bool count_files, off_t count)
{
  if (mtioseekable_archive)
    {
#ifdef MTIOCTOP
      struct mtop operation;
      operation.mt_op = (count_files
			 ? (count < 0 ? MTBSF : MTFSF)
			 : (count < 0 ? MTBSR : MTFSR));
      if (! (count < 0
	     ? ckd_sub (&operation.mt_count, 0, count)
	     : ckd_add (&operation.mt_count, count, 0))
	  && (0 <= rmtioctl (archive, MTIOCTOP, &operation)
	      || (errno == EIO
		  && 0 <= rmtioctl (archive, MTIOCTOP, &operation))))
	return true;
#endif

      mtioseekable_archive = false;
    }
  return false;
}

#if !HAVE_WAITPID /* MingW, MSVC 14.  */

bool
sys_get_archive_stat (void)
{
  return 0;
}

bool
sys_file_is_archive (struct tar_stat_info *p)
{
  return false;
}

void
sys_detect_dev_null_output (void)
{
  static char const dev_null[] = "nul";

  dev_null_output = (strcmp (archive_name_array[0], dev_null) == 0
		     || (! _isrmt (archive)));
}

void
sys_wait_for_child (pid_t child_pid, bool eof)
{
}

void
sys_spawn_shell (void)
{
  spawnl (P_WAIT, getenv ("COMSPEC"), "-", 0);
}

/* stat() in djgpp's C library gives a constant number of 42 as the
   uid and gid of a file.  So, comparing an FTP'ed archive just after
   unpack would fail on MSDOS.  */

bool
sys_compare_uid (struct stat *a, struct stat *b)
{
  return true;
}

bool
sys_compare_gid (struct stat *a, struct stat *b)
{
  return true;
}

int
sys_truncate (int fd)
{
  return write (fd, "", 0);
}

idx_t
sys_write_archive_buffer (void)
{
  return full_write (archive, charptr (record_start), record_size);
}

/* Set ARCHIVE for writing, then compressing an archive.  */
void
sys_child_open_for_compress (void)
{
  paxfatal (0, _("Cannot use compressed or remote archives"));
}

/* Set ARCHIVE for uncompressing, then reading an archive.  */
void
sys_child_open_for_uncompress (void)
{
  paxfatal (0, _("Cannot use compressed or remote archives"));
}

bool
sys_exec_setmtime_script (const char *script_name,
			  int dirfd,
			  const char *file_name,
			  const char *fmt,
			  struct timespec *ts)
{
  paxfatal (0, _("--set-mtime-command not implemented on this platform"));
}
#else

bool
sys_get_archive_stat (void)
{
  bool remote = _isrmt (archive);
  mtioseekable_archive = true;
  if (!remote && 0 <= archive && fstat (archive, &archive_stat) == 0)
    {
      if (!S_ISCHR (archive_stat.st_mode))
	mtioseekable_archive = false;
      return true;
    }
  else
    {
      /* FIXME: This memset should not be needed.  It is present only
	 because other parts of tar may incorrectly access
	 archive_stat even if it's not the archive status.  */
      memset (&archive_stat, 0, sizeof archive_stat);

      return remote;
    }
}

bool
sys_file_is_archive (struct tar_stat_info *p)
{
  return (!dev_null_output && !_isrmt (archive)
	  && psame_inode (&p->stat, &archive_stat));
}

static char const dev_null[] = "/dev/null";

/* Detect if outputting to "/dev/null".  */
void
sys_detect_dev_null_output (void)
{
  static struct stat dev_null_stat;

  dev_null_output = (strcmp (archive_name_array[0], dev_null) == 0
		     || (! _isrmt (archive)
			 && S_ISCHR (archive_stat.st_mode)
			 && (dev_null_stat.st_ino != 0
			     || stat (dev_null, &dev_null_stat) == 0)
			 && psame_inode (&archive_stat, &dev_null_stat)));
}

void
sys_wait_for_child (pid_t child_pid, bool eof)
{
  if (child_pid)
    {
      int wait_status;

      while (waitpid (child_pid, &wait_status, 0) < 0)
	if (errno != EINTR)
	  {
	    waitpid_error (use_compress_program_option);
	    break;
	  }

      if (WIFSIGNALED (wait_status))
	{
	  int sig = WTERMSIG (wait_status);
	  if (!(!eof && sig == SIGPIPE))
	    paxfatal (0, _("Child died with signal %d"), sig);
	}
      else if (WEXITSTATUS (wait_status) != 0)
	paxfatal (0, _("Child returned status %d"),
		  WEXITSTATUS (wait_status));
    }
}

void
sys_spawn_shell (void)
{
  pid_t child;
  const char *shell = getenv ("SHELL");
  if (! shell)
    shell = "/bin/sh";
  child = xfork ();
  if (child == 0)
    {
      priv_set_restore_linkdir ();
      execlp (shell, "-sh", "-i", NULL);
      exec_fatal (shell);
    }
  else
    {
      int wait_status;
      while (waitpid (child, &wait_status, 0) < 0)
	if (errno != EINTR)
	  {
	    waitpid_error (shell);
	    break;
	  }
    }
}

bool
sys_compare_uid (struct stat *a, struct stat *b)
{
  return a->st_uid == b->st_uid;
}

bool
sys_compare_gid (struct stat *a, struct stat *b)
{
  return a->st_gid == b->st_gid;
}

int
sys_truncate (int fd)
{
  off_t pos = lseek (fd, 0, SEEK_CUR);
  return pos < 0 ? -1 : ftruncate (fd, pos);
}

/* Return true if NAME is the name of a regular file, or if the file
   does not exist (so it would be created as a regular file).  */
static bool
is_regular_file (const char *name)
{
  struct stat stbuf;

  if (stat (name, &stbuf) == 0)
    return !!S_ISREG (stbuf.st_mode);
  else
    return errno == ENOENT;
}

idx_t
sys_write_archive_buffer (void)
{
  return rmtwrite (archive, charptr (record_start), record_size);
}

/* Read and write file descriptors from a pipe(pipefd) call.  */
enum { PREAD, PWRITE };

/* Work around GCC bug 109839.  */
#if 13 <= __GNUC__
# pragma GCC diagnostic ignored "-Wanalyzer-fd-leak"
#endif

/* Duplicate file descriptor FROM into becoming INTO.
   INTO is closed first and has to be the next available slot.  */
static void
xdup2 (int from, int into)
{
  if (from != into)
    {
      if (dup2 (from, into) < 0)
	paxfatal (errno, _("Cannot dup2"));
      xclose (from);
    }
}

/* Propagate any failure of the grandchild back to the parent.  */
static _Noreturn void
wait_for_grandchild (pid_t pid)
{
  int wait_status;
  int exit_code = 0;

  while (waitpid (pid, &wait_status, 0) < 0)
    if (errno != EINTR)
      {
	waitpid_error (use_compress_program_option);
	break;
      }

  if (WIFSIGNALED (wait_status))
    raise (WTERMSIG (wait_status));
  else if (WEXITSTATUS (wait_status) != 0)
    exit_code = WEXITSTATUS (wait_status);

  exit (exit_code);
}

/* Set ARCHIVE for writing, then compressing an archive.  */
pid_t
sys_child_open_for_compress (void)
{
  int parent_pipe[2];
  int child_pipe[2];
  pid_t grandchild_pid;
  pid_t child_pid;

  signal (SIGPIPE, SIG_IGN);
  xpipe (parent_pipe);
  child_pid = xfork ();

  if (child_pid > 0)
    {
      /* The parent tar is still here!  Just clean up.  */

      archive = parent_pipe[PWRITE];
      xclose (parent_pipe[PREAD]);
      return child_pid;
    }

  /* The new born child tar is here!  */

  set_program_name (_("tar (child)"));
  signal (SIGPIPE, SIG_DFL);

  xdup2 (parent_pipe[PREAD], STDIN_FILENO);
  xclose (parent_pipe[PWRITE]);

  /* Check if we need a grandchild tar.  This happens only if either:
     a) the file is to be accessed by rmt: compressor doesn't know how;
     b) the file is not a plain file.  */

  if (!_remdev (archive_name_array[0])
      && is_regular_file (archive_name_array[0]))
    {
      if (backup_option)
	maybe_backup_file (archive_name_array[0], 1);

      /* We don't need a grandchild tar.  Open the archive and launch the
	 compressor.  */
      if (strcmp (archive_name_array[0], "-"))
	{
	  archive = creat (archive_name_array[0], MODE_RW);
	  if (archive < 0)
	    {
	      int saved_errno = errno;

	      if (backup_option)
		undo_last_backup ();
	      errno = saved_errno;
	      open_fatal (archive_name_array[0]);
	    }
	  xdup2 (archive, STDOUT_FILENO);
	}
      priv_set_restore_linkdir ();
      xexec (use_compress_program_option);
    }

  /* We do need a grandchild tar.  */

  xpipe (child_pipe);
  grandchild_pid = xfork ();

  if (grandchild_pid == 0)
    {
      /* The newborn grandchild tar is here!  Launch the compressor.  */

      set_program_name (_("tar (grandchild)"));

      xdup2 (child_pipe[PWRITE], STDOUT_FILENO);
      xclose (child_pipe[PREAD]);
      priv_set_restore_linkdir ();
      xexec (use_compress_program_option);
    }

  /* The child tar is still here!  */

  /* Prepare for reblocking the data from the compressor into the archive.  */

  xdup2 (child_pipe[PREAD], STDIN_FILENO);
  xclose (child_pipe[PWRITE]);

  if (strcmp (archive_name_array[0], "-") == 0)
    archive = STDOUT_FILENO;
  else
    {
      archive = rmtcreat (archive_name_array[0], MODE_RW, rsh_command_option);
      if (archive < 0)
	open_fatal (archive_name_array[0]);
    }

  /* Let's read out of the stdin pipe and write an archive.  */

  while (1)
    {
      ptrdiff_t status = 0;
      char *cursor;
      idx_t length;

      /* Assemble a record.  */

      for (length = 0, cursor = charptr (record_start);
	   length < record_size;
	   length += status, cursor += status)
	{
	  idx_t size = record_size - length;

	  status = safe_read (STDIN_FILENO, cursor, size);
	  if (status < 0)
	    read_fatal (use_compress_program_option);
	  if (status == 0)
	    break;
	}

      /* Copy the record.  */

      if (status == 0)
	{
	  /* We hit the end of the file.  Write last record at
	     full length, as the only role of the grandchild is
	     doing proper reblocking.  */

	  if (length > 0)
	    {
	      memset (charptr (record_start) + length, 0, record_size - length);
	      status = sys_write_archive_buffer ();
	      if (status != record_size)
		archive_write_error (status);
	    }

	  /* There is nothing else to read, break out.  */
	  break;
	}

      status = sys_write_archive_buffer ();
      if (status != record_size)
	archive_write_error (status);
    }

  wait_for_grandchild (grandchild_pid);
}

static void
run_decompress_program (void)
{
  int i;
  const char *p, *prog = NULL;
  struct wordsplit ws;
  int wsflags = (WRDSF_DEFFLAGS | WRDSF_ENV | WRDSF_DOOFFS) & ~WRDSF_NOVAR;

  ws.ws_env = (const char **) environ;
  ws.ws_offs = 1;

  for (p = first_decompress_program (&i); p; p = next_decompress_program (&i))
    {
      if (prog)
	{
	  warnopt (WARN_DECOMPRESS_PROGRAM, errno, _("cannot run %s"), prog);
	  warnopt (WARN_DECOMPRESS_PROGRAM, 0, _("trying %s"), p);
	}
      if (wordsplit (p, &ws, wsflags) != WRDSE_OK)
	paxfatal (0, _("cannot split string '%s': %s"),
		  p, wordsplit_strerror (&ws));
      wsflags |= WRDSF_REUSE;
      memmove (ws.ws_wordv, ws.ws_wordv + ws.ws_offs,
	       ws.ws_wordc * sizeof *ws.ws_wordv);
      ws.ws_wordv[ws.ws_wordc] = (char *) "-d";
      prog = p;
      execvp (ws.ws_wordv[0], ws.ws_wordv);
      ws.ws_wordv[ws.ws_wordc] = NULL;
    }
  if (!prog)
    paxfatal (0, _("unable to run decompression program"));
  exec_fatal (prog);
}

/* Set ARCHIVE for uncompressing, then reading an archive.  */
pid_t
sys_child_open_for_uncompress (void)
{
  int parent_pipe[2];
  int child_pipe[2];
  pid_t grandchild_pid;
  pid_t child_pid;

  xpipe (parent_pipe);
  child_pid = xfork ();

  if (child_pid > 0)
    {
      /* The parent tar is still here!  Just clean up.  */

      archive = parent_pipe[PREAD];
      xclose (parent_pipe[PWRITE]);
      return child_pid;
    }

  /* The newborn child tar is here!  */

  set_program_name (_("tar (child)"));
  signal (SIGPIPE, SIG_DFL);

  xdup2 (parent_pipe[PWRITE], STDOUT_FILENO);
  xclose (parent_pipe[PREAD]);

  /* Check if we need a grandchild tar.  This happens only if either:
     a) we're reading stdin: to force unblocking;
     b) the file is to be accessed by rmt: compressor doesn't know how;
     c) the file is not a plain file.  */

  if (strcmp (archive_name_array[0], "-") != 0
      && !_remdev (archive_name_array[0])
      && is_regular_file (archive_name_array[0]))
    {
      /* We don't need a grandchild tar.  Open the archive and launch the
	 uncompressor.  */

      archive = open (archive_name_array[0], O_RDONLY | O_BINARY, MODE_RW);
      if (archive < 0)
	open_fatal (archive_name_array[0]);
      xdup2 (archive, STDIN_FILENO);
      priv_set_restore_linkdir ();
      run_decompress_program ();
    }

  /* We do need a grandchild tar.  */

  xpipe (child_pipe);
  grandchild_pid = xfork ();

  if (grandchild_pid == 0)
    {
      /* The newborn grandchild tar is here!  Launch the uncompressor.  */

      set_program_name (_("tar (grandchild)"));

      xdup2 (child_pipe[PREAD], STDIN_FILENO);
      xclose (child_pipe[PWRITE]);
      priv_set_restore_linkdir ();
      run_decompress_program ();
    }

  /* The child tar is still here!  */

  /* Prepare for unblocking the data from the archive into the
     uncompressor.  */

  xdup2 (child_pipe[PWRITE], STDOUT_FILENO);
  xclose (child_pipe[PREAD]);

  if (strcmp (archive_name_array[0], "-") == 0)
    archive = STDIN_FILENO;
  else
    archive = rmtopen (archive_name_array[0], O_RDONLY | O_BINARY,
		       MODE_RW, rsh_command_option);
  if (archive < 0)
    open_fatal (archive_name_array[0]);

  /* Let's read the archive and pipe it into stdout.  */

  while (true)
    {
      clear_read_error_count ();

      ptrdiff_t n;
      while ((n = rmtread (archive, charptr (record_start), record_size)) < 0)
	archive_read_error ();
      if (n == 0)
	break;

      char *cursor = charptr (record_start);
      do
	{
	  idx_t count = min (n, BLOCKSIZE);
	  if (full_write (STDOUT_FILENO, cursor, count) != count)
	    write_error (use_compress_program_option);
	  cursor += count;
	  n -= count;
	}
      while (n);
    }

  xclose (STDOUT_FILENO);

  wait_for_grandchild (grandchild_pid);
}



static void
dec_to_env (char const *envar, uintmax_t num)
{
  char numstr[UINTMAX_STRSIZE_BOUND];
  if (setenv (envar, umaxtostr (num, numstr), 1) < 0)
    xalloc_die ();
}

static void
time_to_env (char const *envar, struct timespec t)
{
  char buf[TIMESPEC_STRSIZE_BOUND];
  if (setenv (envar, code_timespec (t, buf), 1) < 0)
    xalloc_die ();
}

static void
oct_to_env (char const *envar, mode_t m)
{
  char buf[sizeof "0" + (UINTMAX_WIDTH + 2) / 3];
  uintmax_t um = m;
  if (EXPR_SIGNED (m) && sizeof m < sizeof um)
    um &= ~ (UINTMAX_MAX << TYPE_WIDTH (m));
  sprintf (buf, "%#"PRIoMAX, um);
  if (setenv (envar, buf, 1) < 0)
    xalloc_die ();
}

static void
str_to_env (char const *envar, char const *str)
{
  if (str)
    {
      if (setenv (envar, str, 1) < 0)
	xalloc_die ();
    }
  else
    unsetenv (envar);
}

static void
chr_to_env (char const *envar, char c)
{
  char buf[2];
  buf[0] = c;
  buf[1] = 0;
  if (setenv (envar, buf, 1) < 0)
    xalloc_die ();
}

static void
stat_to_env (char *name, char type, struct tar_stat_info *st)
{
  str_to_env ("TAR_VERSION", PACKAGE_VERSION);
  str_to_env ("TAR_ARCHIVE", *archive_name_cursor);
  dec_to_env ("TAR_VOLUME", archive_name_cursor - archive_name_array + 1);
  dec_to_env ("TAR_BLOCKING_FACTOR", blocking_factor);
  str_to_env ("TAR_FORMAT",
	      archive_format_string (current_format == DEFAULT_FORMAT ?
				     archive_format : current_format));
  chr_to_env ("TAR_FILETYPE", type);
  oct_to_env ("TAR_MODE", st->stat.st_mode);
  str_to_env ("TAR_FILENAME", name);
  str_to_env ("TAR_REALNAME", st->file_name);
  str_to_env ("TAR_UNAME", st->uname);
  str_to_env ("TAR_GNAME", st->gname);
  time_to_env ("TAR_ATIME", st->atime);
  time_to_env ("TAR_MTIME", st->mtime);
  time_to_env ("TAR_CTIME", st->ctime);
  dec_to_env ("TAR_SIZE", st->stat.st_size);
  dec_to_env ("TAR_UID", st->stat.st_uid);
  dec_to_env ("TAR_GID", st->stat.st_gid);

  switch (type)
    {
    case 'b':
    case 'c':
      dec_to_env ("TAR_MINOR", minor (st->stat.st_rdev));
      dec_to_env ("TAR_MAJOR", major (st->stat.st_rdev));
      unsetenv ("TAR_LINKNAME");
      break;

    case 'l':
    case 'h':
      unsetenv ("TAR_MINOR");
      unsetenv ("TAR_MAJOR");
      str_to_env ("TAR_LINKNAME", st->link_name);
      break;

    default:
      unsetenv ("TAR_MINOR");
      unsetenv ("TAR_MAJOR");
      unsetenv ("TAR_LINKNAME");
      break;
    }
}

static pid_t global_pid;
static void (*pipe_handler) (int sig);

int
sys_exec_command (char *file_name, char typechar, struct tar_stat_info *st)
{
  int p[2];

  xpipe (p);
  pipe_handler = signal (SIGPIPE, SIG_IGN);
  global_pid = xfork ();

  if (global_pid != 0)
    {
      xclose (p[PREAD]);
      return p[PWRITE];
    }

  /* Child */
  xdup2 (p[PREAD], STDIN_FILENO);
  xclose (p[PWRITE]);

  stat_to_env (file_name, typechar, st);

  priv_set_restore_linkdir ();
  xexec (to_command_option);
}

void
sys_wait_command (void)
{
  int status;

  if (global_pid < 0)
    return;

  signal (SIGPIPE, pipe_handler);
  while (waitpid (global_pid, &status, 0) < 0)
    if (errno != EINTR)
      {
        global_pid = -1;
        waitpid_error (to_command_option);
        return;
      }

  if (WIFEXITED (status))
    {
      if (!ignore_command_error_option && WEXITSTATUS (status))
	paxerror (0, _("%jd: Child returned status %d"),
		  intmax (global_pid), WEXITSTATUS (status));
    }
  else if (WIFSIGNALED (status))
    {
      paxwarn (0, _("%jd: Child terminated on signal %d"),
	       intmax (global_pid), WTERMSIG (status));
    }
  else
    paxerror (0, _("%jd: Child terminated on unknown reason"),
	      intmax (global_pid));

  global_pid = -1;
}

int
sys_exec_info_script (const char **archive_name, intmax_t volume_number)
{
  int p[2];
  static void (*saved_handler) (int sig);

  xpipe (p);
  saved_handler = signal (SIGPIPE, SIG_IGN);

  pid_t pid = xfork ();

  if (pid != 0)
    {
      /* Master */

      int status;
      char *buf = NULL;
      size_t size = 0;

      xclose (p[PWRITE]);
      FILE *fp = fdopen (p[PREAD], "r");
      if (!fp)
	{
	  signal (SIGPIPE, saved_handler);
	  call_arg_error ("fdopen", info_script_option);
	  return -1;
	}
      ssize_t rc = getline (&buf, &size, fp);
      if (rc < 0)
	{
	  signal (SIGPIPE, saved_handler);
	  read_error (info_script_option);
	  return -1;
	}
      *archive_name = buf;
      buf[rc - 1] = '\0';
      if (fclose (fp) < 0)
	{
	  signal (SIGPIPE, saved_handler);
	  close_error (info_script_option);
	  return -1;
	}

      while (waitpid (pid, &status, 0) < 0)
	if (errno != EINTR)
	  {
	    signal (SIGPIPE, saved_handler);
	    waitpid_error (info_script_option);
	    return -1;
	  }

      signal (SIGPIPE, saved_handler);
      return WIFEXITED (status) ? WEXITSTATUS (status) : -1;
    }

  /* Child */
  str_to_env ("TAR_VERSION", PACKAGE_VERSION);
  str_to_env ("TAR_ARCHIVE", *archive_name);
  dec_to_env ("TAR_VOLUME", volume_number);
  dec_to_env ("TAR_BLOCKING_FACTOR", blocking_factor);
  setenv ("TAR_SUBCOMMAND", subcommand_string (subcommand_option), 1);
  setenv ("TAR_FORMAT",
	  archive_format_string (current_format == DEFAULT_FORMAT ?
				 archive_format : current_format), 1);
  dec_to_env ("TAR_FD", p[PWRITE]);

  xclose (p[PREAD]);

  priv_set_restore_linkdir ();
  xexec (info_script_option);
}

void
sys_exec_checkpoint_script (const char *script_name,
			    const char *archive_name,
			    intmax_t checkpoint_number)
{
  pid_t pid = xfork ();

  if (pid != 0)
    {
      /* Master */

      int status;

      while (waitpid (pid, &status, 0) < 0)
	if (errno != EINTR)
	  {
	    waitpid_error (script_name);
	    break;
	  }

      return;
    }

  /* Child */
  str_to_env ("TAR_VERSION", PACKAGE_VERSION);
  str_to_env ("TAR_ARCHIVE", archive_name);
  dec_to_env ("TAR_CHECKPOINT", checkpoint_number);
  dec_to_env ("TAR_BLOCKING_FACTOR", blocking_factor);
  str_to_env ("TAR_SUBCOMMAND", subcommand_string (subcommand_option));
  str_to_env ("TAR_FORMAT",
	      archive_format_string (current_format == DEFAULT_FORMAT
				     ? archive_format : current_format));
  priv_set_restore_linkdir ();
  xexec (script_name);
}

bool
sys_exec_setmtime_script (const char *script_name,
			  int dirfd,
			  const char *file_name,
			  const char *fmt,
			  struct timespec *ts)
{
  pid_t pid;
  int p[2];
  bool stop = false;
  struct pollfd pfd;

  char *buffer = NULL;
  idx_t buflen = 0;
  idx_t bufsize = 0;
  char *cp;
  bool rc = true;

  if (pipe (p) < 0)
    paxfatal (errno, _("pipe failed"));

  if ((pid = xfork ()) == 0)
    {
      char *command = xmalloc (strlen (script_name) + strlen (file_name) + 2);

      strcpy (command, script_name);
      strcat (command, " ");
      strcat (command, file_name);

      if (dirfd != AT_FDCWD && fchdir (dirfd) < 0)
	paxfatal (errno, _("chdir failed"));

      close (p[0]);
      if (dup2 (p[1], STDOUT_FILENO) < 0)
	paxfatal (errno, _("dup2 failed"));
      if (p[1] != STDOUT_FILENO)
	close (p[1]);

      close (STDIN_FILENO);
      if (open (dev_null, O_RDONLY) != STDIN_FILENO)
	open_error (dev_null);

      priv_set_restore_linkdir ();
      /* FIXME: This mishandles shell metacharacters in the file name.
	 Come to think of it, isn't every use of xexec suspect?  */
      xexec (command);
    }
  close (p[1]);

  pfd.fd = p[0];
  pfd.events = POLLIN;

  while (1)
    {
      int n = poll (&pfd, 1, -1);
      if (n < 0)
	{
	  if (errno != EINTR)
	    {
	      paxerror (errno, _("poll failed"));
	      stop = true;
	      break;
	    }
	}
      if (n == 0)
	break;
      if (pfd.revents & POLLIN)
	{
	  if (buflen == bufsize)
	    buffer = xpalloc (buffer, &bufsize, 1, -1, 1);
	  ssize_t nread = read (pfd.fd, buffer + buflen, bufsize - buflen);
	  if (nread < 0)
	    {
	      paxerror (errno, _("error reading output of %s"), script_name);
	      stop = true;
	      break;
	    }
	  if (nread == 0)
	    break;
	  buflen += n;
	}
      else if (pfd.revents & POLLHUP)
	break;
    }
  close (pfd.fd);

  if (stop)
    kill (SIGKILL, pid);

  sys_wait_for_child (pid, false);

  if (stop)
    {
      free (buffer);
      return false;
    }

  if (buflen == 0)
    {
      paxerror (0, _("empty output from \"%s %s\""), script_name, file_name);
      return false;
    }

  cp = memchr (buffer, '\n', buflen);
  if (cp)
    *cp = 0;
  else
    {
      if (buflen == bufsize)
	buffer = xirealloc (buffer, ++bufsize);
      buffer[buflen] = 0;
    }

  if (fmt)
    {
      struct tm tm;
      time_t t;
      cp = strptime (buffer, fmt, &tm);
      if (cp == NULL)
	{
	  paxerror (0, _("output from \"%s %s\" does not satisfy format string:"
			 " %s"),
		    script_name, file_name, buffer);
	  rc = false;
	}
      else if (*cp != 0)
	{
	  paxwarn (0, _("unconsumed output from \"%s %s\": %s"),
		   script_name, file_name, cp);
	  rc = false;
	}
      else
	{
	  tm.tm_wday = -1;
	  t = mktime (&tm);
	  if (tm.tm_wday < 0)
	    {
	      paxerror (errno, _("mktime failed"));
	      rc = false;
	    }
	  else
	    {
	      ts->tv_sec = t;
	      ts->tv_nsec = 0;
	    }
	}
    }
  else if (! parse_datetime (ts, buffer, NULL))
    {
      paxerror (0, _("unparsable output from \"%s %s\": %s"),
		script_name, file_name, buffer);
      rc = false;
    }

  free (buffer);

  return rc;
}

#endif /* not MSDOS */
