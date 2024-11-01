/* Common declarations for the tar program.

   Copyright 1988-2024 Free Software Foundation, Inc.

   This file is part of GNU tar.

   GNU tar is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   GNU tar is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

/* Declare the GNU tar archive format.  */
#include "tar.h"

/* Some constants from POSIX are given names.  */
enum
  {
    NAME_FIELD_SIZE = 100,
    PREFIX_FIELD_SIZE = 155,
    UNAME_FIELD_SIZE = 32,
    GNAME_FIELD_SIZE = 32
  };


/* Some various global definitions.  */

enum
  {
    TAREXIT_SUCCESS = PAXEXIT_SUCCESS,
    TAREXIT_DIFFERS = PAXEXIT_DIFFERS,
    TAREXIT_FAILURE = PAXEXIT_FAILURE
  };


#include "arith.h"

#define obstack_chunk_alloc xmalloc
#define obstack_chunk_free free
#include <obstack.h>

#include <attribute.h>
#include <backupfile.h>
#include <exclude.h>
#include <full-read.h>
#include <full-write.h>
#include <idx.h>
#include <intprops.h>
#include <inttostr.h>
#include <modechange.h>
#include <paxlib.h>
#include <progname.h>
#include <quote.h>
#include <safe-read.h>
#include <stat-time.h>
#include <timespec.h>
#include <verify.h>
#include <xvasprintf.h>

/* Log base 2 of common values.  */
enum { LG_8 = 3, LG_256 = 8 };

_GL_INLINE_HEADER_BEGIN
#ifndef COMMON_INLINE
# define COMMON_INLINE _GL_INLINE
#endif

/* Information gleaned from the command line.  */

/* Main command option.  */

enum subcommand
{
  UNKNOWN_SUBCOMMAND,		/* none of the following */
  APPEND_SUBCOMMAND,		/* -r */
  CAT_SUBCOMMAND,		/* -A */
  CREATE_SUBCOMMAND,		/* -c */
  DELETE_SUBCOMMAND,		/* -D */
  DIFF_SUBCOMMAND,		/* -d */
  EXTRACT_SUBCOMMAND,		/* -x */
  LIST_SUBCOMMAND,		/* -t */
  UPDATE_SUBCOMMAND,		/* -u */
  TEST_LABEL_SUBCOMMAND,        /* --test-label */
};

extern enum subcommand subcommand_option;

/* Selected format for output archive.  */
extern enum archive_format archive_format;

/* Size of each record, once in blocks, once in bytes.  Those two variables
   are always related, the second being BLOCKSIZE times the first.  They do
   not have _option in their name, even if their values is derived from
   option decoding, as these are especially important in tar.  */
extern idx_t blocking_factor;
extern idx_t record_size;

extern bool absolute_names_option;

/* Display file times in UTC */
extern bool utc_option;
/* Output file timestamps to the full resolution */
extern bool full_time_option;

/* This variable tells how to interpret newer_mtime_option, below.  If zero,
   files get archived if their mtime is not less than newer_mtime_option.
   If nonzero, files get archived if *either* their ctime or mtime is not less
   than newer_mtime_option.  */
extern int after_date_option;

enum atime_preserve
{
  no_atime_preserve,
  replace_atime_preserve,
  system_atime_preserve
};
extern enum atime_preserve atime_preserve_option;

extern bool backup_option;

/* Type of backups being made.  */
extern enum backup_type backup_type;

extern bool block_number_option;

extern intmax_t checkpoint_option;
enum { DEFAULT_CHECKPOINT = 10 };

/* Specified name of compression program, or "gzip" as implied by -z.  */
extern const char *use_compress_program_option;

extern bool dereference_option;
extern bool hard_dereference_option;

/* Patterns that match file names to be excluded.  */
extern struct exclude *excluded;

enum exclusion_tag_type
  {
    exclusion_tag_none,
     /* Exclude the directory contents, but preserve the directory
	itself and the exclusion tag file */
    exclusion_tag_contents,
    /* Exclude everything below the directory, preserving the directory
       itself */
    exclusion_tag_under,
    /* Exclude entire directory  */
    exclusion_tag_all,
  };

/* Specified value to be put into tar file in place of stat () results, or
   just null and -1 if such an override should not take place.  */
extern char const *group_name_option;
extern gid_t group_option;

extern bool ignore_failed_read_option;

extern bool ignore_zeros_option;

extern bool incremental_option;

