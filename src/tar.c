/* A tar (tape archiver) program.

   Copyright 1988-2025 Free Software Foundation, Inc.

   Written by John Gilmore, starting 1985-08-25.

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

#include <fnmatch.h>
#include <argp.h>
#include <argp-namefrob.h>
#include <argp-fmtstream.h>
#include <argp-version-etc.h>

#include <signal.h>
#if ! defined SIGCHLD && defined SIGCLD
# define SIGCHLD SIGCLD
#endif

#include "common.h"
/* Define variables declared in common.h that belong to tar.c.  */
enum subcommand subcommand_option;
enum archive_format archive_format;
idx_t blocking_factor;
idx_t record_size;
bool absolute_names_option;
bool utc_option;
bool full_time_option;
bool after_date_option;
enum atime_preserve atime_preserve_option;
bool backup_option;
enum backup_type backup_type;
bool block_number_option;
intmax_t checkpoint_option;
const char *use_compress_program_option;
bool dereference_option;
bool hard_dereference_option;
struct exclude *excluded;
char const *group_name_option;
gid_t group_option;
bool ignore_failed_read_option;
bool ignore_zeros_option;
bool incremental_option;
const char *info_script_option;
bool interactive_option;
intmax_t occurrence_option;
enum old_files old_files_option;
bool keep_directory_symlink_option;
const char *listed_incremental_option;
signed char incremental_level;
bool check_device_option;
struct mode_change *mode_option;
mode_t initial_umask;
bool multi_volume_option;
struct timespec newer_mtime_option;
enum set_mtime_option_mode set_mtime_option;
struct timespec mtime_option;
char *set_mtime_command;
char *set_mtime_format;
int recursion_option;
bool numeric_owner_option;
bool one_file_system_option;
bool one_top_level_option;
char *one_top_level_dir;
char const *owner_name_option;
uid_t owner_option;
bool recursive_unlink_option;
bool read_full_records_option;
bool remove_files_option;
const char *rsh_command_option;
bool same_order_option;
int same_owner_option;
int same_permissions_option;
int selinux_context_option;
int acls_option;
bool xattrs_option;
idx_t strip_name_components;
bool show_omitted_dirs_option;
bool sparse_option;
intmax_t tar_sparse_major;
intmax_t tar_sparse_minor;
enum hole_detection_method hole_detection;
bool starting_file_option;
tarlong tape_length_option;
bool to_stdout_option;
bool totals_option;
bool touch_option;
char *to_command_option;
bool ignore_command_error_option;
bool restrict_option;
int verbose_option;
bool verify_option;
const char *volno_file_option;
const char *volume_label_option;
bool posixly_correct;
const char **archive_name_array;
idx_t archive_names;
const char **archive_name_cursor;
char const *index_file_name;
int open_read_flags;
int open_searchdir_flags;
int fstatat_flags;
int seek_option;
bool unquote_option;
int savedir_sort_order;
bool show_transformed_names_option;
bool delay_directory_restore_option;

#include <argmatch.h>
#include <c-ctype.h>
#include <closeout.h>
#include <configmake.h>
#include <exitfail.h>
#include <parse-datetime.h>
#include <rmt.h>
#include <rmt-command.h>
#include <wordsplit.h>
#include <sysexits.h>
#include <quotearg.h>
#include <version-etc.h>
#include <xstrtol.h>
#include <stdopen.h>
#include <priv-set.h>
#include <savedir.h>

/* Local declarations.  */

#ifndef DEFAULT_ARCHIVE_FORMAT
# define DEFAULT_ARCHIVE_FORMAT GNU_FORMAT
#endif

#ifndef DEFAULT_ARCHIVE
# define DEFAULT_ARCHIVE "tar.out"
#endif

#ifndef DEFAULT_BLOCKING
# define DEFAULT_BLOCKING 20
#endif

/* Print a message if not all links are dumped */
static bool check_links_option;

/* Number of allocated tape drive names.  */
static idx_t allocated_archive_names;


/* Miscellaneous.  */

/* Name of option using stdin.  */
static const char *stdin_used_by;

/* Doesn't return if stdin already requested.  */
void
request_stdin (const char *option)
{
  if (stdin_used_by)
    paxusage (_("Options '%s' and '%s' both want standard input"),
	      stdin_used_by, option);

  stdin_used_by = option;
}

/* Returns true if and only if the user typed an affirmative response.  */
bool
confirm (const char *message_action, const char *message_name)
{
  static FILE *confirm_file;
  static bool confirm_file_EOF;
  bool status = false;

  if (!confirm_file)
    {
      if (archive == 0 || stdin_used_by)
	{
	  confirm_file = fopen (TTY_NAME, "r");
	  if (! confirm_file)
	    open_fatal (TTY_NAME);
	}
      else
	{
	  request_stdin ("-w");
	  confirm_file = stdin;
	}
    }

  fprintf (stdlis, "%s %s?", message_action, quote (message_name));
  fflush (stdlis);

  if (!confirm_file_EOF)
    {
      char *response = NULL;
      size_t response_size = 0;
      if (getline (&response, &response_size, confirm_file) < 0)
	confirm_file_EOF = true;
      else
	status = rpmatch (response) > 0;
      free (response);
    }

  if (confirm_file_EOF)
    {
      fputc ('\n', stdlis);
      fflush (stdlis);
    }

  return status;
}

static struct fmttab {
  char const *name;
  enum archive_format fmt;
} const fmttab[] = {
  { "v7",      V7_FORMAT },
  { "oldgnu",  OLDGNU_FORMAT },
  { "ustar",   USTAR_FORMAT },
  { "posix",   POSIX_FORMAT },
#if 0 /* not fully supported yet */
  { "star",    STAR_FORMAT },
#endif
  { "gnu",     GNU_FORMAT },
  { "pax",     POSIX_FORMAT }, /* An alias for posix */
  { NULL,      0 }
};

static void
set_archive_format (char const *name)
{
  struct fmttab const *p;

  for (p = fmttab; strcmp (p->name, name) != 0; )
    if (! (++p)->name)
      paxusage (_("%s: Invalid archive format"),
		quotearg_colon (name));

  archive_format = p->fmt;
}

static void
set_xattr_option (bool value)
{
  if (value)
    set_archive_format ("posix");
  xattrs_option = value;
}

const char *
archive_format_string (enum archive_format fmt)
{
  struct fmttab const *p;

  for (p = fmttab; p->name; p++)
    if (p->fmt == fmt)
      return p->name;
  return "unknown?";
}

static int
format_mask (int n)
{
  return 1 << n;
}

static void
assert_format (int fmt_mask)
{
  if ((format_mask (archive_format) & fmt_mask) == 0)
    paxusage (_("GNU features wanted on incompatible archive format"));
}

const char *
subcommand_string (enum subcommand c)
{
  switch (c)
    {
    case UNKNOWN_SUBCOMMAND:
      return "unknown?";

    case APPEND_SUBCOMMAND:
      return "-r";

    case CAT_SUBCOMMAND:
      return "-A";

    case CREATE_SUBCOMMAND:
      return "-c";

    case DELETE_SUBCOMMAND:
      return "-D";

    case DIFF_SUBCOMMAND:
      return "-d";

    case EXTRACT_SUBCOMMAND:
      return "-x";

    case LIST_SUBCOMMAND:
      return "-t";

    case UPDATE_SUBCOMMAND:
      return "-u";

    case TEST_LABEL_SUBCOMMAND:
      return "--test-label";
    }
  abort ();
}

static void
tar_list_quoting_styles (struct obstack *stk, char const *prefix)
{
  idx_t prefixlen = strlen (prefix);

  for (idx_t i = 0; quoting_style_args[i]; i++)
    {
      obstack_grow (stk, prefix, prefixlen);
      obstack_grow (stk, quoting_style_args[i],
		    strlen (quoting_style_args[i]));
      obstack_1grow (stk, '\n');
    }
}

static void
tar_set_quoting_style (char *arg)
{
  for (idx_t i = 0; quoting_style_args[i]; i++)
    if (strcmp (arg, quoting_style_args[i]) == 0)
      {
	set_quoting_style (NULL, i);
	return;
      }
  paxfatal (0,
	    _("Unknown quoting style '%s'."
	      " Try '%s --quoting-style=help' to get a list."),
	    arg, program_name);
}


/* Options.  */

enum
{
  ACLS_OPTION = CHAR_MAX + 1,
  ATIME_PRESERVE_OPTION,
  BACKUP_OPTION,
  CHECK_DEVICE_OPTION,
  CHECKPOINT_OPTION,
  CHECKPOINT_ACTION_OPTION,
  CLAMP_MTIME_OPTION,
  DELAY_DIRECTORY_RESTORE_OPTION,
  HARD_DEREFERENCE_OPTION,
  DELETE_OPTION,
  FORCE_LOCAL_OPTION,
  FULL_TIME_OPTION,
  GROUP_OPTION,
  GROUP_MAP_OPTION,
  IGNORE_COMMAND_ERROR_OPTION,
  IGNORE_FAILED_READ_OPTION,
  INDEX_FILE_OPTION,
  KEEP_DIRECTORY_SYMLINK_OPTION,
  KEEP_NEWER_FILES_OPTION,
  LEVEL_OPTION,
  LZIP_OPTION,
  LZMA_OPTION,
  LZOP_OPTION,
  MODE_OPTION,
  MTIME_OPTION,
  NEWER_MTIME_OPTION,
  NO_ACLS_OPTION,
  NO_AUTO_COMPRESS_OPTION,
  NO_CHECK_DEVICE_OPTION,
  NO_DELAY_DIRECTORY_RESTORE_OPTION,
  NO_IGNORE_COMMAND_ERROR_OPTION,
  NO_OVERWRITE_DIR_OPTION,
  NO_QUOTE_CHARS_OPTION,
  NO_SAME_OWNER_OPTION,
  NO_SAME_PERMISSIONS_OPTION,
  NO_SEEK_OPTION,
  NO_SELINUX_CONTEXT_OPTION,
  NO_XATTR_OPTION,
  NUMERIC_OWNER_OPTION,
  OCCURRENCE_OPTION,
  OLD_ARCHIVE_OPTION,
  ONE_FILE_SYSTEM_OPTION,
  ONE_TOP_LEVEL_OPTION,
  OVERWRITE_DIR_OPTION,
  OVERWRITE_OPTION,
  OWNER_OPTION,
  OWNER_MAP_OPTION,
  PAX_OPTION,
  POSIX_OPTION,
  QUOTE_CHARS_OPTION,
  QUOTING_STYLE_OPTION,
  RECORD_SIZE_OPTION,
  RECURSIVE_UNLINK_OPTION,
  REMOVE_FILES_OPTION,
  RESTRICT_OPTION,
  RMT_COMMAND_OPTION,
  RSH_COMMAND_OPTION,
  SAME_OWNER_OPTION,
  SELINUX_CONTEXT_OPTION,
  SHOW_DEFAULTS_OPTION,
  SHOW_OMITTED_DIRS_OPTION,
  SHOW_SNAPSHOT_FIELD_RANGES_OPTION,
  SHOW_TRANSFORMED_NAMES_OPTION,
  SKIP_OLD_FILES_OPTION,
  SORT_OPTION,
  HOLE_DETECTION_OPTION,
  SPARSE_VERSION_OPTION,
  STRIP_COMPONENTS_OPTION,
  SUFFIX_OPTION,
  TEST_LABEL_OPTION,
  TOTALS_OPTION,
  TO_COMMAND_OPTION,
  TRANSFORM_OPTION,
  UTC_OPTION,
  VOLNO_FILE_OPTION,
  WARNING_OPTION,
  XATTR_OPTION,
  XATTR_EXCLUDE,
  XATTR_INCLUDE,
  ZSTD_OPTION,
  SET_MTIME_COMMAND_OPTION,
  SET_MTIME_FORMAT_OPTION,
};

static char const doc[] = N_("\
GNU 'tar' saves many files together into a single tape or disk archive, \
and can restore individual files from the archive.\n\
\n\
Examples:\n\
  tar -cf archive.tar foo bar  # Create archive.tar from files foo and bar.\n\
  tar -tvf archive.tar         # List all files in archive.tar verbosely.\n\
  tar -xf archive.tar          # Extract all files from archive.tar.\n")
