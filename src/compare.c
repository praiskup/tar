/* Diff files from a tar archive.

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

   Written by John Gilmore, on 1987-04-30.  */

#include <system.h>

#if HAVE_LINUX_FD_H
# include <linux/fd.h>
#endif

#if HAVE_SYS_MTIO_H
# include <sys/ioctl.h>
# include <sys/mtio.h>
#endif

#include "common.h"
#include <alignalloc.h>
#include <quotearg.h>
#include <rmt.h>
#include <stdarg.h>

/* Nonzero if we are verifying at the moment.  */
bool now_verifying;

/* File descriptor for the file we are diffing.  */
static int diff_handle;

/* Area for reading file contents into.  */
static char *diff_buffer;

/* Initialize for a diff operation.  */
void
diff_init (void)
{
  diff_buffer = xalignalloc (getpagesize (), record_size);
  if (listed_incremental_option)
    read_directory_file ();
}

enum { QUOTE_ARG, QUOTE_NAME };

/* Sigh about something that differs by writing a MESSAGE to stdlis,
   given MESSAGE is nonzero.  Also set the exit status if not already.  */
void
report_difference (struct tar_stat_info *st, const char *fmt, ...)
{
  if (fmt)
    {
      va_list ap;

      fprintf (stdlis, "%s: ", quote_n_colon (QUOTE_NAME, st->file_name));
      va_start (ap, fmt);
      vfprintf (stdlis, fmt, ap);
      va_end (ap);
      fprintf (stdlis, "\n");
    }

  set_exit_status (TAREXIT_DIFFERS);
}

/* Take a buffer returned by read_and_process and do nothing with it.  */
static bool
process_noop (MAYBE_UNUSED idx_t size, MAYBE_UNUSED char *data)
{
  return true;
}

static bool
process_rawdata (idx_t bytes, char *buffer)
{
  idx_t status = blocking_read (diff_handle, diff_buffer, bytes);

  if (status < bytes)
    {
      if (errno)
	{
	  read_error (current_stat_info.file_name);
	  report_difference (&current_stat_info, NULL);
	}
      else
	{
	  report_difference (&current_stat_info,
			     ngettext ("Could read only %td of %td byte",
				       "Could read only %td of %td bytes",
				       bytes),
			     status, bytes);
	}
      return false;
    }

  if (memcmp (buffer, diff_buffer, bytes) != 0)
    {
      report_difference (&current_stat_info, _("Contents differ"));
      return false;
    }

  return true;
}

/* Some other routine wants ST->stat.st_size bytes in the archive.
   For each chunk of the archive, call PROCESSOR with the size of the chunk,
   and the address of the chunk it can work with.  PROCESSOR should return
   true for success.  Once it fails, continue skipping without calling
   PROCESSOR anymore.  */

static void
read_and_process (struct tar_stat_info *st, bool (*processor) (idx_t, char *))
{
  mv_begin_read (st);
  for (off_t size = st->stat.st_size; size; )
    {
      union block *data_block = find_next_block ();
      if (! data_block)
	{
	  paxerror (0, _("Unexpected EOF in archive"));
	  return;
	}

      idx_t data_size = available_space_after (data_block);
      if (data_size > size)
	data_size = size;
      if (!processor (data_size, data_block->buffer))
	processor = process_noop;
      set_next_block_after ((union block *)
			    (data_block->buffer + data_size - 1));
      size -= data_size;
      mv_size_left (size);
    }
  mv_end ();
}

/* Call either stat or lstat over STAT_DATA, depending on
   --dereference (-h), for a file which should exist.  Diagnose any
   problem.  Return true for success, false otherwise.  */
static bool
get_stat_data (char const *file_name, struct stat *stat_data)
{
  if (deref_stat (file_name, stat_data) < 0)
    {
      (errno == ENOENT ? stat_warn : stat_error) (file_name);
      report_difference (&current_stat_info, NULL);
      return false;
    }

  return true;
}


