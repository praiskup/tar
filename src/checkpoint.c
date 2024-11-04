/* Checkpoint management for tar.

   Copyright 2007-2024 Free Software Foundation, Inc.

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
#include "common.h"

#include <wordsplit.h>

#include <flexmember.h>
#include <fprintftime.h>

#include <sys/ioctl.h>
#include <termios.h>
#include <signal.h>

enum checkpoint_opcode
  {
    cop_dot,
    cop_bell,
    cop_echo,
    cop_ttyout,
    cop_sleep,
    cop_exec,
    cop_totals,
    cop_wait
  };

struct checkpoint_action
{
  struct checkpoint_action *next;
  enum checkpoint_opcode opcode;
  union
  {
    time_t time;
    char *command;
    int signal;
  } v;
  char commandbuf[FLEXIBLE_ARRAY_MEMBER];
};

/* Checkpointing counter */
static intmax_t checkpoint;

/* List of checkpoint actions */
static struct checkpoint_action *checkpoint_action,
  **checkpoint_action_tail = &checkpoint_action;

/* State of the checkpoint system */
static enum {
  CHKP_INIT,       /* Needs initialization */
  CHKP_COMPILE,    /* Actions are being compiled */
  CHKP_RUN         /* Actions are being run */
} checkpoint_state;
/* Blocked signals */
static sigset_t sigs;

static struct checkpoint_action *
alloc_action (enum checkpoint_opcode opcode, char const *quoted_string)
{
  idx_t quoted_size = quoted_string ? strlen (quoted_string) + 1 : 0;
  struct checkpoint_action *p = xmalloc (FLEXSIZEOF (struct checkpoint_action,
						     commandbuf, quoted_size));
  *checkpoint_action_tail = p;
  checkpoint_action_tail = &p->next;
  p->next = NULL;
  p->opcode = opcode;
  if (quoted_string)
    {
      p->v.command = memcpy (p->commandbuf, quoted_string, quoted_size);
      unquote_string (p->v.command);
    }
  return p;
}

void
checkpoint_compile_action (const char *str)
{
  if (checkpoint_state == CHKP_INIT)
    {
      sigemptyset (&sigs);
      checkpoint_state = CHKP_COMPILE;
    }

  if (strcmp (str, ".") == 0 || strcmp (str, "dot") == 0)
    alloc_action (cop_dot, NULL);
  else if (strcmp (str, "bell") == 0)
    alloc_action (cop_bell, NULL);
  else if (strcmp (str, "echo") == 0)
    alloc_action (cop_echo, NULL)->v.command = NULL;
  else if (strncmp (str, "echo=", 5) == 0)
    alloc_action (cop_echo, str + 5);
  else if (strncmp (str, "exec=", 5) == 0)
    alloc_action (cop_exec, str + 5);
  else if (strncmp (str, "ttyout=", 7) == 0)
    alloc_action (cop_ttyout, str + 7);
  else if (strncmp (str, "sleep=", 6) == 0)
    {
      char const *arg = str + 6;
      char *p;
      alloc_action (cop_sleep, NULL)->v.time
	= stoint (arg, &p, NULL, 0, TYPE_MAXIMUM (time_t));
      if ((p == arg) | *p)
	paxfatal (0, _("%s: not a valid timeout"), str);
    }
  else if (strcmp (str, "totals") == 0)
    alloc_action (cop_totals, NULL);
  else if (strncmp (str, "wait=", 5) == 0)
    {
      int sig = decode_signal (str + 5);
      alloc_action (cop_wait, NULL)->v.signal = sig;
      sigaddset (&sigs, sig);
    }
  else
    paxfatal (0, _("%s: unknown checkpoint action"), str);
}