/* Specified name of script to run at end of each tape change.  */
extern const char *info_script_option;

extern bool interactive_option;

/* If nonzero, extract only Nth occurrence of each named file */
extern uintmax_t occurrence_option;

enum old_files
{
  DEFAULT_OLD_FILES,          /* default */
  NO_OVERWRITE_DIR_OLD_FILES, /* --no-overwrite-dir */
  OVERWRITE_OLD_FILES,        /* --overwrite */
  UNLINK_FIRST_OLD_FILES,     /* --unlink-first */
  KEEP_OLD_FILES,             /* --keep-old-files */
  SKIP_OLD_FILES,             /* --skip-old-files */
  KEEP_NEWER_FILES	      /* --keep-newer-files */
};
enum { MAX_OLD_FILES = KEEP_NEWER_FILES + 1 };
extern enum old_files old_files_option;

extern bool keep_directory_symlink_option;

/* Specified file name for incremental list.  */
extern const char *listed_incremental_option;
/* Incremental dump level: either -1, 0, or 1.  */
extern signed char incremental_level;
/* Check device numbers when doing incremental dumps. */
extern bool check_device_option;

/* Specified mode change string.  */
extern struct mode_change *mode_option;

/* Initial umask, if needed for mode change string.  */
extern mode_t initial_umask;

extern bool multi_volume_option;

/* Specified threshold date and time.  Files having an older time stamp
   do not get archived (also see after_date_option above).  */
extern struct timespec newer_mtime_option;

enum set_mtime_option_mode
{
  USE_FILE_MTIME,
  FORCE_MTIME,
  CLAMP_MTIME,
  COMMAND_MTIME,
};

/* Override actual mtime if set to FORCE_MTIME or CLAMP_MTIME */
extern enum set_mtime_option_mode set_mtime_option;
/* Value to use when forcing or clamping the mtime header field. */
extern struct timespec mtime_option;

/* Command to use to set mtime when archiving. */
extern char *set_mtime_command;

/* Format (as per strptime(3)) of the output of the above command.  If
   not set, parse_datetime will be used. */
extern char *set_mtime_format;

/* Return true if mtime_option or newer_mtime_option is initialized.  */
COMMON_INLINE bool
time_option_initialized (struct timespec opt)
{
  return 0 <= opt.tv_nsec;
}

/* Zero if there is no recursion, otherwise FNM_LEADING_DIR.  */
extern int recursion_option;

extern bool numeric_owner_option;

extern bool one_file_system_option;

/* Create a top-level directory for extracting based on the archive name.  */
extern bool one_top_level_option;
extern char *one_top_level_dir;

/* Specified value to be put into tar file in place of stat () results, or
   just null and -1 if such an override should not take place.  */
extern char const *owner_name_option;
extern uid_t owner_option;

extern bool recursive_unlink_option;

extern bool read_full_records_option;

extern bool remove_files_option;

/* Specified remote shell command.  */
extern const char *rsh_command_option;

extern bool same_order_option;

/* If positive, preserve ownership when extracting.  */
extern int same_owner_option;

/* If positive, preserve permissions when extracting.  */
extern int same_permissions_option;

/* If positive, save the SELinux context.  */
extern int selinux_context_option;

/* If positive, save the ACLs.  */
extern int acls_option;

/* If positive, save the user and root xattrs.  */
extern int xattrs_option;

/* When set, strip the given number of file name components from the file name
   before extracting */
extern idx_t strip_name_components;

extern bool show_omitted_dirs_option;

extern bool sparse_option;
extern intmax_t tar_sparse_major, tar_sparse_minor;

enum hole_detection_method
  {
    HOLE_DETECTION_DEFAULT,
    HOLE_DETECTION_RAW,
    HOLE_DETECTION_SEEK
  };

extern enum hole_detection_method hole_detection;

/* The first entry in names.c:namelist specifies the member name to
   start extracting from. Set by add_starting_file() upon seeing the
   -K option.
*/
extern bool starting_file_option;

/* Specified maximum byte length of each tape volume (multiple of 1024).  */
extern tarlong tape_length_option;

extern bool to_stdout_option;

extern bool totals_option;

extern bool touch_option;

extern char *to_command_option;
extern bool ignore_command_error_option;

/* Restrict some potentially harmful tar options */
extern bool restrict_option;

/* Count how many times the option has been set, multiple setting yields
   more verbose behavior.  Value 0 means no verbosity, 1 means file name
   only, 2 means file name and all attributes.  More than 2 is just like 2.  */
