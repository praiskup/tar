/* POSIX extended headers for tar.

   Copyright (C) 2003-2025 Free Software Foundation, Inc.

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

#include <system.h>

#include <c-ctype.h>
#include <fnmatch.h>
#include <hash.h>
#include <inttostr.h>
#include <quotearg.h>

#include "common.h"

static void xheader_init (struct xheader *xhdr);
static bool xheader_protected_pattern_p (char const *pattern);
static bool xheader_protected_keyword_p (char const *keyword);

/* Used by xheader_finish() */
static void code_string (char const *string, char const *keyword,
			 struct xheader *xhdr);

/* Number of global headers written so far. */
static intmax_t global_header_count;
/* FIXME: Possibly it should be reset after changing the volume.
   POSIX %n specification says that it is expanded to the sequence
   number of current global header in *the* archive. However, for
   multi-volume archives this will yield duplicate header names
   in different volumes, which I'd like to avoid. The best way
   to solve this would be to use per-archive header count as required
   by POSIX *and* set globexthdr.name to, say,
   $TMPDIR/GlobalHead.%p.$NUMVOLUME.%n.

   However it should wait until buffer.c is finally rewritten */


/* Interface functions to obstacks */

static void
x_obstack_grow (struct xheader *xhdr, const char *ptr, idx_t length)
{
  obstack_grow (xhdr->stk, ptr, length);
  xhdr->size += length;
}

static void
x_obstack_1grow (struct xheader *xhdr, char c)
{
  obstack_1grow (xhdr->stk, c);
  xhdr->size++;
}

static void
x_obstack_blank (struct xheader *xhdr, idx_t length)
{
  obstack_blank (xhdr->stk, length);
  xhdr->size += length;
}


/* Keyword options */

struct keyword_list
{
  struct keyword_list *next;
  char *pattern;
  char *value;
};


/* List of keyword patterns set by delete= option */
static struct keyword_list *keyword_pattern_list;

/* List of keyword/value pairs set by 'keyword=value' option */
static struct keyword_list *keyword_global_override_list;

/* List of keyword/value pairs set by 'keyword:=value' option */
static struct keyword_list *keyword_override_list;

/* List of keyword/value pairs decoded from the last 'g' type header */
static struct keyword_list *global_header_override_list;

/* Template for the name field of an 'x' type header */
static char *exthdr_name;

static char *exthdr_mtime_option;
static time_t exthdr_mtime;

/* Template for the name field of a 'g' type header */
static char *globexthdr_name;

static char *globexthdr_mtime_option;
static time_t globexthdr_mtime;

bool
xheader_keyword_deleted_p (const char *kw)
{
  struct keyword_list *kp;

  for (kp = keyword_pattern_list; kp; kp = kp->next)
    if (fnmatch (kp->pattern, kw, 0) == 0)
      return true;
  return false;
}

static bool
xheader_keyword_override_p (const char *keyword)
{
  struct keyword_list *kp;

  for (kp = keyword_override_list; kp; kp = kp->next)
    if (strcmp (kp->pattern, keyword) == 0)
      return true;
  return false;
}

static void
xheader_list_append (struct keyword_list **root, char const *kw,
		     char const *value)
{
  struct keyword_list *kp = xmalloc (sizeof *kp);
  kp->pattern = xstrdup (kw);
  kp->value = value ? xstrdup (value) : NULL;
  kp->next = *root;
  *root = kp;
}

static void
xheader_list_destroy (struct keyword_list **root)
{
  if (root)
    {
      struct keyword_list *kw = *root;
      while (kw)
	{
	  struct keyword_list *next = kw->next;
	  free (kw->pattern);
	  free (kw->value);
	  free (kw);
	  kw = next;
	}
      *root = NULL;
    }
}

static _Noreturn void
xheader_set_single_keyword (char *kw)
{
  paxusage (_("Keyword %s is unknown or not yet implemented"), kw);
}

static void
assign_time_option (char **sval, time_t *tval, const char *input)
{
  char *p;
  struct timespec t = decode_timespec (input, &p, false);
  if (! valid_timespec (t) || *p)
    paxerror (0, _("Time stamp is out of allowed range"));
  else
    {
      *tval = t.tv_sec;
      assign_string (sval, input);
    }
}

static void
xheader_set_keyword_equal (char *kw, char *eq)
{
  bool global = true;
  char *p = eq;

  if (eq == kw)
    paxusage (_("Malformed pax option: %s"), quote (kw));

  if (eq[-1] == ':')
    {
      p--;
      global = false;
    }

  while (p > kw && c_isspace (*p))
    p--;

  *p = 0;

  for (p = eq + 1; *p && c_isspace (*p); p++)
    ;

  if (strcmp (kw, "delete") == 0)
    {
      if (xheader_protected_pattern_p (p))
	paxusage (_("Pattern %s cannot be used"), quote (p));
      xheader_list_append (&keyword_pattern_list, p, NULL);
    }
  else if (strcmp (kw, "exthdr.name") == 0)
    assign_string (&exthdr_name, p);
  else if (strcmp (kw, "globexthdr.name") == 0)
    assign_string (&globexthdr_name, p);
  else if (strcmp (kw, "exthdr.mtime") == 0)
    assign_time_option (&exthdr_mtime_option, &exthdr_mtime, p);
  else if (strcmp (kw, "globexthdr.mtime") == 0)
    assign_time_option (&globexthdr_mtime_option, &globexthdr_mtime, p);
  else
    {
      if (xheader_protected_keyword_p (kw))
	paxusage (_("Keyword %s cannot be overridden"), kw);
      if (global)
	xheader_list_append (&keyword_global_override_list, kw, p);
      else
	xheader_list_append (&keyword_override_list, kw, p);
    }
}

void
xheader_set_option (char *string)
{
  char *token;
  for (token = strtok (string, ","); token; token = strtok (NULL, ","))
    {
      char *p = strchr (token, '=');
      if (!p)
	xheader_set_single_keyword (token);
      else
	xheader_set_keyword_equal (token, p);
    }
}