static void
diff_dir (void)
{
  struct stat stat_data;

  if (!get_stat_data (current_stat_info.file_name, &stat_data))
    return;

  if (!S_ISDIR (stat_data.st_mode))
    report_difference (&current_stat_info, _("File type differs"));
  else if ((current_stat_info.stat.st_mode & MODE_ALL) !=
	   (stat_data.st_mode & MODE_ALL))
    report_difference (&current_stat_info, _("Mode differs"));
}

static void
diff_file (void)
{
  char const *file_name = current_stat_info.file_name;
  struct stat stat_data;

  if (!get_stat_data (file_name, &stat_data))
    skip_member ();
  else if (!S_ISREG (stat_data.st_mode))
    {
      report_difference (&current_stat_info, _("File type differs"));
      skip_member ();
    }
  else
    {
      if ((current_stat_info.stat.st_mode & MODE_ALL) !=
	  (stat_data.st_mode & MODE_ALL))
	report_difference (&current_stat_info, _("Mode differs"));

      if (!sys_compare_uid (&stat_data, &current_stat_info.stat))
	report_difference (&current_stat_info, _("Uid differs"));
      if (!sys_compare_gid (&stat_data, &current_stat_info.stat))
	report_difference (&current_stat_info, _("Gid differs"));

      if (tar_timespec_cmp (get_stat_mtime (&stat_data),
                            current_stat_info.mtime))
	report_difference (&current_stat_info, _("Mod time differs"));
      if (current_header->header.typeflag != GNUTYPE_SPARSE
	  && stat_data.st_size != current_stat_info.stat.st_size)
	{
	  report_difference (&current_stat_info, _("Size differs"));
	  skip_member ();
	}
      else
	{
	  diff_handle = openat (chdir_fd, file_name, open_read_flags);

	  if (diff_handle < 0)
	    {
	      open_error (file_name);
	      skip_member ();
	      report_difference (&current_stat_info, NULL);
	    }
	  else
	    {
	      if (current_stat_info.is_sparse)
		sparse_diff_file (diff_handle, &current_stat_info);
	      else
		read_and_process (&current_stat_info, process_rawdata);

	      if (atime_preserve_option == replace_atime_preserve
		  && stat_data.st_size != 0)
		{
		  struct timespec atime = get_stat_atime (&stat_data);
		  if (set_file_atime (diff_handle, chdir_fd, file_name, atime)
		      < 0)
		    utime_error (file_name);
		}

	      if (close (diff_handle) < 0)
		close_error (file_name);
	    }
	}
    }
}

static void
diff_link (void)
{
  struct stat file_data;
  struct stat link_data;

  if (get_stat_data (current_stat_info.file_name, &file_data)
      && get_stat_data (current_stat_info.link_name, &link_data)
      && !sys_compare_links (&file_data, &link_data))
    report_difference (&current_stat_info,
		       _("Not linked to %s"),
		       quote_n_colon (QUOTE_ARG,
				      current_stat_info.link_name));
}

static void
diff_symlink (void)
{
  char buf[1024];
  idx_t len = strlen (current_stat_info.link_name);
  char *linkbuf = len < sizeof buf ? buf : xmalloc (len + 1);

  ssize_t status = readlinkat (chdir_fd, current_stat_info.file_name,
			       linkbuf, len + 1);

  if (status < 0)
    {
      if (errno == ENOENT)
	readlink_warn (current_stat_info.file_name);
      else
	readlink_error (current_stat_info.file_name);
      report_difference (&current_stat_info, NULL);
    }
  else if (status != len
	   || memcmp (current_stat_info.link_name, linkbuf, len) != 0)
    report_difference (&current_stat_info, _("Symlink differs"));

  if (linkbuf != buf)
    free (linkbuf);
}