"\v"
N_("The backup suffix is '~', unless set with --suffix or SIMPLE_BACKUP_SUFFIX.\n\
The version control may be set with --backup or VERSION_CONTROL, values are:\n\n\
  none, off       never make backups\n\
  t, numbered     make numbered backups\n\
  nil, existing   numbered if numbered backups exist, simple otherwise\n\
  never, simple   always make simple backups\n");


/* NOTE:

   Available option letters are DEQY and eqy. Consider the following
   assignments:

   [For Solaris tar compatibility =/= Is it important at all?]
   e  exit immediately with a nonzero exit status if unexpected errors occur
   E  use extended headers (--format=posix)

   [q  alias for --occurrence=1 =/= this would better be used for quiet?]

   y  per-file gzip compression
   Y  per-block gzip compression.

   Additionally, the 'n' letter is assigned for option --seek, which
   is probably not needed and should be marked as deprecated, so that
   -n may become available in the future.
*/

/* Option group idenitfiers help in sorting options by category: */
enum
  {
    GRH_COMMAND,
    GRID_COMMAND,     /* Main operation mode */

    GRH_MODIFIER,
    GRID_MODIFIER,    /* Operation modifiers */

    GRID_FILE_NAME,

    GRH_OVERWRITE,
    GRID_OVERWRITE,   /* Overwrite control options */

    GRH_OUTPUT,
    GRID_OUTPUT,      /* Output stream selection */

    GRH_FATTR,
    GRID_FATTR,       /* File attributes (ownership and mode) */

    GRH_XATTR,
    GRID_XATTR,       /* Extended file attributes */

    GRH_DEVICE,
    GRID_DEVICE,      /* Device selection */

    GRH_BLOCKING,
    GRID_BLOCKING,    /* Block and record length */

    GRH_FORMAT,
    GRID_FORMAT,      /* Archive format options */
    GRDOC_FORMAT,

    GRID_FORMAT_OPT,

    GRH_COMPRESS,
    GRID_COMPRESS,    /* Compression options */

    GRH_FILE,
    GRID_FILE,        /* File selection options */

    GRH_NAME_XFORM,
    GRID_NAME_XFORM,  /* File name transformations */

    GRH_INFORMATIVE,
    GRID_INFORMATIVE, /* Informative options */

    GRH_COMPAT,
    GRID_COMPAT,      /* Compatibility options */

    GRH_OTHER,
    GRID_OTHER        /* Other options */
  };

static struct argp_option options[] = {
  {NULL, 0, NULL, 0,
   N_("Main operation mode:"), GRH_COMMAND },

  {"list", 't', 0, 0,
   N_("list the contents of an archive"), GRID_COMMAND },
  {"extract", 'x', 0, 0,
   N_("extract files from an archive"), GRID_COMMAND },
  {"get", 0, 0, OPTION_ALIAS, NULL, GRID_COMMAND },
  {"create", 'c', 0, 0,
   N_("create a new archive"), GRID_COMMAND },
  {"diff", 'd', 0, 0,
   N_("find differences between archive and file system"), GRID_COMMAND },
  {"compare", 0, 0, OPTION_ALIAS, NULL, GRID_COMMAND },
  {"append", 'r', 0, 0,
   N_("append files to the end of an archive"), GRID_COMMAND },
  {"update", 'u', 0, 0,
   N_("only append files newer than copy in archive"), GRID_COMMAND },
  {"catenate", 'A', 0, 0,
   N_("append tar files to an archive"), GRID_COMMAND },
  {"concatenate", 0, 0, OPTION_ALIAS, NULL, GRID_COMMAND },
  {"delete", DELETE_OPTION, 0, 0,
   N_("delete from the archive (not on mag tapes!)"), GRID_COMMAND },
  {"test-label", TEST_LABEL_OPTION, NULL, 0,
   N_("test the archive volume label and exit"), GRID_COMMAND },

  {NULL, 0, NULL, 0,
   N_("Operation modifiers:"), GRH_MODIFIER },

  {"sparse", 'S', 0, 0,
   N_("handle sparse files efficiently"), GRID_MODIFIER },
  {"hole-detection", HOLE_DETECTION_OPTION, N_("TYPE"), 0,
   N_("technique to detect holes"), GRID_MODIFIER },
  {"sparse-version", SPARSE_VERSION_OPTION, N_("MAJOR[.MINOR]"), 0,
   N_("set version of the sparse format to use (implies --sparse)"),
   GRID_MODIFIER},
  {"incremental", 'G', 0, 0,
   N_("handle old GNU-format incremental backup"), GRID_MODIFIER },
  {"listed-incremental", 'g', N_("FILE"), 0,
   N_("handle new GNU-format incremental backup"), GRID_MODIFIER },
  {"level", LEVEL_OPTION, N_("NUMBER"), 0,
   N_("dump level for created listed-incremental archive"), GRID_MODIFIER },
  {"ignore-failed-read", IGNORE_FAILED_READ_OPTION, 0, 0,
   N_("do not exit with nonzero on unreadable files"), GRID_MODIFIER },
  {"occurrence", OCCURRENCE_OPTION, N_("NUMBER"), OPTION_ARG_OPTIONAL,
   N_("process only the NUMBERth occurrence of each file in the archive;"
      " this option is valid only in conjunction with one of the subcommands"
      " --delete, --diff, --extract or --list and when a list of files"
      " is given either on the command line or via the -T option;"
      " NUMBER defaults to 1"), GRID_MODIFIER },
  {"seek", 'n', NULL, 0,
   N_("archive is seekable"), GRID_MODIFIER },
  {"no-seek", NO_SEEK_OPTION, NULL, 0,
   N_("archive is not seekable"), GRID_MODIFIER },
  {"no-check-device", NO_CHECK_DEVICE_OPTION, NULL, 0,
   N_("do not check device numbers when creating incremental archives"),
   GRID_MODIFIER },
  {"check-device", CHECK_DEVICE_OPTION, NULL, 0,
   N_("check device numbers when creating incremental archives (default)"),
   GRID_MODIFIER },

  {NULL, 0, NULL, 0,
   N_("Overwrite control:"), GRH_OVERWRITE },

  {"verify", 'W', 0, 0,
   N_("attempt to verify the archive after writing it"), GRID_OVERWRITE },
  {"remove-files", REMOVE_FILES_OPTION, 0, 0,
   N_("remove files after adding them to the archive"), GRID_OVERWRITE },
  {"keep-old-files", 'k', 0, 0,
   N_("don't replace existing files when extracting, "
      "treat them as errors"), GRID_OVERWRITE },
  {"skip-old-files", SKIP_OLD_FILES_OPTION, 0, 0,
   N_("don't replace existing files when extracting, silently skip over them"),
   GRID_OVERWRITE },
  {"keep-newer-files", KEEP_NEWER_FILES_OPTION, 0, 0,
   N_("don't replace existing files that are newer than their archive copies"), GRID_OVERWRITE },
  {"overwrite", OVERWRITE_OPTION, 0, 0,
   N_("overwrite existing files when extracting"), GRID_OVERWRITE },
  {"unlink-first", 'U', 0, 0,
   N_("remove each file prior to extracting over it"), GRID_OVERWRITE },
  {"recursive-unlink", RECURSIVE_UNLINK_OPTION, 0, 0,
   N_("empty hierarchies prior to extracting directory"), GRID_OVERWRITE },
  {"no-overwrite-dir", NO_OVERWRITE_DIR_OPTION, 0, 0,
   N_("preserve metadata of existing directories"), GRID_OVERWRITE },
  {"overwrite-dir", OVERWRITE_DIR_OPTION, 0, 0,
   N_("overwrite metadata of existing directories when extracting (default)"),
   GRID_OVERWRITE },
  {"keep-directory-symlink", KEEP_DIRECTORY_SYMLINK_OPTION, 0, 0,
   N_("preserve existing symlinks to directories when extracting"),
   GRID_OVERWRITE },
  {"one-top-level", ONE_TOP_LEVEL_OPTION, N_("DIR"), OPTION_ARG_OPTIONAL,
   N_("create a subdirectory to avoid having loose files extracted"),
   GRID_OVERWRITE },

  {NULL, 0, NULL, 0,
   N_("Select output stream:"), GRH_OUTPUT },

  {"to-stdout", 'O', 0, 0,
   N_("extract files to standard output"), GRID_OUTPUT },
  {"to-command", TO_COMMAND_OPTION, N_("COMMAND"), 0,
   N_("pipe extracted files to another program"), GRID_OUTPUT },
  {"ignore-command-error", IGNORE_COMMAND_ERROR_OPTION, 0, 0,
   N_("ignore exit codes of children"), GRID_OUTPUT },
  {"no-ignore-command-error", NO_IGNORE_COMMAND_ERROR_OPTION, 0, 0,
   N_("treat non-zero exit codes of children as error"), GRID_OUTPUT },

  {NULL, 0, NULL, 0,
   N_("Handling of file attributes:"), GRH_FATTR },

  {"owner", OWNER_OPTION, N_("NAME"), 0,
   N_("force NAME as owner for added files"), GRID_FATTR },
  {"group", GROUP_OPTION, N_("NAME"), 0,
   N_("force NAME as group for added files"), GRID_FATTR },
  {"owner-map", OWNER_MAP_OPTION, N_("FILE"), 0,
   N_("use FILE to map file owner UIDs and names"), GRID_FATTR },
  {"group-map", GROUP_MAP_OPTION, N_("FILE"), 0,
   N_("use FILE to map file owner GIDs and names"), GRID_FATTR },
  {"mtime", MTIME_OPTION, N_("DATE-OR-FILE"), 0,
   N_("set mtime for added files from DATE-OR-FILE"), GRID_FATTR },
  {"clamp-mtime", CLAMP_MTIME_OPTION, 0, 0,
   N_("only set time when the file is more recent than what was given with --mtime"), GRID_FATTR },
  {"set-mtime-command", SET_MTIME_COMMAND_OPTION, N_("COMMAND"), 0,
   N_("use output of the COMMAND to set mtime of the stored archive members"),
   GRID_FATTR },
  {"set-mtime-format", SET_MTIME_FORMAT_OPTION, N_("FORMAT"), 0,
   N_("set output format (in the sense of strptime(3)) of the --set-mtime-command command"), GRID_FATTR },
  {"mode", MODE_OPTION, N_("CHANGES"), 0,
   N_("force (symbolic) mode CHANGES for added files"), GRID_FATTR },
  {"atime-preserve", ATIME_PRESERVE_OPTION,
   N_("METHOD"), OPTION_ARG_OPTIONAL,
   N_("preserve access times on dumped files, either by restoring the times"
      " after reading (METHOD='replace'; default) or by not setting the times"
      " in the first place (METHOD='system')"), GRID_FATTR },
  {"touch", 'm', 0, 0,
   N_("don't extract file modified time"), GRID_FATTR },
  {"same-owner", SAME_OWNER_OPTION, 0, 0,
   N_("try extracting files with the same ownership as exists in the archive (default for superuser)"), GRID_FATTR },
  {"no-same-owner", NO_SAME_OWNER_OPTION, 0, 0,
   N_("extract files as yourself (default for ordinary users)"), GRID_FATTR },
  {"numeric-owner", NUMERIC_OWNER_OPTION, 0, 0,
   N_("always use numbers for user/group names"), GRID_FATTR },
  {"preserve-permissions", 'p', 0, 0,
   N_("extract information about file permissions (default for superuser)"),
   GRID_FATTR },
  {"same-permissions", 0, 0, OPTION_ALIAS, NULL, GRID_FATTR },
  {"no-same-permissions", NO_SAME_PERMISSIONS_OPTION, 0, 0,
   N_("apply the user's umask when extracting permissions from the archive (default for ordinary users)"), GRID_FATTR },
  {"preserve-order", 's', 0, 0,
   N_("member arguments are listed in the same order as the "
      "files in the archive"), GRID_FATTR },
  {"same-order", 0, 0, OPTION_ALIAS, NULL, GRID_FATTR },
  {"delay-directory-restore", DELAY_DIRECTORY_RESTORE_OPTION, 0, 0,
   N_("delay setting modification times and permissions of extracted"
      " directories until the end of extraction"), GRID_FATTR },
  {"no-delay-directory-restore", NO_DELAY_DIRECTORY_RESTORE_OPTION, 0, 0,
   N_("cancel the effect of --delay-directory-restore option"), GRID_FATTR },
  {"sort", SORT_OPTION, N_("ORDER"), 0,
#if D_INO_IN_DIRENT
   N_("directory sorting order: none (default), name or inode")
#else
   N_("directory sorting order: none (default) or name")
#endif
     , GRID_FATTR },