/*
    string Includes:          Replaced By:
     %d                       The directory name of the file,
                              equivalent to the result of the
                              dirname utility on the translated
                              file name.
     %f                       The filename of the file, equivalent
                              to the result of the basename
                              utility on the translated file name.
     %p                       The process ID of the pax process.
     %n                       The value of the 3rd argument.
     %%                       A '%' character. */

char *
xheader_format_name (struct tar_stat_info *st, const char *fmt, intmax_t n)
{
  char *buf;
  char *q;
  const char *p;
  char *dirp = NULL;
  char *dir = NULL;
  char *base = NULL;
  char pidbuf[UINTMAX_STRSIZE_BOUND];
  char const *pptr = NULL;
  char nbuf[INTMAX_STRSIZE_BOUND];
  char const *nptr = NULL;

  idx_t len = 0;
  for (p = fmt; *p; p++)
    {
      if (*p == '%' && p[1])
	{
	  switch (*++p)
	    {
	    case '%':
	      len++;
	      break;

	    case 'd':
	      if (st)
		{
		  if (!dirp)
		    dirp = dir_name (st->orig_file_name);
		  dir = safer_name_suffix (dirp, false, absolute_names_option);
		  len += strlen (dir);
		}
	      break;

	    case 'f':
	      if (st)
		{
		  base = last_component (st->orig_file_name);
		  len += strlen (base);
		}
	      break;

	    case 'p':
	      pptr = umaxtostr (getpid (), pidbuf);
	      len += pidbuf + sizeof pidbuf - 1 - pptr;
	      break;

	    case 'n':
	      nptr = imaxtostr (n, nbuf);
	      len += nbuf + sizeof nbuf - 1 - nptr;
	      break;

	    default:
	      len += 2;
	    }
	}
      else
	len++;
    }

  buf = xmalloc (len + 1);
  for (q = buf, p = fmt; *p; )
    {
      if (*p == '%')
	{
	  switch (p[1])
	    {
	    case '%':
	      *q++ = *p++;
	      p++;
	      break;

	    case 'd':
	      if (dir)
		q = stpcpy (q, dir);
	      p += 2;
	      break;

	    case 'f':
	      if (base)
		q = stpcpy (q, base);
	      p += 2;
	      break;

	    case 'p':
	      q = stpcpy (q, pptr);
	      p += 2;
	      break;

	    case 'n':
	      q = stpcpy (q, nptr);
	      p += 2;
	      break;


	    default:
	      *q++ = *p++;
	      if (*p)
		*q++ = *p++;
	    }
	}
      else
	*q++ = *p++;
    }

  free (dirp);

  /* Do not allow it to end in a slash */
  while (q > buf && ISSLASH (q[-1]))
    q--;
  *q = 0;
  return buf;
}

/* Table of templates for the names of POSIX extended headers.
   Indexed by the the type of the header (per-file or global)
   and POSIX compliance mode (0 or q depending on whether
   POSIXLY_CORRECT environment variable is set. */
static const char *header_template[][2] = {
  /* Individual header templates: */
  { "%d/PaxHeaders/%f", "%d/PaxHeaders.%p/%f" },
  /* Global header templates: */
  { "/GlobalHead.%n", "/GlobalHead.%p.%n" }
};
/* Indices to the above table */
enum {
  pax_file_header,
  pax_global_header
};

char *
xheader_xhdr_name (struct tar_stat_info *st)
{
  if (!exthdr_name)
    assign_string (&exthdr_name,
		   header_template[pax_file_header][posixly_correct]);
  return xheader_format_name (st, exthdr_name, 0);
}

char *
xheader_ghdr_name (void)
{
  if (!globexthdr_name)
    {
      const char *global_header_template
	= header_template[pax_global_header][posixly_correct];
      const char *tmp = getenv ("TMPDIR");
      if (!tmp)
	tmp = "/tmp";
      idx_t tmplen = strlen (tmp);
      idx_t templatesize = strlen (global_header_template) + 1;
      globexthdr_name = ximalloc (tmplen + templatesize);
      char *p = mempcpy (globexthdr_name, tmp, tmplen);
      memcpy (p, global_header_template, templatesize);
    }

  return xheader_format_name (NULL, globexthdr_name, global_header_count + 1);
}

void
xheader_write (char type, char *name, time_t t, struct xheader *xhdr)
{
  idx_t size = xhdr->size;
  switch (type)
    {
    case XGLTYPE:
      if (globexthdr_mtime_option)
	t = globexthdr_mtime;
      break;

    case XHDTYPE:
      if (exthdr_mtime_option)
	t = exthdr_mtime;
      break;
    }
  union block *header = start_private_header (name, size, t);
  header->header.typeflag = type;

  simple_finish_header (header);

  char *p = xhdr->buffer;

  do
    {
      header = find_next_block ();
      idx_t len = min (size, BLOCKSIZE);
      memcpy (header->buffer, p, len);
      if (len < BLOCKSIZE)
	memset (header->buffer + len, 0, BLOCKSIZE - len);
      p += len;
      size -= len;
      set_next_block_after (header);
    }
  while (size > 0);
  xheader_destroy (xhdr);

  if (type == XGLTYPE)
    global_header_count++;
}

void
xheader_write_global (struct xheader *xhdr)
{
  if (keyword_global_override_list)
    {
      struct keyword_list *kp;

      xheader_init (xhdr);
      for (kp = keyword_global_override_list; kp; kp = kp->next)
	code_string (kp->value, kp->pattern, xhdr);
    }
  if (xhdr->stk)
    {
      char *name;

      xheader_finish (xhdr);
      name = xheader_ghdr_name ();
      xheader_write (XGLTYPE, name, start_time.tv_sec, xhdr);
      free (name);
    }
}

/* Forbid modifications of the global extended header */
void
xheader_forbid_global (void)
{
  if (keyword_global_override_list)
    paxusage (_("can't update global extended header record"));
}

/* This is reversal function for xattr_encode_keyword.  See comment for
   xattr_encode_keyword() for more info. */