static void
diff_special (void)
{
  struct stat stat_data;

  /* FIXME: deal with umask.  */

  if (!get_stat_data (current_stat_info.file_name, &stat_data))
    return;

  if (current_header->header.typeflag == CHRTYPE
      ? !S_ISCHR (stat_data.st_mode)
      : current_header->header.typeflag == BLKTYPE
      ? !S_ISBLK (stat_data.st_mode)
      : /* current_header->header.typeflag == FIFOTYPE */
      !S_ISFIFO (stat_data.st_mode))
    {
      report_difference (&current_stat_info, _("File type differs"));
      return;
    }

  if ((current_header->header.typeflag == CHRTYPE
       || current_header->header.typeflag == BLKTYPE)
      && current_stat_info.stat.st_rdev != stat_data.st_rdev)
    {
      report_difference (&current_stat_info, _("Device number differs"));
      return;
    }

  if ((current_stat_info.stat.st_mode & MODE_ALL) !=
      (stat_data.st_mode & MODE_ALL))
    report_difference (&current_stat_info, _("Mode differs"));
}

/* Return zero if and only if A and B should be considered equal.
   for the purposes of dump directory comparison.  */
static char
dumpdir_cmp (const char *a, const char *b)
{
  while (*a)
    switch (*a)
      {
      case 'Y':
      case 'N':
	/* If the null-terminated strings A and B are equal, other
	   than possibly A's first byte being 'Y' where B's is 'N' or
	   vice versa, advance A and B past the strings.
	   Otherwise, return 1.  */
	if (! (*b == 'Y' || *b == 'N'))
	  return 1;
	a++, b++;
	FALLTHROUGH;
      case 'D':
	/* If the null-terminated strings A and B are equal, advance A
	   and B past them.  Otherwise, return 1.  */
	while (*a)
	  if (*a++ != *b++)
	    return 1;
	if (*b)
	  return 1;
	a++, b++;
	break;

      case 'R':
      case 'T':
      case 'X':
	return *b;

      default:
	unreachable ();
      }
  return *b;
}

static void
diff_dumpdir (struct tar_stat_info *dir)
{
  const char *dumpdir_buffer;

  if (dir->fd == 0)
    {
      void (*diag) (char const *) = NULL;
      int fd = subfile_open (dir->parent, dir->orig_file_name, open_read_flags);
      if (fd < 0)
	diag = open_diag;
      else if (fstat (fd, &dir->stat) < 0)
        {
	  diag = stat_diag;
          close (fd);
        }
      else
	dir->fd = fd;
      if (diag)
	{
	  file_removed_diag (dir->orig_file_name, false, diag);
	  return;
	}
    }
  dumpdir_buffer = directory_contents (scan_directory (dir));

  if (dumpdir_buffer)
    {
      if (dumpdir_cmp (dir->dumpdir, dumpdir_buffer) != 0)
	report_difference (dir, _("Contents differ"));
    }
  else
    read_and_process (dir, process_noop);
}

static void
diff_multivol (void)
{
  if (current_stat_info.had_trailing_slash)
    {
      diff_dir ();
      return;
    }

  struct stat stat_data;
  if (!get_stat_data (current_stat_info.file_name, &stat_data))
    return;

  if (!S_ISREG (stat_data.st_mode))
    {
      report_difference (&current_stat_info, _("File type differs"));
      skip_member ();
      return;
    }

  off_t offset = OFF_FROM_HEADER (current_header->oldgnu_header.offset);
  off_t file_size;
  if (offset < 0
      || ckd_add (&file_size, current_stat_info.stat.st_size, offset)
      || stat_data.st_size != file_size)
    {
      report_difference (&current_stat_info, _("Size differs"));
      skip_member ();
      return;
    }


  int fd = openat (chdir_fd, current_stat_info.file_name, open_read_flags);

  if (fd < 0)
    {
      open_error (current_stat_info.file_name);
      report_difference (&current_stat_info, NULL);
      skip_member ();
      return;
    }

  if (lseek (fd, offset, SEEK_SET) < 0)
    {
      seek_error_details (current_stat_info.file_name, offset);
      report_difference (&current_stat_info, NULL);
    }
  else
    read_and_process (&current_stat_info, process_rawdata);

  if (close (fd) < 0)
    close_error (current_stat_info.file_name);
}

