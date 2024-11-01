/* This file is part of GNU tar.
   Copyright 2007-2024 Free Software Foundation, Inc.

   Written by Sergey Poznyakoff.

   GNU tar is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the
   Free Software Foundation; either version 3, or (at your option) any later
   version.

   GNU tar is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
   Public License for more details.

   You should have received a copy of the GNU General Public License along
   with GNU tar.  If not, see <http://www.gnu.org/licenses/>.  */

#include <system.h>
#include "common.h"

struct compression_suffix
{
  const char *suffix;
  idx_t length;
  const char *program;
};

static struct compression_suffix compression_suffixes[] = {
#define __CAT2__(a,b) a ## b
#define S(s,p) #s, sizeof (#s) - 1, __CAT2__(p,_PROGRAM)
  { "tar", 3, NULL },
  { S(gz,   GZIP) },
  { S(z,    GZIP) },
  { S(tgz,  GZIP) },
  { S(taz,  GZIP) },
  { S(Z,    COMPRESS) },
  { S(taZ,  COMPRESS) },
  { S(bz2,  BZIP2) },
  { S(tbz,  BZIP2) },
  { S(tbz2, BZIP2) },
  { S(tz2,  BZIP2) },
  { S(lz,   LZIP) },
  { S(lzma, LZMA) },
  { S(tlz,  LZMA) },
  { S(lzo,  LZOP) },
  { S(tzo,  LZOP) },
  { S(xz,   XZ) },
  { S(txz,  XZ) }, /* Slackware */
  { S(zst,  ZSTD) },
  { S(tzst, ZSTD) },
  { NULL }
#undef S
#undef __CAT2__
};

/* Extract the suffix from archive file NAME, and return a pointer to
   compression_suffix associated with it or NULL if none is found.
   No matter what is the return value, if RET_LEN is not NULL, store
   there the length of NAME with that suffix stripped, or 0 if NAME has
   no suffix. */
static struct compression_suffix const *
find_compression_suffix (const char *name, idx_t *ret_len)
{
  char *suf = strrchr (name, '.');

  if (suf && suf[1] != 0 && suf[1] != '/')
    {
      idx_t len;
      struct compression_suffix *p;

      suf++;
      len = strlen (suf);
      if (ret_len)
	*ret_len = strlen (name) - len - 1;

      for (p = compression_suffixes; p->suffix; p++)
	{
	  if (p->length == len && memcmp (p->suffix, suf, len) == 0)
	    {
	      return p;
	    }
	}
    }
  else if (ret_len)
    *ret_len = 0;
  return NULL;
}

/* Select compression program using the suffix of the archive file NAME.
   Use DEFPROG, if there is no suffix, or if no program is associated with
   the suffix.  In the latter case, if VERBOSE is true, issue a warning.
 */
void
set_compression_program_by_suffix (const char *name, const char *defprog,
				   bool verbose)
{
  idx_t len;
  struct compression_suffix const *p = find_compression_suffix (name, &len);
  if (p)
    use_compress_program_option = p->program;
  else
    {
      use_compress_program_option = defprog;
      if (len > 0 && verbose)
	paxwarn (0,
		 _("no compression program is defined for suffix '%s';"
		   " assuming %s"),
		 name + len,
		 defprog ? defprog : "uncompressed archive");
    }
}

char *
strip_compression_suffix (const char *name)
{
  char *s = NULL;
  idx_t len;
  struct compression_suffix const *p = find_compression_suffix (name, &len);

  if (p)
    {
      /* Strip an additional ".tar" suffix, but only if the just-stripped
	 "outer" suffix did not begin with "t".  */
      if (len > 4 && strncmp (name + len - 4, ".tar", 4) == 0
	  && p->suffix[0] != 't')
	len -= 4;
      if (len == 0)
	return NULL;
      s = xmalloc (len + 1);
      memcpy (s, name, len);
      s[len] = 0;
    }
  return s;
}