  {NULL, 0, NULL, 0,
   N_("Handling of extended file attributes:"), GRH_XATTR },

  {"xattrs", XATTR_OPTION, 0, 0,
   N_("Enable extended attributes support"), GRID_XATTR },
  {"no-xattrs", NO_XATTR_OPTION, 0, 0,
   N_("Disable extended attributes support"), GRID_XATTR },
  {"xattrs-include", XATTR_INCLUDE, N_("MASK"), 0,
   N_("specify the include pattern for xattr keys"), GRID_XATTR },
  {"xattrs-exclude", XATTR_EXCLUDE, N_("MASK"), 0,
   N_("specify the exclude pattern for xattr keys"), GRID_XATTR },
  {"selinux", SELINUX_CONTEXT_OPTION, 0, 0,
   N_("Enable the SELinux context support"), GRID_XATTR },
  {"no-selinux", NO_SELINUX_CONTEXT_OPTION, 0, 0,
   N_("Disable the SELinux context support"), GRID_XATTR },
  {"acls", ACLS_OPTION, 0, 0,
   N_("Enable the POSIX ACLs support"), GRID_XATTR },
  {"no-acls", NO_ACLS_OPTION, 0, 0,
   N_("Disable the POSIX ACLs support"), GRID_XATTR },

  {NULL, 0, NULL, 0,
   N_("Device selection and switching:"), GRH_DEVICE },

  {"file", 'f', N_("ARCHIVE"), 0,
   N_("use archive file or device ARCHIVE"), GRID_DEVICE },
#ifdef DEVICE_PREFIX
  {"-[0-7][lmh]", 0, NULL, OPTION_DOC, /* It is OK, since 'name' will never be
					  translated */
   N_("specify drive and density"), GRID_DEVICE },
#endif
  {NULL, '0', NULL, OPTION_HIDDEN, NULL, GRID_DEVICE },
  {NULL, '1', NULL, OPTION_HIDDEN, NULL, GRID_DEVICE },
  {NULL, '2', NULL, OPTION_HIDDEN, NULL, GRID_DEVICE },
  {NULL, '3', NULL, OPTION_HIDDEN, NULL, GRID_DEVICE },
  {NULL, '4', NULL, OPTION_HIDDEN, NULL, GRID_DEVICE },
  {NULL, '5', NULL, OPTION_HIDDEN, NULL, GRID_DEVICE },
  {NULL, '6', NULL, OPTION_HIDDEN, NULL, GRID_DEVICE },
  {NULL, '7', NULL, OPTION_HIDDEN, NULL, GRID_DEVICE },
  {NULL, '8', NULL, OPTION_HIDDEN, NULL, GRID_DEVICE },
  {NULL, '9', NULL, OPTION_HIDDEN, NULL, GRID_DEVICE },

  {"force-local", FORCE_LOCAL_OPTION, 0, 0,
   N_("archive file is local even if it has a colon"),
   GRID_DEVICE },
  {"rmt-command", RMT_COMMAND_OPTION, N_("COMMAND"), 0,
   N_("use given rmt COMMAND instead of rmt"),
   GRID_DEVICE },
  {"rsh-command", RSH_COMMAND_OPTION, N_("COMMAND"), 0,
   N_("use remote COMMAND instead of rsh"),
   GRID_DEVICE },

  {"multi-volume", 'M', 0, 0,
   N_("create/list/extract multi-volume archive"),
   GRID_DEVICE },
  {"tape-length", 'L', N_("NUMBER"), 0,
   N_("change tape after writing NUMBER x 1024 bytes"),
   GRID_DEVICE },
  {"info-script", 'F', N_("NAME"), 0,
   N_("run script at end of each tape (implies -M)"),
   GRID_DEVICE },
  {"new-volume-script", 0, 0, OPTION_ALIAS, NULL, GRID_DEVICE },
  {"volno-file", VOLNO_FILE_OPTION, N_("FILE"), 0,
   N_("use/update the volume number in FILE"),
   GRID_DEVICE },

  {NULL, 0, NULL, 0,
   N_("Device blocking:"), GRH_BLOCKING },

  {"blocking-factor", 'b', N_("BLOCKS"), 0,
   N_("BLOCKS x 512 bytes per record"), GRID_BLOCKING },
  {"record-size", RECORD_SIZE_OPTION, N_("NUMBER"), 0,
   N_("NUMBER of bytes per record, multiple of 512"), GRID_BLOCKING },
  {"ignore-zeros", 'i', 0, 0,
   N_("ignore zeroed blocks in archive (means EOF)"), GRID_BLOCKING },
  {"read-full-records", 'B', 0, 0,
   N_("reblock as we read (for 4.2BSD pipes)"), GRID_BLOCKING },

  {NULL, 0, NULL, 0,
   N_("Archive format selection:"), GRH_FORMAT },

  {"format", 'H', N_("FORMAT"), 0,
   N_("create archive of the given format"), GRID_FORMAT },

  {NULL, 0, NULL, 0, N_("FORMAT is one of the following:"), GRDOC_FORMAT },
  {"  v7", 0, NULL, OPTION_DOC|OPTION_NO_TRANS, N_("old V7 tar format"),
   GRDOC_FORMAT },
  {"  oldgnu", 0, NULL, OPTION_DOC|OPTION_NO_TRANS,
   N_("GNU format as per tar <= 1.12"), GRDOC_FORMAT },
  {"  gnu", 0, NULL, OPTION_DOC|OPTION_NO_TRANS,
   N_("GNU tar 1.13.x format"), GRDOC_FORMAT },
  {"  ustar", 0, NULL, OPTION_DOC|OPTION_NO_TRANS,
   N_("POSIX 1003.1-1988 (ustar) format"), GRDOC_FORMAT },
  {"  pax", 0, NULL, OPTION_DOC|OPTION_NO_TRANS,
   N_("POSIX 1003.1-2001 (pax) format"), GRDOC_FORMAT },
  {"  posix", 0, NULL, OPTION_DOC|OPTION_NO_TRANS, N_("same as pax"),
   GRDOC_FORMAT },

  {"old-archive", OLD_ARCHIVE_OPTION, 0, 0, /* FIXME */
   N_("same as --format=v7"), GRID_FORMAT_OPT },
  {"portability", 0, 0, OPTION_ALIAS, NULL, GRID_FORMAT_OPT },
  {"posix", POSIX_OPTION, 0, 0,
   N_("same as --format=posix"), GRID_FORMAT_OPT },
  {"pax-option", PAX_OPTION, N_("keyword[[:]=value][,keyword[[:]=value]]..."), 0,
   N_("control pax keywords"), GRID_FORMAT_OPT },
  {"label", 'V', N_("TEXT"), 0,
   N_("create archive with volume name TEXT; at list/extract time, use TEXT as a globbing pattern for volume name"),
   GRID_FORMAT_OPT },

  {NULL, 0, NULL, 0,
   N_("Compression options:"), GRH_COMPRESS },
  {"auto-compress", 'a', 0, 0,
   N_("use archive suffix to determine the compression program"),
   GRID_COMPRESS },
  {"no-auto-compress", NO_AUTO_COMPRESS_OPTION, 0, 0,
   N_("do not use archive suffix to determine the compression program"),
   GRID_COMPRESS },
  {"use-compress-program", 'I', N_("PROG"), 0,
   N_("filter through PROG (must accept -d)"), GRID_COMPRESS },
  /* Note: docstrings for the options below are generated by tar_help_filter */
  {"bzip2", 'j', 0, 0, NULL, GRID_COMPRESS },
  {"gzip", 'z', 0, 0, NULL, GRID_COMPRESS },
  {"gunzip", 0, 0, OPTION_ALIAS, NULL, GRID_COMPRESS },
  {"ungzip", 0, 0, OPTION_ALIAS, NULL, GRID_COMPRESS },
  {"compress", 'Z', 0, 0, NULL, GRID_COMPRESS },
  {"uncompress", 0, 0, OPTION_ALIAS, NULL, GRID_COMPRESS },
  {"lzip", LZIP_OPTION, 0, 0, NULL, GRID_COMPRESS },
  {"lzma", LZMA_OPTION, 0, 0, NULL, GRID_COMPRESS },
  {"lzop", LZOP_OPTION, 0, 0, NULL, GRID_COMPRESS },
  {"xz", 'J', 0, 0, NULL, GRID_COMPRESS },
  {"zstd", ZSTD_OPTION, 0, 0, NULL, GRID_COMPRESS },

  {NULL, 0, NULL, 0,
   N_("Local file selection:"), GRH_FILE },
  {"one-file-system", ONE_FILE_SYSTEM_OPTION, 0, 0,
   N_("stay in local file system when creating archive"), GRID_FILE },
  {"absolute-names", 'P', 0, 0,
   N_("don't strip leading '/'s from file names"), GRID_FILE },
  {"dereference", 'h', 0, 0,
   N_("follow symlinks; archive and dump the files they point to"),
   GRID_FILE },
  {"hard-dereference", HARD_DEREFERENCE_OPTION, 0, 0,
   N_("follow hard links; archive and dump the files they refer to"),
   GRID_FILE },
  {"starting-file", 'K', N_("MEMBER-NAME"), 0,
   N_("begin at member MEMBER-NAME when reading the archive"),
   GRID_FILE },
  {"newer", 'N', N_("DATE-OR-FILE"), 0,
   N_("only store files newer than DATE-OR-FILE"), GRID_FILE },
  {"after-date", 0, 0, OPTION_ALIAS, NULL, GRID_FILE },
  {"newer-mtime", NEWER_MTIME_OPTION, N_("DATE"), 0,
   N_("compare date and time when data changed only"), GRID_FILE },
  {"backup", BACKUP_OPTION, N_("CONTROL"), OPTION_ARG_OPTIONAL,
   N_("backup before removal, choose version CONTROL"), GRID_FILE },
  {"suffix", SUFFIX_OPTION, N_("STRING"), 0,
   N_("backup before removal, override usual suffix ('~' unless overridden by environment variable SIMPLE_BACKUP_SUFFIX)"),
   GRID_FILE },

  {NULL, 0, NULL, 0,
   N_("File name transformations:"), GRH_NAME_XFORM },
  {"strip-components", STRIP_COMPONENTS_OPTION, N_("NUMBER"), 0,
   N_("strip NUMBER leading components from file names on extraction"),
   GRID_NAME_XFORM },
  {"transform", TRANSFORM_OPTION, N_("EXPRESSION"), 0,
   N_("use sed replace EXPRESSION to transform file names"),
   GRID_NAME_XFORM },
  {"xform", 0, 0, OPTION_ALIAS, NULL, GRID_NAME_XFORM },

  {NULL, 0, NULL, 0,
   N_("Informative output:"), GRH_INFORMATIVE },

