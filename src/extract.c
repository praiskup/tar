/* Extract files from a tar archive.

   Copyright 1988-2025 Free Software Foundation, Inc.

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
   along with this program.  If not, see <http://www.gnu.org/licenses/>.

   Written by John Gilmore, on 1985-11-19.  */

#include <system.h>
#include <quotearg.h>
#include <errno.h>
#include <flexmember.h>
#include <hash.h>
#include <issymlink.h>
#include <priv-set.h>
#include <root-uid.h>
#include <same-inode.h>
#include <utimens.h>

#include "common.h"

dev_t root_device;

static bool we_are_root;	/* true if our effective uid == 0 */
static mode_t newdir_umask;	/* umask when creating new directories */
static mode_t current_umask;	/* current umask (which is set to 0 if -p) */

static mode_t const all_mode_bits = ~ (mode_t) 0;

#if ! HAVE_FCHMOD && ! defined fchmod
# define fchmod(fd, mode) (errno = ENOSYS, -1)
#endif
#if ! HAVE_FCHOWN && ! defined fchown
# define fchown(fd, uid, gid) (errno = ENOSYS, -1)
#endif

#if (defined HAVE_STRUCT_STAT_ST_BIRTHTIMESPEC_TV_NSEC \
     || defined HAVE_STRUCT_STAT_ST_BIRTHTIM_TV_NSEC \
     || defined HAVE_STRUCT_STAT_ST_BIRTHTIMENSEC \
     || (defined _WIN32 && ! defined __CYGWIN__))
# define HAVE_BIRTHTIME 1
#else
# define HAVE_BIRTHTIME 0
#endif

#if HAVE_BIRTHTIME
# define BIRTHTIME_EQ(a, b) (timespec_cmp (a, b) == 0)
#else
# define BIRTHTIME_EQ(a, b) true
#endif

/* Return true if an error number ERR means the system call is
   supported in this case.  */
static bool
implemented (int err)
{
  return ! (err == ENOSYS
	    || err == ENOTSUP
	    || (EOPNOTSUPP != ENOTSUP && err == EOPNOTSUPP));
}

/* List of directories whose statuses we need to extract after we've
   finished extracting their subsidiary files.  If you consider each
   contiguous subsequence of elements of the form [D]?[^D]*, where [D]
   represents an element where AFTER_LINKS is nonzero and [^D]
   represents an element where AFTER_LINKS is zero, then the head
   of the subsequence has the longest name, and each non-head element
   in the prefix is an ancestor (in the directory hierarchy) of the
   preceding element.  */

struct delayed_set_stat
  {
    /* Next directory in list.  */
    struct delayed_set_stat *next;

    /* Metadata for this directory.  */
    dev_t st_dev;
    ino_t st_ino;
    mode_t mode; /* The desired mode is MODE & ~ current_umask.  */
    uid_t uid;
    gid_t gid;
    struct timespec atime;
    struct timespec mtime;

    /* An estimate of the directory's current mode, along with a mask
       specifying which bits of this estimate are known to be correct.
       If CURRENT_MODE_MASK is zero, CURRENT_MODE's value doesn't
       matter.  */
    mode_t current_mode;
    mode_t current_mode_mask;

    /* This directory is an intermediate directory that was created
       as an ancestor of some other directory; it was not mentioned
       in the archive, so do not set its uid, gid, atime, or mtime,
       and don't alter its mode outside of MODE_RWX.  */
    bool interdir;

    /* Whether symbolic links should be followed when accessing the
       directory.  */
    int atflag;

    /* Do not set the status of this directory until after delayed
       links are created.  */
    bool after_links;

    /* Directory that the name is relative to.  */
    idx_t change_dir;

    /* extended attributes*/
    char *cntx_name;
    char *acls_a_ptr;
    idx_t acls_a_len;
    char *acls_d_ptr;
    idx_t acls_d_len;
    struct xattr_map xattr_map;
    /* Length and contents of name.  */
    idx_t file_name_len;
    char *file_name;
  };

static struct delayed_set_stat *delayed_set_stat_head;

/* Table of delayed stat updates hashed by path; null if none.  */
static Hash_table *delayed_set_stat_table;

/* A link whose creation we have delayed.  */
struct delayed_link
  {
    /* The next in a list of delayed links that should be made after
       this delayed link.  */
    struct delayed_link *next;

    /* The device, inode number and birthtime of the placeholder.
       birthtime.tv_nsec is negative if the birthtime is not available.
       Don't use mtime as this would allow for false matches if some
       other process removes the placeholder.  Don't use ctime as
       this would cause race conditions and other screwups, e.g.,
       when restoring hard-linked symlinks.  */
    dev_t st_dev;
    ino_t st_ino;
#if HAVE_BIRTHTIME
    struct timespec birthtime;
#endif

    /* True if the link is symbolic.  */
    bool is_symlink;

    /* The desired metadata, valid only the link is symbolic.  */
    mode_t mode;
    uid_t uid;
    gid_t gid;
    struct timespec atime;
    struct timespec mtime;

    /* The directory that the sources and target are relative to.  */
    idx_t change_dir;

    /* A list of sources for this link.  The sources are all to be
       hard-linked together.  */
    struct string_list *sources;

    /* SELinux context */
    char *cntx_name;

    /* ACLs */
    char *acls_a_ptr;
    idx_t acls_a_len;
    char *acls_d_ptr;
    idx_t acls_d_len;

    struct xattr_map xattr_map;

    /* The desired target of the desired link.  */
    char target[FLEXIBLE_ARRAY_MEMBER];
  };

/* Table of delayed links hashed by device and inode; null if none.  */
static Hash_table *delayed_link_table;

/* A list of the delayed links in tar file order,
   and the tail of that list.  */
static struct delayed_link *delayed_link_head;
static struct delayed_link **delayed_link_tail = &delayed_link_head;

struct string_list
  {
    struct string_list *next;
    char string[FLEXIBLE_ARRAY_MEMBER];
  };

static size_t
dl_hash (void const *entry, size_t table_size)
{
  struct delayed_link const *dl = entry;
  uintmax_t n = dl->st_dev;
  int nshift = TYPE_WIDTH (n) - TYPE_WIDTH (dl->st_dev);
  if (0 < nshift)
    n <<= nshift;
  n ^= dl->st_ino;
  return n % table_size;
}

static bool
dl_compare (void const *a, void const *b)
{
  struct delayed_link const *da = a, *db = b;
  return PSAME_INODE (da, db);
}

static size_t
ds_hash (void const *entry, size_t table_size)
{
  struct delayed_set_stat const *ds = entry;
  return hash_string (ds->file_name, table_size);
}

static bool
ds_compare (void const *a, void const *b)
{
  struct delayed_set_stat const *dsa = a, *dsb = b;
  return streq (dsa->file_name, dsb->file_name);
}

/*  Set up to extract files.  */
void
extr_init (void)
{
  we_are_root = geteuid () == ROOT_UID;
  same_permissions_option += we_are_root;
  same_owner_option += we_are_root;

  /* Option -p clears the kernel umask, so it does not affect proper
     restoration of file permissions.  New intermediate directories will
     comply with umask at start of program.  */

  newdir_umask = umask (0);
  if (0 < same_permissions_option)
    current_umask = 0;
  else
    {
      umask (newdir_umask);	/* restore the kernel umask */
      current_umask = newdir_umask;
    }
}

/* Use fchmod if possible, fchmodat otherwise.  */
static int
fd_i_chmod (int fd, char const *file, mode_t mode, int atflag)
{
  if (0 <= fd)
    {
      int result = fchmod (fd, mode);
      if (result == 0 || implemented (errno))
	return result;
    }
  return fchmodat (chdir_fd, file, mode, atflag);
}

