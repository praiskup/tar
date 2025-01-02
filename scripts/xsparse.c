/* xsparse - expands compressed sparse file images extracted from GNU tar
   archives.

   Copyright 2006-2025 Free Software Foundation, Inc.

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

   Written by Sergey Poznyakoff  */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Bound on length of the string representing an off_t.
   See INT_STRLEN_BOUND in intprops.h for explanation */
#define OFF_T_STRLEN_BOUND ((sizeof (off_t) * CHAR_BIT) * 146 / 485 + 1)
#define OFF_T_STRSIZE_BOUND (OFF_T_STRLEN_BOUND+1)

#define BLOCKSIZE 512

struct sp_array
{
  off_t offset;
  off_t numbytes;
};

static char *progname;
static bool verbose;

static void
die (int code, char *fmt, ...)
{
  va_list ap;

  fprintf (stderr, "%s: ", progname);
  va_start (ap, fmt);
  vfprintf (stderr, fmt, ap);
  va_end (ap);
  fprintf (stderr, "\n");
  exit (code);
}

static void *
emalloc (size_t size)
{
  char *p = malloc (size);
  if (!p && size)
    die (1, "not enough memory");
  return p;
}

static off_t
string_to_off (char *p, char **endp)
{
  errno = 0;
  intmax_t i = strtoimax (p, endp, 10);
  off_t v = i;
  if (i < 0 || v != i || errno == ERANGE)
    die (1, "number out of allowed range, near %s", p);
  if (errno || p == *endp)
    die (1, "number parse error near %s", p);
  return v;
}

static size_t
string_to_size (char *p, char **endp, size_t maxsize)
{
  off_t v = string_to_off (p, endp);
  size_t ret = v;
  if (! (ret == v && ret <= maxsize))
    die (1, "number too big");
  return ret;
}

static size_t sparse_map_size;
static struct sp_array *sparse_map;

static void
get_line (char *s, int size, FILE *stream)
{
  char *p = fgets (s, size, stream);
  size_t len;

  if (!p)
    die (1, "unexpected end of file");
  len = strlen (p);
  if (s[len - 1] != '\n')
    die (1, "invalid or too-long data");
  s[len - 1] = '\0';
}

static bool
get_var (FILE *fp, char **name, char **value)
{
  static char *buffer;
  static size_t bufsize = OFF_T_STRSIZE_BOUND;
  char *p, *q;

  buffer = emalloc (bufsize);
  do
    {
      size_t len, s;

      if (!fgets (buffer, bufsize, fp))
	return false;
      len = strlen (buffer);
      if (len == 0)
	return false;

      s = string_to_size (buffer, &p, SIZE_MAX - 1);
      if (*p != ' ')
	die (1, "malformed header: expected space but found %s", p);
      if (buffer[len-1] != '\n')
	{
	  if (bufsize < s + 1)
	    {
	      bufsize = s + 1;
	      buffer = realloc (buffer, bufsize);
	      if (!buffer)
		die (1, "not enough memory");
	    }
	  if (!fgets (buffer + len, s - len + 1, fp))
	    die (1, "unexpected end of file or read error");
	}
      p++;
    }
  while (strncmp (p, "GNU.sparse.", 11) != 0);

  p += 11;
  q = strchr (p, '=');
  if (!q)
    die (1, "malformed header: expected '=' not found");
  *q++ = 0;
  q[strlen (q) - 1] = 0;
  *name = p;
  *value = q;
  return true;
}

static char *outname;
static off_t outsize;
static off_t version_major;
static off_t version_minor;