void
checkpoint_finish_compile (void)
{
  if (checkpoint_state == CHKP_INIT
      && checkpoint_option
      && !checkpoint_action)
    {
      /* Provide a historical default */
      checkpoint_compile_action ("echo");
    }

  if (checkpoint_state == CHKP_COMPILE)
    {
      sigprocmask (SIG_BLOCK, &sigs, NULL);

      if (!checkpoint_option)
	/* set default checkpoint rate */
	checkpoint_option = DEFAULT_CHECKPOINT;

      checkpoint_state = CHKP_RUN;
    }
}

static intmax_t
getwidth (FILE *fp)
{
  char const *columns;

#ifdef TIOCGWINSZ
  struct winsize ws;
  if (ioctl (fileno (fp), TIOCGWINSZ, &ws) == 0 && 0 < ws.ws_col)
    return ws.ws_col;
#endif

  columns = getenv ("COLUMNS");
  if (columns)
    {
      char *end;
      intmax_t col = stoint (columns, &end, NULL, 0, INTMAX_MAX);
      if (! (*end | !col))
	return col;
    }

  return 80;
}

static char *
getarg (char const *input, char const **endp, char **argbuf, idx_t *arglen)
{
  if (input[0] == '{')
    {
      char *p = strchr (input + 1, '}');
      if (p)
	{
	  idx_t n = p - input;
	  if (n > *arglen)
	    *argbuf = xpalloc (*argbuf, arglen, n - *arglen, -1, 1);
	  n--;
	  *endp = p + 1;
	  (*argbuf)[n] = 0;
	  return memcpy (*argbuf, input + 1, n);
	}
    }

  *endp = input;
  return NULL;
}

static bool tty_cleanup;

static const char *def_format =
  "%{%Y-%m-%d %H:%M:%S}t: %ds, %{read,wrote}T%*\r";

static intmax_t
format_checkpoint_string (FILE *fp, intmax_t len,
			  const char *input, bool do_write,
			  intmax_t cpn)
{
  const char *opstr = do_write ? gettext ("write") : gettext ("read");
  const char *ip;

  static char *argbuf = NULL;
  static idx_t arglen = 0;
  char *arg = NULL;

  if (!input)
    {
      if (do_write)
	/* TRANSLATORS: This is a "checkpoint of write operation",
	 *not* "Writing a checkpoint". */
	input = gettext ("Write checkpoint %u");
      else
	/* TRANSLATORS: This is a "checkpoint of read operation",
	 *not* "Reading a checkpoint". */
	input = gettext ("Read checkpoint %u");
    }

  for (ip = input; *ip; ip++)
    {
      if (*ip == '%')
	{
	  if (*++ip == '{')
	    {
	      arg = getarg (ip, &ip, &argbuf, &arglen);
	      if (!arg)
		{
		  fputc ('%', fp);
		  fputc (*ip, fp);
		  len = add_printf (len, 2);
		  continue;
		}
	    }
	  switch (*ip)
	    {
	    case 'c':
	      len = add_printf (len,
				format_checkpoint_string (fp, len, def_format,
							  do_write, cpn));
	      break;

	    case 'u':
	      len = add_printf (len, fprintf (fp, "%jd", cpn));
	      break;

	    case 's':
	      fputs (opstr, fp);
	      len = add_printf (len, strlen (opstr));
	      break;

	    case 'd':
	      len = add_printf (len,
				fprintf (fp, "%.0f",
					 compute_duration_ns () / BILLION));
	      break;

	    case 'T':
	      {
		static char const *const checkpoint_total_format[]
		  = { "R", "W", "D" };
		char const *const *fmt = checkpoint_total_format, *fmtbuf[3];
		struct wordsplit ws;
		compute_duration_ns ();

		if (arg)
		  {
		    ws.ws_delim = ",";
		    if (wordsplit (arg, &ws,
				   (WRDSF_NOVAR | WRDSF_NOCMD
				    | WRDSF_QUOTE | WRDSF_DELIM))
			!= WRDSE_OK)
		      paxerror (0, _("cannot split string '%s': %s"),
				arg, wordsplit_strerror (&ws));
		    else if (3 < ws.ws_wordc)
		      paxerror (0, _("too many words in '%s'"), arg);
		    else
		      {
			int i;

			for (i = 0; i < ws.ws_wordc; i++)
			  fmtbuf[i] = ws.ws_wordv[i];
			for (; i < 3; i++)
			  fmtbuf[i] = NULL;
			fmt = fmtbuf;
		      }
		  }
		len = add_printf (len, format_total_stats (fp, fmt, ',', 0));
		if (arg)
		  wordsplit_free (&ws);
	      }
	      break;

	    case 't':
	      {
		struct timespec ts = current_timespec ();
		const char *fmt = arg ? arg : "%c";
		struct tm *tm = localtime (&ts.tv_sec);
		len = add_printf (len,
				  (tm ? fprintftime (fp, fmt, tm, 0, ts.tv_nsec)
				   : fprintf (fp, "????""-??""-?? ??:??:??")));
	      }
	      break;

	    case '*':
	      if (0 <= len)
		{
		  intmax_t w;
		  if (!arg)
		    w = getwidth (fp);
		  else
		    {
		      char *end;
		      w = stoint (arg, &end, NULL, 0, INTMAX_MAX);
		      if ((end == arg) | *end)
			w = 80;
		    }
		  for (; w > len; len++)
		    fputc (' ', fp);
		}
	      break;

	    default:
	      fputc ('%', fp);
	      fputc (*ip, fp);
	      len = add_printf (len, 2);
	      break;
	    }
	  arg = NULL;
	}
      else
	{
	  fputc (*ip, fp);
	  if (*ip == '\r')
	    {
	      len = 0;
	      tty_cleanup = true;
	    }
	  else
	    len = add_printf (len, 1);
	}
    }
  fflush (fp);
  return len;
}