/* A version of fd_i_chmod which gracefully handles several common error
   conditions.  Additional argument TYPEFLAG is the type of file in tar
   notation.
 */
static int
fd_chmod (int fd, char const *file_name, int mode, int atflag, char typeflag)
{
  int chmod_errno = fd_i_chmod (fd, file_name, mode, atflag) < 0 ? errno : 0;

  /* On Solaris, chmod may fail if we don't have PRIV_ALL, because
     setuid-root files would otherwise be a backdoor.  See
     http://opensolaris.org/jive/thread.jspa?threadID=95826
     (2009-09-03).  */
  if (chmod_errno == EPERM && (mode & S_ISUID)
      && priv_set_restore_linkdir () == 0)
    {
      chmod_errno = fd_i_chmod (fd, file_name, mode, atflag) < 0 ? errno : 0;
      priv_set_remove_linkdir ();
    }

  /* Linux fchmodat does not support AT_SYMLINK_NOFOLLOW, and
     returns ENOTSUP even when operating on non-symlinks, try
     again with the flag disabled if it does not appear to be
     supported and if the file is not a symlink.  This
     introduces a race, alas.  */
  if (atflag && typeflag != SYMTYPE && ! implemented (chmod_errno))
    chmod_errno = fd_i_chmod (fd, file_name, mode, 0) < 0 ? errno : 0;

  if (chmod_errno && (typeflag != SYMTYPE || implemented (chmod_errno)))
    {
      errno = chmod_errno;
      return -1;
    }
  return 0;
}

/* Use fchown if possible, fchownat otherwise.  */
static int
fd_chown (int fd, char const *file, uid_t uid, gid_t gid, int atflag)
{
  if (0 <= fd)
    {
      int result = fchown (fd, uid, gid);
      if (result == 0 || implemented (errno))
	return result;
    }
  return fchownat (chdir_fd, file, uid, gid, atflag);
}

/* Use fstat if possible, fstatat otherwise.  */
static int
fd_stat (int fd, char const *file, struct stat *st, int atflag)
{
  return (0 <= fd
	  ? fstat (fd, st)
	  : fstatat (chdir_fd, file, st, atflag));
}

/* Set the mode for FILE_NAME to MODE.
   MODE_MASK specifies the bits of MODE that we care about;
   thus if MODE_MASK is zero, do nothing.
   If FD is nonnegative, it is a file descriptor for the file.
   CURRENT_MODE and CURRENT_MODE_MASK specify information known about
   the file's current mode, using the style of struct delayed_set_stat.
   TYPEFLAG specifies the type of the file.
   ATFLAG specifies the flag to use when statting the file.  */
static void
set_mode (char const *file_name,
	  mode_t mode, mode_t mode_mask, int fd,
	  mode_t current_mode, mode_t current_mode_mask,
	  char typeflag, int atflag)
{
  if (((current_mode ^ mode) | ~ current_mode_mask) & mode_mask)
    {
      if (MODE_ALL & ~ (mode_mask & current_mode_mask))
	{
	  struct stat st;
	  if (fd_stat (fd, file_name, &st, atflag) < 0)
	    {
	      stat_error (file_name);
	      return;
	    }
	  current_mode = st.st_mode;
	}

      current_mode &= MODE_ALL;
      mode = (current_mode & ~ mode_mask) | (mode & mode_mask);

      if (current_mode != mode)
	{
	  if (fd_chmod (fd, file_name, mode, atflag, typeflag) < 0)
	    chmod_error_details (file_name, mode);
	}
    }
}

/* Check time after successfully setting FILE_NAME's time stamp to T.  */
static void
check_time (char const *file_name, struct timespec t)
{
  if (t.tv_sec < 0)
    warnopt (WARN_TIMESTAMP, 0, _("%s: implausibly old time stamp %s"),
	     quotearg_colon (file_name), tartime (t, true));
  else if (timespec_cmp (volume_start_time, t) < 0)
    {
      struct timespec now;
      gettime (&now);
      if (timespec_cmp (now, t) < 0)
	{
	  char buf[TIMESPEC_STRSIZE_BOUND];
	  struct timespec diff;
	  diff.tv_sec = t.tv_sec - now.tv_sec;
	  diff.tv_nsec = t.tv_nsec - now.tv_nsec;
	  if (diff.tv_nsec < 0)
	    {
	      diff.tv_nsec += BILLION;
	      diff.tv_sec--;
	    }
	  warnopt (WARN_TIMESTAMP, 0, _("%s: time stamp %s is %s s in the future"),
		   quotearg_colon (file_name), tartime (t, true),
		   code_timespec (diff, buf));
	}
    }
}

/* Restore stat attributes (owner, group, mode and times) for
   FILE_NAME, using information given in *ST.
   If FD is nonnegative, it is a file descriptor for the file.
   CURRENT_MODE and CURRENT_MODE_MASK specify information known about
   the file's current mode, using the style of struct delayed_set_stat.
   TYPEFLAG specifies the type of the file.
   If INTERDIR, this is an intermediate directory.
   ATFLAG specifies the flag to use when statting the file.  */

static void
set_stat (char const *file_name,
	  struct tar_stat_info const *st,
	  int fd, mode_t current_mode, mode_t current_mode_mask,
	  char typeflag, bool interdir, int atflag)
{
  /* Do the utime before the chmod because some versions of utime are
     broken and trash the modes of the file.  */

  if (! touch_option && ! interdir)
    {
      struct timespec ts[2];
      if (incremental_option)
	ts[0] = st->atime;
      else
	ts[0].tv_nsec = UTIME_OMIT;
      ts[1] = st->mtime;

      if (fdutimensat (fd, chdir_fd, file_name, ts, atflag) == 0)
	{
	  if (incremental_option)
	    check_time (file_name, ts[0]);
	  check_time (file_name, ts[1]);
	}
      else if (typeflag != SYMTYPE || implemented (errno))
	utime_error (file_name);
    }

  if (0 < same_owner_option && ! interdir)
    {
      /* Some systems allow non-root users to give files away.  Once this
	 done, it is not possible anymore to change file permissions.
	 However, setting file permissions now would be incorrect, since
	 they would apply to the wrong user, and there would be a race
	 condition.  So, don't use systems that allow non-root users to
	 give files away.  */
      uid_t uid = st->stat.st_uid;
      gid_t gid = st->stat.st_gid;

      if (fd_chown (fd, file_name, uid, gid, atflag) == 0)
	{
	  /* Changing the owner can clear st_mode bits in some cases.  */
	  if ((current_mode | ~ current_mode_mask) & S_IXUGO)
	    current_mode_mask &= ~ (current_mode & (S_ISUID | S_ISGID));
	}
      else if (typeflag != SYMTYPE || implemented (errno))
	chown_error_details (file_name, uid, gid);
    }

  set_mode (file_name,
	    st->stat.st_mode & ~ current_umask,
	    0 < same_permissions_option && ! interdir ? MODE_ALL : MODE_RWX,
	    fd, current_mode, current_mode_mask, typeflag, atflag);

  /* these three calls must be done *after* fd_chown() call because fd_chown
     causes that linux capabilities becomes cleared. */
  xattrs_xattrs_set (st, file_name, typeflag, true);
  xattrs_acls_set (st, file_name, typeflag);
  xattrs_selinux_set (st, file_name, typeflag);
}

/* Find the direct ancestor of FILE_NAME in the delayed_set_stat list.
 */