static void
read_xheader (char *name)
{
  char *kw, *val;
  FILE *fp = fopen (name, "r");
  size_t i = 0;

  if (verbose)
    printf ("Reading extended header file\n");

  while (get_var (fp, &kw, &val))
    {
      if (verbose)
	printf ("Found variable GNU.sparse.%s = %s\n", kw, val);

      if (strcmp (kw, "name") == 0)
	{
	  outname = emalloc (strlen (val) + 1);
	  strcpy (outname, val);
	}
      else if (strcmp (kw, "major") == 0)
	{
	  version_major = string_to_off (val, NULL);
	}
      else if (strcmp (kw, "minor") == 0)
	{
	  version_minor = string_to_off (val, NULL);
	}
      else if (strcmp (kw, "realsize") == 0
	       || strcmp (kw, "size") == 0)
	{
	  outsize = string_to_off (val, NULL);
	}
      else if (strcmp (kw, "numblocks") == 0)
	{
	  sparse_map_size = string_to_size (val, NULL,
					    SIZE_MAX / sizeof *sparse_map);
	  if (sparse_map_size)
	    {
	      sparse_map = emalloc (sparse_map_size * sizeof *sparse_map);
	      sparse_map[0].offset = -1;
	    }
	}
      else if (strcmp (kw, "offset") == 0)
	{
	  if (sparse_map_size <= i)
	    die (1, "bad GNU.sparse.map: spurious offset");
	  sparse_map[i].offset = string_to_off (val, NULL);
	}
      else if (strcmp (kw, "numbytes") == 0)
	{
	  if (sparse_map_size <= i || sparse_map[i].offset < 0)
	    die (1, "bad GNU.sparse.map: spurious numbytes");
	  sparse_map[i++].numbytes = string_to_off (val, NULL);
	  if (i < sparse_map_size)
	    sparse_map[i].offset = -1;
	}
      else if (strcmp (kw, "map") == 0)
	{
	  for (i = 0; i < sparse_map_size; i++)
	    {
	      sparse_map[i].offset = string_to_off (val, &val);
	      if (*val != ',')
		die (1, "bad GNU.sparse.map: expected ',' but found '%c'",
		     *val);
	      sparse_map[i].numbytes = string_to_off (val+1, &val);
	      if (*val != ',')
		{
		  if (!(*val == 0 && i == sparse_map_size-1))
		    die (1, "bad GNU.sparse.map: expected ',' but found '%c'",
			 *val);
		}
	      else
		val++;
	    }
	  if (*val)
	    die (1, "bad GNU.sparse.map: garbage at the end");
	}
    }
  if (version_major == 0 && sparse_map_size == 0)
    die (1, "size of the sparse map unknown");
  if (i != sparse_map_size)
    die (1, "not all sparse entries supplied");
  if (ferror (fp) || fclose (fp) < 0)
    die (1, "read error: %s", name);
}

static void
read_map (FILE *ifp)
{
  size_t i;
  char nbuf[OFF_T_STRSIZE_BOUND];

  if (verbose)
    printf ("Reading v.1.0 sparse map\n");

  get_line (nbuf, sizeof nbuf, ifp);
  sparse_map_size = string_to_size (nbuf, NULL, SIZE_MAX / sizeof *sparse_map);
  sparse_map = emalloc (sparse_map_size * sizeof *sparse_map);

  for (i = 0; i < sparse_map_size; i++)
    {
      get_line (nbuf, sizeof nbuf, ifp);
      sparse_map[i].offset = string_to_off (nbuf, NULL);
      get_line (nbuf, sizeof nbuf, ifp);
      sparse_map[i].numbytes = string_to_off (nbuf, NULL);
    }

  off_t ifp_offset = ftello (ifp);
  if (ifp_offset < 0)
    die (1, "ftello");
  if (ifp_offset % BLOCKSIZE != 0
      && fseeko (ifp, BLOCKSIZE - ifp_offset % BLOCKSIZE, SEEK_CUR) < 0)
    die (1, "fseeko");
}

