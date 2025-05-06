/* This file is part of GNU tar.
   Copyright 2006-2025 Free Software Foundation, Inc.

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
#include <regex.h>
#include <mcel.h>
#include <quotearg.h>
#include "common.h"

enum transform_type
  {
    transform_first,
    transform_global
  };

enum replace_segm_type
  {
    segm_literal,   /* Literal segment */
    segm_backref,   /* Back-reference segment */
    segm_case_ctl   /* Case control segment (GNU extension) */
  };

enum case_ctl_type
  {
    ctl_stop,       /* Stop case conversion */
    ctl_upcase_next,/* Turn the next character to uppercase */
    ctl_locase_next,/* Turn the next character to lowercase */
    ctl_upcase,     /* Turn the replacement to uppercase until ctl_stop */
    ctl_locase      /* Turn the replacement to lowercase until ctl_stop */
  };

struct replace_segm
{
  struct replace_segm *next;
  enum replace_segm_type type;
  union
  {
    struct
    {
      char *ptr;
      idx_t size;
    } literal;                /* type == segm_literal */
    idx_t ref;		      /* type == segm_backref */
    enum case_ctl_type ctl;   /* type == segm_case_ctl */
  } v;
};

struct transform
{
  struct transform *next;
  enum transform_type transform_type;
  int flags;
  idx_t match_number;
  regex_t regex;
  /* Compiled replacement expression */
  struct replace_segm *repl_head, *repl_tail;
  idx_t segm_count; /* Number of elements in the above list */
};



static int transform_flags = XFORM_ALL;
static struct transform *transform_head, *transform_tail;

static struct transform *
new_transform (void)
{
  struct transform *p = xzalloc (sizeof *p);
  if (transform_tail)
    transform_tail->next = p;
  else
    transform_head = p;
  transform_tail = p;
  return p;
}

static struct replace_segm *
add_segment (struct transform *tf)
{
  struct replace_segm *segm = xmalloc (sizeof *segm);
  segm->next = NULL;
  if (tf->repl_tail)
    tf->repl_tail->next = segm;
  else
    tf->repl_head = segm;
  tf->repl_tail = segm;
  tf->segm_count++;
  return segm;
}

static void
add_literal_segment (struct transform *tf, const char *str, const char *end)
{
  idx_t len = end - str;
  if (len)
    {
      struct replace_segm *segm = add_segment (tf);
      segm->type = segm_literal;
      segm->v.literal.ptr = xmalloc (len + 1);
      memcpy (segm->v.literal.ptr, str, len);
      segm->v.literal.ptr[len] = 0;
      segm->v.literal.size = len;
    }
}

static void
add_char_segment (struct transform *tf, char chr)
{
  struct replace_segm *segm = add_segment (tf);
  segm->type = segm_literal;
  segm->v.literal.ptr = xmalloc (2);
  segm->v.literal.ptr[0] = chr;
  segm->v.literal.ptr[1] = 0;
  segm->v.literal.size = 1;
}

static void
add_backref_segment (struct transform *tf, idx_t ref)
{
  struct replace_segm *segm = add_segment (tf);
  segm->type = segm_backref;
  segm->v.ref = ref;
}

static bool
parse_xform_flags (int *pflags, char c)
{
  switch (c)
    {
    case 'r':
      *pflags |= XFORM_REGFILE;
      break;

    case 'R':
      *pflags &= ~XFORM_REGFILE;
      break;

    case 'h':
      *pflags |= XFORM_LINK;
      break;

    case 'H':
      *pflags &= ~XFORM_LINK;
      break;

    case 's':
      *pflags |= XFORM_SYMLINK;
      break;

    case 'S':
      *pflags &= ~XFORM_SYMLINK;
      break;

    default:
      return false;
    }
  return true;
}

static void
add_case_ctl_segment (struct transform *tf, enum case_ctl_type ctl)
{
  struct replace_segm *segm = add_segment (tf);
  segm->type = segm_case_ctl;
  segm->v.ctl = ctl;
}