static struct delayed_set_stat *
find_direct_ancestor (char const *file_name)
{
  struct delayed_set_stat *h = delayed_set_stat_head;
  while (h)
    {
      if (! h->after_links
	  && strncmp (file_name, h->file_name, h->file_name_len) == 0
	  && ISSLASH (file_name[h->file_name_len])
	  && (last_component (file_name) == file_name + h->file_name_len + 1))
	break;
      h = h->next;
    }
  return h;
}

/* For each entry H in the leading prefix of entries in HEAD that do
   not have after_links marked, mark H and fill in its dev and ino
   members.  Assume HEAD && ! HEAD->after_links.  */
static void
mark_after_links (struct delayed_set_stat *head)
{
  struct delayed_set_stat *h = head;

  do
    {
      struct stat st;
      h->after_links = 1;

      if (deref_stat (h->file_name, &st) < 0)
	stat_error (h->file_name);
      else
	{
	  h->st_dev = st.st_dev;
	  h->st_ino = st.st_ino;
	}
    }
  while ((h = h->next) && ! h->after_links);
}

/* Remember to restore stat attributes (owner, group, mode and times)
   for the directory FILE_NAME, using information given in *ST,
   once we stop extracting files into that directory.

   If ST is null, merely create a placeholder node for an intermediate
   directory that was created by make_directories.

   NOTICE: this works only if the archive has usual member order, i.e.
   directory, then the files in that directory. Incremental archive have
   somewhat reversed order: first go subdirectories, then all other
   members. To help cope with this case the variable
   delay_directory_restore_option is set by prepare_to_extract.

   If an archive was explicitly created so that its member order is
   reversed, some directory timestamps can be restored incorrectly,
   e.g.:
       tar --no-recursion -cf archive dir dir/file1 foo dir/file2
*/
static void
delay_set_stat (char const *file_name, struct tar_stat_info const *st,
		mode_t current_mode, mode_t current_mode_mask,
		mode_t mode, int atflag)
{
  idx_t file_name_len = strlen (file_name);
  struct delayed_set_stat *data;

  if (! (delayed_set_stat_table
	 || (delayed_set_stat_table = hash_initialize (0, 0, ds_hash,
	                                               ds_compare, NULL))))
    xalloc_die ();

  struct delayed_set_stat key;
  key.file_name = (char *) file_name;

  data = hash_lookup (delayed_set_stat_table, &key);
  if (data)
    {
      if (data->interdir)
	{
	  struct stat real_st;
	  if (fstatat (chdir_fd, data->file_name,
		       &real_st, data->atflag)
	      < 0)
	    {
	      stat_error (data->file_name);
	    }
	  else
	    {
	      data->st_dev = real_st.st_dev;
	      data->st_ino = real_st.st_ino;
	    }
	}
    }
  else
    {
      data = xmalloc (sizeof (*data));
      data->next = delayed_set_stat_head;
      delayed_set_stat_head = data;
      data->file_name_len = file_name_len;
      data->file_name = xstrdup (file_name);
      if (! hash_insert (delayed_set_stat_table, data))
	xalloc_die ();
      data->after_links = false;
      if (st)
	{
	  data->st_dev = st->stat.st_dev;
	  data->st_ino = st->stat.st_ino;
	}
      xattr_map_init (&data->xattr_map);
    }

  data->mode = mode;
  if (st)
    {
      data->uid = st->stat.st_uid;
      data->gid = st->stat.st_gid;
      data->atime = st->atime;
      data->mtime = st->mtime;
    }
  data->current_mode = current_mode;
  data->current_mode_mask = current_mode_mask;
  data->interdir = ! st;
  data->atflag = atflag;
  data->change_dir = chdir_current;
  data->cntx_name = NULL;
  if (st)
    assign_string_or_null (&data->cntx_name, st->cntx_name);
  if (st && st->acls_a_ptr)
    {
      data->acls_a_ptr = xmemdup (st->acls_a_ptr, st->acls_a_len + 1);
      data->acls_a_len = st->acls_a_len;
    }
  else
    {
      data->acls_a_ptr = NULL;
      data->acls_a_len = 0;
    }
  if (st && st->acls_d_ptr)
    {
      data->acls_d_ptr = xmemdup (st->acls_d_ptr, st->acls_d_len + 1);
      data->acls_d_len = st->acls_d_len;
    }
  else
    {
      data->acls_d_ptr = NULL;
      data->acls_d_len = 0;
    }
  if (st)
    xattr_map_copy (&data->xattr_map, &st->xattr_map);
  if (must_be_dot_or_slash (file_name))
    mark_after_links (data);
}

/* If DIR is an intermediate directory created earlier, update its
   metadata from the current_stat_info and clear, its intermediate
   status and return true.  Return false otherwise.
 */
static bool
update_interdir_set_stat (char const *dir)
{
  if (delayed_set_stat_table)
    {
      struct delayed_set_stat key, *data;

      key.file_name = (char *) dir;
      data = hash_lookup (delayed_set_stat_table, &key);
      if (data && data->interdir)
	{
	  data->st_dev = current_stat_info.stat.st_dev;
	  data->st_ino = current_stat_info.stat.st_ino;
	  data->mode = current_stat_info.stat.st_mode;
	  data->uid = current_stat_info.stat.st_uid;
	  data->gid = current_stat_info.stat.st_gid;
	  data->atime = current_stat_info.atime;
	  data->mtime = current_stat_info.mtime;
	  data->interdir = false;
	  return true;
	}
    }
  return false;
}

/* Update the delayed_set_stat info for an intermediate directory
   created within the file name of DIR.  The intermediate directory turned
   out to be the same as this directory, e.g. due to ".." or symbolic
   links.  *DIR_STAT_INFO is the status of the directory.  */
static void
repair_delayed_set_stat (char const *dir,
			 struct stat const *dir_stat_info)
{
  struct delayed_set_stat *data;
  for (data = delayed_set_stat_head; data; data = data->next)
    {
      struct stat st;
      if (fstatat (chdir_fd, data->file_name, &st, data->atflag) < 0)
	{
	  stat_error (data->file_name);
	  return;
	}

      if (psame_inode (&st, dir_stat_info))
	{
	  data->st_dev = current_stat_info.stat.st_dev;
	  data->st_ino = current_stat_info.stat.st_ino;
	  data->mode = current_stat_info.stat.st_mode;
	  data->uid = current_stat_info.stat.st_uid;
	  data->gid = current_stat_info.stat.st_gid;
	  data->atime = current_stat_info.atime;
	  data->mtime = current_stat_info.mtime;
	  data->current_mode = st.st_mode;
	  data->current_mode_mask = all_mode_bits;
	  data->interdir = false;
	  return;
	}
    }

  paxerror (0, _("%s: Unexpected inconsistency when making directory"),
	    quotearg_colon (dir));
}

static void
free_delayed_set_stat (struct delayed_set_stat *data)
{
  free (data->file_name);
  xattr_map_free (&data->xattr_map);
  free (data->cntx_name);
  free (data->acls_a_ptr);
  free (data->acls_d_ptr);
  free (data);
}

void
remove_delayed_set_stat (const char *fname)
{
  struct delayed_set_stat *data, *next, *prev = NULL;
  for (data = delayed_set_stat_head; data; data = next)
    {
      next = data->next;
      if (chdir_current == data->change_dir
	  && streq (data->file_name, fname))
	{
	  hash_remove (delayed_set_stat_table, data);
	  free_delayed_set_stat (data);
	  if (prev)
	    prev->next = next;
	  else
	    delayed_set_stat_head = next;
	  return;
	}
      else
	prev = data;
    }
}

static void
fixup_delayed_set_stat (char const *src, char const *dst)
{
  struct delayed_set_stat *data;
  for (data = delayed_set_stat_head; data; data = data->next)
    {
      if (chdir_current == data->change_dir
	  && streq (data->file_name, src))
	{
	  free (data->file_name);
	  data->file_name = xstrdup (dst);
	  data->file_name_len = strlen (dst);
	  return;
	}
    }
}