static void
xattr_decode_keyword (char *keyword)
{
  char *kpr, *kpl; /* keyword pointer left/right */
  kpr = kpl = keyword;

  for (;;)
    {
      if (*kpr == '%')
        {
          if (kpr[1] == '3' && kpr[2] == 'D')
            {
              *kpl = '=';
              kpr += 3;
              kpl ++;
              continue;
            }
          else if (kpr[1] == '2' && kpr[2] == '5')
            {
              *kpl = '%';
              kpr += 3;
              kpl ++;
              continue;
            }
        }

      *kpl = *kpr;

      if (*kpr == 0)
        break;

      kpr++;
      kpl++;
    }
}

/* General Interface */

enum
  {
    XHDR_PROTECTED	= 0x01,
    XHDR_GLOBAL		= 0x02
  };

struct xhdr_tab
{
  char const *keyword;
  void (*coder) (struct tar_stat_info const *, char const *,
		 struct xheader *, void const *data);
  void (*decoder) (struct tar_stat_info *, char const *, char const *, idx_t);
  int flags;
  bool prefix; /* select handler comparing prefix only */
};

/* This declaration must be extern, because ISO C99 section 6.9.2
   prohibits a tentative definition that has both internal linkage and
   incomplete type.  If we made it static, we'd have to declare its
   size which would be a maintenance pain; if we put its initializer
   here, we'd need a boatload of forward declarations, which would be
   even more of a pain.  */
extern struct xhdr_tab const xhdr_tab[];

static struct xhdr_tab const *
locate_handler (char const *keyword)
{
  struct xhdr_tab const *p;
  for (p = xhdr_tab; p->keyword; p++)
    if (p->prefix)
      {
	idx_t kwlen = strlen (p->keyword);
	if (strncmp (p->keyword, keyword, kwlen) == 0 && keyword[kwlen] == '.')
          return p;
      }
    else
      {
        if (strcmp (p->keyword, keyword) == 0)
          return p;
      }

  return NULL;
}

static bool
xheader_protected_pattern_p (const char *pattern)
{
  struct xhdr_tab const *p;

  for (p = xhdr_tab; p->keyword; p++)
    if (!p->prefix && (p->flags & XHDR_PROTECTED)
        && fnmatch (pattern, p->keyword, 0) == 0)
      return true;
  return false;
}

static bool
xheader_protected_keyword_p (const char *keyword)
{
  struct xhdr_tab const *p;

  for (p = xhdr_tab; p->keyword; p++)
    if (!p->prefix && (p->flags & XHDR_PROTECTED)
        && strcmp (p->keyword, keyword) == 0)
      return true;
  return false;
}

/* Decode a single extended header record, advancing *PTR to the next record.
   Return true on success, false otherwise.  */
static bool
decode_record (struct xheader *xhdr,
	       char **ptr,
	       void (*handler) (void *, char const *, char const *, idx_t),
	       void *data)
{
  char *start = *ptr;
  char *p = start;
  char *len_lim;
  char const *keyword;
  char *nextp;
  idx_t len_max = xhdr->buffer + xhdr->size - start;

  while (*p == ' ' || *p == '\t')
    p++;

  idx_t len = stoint (p, &len_lim, NULL, 0, IDX_MAX);

  if (len_lim == p)
    {
      /* The length is missing.
	 FIXME: Comment why this is diagnosed only if (*p), or change code.  */
      if (*p)
	paxerror (0, _("Malformed extended header: missing length"));
      return false;
    }

  if (len_max < len)
    {
      /* Avoid giant diagnostics, as this won't help user.  */
      int len_len = min (len_lim - p, 1000);

      paxerror (0, _("Extended header length %.*s is out of range"),
		len_len, p);
      return false;
    }

  nextp = start + len;

  for (p = len_lim; *p == ' ' || *p == '\t'; p++)
    continue;
  if (p == len_lim)
    {
      paxerror (0, _("Malformed extended header: missing blank after length"));
      return false;
    }

  keyword = p;
  p = strchr (p, '=');
  if (! (p && p < nextp))
    {
      paxerror (0, _("Malformed extended header: missing equal sign"));
      return false;
    }

  if (nextp[-1] != '\n')
    {
      paxerror (0, _("Malformed extended header: missing newline"));
      return false;
    }

  *p = nextp[-1] = '\0';
  handler (data, keyword, p + 1, nextp - p - 2); /* '=' + trailing '\n' */
  *p = '=';
  nextp[-1] = '\n';
  *ptr = nextp;
  return true;
}

static void
run_override_list (struct keyword_list *kp, struct tar_stat_info *st)
{
  for (; kp; kp = kp->next)
    {
      struct xhdr_tab const *t = locate_handler (kp->pattern);
      if (t)
	t->decoder (st, kp->pattern, kp->value, strlen (kp->value));
    }
}

static void
decx (void *data, char const *keyword, char const *value, idx_t size)
{
  struct xhdr_tab const *t;
  struct tar_stat_info *st = data;

  if (xheader_keyword_deleted_p (keyword)
      || xheader_keyword_override_p (keyword))
    return;

  t = locate_handler (keyword);
  if (t)
    t->decoder (st, keyword, value, size);
  else
    warnopt (WARN_UNKNOWN_KEYWORD, 0,
	     _("Ignoring unknown extended header keyword %s"),
	     quotearg_style (shell_escape_always_quoting_style, keyword));
}

void
xheader_decode (struct tar_stat_info *st)
{
  run_override_list (keyword_global_override_list, st);
  run_override_list (global_header_override_list, st);

  if (st->xhdr.size)
    {
      char *p = st->xhdr.buffer + BLOCKSIZE;
      while (decode_record (&st->xhdr, &p, decx, st))
	continue;
    }
  run_override_list (keyword_override_list, st);

  /* The archived (effective) file size is always set directly in tar header
     field, possibly overridden by "size" extended header - in both cases,
     result is now decoded in st->stat.st_size */
  st->archive_file_size = st->stat.st_size;

  /* The real file size (given by stat()) may be redefined for sparse
     files in "GNU.sparse.realsize" extended header */
  if (st->real_size_set)
    st->stat.st_size = st->real_size;
}

static void
decg (void *data, char const *keyword, char const *value,
      MAYBE_UNUSED idx_t size)
{
  struct keyword_list **kwl = data;
  struct xhdr_tab const *tab = locate_handler (keyword);
  if (tab && (tab->flags & XHDR_GLOBAL))
    tab->decoder (data, keyword, value, size);
  else
    xheader_list_append (kwl, keyword, value);
}