static const char *
parse_transform_expr (const char *expr)
{
  idx_t i, j;
  char *str, *beg, *cur;
  const char *p;
  int cflags = 0;
  struct transform *tf = new_transform ();

  if (expr[0] != 's')
    {
      if (strncmp (expr, "flags=", 6) == 0)
	{
	  transform_flags = 0;
	  for (expr += 6; *expr; expr++)
	    {
	      if (*expr == ';')
		{
		  expr++;
		  break;
		}
	      if (!parse_xform_flags (&transform_flags, *expr))
		paxusage (_("Unknown transform flag: %c"), *expr);
	    }
	  return expr;
	}
      paxusage (_("Invalid transform expression"));
    }

  char delim = expr[1];
  if (!delim)
    paxusage (_("Invalid transform expression"));

  /* Scan regular expression */
  for (i = 2; expr[i] && expr[i] != delim; i++)
    if (expr[i] == '\\' && expr[i+1])
      i++;

  if (expr[i] != delim)
    paxusage (_("Invalid transform expression"));

  /* Scan replacement expression */
  for (j = i + 1; expr[j] && expr[j] != delim; j++)
    if (expr[j] == '\\' && expr[j+1])
      j++;

  if (expr[j] != delim)
    paxusage (_("Invalid transform expression"));

  /* Check flags */
  tf->transform_type = transform_first;
  tf->flags = transform_flags;
  for (p = expr + j + 1; *p && *p != ';'; p++)
    switch (*p)
      {
      case 'g':
	tf->transform_type = transform_global;
	break;

      case 'i':
	cflags |= REG_ICASE;
	break;

      case 'x':
	cflags |= REG_EXTENDED;
	break;

      case '0': case '1': case '2': case '3': case '4':
      case '5': case '6': case '7': case '8': case '9':
	{
	  char *endp;
	  tf->match_number = stoint (p, &endp, NULL, 0, IDX_MAX);
	  p = endp - 1;
	}
	break;

      default:
	if (!parse_xform_flags (&tf->flags, *p))
	  paxusage (_("Unknown flag in transform expression: %c"), *p);
      }

  if (*p == ';')
    p++;

  /* Extract and compile regex */
  str = xmalloc (i - 1);
  memcpy (str, expr + 2, i - 2);
  str[i - 2] = 0;

  int rc = regcomp (&tf->regex, str, cflags);

  if (rc)
    {
      char errbuf[512];
      regerror (rc, &tf->regex, errbuf, sizeof (errbuf));
      paxusage (_("Invalid transform expression: %s"), errbuf);
    }

  if (str[0] == '^' || (i > 2 && str[i - 3] == '$'))
    tf->transform_type = transform_first;

  free (str);

  /* Extract and compile replacement expr */
  i++;
  str = xmalloc (j - i + 1);
  memcpy (str, expr + i, j - i);
  str[j - i] = 0;

  for (cur = beg = str; *cur;)
    {
      if (*cur == '\\')
	{
	  add_literal_segment (tf, beg, cur);
	  switch (*++cur)
	    {
	    case '0': case '1': case '2': case '3': case '4':
	    case '5': case '6': case '7': case '8': case '9':
	      {
		idx_t n = stoint (cur, &cur, NULL, 0, IDX_MAX);
		if (tf->regex.re_nsub < n)
		  paxusage (_("Invalid transform replacement:"
			      " back reference out of range"));
		add_backref_segment (tf, n);
	      }
	      break;

	    case '\\':
	      add_char_segment (tf, '\\');
	      cur++;
	      break;

	    case 'a':
	      add_char_segment (tf, '\a');
	      cur++;
	      break;

	    case 'b':
	      add_char_segment (tf, '\b');
	      cur++;
	      break;

	    case 'f':
	      add_char_segment (tf, '\f');
	      cur++;
	      break;

	    case 'n':
	      add_char_segment (tf, '\n');
	      cur++;
	      break;

	    case 'r':
	      add_char_segment (tf, '\r');
	      cur++;
	      break;

	    case 't':
	      add_char_segment (tf, '\t');
	      cur++;
	      break;

	    case 'v':
	      add_char_segment (tf, '\v');
	      cur++;
	      break;

	    case '&':
	      add_char_segment (tf, '&');
	      cur++;
	      break;

	    case 'L':
	      /* Turn the replacement to lowercase until a '\U' or '\E'
		 is found, */
	      add_case_ctl_segment (tf, ctl_locase);
	      cur++;
	      break;

	    case 'l':
	      /* Turn the next character to lowercase, */
	      add_case_ctl_segment (tf, ctl_locase_next);
	      cur++;
	      break;

	    case 'U':
	      /* Turn the replacement to uppercase until a '\L' or '\E'
		 is found, */
	      add_case_ctl_segment (tf, ctl_upcase);
	      cur++;
	      break;

	    case 'u':
	      /* Turn the next character to uppercase, */
	      add_case_ctl_segment (tf, ctl_upcase_next);
	      cur++;
	      break;

	    case 'E':
	      /* Stop case conversion started by '\L' or '\U'. */
	      add_case_ctl_segment (tf, ctl_stop);
	      cur++;
	      break;

	    default:
	      if (*cur == delim)
		add_char_segment (tf, delim);
	      else
		{
		  char buf[2];
		  buf[0] = '\\';
		  buf[1] = *cur;
		  add_literal_segment (tf, buf, buf + 2);
		}
	      cur++;
	      break;
	    }
	  beg = cur;
	}
      else if (*cur == '&')
	{
	  add_literal_segment (tf, beg, cur);
	  add_backref_segment (tf, 0);
	  beg = ++cur;
	}
      else
	cur++;
    }
  add_literal_segment (tf, beg, cur);
  free(str);

  return p;
}