  {"checkpoint", CHECKPOINT_OPTION, N_("NUMBER"), OPTION_ARG_OPTIONAL,
   N_("display progress messages every NUMBERth record (default 10)"),
   GRID_INFORMATIVE },
  {"checkpoint-action", CHECKPOINT_ACTION_OPTION, N_("ACTION"), 0,
   N_("execute ACTION on each checkpoint"),
   GRID_INFORMATIVE },
  {"check-links", 'l', 0, 0,
   N_("print a message if not all links are dumped"), GRID_INFORMATIVE },
  {"totals", TOTALS_OPTION, N_("SIGNAL"), OPTION_ARG_OPTIONAL,
   N_("print total bytes after processing the archive; "
      "with an argument - print total bytes when this SIGNAL is delivered; "
      "Allowed signals are: SIGHUP, SIGQUIT, SIGINT, SIGUSR1 and SIGUSR2; "
      "the names without SIG prefix are also accepted"), GRID_INFORMATIVE },
  {"utc", UTC_OPTION, 0, 0,
   N_("print file modification times in UTC"), GRID_INFORMATIVE },
  {"full-time", FULL_TIME_OPTION, 0, 0,
   N_("print file time to its full resolution"), GRID_INFORMATIVE },
  {"index-file", INDEX_FILE_OPTION, N_("FILE"), 0,
   N_("send verbose output to FILE"), GRID_INFORMATIVE },
  {"block-number", 'R', 0, 0,
   N_("show block number within archive with each message"), GRID_INFORMATIVE },
  {"show-defaults", SHOW_DEFAULTS_OPTION, 0, 0,
   N_("show tar defaults"), GRID_INFORMATIVE },
  {"show-snapshot-field-ranges", SHOW_SNAPSHOT_FIELD_RANGES_OPTION, 0, 0,
   N_("show valid ranges for snapshot-file fields"), GRID_INFORMATIVE },
  {"show-omitted-dirs", SHOW_OMITTED_DIRS_OPTION, 0, 0,
   N_("when listing or extracting, list each directory that does not match search criteria"), GRID_INFORMATIVE },
  {"show-transformed-names", SHOW_TRANSFORMED_NAMES_OPTION, 0, 0,
   N_("show file or archive names after transformation"),
   GRID_INFORMATIVE },
  {"show-stored-names", 0, 0, OPTION_ALIAS, NULL, GRID_INFORMATIVE },
  {"quoting-style", QUOTING_STYLE_OPTION, N_("STYLE"), 0,
   N_("set name quoting style; see below for valid STYLE values"), GRID_INFORMATIVE },
  {"quote-chars", QUOTE_CHARS_OPTION, N_("STRING"), 0,
   N_("additionally quote characters from STRING"), GRID_INFORMATIVE },
  {"no-quote-chars", NO_QUOTE_CHARS_OPTION, N_("STRING"), 0,
   N_("disable quoting for characters from STRING"), GRID_INFORMATIVE },
  {"interactive", 'w', 0, 0,
   N_("ask for confirmation for every action"), GRID_INFORMATIVE },
  {"confirmation", 0, 0, OPTION_ALIAS, NULL, GRID_INFORMATIVE },
  {"verbose", 'v', 0, 0,
   N_("verbosely list files processed"), GRID_INFORMATIVE },
  {"warning", WARNING_OPTION, N_("KEYWORD"), 0,
   N_("warning control"), GRID_INFORMATIVE },

  {NULL, 0, NULL, 0,
   N_("Compatibility options:"), GRH_COMPAT },

  {NULL, 'o', 0, 0,
   N_("when creating, same as --old-archive; when extracting, same as --no-same-owner"), GRID_COMPAT },

  {NULL, 0, NULL, 0,
   N_("Other options:"), GRH_OTHER },

  {"restrict", RESTRICT_OPTION, 0, 0,
   N_("disable use of some potentially harmful options"), -1 },

  {0, 0, 0, 0, 0, 0}
};

static char const *const atime_preserve_args[] =
{
  "replace", "system", NULL
};

static enum atime_preserve const atime_preserve_types[] =
{
  replace_atime_preserve, system_atime_preserve
};

/* Make sure atime_preserve_types has as much entries as atime_preserve_args
   (minus 1 for NULL guard) */
ARGMATCH_VERIFY (atime_preserve_args, atime_preserve_types);


static char * ATTRIBUTE_FORMAT ((printf, 1, 2))
easprintf (char const *format, ...)
{
  va_list args;

  va_start (args, format);
  char *result = xvasprintf (format, args);
  int err = errno;
  va_end (args);

  if (!result)
    paxfatal (err, "vasprintf");
  return result;
}

static char *
format_default_settings (void)
{
  return easprintf (
	    "--format=%s -f%s -b%d --quoting-style=%s --rmt-command=%s"
#ifdef REMOTE_SHELL
	    " --rsh-command=%s"
#endif
	    ,
	    archive_format_string (DEFAULT_ARCHIVE_FORMAT),
	    DEFAULT_ARCHIVE, DEFAULT_BLOCKING,
	    quoting_style_args[DEFAULT_QUOTING_STYLE],
	    DEFAULT_RMT_COMMAND
#ifdef REMOTE_SHELL
	    , REMOTE_SHELL
#endif
	    );
}

static void
option_conflict_error (const char *a, const char *b)
{
  /* TRANSLATORS: Both %s in this statement are replaced with
     option names. */
  paxusage (_("'%s' cannot be used with '%s'"), a, b);
}

/* Classes of options that can conflict: */
enum option_class
  {
    OC_COMPRESS,                 /* Compress options: -JjZz, -I, etc. */
    OC_OCCURRENCE,               /* --occurrence */
    OC_LISTED_INCREMENTAL,       /* --listed-incremental */
    OC_NEWER,                    /* --newer, --newer-mtime, --after-date */
    OC_VERIFY,                   /* --verify */
    OC_STARTING_FILE,            /* --starting-file */
    OC_SAME_ORDER,               /* --same-order */
    OC_ONE_TOP_LEVEL,            /* --one-top-level */
    OC_ABSOLUTE_NAMES,           /* --absolute-names */
    OC_OLD_FILES,                /* --keep-old-files, --overwrite, etc. */
    OC_MAX
  };

/* Table of locations of potentially conflicting options.  Two options can
   conflict only if they proceed from the command line.  Otherwise, options
   in command line silently override those defined in TAR_OPTIONS. */
static struct option_locus *option_class[OC_MAX];

/* Save location of an option of class ID.  Return location of a previous
   occurrence of an option of that class, or NULL. */
static struct option_locus *
optloc_save (enum option_class id, struct option_locus *loc)
{
  char const *name = loc->name;
  idx_t namesize = name ? strlen (name) + 1 : 0;
  struct option_locus *optloc = ximalloc (sizeof *loc + namesize);
  optloc->name = name ? memcpy (optloc + 1, name, namesize) : NULL;
  optloc->source = loc->source;
  optloc->line = loc->line;
  optloc->prev = option_class[id];
  option_class[id] = optloc;
  return optloc->prev;
}

/* Return location of a recent option of class ID */
static struct option_locus *
optloc_lookup (enum option_class id)
{
  return option_class[id];
}

/* Return true if the latest occurrence of option ID was in the command line */
static bool
option_set_in_cl (enum option_class id)
{
  struct option_locus *loc = optloc_lookup (id);
  return loc && loc->source == OPTS_COMMAND_LINE;
}

/* Compare two option locations */
static bool
optloc_eq (struct option_locus *a, struct option_locus *b)
{
  assume (a);  /* Pacify GCC bug 106436.  */
  if (a->source != b->source)
    return false;
  if (a->source == OPTS_COMMAND_LINE)
    return true;
  assume (a->name);
  return strcmp (a->name, b->name) == 0;
}

static void
set_subcommand_option (enum subcommand subcommand)
{
  if (subcommand_option != UNKNOWN_SUBCOMMAND
      && subcommand_option != subcommand)
    paxusage (_("You may not specify more than one '-Acdtrux',"
		" '--delete' or  '--test-label' option"));

  subcommand_option = subcommand;
}

static void
set_use_compress_program_option (const char *string, struct option_locus *loc)
{
  struct option_locus *p = optloc_save (OC_COMPRESS, loc);
  if (use_compress_program_option
      && strcmp (use_compress_program_option, string) != 0
      && p->source == OPTS_COMMAND_LINE)
    paxusage (_("Conflicting compression options"));

  use_compress_program_option = string;
}

static void
sigstat (int signo)
{
  compute_duration_ns ();
  print_total_stats ();
#ifndef HAVE_SIGACTION
  signal (signo, sigstat);
#endif
}

static void
stat_on_signal (int signo)
{
#ifdef HAVE_SIGACTION
# ifndef SA_RESTART
#  define SA_RESTART 0
# endif
  struct sigaction act;
  act.sa_handler = sigstat;
  sigemptyset (&act.sa_mask);
  act.sa_flags = SA_RESTART;
  sigaction (signo, &act, NULL);
#else
  signal (signo, sigstat);
#endif
}

int
decode_signal (const char *name)
{
  static struct sigtab
  {
    char name[sizeof "USR1"];
    int signo;
  } const sigtab[] = {
    { "USR1", SIGUSR1 },
    { "USR2", SIGUSR2 },
    { "HUP", SIGHUP },
    { "INT", SIGINT },
    { "QUIT", SIGQUIT }
  };
  enum { nsigtab = sizeof sigtab / sizeof *sigtab };
  char const *s = name;

  if (strncmp (s, "SIG", 3) == 0)
    s += 3;
  for (struct sigtab const *p = sigtab; p < sigtab + nsigtab; p++)
    if (strcmp (p->name, s) == 0)
      return p->signo;
  paxfatal (0, _("Unknown signal name: %s"), name);
}

static void
set_stat_signal (const char *name)
{
  stat_on_signal (decode_signal (name));
}


struct textual_date
{
  struct textual_date *next;
  struct timespec ts;
  const char *option;
  char *date;
};

static bool
get_date_or_file (struct tar_args *args, const char *option,
		  const char *str, struct timespec *ts)
{
  if (FILE_SYSTEM_PREFIX_LEN (str) != 0
      || ISSLASH (*str)
      || *str == '.')
    {
      struct stat st;
      if (stat (str, &st) < 0)
	{
	  stat_error (str);
	  paxusage (_("Date sample file not found"));
	}
      *ts = get_stat_mtime (&st);
    }
  else
    {
      if (! parse_datetime (ts, str, NULL))
	{
	  paxwarn (0, _("Substituting %s for unknown date format %s"),
		   tartime (*ts, false), quote (str));
	  ts->tv_nsec = 0;
	  return false;
	}
      else
	{
	  struct textual_date *p = xmalloc (sizeof (*p));
	  p->ts = *ts;
	  p->option = option;
	  p->date = xstrdup (str);
	  p->next = args->textual_date;
	  args->textual_date = p;
	}
    }
  return true;
}

static void
report_textual_dates (struct tar_args *args)
{
  struct textual_date *p;
  for (p = args->textual_date; p; )
    {
      struct textual_date *next = p->next;
      if (verbose_option)
	{
	  char const *treated_as = tartime (p->ts, true);
	  if (strcmp (p->date, treated_as) != 0)
	    paxwarn (0, _("Option %s: Treating date '%s' as %s"),
		     p->option, p->date, treated_as);
	}
      free (p->date);
      free (p);
      p = next;
    }
}


/* Default density numbers for [0-9][lmh] device specifications */

#if defined DEVICE_PREFIX && !defined DENSITY_LETTER
# ifndef LOW_DENSITY_NUM
#  define LOW_DENSITY_NUM 0
# endif

# ifndef MID_DENSITY_NUM
#  define MID_DENSITY_NUM 8
# endif

# ifndef HIGH_DENSITY_NUM
#  define HIGH_DENSITY_NUM 16
# endif
#endif


static char *
tar_help_filter (int key, const char *text, MAYBE_UNUSED void *input)
{
  struct obstack stk;
  char *s;

  switch (key)
    {
    default:
      s = (char *) text;
      break;

    case 'j':
      s = easprintf (_("filter the archive through %s"), BZIP2_PROGRAM);
      break;

    case 'z':
      s = easprintf (_("filter the archive through %s"), GZIP_PROGRAM);
      break;

    case 'Z':
      s = easprintf (_("filter the archive through %s"), COMPRESS_PROGRAM);
      break;

    case LZIP_OPTION:
      s = easprintf (_("filter the archive through %s"), LZIP_PROGRAM);
      break;

    case LZMA_OPTION:
      s = easprintf (_("filter the archive through %s"), LZMA_PROGRAM);
      break;

    case LZOP_OPTION:
      s = easprintf (_("filter the archive through %s"), LZOP_PROGRAM);
      break;

    case 'J':
      s = easprintf (_("filter the archive through %s"), XZ_PROGRAM);
      break;

    case ZSTD_OPTION:
      s = easprintf (_("filter the archive through %s"), ZSTD_PROGRAM);
      break;

    case ARGP_KEY_HELP_EXTRA:
      {
	const char *tstr;

	obstack_init (&stk);
	tstr = _("Valid arguments for the --quoting-style option are:");
	obstack_grow (&stk, tstr, strlen (tstr));
	obstack_grow (&stk, "\n\n", 2);
	tar_list_quoting_styles (&stk, "  ");
	tstr = _("\n*This* tar defaults to:\n");
	obstack_grow (&stk, tstr, strlen (tstr));
	s = format_default_settings ();
	obstack_grow (&stk, s, strlen (s));
	obstack_1grow (&stk, '\n');
	obstack_1grow (&stk, 0);
	s = xstrdup (obstack_finish (&stk));
	obstack_free (&stk, NULL);
      }
    }
  return s;
}