extern int verbose_option;

extern bool verify_option;

/* Specified name of file containing the volume number.  */
extern const char *volno_file_option;

/* Specified value or pattern.  */
extern const char *volume_label_option;

/* Other global variables.  */

/* Force POSIX-compliance */
extern bool posixly_correct;

/* List of tape drive names, number of such tape drives,
   and current cursor in list.  */
extern const char **archive_name_array;
extern idx_t archive_names;
extern const char **archive_name_cursor;

/* Output index file name.  */
extern char const *index_file_name;

/* Opaque structure for keeping directory meta-data */
struct directory;

/* Structure for keeping track of filenames and lists thereof.  */
struct name
  {
    struct name *next;          /* Link to the next element */
    struct name *prev;          /* Link to the previous element */

    char *name;                 /* File name or globbing pattern */
    size_t length;		/* cached strlen (name) */
    int matching_flags;         /* wildcard flags if name is a pattern */
    bool is_wildcard;           /* true if this is a wildcard pattern */
    bool cmdline;               /* true if this name was given in the
				   command line */

    idx_t change_dir;		/* Number of the directory to change to.
				   Set with the -C option. */
    uintmax_t found_count;	/* number of times a matching file has
				   been found */

    /* The following members are used for incremental dumps only,
       if this struct name represents a directory;
       see incremen.c */
    struct directory *directory;/* directory meta-data and contents */
    struct name *parent;        /* pointer to the parent hierarchy */
    struct name *child;         /* pointer to the first child */
    struct name *sibling;       /* pointer to the next sibling */
    char *caname;               /* canonical name */
  };

/* Flags for reading, searching, and fstatatting files.  */
extern int open_read_flags;
extern int open_searchdir_flags;
extern int fstatat_flags;

extern int seek_option;

/* Unquote filenames */
extern bool unquote_option;

extern int savedir_sort_order;

/* Show file or archive names after transformation.
   In particular, when creating archive in verbose mode, list member names
   as stored in the archive */
extern bool show_transformed_names_option;

/* Delay setting modification times and permissions of extracted directories
   until the end of extraction. This variable helps correctly restore directory
   timestamps from archives with an unusual member order. It is automatically
   set for incremental archives. */
extern bool delay_directory_restore_option;

/* Declarations for each module.  */

/* FIXME: compare.c should not directly handle the following variable,
   instead, this should be done in buffer.c only.  */

enum access_mode
{
  ACCESS_READ,
  ACCESS_WRITE,
  ACCESS_UPDATE
};
extern enum access_mode access_mode;

/* Module buffer.c.  */

/* File descriptor for archive file.  */
extern int archive;

/* Timestamps: */
extern struct timespec start_time;        /* when we started execution */
extern struct timespec volume_start_time; /* when the current volume was
					     opened*/

extern struct tar_stat_info current_stat_info;

/* Status of archive file, or all zeros if remote.  */
extern struct stat archive_stat;

/* true if archive if lseek should be used on the archive, 0 if it
   should not be used.  */
extern bool seekable_archive;

extern FILE *stdlis;
extern bool write_archive_to_stdout;
extern char *volume_label;
extern size_t volume_label_count;
extern char *continued_file_name;
extern uintmax_t continued_file_size;
extern uintmax_t continued_file_offset;
extern off_t records_written;
extern union block *record_start;
extern union block *record_end;
extern union block *current_block;
extern off_t records_read;

char *drop_volume_label_suffix (const char *label)
  _GL_ATTRIBUTE_MALLOC _GL_ATTRIBUTE_DEALLOC_FREE;

idx_t available_space_after (union block *pointer);
off_t current_block_ordinal (void);
void close_archive (void);
void closeout_volume_number (void);
double compute_duration_ns (void);
union block *find_next_block (void);
void flush_read (void);
void flush_write (void);
void flush_archive (void);
void init_volume_number (void);
void open_archive (enum access_mode mode);
void print_total_stats (void);
void reset_eof (void);
void set_next_block_after (union block *block);
void clear_read_error_count (void);
void xclose (int fd);
_Noreturn void archive_write_error (ssize_t status);
void archive_read_error (void);
off_t seek_archive (off_t size);
void set_start_time (void);

enum { TF_READ, TF_WRITE, TF_DELETED };
int format_total_stats (FILE *fp, char const *const *formats, int eor, int eol);
void print_total_stats (void);