void
xheader_decode_global (struct xheader *xhdr)
{
  if (xhdr->size)
    {
      char *p = xhdr->buffer + BLOCKSIZE;

      xheader_list_destroy (&global_header_override_list);
      while (decode_record (xhdr, &p, decg, &global_header_override_list))
	continue;
    }
}

static void
xheader_init (struct xheader *xhdr)
{
  if (!xhdr->stk)
    {
      xhdr->stk = xmalloc (sizeof *xhdr->stk);
      obstack_init (xhdr->stk);
    }
}

void
xheader_store (char const *keyword, struct tar_stat_info *st,
	       void const *data)
{
  struct xhdr_tab const *t;

  if (st->xhdr.buffer)
    return;
  t = locate_handler (keyword);
  if (!t || !t->coder)
    return;
  if (xheader_keyword_deleted_p (keyword))
    return;
  xheader_init (&st->xhdr);
  if (!xheader_keyword_override_p (keyword))
    t->coder (st, keyword, &st->xhdr, data);
}

void
xheader_read (struct xheader *xhdr, union block *p, off_t size)
{
  idx_t j = 0;

  if (size < 0)
    size = 0; /* Already diagnosed.  */

  idx_t size_plus_1;
  if (ckd_add (&size_plus_1, size, BLOCKSIZE + 1))
    xalloc_die ();
  size = size_plus_1 - 1;

  xhdr->size = size;
  xhdr->buffer = xmalloc (size_plus_1);
  xhdr->buffer[size] = '\0';

  do
    {
      if (!p)
	paxfatal (0, _("Unexpected EOF in archive"));

      idx_t len = min (size, BLOCKSIZE);
      memcpy (&xhdr->buffer[j], p->buffer, len);
      set_next_block_after (p);

      p = find_next_block ();

      j += len;
      size -= len;
    }
  while (size > 0);
}

/* xattr_encode_keyword() substitutes '=' ~~> '%3D' and '%' ~~> '%25'
   in extended attribute keywords.  This is needed because the '=' character
   has special purpose in extended attribute header - it splits keyword and
   value part of header.  If there was the '=' occurrence allowed inside
   keyword, there would be no unambiguous way how to decode this extended
   attribute.

   (http://lists.gnu.org/archive/html/bug-tar/2012-10/msg00017.html)
 */
static char *
xattr_encode_keyword (char const *keyword)
{
  static char *encode_buffer = NULL;
  static idx_t encode_buffer_size = 0;

  for (idx_t bp = 0; ; )
    {
      if (encode_buffer_size < bp + 3 /* enough for URL encoding also.. */)
	encode_buffer = xpalloc (encode_buffer, &encode_buffer_size, 3, -1, 1);

      char c = *keyword++;
      switch (c)
	{
	case '%':
	  memcpy (encode_buffer + bp, "%25", 3);
	  bp += 3;
	  break;

	case '=':
	  memcpy (encode_buffer + bp, "%3D", 3);
	  bp += 3;
	  break;

	default:
	  encode_buffer[bp++] = c;
	  if (!c)
	    return encode_buffer;
	  break;
	}
    }
}

static void
xheader_print_n (struct xheader *xhdr, char const *keyword,
		 char const *value, idx_t vsize)
{
  idx_t p;
  idx_t n = 0;
  char nbuf[INTMAX_STRSIZE_BOUND];
  char const *np;

  keyword = xattr_encode_keyword (keyword);
  idx_t klen = strlen (keyword);
  idx_t len = klen + vsize + 3; /* ' ' + '=' + '\n' */

  do
    {
      p = n;
      np = imaxtostr (len + p, nbuf);
      n = nbuf + sizeof nbuf - 1 - np;
    }
  while (n != p);

  x_obstack_grow (xhdr, np, n);
  x_obstack_1grow (xhdr, ' ');
  x_obstack_grow (xhdr, keyword, klen);
  x_obstack_1grow (xhdr, '=');
  x_obstack_grow (xhdr, value, vsize);
  x_obstack_1grow (xhdr, '\n');
}

static void
xheader_print (struct xheader *xhdr, char const *keyword, char const *value)
{
  xheader_print_n (xhdr, keyword, value, strlen (value));
}

void
xheader_finish (struct xheader *xhdr)
{
  struct keyword_list *kp;

  for (kp = keyword_override_list; kp; kp = kp->next)
    code_string (kp->value, kp->pattern, xhdr);

  xhdr->buffer = obstack_finish (xhdr->stk);
}

void
xheader_destroy (struct xheader *xhdr)
{
  if (xhdr->stk)
    {
      obstack_free (xhdr->stk, NULL);
      free (xhdr->stk);
      xhdr->stk = NULL;
    }
  else
    free (xhdr->buffer);
  xhdr->buffer = 0;
  xhdr->size = 0;
}


/* Buildable strings */

void
xheader_string_begin (struct xheader *xhdr)
{
  xhdr->string_length = 0;
}

void
xheader_string_add (struct xheader *xhdr, char const *s)
{
  if (xhdr->buffer)
    return;
  xheader_init (xhdr);
  idx_t slen = strlen (s);
  xhdr->string_length += slen;
  x_obstack_grow (xhdr, s, slen);
}

bool
xheader_string_end (struct xheader *xhdr, char const *keyword)
{
  idx_t p;
  idx_t n = 0;
  char nbuf[UINTMAX_STRSIZE_BOUND];
  char const *np;

  if (xhdr->buffer)
    return false;
  xheader_init (xhdr);

  idx_t len = strlen (keyword) + xhdr->string_length + (sizeof " =\n" - 1);

  do
    {
      p = n;
      np = umaxtostr (len + p, nbuf);
      n = nbuf + sizeof nbuf - 1 - np;
    }
  while (n != p);

  p = strlen (keyword) + n + 2;
  x_obstack_blank (xhdr, p);
  x_obstack_1grow (xhdr, '\n');
  char *cp = obstack_next_free (xhdr->stk);
  cp -= xhdr->string_length + p + 1;
  memmove (cp + p, cp, xhdr->string_length);
  cp = stpcpy (cp, np);
  *cp++ = ' ';
  cp = stpcpy (cp, keyword);
  *cp++ = '=';
  return true;
}


