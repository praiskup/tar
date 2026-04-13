/* Copyright 2026 Free Software Foundation, Inc.

   This program is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the
   Free Software Foundation; either version 3 of the License, or (at your
   option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program. If not, see <http://www.gnu.org/licenses/>.

   Written by Paul Eggert.  */

#include <config.h>

#include "mkdtempat.h"

#include <tempname.h>

#include <stddef.h>
#include <sys/stat.h>

static int
try_dir (char *tmpl, void *flags)
{
  int *pdirfd = flags;
  return mkdirat (*pdirfd, tmpl, S_IRUSR | S_IWUSR | S_IXUSR);
}

/* Relative to the directory DIRFD if XTEMPLATE is relative,
   generate a unique temporary directory from XTEMPLATE.
   The last six characters of XTEMPLATE must be "XXXXXX";
   replace them with a string that makes the generated directory unique.
   Create the directory mode 700, and return its name.
   On failure, return NULL and set errno.  */
char *
mkdtempat (int dirfd, char *xtemplate)
{
  return (try_tempname_len (xtemplate, 0, &dirfd, try_dir, 6) < 0
	  ? NULL : xtemplate);
}