void mv_begin_write (const char *file_name, off_t totsize, off_t sizeleft);

void mv_begin_read (struct tar_stat_info *st);
void mv_end (void);
void mv_size_left (off_t size);

void buffer_write_global_xheader (void);

const char *first_decompress_program (int *pstate);
const char *next_decompress_program (int *pstate);

/* Module create.c.  */

enum dump_status
  {
    dump_status_ok,
    dump_status_short,
    dump_status_fail,
    dump_status_not_implemented
  };

void add_exclusion_tag (const char *name, enum exclusion_tag_type type,
			bool (*predicate) (int));
bool cachedir_file_p (int fd);
char *get_directory_entries (struct tar_stat_info *st)
  _GL_ATTRIBUTE_MALLOC _GL_ATTRIBUTE_DEALLOC_FREE;

void create_archive (void);
void pad_archive (off_t size_left);
void dump_file (struct tar_stat_info *parent, char const *name,
		char const *fullname);
union block *start_header (struct tar_stat_info *st);
void finish_header (struct tar_stat_info *st, union block *header,
		    off_t block_ordinal);
void simple_finish_header (union block *header);
union block *write_extended (bool global, struct tar_stat_info *st,
			     union block *old_header);
union block *start_private_header (const char *name, idx_t size, time_t t);
void write_eot (void);
void check_links (void);
int subfile_open (struct tar_stat_info const *dir, char const *file, int flags);
void restore_parent_fd (struct tar_stat_info const *st);
void exclusion_tag_warning (const char *dirname, const char *tagname,
			    const char *message);
enum exclusion_tag_type check_exclusion_tags (struct tar_stat_info const *st,
					      const char **tag_file_name);

#define OFF_TO_CHARS(val, where) off_to_chars (val, where, sizeof (where))
#define TIME_TO_CHARS(val, where) time_to_chars (val, where, sizeof (where))

bool off_to_chars (off_t off, char *buf, idx_t size);
bool time_to_chars (time_t t, char *buf, idx_t size);

/* Module diffarch.c.  */

extern bool now_verifying;

void diff_archive (void);
void diff_init (void);
void verify_volume (void);

/* Module extract.c.  */

extern dev_t root_device;

void extr_init (void);
void extract_archive (void);
void extract_finish (void);
bool rename_directory (char *src, char *dst);

void remove_delayed_set_stat (const char *fname);

/* Module delete.c.  */

/* number of records skipped at the start of the archive.  */
extern off_t records_skipped;

void delete_archive_members (void);

/* Module incremen.c.  */

struct directory *scan_directory (struct tar_stat_info *st);
const char *directory_contents (struct directory *dir);
const char *safe_directory_contents (struct directory *dir);

void rebase_directory (struct directory *dir,
		       const char *samp, idx_t slen,
		       const char *repl, idx_t rlen);

void append_incremental_renames (struct directory *dir);
void show_snapshot_field_ranges (void);
void read_directory_file (void);
void write_directory_file (void);
void purge_directory (char const *directory_name);
void list_dumpdir (char *buffer, idx_t size);
void update_parent_directory (struct tar_stat_info *st);

idx_t dumpdir_size (const char *p);
bool is_dumpdir (struct tar_stat_info *stat_info);
void clear_directory_table (void);

/* Module list.c.  */

enum read_header
{
  HEADER_STILL_UNREAD,		/* for when read_header has not been called */
  HEADER_SUCCESS,		/* header successfully read and checksummed */
  HEADER_SUCCESS_EXTENDED,	/* likewise, but we got an extended header */
  HEADER_ZERO_BLOCK,		/* zero block where header expected */
  HEADER_END_OF_FILE,		/* true end of file while header expected */
  HEADER_FAILURE		/* ill-formed header, or bad checksum */
};

/* Operation mode for read_header: */

enum read_header_mode
{
  read_header_auto,             /* process extended headers automatically */
  read_header_x_raw,            /* return raw extended headers (return
				   HEADER_SUCCESS_EXTENDED) */
  read_header_x_global          /* when POSIX global extended header is read,
				   decode it and return
				   HEADER_SUCCESS_EXTENDED */
};
extern union block *current_header;
extern enum archive_format current_format;
extern union block *recent_long_name;
extern union block *recent_long_link;
extern idx_t recent_long_name_blocks;
extern idx_t recent_long_link_blocks;