/* Implementations */

static void
out_of_range_header (char const *keyword, char const *value,
		     intmax_t minval, uintmax_t maxval)
{
  /* TRANSLATORS: The first %s is the pax extended header keyword
     (atime, gid, etc.).  */
  paxerror (0, _("Extended header %s=%s is out of range %jd..%ju"),
	    keyword, value, minval, maxval);
}

static void
code_string (char const *string, char const *keyword, struct xheader *xhdr)
{
  char *outstr;
  if (!utf8_convert (true, string, &outstr))
    {
      /* FIXME: report error */
      outstr = xstrdup (string);
    }
  xheader_print (xhdr, keyword, outstr);
  free (outstr);
}

static void
decode_string (char **string, char const *arg)
{
  if (*string)
    {
      free (*string);
      *string = NULL;
    }
  if (!utf8_convert (false, arg, string))
    {
      /* FIXME: report error and act accordingly to --pax invalid=UTF-8 */
      assign_string (string, arg);
    }
}

static void
code_time (struct timespec t, char const *keyword, struct xheader *xhdr)
{
  char buf[TIMESPEC_STRSIZE_BOUND];
  xheader_print (xhdr, keyword, code_timespec (t, buf));
}

static bool
decode_time (struct timespec *ts, char const *arg, char const *keyword)
{
  char *arg_lim;
  struct timespec t = decode_timespec (arg, &arg_lim, true);

  if (! valid_timespec (t))
    {
      if (arg < arg_lim && !*arg_lim)
	out_of_range_header (keyword, arg, TYPE_MINIMUM (time_t),
			     TYPE_MAXIMUM (time_t));
      else
	paxerror (0, _("Malformed extended header: invalid %s=%s"),
		  keyword, arg);
      return false;
    }
  if (*arg_lim)
    {
      paxerror (0, _("Malformed extended header: invalid %s=%s"),
		keyword, arg);
      return false;
    }

  *ts = t;
  return true;
}

static void
code_signed_num (uintmax_t value, char const *keyword,
		 intmax_t minval, uintmax_t maxval, struct xheader *xhdr)
{
  char sbuf[SYSINT_BUFSIZE];
  xheader_print (xhdr, keyword, sysinttostr (value, minval, maxval, sbuf));
}

static void
code_num (uintmax_t value, char const *keyword, struct xheader *xhdr)
{
  code_signed_num (value, keyword, 0, UINTMAX_MAX, xhdr);
}

static bool
decode_signed_num (intmax_t *num, char const *arg,
		   intmax_t minval, uintmax_t maxval,
		   char const *keyword)
{
  char *arg_lim;
  bool overflow;
  intmax_t u = stoint (arg, &arg_lim, &overflow, minval, maxval);

  if ((arg_lim == arg) | *arg_lim)
    {
      paxerror (0, _("Malformed extended header: invalid %s=%s"),
		keyword, arg);
      return false;
    }

  if (overflow)
    {
      out_of_range_header (keyword, arg, minval, maxval);
      return false;
    }

  *num = u;
  return true;
}

static bool
decode_num (uintmax_t *num, char const *arg, uintmax_t maxval,
	    char const *keyword)
{
  intmax_t i;
  if (! decode_signed_num (&i, arg, 0, maxval, keyword))
    return false;
  *num = i;
  return true;
}

static void
dummy_coder (MAYBE_UNUSED struct tar_stat_info const *st,
	     MAYBE_UNUSED char const *keyword,
	     MAYBE_UNUSED struct xheader *xhdr,
	     MAYBE_UNUSED void const *data)
{
}

static void
dummy_decoder (MAYBE_UNUSED struct tar_stat_info *st,
	       MAYBE_UNUSED char const *keyword,
	       MAYBE_UNUSED char const *arg,
	       MAYBE_UNUSED idx_t size)
{
}

static void
atime_coder (struct tar_stat_info const *st, char const *keyword,
	     struct xheader *xhdr, MAYBE_UNUSED void const *data)
{
  code_time (st->atime, keyword, xhdr);
}

static void
atime_decoder (struct tar_stat_info *st,
	       char const *keyword,
	       char const *arg,
	       MAYBE_UNUSED idx_t size)
{
  struct timespec ts;
  if (decode_time (&ts, arg, keyword))
    st->atime = ts;
}

static void
gid_coder (struct tar_stat_info const *st, char const *keyword,
	   struct xheader *xhdr, MAYBE_UNUSED void const *data)
{
  code_signed_num (st->stat.st_gid, keyword,
		   TYPE_MINIMUM (gid_t), TYPE_MAXIMUM (gid_t), xhdr);
}

static void
gid_decoder (struct tar_stat_info *st,
	     char const *keyword,
	     char const *arg,
	     MAYBE_UNUSED idx_t size)
{
  intmax_t u;
  if (decode_signed_num (&u, arg, TYPE_MINIMUM (gid_t),
			 TYPE_MAXIMUM (gid_t), keyword))
    st->stat.st_gid = u;
}

static void
gname_coder (struct tar_stat_info const *st, char const *keyword,
	     struct xheader *xhdr, MAYBE_UNUSED void const *data)
{
  code_string (st->gname, keyword, xhdr);
}

static void
gname_decoder (struct tar_stat_info *st,
	       MAYBE_UNUSED char const *keyword,
	       char const *arg,
	       MAYBE_UNUSED idx_t size)
{
  decode_string (&st->gname, arg);
}

static void
linkpath_coder (struct tar_stat_info const *st, char const *keyword,
		struct xheader *xhdr, MAYBE_UNUSED void const *data)
{
  code_string (st->link_name, keyword, xhdr);
}

static void
linkpath_decoder (struct tar_stat_info *st,
		  MAYBE_UNUSED char const *keyword,
		  char const *arg,
		  MAYBE_UNUSED idx_t size)
{
  decode_string (&st->link_name, arg);
}

static void
ctime_coder (struct tar_stat_info const *st, char const *keyword,
	     struct xheader *xhdr, MAYBE_UNUSED void const *data)
{
  code_time (st->ctime, keyword, xhdr);
}