/* After a file/link/directory creation has failed due to ENOENT,
   create all required directories.  Return zero if all the required
   directories were created, nonzero (issuing a diagnostic) otherwise.
   Set *INTERDIR_MADE (unless NULL) if at least one directory was created. */
static int
make_directories (char *file_name, bool *interdir_made)
{
  char *cursor0 = file_name + FILE_SYSTEM_PREFIX_LEN (file_name);
  char *cursor;	        	/* points into the file name */
  char *parent_end = NULL;
  int parent_errno;

  for (cursor = cursor0; *cursor; cursor++)
    {
      mode_t mode;
      mode_t desired_mode;
      int status;

      if (! ISSLASH (*cursor))
	continue;

      /* Avoid mkdir of empty string, if leading or double '/'.  */

      if (cursor == cursor0 || ISSLASH (cursor[-1]))
	continue;

      /* Avoid mkdir where last part of file name is "." or "..".  */

      if (cursor[-1] == '.'
	  && (cursor == cursor0 + 1 || ISSLASH (cursor[-2])
	      || (cursor[-2] == '.'
		  && (cursor == cursor0 + 2 || ISSLASH (cursor[-3])))))
	continue;

      *cursor = '\0';		/* truncate the name there */
      desired_mode = MODE_RWX & ~ newdir_umask;
      mode = desired_mode | (we_are_root ? 0 : MODE_WXUSR);
      status = mkdirat (chdir_fd, file_name, mode);

      if (status == 0)
	{
	  /* Create a struct delayed_set_stat even if
	     mode == desired_mode, because
	     repair_delayed_set_stat may need to update the struct.  */
	  delay_set_stat (file_name,
			  0, mode & ~ current_umask, MODE_RWX,
			  desired_mode, AT_SYMLINK_NOFOLLOW);
	  if (interdir_made)
	    *interdir_made = true;
	  print_for_mkdir (file_name, desired_mode);
	  parent_end = NULL;
	}
      else
	switch (errno)
	  {
	  case ELOOP: case ENAMETOOLONG: case ENOENT: case ENOTDIR:
	    /* FILE_NAME doesn't exist and couldn't be created; fail now.  */
	    mkdir_error (file_name);
	    *cursor = '/';
	    return status;

	  default:
	    /* FILE_NAME may be an existing directory so do not fail now.
	       Instead, arrange to check at loop exit, assuming this is
	       the last loop iteration.  */
	    parent_end = cursor;
	    parent_errno = errno;
	    break;
	  }

      *cursor = '/';
    }

  if (!parent_end)
    return 0;

  /* Although we did not create the parent directory, some other
     process may have created it, so check whether it exists now.  */
  *parent_end = '\0';
  struct stat st;
  int stat_status = fstatat (chdir_fd, file_name, &st, 0);
  if (! (stat_status < 0 || S_ISDIR (st.st_mode)))
    stat_status = -1;
  if (stat_status < 0)
    {
      errno = parent_errno;
      mkdir_error (file_name);
    }
  else if (interdir_made)
    *interdir_made = true;

  *parent_end = '/';

  return stat_status;
}

/* Return true if FILE_NAME (with status *STP, if STP) is not a
   directory, and has a time stamp newer than (or equal to) that of
   TAR_STAT.  */
static bool
file_newer_p (const char *file_name, struct stat const *stp,
	      struct tar_stat_info *tar_stat)
{
  struct stat st;

  if (!stp)
    {
      if (deref_stat (file_name, &st) < 0)
	{
	  if (errno != ENOENT)
	    {
	      stat_warn (file_name);
	      /* Be safer: if the file exists, assume it is newer.  */
	      return true;
	    }
	  return false;
	}
      stp = &st;
    }

  return (! S_ISDIR (stp->st_mode)
	  && tar_timespec_cmp (tar_stat->mtime, get_stat_mtime (stp)) <= 0);
}

enum recover { RECOVER_NO, RECOVER_OK, RECOVER_SKIP };

/* Attempt repairing what went wrong with the extraction.  Delete an
   already existing file or create missing intermediate directories.
   Return RECOVER_OK if we somewhat increased our chances at a successful
   extraction, RECOVER_NO if there are no chances, and RECOVER_SKIP if the
   caller should skip extraction of that member.  The value of errno is
   properly restored on returning RECOVER_NO.

   If REGULAR, the caller was trying to extract onto a regular file.

   Set *INTERDIR_MADE if an intermediate directory is made as part of
   the recovery process.  */

static enum recover
maybe_recoverable (char *file_name, bool regular, bool *interdir_made)
{
  int e = errno;
  struct stat st;
  struct stat const *stp = 0;

  if (*interdir_made)
    return RECOVER_NO;

  switch (e)
    {
    case ELOOP:

      /* With open ("symlink", O_NOFOLLOW|...), POSIX says errno == ELOOP,
	 but some operating systems do not conform to the standard.  */
#ifdef EFTYPE
      /* NetBSD uses errno == EFTYPE; see <http://gnats.netbsd.org/43154>.  */
    case EFTYPE:
#endif
      /* FreeBSD 8.1 uses errno == EMLINK.  */
    case EMLINK:
      /* Tru64 5.1B uses errno == ENOTSUP.  */
    case ENOTSUP:

      if (! regular
	  || old_files_option != OVERWRITE_OLD_FILES || dereference_option)
	break;
      if (strchr (file_name, '/'))
	{
	  if (deref_stat (file_name, &st) < 0)
	    break;
	  stp = &st;
	}
      /* The caller tried to open a symbolic link with O_NOFOLLOW.
	 Fall through, treating it as an already-existing file.  */
      FALLTHROUGH;
    case EEXIST:
      /* Remove an old file, if the options allow this.  */

      switch (old_files_option)
	{
	case SKIP_OLD_FILES:
	  warnopt (WARN_EXISTING_FILE, 0, _("%s: skipping existing file"),
		   quotearg_colon (file_name));
	  return RECOVER_SKIP;

	case KEEP_OLD_FILES:
	  return RECOVER_NO;

	case KEEP_NEWER_FILES:
	  if (file_newer_p (file_name, stp, &current_stat_info))
	    break;
	  FALLTHROUGH;
	case DEFAULT_OLD_FILES:
	case NO_OVERWRITE_DIR_OLD_FILES:
	case OVERWRITE_OLD_FILES:
	  if (0 < remove_any_file (file_name, ORDINARY_REMOVE_OPTION))
	    return RECOVER_OK;
	  break;

	case UNLINK_FIRST_OLD_FILES:
	  break;
	}
      FALLTHROUGH;

    case ENOENT:
      /* Attempt creating missing intermediate directories. */
      if (make_directories (file_name, interdir_made) == 0 && *interdir_made)
	return RECOVER_OK;
      break;

    default:
      /* Just say we can't do anything about it...  */
      break;
    }

  errno = e;
  return RECOVER_NO;
}

/* Restore stat extended attributes (xattr) for FILE_NAME, using information
   given in *ST.  Restore before extraction because they may affect file layout
   (e.g. on Lustre distributed parallel filesystem - setting info about how many
   servers is this file striped over, stripe size, mirror copies, etc.
   in advance dramatically improves the following  performance of reading and
   writing a file).  TYPEFLAG specifies the type of the file.  Return a negative
   number (setting errno) on failure, zero if successful but FILE_NAME was not
   created (e.g., xattrs not available), and a positive number if FILE_NAME was
   created.  */