static char * _GL_ATTRIBUTE_MALLOC
expand_pax_option (struct tar_args *targs, const char *arg)
{
  struct obstack stk;
  char *res;

  obstack_init (&stk);
  while (*arg)
    {
      idx_t seglen = strcspn (arg, ",");
      char *p = memchr (arg, '=', seglen);
      if (p)
	{
	  idx_t len = p - arg + 1;
	  obstack_grow (&stk, arg, len);
	  len = seglen - len;
	  for (++p; *p && c_isspace (*p); p++)
	    len--;
	  if (*p == '{' && p[len-1] == '}')
	    {
	      struct timespec ts;
	      char *tmp = xmalloc (len);
	      memcpy (tmp, p + 1, len-2);
	      tmp[len-2] = 0;
	      if (get_date_or_file (targs, "--pax-option", tmp, &ts))
		{
		  char buf[TIMESPEC_STRSIZE_BOUND];
		  char const *s = code_timespec (ts, buf);
		  obstack_grow (&stk, s, strlen (s));
		}
	      else
		obstack_grow (&stk, p, len);
	      free (tmp);
	    }
	  else
	    obstack_grow (&stk, p, len);
	}
      else
	obstack_grow (&stk, arg, seglen);

      arg += seglen;
      if (*arg)
	{
	  obstack_1grow (&stk, *arg);
	  arg++;
	}
    }
  obstack_1grow (&stk, 0);
  res = xstrdup (obstack_finish (&stk));
  obstack_free (&stk, NULL);
  return res;
}


static uintmax_t
parse_owner_group (char *arg, uintmax_t field_max, char const **name_option)
{
  char const *name = NULL;
  char const *num = arg;
  char *colon = strchr (arg, ':');
  if (colon)
    {
      num = colon + 1;
      *colon = '\0';
      if (arg != colon)
	name = arg;
    }

  bool overflow;
  char *end;
  uintmax_t u = stoint (num, &end, &overflow, 0, field_max);
  if ((end == num) | *end | overflow)
    paxfatal (0, "%s: %s", quotearg_colon (num),
	      _("Invalid owner or group ID"));
  if (name_option)
    *name_option = name;
  return u;
}

static char const TAR_SIZE_SUFFIXES[] = "bBcGgkKMmPTtw";

static char const *const sort_mode_arg[] = {
  "none",
  "name",
#if D_INO_IN_DIRENT
  "inode",
#endif
  NULL
};

static enum savedir_option const sort_mode_flag[] = {
    SAVEDIR_SORT_NONE,
    SAVEDIR_SORT_NAME,
#if D_INO_IN_DIRENT
    SAVEDIR_SORT_INODE
#endif
};

ARGMATCH_VERIFY (sort_mode_arg, sort_mode_flag);

static char const *const hole_detection_args[] =
{
  "raw", "seek", NULL
};

static enum hole_detection_method const hole_detection_types[] =
{
  HOLE_DETECTION_RAW, HOLE_DETECTION_SEEK
};

ARGMATCH_VERIFY (hole_detection_args, hole_detection_types);


static void
set_old_files_option (enum old_files code, struct option_locus *loc)
{
  struct option_locus *prev;
  /* Option compatibility map. 0 means two options are incompatible. */
  static bool compat_map[MAX_OLD_FILES][MAX_OLD_FILES] = {
    [NO_OVERWRITE_DIR_OLD_FILES] = {
      [KEEP_OLD_FILES] = 1,
      [SKIP_OLD_FILES] = 1
    },
    [KEEP_OLD_FILES] = {
      [NO_OVERWRITE_DIR_OLD_FILES] = 1
    },
    [SKIP_OLD_FILES] = {
      [NO_OVERWRITE_DIR_OLD_FILES] = 1
    }
  };
  static char const *const code_to_opt[] = {
    "--overwrite-dir",
    "--no-overwrite-dir",
    "--overwrite",
    "--unlink-first",
    "--keep-old-files",
    "--skip-old-files",
    "--keep-newer-files"
  };

  prev = optloc_save (OC_OLD_FILES, loc);
  if (prev && optloc_eq (loc, prev) && code != old_files_option &&
      compat_map[code][old_files_option] == 0)
    option_conflict_error (code_to_opt[code], code_to_opt[old_files_option]);

  old_files_option = code;
}