static void
expand_sparse (FILE *sfp, int ofd)
{
  size_t i;

  for (i = 0; i < sparse_map_size; i++)
    {
      off_t size = sparse_map[i].numbytes;

      if (size == 0)
	{
	  if (0 <= ofd && ftruncate (ofd, sparse_map[i].offset) < 0)
	    die (1, "ftruncate error (%d)", errno);
	}
      else
	{
	  if (0 <= ofd && lseek (ofd, sparse_map[i].offset, SEEK_SET) < 0)
	    die (1, "lseek error (%d)", errno);
	  while (size)
	    {
	      char buffer[BUFSIZ];
	      size_t rdsize = size < BUFSIZ ? size : BUFSIZ;
	      if (rdsize != fread (buffer, 1, rdsize, sfp))
		die (1, "read error (%d)", errno);
	      if (0 <= ofd)
		{
		  ssize_t written = write (ofd, buffer, rdsize);
		  if (written != rdsize)
		    die (1, "write error (%d)", written < 0 ? errno : 0);
		}
	      size -= rdsize;
	    }
	}
    }
}

static void
usage (int code)
{
  printf ("Usage: %s [OPTIONS] infile [outfile]\n", progname);
  printf ("%s: expand sparse files extracted from GNU archives\n",
	  progname);
  printf ("\nOPTIONS are:\n\n");
  printf ("  -h           Display this help list\n");
  printf ("  -n           Dry run: do nothing, print what would have been done\n");
  printf ("  -v           Increase verbosity level\n");
  printf ("  -x FILE      Parse extended header FILE\n\n");

  exit (code);
}

static void
guess_outname (char *name)
{
  char *base = strrchr (name, '/');
  base = base ? base + 1 : name;
  size_t dirlen = base - name, baselen = strlen (base);
  static char const parentdir[] = "../";
  int parentdirlen = sizeof parentdir - 1;
  outname = emalloc (dirlen + parentdirlen + baselen + 1);
  memcpy (outname, name, dirlen);
  memcpy (outname + dirlen, parentdir, parentdirlen);
  memcpy (outname + dirlen + parentdirlen, base, baselen + 1);
}

int
main (int argc, char **argv)
{
  int c;
  bool dry_run = false;
  char *xheader_file = NULL;

  progname = argv[0];
  while ((c = getopt (argc, argv, "hnvx:")) != EOF)
    {
      switch (c)
	{
	case 'h':
	  usage (0);
	  break;

	case 'x':
	  xheader_file = optarg;
	  break;

	case 'n':
	  dry_run = true;
	case 'v':
	  verbose = true;
	  break;

	default:
	  exit (1);
	}
    }

  argc -= optind;
  argv += optind;

  if (argc == 0 || argc > 2)
    usage (1);

  if (xheader_file)
    read_xheader (xheader_file);

  char *inname = argv[0];
  if (argv[1])
    outname = argv[1];

  struct stat st;
  if (stat (inname, &st) < 0)
    die (1, "cannot stat %s (%d)", inname, errno);

  FILE *ifp = fopen (inname, "r");
  if (!ifp)
    die (1, "cannot open file %s (%d)", inname, errno);

  if (!xheader_file || version_major == 1)
    read_map (ifp);

  if (!outname)
    guess_outname (inname);

  if (verbose)
    printf ("Expanding file '%s' to '%s'\n", inname, outname);

  int ofd = -1;
  if (!dry_run)
    {
      ofd = open (outname, O_RDWR | O_CREAT | O_TRUNC, st.st_mode);
      if (ofd < 0)
	die (1, "cannot open file %s (%d)", outname, errno);
    }

  expand_sparse (ifp, ofd);

  if (ferror (ifp) || fclose (ifp) < 0)
    die (1, "input error: %s", inname);
  if (close (ofd) < 0)
    die (1, "output error: %s", outname);

  if (verbose)
    printf ("Done\n");

  if (dry_run)
    {
      printf ("Finished dry run\n");
      return 0;
    }

  if (outsize)
    {
      if (stat (outname, &st) < 0)
	die (1, "cannot stat output file %s (%d)", outname, errno);
      if (st.st_size != outsize)
	die (1, "expanded file has wrong size");
    }

  return 0;
}