static int
set_xattr (MAYBE_UNUSED char const *file_name,
	   MAYBE_UNUSED struct tar_stat_info const *st,
	   MAYBE_UNUSED mode_t mode, MAYBE_UNUSED char typeflag)
{
#ifdef HAVE_XATTRS
  if (xattrs_option && st->xattr_map.xm_size)
    {
      int r = mknodat (chdir_fd, file_name, mode | S_IFREG, 0);
      if (r < 0)
	return r;
      xattrs_xattrs_set (st, file_name, typeflag, false);
      return 1;
    }
#endif

  return 0;
}

/* Fix the statuses of all directories whose statuses need fixing, and
   which are not ancestors of FILE_NAME.  If AFTER_LINKS is
   nonzero, do this for all such directories; otherwise, stop at the
   first directory that is marked to be fixed up only after delayed
   links are applied.  */
static void
apply_nonancestor_delayed_set_stat (char const *file_name, bool after_links)
{
  idx_t file_name_len = strlen (file_name);
  bool check_for_renamed_directories = 0;

  while (delayed_set_stat_head)
    {
      struct delayed_set_stat *data = delayed_set_stat_head;
      bool skip_this_one = 0;
      struct stat st;
      mode_t current_mode = data->current_mode;
      mode_t current_mode_mask = data->current_mode_mask;

      check_for_renamed_directories |= data->after_links;

      if (after_links < data->after_links
	  || (data->file_name_len < file_name_len
	      && file_name[data->file_name_len]
	      && (ISSLASH (file_name[data->file_name_len])
		  || ISSLASH (file_name[data->file_name_len - 1]))
	      && memeq (file_name, data->file_name, data->file_name_len)))
	break;

      chdir_do (data->change_dir);

      if (check_for_renamed_directories)
	{
	  if (fstatat (chdir_fd, data->file_name, &st, data->atflag) < 0)
	    {
	      stat_error (data->file_name);
	      skip_this_one = 1;
	    }
	  else
	    {
	      current_mode = st.st_mode;
	      current_mode_mask = all_mode_bits;
	      if (!SAME_INODE (st, *data))
		{
		  paxerror (0,
			    _("%s: Directory renamed before its status"
			      " could be extracted"),
			    quotearg_colon (data->file_name));
		  skip_this_one = 1;
		}
	    }
	}

      if (! skip_this_one)
	{
	  struct tar_stat_info sb;
	  sb.stat.st_mode = data->mode;
	  sb.stat.st_uid = data->uid;
	  sb.stat.st_gid = data->gid;
	  sb.atime = data->atime;
	  sb.mtime = data->mtime;
	  sb.cntx_name = data->cntx_name;
	  sb.acls_a_ptr = data->acls_a_ptr;
	  sb.acls_a_len = data->acls_a_len;
	  sb.acls_d_ptr = data->acls_d_ptr;
	  sb.acls_d_len = data->acls_d_len;
	  sb.xattr_map = data->xattr_map;
	  set_stat (data->file_name, &sb,
		    -1, current_mode, current_mode_mask,
		    DIRTYPE, data->interdir, data->atflag);
	}

      delayed_set_stat_head = data->next;
      hash_remove (delayed_set_stat_table, data);
      free_delayed_set_stat (data);
    }
}


static bool
is_directory_link (char const *file_name, struct stat *st)
{
  return (issymlinkat (chdir_fd, file_name)
	  && fstatat (chdir_fd, file_name, st, 0) == 0
	  && S_ISDIR (st->st_mode));
}

/* Given struct stat of a directory (or directory member) whose ownership
   or permissions of will be restored later, return the temporary permissions
   for that directory, sufficiently restrictive so that in the meantime
   processes owned by other users do not inadvertently create files under this
   directory that inherit the wrong owner, group, or permissions from the
   directory.

   If not root, though, make the directory writeable and searchable at first,
   so that files can be created under it.
*/
static int
safe_dir_mode (struct stat const *st)
{
  return ((st->st_mode
	   & (0 < same_owner_option || 0 < same_permissions_option
	      ? S_IRWXU
	      : MODE_RWX))
	  | (we_are_root ? 0 : MODE_WXUSR));
}

/* Extractor functions for various member types */

static bool
extract_dir (char *file_name, char typeflag)
{
  int status;
  mode_t mode;
  mode_t current_mode = 0;
  mode_t current_mode_mask = 0;
  int atflag = 0;
  bool interdir_made = false;

  /* Save 'root device' to avoid purging mount points. */
  if (one_file_system_option && root_device == 0)
    {
      struct chdir_id id = chdir_id ();
      if (id.err)
	{
	  errno = id.err;
	  stat_diag (".");
	}
      else
	root_device = id.st_dev;
    }

  if (incremental_option)
    /* Read the entry and delete files that aren't listed in the archive.  */
    purge_directory (file_name);
  else if (typeflag == GNUTYPE_DUMPDIR)
    skip_member ();

  mode = safe_dir_mode (&current_stat_info.stat);

  for (;;)
    {
      status = mkdirat (chdir_fd, file_name, mode);
      if (status == 0)
	{
	  current_mode = mode & ~ current_umask;
	  current_mode_mask = MODE_RWX;
	  atflag = AT_SYMLINK_NOFOLLOW;
	  break;
	}

      if (errno == EEXIST)
	{
	  if (interdir_made
	      || keep_directory_symlink_option
	      || old_files_option == NO_OVERWRITE_DIR_OLD_FILES
	      || old_files_option == DEFAULT_OLD_FILES
	      || old_files_option == OVERWRITE_OLD_FILES)
	    {
	      struct stat st;
	      st.st_mode = 0;

	      if (keep_directory_symlink_option
		  && is_directory_link (file_name, &st))
		return true;

	      if ((st.st_mode != 0 && fstatat_flags == 0)
		  || deref_stat (file_name, &st) == 0)
		{
		  current_mode = st.st_mode;
		  current_mode_mask = all_mode_bits;

		  if (S_ISDIR (current_mode))
		    {
		      if (interdir_made)
			{
			  repair_delayed_set_stat (file_name, &st);
			  return true;
			}
		      break;
		    }
		}
	    }
	  else if (update_interdir_set_stat (file_name))
	    return true;
	  else if (old_files_option == UNLINK_FIRST_OLD_FILES)
	    {
	      status = 0;
	      break;
	    }

	  errno = EEXIST;
	}

      switch (maybe_recoverable (file_name, false, &interdir_made))
	{
	case RECOVER_OK:
	  continue;

	case RECOVER_SKIP:
	  break;

	case RECOVER_NO:
	  if (errno != EEXIST)
	    {
	      mkdir_error (file_name);
	      return false;
	    }
	  break;
	}
      break;
    }

  if (status == 0
      || old_files_option == DEFAULT_OLD_FILES
      || old_files_option == OVERWRITE_OLD_FILES)
    delay_set_stat (file_name, &current_stat_info,
		    current_mode, current_mode_mask,
		    current_stat_info.stat.st_mode, atflag);
  return status == 0;
}