static error_t
parse_opt (int key, char *arg, struct argp_state *state)
{
  struct tar_args *args = state->input;

  switch (key)
    {
    case ARGP_KEY_INIT:
      if (state->root_argp->children)
	for (idx_t i = 0; state->root_argp->children[i].argp; i++)
	  state->child_inputs[i] = state->input;
      break;

    case ARGP_KEY_ARG:
      /* File name or non-parsed option, because of ARGP_IN_ORDER */
      name_add_name (arg);
      break;

    case 'A':
      set_subcommand_option (CAT_SUBCOMMAND);
      break;

    case 'a':
      args->compress_autodetect = true;
      break;

    case NO_AUTO_COMPRESS_OPTION:
      args->compress_autodetect = false;
      break;

    case 'b':
      {
	bool overflow;
	char *end;
	blocking_factor = stoint (arg, &end, &overflow, 0,
				  (min (IDX_MAX, min (SSIZE_MAX, SIZE_MAX))
				   / BLOCKSIZE));
	if ((end == arg) | *end | overflow | !blocking_factor)
	  paxusage ("%s: %s", quotearg_colon (arg),
		    _("Invalid blocking factor"));
	record_size = blocking_factor * BLOCKSIZE;
      }
      break;

    case 'B':
      /* Try to reblock input records.  For reading 4.2BSD pipes.  */

      /* It would surely make sense to exchange -B and -R, but it seems
	 that -B has been used for a long while in Sun tar and most
	 BSD-derived systems.  This is a consequence of the block/record
	 terminology confusion.  */

      read_full_records_option = true;
      break;

    case 'c':
      set_subcommand_option (CREATE_SUBCOMMAND);
      break;

    case CLAMP_MTIME_OPTION:
      set_mtime_option = CLAMP_MTIME;
      break;

    case 'd':
      set_subcommand_option (DIFF_SUBCOMMAND);
      break;

    case 'F':
      /* Since -F is only useful with -M, make it implied.  Run this
	 script at the end of each tape.  */

      info_script_option = arg;
      multi_volume_option = true;
      break;

    case 'f':
      if (archive_names == allocated_archive_names)
	archive_name_array = xpalloc (archive_name_array,
				      &allocated_archive_names,
				      1, -1, sizeof *archive_name_array);
      archive_name_array[archive_names++] = arg;
      break;

    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':

#ifdef DEVICE_PREFIX
      {
	int device = key - '0';
	static char buf[sizeof DEVICE_PREFIX + INT_STRLEN_BOUND (int)]
	  = DEVICE_PREFIX;

	if (arg[1])
	  argp_error (state, _("Malformed density argument: %s"), quote (arg));

	char *cursor = buf + sizeof DEVICE_PREFIX - 1;

#ifdef DENSITY_LETTER

	sprintf (cursor, "%d%c", device, arg[0]);

#else /* not DENSITY_LETTER */

	switch (arg[0])
	  {
	  case 'l':
	    device += LOW_DENSITY_NUM;
	    break;

	  case 'm':
	    device += MID_DENSITY_NUM;
	    break;

	  case 'h':
	    device += HIGH_DENSITY_NUM;
	    break;

	  default:
	    argp_error (state, _("Unknown density: '%c'"), arg[0]);
	  }
	sprintf (cursor, "%d", device);

#endif /* not DENSITY_LETTER */

	if (archive_names == allocated_archive_names)
	  archive_name_array = xpalloc (archive_name_array,
					&allocated_archive_names,
					1, -1, sizeof *archive_name_array);
	archive_name_array[archive_names++] = xstrdup (buf);
      }
      break;

#else /* not DEVICE_PREFIX */

      argp_error (state,
		  _("Options '-[0-7][lmh]' not supported by *this* tar"));
      exit (EX_USAGE);

#endif /* not DEVICE_PREFIX */
      break;

    case FULL_TIME_OPTION:
      full_time_option = true;
      break;

    case 'g':
      optloc_save (OC_LISTED_INCREMENTAL, args->loc);
      listed_incremental_option = arg;
      after_date_option = true;
      FALLTHROUGH;
    case 'G':
      /* We are making an incremental dump (FIXME: are we?); save
	 directories at the beginning of the archive, and include in each
	 directory its contents.  */

      incremental_option = true;
      break;

    case 'h':
      /* Follow symbolic links.  */
      dereference_option = true;
      break;

    case HARD_DEREFERENCE_OPTION:
      hard_dereference_option = true;
      break;

    case 'i':
      /* Ignore zero blocks (eofs).  This can't be the default,
	 because Unix tar writes two blocks of zeros, then pads out
	 the record with garbage.  */

      ignore_zeros_option = true;
      break;

    case 'j':
      set_use_compress_program_option (BZIP2_PROGRAM, args->loc);
      break;

    case 'J':
      set_use_compress_program_option (XZ_PROGRAM, args->loc);
      break;

    case 'k':
      /* Don't replace existing files.  */
      set_old_files_option (KEEP_OLD_FILES, args->loc);
      break;

    case 'K':
      optloc_save (OC_STARTING_FILE, args->loc);
      add_starting_file (arg);
      break;

    case ONE_FILE_SYSTEM_OPTION:
      /* When dumping directories, don't dump files/subdirectories
	 that are on other filesystems. */
      one_file_system_option = true;
      break;

    case ONE_TOP_LEVEL_OPTION:
      optloc_save (OC_ONE_TOP_LEVEL, args->loc);
      one_top_level_option = true;
      one_top_level_dir = arg;
      break;

    case 'l':
      check_links_option = true;
      break;

    case 'L':
      {
	uintmax_t u;
	char *p;

	switch (xstrtoumax (arg, &p, 10, &u, TAR_SIZE_SUFFIXES))
	  {
	  case LONGINT_OK:
	    tape_length_option = u;
	    if (arg < p && !strchr (TAR_SIZE_SUFFIXES, p[-1]))
	      tape_length_option *= 1024;
	    break;

	  case LONGINT_OVERFLOW:
	    /* Treat enormous values as effectively infinity.  */
	    tape_length_option = 0;
	    break;

	  default:
	    paxusage ("%s: %s", quotearg_colon (arg),
		      _("Invalid tape length"));
	  }

	multi_volume_option = true;
      }
      break;

    case LEVEL_OPTION:
      {
	char *end;
	incremental_level = stoint (arg, &end, NULL, 0, 1);
	if ((end == arg) | *end)
	  paxusage (_("Invalid incremental level value"));
      }
      break;

    case LZIP_OPTION:
      set_use_compress_program_option (LZIP_PROGRAM, args->loc);
      break;

    case LZMA_OPTION:
      set_use_compress_program_option (LZMA_PROGRAM, args->loc);
      break;

    case LZOP_OPTION:
      set_use_compress_program_option (LZOP_PROGRAM, args->loc);
      break;

    case 'm':
      touch_option = true;
      break;

    case 'M':
      /* Make multivolume archive: when we can't write any more into
	 the archive, re-open it, and continue writing.  */

      multi_volume_option = true;
      break;

    case MTIME_OPTION:
      get_date_or_file (args, "--mtime", arg, &mtime_option);
      if (set_mtime_option == USE_FILE_MTIME)
        set_mtime_option = FORCE_MTIME;
      break;

    case 'n':
      seek_option = 1;
      break;

    case NO_SEEK_OPTION:
      seek_option = 0;
      break;

    case 'N':
      after_date_option = true;
      FALLTHROUGH;
    case NEWER_MTIME_OPTION:
      if (time_option_initialized (newer_mtime_option))
	paxusage (_("More than one threshold date"));
      get_date_or_file (args,
			key == NEWER_MTIME_OPTION ? "--newer-mtime"
			: "--after-date", arg, &newer_mtime_option);
      optloc_save (OC_NEWER, args->loc);
      break;

    case 'o':
      args->o_option = true;
      break;

    case 'O':
      to_stdout_option = true;
      break;

    case 'p':
      same_permissions_option = true;
      break;

    case 'P':
      optloc_save (OC_ABSOLUTE_NAMES, args->loc);
      absolute_names_option = true;
      break;

    case 'r':
      set_subcommand_option (APPEND_SUBCOMMAND);
      break;

    case 'R':
      /* Print block numbers for debugging bad tar archives.  */

      /* It would surely make sense to exchange -B and -R, but it seems
	 that -B has been used for a long while in Sun tar and most
	 BSD-derived systems.  This is a consequence of the block/record
	 terminology confusion.  */

      block_number_option = true;
      break;

    case 's':
      /* Names to extract are sorted.  */
      optloc_save (OC_SAME_ORDER, args->loc);
      same_order_option = true;
      break;

    case 'S':
      sparse_option = true;
      break;

    case SKIP_OLD_FILES_OPTION:
      set_old_files_option (SKIP_OLD_FILES, args->loc);
      break;

    case HOLE_DETECTION_OPTION:
      hole_detection = XARGMATCH ("--hole-detection", arg,
				  hole_detection_args, hole_detection_types);
      sparse_option = true;
      break;

    case SET_MTIME_COMMAND_OPTION:
      set_mtime_command = arg;
      break;

    case SET_MTIME_FORMAT_OPTION:
      set_mtime_format = arg;
      break;

    case SPARSE_VERSION_OPTION:
      sparse_option = true;
      {
	char *p;
	bool vmajor, vminor;
	tar_sparse_major = stoint (arg, &p, &vmajor, 0, INTMAX_MAX);
	if ((p != arg) & (*p == '.'))
	  tar_sparse_minor = stoint (p + 1, &p, &vminor, 0, INTMAX_MAX);
	if ((p == arg) | *p | vmajor | vminor)
	  paxusage (_("Invalid sparse version value"));
      }
      break;

    case 't':
      set_subcommand_option (LIST_SUBCOMMAND);
      verbose_option += verbose_option <= 2;
      break;

    case TEST_LABEL_OPTION:
      set_subcommand_option (TEST_LABEL_SUBCOMMAND);
      break;

    case TRANSFORM_OPTION:
      set_transform_expr (arg);
      break;

    case 'u':
      set_subcommand_option (UPDATE_SUBCOMMAND);
      break;

    case 'U':
      set_old_files_option (UNLINK_FIRST_OLD_FILES, args->loc);
      break;

    case UTC_OPTION:
      utc_option = true;
      break;

    case 'V':
      volume_label_option = arg;
      break;

    case 'v':
      verbose_option += verbose_option <= 2;
      warning_option |= WARN_VERBOSE_WARNINGS;
      break;

    case 'w':
      interactive_option = true;
      break;

    case 'W':
      optloc_save (OC_VERIFY, args->loc);
      verify_option = true;
      break;

    case WARNING_OPTION:
      set_warning_option (arg);
      break;

    case 'x':
      set_subcommand_option (EXTRACT_SUBCOMMAND);
      break;

    case 'z':
      set_use_compress_program_option (GZIP_PROGRAM, args->loc);
      break;

    case 'Z':
      set_use_compress_program_option (COMPRESS_PROGRAM, args->loc);
      break;

    case ZSTD_OPTION:
      set_use_compress_program_option (ZSTD_PROGRAM, args->loc);
      break;

    case ATIME_PRESERVE_OPTION:
      atime_preserve_option =
	(arg
	 ? XARGMATCH ("--atime-preserve", arg,
		      atime_preserve_args, atime_preserve_types)
	 : replace_atime_preserve);
      if (! O_NOATIME && atime_preserve_option == system_atime_preserve)
	paxfatal (0, _("--atime-preserve='system' is not supported"
		       " on this platform"));
      break;

    case CHECK_DEVICE_OPTION:
      check_device_option = true;
      break;

    case NO_CHECK_DEVICE_OPTION:
      check_device_option = false;
      break;

    case CHECKPOINT_OPTION:
      if (arg)
	{
	  char *p;

	  if (*arg == '.')
	    {
	      checkpoint_compile_action (".");
	      arg++;
	    }
	  checkpoint_option = stoint (arg, &p, NULL, 0, INTMAX_MAX);
	  if (*p | (checkpoint_option <= 0))
	    paxfatal (0, _("invalid --checkpoint value"));
	}
      else
	checkpoint_option = DEFAULT_CHECKPOINT;
      break;

    case CHECKPOINT_ACTION_OPTION:
      checkpoint_compile_action (arg);
      break;

    case BACKUP_OPTION:
      backup_option = true;
      if (arg)
	args->version_control_string = arg;
      break;

    case DELAY_DIRECTORY_RESTORE_OPTION:
      delay_directory_restore_option = true;
      break;

    case NO_DELAY_DIRECTORY_RESTORE_OPTION:
      delay_directory_restore_option = false;
      break;

    case DELETE_OPTION:
      set_subcommand_option (DELETE_SUBCOMMAND);
      break;

    case FORCE_LOCAL_OPTION:
      force_local_option = true;
      break;

    case 'H':
      set_archive_format (arg);
      break;

    case INDEX_FILE_OPTION:
      index_file_name = arg;
      break;

    case IGNORE_COMMAND_ERROR_OPTION:
      ignore_command_error_option = true;
      break;

    case IGNORE_FAILED_READ_OPTION:
      ignore_failed_read_option = true;
      break;

    case KEEP_DIRECTORY_SYMLINK_OPTION:
      keep_directory_symlink_option = true;
      break;

    case KEEP_NEWER_FILES_OPTION:
      set_old_files_option (KEEP_NEWER_FILES, args->loc);
      break;

    case GROUP_OPTION:
      {
	uintmax_t u = parse_owner_group (arg, TYPE_MAXIMUM (gid_t),
					 &group_name_option);
	if (u == UINTMAX_MAX)
	  {
	    group_option = -1;
	    if (group_name_option)
	      gname_to_gid (group_name_option, &group_option);
	  }
	else
	  group_option = u;
      }
      break;

    case GROUP_MAP_OPTION:
      group_map_read (arg);
      break;

    case MODE_OPTION:
      mode_option = mode_compile (arg);
      if (!mode_option)
	paxfatal (0, _("Invalid mode given on option"));
      initial_umask = umask (0);
      umask (initial_umask);
      break;

    case NO_IGNORE_COMMAND_ERROR_OPTION:
      ignore_command_error_option = false;
      break;

    case NO_OVERWRITE_DIR_OPTION:
      set_old_files_option (NO_OVERWRITE_DIR_OLD_FILES, args->loc);
      break;

    case NO_QUOTE_CHARS_OPTION:
      for (;*arg; arg++)
	set_char_quoting (NULL, *arg, 0);
      break;

    case NUMERIC_OWNER_OPTION:
      numeric_owner_option = true;
      break;

    case OCCURRENCE_OPTION:
      optloc_save (OC_OCCURRENCE, args->loc);
      if (!arg)
	occurrence_option = 1;
      else
	{
	  char *end;
	  occurrence_option = stoint (arg, &end, NULL, 0, INTMAX_MAX);
	  if (*end)
	    paxfatal (0, "%s: %s", quotearg_colon (arg), _("Invalid number"));
	}
      break;

    case OLD_ARCHIVE_OPTION:
      set_archive_format ("v7");
      break;

    case OVERWRITE_DIR_OPTION:
      set_old_files_option (DEFAULT_OLD_FILES, args->loc);
      break;

    case OVERWRITE_OPTION:
      set_old_files_option (OVERWRITE_OLD_FILES, args->loc);
      break;

    case OWNER_OPTION:
      {
	uintmax_t u = parse_owner_group (arg, TYPE_MAXIMUM (uid_t),
					 &owner_name_option);
	if (u == UINTMAX_MAX)
	  {
	    owner_option = -1;
	    if (owner_name_option)
	      uname_to_uid (owner_name_option, &owner_option);
	  }
	else
	  owner_option = u;
      }
      break;

    case OWNER_MAP_OPTION:
      owner_map_read (arg);
      break;

    case QUOTE_CHARS_OPTION:
      for (;*arg; arg++)
	set_char_quoting (NULL, *arg, 1);
      break;

    case QUOTING_STYLE_OPTION:
      tar_set_quoting_style (arg);
      break;

    case PAX_OPTION:
      {
	char *tmp = expand_pax_option (args, arg);
	args->pax_option = true;
	xheader_set_option (tmp);
	free (tmp);
      }
      break;

    case POSIX_OPTION:
      set_archive_format ("posix");
      break;

    case RECORD_SIZE_OPTION:
      {
	uintmax_t u;

	if (! (xstrtoumax (arg, NULL, 10, &u, TAR_SIZE_SUFFIXES) == LONGINT_OK
	       && !ckd_add (&record_size, u, 0)
	       && record_size <= min (SSIZE_MAX, SIZE_MAX)))
	  paxusage ("%s: %s", quotearg_colon (arg), _("Invalid record size"));
	if (record_size % BLOCKSIZE != 0)
	  paxusage (_("Record size must be a multiple of %d."), BLOCKSIZE);
	blocking_factor = record_size >> LG_BLOCKSIZE;
      }
      break;

    case RECURSIVE_UNLINK_OPTION:
      recursive_unlink_option = true;
      break;

    case REMOVE_FILES_OPTION:
      remove_files_option = true;
      break;

    case RESTRICT_OPTION:
      restrict_option = true;
      break;

    case RMT_COMMAND_OPTION:
      rmt_command = arg;
      break;

    case RSH_COMMAND_OPTION:
      rsh_command_option = arg;
      break;

    case SHOW_DEFAULTS_OPTION:
      {
	char *s = format_default_settings ();
	printf ("%s\n", s);
	close_stdout ();
	free (s);
	exit (0);
      }

    case SHOW_SNAPSHOT_FIELD_RANGES_OPTION:
      show_snapshot_field_ranges ();
      close_stdout ();
      exit (0);

    case STRIP_COMPONENTS_OPTION:
      {
	char *end;
	strip_name_components = stoint (arg, &end, NULL, 0, IDX_MAX);
	if (*end)
	  paxusage ("%s: %s", quotearg_colon (arg),
		    _("Invalid number of elements"));
      }
      break;

    case SHOW_OMITTED_DIRS_OPTION:
      show_omitted_dirs_option = true;
      break;

    case SHOW_TRANSFORMED_NAMES_OPTION:
      show_transformed_names_option = true;
      break;

    case SORT_OPTION:
      savedir_sort_order = XARGMATCH ("--sort", arg,
				      sort_mode_arg, sort_mode_flag);
      break;

    case SUFFIX_OPTION:
      backup_option = true;
      args->backup_suffix_string = arg;
      break;

    case TO_COMMAND_OPTION:
      if (to_command_option)
	paxusage (_("Only one --to-command option allowed"));
      to_command_option = arg;
      break;

    case TOTALS_OPTION:
      if (arg)
	set_stat_signal (arg);
      else
	totals_option = true;
      break;

    case 'I':
      set_use_compress_program_option (arg, args->loc);
      break;

    case VOLNO_FILE_OPTION:
      volno_file_option = arg;
      break;

    case NO_SAME_OWNER_OPTION:
      same_owner_option = -1;
      break;

    case NO_SAME_PERMISSIONS_OPTION:
      same_permissions_option = -1;
      break;

    case ACLS_OPTION:
      set_archive_format ("posix");
      acls_option = 1;
      break;

    case NO_ACLS_OPTION:
      acls_option = -1;
      break;

    case SELINUX_CONTEXT_OPTION:
      set_archive_format ("posix");
      selinux_context_option = 1;
      break;

    case NO_SELINUX_CONTEXT_OPTION:
      selinux_context_option = -1;
      break;

    case XATTR_OPTION:
      set_xattr_option (true);
      break;

    case NO_XATTR_OPTION:
      set_xattr_option (false);
      break;

    case XATTR_INCLUDE:
    case XATTR_EXCLUDE:
      set_xattr_option (true);
      xattrs_mask_add (arg, (key == XATTR_INCLUDE));
      break;

    case SAME_OWNER_OPTION:
      same_owner_option = 1;
      break;

    case ARGP_KEY_ERROR:
      if (args->loc->source == OPTS_FILE)
	error (0, 0, _("%s:%jd: location of the error"), args->loc->name,
	       args->loc->line);
      else if (args->loc->source == OPTS_ENVIRON)
	error (0, 0, _("error parsing %s"), args->loc->name);
      exit (EX_USAGE);

    default:
      return ARGP_ERR_UNKNOWN;
    }
  return 0;
}

static struct argp_child argp_children[] = {
  { &names_argp, 0, NULL, GRID_FILE_NAME },
  { NULL }
};