void decode_header (union block *header, struct tar_stat_info *stat_info,
		    enum archive_format *format_pointer, int do_user_group);
void transform_stat_info (int typeflag, struct tar_stat_info *stat_info);
char const *tartime (struct timespec t, bool full_time);

#define OFF_FROM_HEADER(where) off_from_header (where, sizeof (where))
#define UINTMAX_FROM_HEADER(where) uintmax_from_header (where, sizeof (where))

off_t off_from_header (const char *buf, idx_t size);
uintmax_t uintmax_from_header (const char *buf, idx_t size);

void list_archive (void);
void test_archive_label (void);
void print_for_mkdir (char *dirname, mode_t mode);
void print_header (struct tar_stat_info *st, union block *blk,
	           off_t block_ordinal);
void read_and (void (*do_something) (void));
enum read_header read_header (union block **return_block,
			      struct tar_stat_info *info,
			      enum read_header_mode m);
enum read_header tar_checksum (union block *header, bool silent);
void skim_file (off_t size, bool must_copy);
void skip_member (void);
void skim_member (bool must_copy);

/* Module misc.c.  */

#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) < (b) ? (b) : (a))

char const *quote_n_colon (int n, char const *arg);
void assign_string_or_null (char **dest, const char *src)
  ATTRIBUTE_NONNULL ((1));
void assign_string (char **dest, const char *src) ATTRIBUTE_NONNULL ((1, 2));
void assign_null (char **dest) ATTRIBUTE_NONNULL ((1));
void assign_string_n (char **string, const char *value, idx_t n);
#define ASSIGN_STRING_N(s,v) assign_string_n (s, v, sizeof (v))
int unquote_string (char *str);
char *zap_slashes (char *name);
char *normalize_filename (idx_t, char const *);
void normalize_filename_x (char *name);
void replace_prefix (char **pname, const char *samp, idx_t slen,
		     const char *repl, idx_t rlen);
char *tar_savedir (const char *name, int must_exist);

typedef struct namebuf *namebuf_t;
namebuf_t namebuf_create (const char *dir);
void namebuf_free (namebuf_t buf);
char *namebuf_name (namebuf_t buf, const char *name);

const char *tar_dirname (void);

/* intmax (N) is like ((intmax_t) (N)) except without a cast so
   that it is an error if N is a pointer.  Similarly for uintmax.  */
COMMON_INLINE intmax_t
intmax (intmax_t n)
{
  return n;
}
COMMON_INLINE uintmax_t
uintmax (uintmax_t n)
{
  return n;
}
/* intmax should be used only with signed types, and uintmax for unsigned.
   To bypass this check parenthesize the function, e.g., (intmax) (n).  */
#define intmax(n) verify_expr (EXPR_SIGNED (n), (intmax) (n))
#define uintmax(n) verify_expr (!EXPR_SIGNED (n), (uintmax) (n))

/* Represent N using a signed integer I such that (uintmax_t) I == N.
   With a good optimizing compiler, this is equivalent to (intmax_t) i
   and requires zero machine instructions.  */
COMMON_INLINE intmax_t
represent_uintmax (uintmax_t n)
{
  static_assert (UINTMAX_MAX / 2 <= INTMAX_MAX);

  if (n <= INTMAX_MAX)
    return n;
  else
    {
      /* Avoid signed integer overflow on picky platforms.  */
      intmax_t nd = n - INTMAX_MIN;
      return nd + INTMAX_MIN;
    }
}

enum { UINTMAX_STRSIZE_BOUND = INT_BUFSIZE_BOUND (uintmax_t) };
enum { SYSINT_BUFSIZE =
	 max (UINTMAX_STRSIZE_BOUND, INT_BUFSIZE_BOUND (intmax_t)) };
char *sysinttostr (uintmax_t, intmax_t, uintmax_t, char buf[SYSINT_BUFSIZE]);
intmax_t stoint (char const *, char **, bool *, intmax_t, uintmax_t);
char *timetostr (time_t, char buf[SYSINT_BUFSIZE]);
void code_ns_fraction (int ns, char *p);
enum { BILLION = 1000000000, LOG10_BILLION = 9 };
enum { TIMESPEC_STRSIZE_BOUND =
         SYSINT_BUFSIZE + LOG10_BILLION + sizeof "." - 1 };
char const *code_timespec (struct timespec ts,
			   char tsbuf[TIMESPEC_STRSIZE_BOUND]);