static int
open_output_file (char const *file_name, char typeflag, mode_t mode,
                  int file_created, mode_t *current_mode,
                  mode_t *current_mode_mask)
{
  int fd;
  bool overwriting_old_files = old_files_option == OVERWRITE_OLD_FILES;
  int openflag = (O_WRONLY | O_BINARY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK
		  | (file_created
		     ? O_NOFOLLOW
		     : (O_CREAT
			| (overwriting_old_files
			   ? O_TRUNC | (dereference_option ? 0 : O_NOFOLLOW)
			   : O_EXCL))));

  if (typeflag == CONTTYPE)
    {
      static bool conttype_diagnosed;

      if (!conttype_diagnosed)
	{
	  conttype_diagnosed = true;
	  warnopt (WARN_CONTIGUOUS_CAST, 0,
		   _("Extracting contiguous files as regular files"));
	}
    }

  /* If O_NOFOLLOW is needed but does not work, check for a symlink
     separately.  There's a race condition, but that cannot be avoided
     on hosts lacking O_NOFOLLOW.  */
  if (! HAVE_WORKING_O_NOFOLLOW
      && overwriting_old_files && ! dereference_option
      && issymlinkat (chdir_fd, file_name))
    {
      errno = ELOOP;
      return -1;
    }

  fd = openat (chdir_fd, file_name, openflag, mode);
  if (0 <= fd)
    {
      if (openflag & O_EXCL)
	{
	  *current_mode = mode & ~ current_umask;
	  *current_mode_mask = MODE_RWX;
	}
      else
	{
	  struct stat st;
	  if (fstat (fd, &st) < 0)
	    {
	      int e = errno;
	      close (fd);
	      errno = e;
	      return -1;
	    }
	  if (! S_ISREG (st.st_mode))
	    {
	      close (fd);
	      errno = EEXIST;
	      return -1;
	    }
	  *current_mode = st.st_mode;
	  *current_mode_mask = all_mode_bits;
	}
    }

  return fd;
}

static bool
extract_file (char *file_name, char typeflag)
{
  int fd;
  off_t size;
  union block *data_block;
  int status;
  bool interdir_made = false;
  mode_t mode = (current_stat_info.stat.st_mode & MODE_RWX
		 & ~ (0 < same_owner_option ? S_IRWXG | S_IRWXO : 0));
  mode_t current_mode = 0;
  mode_t current_mode_mask = 0;

  if (to_stdout_option)
    fd = STDOUT_FILENO;
  else if (to_command_option)
    {
      fd = sys_exec_command (file_name, 'f', &current_stat_info);
      if (fd < 0)
	{
	  skip_member ();
	  return true;
	}
    }
  else
    {
      int file_created;
      /* Either we pre-create the file in set_xattr(), or we just directly open
         the file in open_output_file() with O_CREAT.  If pre-creating, we need
         to use S_IWUSR so we can open the file O_WRONLY in open_output_file().
         The additional mode bit is cleared later by set_stat()->set_mode().  */
      while (((file_created = set_xattr (file_name, &current_stat_info,
					 mode | S_IWUSR, typeflag))
	      < 0)
	     || ((fd = open_output_file (file_name, typeflag, mode,
					 file_created, &current_mode,
					 &current_mode_mask))
		 < 0))
	{
	  enum recover recover
	    = maybe_recoverable (file_name, true, &interdir_made);
	  if (recover != RECOVER_OK)
	    {
	      skip_member ();
	      if (recover == RECOVER_SKIP)
		return true;
	      open_error (file_name);
	      return false;
	    }
	}
    }

  mv_begin_read (&current_stat_info);
  if (current_stat_info.is_sparse)
    sparse_extract_file (fd, &current_stat_info, &size);
  else
    for (size = current_stat_info.stat.st_size; size > 0; )
      {
	mv_size_left (size);

	/* Locate data, determine max length writeable, write it,
	   block that we have used the data, then check if the write
	   worked.  */

	data_block = find_next_block ();
	if (! data_block)
	  {
	    paxerror (0, _("Unexpected EOF in archive"));
	    break;		/* FIXME: What happens, then?  */
	  }

	idx_t written = available_space_after (data_block);

	if (written > size)
	  written = size;
	errno = 0;
	idx_t count = blocking_write (fd, charptr (data_block), written);
	size -= written;

	set_next_block_after (charptr (data_block) + written - 1);
	if (count != written)
	  {
	    if (!to_command_option)
	      write_error_details (file_name, count, written);
	    /* FIXME: shouldn't we restore from backup? */
	    break;
	  }
      }

  skim_file (size, false);

  mv_end ();

  /* If writing to stdout, don't try to do anything to the filename;
     it doesn't exist, or we don't want to touch it anyway.  */

  if (to_stdout_option)
    return true;

  if (! to_command_option)
    set_stat (file_name, &current_stat_info, fd,
	      current_mode, current_mode_mask, typeflag, false,
	      (old_files_option == OVERWRITE_OLD_FILES
	       ? 0 : AT_SYMLINK_NOFOLLOW));

  status = close (fd);
  if (status < 0)
    close_error (file_name);

  if (to_command_option)
    sys_wait_command ();

  return status == 0;
}

/* Return true if NAME is a delayed link.  This can happen only if the link
   placeholder file has been created. Therefore, try to stat the NAME
   first. If it doesn't exist, there is no matching entry in the table.
   Otherwise, look for the entry in the table that has the matching dev
   and ino numbers.  Return false if not found.

   Do not rely on comparing file names, which may differ for
   various reasons (e.g. relative vs. absolute file names).
 */
static bool
find_delayed_link_source (char const *name)
{
  struct stat st;

  if (!delayed_link_table)
    return false;

  if (fstatat (chdir_fd, name, &st, AT_SYMLINK_NOFOLLOW) < 0)
    {
      if (errno != ENOENT)
	stat_error (name);
      return false;
    }

  struct delayed_link dl;
  dl.st_dev = st.st_dev;
  dl.st_ino = st.st_ino;
  return hash_lookup (delayed_link_table, &dl) != NULL;
}

/* Create a placeholder file with name FILE_NAME, which will be
   replaced after other extraction is done by a symbolic link if
   IS_SYMLINK is true, and by a hard link otherwise.  Set
   *INTERDIR_MADE if an intermediate directory is made in the
   process.
*/

static bool
create_placeholder_file (char *file_name, bool is_symlink, bool *interdir_made)
{
  int fd;
  struct stat st;

  while ((fd = openat (chdir_fd, file_name, O_WRONLY | O_CREAT | O_EXCL, 0)) < 0)
    {
      if (errno == EEXIST && find_delayed_link_source (file_name))
	{
	  /* The placeholder file has already been created.  This means
	     that the link being extracted is a duplicate of an already
	     processed one.  Skip it.
	   */
	  return true;
	}

      switch (maybe_recoverable (file_name, false, interdir_made))
	{
	case RECOVER_OK:
	  continue;

	case RECOVER_SKIP:
	  return true;

	case RECOVER_NO:
	  open_error (file_name);
	  return false;
	}
      }

  if (fstat (fd, &st) < 0)
    {
      stat_error (file_name);
      close (fd);
    }
  else if (close (fd) < 0)
    close_error (file_name);
  else
    {
      struct delayed_set_stat *h;
      struct delayed_link *p =
	xmalloc (FLEXNSIZEOF (struct delayed_link, target,
			      strlen (current_stat_info.link_name) + 1));
      p->next = NULL;
      p->st_dev = st.st_dev;
      p->st_ino = st.st_ino;
#if HAVE_BIRTHTIME
      p->birthtime = get_stat_birthtime (&st);
#endif
      p->is_symlink = is_symlink;
      if (is_symlink)
	{
	  p->mode = current_stat_info.stat.st_mode;
	  p->uid = current_stat_info.stat.st_uid;
	  p->gid = current_stat_info.stat.st_gid;
	  p->atime = current_stat_info.atime;
	  p->mtime = current_stat_info.mtime;
	}
      p->change_dir = chdir_current;
      p->sources = xmalloc (FLEXNSIZEOF (struct string_list, string,
					 strlen (file_name) + 1));
      p->sources->next = 0;
      strcpy (p->sources->string, file_name);
      p->cntx_name = NULL;
      assign_string_or_null (&p->cntx_name, current_stat_info.cntx_name);
      p->acls_a_ptr = NULL;
      p->acls_a_len = 0;
      p->acls_d_ptr = NULL;
      p->acls_d_len = 0;
      xattr_map_init (&p->xattr_map);
      xattr_map_copy (&p->xattr_map, &current_stat_info.xattr_map);
      strcpy (p->target, current_stat_info.link_name);

      *delayed_link_tail = p;
      delayed_link_tail = &p->next;
      if (! ((delayed_link_table
	      || (delayed_link_table = hash_initialize (0, 0, dl_hash,
							dl_compare, free)))
	     && hash_insert (delayed_link_table, p)))
	xalloc_die ();

      if ((h = find_direct_ancestor (file_name)) != NULL)
	mark_after_links (h);

      return true;
    }

  return false;
}