static struct argp argp = {
  options,
  parse_opt,
  N_("[FILE]..."),
  doc,
  argp_children,
  tar_help_filter,
  NULL
};

void
usage (int status)
{
  argp_help (&argp, stderr, ARGP_HELP_SEE, (char *) program_name);
  close_stdout ();
  exit (status);
}

/* Parse the options for tar.  */

static struct argp_option const *
find_argp_option_key (struct argp_option const *o, char key)
{
  for (;
       !(o->name == NULL
	 && o->key == 0
	 && o->arg == 0
	 && o->flags == 0
	 && o->doc == NULL); o++)
    if (o->key == key)
      return o;
  return NULL;
}

static struct argp_option const *
find_argp_option (struct argp *ap, char key)
{
  struct argp_option const *p = NULL;
  struct argp_child const *child;

  p = find_argp_option_key (ap->options, key);
  if (!p && ap->children)
    {
      for (child = ap->children; child->argp; child++)
	{
	  p = find_argp_option_key (child->argp->options, key);
	  if (p)
	    break;
	}
    }
  return p;
}

static const char *tar_authors[] = {
  "John Gilmore",
  "Jay Fenlason",
  NULL
};

/* Subcommand classes */
enum subcommand_class
  {
    SUBCL_READ   = 1 << 0, /* Reads from the archive.  */
    SUBCL_WRITE  = 1 << 1, /* Writes to the archive.  */
    SUBCL_UPDATE = 1 << 2, /* Updates existing archive.  */
    SUBCL_TEST   = 1 << 3, /* Tests archive header or meta-info.  */
    SUBCL_OCCUR  = 1 << 4, /* Allows the use of the occurrence option.  */
  };

static char const subcommand_class[] = {
  [UNKNOWN_SUBCOMMAND	] = 0,
  [APPEND_SUBCOMMAND	] = SUBCL_WRITE | SUBCL_UPDATE,
  [CAT_SUBCOMMAND	] = SUBCL_WRITE,
  [CREATE_SUBCOMMAND	] = SUBCL_WRITE,
  [DELETE_SUBCOMMAND	] = SUBCL_WRITE | SUBCL_UPDATE | SUBCL_OCCUR,
  [DIFF_SUBCOMMAND	] = SUBCL_READ | SUBCL_OCCUR,
  [EXTRACT_SUBCOMMAND	] = SUBCL_READ | SUBCL_OCCUR,
  [LIST_SUBCOMMAND	] = SUBCL_READ | SUBCL_OCCUR,
  [UPDATE_SUBCOMMAND	] = SUBCL_WRITE | SUBCL_UPDATE,
  [TEST_LABEL_SUBCOMMAND] = SUBCL_TEST
};

/* Is subcommand_option in class(es) f?  */
static bool
is_subcommand_class (enum subcommand_class f)
{
  return subcommand_class[subcommand_option] & f;
}

void
more_options (int argc, char **argv, struct option_locus *loc)
{
  struct tar_args args = { .loc = loc };
  argp_parse (&names_argp, argc, argv, ARGP_IN_ORDER|ARGP_NO_EXIT|ARGP_NO_ERRS,
	      NULL, &args);
}

/* Are there file names in the list?  */
static bool
name_more_files (void)
{
  return filename_args != FILES_NONE;
}

static void
parse_default_options (struct tar_args *args)
{
  char *opts = getenv ("TAR_OPTIONS");
  struct wordsplit ws;
  struct option_locus loc = { OPTS_ENVIRON, "TAR_OPTIONS", 0, 0 };
  struct option_locus *save_loc_ptr;

  if (!opts)
    return;

  ws.ws_offs = 1;
  if (wordsplit (opts, &ws, WRDSF_DEFFLAGS | WRDSF_DOOFFS) != WRDSE_OK)
    paxfatal (0, _("cannot split TAR_OPTIONS: %s"), wordsplit_strerror (&ws));
  if (ws.ws_wordc)
    {
      int idx;
      ws.ws_wordv[0] = (char *) program_name;
      save_loc_ptr = args->loc;
      args->loc = &loc;
      int argc;
      if (ckd_add (&argc, ws.ws_offs, ws.ws_wordc))
	paxfatal (0, _("too many options"));
      if (argp_parse (&argp, argc, ws.ws_wordv,
		      ARGP_IN_ORDER | ARGP_NO_EXIT, &idx, args))
	abort (); /* shouldn't happen */
      args->loc = save_loc_ptr;
      if (name_more_files ())
	paxusage (_("non-option arguments in %s"), loc.name);
      /* Don't free consumed words */
      ws.ws_wordc = 0;
    }
  wordsplit_free (&ws);
}

static void
decode_options (int argc, char **argv)
{
  int idx;
  struct option_locus loc = { .source = OPTS_COMMAND_LINE };
  struct tar_args args = { .loc = &loc };

  argp_version_setup ("tar", tar_authors);

  /* Set some default option values.  */
  args.backup_suffix_string = getenv ("SIMPLE_BACKUP_SUFFIX");

  posixly_correct = getenv ("POSIXLY_CORRECT") != NULL;

  subcommand_option = UNKNOWN_SUBCOMMAND;
  archive_format = DEFAULT_FORMAT;
  blocking_factor = DEFAULT_BLOCKING;
  record_size = DEFAULT_BLOCKING * BLOCKSIZE;
  excluded = new_exclude ();
  hole_detection = HOLE_DETECTION_DEFAULT;

  newer_mtime_option.tv_sec = TYPE_MINIMUM (time_t);
  newer_mtime_option.tv_nsec = -1;
  mtime_option.tv_sec = TYPE_MINIMUM (time_t);
  mtime_option.tv_nsec = -1;
  recursion_option = FNM_LEADING_DIR;
  unquote_option = true;
  tar_sparse_major = 1;
  tar_sparse_minor = 0;

  savedir_sort_order = SAVEDIR_SORT_NONE;

  owner_option = -1; owner_name_option = NULL;
  group_option = -1; group_name_option = NULL;

  check_device_option = true;

  incremental_level = -1;

  seek_option = -1;

  /* Convert old-style tar call by exploding option element and rearranging
     options accordingly.  */

  if (argc > 1 && argv[1][0] != '-')
    {
      int new_argc;		/* argc value for rearranged arguments */
      char **new_argv;		/* argv value for rearranged arguments */
      char *const *in;		/* cursor into original argv */
      char **out;		/* cursor into rearranged argv */
      char buffer[3];		/* constructed option buffer */

      /* Initialize a constructed option.  */

      buffer[0] = '-';
      buffer[2] = '\0';

      /* Allocate a new argument array, and copy program name in it.  */

      idx_t new_arg_slots;
      if (ckd_add (&new_arg_slots, argc, strlen (argv[1]))
	  || ckd_sub (&new_argc, new_arg_slots, 1))
	xalloc_die ();
      new_argv = xinmalloc (new_arg_slots, sizeof *new_argv);
      in = argv;
      out = new_argv;
      *out++ = *in++;

      /* Copy each old letter option as a separate option, and have the
	 corresponding argument moved next to it.  */

      for (char const *letter = *in++; *letter; letter++)
	{
	  struct argp_option const *opt;

	  buffer[1] = *letter;
	  *out++ = xstrdup (buffer);
	  opt = find_argp_option (&argp, *letter);
	  if (opt && opt->arg)
	    {
	      if (! (in < argv + argc))
		paxusage (_("Old option '%c' requires an argument."), *letter);
	      *out++ = *in++;
	    }
	}

      /* Copy all remaining options.  */

      while (in < argv + argc)
	*out++ = *in++;
      *out = 0;

      /* Replace the old option list by the new one.  */

      argc = new_argc;
      argv = new_argv;
    }

  /* Parse all options and non-options as they appear.  */
  parse_default_options (&args);

  if (argp_parse (&argp, argc, argv, ARGP_IN_ORDER, &idx, &args))
    exit (TAREXIT_FAILURE);

  /* Special handling for 'o' option:

     GNU tar used to say "output old format".
     UNIX98 tar says don't chown files after extracting (we use
     "--no-same-owner" for this).

     The old GNU tar semantics is retained when used with --create
     option, otherwise UNIX98 semantics is assumed */

  if (args.o_option)
    {
      if (subcommand_option == CREATE_SUBCOMMAND)
	{
	  /* GNU Tar <= 1.13 compatibility */
	  set_archive_format ("v7");
	}
      else
	{
	  /* UNIX98 compatibility */
	  same_owner_option = -1;
	}
    }

  /* Handle operands after any "--" argument.  */
  for (; idx < argc; idx++)
    name_add_name (argv[idx]);

  /* Derive option values and check option consistency.  */

  if (archive_format == DEFAULT_FORMAT)
    {
      if (args.pax_option)
	archive_format = POSIX_FORMAT;
      else
	archive_format = DEFAULT_ARCHIVE_FORMAT;
    }

  if ((volume_label_option && subcommand_option == CREATE_SUBCOMMAND)
      || incremental_option
      || multi_volume_option
      || sparse_option)
    assert_format (format_mask (OLDGNU_FORMAT)
		   | format_mask (GNU_FORMAT)
		   | format_mask (POSIX_FORMAT));

  if (occurrence_option)
    {
      if (!name_more_files ())
	paxusage (_("--occurrence is meaningless without a file list"));
      if (!is_subcommand_class (SUBCL_OCCUR))
	{
	  if (option_set_in_cl (OC_OCCURRENCE))
	    option_conflict_error ("--occurrence",
				   subcommand_string (subcommand_option));
	  else
	    occurrence_option = 0;
	}
    }

  if (archive_names == 0)
    {
      /* If no archive file name given, try TAPE from the environment, or
	 else, DEFAULT_ARCHIVE from the configuration process.  */

      static char const *default_archive = DEFAULT_ARCHIVE;
      char const *tape = getenv ("TAPE");
      if (tape)
	default_archive = tape;
      archive_name_array = &default_archive;
      archive_names = 1;
    }

  /* Allow multiple archives only with '-M'.  */

  if (archive_names > 1 && !multi_volume_option)
    paxusage (_("Multiple archive files require '-M' option"));

  if (listed_incremental_option
      && time_option_initialized (newer_mtime_option))
    {
      struct option_locus *listed_loc = optloc_lookup (OC_LISTED_INCREMENTAL);
      struct option_locus *newer_loc = optloc_lookup (OC_NEWER);
      if (optloc_eq (listed_loc, newer_loc))
	option_conflict_error ("--listed-incremental", "--newer");
      else if (listed_loc->source == OPTS_COMMAND_LINE)
	listed_incremental_option = NULL;
      else
	memset (&newer_mtime_option, 0, sizeof (newer_mtime_option));
    }

  if (0 <= incremental_level && !listed_incremental_option)
    paxwarn (0, _("--level is meaningless without --listed-incremental"));

  if (volume_label_option)
    {
      if (archive_format == GNU_FORMAT || archive_format == OLDGNU_FORMAT)
	{
	  int volume_label_max_len =
	    (sizeof current_header->header.name
	     - 1 /* for trailing '\0' */
	     - (multi_volume_option
		? (sizeof " Volume "
		   - 1 /* for null at end of " Volume " */
		   + INT_STRLEN_BOUND (int) /* for volume number */
		   - 1 /* for sign, as 0 <= volno */)
		: 0));
	  if (volume_label_max_len < strlen (volume_label_option))
	    paxusage (_("%s: Volume label length exceeds %d bytes"),
		      quotearg_colon (volume_label_option),
		      volume_label_max_len);
	}
      /* else FIXME
	 Label length in PAX format is limited by the volume size. */
    }

  if (verify_option)
    {
      if (multi_volume_option)
	paxusage (_("Cannot verify multi-volume archives"));
      if (use_compress_program_option)
	paxusage (_("Cannot verify compressed archives"));
      if (!is_subcommand_class (SUBCL_WRITE))
	{
	  if (option_set_in_cl (OC_VERIFY))
	    option_conflict_error ("--verify",
				   subcommand_string (subcommand_option));
	  else
	    verify_option = false;
	}
    }

  if (use_compress_program_option)
    {
      if (multi_volume_option)
	paxusage (_("Cannot use multi-volume compressed archives"));
      if (is_subcommand_class (SUBCL_UPDATE))
	paxusage (_("Cannot update compressed archives"));
      if (subcommand_option == CAT_SUBCOMMAND)
	paxusage (_("Cannot concatenate compressed archives"));
    }

  if (set_mtime_command)
    {
      if (set_mtime_option != USE_FILE_MTIME)
	paxusage (_("--mtime conflicts with --set-mtime-command"));
      set_mtime_option = COMMAND_MTIME;
    }
  else if (set_mtime_option == CLAMP_MTIME)
    {
      if (!time_option_initialized (mtime_option))
	paxusage (_("--clamp-mtime needs a date specified using --mtime"));
    }

  /* It is no harm to use --pax-option on non-pax archives in archive
     reading mode. It may even be useful, since it allows to override
     file attributes from tar headers. Therefore I allow such usage.
     --gray */
  if (args.pax_option
      && archive_format != POSIX_FORMAT
      && !is_subcommand_class (SUBCL_READ))
    paxusage (_("--pax-option can be used only on POSIX archives"));

  /* star creates non-POSIX typed archives with xattr support, so allow the
     extra headers when reading */
  if ((acls_option > 0)
      && archive_format != POSIX_FORMAT
      && !is_subcommand_class (SUBCL_READ))
    paxusage (_("--acls can be used only on POSIX archives"));

  if ((selinux_context_option > 0)
      && archive_format != POSIX_FORMAT
      && !is_subcommand_class (SUBCL_READ))
    paxusage (_("--selinux can be used only on POSIX archives"));

  if (xattrs_option
      && archive_format != POSIX_FORMAT
      && !is_subcommand_class (SUBCL_READ))
    paxusage (_("--xattrs can be used only on POSIX archives"));

  if (starting_file_option && !is_subcommand_class (SUBCL_READ))
    {
      if (option_set_in_cl (OC_STARTING_FILE))
	option_conflict_error ("--starting-file",
			       subcommand_string (subcommand_option));
      else
	starting_file_option = false;
    }

  if (same_order_option && !is_subcommand_class (SUBCL_READ))
    {
      if (option_set_in_cl (OC_SAME_ORDER))
	option_conflict_error ("--same-order",
			       subcommand_string (subcommand_option));
      else
	same_order_option = false;
    }

  if (one_top_level_option)
    {
      char *base;

      if (absolute_names_option)
	{
	  struct option_locus *one_top_level_loc =
	    optloc_lookup (OC_ONE_TOP_LEVEL);
	  struct option_locus *absolute_names_loc =
	    optloc_lookup (OC_ABSOLUTE_NAMES);

	  if (optloc_eq (one_top_level_loc, absolute_names_loc))
	    option_conflict_error ("--one-top-level", "--absolute-names");
	  else if (one_top_level_loc->source == OPTS_COMMAND_LINE)
	    absolute_names_option = false;
	  else
	    one_top_level_option = false;
	}

      if (one_top_level_option && !one_top_level_dir)
	{
	  /* If the user wants to guarantee that everything is under one
	     directory, determine its name now and let it be created later.  */
	  base = base_name (archive_name_array[0]);
	  one_top_level_dir = strip_compression_suffix (base);
	  free (base);

	  if (!one_top_level_dir)
	    paxusage (_("Cannot deduce top-level directory name; "
			"please set it explicitly with --one-top-level=DIR"));
	}
    }

  /* If ready to unlink hierarchies, so we are for simpler files.  */
  if (recursive_unlink_option)
    old_files_option = UNLINK_FIRST_OLD_FILES;

  /* Flags for accessing files to be read from or copied into.  POSIX says
     O_NONBLOCK has unspecified effect on most types of files, but in
     practice it never harms and sometimes helps.  */
  {
    int base_open_flags =
      (O_BINARY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK
       | (dereference_option ? 0 : O_NOFOLLOW)
       | (atime_preserve_option == system_atime_preserve ? O_NOATIME : 0));
    open_read_flags = O_RDONLY | base_open_flags;
    open_searchdir_flags = O_SEARCH | O_DIRECTORY | base_open_flags;
  }
  fstatat_flags = dereference_option ? 0 : AT_SYMLINK_NOFOLLOW;

  if (subcommand_option == TEST_LABEL_SUBCOMMAND)
    {
      /* --test-label is silent if the user has specified the label name to
	 compare against. */
      if (!name_more_files ())
	verbose_option += verbose_option <= 2;
    }
  else if (utc_option)
    verbose_option = 2;

  if (tape_length_option && tape_length_option < record_size)
    paxusage (_("Volume length cannot be less than record size"));

  if (same_order_option && listed_incremental_option)
    {
      struct option_locus *preserve_order_loc = optloc_lookup (OC_SAME_ORDER);
      struct option_locus *listed_incremental_loc =
	optloc_lookup (OC_LISTED_INCREMENTAL);

      if (optloc_eq (preserve_order_loc, listed_incremental_loc))
	option_conflict_error ("--preserve-order", "--listed-incremental");
      else if (preserve_order_loc->source == OPTS_COMMAND_LINE)
	listed_incremental_option = NULL;
      else
	same_order_option = false;
    }

  /* Forbid using -c with no input files whatsoever.  Check that '-f -',
     explicit or implied, is used correctly.  */

  switch (subcommand_option)
    {
    case CREATE_SUBCOMMAND:
      if (!name_more_files ())
	paxusage (_("Cowardly refusing to create an empty archive"));
      if (args.compress_autodetect && archive_names
	  && strcmp (archive_name_array[0], "-"))
	set_compression_program_by_suffix (archive_name_array[0],
					   use_compress_program_option,
					   true);
      break;

    case EXTRACT_SUBCOMMAND:
    case LIST_SUBCOMMAND:
    case DIFF_SUBCOMMAND:
    case TEST_LABEL_SUBCOMMAND:
      for (archive_name_cursor = archive_name_array;
	   archive_name_cursor < archive_name_array + archive_names;
	   archive_name_cursor++)
	if (strcmp (*archive_name_cursor, "-") == 0)
	  request_stdin ("-f");
      break;

    case CAT_SUBCOMMAND:
    case UPDATE_SUBCOMMAND:
    case APPEND_SUBCOMMAND:
      for (archive_name_cursor = archive_name_array;
	   archive_name_cursor < archive_name_array + archive_names;
	   archive_name_cursor++)
	if (strcmp (*archive_name_cursor, "-") == 0)
	  paxusage (_("Options '-Aru' are incompatible with '-f -'"));

    default:
      break;
    }

  /* Initialize stdlis */
  if (index_file_name)
    {
      stdlis = fopen (index_file_name, "w");
      if (! stdlis)
	open_fatal (index_file_name);
    }
  else
    stdlis = to_stdout_option ? stderr : stdout;

  archive_name_cursor = archive_name_array;

  /* Prepare for generating backup names.  */

  if (args.backup_suffix_string)
    simple_backup_suffix = xstrdup (args.backup_suffix_string);

  if (backup_option)
    {
      backup_type = xget_version ("--backup", args.version_control_string);
      /* No backup is needed either if explicitly disabled or if
	 the extracted files are not being written to disk. */
      if (backup_type == no_backups || to_stdout_option || to_command_option)
	backup_option = false;
    }

  checkpoint_finish_compile ();

  report_textual_dates (&args);
}