struct timespec decode_timespec (char const *, char **, bool);

/* Return true if T does not represent an out-of-range or invalid value.  */
COMMON_INLINE bool
valid_timespec (struct timespec t)
{
  return 0 <= t.tv_nsec;
}

bool must_be_dot_or_slash (char const *);

enum remove_option
{
  ORDINARY_REMOVE_OPTION,
  RECURSIVE_REMOVE_OPTION,

  /* FIXME: The following value is never used. It seems to be intended
     as a placeholder for a hypothetical option that should instruct tar
     to recursively remove subdirectories in purge_directory(),
     as opposed to the functionality of --recursive-unlink
     (RECURSIVE_REMOVE_OPTION value), which removes them in
     prepare_to_extract() phase. However, with the addition of more
     meta-info to the incremental dumps, this should become unnecessary */
  WANT_DIRECTORY_REMOVE_OPTION
};
int remove_any_file (const char *file_name, enum remove_option option);
bool maybe_backup_file (const char *file_name, bool this_is_the_archive);
void undo_last_backup (void);

int deref_stat (char const *name, struct stat *buf);

idx_t blocking_read (int fd, void *buf, idx_t count);
idx_t blocking_write (int fd, void const *buf, idx_t count);

extern idx_t chdir_current;
extern int chdir_fd;
idx_t chdir_arg (char const *dir);
void chdir_do (idx_t dir);
idx_t chdir_count (void);

void close_diag (char const *name);
void open_diag (char const *name);
void read_diag_details (char const *name, off_t offset, idx_t size);
void readlink_diag (char const *name);
void savedir_diag (char const *name);
void seek_diag_details (char const *name, off_t offset);
void stat_diag (char const *name);
void file_removed_diag (const char *name, bool top_level,
			void (*diagfn) (char const *name));
_Noreturn void write_fatal (char const *name);

pid_t xfork (void);
void xpipe (int fd[2]);

int set_file_atime (int fd, int parentfd, char const *file,
		    struct timespec atime);

/* Module names.c.  */

enum files_count
  {
    FILES_NONE,
    FILES_ONE,
    FILES_MANY
  };
extern enum files_count filename_args;

/* Declare only if argp.h has already been included,
   as this declaration needs struct argp.  */
#ifdef ARGP_ERR_UNKNOWN
extern struct argp names_argp;
#endif

extern struct name *gnu_list_name;

void gid_to_gname (gid_t gid, char **gname);
int gname_to_gid (char const *gname, gid_t *pgid);
void uid_to_uname (uid_t uid, char **uname);
int uname_to_uid (char const *uname, uid_t *puid);

void name_init (void);
void name_add_name (const char *name);
char const *name_next (bool);
void name_gather (void);
struct name *addname (char const *, idx_t, bool, struct name *);
void add_starting_file (char const *file_name);
void remname (struct name *name);
bool name_match (const char *name);
void names_notfound (void);
void label_notfound (void);
void collect_and_sort_names (void);
struct name *name_scan (const char *name, bool exact);
struct name const *name_from_list (void);
void blank_name_list (void);
char *make_file_name (const char *dir_name, const char *name);
ptrdiff_t stripped_prefix_len (char const *file_name, idx_t num);
bool all_names_found (struct tar_stat_info *st);

void add_avoided_name (char const *name);
bool is_avoided_name (char const *name);

bool contains_dot_dot (char const *name);

COMMON_INLINE bool
isfound (struct name const *c)
{
  return (occurrence_option == 0
	  ? (c)->found_count != 0
	  : (c)->found_count == occurrence_option);
}

COMMON_INLINE bool
wasfound (struct name const *c)
{
  return (occurrence_option == 0
	  ? (c)->found_count != 0
	  : occurrence_option <= (c)->found_count);
}

/* Module tar.c.  */

_Noreturn void usage (int);

int confirm (const char *message_action, const char *name);

void tar_stat_init (struct tar_stat_info *st);
bool tar_stat_close (struct tar_stat_info *st);
void tar_stat_destroy (struct tar_stat_info *st);
_Noreturn void usage (int);
int tar_timespec_cmp (struct timespec a, struct timespec b);
const char *archive_format_string (enum archive_format fmt);
const char *subcommand_string (enum subcommand c);
void set_exit_status (int val);

void request_stdin (const char *option);

int decode_signal (const char *);

