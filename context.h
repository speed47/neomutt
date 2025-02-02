/**
 * @file
 * The "currently-open" mailbox
 *
 * @authors
 * Copyright (C) 2017 Richard Russon <rich@flatcap.org>
 *
 * @copyright
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 2 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef MUTT_CONTEXT_H
#define MUTT_CONTEXT_H

#include <stdbool.h>
#include <sys/types.h>

struct Email;
struct EmailList;
struct NotifyCallback;

/**
 * struct Context - The "current" mailbox
 */
struct Context
{
  off_t vsize;
  char *pattern;                     ///< Limit pattern string
  struct PatternHead *limit_pattern; ///< Compiled limit pattern
  struct Email *last_tag;            ///< Last tagged msg (used to link threads)
  struct MuttThread *tree;           ///< Top of thread tree
  struct Hash *thread_hash;          ///< Hash table for threading
  int msg_not_read_yet;              ///< Which msg "new" in pager, -1 if none

  struct Menu *menu;                 ///< Needed for pattern compilation

  bool collapsed : 1;                ///< Are all threads collapsed?

  struct Mailbox *mailbox;
  struct Notify *notify;             ///< Notifications handler
};

/**
 * struct EventContext - An Event that happened to an Context
 */
struct EventContext
{
  struct Context *context; ///< The Context this Event relates to
};

/**
 * enum NotifyContext - Types of Context Event
 */
enum NotifyContext
{
  NT_CONTEXT_OPEN = 1, ///< The Context has been opened
  NT_CONTEXT_CLOSE,    ///< The Context is about to be destroyed
};

void            ctx_free            (struct Context **ptr);
int             ctx_mailbox_observer(struct NotifyCallback *nc);
struct Context *ctx_new             (void);
void            ctx_update          (struct Context *ctx);
void            ctx_update_tables   (struct Context *ctx, bool committing);

bool message_is_tagged (struct Context *ctx, int index);
bool message_is_visible(struct Context *ctx, int index);

int  el_add_email   (struct EmailList *el, struct Email *e);
int  el_add_tagged  (struct EmailList *el, struct Context *ctx, struct Email *e, bool use_tagged);
void emaillist_clear(struct EmailList *el);

#endif /* MUTT_CONTEXT_H */