/* Diff a file against the archive.  */
void
diff_archive (void)
{

  set_next_block_after (current_header);

  /* Print the block from current_header and current_stat_info.  */

  if (verbose_option)
    {
      if (now_verifying)
	fprintf (stdlis, _("Verify "));
      print_header (&current_stat_info, current_header, -1);
    }

  switch (current_header->header.typeflag)
    {
    default:
      paxerror (0, _("%s: Unknown file type '%c', diffed as normal file"),
		quotearg_colon (current_stat_info.file_name),
		current_header->header.typeflag);
      FALLTHROUGH;
    case AREGTYPE:
    case REGTYPE:
    case GNUTYPE_SPARSE:
    case CONTTYPE:

      /* Appears to be a file.  See if it's really a directory.  */

      if (current_stat_info.had_trailing_slash)
	diff_dir ();
      else
	diff_file ();
      break;

    case LNKTYPE:
      diff_link ();
      break;

    case SYMTYPE:
      diff_symlink ();
      break;

    case CHRTYPE:
    case BLKTYPE:
    case FIFOTYPE:
      diff_special ();
      break;

    case GNUTYPE_DUMPDIR:
    case DIRTYPE:
      if (is_dumpdir (&current_stat_info))
	diff_dumpdir (&current_stat_info);
      diff_dir ();
      break;

    case GNUTYPE_VOLHDR:
      break;

    case GNUTYPE_MULTIVOL:
      diff_multivol ();
    }
}

void
verify_volume (void)
{
  bool may_fail = false;
  if (removed_prefixes_p ())
    {
      paxwarn (0,
	       _("Archive contains file names with leading prefixes removed."));
      may_fail = true;
    }
  if (transform_program_p ())
    {
      paxwarn (0, _("Archive contains transformed file names."));
      may_fail = true;
    }
  if (may_fail)
    paxwarn (0, _("Verification may fail to locate original files."));

  clear_directory_table ();

  if (!diff_buffer)
    diff_init ();

  /* Verifying an archive is meant to check if the physical media got it
     correctly, so try to defeat clever in-memory buffering pertaining to
     this particular media.  On Linux, for example, the floppy drive would
     not even be accessed for the whole verification.

     The code was using fsync only when the ioctl is unavailable, but
     Marty Leisner says that the ioctl does not work when not preceded by
     fsync.  So, until we know better, or maybe to please Marty, let's do it
     the unbelievable way :-).  */

#if HAVE_FSYNC
  fsync (archive);
#endif
#ifdef FDFLUSH
  ioctl (archive, FDFLUSH);
#endif

  if (!mtioseek (true, -1) && rmtlseek (archive, 0, SEEK_SET) < 0)
    {
      /* Lseek failed.  Try a different method.  */
      seek_warn (archive_name_array[0]);
      return;
    }

  access_mode = ACCESS_READ;
  now_verifying = 1;

  flush_read ();
  while (1)
    {
      enum read_header status = read_header (&current_header,
                                             &current_stat_info,
                                             read_header_auto);

      if (status == HEADER_FAILURE)
	{
	  intmax_t counter = 0;

	  do
	    {
	      counter++;
	      set_next_block_after (current_header);
	      status = read_header (&current_header, &current_stat_info,
	                            read_header_auto);
	    }
	  while (status == HEADER_FAILURE);

	  paxerror (0,
		    ngettext ("VERIFY FAILURE: %jd invalid header detected",
			      "VERIFY FAILURE: %jd invalid headers detected",
			      counter),
		    counter);
	}
      if (status == HEADER_END_OF_FILE)
	break;
      if (status == HEADER_ZERO_BLOCK)
	{
	  set_next_block_after (current_header);
          if (!ignore_zeros_option)
            {
	      status = read_header (&current_header, &current_stat_info,
	                            read_header_auto);
	      if (status == HEADER_ZERO_BLOCK)
	        break;
	      warnopt (WARN_ALONE_ZERO_BLOCK,
		       0, _("A lone zero block at %jd"),
		       intmax (current_block_ordinal ()));
            }
	  continue;
	}

      decode_header (current_header, &current_stat_info, &current_format, true);
      diff_archive ();
      tar_stat_destroy (&current_stat_info);
    }

  access_mode = ACCESS_WRITE;
  now_verifying = 0;
}