/* Where an option comes from: */
enum option_source
  {
    OPTS_ENVIRON,        /* Environment variable TAR_OPTIONS */
    OPTS_COMMAND_LINE,   /* Command line */
    OPTS_FILE            /* File supplied by --files-from */
  };

/* Option location */
struct option_locus
{
  enum option_source source;  /* Option origin */
  char const *name;           /* File or variable name */
  intmax_t line;              /* Number of input line if source is OPTS_FILE */
  struct option_locus *prev;  /* Previous occurrence of the option of same
				 class */
};

struct tar_args        /* Variables used during option parsing */
{
  struct option_locus *loc;

  struct textual_date *textual_date; /* Keeps the arguments to --newer-mtime
					and/or --date option if they are
					textual dates */
  bool o_option;                   /* True if -o option was given */
  bool pax_option;                 /* True if --pax-option was given */
  bool compress_autodetect;        /* True if compression autodetection should
				      be attempted when creating archives */
  char const *backup_suffix_string;   /* --suffix option argument */
  char const *version_control_string; /* --backup option argument */
};

void more_options (int argc, char **argv, struct option_locus *loc);

/* Module update.c.  */

extern bool time_to_start_writing;
extern char *output_start;

void update_archive (void);

/* Module attrs.c.  */
#include "xattrs.h"

/* Module xheader.c.  */

void xheader_decode (struct tar_stat_info *stat);
void xheader_decode_global (struct xheader *xhdr);
void xheader_store (char const *keyword, struct tar_stat_info *st,
		    void const *data);
void xheader_read (struct xheader *xhdr, union block *header, off_t size);
void xheader_write (char type, char *name, time_t t, struct xheader *xhdr);
void xheader_write_global (struct xheader *xhdr);
void xheader_forbid_global (void);
void xheader_finish (struct xheader *hdr);
void xheader_destroy (struct xheader *hdr);
char *xheader_xhdr_name (struct tar_stat_info *st);
char *xheader_ghdr_name (void);
void xheader_set_option (char *string);
void xheader_string_begin (struct xheader *xhdr);
void xheader_string_add (struct xheader *xhdr, char const *s);
bool xheader_string_end (struct xheader *xhdr, char const *keyword);
bool xheader_keyword_deleted_p (const char *kw);
char *xheader_format_name (struct tar_stat_info *st, const char *fmt,
			   size_t n);
void xheader_xattr_init (struct tar_stat_info *st);

void xattr_map_init (struct xattr_map *map);
void xattr_map_copy (struct xattr_map *dst,
		     const struct xattr_map *src);
void xattr_map_add (struct xattr_map *map,
		    const char *key, const char *val, idx_t len);
void xattr_map_free (struct xattr_map *xattr_map);

/* Module system.c */

/* Nonzero when outputting to /dev/null.  */
extern bool dev_null_output;

void sys_detect_dev_null_output (void);
void sys_wait_for_child (pid_t, bool);
void sys_spawn_shell (void);
bool sys_compare_uid (struct stat *a, struct stat *b);
bool sys_compare_gid (struct stat *a, struct stat *b);
bool sys_file_is_archive (struct tar_stat_info *p);
bool sys_compare_links (struct stat *link_data, struct stat *stat_data);
int sys_truncate (int fd);
pid_t sys_child_open_for_compress (void);
pid_t sys_child_open_for_uncompress (void);
idx_t sys_write_archive_buffer (void);
bool sys_get_archive_stat (void);
int sys_exec_command (char *file_name, int typechar, struct tar_stat_info *st);
void sys_wait_command (void);
int sys_exec_info_script (const char **archive_name, int volume_number);
void sys_exec_checkpoint_script (const char *script_name,
				 const char *archive_name,
				 intmax_t checkpoint_number);
bool mtioseek (bool count_files, off_t count);
int sys_exec_setmtime_script (const char *script_name,
			      int dirfd,
			      const char *file_name,
			      const char *fmt,
			      struct timespec *ts);

/* Module compare.c */
void report_difference (struct tar_stat_info *st, const char *message, ...)
  ATTRIBUTE_FORMAT ((printf, 2, 3));

/* Module sparse.c */
bool sparse_member_p (struct tar_stat_info *st);
bool sparse_fixup_header (struct tar_stat_info *st);
enum dump_status sparse_dump_file (int, struct tar_stat_info *st);
enum dump_status sparse_extract_file (int fd, struct tar_stat_info *st,
				      off_t *size);