void
set_transform_expr (const char *expr)
{
  while (*expr)
    expr = parse_transform_expr (expr);
}


static struct obstack stk;
static bool stk_init;

/* Run case conversion specified by CASE_CTL on array PTR of SIZE
   characters.  Append the result to STK.  */
static void
run_case_conv (enum case_ctl_type case_ctl, char *ptr, idx_t size)
{
  char const *p = ptr, *plim = ptr + size;
  mbstate_t mbs; mbszero (&mbs);
  while (p < plim)
    {
      mcel_t g = mcel_scan (p, plim);
      char32_t ch;
      switch (case_ctl)
	{
	case ctl_upcase: case ctl_upcase_next: ch = c32toupper (g.ch); break;
	case ctl_locase: case ctl_locase_next: ch = c32tolower (g.ch); break;
	default: ch = g.ch; break;
	}
      if (ch == g.ch)
	obstack_grow (&stk, p, g.len);
      else
	{
	  obstack_make_room (&stk, MB_LEN_MAX);
	  mbstate_t ombs; mbszero (&ombs);
	  idx_t outbytes = c32rtomb (obstack_next_free (&stk), ch, &ombs);
	  obstack_blank_fast (&stk, outbytes);
	}
      p += g.len;
      if (case_ctl != ctl_upcase && case_ctl != ctl_locase)
	break;
    }

  obstack_grow (&stk, p, plim - p);
}

