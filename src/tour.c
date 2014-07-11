/* Create a tar archive.

   Copyright 2015 Free Software Foundation, Inc.

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

   Routines making the directory traversal without function recursion.  This
   avoids segfaults due to small stack space for rarely deep directory
   hierarchies.

   Written by Pavel Raiskup <praiskup@redhat.com>, on 2014-07-15.  */


#include <system.h>

#include "common.h"

#include <assert.h>
#include <gl_xlist.h>
#include <gl_array_list.h>

struct tour
{
  gl_list_t list;
  gl_list_node_t current;
};


static tour_node_t *
tour_node_new (void)
{
  tour_node_t *node = xzalloc (sizeof (tour_node_t));
  tar_stat_init (&node->st);
  return node;
}

tour_t
tour_init (const char *initial_name, struct tar_stat_info * parent)
{
  size_t len = strlen (initial_name);
  tour_node_t *node;
  tour_t t;

  node = tour_node_new ();
  node->items = xmalloc (len + 2);
  node->items[len + 1] = 0;     /* double 0 */
  strcpy (node->items, initial_name);
  node->parent = parent;

  /* current item is the first one */
  node->item = node->items;

  t = xzalloc (sizeof (struct tour));
  t->list = gl_list_create_empty (GL_ARRAY_LIST, NULL, NULL, NULL, 1);

  t->current = gl_list_add_last (t->list, node);
  return t;
}

void
tour_plan_dir (tour_t t, char *names)
{
  tour_node_t *node, *curr;

  curr = tour_current (t);

  if (strlen (names) == 0)
    {
      /* no child planned */
      free (names);
      return;
    }

  node = tour_node_new ();
  node->items = names;
  node->parent = &curr->st;
  node->item = node->items;

  gl_list_add_last (t->list, node);
}

void
tour_plan_file (tour_t t, const char *name)
{
  size_t nlen = strlen (name);
  char *dirlist = xmalloc (nlen + 2);
  dirlist[nlen + 1] = 0;
  strcpy (dirlist, name);

  tour_plan_dir (t, dirlist);
}

tour_node_t *
tour_current (tour_t t)
{
  if (!t->current)
    return NULL;
  return (tour_node_t *) gl_list_node_value (t->list, t->current);
}

bool
tour_has_child (tour_t t)
{
  return !!gl_list_next_node (t->list, t->current);
}

static tour_node_t *
tour_next_node (tour_t t)
{
  gl_list_node_t next = gl_list_next_node (t->list, t->current);
  if (!next)
    return NULL;

  t->current = next;
  return (tour_node_t *) gl_list_node_value (t->list, next);
}

static void
tour_destroy_node (tour_node_t * n)
{
  tar_stat_destroy (&n->st);
  free (n->namebuf);
  free (n->items);
  free (n);
}

static tour_node_t *
tour_prev_node (tour_t t)
{
  tour_node_t *data = tour_current (t);

  /* we are going to drop current node -> make sure parent
     fd is opened */
  if (data->parent)
    restore_parent_fd (data->parent);

  gl_list_node_t n = gl_list_previous_node (t->list, t->current);;
  gl_list_remove_node (t->list, t->current);
  t->current = n;

  data->st.parent = data->parent;

  tour_destroy_node (data);

  return tour_current (t);
}

/* Obtain the next NAME and FULLNAME (full path) of file in tour_t to bue dumped
   (in correct order). */
bool
tour_next (tour_t t, const char **name, const char **fullname)
{
  size_t parent_nlen = 0, nlen = 0, len = 0;
  tour_node_t *curr, *next;

  curr = tour_current (t);

  /* go to the next node when exists (go down to subfolder) */
  if ((next = tour_next_node (t)))
    curr = next;

  /* when ended up the current directory, try to go up in directory */
  while (!curr->item[0])
    {
      if ((curr = tour_prev_node (t)) == NULL)
        /* last member-item has been processed */
        return false;
    }

  /* now curr points to correct node/item */

  /* re-init ST, TODO: here is good place to save some malloc/free calls? */
  tar_stat_destroy (&curr->st);
  curr->st.parent = curr->parent;

  nlen = strlen (curr->item);
  if (curr->parent)
    parent_nlen = strlen (curr->parent->orig_file_name);

  len = nlen + parent_nlen + 1;

  if (!curr->namebuf)
    {
      /* first item of particular tour_node_t is handled */
      curr->namebuf = xmalloc (len);
      if (curr->parent)
        strcpy (curr->namebuf, curr->parent->orig_file_name);
      curr->buflen = len;
    }
  else if (curr->buflen < len)
    {
      curr->namebuf = xrealloc (curr->namebuf, len);
      curr->buflen = len;
    }

  strcpy (curr->namebuf + parent_nlen, curr->item);
  *name = curr->item;
  if (fullname)
    *fullname = curr->namebuf;

  /* move the pointer for successive call tour () call */
  curr->item += nlen + 1;

  return true;
}

void
tour_free (tour_t t)
{
  const void *data;

  gl_list_iterator_t it = gl_list_iterator (t->list);
  while (gl_list_iterator_next (&it, &data, NULL))
    {
      tour_node_t *n = (tour_node_t *) data;
      tour_destroy_node (n);
    }

  gl_list_free (t->list);
  free (t);
}