enum dump_status sparse_skim_file (struct tar_stat_info *st, bool must_copy);
bool sparse_diff_file (int, struct tar_stat_info *st);

/* Module utf8.c */
bool string_ascii_p (const char *str);
bool utf8_convert (bool to_utf, char const *input, char **output);

/* Module transform.c */
enum
  {
    XFORM_REGFILE	= 1 << 0,
    XFORM_LINK		= 1 << 1,
    XFORM_SYMLINK	= 1 << 2,
    XFORM_ALL = XFORM_REGFILE | XFORM_LINK | XFORM_SYMLINK
  };

void set_transform_expr (const char *expr);
bool transform_name (char **pinput, int type);
bool transform_name_fp (char **pinput, int type,
			char *(*fun)(char *, void *), void *);
bool transform_program_p (void);

/* Module suffix.c */
void set_compression_program_by_suffix (const char *name, const char *defprog,
					bool verbose);
char *strip_compression_suffix (const char *name);

/* Module checkpoint.c */
void checkpoint_compile_action (const char *str);
void checkpoint_finish_compile (void);
void checkpoint_run (bool do_write);
void checkpoint_finish (void);
void checkpoint_flush_actions (void);

/* Module warning.c */
enum
  {
    WARN_ALONE_ZERO_BLOCK	= 1 <<  0,
    WARN_BAD_DUMPDIR		= 1 <<  1,
    WARN_CACHEDIR		= 1 <<  2,
    WARN_CONTIGUOUS_CAST	= 1 <<  3,
    WARN_FILE_CHANGED		= 1 <<  4,
    WARN_FILE_IGNORED		= 1 <<  5,
    WARN_FILE_REMOVED		= 1 <<  6,
    WARN_FILE_SHRANK		= 1 <<  7,
    WARN_FILE_UNCHANGED		= 1 <<  8,
    WARN_FILENAME_WITH_NULS	= 1 <<  9,
    WARN_IGNORE_ARCHIVE		= 1 << 10,
    WARN_IGNORE_NEWER		= 1 << 11,
    WARN_NEW_DIRECTORY		= 1 << 12,
    WARN_RENAME_DIRECTORY	= 1 << 13,
    WARN_SYMLINK_CAST		= 1 << 14,
    WARN_TIMESTAMP		= 1 << 15,
    WARN_UNKNOWN_CAST		= 1 << 16,
    WARN_UNKNOWN_KEYWORD	= 1 << 17,
    WARN_XDEV			= 1 << 18,
    WARN_DECOMPRESS_PROGRAM	= 1 << 19,
    WARN_EXISTING_FILE		= 1 << 20,
    WARN_XATTR_WRITE		= 1 << 21,
    WARN_RECORD_SIZE		= 1 << 22,
    WARN_FAILED_READ		= 1 << 23,
    WARN_MISSING_ZERO_BLOCKS	= 1 << 24
  };
/* These warnings are enabled by default in verbose mode: */
enum
  {
    WARN_VERBOSE_WARNINGS = (WARN_RENAME_DIRECTORY | WARN_NEW_DIRECTORY
			     | WARN_DECOMPRESS_PROGRAM | WARN_EXISTING_FILE
			     | WARN_RECORD_SIZE),
    WARN_ALL = ~0
  };

void set_warning_option (const char *arg);

extern int warning_option;

COMMON_INLINE bool
warning_enabled (int opt)
{
  return warning_option & opt;
}

extern void warnopt (int, int, char const *, ...)
  ATTRIBUTE_COLD ATTRIBUTE_FORMAT ((printf, 3, 4));

/* Module unlink.c */

void queue_deferred_unlink (const char *name, bool is_dir);
void finish_deferred_unlinks (void);

/* Module exit.c */
extern void (*fatal_exit_hook) (void);

/* Module exclist.c */
enum { EXCL_DEFAULT, EXCL_RECURSIVE, EXCL_NON_RECURSIVE };

void excfile_add (const char *name, int flags);
void info_attach_exclist (struct tar_stat_info *dir);
void info_free_exclist (struct tar_stat_info *dir);
bool excluded_name (char const *name, struct tar_stat_info *st);
void exclude_vcs_ignores (void);

/* Module map.c */
void owner_map_read (char const *name);
int owner_map_translate (uid_t uid, uid_t *new_uid, char const **new_name);
void group_map_read (char const *file);
int group_map_translate (gid_t gid, gid_t *new_gid, char const **new_name);


_GL_INLINE_HEADER_END