static void
_single_transform_name_to_obstack (struct transform *tf, char *input)
{
  int rc;
  idx_t nmatches = 0;
  enum case_ctl_type case_ctl = ctl_stop,  /* Current case conversion op */
                     save_ctl = ctl_stop;  /* Saved case_ctl for \u and \l */
  regmatch_t *rmp = xinmalloc (tf->regex.re_nsub + 1, sizeof *rmp);

  while (*input)
    {
      idx_t disp;

      rc = regexec (&tf->regex, input, tf->regex.re_nsub + 1, rmp, 0);

      if (rc == 0)
	{
	  struct replace_segm *segm;

	  disp = rmp[0].rm_eo;

	  nmatches++;
	  if (tf->match_number && nmatches < tf->match_number)
	    {
	      obstack_grow (&stk, input, disp);
	      input += disp;
	      continue;
	    }

	  if (rmp[0].rm_so)
	    obstack_grow (&stk, input, rmp[0].rm_so);

	  for (segm = tf->repl_head; segm; segm = segm->next)
	    {
	      switch (segm->type)
		{
		case segm_literal:    /* Literal segment */
		  run_case_conv (case_ctl,
				 segm->v.literal.ptr,
				 segm->v.literal.size);
		case_ctl_reset:
		  /* Reset case conversion after a single-char operation.  */
		  if (case_ctl == ctl_upcase_next
		      || case_ctl == ctl_locase_next)
		    {
		      case_ctl = save_ctl;
		      save_ctl = ctl_stop;
		    }
		  break;

		case segm_backref:    /* Back-reference segment */
		  if (0 <= rmp[segm->v.ref].rm_so
		      && 0 <= rmp[segm->v.ref].rm_eo)
		    {
		      idx_t size = (rmp[segm->v.ref].rm_eo
				    - rmp[segm->v.ref].rm_so);
		      run_case_conv (case_ctl,
				     input + rmp[segm->v.ref].rm_so, size);
		      goto case_ctl_reset;
		    }
		  break;

		case segm_case_ctl:
		  switch (segm->v.ctl)
		    {
		    case ctl_upcase_next:
		    case ctl_locase_next:
		      switch (save_ctl)
			{
			case ctl_stop:
			case ctl_upcase:
			case ctl_locase:
			  save_ctl = case_ctl;
			default:
			  break;
			}
		      FALLTHROUGH;

		    case ctl_upcase:
		    case ctl_locase:
		    case ctl_stop:
		      case_ctl = segm->v.ctl;
		    }
		}
	    }
	}
      else
	{
	  disp = strlen (input);
	  obstack_grow (&stk, input, disp);
	}

      input += disp;

      if (tf->transform_type == transform_first)
	{
	  obstack_grow (&stk, input, strlen (input));
	  break;
	}
    }

  obstack_1grow (&stk, 0);
  free (rmp);
}

static void
_transform_name_to_obstack (int flags, char *input, char **output)
{
  struct transform *tf;
  bool ok = false;

  if (!stk_init)
    {
      obstack_init (&stk);
      stk_init = true;
    }

  for (tf = transform_head; tf; tf = tf->next)
    {
      if (tf->flags & flags)
	{
	  _single_transform_name_to_obstack (tf, input);
	  input = obstack_finish (&stk);
	  ok = true;
	}
    }
  if (!ok)
    {
      obstack_grow0 (&stk, input, strlen (input));
      input = obstack_finish (&stk);
    }
  *output = input;
}

/* Transform name *PINPUT of a file or archive member of type TYPE
   (a single XFORM_* bit).  If FUN is not NULL, call this function
   to further transform the result.  Arguments to FUN are the transformed
   name and type, it's return value is the new transformed name.

   If transformation results in a non-empty string, store the result in
   *PINPUT and return true.  Otherwise, if it results in an empty string,
   issue a warning, return false and don't modify PINPUT.
 */
bool
transform_name_fp (char **pinput, int type,
		   char const *(*fun) (char const *, int))
{
  char *str;
  char const *result;

  _transform_name_to_obstack (type, *pinput, &str);
  result = (str[0] != 0 && fun) ? fun (str, type) : str;

  if (result[0] == 0)
    {
      warnopt (WARN_EMPTY_TRANSFORM, 0,
	       _("%s: transforms to empty name"), quotearg_colon (*pinput));
      obstack_free (&stk, str);
      return false;
    }

  assign_string (pinput, result);
  obstack_free (&stk, str);
  return true;
}

bool
transform_name (char **pinput, int type)
{
  return transform_name_fp (pinput, type, NULL);
}

bool
transform_program_p (void)
{
  return transform_head != NULL;
}