static void
ctime_decoder (struct tar_stat_info *st,
	       char const *keyword,
	       char const *arg,
	       MAYBE_UNUSED idx_t size)
{
  struct timespec ts;
  if (decode_time (&ts, arg, keyword))
    st->ctime = ts;
}

static void
mtime_coder (struct tar_stat_info const *st, char const *keyword,
	     struct xheader *xhdr, void const *data)
{
  struct timespec const *mtime = data;
  code_time (mtime ? *mtime : st->mtime, keyword, xhdr);
}

static void
mtime_decoder (struct tar_stat_info *st,
	       char const *keyword,
	       char const *arg,
	       MAYBE_UNUSED idx_t size)
{
  struct timespec ts;
  if (decode_time (&ts, arg, keyword))
    st->mtime = ts;
}

static void
path_coder (struct tar_stat_info const *st, char const *keyword,
	    struct xheader *xhdr, MAYBE_UNUSED void const *data)
{
  code_string (st->file_name, keyword, xhdr);
}

static void
raw_path_decoder (struct tar_stat_info *st, char const *arg)
{
  if (*arg)
    {
      decode_string (&st->orig_file_name, arg);
      decode_string (&st->file_name, arg);
      st->had_trailing_slash = strip_trailing_slashes (st->file_name);
    }
}


static void
path_decoder (struct tar_stat_info *st,
	      MAYBE_UNUSED char const *keyword,
	      char const *arg,
	      MAYBE_UNUSED idx_t size)
{
  if (! st->sparse_name_done)
    raw_path_decoder (st, arg);
}

static void
sparse_path_decoder (struct tar_stat_info *st,
		     MAYBE_UNUSED char const *keyword,
                     char const *arg,
		     MAYBE_UNUSED idx_t size)
{
  st->sparse_name_done = true;
  raw_path_decoder (st, arg);
}

static void
size_coder (struct tar_stat_info const *st, char const *keyword,
	    struct xheader *xhdr, MAYBE_UNUSED void const *data)
{
  code_num (st->stat.st_size, keyword, xhdr);
}

static void
size_decoder (struct tar_stat_info *st,
	      char const *keyword,
	      char const *arg,
	      MAYBE_UNUSED idx_t size)
{
  uintmax_t u;
  if (decode_num (&u, arg, TYPE_MAXIMUM (off_t), keyword))
    st->stat.st_size = u;
}

static void
uid_coder (struct tar_stat_info const *st, char const *keyword,
	   struct xheader *xhdr, MAYBE_UNUSED void const *data)
{
  code_signed_num (st->stat.st_uid, keyword,
		   TYPE_MINIMUM (uid_t), TYPE_MAXIMUM (uid_t), xhdr);
}

static void
uid_decoder (struct tar_stat_info *st,
	     char const *keyword,
	     char const *arg,
	     MAYBE_UNUSED idx_t size)
{
  intmax_t u;
  if (decode_signed_num (&u, arg, TYPE_MINIMUM (uid_t),
			 TYPE_MAXIMUM (uid_t), keyword))
    st->stat.st_uid = u;
}

static void
uname_coder (struct tar_stat_info const *st, char const *keyword,
	     struct xheader *xhdr, MAYBE_UNUSED void const *data)
{
  code_string (st->uname, keyword, xhdr);
}

static void
uname_decoder (struct tar_stat_info *st,
	       MAYBE_UNUSED char const *keyword,
	       char const *arg,
	       MAYBE_UNUSED idx_t size)
{
  decode_string (&st->uname, arg);
}

static void
sparse_size_coder (struct tar_stat_info const *st, char const *keyword,
	     struct xheader *xhdr, void const *data)
{
  size_coder (st, keyword, xhdr, data);
}

static void
sparse_size_decoder (struct tar_stat_info *st,
		     char const *keyword,
		     char const *arg,
		     MAYBE_UNUSED idx_t size)
{
  uintmax_t u;
  if (decode_num (&u, arg, TYPE_MAXIMUM (off_t), keyword))
    {
      st->real_size_set = true;
      st->real_size = u;
    }
}

static void
sparse_numblocks_coder (struct tar_stat_info const *st, char const *keyword,
			struct xheader *xhdr,
			MAYBE_UNUSED void const *data)
{
  code_num (st->sparse_map_avail, keyword, xhdr);
}

static void
sparse_numblocks_decoder (struct tar_stat_info *st,
			  char const *keyword,
			  char const *arg,
			  MAYBE_UNUSED idx_t size)
{
  uintmax_t u;
  if (decode_num (&u, arg, SIZE_MAX, keyword))
    {
      st->sparse_map_size = u;
      st->sparse_map = xcalloc (u, sizeof st->sparse_map[0]);
      st->sparse_map_avail = 0;
    }
}

static void
sparse_offset_coder (struct tar_stat_info const *st, char const *keyword,
		     struct xheader *xhdr, void const *data)
{
  idx_t const *pi = data;
  code_num (st->sparse_map[*pi].offset, keyword, xhdr);
}

static void
sparse_offset_decoder (struct tar_stat_info *st,
		       char const *keyword,
		       char const *arg,
		       MAYBE_UNUSED idx_t size)
{
  uintmax_t u;
  if (decode_num (&u, arg, TYPE_MAXIMUM (off_t), keyword))
    {
      if (st->sparse_map_avail < st->sparse_map_size)
	st->sparse_map[st->sparse_map_avail].offset = u;
      else
	paxerror (0, _("Malformed extended header: excess %s=%s"),
		  "GNU.sparse.offset", arg);
    }
}

static void
sparse_numbytes_coder (struct tar_stat_info const *st, char const *keyword,
		       struct xheader *xhdr, void const *data)
{
  idx_t const *pi = data;
  code_num (st->sparse_map[*pi].numbytes, keyword, xhdr);
}

