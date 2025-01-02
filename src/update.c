/* Update a tar archive.

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
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

/* Implement the 'r', 'u' and 'A' options for tar.  'A' means that the
   file names are tar files, and they should simply be appended to the end
   of the archive.  No attempt is made to record the reads from the args; if
   they're on raw tape or something like that, it'll probably lose...  */

#include <system.h>
#include <quotearg.h>
#include "common.h"

/* We've hit the end of the old stuff, and its time to start writing new
   stuff to the tape.  This involves seeking back one record and
   re-writing the current record (which has been changed).
   FIXME: Either eliminate it or move it to common.h.
*/
bool time_to_start_writing;

/* Pointer to where we started to write in the first record we write out.
   This is used if we can't backspace the output and have to null out the
   first part of the record.  */
char *output_start;

static bool acting_as_filter;

/* Catenate file FILE_NAME to the archive without creating a header for it.
   It had better be a tar file or the archive is screwed.  */
static void
append_file (char *file_name)
{
  int handle = openat (chdir_fd, file_name, O_RDONLY | O_BINARY);

  if (handle < 0)
    {
      open_error (file_name);
      return;
    }

  while (true)
    {
      union block *start = find_next_block ();
      idx_t bufsize = available_space_after (start);
      idx_t status = full_read (handle, start->buffer, bufsize);
      if (status < bufsize && errno)
	read_fatal (file_name);
      if (status == 0)
	break;
      idx_t rem = status % BLOCKSIZE;
      if (rem)
	memset (start->buffer + (status - rem), 0, BLOCKSIZE - rem);
      set_next_block_after (start + ((status - 1) >> LG_BLOCKSIZE));
    }

  if (close (handle) < 0)
    close_error (file_name);
}

/* If NAME is not a pattern, remove it from the namelist.  Otherwise,
   remove the FILE_NAME that matched it.  Take care to look for exact
   match when removing it. */
static void
remove_exact_name (struct name *name, char const *file_name)
{
  if (name->is_wildcard)
    {
      struct name *match = name_scan (file_name, true);
      name->found_count++;
      if (match)
	name = match;
      else
	return;
    }

  remname (name);
}

/* Implement the 'r' (add files to end of archive), and 'u' (add files
   to end of archive if they aren't there, or are more up to date than
   the version in the archive) commands.  */
void
update_archive (void)
{
  enum read_header previous_status = HEADER_STILL_UNREAD;
  bool found_end = false;

  name_gather ();
  open_archive (ACCESS_UPDATE);
  acting_as_filter = strcmp (archive_name_array[0], "-") == 0;
  xheader_forbid_global ();

  while (!found_end)
    {
      enum read_header status = read_header (&current_header,
                                             &current_stat_info,
                                             read_header_auto);

      switch (status)
	{
	case HEADER_STILL_UNREAD:
	case HEADER_SUCCESS_EXTENDED:
	  abort ();

	case HEADER_SUCCESS:
	  {
	    struct name *name;

	    decode_header (current_header, &current_stat_info,
			   &current_format, false);
	    transform_stat_info (current_header->header.typeflag,
				 &current_stat_info);
	    archive_format = current_format;

	    if (subcommand_option == UPDATE_SUBCOMMAND
		&& (name = name_scan (current_stat_info.file_name, false)) != NULL)
	      {
		struct stat s;

		chdir_do (name->change_dir);
		if (deref_stat (current_stat_info.file_name, &s) == 0)
		  {
		    if (S_ISDIR (s.st_mode))
		      {
			char *p;
			char *dirp = tar_savedir (current_stat_info.file_name,
						  true);
			if (dirp)
			  {
			    namebuf_t nbuf = namebuf_create (current_stat_info.file_name);

			    for (p = dirp; *p; p += strlen (p) + 1)
			      addname (namebuf_name (nbuf, p),
				       name->change_dir, false, NULL);

			    namebuf_free (nbuf);
			    free (dirp);

			    remove_exact_name (name, current_stat_info.file_name);
			  }
		      }
		    else if (tar_timespec_cmp (get_stat_mtime (&s),
					       current_stat_info.mtime)
			     <= 0)
		      {
			remove_exact_name (name, current_stat_info.file_name);
		      }
		    else if (name->is_wildcard)
		      addname (current_stat_info.file_name,
			       name->change_dir, false, NULL);
		  }
	      }

	    skim_member (acting_as_filter);
	    break;
	  }

	case HEADER_ZERO_BLOCK:
	  current_block = current_header;
	  found_end = true;
	  break;

	case HEADER_END_OF_FILE:
	  found_end = true;
	  break;

	case HEADER_FAILURE:
	  set_next_block_after (current_header);
	  switch (previous_status)
	    {
	    case HEADER_STILL_UNREAD:
	      paxwarn (0, _("This does not look like a tar archive"));
	      FALLTHROUGH;
	    case HEADER_SUCCESS:
	    case HEADER_ZERO_BLOCK:
	      paxerror (0, _("Skipping to next header"));
	      FALLTHROUGH;
	    case HEADER_FAILURE:
	      break;

	    case HEADER_END_OF_FILE:
	    case HEADER_SUCCESS_EXTENDED:
	      abort ();
	    }
	  break;
	}

      tar_stat_destroy (&current_stat_info);
      previous_status = status;
    }

  reset_eof ();
  time_to_start_writing = true;
  output_start = current_block->buffer;

  {
    struct name const *p;
    while ((p = name_from_list ()) != NULL)
      {
	char *file_name = p->name;
	if (excluded_name (file_name, NULL))
	  continue;
	if (interactive_option && !confirm ("add", file_name))
	  continue;
	if (subcommand_option == CAT_SUBCOMMAND)
	  append_file (file_name);
	else
	  dump_file (0, file_name, file_name);
      }
  }

  write_eot ();
  close_archive ();
  finish_deferred_unlinks ();
  names_notfound ();
}