static bool
extract_link (char *file_name, MAYBE_UNUSED char typeflag)
{
  bool interdir_made = false;
  char const *link_name;
  enum recover rc;

  link_name = current_stat_info.link_name;

  if ((! absolute_names_option && contains_dot_dot (link_name))
      || find_delayed_link_source (link_name))
    return create_placeholder_file (file_name, false, &interdir_made);

  do
    {
      struct stat st1, st2;
      int e;
      int status = linkat (chdir_fd, link_name, chdir_fd, file_name, 0);
      e = errno;

      if (status == 0)
	{
	  if (delayed_link_table
	      && fstatat (chdir_fd, link_name, &st1, AT_SYMLINK_NOFOLLOW) == 0)
	    {
	      struct delayed_link dl1;
	      dl1.st_ino = st1.st_ino;
	      dl1.st_dev = st1.st_dev;
	      struct delayed_link *ds = hash_lookup (delayed_link_table, &dl1);
	      if (ds && ds->change_dir == chdir_current
		  && BIRTHTIME_EQ (ds->birthtime, get_stat_birthtime (&st1)))
		{
		  struct string_list *p
		    = xmalloc (FLEXNSIZEOF (struct string_list,
					    string, strlen (file_name) + 1));
		  strcpy (p->string, file_name);
		  p->next = ds->sources;
		  ds->sources = p;
		}
	    }

	  return true;
	}
      else if ((e == EEXIST && streq (link_name, file_name))
	       || ((fstatat (chdir_fd, link_name, &st1, AT_SYMLINK_NOFOLLOW)
		    == 0)
		   && (fstatat (chdir_fd, file_name, &st2, AT_SYMLINK_NOFOLLOW)
		       == 0)
		   && psame_inode (&st1, &st2)))
	return true;

      errno = e;
    }
  while ((rc = maybe_recoverable (file_name, false, &interdir_made))
	 == RECOVER_OK);

  if (rc == RECOVER_SKIP)
    return true;
  if (!(incremental_option && errno == EEXIST))
    {
      link_error (link_name, file_name);
      return false;
    }
  return true;
}

static bool
extract_symlink (char *file_name, MAYBE_UNUSED char typeflag)
{
  bool interdir_made = false;

  if (! absolute_names_option
      && (IS_ABSOLUTE_FILE_NAME (current_stat_info.link_name)
	  || contains_dot_dot (current_stat_info.link_name)))
    return create_placeholder_file (file_name, true, &interdir_made);

  while (symlinkat (current_stat_info.link_name, chdir_fd, file_name) < 0)
    switch (maybe_recoverable (file_name, false, &interdir_made))
      {
      case RECOVER_OK:
	continue;

      case RECOVER_SKIP:
	return true;

      case RECOVER_NO:
	if (!implemented (errno))
	  {
	    static bool warned;
	    if (!warned)
	      {
		warned = true;
		warnopt (WARN_SYMLINK_CAST, 0,
			 _("Attempting extraction of symbolic links"
			   " as hard links"));
	      }
	    return extract_link (file_name, typeflag);
	  }
	symlink_error (current_stat_info.link_name, file_name);
	return false;
      }

  set_stat (file_name, &current_stat_info, -1, 0, 0,
	    SYMTYPE, false, AT_SYMLINK_NOFOLLOW);
  return true;
}

#if S_IFCHR || S_IFBLK
static bool
extract_node (char *file_name, char typeflag)
{
  bool interdir_made = false;
  mode_t mode = (current_stat_info.stat.st_mode & (MODE_RWX | S_IFBLK | S_IFCHR)
		 & ~ (0 < same_owner_option ? S_IRWXG | S_IRWXO : 0));

  while (mknodat (chdir_fd, file_name, mode, current_stat_info.stat.st_rdev)
	 < 0)
    switch (maybe_recoverable (file_name, false, &interdir_made))
      {
      case RECOVER_OK:
	continue;

      case RECOVER_SKIP:
	return true;

      case RECOVER_NO:
	mknod_error (file_name);
	return false;
      }

  set_stat (file_name, &current_stat_info, -1,
	    mode & ~ current_umask, MODE_RWX,
	    typeflag, false, AT_SYMLINK_NOFOLLOW);
  return true;
}
#endif

#if HAVE_MKFIFO || defined mkfifo
static bool
extract_fifo (char *file_name, char typeflag)
{
  bool interdir_made = false;
  mode_t mode = (current_stat_info.stat.st_mode & MODE_RWX
		 & ~ (0 < same_owner_option ? S_IRWXG | S_IRWXO : 0));

  while (mkfifoat (chdir_fd, file_name, mode) < 0)
    switch (maybe_recoverable (file_name, false, &interdir_made))
      {
      case RECOVER_OK:
	continue;

      case RECOVER_SKIP:
	return true;

      case RECOVER_NO:
	mkfifo_error (file_name);
	return false;
      }

  set_stat (file_name, &current_stat_info, -1,
	    mode & ~ current_umask, MODE_RWX,
	    typeflag, false, AT_SYMLINK_NOFOLLOW);
  return true;
}
#endif

typedef bool (*tar_extractor_t) (char *file_name, char typeflag);


/* Prepare to extract a file. Find extractor function.
   Return an extractor to proceed with the extraction,
   a null pointer to skip the current member.  */

static tar_extractor_t
prepare_to_extract (char const *file_name, char typeflag)
{
  tar_extractor_t extractor;

  /* Select the extractor */
  switch (typeflag)
    {
    case GNUTYPE_SPARSE:
      extractor = extract_file;
      break;

    case AREGTYPE:
    case REGTYPE:
    case CONTTYPE:
      /* Appears to be a file.  But BSD tar uses the convention that a slash
	 suffix means a directory.  */
      extractor = (current_stat_info.had_trailing_slash
		   ? extract_dir : extract_file);
      break;

    case SYMTYPE:
      extractor = extract_symlink;
      break;

    case LNKTYPE:
      extractor = extract_link;
      break;

#if S_IFCHR
    case CHRTYPE:
      current_stat_info.stat.st_mode |= S_IFCHR;
      extractor = extract_node;
      break;
#endif

#if S_IFBLK
    case BLKTYPE:
      current_stat_info.stat.st_mode |= S_IFBLK;
      extractor = extract_node;
      break;
#endif

#if HAVE_MKFIFO || defined mkfifo
    case FIFOTYPE:
      extractor = extract_fifo;
      break;
#endif

    case DIRTYPE:
    case GNUTYPE_DUMPDIR:
      extractor = extract_dir;
      if (current_stat_info.is_dumpdir)
	delay_directory_restore_option = true;
      break;

    case GNUTYPE_VOLHDR:
      return NULL;

    case GNUTYPE_MULTIVOL:
      paxerror (0, _("%s: Cannot extract -- file is continued from another volume"),
		quotearg_colon (current_stat_info.file_name));
      return NULL;

    case GNUTYPE_LONGNAME:
    case GNUTYPE_LONGLINK:
      paxerror (0, _("Unexpected long name header"));
      return NULL;

    default:
      warnopt (WARN_UNKNOWN_CAST, 0,
	       _("%s: Unknown file type '%c', extracted as normal file"),
	       quotearg_colon (file_name), typeflag);
      extractor = extract_file;
    }

  if (to_stdout_option || to_command_option)
    {
      if (extractor != extract_file)
	return NULL;
    }
  else
    {
      switch (old_files_option)
	{
	case UNLINK_FIRST_OLD_FILES:
	  if (!remove_any_file (file_name,
				recursive_unlink_option
				  ? RECURSIVE_REMOVE_OPTION
				  : ORDINARY_REMOVE_OPTION)
	      && errno && errno != ENOENT)
	    {
	      unlink_error (file_name);
	      return NULL;
	    }
	  break;

	case KEEP_NEWER_FILES:
	  if (file_newer_p (file_name, 0, &current_stat_info))
	    {
	      warnopt (WARN_IGNORE_NEWER, 0, _("Current %s is newer or same age"),
		       quote (file_name));
	      return NULL;
	    }
	  break;

	default:
	  break;
	}
    }
  return extractor;
}