static void
sparse_numbytes_decoder (struct tar_stat_info *st,
			 char const *keyword,
			 char const *arg,
			 MAYBE_UNUSED idx_t size)
{
  uintmax_t u;
  if (decode_num (&u, arg, TYPE_MAXIMUM (off_t), keyword))
    {
      if (st->sparse_map_avail < st->sparse_map_size)
	st->sparse_map[st->sparse_map_avail++].numbytes = u;
      else
	paxerror (0, _("Malformed extended header: excess %s=%s"),
		  keyword, arg);
    }
}

static void
sparse_map_decoder (struct tar_stat_info *st,
		    char const *keyword,
		    char const *arg,
		    MAYBE_UNUSED idx_t size)
{
  bool offset = true;
  struct sp_array e;

  st->sparse_map_avail = 0;
  while (true)
    {
      char *delim;
      bool overflow;
      off_t u = stoint (arg, &delim, &overflow, 0, TYPE_MAXIMUM (off_t));
      if (delim == arg)
	{
	  paxerror (0, _("Malformed extended header: invalid %s=%s"),
		    keyword, arg);
	  return;
	}

      if (offset)
	{
	  e.offset = u;
	  if (overflow)
	    {
	      out_of_range_header (keyword, arg, 0, TYPE_MAXIMUM (off_t));
	      return;
	    }
	}
      else
	{
	  e.numbytes = u;
	  if (overflow)
	    {
	      out_of_range_header (keyword, arg, 0, TYPE_MAXIMUM (off_t));
	      return;
	    }
	  if (st->sparse_map_avail < st->sparse_map_size)
	    st->sparse_map[st->sparse_map_avail++] = e;
	  else
	    {
	      paxerror (0, _("Malformed extended header: excess %s=%s"),
			keyword, arg);
	      return;
	    }
	}

      offset = !offset;

      if (*delim == 0)
	break;
      else if (*delim != ',')
	{
	  paxerror (0,
		    _("Malformed extended header: invalid %s:"
		      " unexpected delimiter %c"),
		    keyword, *delim);
	  return;
	}

      arg = delim + 1;
    }

  if (!offset)
    paxerror (0,
	      _("Malformed extended header: invalid %s:"
		" odd number of values"),
	      keyword);
}

static void
dumpdir_coder (MAYBE_UNUSED struct tar_stat_info const *st, char const *keyword,
	       struct xheader *xhdr, void const *data)
{
  xheader_print_n (xhdr, keyword, data, dumpdir_size (data));
}

static void
dumpdir_decoder (struct tar_stat_info *st,
		 MAYBE_UNUSED char const *keyword,
		 char const *arg,
		 idx_t size)
{
  st->dumpdir = ximalloc (size);
  memcpy (st->dumpdir, arg, size);
}

static void
volume_label_coder (MAYBE_UNUSED struct tar_stat_info const *st,
		    char const *keyword,
		    struct xheader *xhdr, void const *data)
{
  code_string (data, keyword, xhdr);
}

static void
volume_label_decoder (MAYBE_UNUSED struct tar_stat_info *st,
		      MAYBE_UNUSED char const *keyword,
		      char const *arg,
		      MAYBE_UNUSED idx_t size)
{
  decode_string (&volume_label, arg);
}

static void
volume_size_coder (MAYBE_UNUSED struct tar_stat_info const *st,
		   char const *keyword,
		   struct xheader *xhdr, void const *data)
{
  off_t const *v = data;
  code_num (*v, keyword, xhdr);
}

static void
volume_size_decoder (MAYBE_UNUSED struct tar_stat_info *st,
		     char const *keyword,
		     char const *arg, MAYBE_UNUSED idx_t size)
{
  uintmax_t u;
  if (decode_num (&u, arg, TYPE_MAXIMUM (off_t), keyword))
    continued_file_size = u;
}

/* FIXME: Merge with volume_size_coder */
static void
volume_offset_coder (MAYBE_UNUSED struct tar_stat_info const *st,
		     char const *keyword,
		     struct xheader *xhdr, void const *data)
{
  off_t const *v = data;
  code_num (*v, keyword, xhdr);
}

static void
volume_offset_decoder (MAYBE_UNUSED struct tar_stat_info *st,
		       char const *keyword,
		       char const *arg, MAYBE_UNUSED idx_t size)
{
  uintmax_t u;
  if (decode_num (&u, arg, TYPE_MAXIMUM (off_t), keyword))
    continued_file_offset = u;
}

static void
volume_filename_decoder (MAYBE_UNUSED struct tar_stat_info *st,
			 MAYBE_UNUSED char const *keyword,
			 char const *arg,
			 MAYBE_UNUSED idx_t size)
{
  decode_string (&continued_file_name, arg);
}

static void
xattr_selinux_coder (struct tar_stat_info const *st, char const *keyword,
		     struct xheader *xhdr, MAYBE_UNUSED void const *data)
{
  code_string (st->cntx_name, keyword, xhdr);
}

static void
xattr_selinux_decoder (struct tar_stat_info *st,
		       MAYBE_UNUSED char const *keyword, char const *arg,
		       MAYBE_UNUSED idx_t size)
{
  decode_string (&st->cntx_name, arg);
}

static void
xattr_acls_a_coder (struct tar_stat_info const *st , char const *keyword,
		    struct xheader *xhdr, MAYBE_UNUSED void const *data)
{
  xheader_print_n (xhdr, keyword, st->acls_a_ptr, st->acls_a_len);
}

static void
xattr_acls_a_decoder (struct tar_stat_info *st,
		      MAYBE_UNUSED char const *keyword,
		      char const *arg, idx_t size)
{
  st->acls_a_ptr = xmemdup (arg, size + 1);
  st->acls_a_len = size;
}

static void
xattr_acls_d_coder (struct tar_stat_info const *st , char const *keyword,
		    struct xheader *xhdr, MAYBE_UNUSED void const *data)
{
  xheader_print_n (xhdr, keyword, st->acls_d_ptr, st->acls_d_len);
}

static void
xattr_acls_d_decoder (struct tar_stat_info *st,
		      MAYBE_UNUSED char const *keyword, char const *arg,
		      idx_t size)
{
  st->acls_d_ptr = xmemdup (arg, size + 1);
  st->acls_d_len = size;
}