#ifdef ENABLE_ERROR_PRINT_PROGNAME
/* The error() function from glibc correctly prefixes each message it
   prints with program_name as set by set_program_name. However, its
   replacement from gnulib, which is linked in on systems where this
   function is not available, prints the name returned by getprogname()
   instead. Due to this messages output by tar subprocess (which sets its
   program name to 'tar (child)') become indiscernible from those printed
   by the main process. In particular, this breaks the remfiles01.at and
   remfiles02.at test cases.

   To avoid this, on such systems the following helper function is used
   to print proper program name. Its address is assigned to the
   error_print_progname variable, which error() then uses instead of
   printing getprogname() result.
 */
static void
tar_print_progname (void)
{
  fprintf (stderr, "%s: ", program_name);
}
#endif

/* Tar proper.  */

/* Main routine for tar.  */
int
main (int argc, char **argv)
{
  set_start_time ();
  set_program_name (argv[0]);
#ifdef ENABLE_ERROR_PRINT_PROGNAME
  error_print_progname = tar_print_progname;
#endif
  setlocale (LC_ALL, "");
  bindtextdomain (PACKAGE, LOCALEDIR);
  textdomain (PACKAGE);

  exit_failure = TAREXIT_FAILURE;
  exit_status = TAREXIT_SUCCESS;
  error_hook = checkpoint_flush_actions;

  set_quoting_style (0, DEFAULT_QUOTING_STYLE);

  close_stdout_set_file_name (_("stdout"));

  int err = stdopen ();
  if (err != 0)
    paxfatal (err, _("failed to assert availability"
		     " of the standard file descriptors"));

  /* System V fork+wait does not work if SIGCHLD is ignored.  */
  signal (SIGCHLD, SIG_DFL);

  /* Try to disable the ability to unlink a directory.  */
  priv_set_remove_linkdir ();

  /* Decode options.  */

  decode_options (argc, argv);

  name_init ();

  /* Main command execution.  */

  if (volno_file_option)
    init_volume_number ();

  switch (subcommand_option)
    {
    case UNKNOWN_SUBCOMMAND:
      paxusage (_("You must specify one of the '-Acdtrux',"
		  " '--delete' or '--test-label' options"));

    case CAT_SUBCOMMAND:
    case UPDATE_SUBCOMMAND:
    case APPEND_SUBCOMMAND:
      update_archive ();
      break;

    case DELETE_SUBCOMMAND:
      delete_archive_members ();
      break;

    case CREATE_SUBCOMMAND:
      create_archive ();
      break;

    case EXTRACT_SUBCOMMAND:
      extr_init ();
      read_and (extract_archive);

      /* FIXME: should extract_finish () even if an ordinary signal is
	 received.  */
      extract_finish ();

      break;

    case LIST_SUBCOMMAND:
      read_and (list_archive);
      break;

    case DIFF_SUBCOMMAND:
      diff_init ();
      read_and (diff_archive);
      break;

    case TEST_LABEL_SUBCOMMAND:
      test_archive_label ();
    }

  checkpoint_finish ();

  if (totals_option)
    print_total_stats ();

  if (check_links_option)
    check_links ();

  if (volno_file_option)
    closeout_volume_number ();

  if (exit_status == TAREXIT_FAILURE)
    error (0, 0, _("Exiting with failure status due to previous errors"));

  if (stdlis == stdout)
    close_stdout ();
  else if (ferror (stderr) || fclose (stderr) < 0)
    set_exit_status (TAREXIT_FAILURE);

  return exit_status;
}

void
tar_stat_init (struct tar_stat_info *st)
{
  memset (st, 0, sizeof (*st));
}

/* Close the stream or file descriptor associated with ST, and remove
   all traces of it from ST.  Return true if successful, false (with a
   diagnostic) otherwise.  */
bool
tar_stat_close (struct tar_stat_info *st)
{
  int status = (st->dirstream ? closedir (st->dirstream)
		: 0 < st->fd ? close (st->fd)
		: 0);
  st->dirstream = 0;
  st->fd = 0;

  if (status == 0)
    return true;
  else
    {
      close_diag (st->orig_file_name);
      return false;
    }
}

void
tar_stat_destroy (struct tar_stat_info *st)
{
  tar_stat_close (st);
  xattr_map_free (&st->xattr_map);
  free (st->orig_file_name);
  free (st->file_name);
  free (st->link_name);
  free (st->uname);
  free (st->gname);
  free (st->cntx_name);
  free (st->acls_a_ptr);
  free (st->acls_d_ptr);
  free (st->sparse_map);
  free (st->dumpdir);
  xheader_destroy (&st->xhdr);
  info_free_exclist (st);
  memset (st, 0, sizeof (*st));
}

/* Same as timespec_cmp, but ignore nanoseconds if current archive
   format does not provide sufficient resolution.  */
int
tar_timespec_cmp (struct timespec a, struct timespec b)
{
  /* Format mask for all available formats that support nanosecond
     timestamp resolution. */
  int ns_precision_format_mask = format_mask (POSIX_FORMAT);

  if (!(format_mask (current_format) & ns_precision_format_mask))
    a.tv_nsec = b.tv_nsec = 0;
  return timespec_cmp (a, b);
}

/* Set tar exit status to VAL, unless it is already indicating
   a more serious condition. This relies on the fact that the
   values of TAREXIT_ constants are ranged by severity. */
void
set_exit_status (int val)
{
  if (val > exit_status)
    exit_status = val;
}