static FILE *tty = NULL;

static void
run_checkpoint_actions (bool do_write)
{
  struct checkpoint_action *p;

  for (p = checkpoint_action; p; p = p->next)
    {
      switch (p->opcode)
	{
	case cop_dot:
	  fputc ('.', stdlis);
	  fflush (stdlis);
	  break;

	case cop_bell:
	  if (!tty)
	    tty = fopen ("/dev/tty", "w");
	  if (tty)
	    {
	      fputc ('\a', tty);
	      fflush (tty);
	    }
	  break;

	case cop_echo:
	  {
	    int n = fprintf (stderr, "%s: ", program_name);
	    format_checkpoint_string (stderr, n, p->v.command, do_write,
				      checkpoint);
	    fputc ('\n', stderr);
	  }
	  break;

	case cop_ttyout:
	  if (!tty)
	    tty = fopen ("/dev/tty", "w");
	  if (tty)
	    format_checkpoint_string (tty, 0, p->v.command, do_write,
				      checkpoint);
	  break;

	case cop_sleep:
	  sleep (p->v.time);
	  break;

	case cop_exec:
	  sys_exec_checkpoint_script (p->v.command,
				      archive_name_cursor[0],
				      checkpoint);
	  break;

	case cop_totals:
	  compute_duration_ns ();
	  print_total_stats ();
	  break;

	case cop_wait:
	  {
	    int n;
	    sigwait (&sigs, &n);
	  }
	}
    }
}

void
checkpoint_flush_actions (void)
{
  struct checkpoint_action *p;

  for (p = checkpoint_action; p; p = p->next)
    {
      switch (p->opcode)
	{
	case cop_ttyout:
	  if (tty && tty_cleanup)
	    {
	      intmax_t w = getwidth (tty);
	      while (w--)
		fputc (' ', tty);
	      fputc ('\r', tty);
	      fflush (tty);
	    }
	  break;
	default:
	  /* nothing */;
	}
    }
}

void
checkpoint_run (bool do_write)
{
  if (checkpoint_option && !(++checkpoint % checkpoint_option))
    run_checkpoint_actions (do_write);
}

void
checkpoint_finish (void)
{
  if (checkpoint_option)
    {
      checkpoint_flush_actions ();
      if (tty)
	fclose (tty);
    }
}