static void
xattr_coder (struct tar_stat_info const *st, char const *keyword,
             struct xheader *xhdr, void const *data)
{
  idx_t const *idx_data = data;
  idx_t n = *idx_data;
  xheader_print_n (xhdr, keyword,
		   st->xattr_map.xm_map[n].xval_ptr,
		   st->xattr_map.xm_map[n].xval_len);
}

static void
xattr_decoder (struct tar_stat_info *st,
               char const *keyword, char const *arg, idx_t size)
{
  char *xkey;

  /* copy keyword */
  xkey = xstrdup (keyword);

  xattr_decode_keyword (xkey);

  xattr_map_add (&st->xattr_map, xkey, arg, size);

  free (xkey);
}

static void
sparse_major_coder (struct tar_stat_info const *st, char const *keyword,
		    struct xheader *xhdr, MAYBE_UNUSED void const *data)
{
  code_num (st->sparse_major, keyword, xhdr);
}

static void
sparse_major_decoder (struct tar_stat_info *st,
		      char const *keyword,
		      char const *arg,
		      MAYBE_UNUSED idx_t size)
{
  uintmax_t u;
  if (decode_num (&u, arg, INTMAX_MAX, keyword))
    st->sparse_major = u;
}

static void
sparse_minor_coder (struct tar_stat_info const *st, char const *keyword,
		    struct xheader *xhdr, MAYBE_UNUSED void const *data)
{
  code_num (st->sparse_minor, keyword, xhdr);
}

static void
sparse_minor_decoder (struct tar_stat_info *st,
		      char const *keyword,
		      char const *arg,
		      MAYBE_UNUSED idx_t size)
{
  uintmax_t u;
  if (decode_num (&u, arg, INTMAX_MAX, keyword))
    st->sparse_minor = u;
}

struct xhdr_tab const xhdr_tab[] = {
  { "atime",    atime_coder,    atime_decoder,    0, false },
  { "comment",  dummy_coder,    dummy_decoder,    0, false },
  { "charset",  dummy_coder,    dummy_decoder,    0, false },
  { "ctime",    ctime_coder,    ctime_decoder,    0, false },
  { "gid",      gid_coder,      gid_decoder,      0, false },
  { "gname",    gname_coder,    gname_decoder,    0, false },
  { "linkpath", linkpath_coder, linkpath_decoder, 0, false },
  { "mtime",    mtime_coder,    mtime_decoder,    0, false },
  { "path",     path_coder,     path_decoder,     0, false },
  { "size",     size_coder,     size_decoder,     0, false },
  { "uid",      uid_coder,      uid_decoder,      0, false },
  { "uname",    uname_coder,    uname_decoder,    0, false },

  /* Sparse file handling */
  { "GNU.sparse.name",       path_coder, sparse_path_decoder,
    XHDR_PROTECTED, false },
  { "GNU.sparse.major",      sparse_major_coder, sparse_major_decoder,
    XHDR_PROTECTED, false },
  { "GNU.sparse.minor",      sparse_minor_coder, sparse_minor_decoder,
    XHDR_PROTECTED, false },
  { "GNU.sparse.realsize",   sparse_size_coder, sparse_size_decoder,
    XHDR_PROTECTED, false },
  { "GNU.sparse.numblocks",  sparse_numblocks_coder, sparse_numblocks_decoder,
    XHDR_PROTECTED, false },

  /* tar 1.14 - 1.15.90 keywords. */
  { "GNU.sparse.size",       sparse_size_coder, sparse_size_decoder,
    XHDR_PROTECTED, false },
  /* tar 1.14 - 1.15.1 keywords. Multiple instances of these appeared in 'x'
     headers, and each of them was meaningful. It confilcted with POSIX specs,
     which requires that "when extended header records conflict, the last one
     given in the header shall take precedence." */
  { "GNU.sparse.offset",     sparse_offset_coder, sparse_offset_decoder,
    XHDR_PROTECTED, false },
  { "GNU.sparse.numbytes",   sparse_numbytes_coder, sparse_numbytes_decoder,
    XHDR_PROTECTED, false },
  /* tar 1.15.90 keyword, introduced to remove the above-mentioned conflict. */
  { "GNU.sparse.map",        NULL /* Unused, see pax_dump_header() */,
    sparse_map_decoder, 0, false },

  { "GNU.dumpdir",           dumpdir_coder, dumpdir_decoder,
    XHDR_PROTECTED, false },

  /* Keeps the tape/volume label. May be present only in the global headers.
     Equivalent to GNUTYPE_VOLHDR.  */
  { "GNU.volume.label", volume_label_coder, volume_label_decoder,
    XHDR_PROTECTED | XHDR_GLOBAL, false },

  /* These may be present in a first global header of the archive.
     They provide the same functionality as GNUTYPE_MULTIVOL header.
     The GNU.volume.size keeps the real_s_sizeleft value, which is
     otherwise kept in the size field of a multivolume header.  The
     GNU.volume.offset keeps the offset of the start of this volume,
     otherwise kept in oldgnu_header.offset.  */
  { "GNU.volume.filename", volume_label_coder, volume_filename_decoder,
    XHDR_PROTECTED | XHDR_GLOBAL, false },
  { "GNU.volume.size", volume_size_coder, volume_size_decoder,
    XHDR_PROTECTED | XHDR_GLOBAL, false },
  { "GNU.volume.offset", volume_offset_coder, volume_offset_decoder,
    XHDR_PROTECTED | XHDR_GLOBAL, false },

  /* We get the SELinux value from filecon, so add a namespace for SELinux
     instead of storing it in SCHILY.xattr.* (which would be RAW). */
  { "RHT.security.selinux",
    xattr_selinux_coder, xattr_selinux_decoder, 0, false },

  /* ACLs, use the star format... */
  { "SCHILY.acl.access",
    xattr_acls_a_coder, xattr_acls_a_decoder, 0, false },

  { "SCHILY.acl.default",
    xattr_acls_d_coder, xattr_acls_d_decoder, 0, false },

  /* We are storing all extended attributes using this rule even if some of them
     were stored by some previous rule (duplicates) -- we just have to make sure
     they are restored *only once* during extraction later on. */
  { "SCHILY.xattr", xattr_coder, xattr_decoder, 0, true },

  { NULL, NULL, NULL, 0, false }
};