/* Extract a file from the archive.  */
void
extract_archive (void)
{
  char typeflag;
  bool skip_dotdot_name;

  fatal_exit_hook = extract_finish;

  set_next_block_after (current_header);

  skip_dotdot_name = (!absolute_names_option
		      && contains_dot_dot (current_stat_info.orig_file_name));
  if (skip_dotdot_name)
    paxerror (0, _("%s: Member name contains '..'"),
	      quotearg_colon (current_stat_info.orig_file_name));

  if (!current_stat_info.file_name[0]
      || skip_dotdot_name
      || (interactive_option
	  && !confirm ("extract", current_stat_info.file_name)))
    {
      skip_member ();
      return;
    }

  /* Print the block from current_header and current_stat.  */
  if (verbose_option)
    print_header (&current_stat_info, current_header, -1);

  /* Restore stats for all non-ancestor directories, unless
     it is an incremental archive.
     (see NOTICE in the comment to delay_set_stat above) */
  if (!delay_directory_restore_option)
    {
      idx_t dir = chdir_current;
      apply_nonancestor_delayed_set_stat (current_stat_info.file_name, false);
      chdir_do (dir);
    }

  /* Take a safety backup of a previously existing file.  */

  if (backup_option)
    if (!maybe_backup_file (current_stat_info.file_name, 0))
      {
	paxerror (errno, _("%s: Was unable to backup this file"),
		  quotearg_colon (current_stat_info.file_name));
	skip_member ();
	return;
      }

  /* Extract the archive entry according to its type.  */
  /* KLUDGE */
  typeflag = (sparse_member_p (&current_stat_info)
	      ? GNUTYPE_SPARSE : current_header->header.typeflag);

  tar_extractor_t fun = prepare_to_extract (current_stat_info.file_name,
					    typeflag);
  if (fun)
    {
      if (fun (current_stat_info.file_name, typeflag))
	return;
    }
  else
    skip_member ();

  if (backup_option)
    undo_last_backup ();
}

/* Extract the link DS whose final extraction was delayed.  */
static void
apply_delayed_link (struct delayed_link *ds)
{
  struct string_list *sources = ds->sources;
  char const *valid_source = 0;

  chdir_do (ds->change_dir);

  for (sources = ds->sources; sources; sources = sources->next)
    {
      char const *source = sources->string;
      struct stat st;

      /* Make sure the placeholder file is still there.  If not,
	 don't create a link, as the placeholder was probably
	 removed by a later extraction.  */
      if (fstatat (chdir_fd, source, &st, AT_SYMLINK_NOFOLLOW) == 0
	  && SAME_INODE (st, *ds)
	  && BIRTHTIME_EQ (get_stat_birthtime (&st), ds->birthtime))
	{
	  /* Unlink the placeholder, then create a hard link if possible,
	     a symbolic link otherwise.  */
	  if (unlinkat (chdir_fd, source, 0) < 0)
	    unlink_error (source);
	  else if (valid_source
		   && (linkat (chdir_fd, valid_source, chdir_fd, source, 0)
		       == 0))
	    ;
	  else if (!ds->is_symlink)
	    {
	      if (linkat (chdir_fd, ds->target, chdir_fd, source, 0) < 0)
		link_error (ds->target, source);
	    }
	  else if (symlinkat (ds->target, chdir_fd, source) < 0)
	    symlink_error (ds->target, source);
	  else
	    {
	      struct tar_stat_info st1;
	      st1.stat.st_mode = ds->mode;
	      st1.stat.st_uid = ds->uid;
	      st1.stat.st_gid = ds->gid;
	      st1.atime = ds->atime;
	      st1.mtime = ds->mtime;
	      st1.cntx_name = ds->cntx_name;
	      st1.acls_a_ptr = ds->acls_a_ptr;
	      st1.acls_a_len = ds->acls_a_len;
	      st1.acls_d_ptr = ds->acls_d_ptr;
	      st1.acls_d_len = ds->acls_d_len;
	      st1.xattr_map = ds->xattr_map;
	      set_stat (source, &st1, -1, 0, 0, SYMTYPE,
			false, AT_SYMLINK_NOFOLLOW);
	      valid_source = source;
	    }
	}
    }

  /* There is little point to freeing, as we are about to exit,
     and freeing is more likely to cause than cure trouble.  */
  if (false)
    {
      for (sources = ds->sources; sources; )
	{
	  struct string_list *next = sources->next;
	  free (sources);
	  sources = next;
	}

      xattr_map_free (&ds->xattr_map);
      free (ds->cntx_name);
    }
}

/* Extract the links whose final extraction were delayed.  */
static void
apply_delayed_links (void)
{
  for (struct delayed_link *ds = delayed_link_head; ds; ds = ds->next)
    apply_delayed_link (ds);

  if (false && delayed_link_table)
    {
      /* There is little point to freeing, as we are about to exit,
	 and freeing is more likely to cause than cure trouble.
	 Also, the above code has not bothered to free the list
	 in delayed_link_head.  */
      hash_free (delayed_link_table);
      delayed_link_table = NULL;
    }
}

/* Finish the extraction of an archive.  */
void
extract_finish (void)
{
  /* First, fix the status of ordinary directories that need fixing.  */
  apply_nonancestor_delayed_set_stat ("", false);

  /* Then, apply delayed links, so that they don't affect delayed
     directory status-setting for ordinary directories.  */
  apply_delayed_links ();

  /* Finally, fix the status of directories that are ancestors
     of delayed links.  */
  apply_nonancestor_delayed_set_stat ("", true);

  /* This table should be empty after apply_nonancestor_delayed_set_stat.  */
  if (false && delayed_set_stat_table)
    {
      /* There is little point to freeing, as we are about to exit,
	 and freeing is more likely to cause than cure trouble.  */
      hash_free (delayed_set_stat_table);
      delayed_set_stat_table = NULL;
    }
}

bool
rename_directory (char *src, char *dst)
{
  if (renameat (chdir_fd, src, chdir_fd, dst) == 0)
    fixup_delayed_set_stat (src, dst);
  else
    {
      int e = errno;

      switch (e)
	{
	case ENOENT:
	  if (make_directories (dst, NULL) == 0)
	    {
	      if (renameat (chdir_fd, src, chdir_fd, dst) == 0)
		return true;
	      e = errno;
	    }
	  break;

	case EXDEV:
	  /* FIXME: Fall back to recursive copying */

	default:
	  break;
	}

      paxerror (e, _("Cannot rename %s to %s"),
		quote_n (0, src),
		quote_n (1, dst));
      return false;
    }
  return true;
}
