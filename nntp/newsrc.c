/**
 * @file
 * Read/parse/write an NNTP config file of subscribed newsgroups
 *
 * @authors
 * Copyright (C) 1998 Brandon Long <blong@fiction.net>
 * Copyright (C) 1999 Andrej Gritsenko <andrej@lucky.net>
 * Copyright (C) 2000-2017 Vsevolod Volkov <vvv@mutt.org.ua>
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

/**
 * @page nntp_newsrc Read/parse/write an NNTP config file of subscribed newsgroups
 *
 * Read/parse/write an NNTP config file of subscribed newsgroups
 */

#include "config.h"
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "nntp_private.h"
#include "mutt/mutt.h"
#include "config/lib.h"
#include "email/lib.h"
#include "core/lib.h"
#include "conn/conn.h"
#include "mutt.h"
#include "bcache.h"
#include "format_flags.h"
#include "globals.h"
#include "hcache/hcache.h"
#include "mutt_account.h"
#include "mutt_logging.h"
#include "mutt_socket.h"
#include "mutt_window.h"
#include "muttlib.h"
#include "nntp.h"
#include "protos.h"
#include "sort.h"

/* These Config Variables are only used in nntp/newsrc.c */
char *C_NewsCacheDir; ///< Config: (nntp) Directory for cached news articles
char *C_Newsrc; ///< Config: (nntp) File containing list of subscribed newsgroups

struct BodyCache;

/**
 * mdata_find - Find NntpMboxData for given newsgroup or add it
 * @param adata NNTP server
 * @param group Newsgroup
 * @retval ptr  NNTP data
 * @retval NULL Error
 */
static struct NntpMboxData *mdata_find(struct NntpAccountData *adata, const char *group)
{
  struct NntpMboxData *mdata = mutt_hash_find(adata->groups_hash, group);
  if (mdata)
    return mdata;

  size_t len = strlen(group) + 1;
  /* create NntpMboxData structure and add it to hash */
  mdata = mutt_mem_calloc(1, sizeof(struct NntpMboxData) + len);
  mdata->group = (char *) mdata + sizeof(struct NntpMboxData);
  mutt_str_strfcpy(mdata->group, group, len);
  mdata->adata = adata;
  mdata->deleted = true;
  mutt_hash_insert(adata->groups_hash, mdata->group, mdata);

  /* add NntpMboxData to list */
  if (adata->groups_num >= adata->groups_max)
  {
    adata->groups_max *= 2;
    mutt_mem_realloc(&adata->groups_list, adata->groups_max * sizeof(mdata));
  }
  adata->groups_list[adata->groups_num++] = mdata;

  return mdata;
}

/**
 * nntp_acache_free - Remove all temporarily cache files
 * @param mdata NNTP Mailbox data
 */
void nntp_acache_free(struct NntpMboxData *mdata)
{
  for (int i = 0; i < NNTP_ACACHE_LEN; i++)
  {
    if (mdata->acache[i].path)
    {
      unlink(mdata->acache[i].path);
      FREE(&mdata->acache[i].path);
    }
  }
}

/**
 * nntp_newsrc_close - Unlock and close .newsrc file
 * @param adata NNTP server
 */
void nntp_newsrc_close(struct NntpAccountData *adata)
{
  if (!adata->fp_newsrc)
    return;

  mutt_debug(LL_DEBUG1, "Unlocking %s\n", adata->newsrc_file);
  mutt_file_unlock(fileno(adata->fp_newsrc));
  mutt_file_fclose(&adata->fp_newsrc);
}

/**
 * nntp_group_unread_stat - Count number of unread articles using .newsrc data
 * @param mdata NNTP Mailbox data
 */
void nntp_group_unread_stat(struct NntpMboxData *mdata)
{
  mdata->unread = 0;
  if ((mdata->last_message == 0) || (mdata->first_message > mdata->last_message))
    return;

  mdata->unread = mdata->last_message - mdata->first_message + 1;
  for (unsigned int i = 0; i < mdata->newsrc_len; i++)
  {
    anum_t first = mdata->newsrc_ent[i].first;
    if (first < mdata->first_message)
      first = mdata->first_message;
    anum_t last = mdata->newsrc_ent[i].last;
    if (last > mdata->last_message)
      last = mdata->last_message;
    if (first <= last)
      mdata->unread -= last - first + 1;
  }
}

/**
 * nntp_newsrc_parse - Parse .newsrc file
 * @param adata NNTP server
 * @retval  0 Not changed
 * @retval  1 Parsed
 * @retval -1 Error
 */
int nntp_newsrc_parse(struct NntpAccountData *adata)
{
  char *line = NULL;
  struct stat sb;

  if (adata->fp_newsrc)
  {
    /* if we already have a handle, close it and reopen */
    mutt_file_fclose(&adata->fp_newsrc);
  }
  else
  {
    /* if file doesn't exist, create it */
    adata->fp_newsrc = mutt_file_fopen(adata->newsrc_file, "a");
    mutt_file_fclose(&adata->fp_newsrc);
  }

  /* open .newsrc */
  adata->fp_newsrc = mutt_file_fopen(adata->newsrc_file, "r");
  if (!adata->fp_newsrc)
  {
    mutt_perror(adata->newsrc_file);
    return -1;
  }

  /* lock it */
  mutt_debug(LL_DEBUG1, "Locking %s\n", adata->newsrc_file);
  if (mutt_file_lock(fileno(adata->fp_newsrc), false, true))
  {
    mutt_file_fclose(&adata->fp_newsrc);
    return -1;
  }

  if (stat(adata->newsrc_file, &sb))
  {
    mutt_perror(adata->newsrc_file);
    nntp_newsrc_close(adata);
    return -1;
  }

  if ((adata->size == sb.st_size) && (adata->mtime == sb.st_mtime))
    return 0;

  adata->size = sb.st_size;
  adata->mtime = sb.st_mtime;
  adata->newsrc_modified = true;
  mutt_debug(LL_DEBUG1, "Parsing %s\n", adata->newsrc_file);

  /* .newsrc has been externally modified or hasn't been loaded yet */
  for (unsigned int i = 0; i < adata->groups_num; i++)
  {
    struct NntpMboxData *mdata = adata->groups_list[i];
    if (!mdata)
      continue;

    mdata->subscribed = false;
    mdata->newsrc_len = 0;
    FREE(&mdata->newsrc_ent);
  }

  line = mutt_mem_malloc(sb.st_size + 1);
  while (sb.st_size && fgets(line, sb.st_size + 1, adata->fp_newsrc))
  {
    char *b = NULL, *h = NULL;
    unsigned int j = 1;
    bool subs = false;

    /* find end of newsgroup name */
    char *p = strpbrk(line, ":!");
    if (!p)
      continue;

    /* ":" - subscribed, "!" - unsubscribed */
    if (*p == ':')
      subs = true;
    *p++ = '\0';

    /* get newsgroup data */
    struct NntpMboxData *mdata = mdata_find(adata, line);
    FREE(&mdata->newsrc_ent);

    /* count number of entries */
    b = p;
    while (*b)
      if (*b++ == ',')
        j++;
    mdata->newsrc_ent = mutt_mem_calloc(j, sizeof(struct NewsrcEntry));
    mdata->subscribed = subs;

    /* parse entries */
    j = 0;
    while (p)
    {
      b = p;

      /* find end of entry */
      p = strchr(p, ',');
      if (p)
        *p++ = '\0';

      /* first-last or single number */
      h = strchr(b, '-');
      if (h)
        *h++ = '\0';
      else
        h = b;

      if ((sscanf(b, ANUM, &mdata->newsrc_ent[j].first) == 1) &&
          (sscanf(h, ANUM, &mdata->newsrc_ent[j].last) == 1))
      {
        j++;
      }
    }
    if (j == 0)
    {
      mdata->newsrc_ent[j].first = 1;
      mdata->newsrc_ent[j].last = 0;
      j++;
    }
    if (mdata->last_message == 0)
      mdata->last_message = mdata->newsrc_ent[j - 1].last;
    mdata->newsrc_len = j;
    mutt_mem_realloc(&mdata->newsrc_ent, j * sizeof(struct NewsrcEntry));
    nntp_group_unread_stat(mdata);
    mutt_debug(LL_DEBUG2, "%s\n", mdata->group);
  }
  FREE(&line);
  return 1;
}

/**
 * nntp_newsrc_gen_entries - Generate array of .newsrc entries
 * @param m Mailbox
 */
void nntp_newsrc_gen_entries(struct Mailbox *m)
{
  if (!m)
    return;

  struct NntpMboxData *mdata = m->mdata;
  anum_t last = 0, first = 1;
  bool series;
  enum SortType save_sort = SORT_ORDER;
  unsigned int entries;

  if (C_Sort != SORT_ORDER)
  {
    save_sort = C_Sort;
    C_Sort = SORT_ORDER;
    mailbox_changed(m, MBN_RESORT);
  }

  entries = mdata->newsrc_len;
  if (!entries)
  {
    entries = 5;
    mdata->newsrc_ent = mutt_mem_calloc(entries, sizeof(struct NewsrcEntry));
  }

  /* Set up to fake initial sequence from 1 to the article before the
   * first article in our list */
  mdata->newsrc_len = 0;
  series = true;
  for (int i = 0; i < m->msg_count; i++)
  {
    /* search for first unread */
    if (series)
    {
      /* We don't actually check sequential order, since we mark
       * "missing" entries as read/deleted */
      last = nntp_edata_get(m->emails[i])->article_num;
      if ((last >= mdata->first_message) && !m->emails[i]->deleted &&
          !m->emails[i]->read)
      {
        if (mdata->newsrc_len >= entries)
        {
          entries *= 2;
          mutt_mem_realloc(&mdata->newsrc_ent, entries * sizeof(struct NewsrcEntry));
        }
        mdata->newsrc_ent[mdata->newsrc_len].first = first;
        mdata->newsrc_ent[mdata->newsrc_len].last = last - 1;
        mdata->newsrc_len++;
        series = false;
      }
    }

    /* search for first read */
    else
    {
      if (m->emails[i]->deleted || m->emails[i]->read)
      {
        first = last + 1;
        series = true;
      }
      last = nntp_edata_get(m->emails[i])->article_num;
    }
  }

  if (series && (first <= mdata->last_loaded))
  {
    if (mdata->newsrc_len >= entries)
    {
      entries++;
      mutt_mem_realloc(&mdata->newsrc_ent, entries * sizeof(struct NewsrcEntry));
    }
    mdata->newsrc_ent[mdata->newsrc_len].first = first;
    mdata->newsrc_ent[mdata->newsrc_len].last = mdata->last_loaded;
    mdata->newsrc_len++;
  }
  mutt_mem_realloc(&mdata->newsrc_ent, mdata->newsrc_len * sizeof(struct NewsrcEntry));

  if (save_sort != C_Sort)
  {
    C_Sort = save_sort;
    mailbox_changed(m, MBN_RESORT);
  }
}

/**
 * update_file - Update file with new contents
 * @param filename File to update
 * @param buf      New context
 * @retval  0 Success
 * @retval -1 Failure
 */
static int update_file(char *filename, char *buf)
{
  FILE *fp = NULL;
  char tmpfile[PATH_MAX];
  int rc = -1;

  while (true)
  {
    snprintf(tmpfile, sizeof(tmpfile), "%s.tmp", filename);
    fp = mutt_file_fopen(tmpfile, "w");
    if (!fp)
    {
      mutt_perror(tmpfile);
      *tmpfile = '\0';
      break;
    }
    if (fputs(buf, fp) == EOF)
    {
      mutt_perror(tmpfile);
      break;
    }
    if (mutt_file_fclose(&fp) == EOF)
    {
      mutt_perror(tmpfile);
      fp = NULL;
      break;
    }
    fp = NULL;
    if (rename(tmpfile, filename) < 0)
    {
      mutt_perror(filename);
      break;
    }
    *tmpfile = '\0';
    rc = 0;
    break;
  }
  mutt_file_fclose(&fp);

  if (*tmpfile)
    unlink(tmpfile);
  return rc;
}

/**
 * nntp_newsrc_update - Update .newsrc file
 * @param adata NNTP server
 * @retval  0 Success
 * @retval -1 Failure
 */
int nntp_newsrc_update(struct NntpAccountData *adata)
{
  if (!adata)
    return -1;

  int rc = -1;

  size_t buflen = 10240;
  char *buf = mutt_mem_calloc(1, buflen);
  size_t off = 0;

  /* we will generate full newsrc here */
  for (unsigned int i = 0; i < adata->groups_num; i++)
  {
    struct NntpMboxData *mdata = adata->groups_list[i];

    if (!mdata || !mdata->newsrc_ent)
      continue;

    /* write newsgroup name */
    if (off + strlen(mdata->group) + 3 > buflen)
    {
      buflen *= 2;
      mutt_mem_realloc(&buf, buflen);
    }
    snprintf(buf + off, buflen - off, "%s%c ", mdata->group, mdata->subscribed ? ':' : '!');
    off += strlen(buf + off);

    /* write entries */
    for (unsigned int j = 0; j < mdata->newsrc_len; j++)
    {
      if (off + 1024 > buflen)
      {
        buflen *= 2;
        mutt_mem_realloc(&buf, buflen);
      }
      if (j)
        buf[off++] = ',';
      if (mdata->newsrc_ent[j].first == mdata->newsrc_ent[j].last)
        snprintf(buf + off, buflen - off, "%u", mdata->newsrc_ent[j].first);
      else if (mdata->newsrc_ent[j].first < mdata->newsrc_ent[j].last)
      {
        snprintf(buf + off, buflen - off, "%u-%u", mdata->newsrc_ent[j].first,
                 mdata->newsrc_ent[j].last);
      }
      off += strlen(buf + off);
    }
    buf[off++] = '\n';
  }
  buf[off] = '\0';

  /* newrc being fully rewritten */
  mutt_debug(LL_DEBUG1, "Updating %s\n", adata->newsrc_file);
  if (adata->newsrc_file && (update_file(adata->newsrc_file, buf) == 0))
  {
    struct stat sb;

    rc = stat(adata->newsrc_file, &sb);
    if (rc == 0)
    {
      adata->size = sb.st_size;
      adata->mtime = sb.st_mtime;
    }
    else
    {
      mutt_perror(adata->newsrc_file);
    }
  }
  FREE(&buf);
  return rc;
}

/**
 * cache_expand - Make fully qualified cache file name
 * @param dst    Buffer for filename
 * @param dstlen Length of buffer
 * @param acct   Account
 * @param src    Path to add to the URL
 */
static void cache_expand(char *dst, size_t dstlen, struct ConnAccount *acct, const char *src)
{
  char *c = NULL;
  char file[PATH_MAX];

  /* server subdirectory */
  if (acct)
  {
    struct Url url;

    mutt_account_tourl(acct, &url);
    url.path = mutt_str_strdup(src);
    url_tostring(&url, file, sizeof(file), U_PATH);
    FREE(&url.path);
  }
  else
    mutt_str_strfcpy(file, src ? src : "", sizeof(file));

  snprintf(dst, dstlen, "%s/%s", C_NewsCacheDir, file);

  /* remove trailing slash */
  c = dst + strlen(dst) - 1;
  if (*c == '/')
    *c = '\0';
  mutt_expand_path(dst, dstlen);
  mutt_encode_path(dst, dstlen, dst);
}

/**
 * nntp_expand_path - Make fully qualified url from newsgroup name
 * @param buf    Buffer for the result
 * @param buflen Length of buffer
 * @param acct Account to serialise
 */
void nntp_expand_path(char *buf, size_t buflen, struct ConnAccount *acct)
{
  struct Url url;

  mutt_account_tourl(acct, &url);
  url.path = mutt_str_strdup(buf);
  url_tostring(&url, buf, buflen, 0);
  FREE(&url.path);
}

/**
 * nntp_add_group - Parse newsgroup
 * @param line String to parse
 * @param data NNTP data
 * @retval 0 Always
 */
int nntp_add_group(char *line, void *data)
{
  struct NntpAccountData *adata = data;
  struct NntpMboxData *mdata = NULL;
  char group[1024] = { 0 };
  char desc[8192] = { 0 };
  char mod;
  anum_t first, last;

  if (!adata || !line)
    return 0;

  /* These sscanf limits must match the sizes of the group and desc arrays */
  if (sscanf(line, "%1023s " ANUM " " ANUM " %c %8191[^\n]", group, &last,
             &first, &mod, desc) < 4)
  {
    mutt_debug(LL_DEBUG2, "Can't parse server line: %s\n", line);
    return 0;
  }

  mdata = mdata_find(adata, group);
  mdata->deleted = false;
  mdata->first_message = first;
  mdata->last_message = last;
  mdata->allowed = (mod == 'y') || (mod == 'm');
  mutt_str_replace(&mdata->desc, desc);
  if (mdata->newsrc_ent || mdata->last_cached)
    nntp_group_unread_stat(mdata);
  else if (mdata->last_message && (mdata->first_message <= mdata->last_message))
    mdata->unread = mdata->last_message - mdata->first_message + 1;
  else
    mdata->unread = 0;
  return 0;
}

/**
 * active_get_cache - Load list of all newsgroups from cache
 * @param adata NNTP server
 * @retval  0 Success
 * @retval -1 Failure
 */
static int active_get_cache(struct NntpAccountData *adata)
{
  char buf[8192];
  char file[4096];
  time_t t;

  cache_expand(file, sizeof(file), &adata->conn->account, ".active");
  mutt_debug(LL_DEBUG1, "Parsing %s\n", file);
  FILE *fp = mutt_file_fopen(file, "r");
  if (!fp)
    return -1;

  if (!fgets(buf, sizeof(buf), fp) || (sscanf(buf, "%ld%4095s", &t, file) != 1) || (t == 0))
  {
    mutt_file_fclose(&fp);
    return -1;
  }
  adata->newgroups_time = t;

  mutt_message(_("Loading list of groups from cache..."));
  while (fgets(buf, sizeof(buf), fp))
    nntp_add_group(buf, adata);
  nntp_add_group(NULL, NULL);
  mutt_file_fclose(&fp);
  mutt_clear_error();
  return 0;
}

/**
 * nntp_active_save_cache - Save list of all newsgroups to cache
 * @param adata NNTP server
 * @retval  0 Success
 * @retval -1 Failure
 */
int nntp_active_save_cache(struct NntpAccountData *adata)
{
  if (!adata->cacheable)
    return 0;

  size_t buflen = 10240;
  char *buf = mutt_mem_calloc(1, buflen);
  snprintf(buf, buflen, "%lu\n", (unsigned long) adata->newgroups_time);
  size_t off = strlen(buf);

  for (unsigned int i = 0; i < adata->groups_num; i++)
  {
    struct NntpMboxData *mdata = adata->groups_list[i];

    if (!mdata || mdata->deleted)
      continue;

    if ((off + strlen(mdata->group) + (mdata->desc ? strlen(mdata->desc) : 0) + 50) > buflen)
    {
      buflen *= 2;
      mutt_mem_realloc(&buf, buflen);
    }
    snprintf(buf + off, buflen - off, "%s %u %u %c%s%s\n", mdata->group,
             mdata->last_message, mdata->first_message, mdata->allowed ? 'y' : 'n',
             mdata->desc ? " " : "", mdata->desc ? mdata->desc : "");
    off += strlen(buf + off);
  }

  char file[PATH_MAX];
  cache_expand(file, sizeof(file), &adata->conn->account, ".active");
  mutt_debug(LL_DEBUG1, "Updating %s\n", file);
  int rc = update_file(file, buf);
  FREE(&buf);
  return rc;
}

#ifdef USE_HCACHE
/**
 * nntp_hcache_namer - Compose hcache file names - Implements ::hcache_namer_t
 */
static int nntp_hcache_namer(const char *path, char *dest, size_t destlen)
{
  int count = snprintf(dest, destlen, "%s.hcache", path);

  /* Strip out any directories in the path */
  char *first = strchr(dest, '/');
  char *last = strrchr(dest, '/');
  if (first && last && (last > first))
  {
    memmove(first, last, strlen(last) + 1);
    count -= (last - first);
  }

  return count;
}

/**
 * nntp_hcache_open - Open newsgroup hcache
 * @param mdata NNTP Mailbox data
 * @retval ptr  Header cache
 * @retval NULL Error
 */
header_cache_t *nntp_hcache_open(struct NntpMboxData *mdata)
{
  struct Url url;
  char file[PATH_MAX];

  if (!mdata->adata || !mdata->adata->cacheable || !mdata->adata->conn ||
      !mdata->group || !(mdata->newsrc_ent || mdata->subscribed || C_SaveUnsubscribed))
  {
    return NULL;
  }

  mutt_account_tourl(&mdata->adata->conn->account, &url);
  url.path = mdata->group;
  url_tostring(&url, file, sizeof(file), U_PATH);
  return mutt_hcache_open(C_NewsCacheDir, file, nntp_hcache_namer);
}

/**
 * nntp_hcache_update - Remove stale cached headers
 * @param mdata NNTP Mailbox data
 * @param hc    Header cache
 */
void nntp_hcache_update(struct NntpMboxData *mdata, header_cache_t *hc)
{
  if (!hc)
    return;

  char buf[32];
  bool old = false;
  anum_t first = 0, last = 0;

  /* fetch previous values of first and last */
  void *hdata = mutt_hcache_fetch_raw(hc, "index", 5);
  if (hdata)
  {
    mutt_debug(LL_DEBUG2, "mutt_hcache_fetch index: %s\n", (char *) hdata);
    if (sscanf(hdata, ANUM " " ANUM, &first, &last) == 2)
    {
      old = true;
      mdata->last_cached = last;

      /* clean removed headers from cache */
      for (anum_t current = first; current <= last; current++)
      {
        if ((current >= mdata->first_message) && (current <= mdata->last_message))
          continue;

        snprintf(buf, sizeof(buf), "%u", current);
        mutt_debug(LL_DEBUG2, "mutt_hcache_delete_header %s\n", buf);
        mutt_hcache_delete_header(hc, buf, strlen(buf));
      }
    }
    mutt_hcache_free(hc, &hdata);
  }

  /* store current values of first and last */
  if (!old || (mdata->first_message != first) || (mdata->last_message != last))
  {
    snprintf(buf, sizeof(buf), "%u %u", mdata->first_message, mdata->last_message);
    mutt_debug(LL_DEBUG2, "mutt_hcache_store index: %s\n", buf);
    mutt_hcache_store_raw(hc, "index", 5, buf, strlen(buf) + 1);
  }
}
#endif

/**
 * nntp_bcache_delete - Remove bcache file - Implements ::bcache_list_t
 * @retval 0 Always
 */
static int nntp_bcache_delete(const char *id, struct BodyCache *bcache, void *data)
{
  struct NntpMboxData *mdata = data;
  anum_t anum;
  char c;

  if (!mdata || (sscanf(id, ANUM "%c", &anum, &c) != 1) ||
      (anum < mdata->first_message) || (anum > mdata->last_message))
  {
    if (mdata)
      mutt_debug(LL_DEBUG2, "mutt_bcache_del %s\n", id);
    mutt_bcache_del(bcache, id);
  }
  return 0;
}

/**
 * nntp_bcache_update - Remove stale cached messages
 * @param mdata NNTP Mailbox data
 */
void nntp_bcache_update(struct NntpMboxData *mdata)
{
  mutt_bcache_list(mdata->bcache, nntp_bcache_delete, mdata);
}

/**
 * nntp_delete_group_cache - Remove hcache and bcache of newsgroup
 * @param mdata NNTP Mailbox data
 */
void nntp_delete_group_cache(struct NntpMboxData *mdata)
{
  if (!mdata || !mdata->adata || !mdata->adata->cacheable)
    return;

#ifdef USE_HCACHE
  char file[PATH_MAX];
  nntp_hcache_namer(mdata->group, file, sizeof(file));
  cache_expand(file, sizeof(file), &mdata->adata->conn->account, file);
  unlink(file);
  mdata->last_cached = 0;
  mutt_debug(LL_DEBUG2, "%s\n", file);
#endif

  if (!mdata->bcache)
  {
    mdata->bcache = mutt_bcache_open(&mdata->adata->conn->account, mdata->group);
  }
  if (mdata->bcache)
  {
    mutt_debug(LL_DEBUG2, "%s/*\n", mdata->group);
    mutt_bcache_list(mdata->bcache, nntp_bcache_delete, NULL);
    mutt_bcache_close(&mdata->bcache);
  }
}

/**
 * nntp_clear_cache - Clear the NNTP cache
 * @param adata NNTP server
 *
 * Remove hcache and bcache of all unexistent and unsubscribed newsgroups
 */
void nntp_clear_cache(struct NntpAccountData *adata)
{
  char file[PATH_MAX];
  char *fp = NULL;
  struct dirent *entry = NULL;
  DIR *dp = NULL;

  if (!adata || !adata->cacheable)
    return;

  cache_expand(file, sizeof(file), &adata->conn->account, NULL);
  dp = opendir(file);
  if (dp)
  {
    mutt_str_strncat(file, sizeof(file), "/", 1);
    fp = file + strlen(file);
    while ((entry = readdir(dp)))
    {
      char *group = entry->d_name;
      struct stat sb;
      struct NntpMboxData *mdata = NULL;
      struct NntpMboxData tmp_mdata;

      if ((mutt_str_strcmp(group, ".") == 0) || (mutt_str_strcmp(group, "..") == 0))
        continue;
      *fp = '\0';
      mutt_str_strncat(file, sizeof(file), group, strlen(group));
      if (stat(file, &sb))
        continue;

#ifdef USE_HCACHE
      if (S_ISREG(sb.st_mode))
      {
        char *ext = group + strlen(group) - 7;
        if ((strlen(group) < 8) || (mutt_str_strcmp(ext, ".hcache") != 0))
          continue;
        *ext = '\0';
      }
      else
#endif
          if (!S_ISDIR(sb.st_mode))
        continue;

      mdata = mutt_hash_find(adata->groups_hash, group);
      if (!mdata)
      {
        mdata = &tmp_mdata;
        mdata->adata = adata;
        mdata->group = group;
        mdata->bcache = NULL;
      }
      else if (mdata->newsrc_ent || mdata->subscribed || C_SaveUnsubscribed)
        continue;

      nntp_delete_group_cache(mdata);
      if (S_ISDIR(sb.st_mode))
      {
        rmdir(file);
        mutt_debug(LL_DEBUG2, "%s\n", file);
      }
    }
    closedir(dp);
  }
}

/**
 * nntp_format_str - Expand the newsrc filename - Implements ::format_t
 *
 * | Expando | Description
 * |:--------|:--------------------------------------------------------
 * | \%a     | Account url
 * | \%p     | Port
 * | \%P     | Port if specified
 * | \%s     | News server name
 * | \%S     | Url schema
 * | \%u     | Username
 */
const char *nntp_format_str(char *buf, size_t buflen, size_t col, int cols, char op,
                            const char *src, const char *prec, const char *if_str,
                            const char *else_str, unsigned long data, MuttFormatFlags flags)
{
  struct NntpAccountData *adata = (struct NntpAccountData *) data;
  struct ConnAccount *acct = &adata->conn->account;
  char fn[128], fmt[128];

  switch (op)
  {
    case 'a':
    {
      struct Url url;
      mutt_account_tourl(acct, &url);
      url_tostring(&url, fn, sizeof(fn), U_PATH);
      char *p = strchr(fn, '/');
      if (p)
        *p = '\0';
      snprintf(fmt, sizeof(fmt), "%%%ss", prec);
      snprintf(buf, buflen, fmt, fn);
      break;
    }
    case 'p':
      snprintf(fmt, sizeof(fmt), "%%%su", prec);
      snprintf(buf, buflen, fmt, acct->port);
      break;
    case 'P':
      *buf = '\0';
      if (acct->flags & MUTT_ACCT_PORT)
      {
        snprintf(fmt, sizeof(fmt), "%%%su", prec);
        snprintf(buf, buflen, fmt, acct->port);
      }
      break;
    case 's':
      mutt_str_strfcpy(fn, acct->host, sizeof(fn));
      mutt_str_strlower(fn);
      snprintf(fmt, sizeof(fmt), "%%%ss", prec);
      snprintf(buf, buflen, fmt, fn);
      break;
    case 'S':
    {
      struct Url url;
      mutt_account_tourl(acct, &url);
      url_tostring(&url, fn, sizeof(fn), U_PATH);
      char *p = strchr(fn, ':');
      if (p)
        *p = '\0';
      snprintf(fmt, sizeof(fmt), "%%%ss", prec);
      snprintf(buf, buflen, fmt, fn);
      break;
    }
    case 'u':
      snprintf(fmt, sizeof(fmt), "%%%ss", prec);
      snprintf(buf, buflen, fmt, acct->user);
      break;
  }
  return src;
}

/**
 * nntp_select_server - Open a connection to an NNTP server
 * @param m          Mailbox
 * @param server     Server URI
 * @param leave_lock Leave the server locked?
 * @retval ptr  NNTP server
 * @retval NULL Error
 *
 * Automatically loads a newsrc into memory, if necessary.  Checks the
 * size/mtime of a newsrc file, if it doesn't match, load again.  Hmm, if a
 * system has broken mtimes, this might mean the file is reloaded every time,
 * which we'd have to fix.
 */
struct NntpAccountData *nntp_select_server(struct Mailbox *m, char *server, bool leave_lock)
{
  char file[PATH_MAX];
  int rc;
  struct ConnAccount acct = { { 0 } };
  struct NntpAccountData *adata = NULL;
  struct Connection *conn = NULL;

  if (!server || !*server)
  {
    mutt_error(_("No news server defined"));
    return NULL;
  }

  /* create account from news server url */
  acct.flags = 0;
  acct.port = NNTP_PORT;
  acct.type = MUTT_ACCT_TYPE_NNTP;
  snprintf(file, sizeof(file), "%s%s", strstr(server, "://") ? "" : "news://", server);
  struct Url *url = url_parse(file);
  if (!url || (url->path && *url->path) ||
      !((url->scheme == U_NNTP) || (url->scheme == U_NNTPS)) || !url->host ||
      (mutt_account_fromurl(&acct, url) < 0))
  {
    url_free(&url);
    mutt_error(_("%s is an invalid news server specification"), server);
    return NULL;
  }
  if (url->scheme == U_NNTPS)
  {
    acct.flags |= MUTT_ACCT_SSL;
    acct.port = NNTP_SSL_PORT;
  }
  url_free(&url);

  /* find connection by account */
  conn = mutt_conn_find(NULL, &acct);
  if (!conn)
    return NULL;
  if (!(conn->account.flags & MUTT_ACCT_USER) && acct.flags & MUTT_ACCT_USER)
  {
    conn->account.flags |= MUTT_ACCT_USER;
    conn->account.user[0] = '\0';
  }

  /* news server already exists */
  // adata = conn->data;
  if (adata)
  {
    if (adata->status == NNTP_BYE)
      adata->status = NNTP_NONE;
    if (nntp_open_connection(adata) < 0)
      return NULL;

    rc = nntp_newsrc_parse(adata);
    if (rc < 0)
      return NULL;

    /* check for new newsgroups */
    if (!leave_lock && (nntp_check_new_groups(m, adata) < 0))
      rc = -1;

    /* .newsrc has been externally modified */
    if (rc > 0)
      nntp_clear_cache(adata);
    if ((rc < 0) || !leave_lock)
      nntp_newsrc_close(adata);
    return (rc < 0) ? NULL : adata;
  }

  /* new news server */
  adata = nntp_adata_new(conn);

  rc = nntp_open_connection(adata);

  /* try to create cache directory and enable caching */
  adata->cacheable = false;
  if ((rc >= 0) && C_NewsCacheDir)
  {
    cache_expand(file, sizeof(file), &conn->account, NULL);
    if (mutt_file_mkdir(file, S_IRWXU) < 0)
    {
      mutt_error(_("Can't create %s: %s"), file, strerror(errno));
    }
    adata->cacheable = true;
  }

  /* load .newsrc */
  if (rc >= 0)
  {
    mutt_expando_format(file, sizeof(file), 0, MuttIndexWindow->state.cols,
                        NONULL(C_Newsrc), nntp_format_str,
                        (unsigned long) adata, MUTT_FORMAT_NO_FLAGS);
    mutt_expand_path(file, sizeof(file));
    adata->newsrc_file = mutt_str_strdup(file);
    rc = nntp_newsrc_parse(adata);
  }
  if (rc >= 0)
  {
    /* try to load list of newsgroups from cache */
    if (adata->cacheable && (active_get_cache(adata) == 0))
      rc = nntp_check_new_groups(m, adata);

    /* load list of newsgroups from server */
    else
      rc = nntp_active_fetch(adata, false);
  }

  if (rc >= 0)
    nntp_clear_cache(adata);

#ifdef USE_HCACHE
  /* check cache files */
  if ((rc >= 0) && adata->cacheable)
  {
    struct dirent *entry = NULL;
    DIR *dp = opendir(file);

    if (dp)
    {
      while ((entry = readdir(dp)))
      {
        header_cache_t *hc = NULL;
        void *hdata = NULL;
        char *group = entry->d_name;

        char *p = group + strlen(group) - 7;
        if ((strlen(group) < 8) || (strcmp(p, ".hcache") != 0))
          continue;
        *p = '\0';
        struct NntpMboxData *mdata = mutt_hash_find(adata->groups_hash, group);
        if (!mdata)
          continue;

        hc = nntp_hcache_open(mdata);
        if (!hc)
          continue;

        /* fetch previous values of first and last */
        hdata = mutt_hcache_fetch_raw(hc, "index", 5);
        if (hdata)
        {
          anum_t first, last;

          if (sscanf(hdata, ANUM " " ANUM, &first, &last) == 2)
          {
            if (mdata->deleted)
            {
              mdata->first_message = first;
              mdata->last_message = last;
            }
            if ((last >= mdata->first_message) && (last <= mdata->last_message))
            {
              mdata->last_cached = last;
              mutt_debug(LL_DEBUG2, "%s last_cached=%u\n", mdata->group, last);
            }
          }
          mutt_hcache_free(hc, &hdata);
        }
        mutt_hcache_close(hc);
      }
      closedir(dp);
    }
  }
#endif

  if ((rc < 0) || !leave_lock)
    nntp_newsrc_close(adata);

  if (rc < 0)
  {
    mutt_hash_free(&adata->groups_hash);
    FREE(&adata->groups_list);
    FREE(&adata->newsrc_file);
    FREE(&adata->authenticators);
    FREE(&adata);
    mutt_socket_close(conn);
    FREE(&conn);
    return NULL;
  }

  return adata;
}

/**
 * nntp_article_status - Get status of articles from .newsrc
 * @param m       Mailbox
 * @param e       Email
 * @param group   Newsgroup
 * @param anum    Article number
 *
 * Full status flags are not supported by nntp, but we can fake some of them:
 * Read = a read message number is in the .newsrc
 * New = not read and not cached
 * Old = not read but cached
 */
void nntp_article_status(struct Mailbox *m, struct Email *e, char *group, anum_t anum)
{
  struct NntpMboxData *mdata = m->mdata;

  if (group)
    mdata = mutt_hash_find(mdata->adata->groups_hash, group);

  if (!mdata)
    return;

  for (unsigned int i = 0; i < mdata->newsrc_len; i++)
  {
    if ((anum >= mdata->newsrc_ent[i].first) && (anum <= mdata->newsrc_ent[i].last))
    {
      /* can't use mutt_set_flag() because ctx_update() didn't get called yet */
      e->read = true;
      return;
    }
  }

  /* article was not cached yet, it's new */
  if (anum > mdata->last_cached)
    return;

  /* article isn't read but cached, it's old */
  if (C_MarkOld)
    e->old = true;
}

/**
 * mutt_newsgroup_subscribe - Subscribe newsgroup
 * @param adata NNTP server
 * @param group Newsgroup
 * @retval ptr  NNTP data
 * @retval NULL Error
 */
struct NntpMboxData *mutt_newsgroup_subscribe(struct NntpAccountData *adata, char *group)
{
  if (!adata || !adata->groups_hash || !group || !*group)
    return NULL;

  struct NntpMboxData *mdata = mdata_find(adata, group);
  mdata->subscribed = true;
  if (!mdata->newsrc_ent)
  {
    mdata->newsrc_ent = mutt_mem_calloc(1, sizeof(struct NewsrcEntry));
    mdata->newsrc_len = 1;
    mdata->newsrc_ent[0].first = 1;
    mdata->newsrc_ent[0].last = 0;
  }
  return mdata;
}

/**
 * mutt_newsgroup_unsubscribe - Unsubscribe newsgroup
 * @param adata NNTP server
 * @param group Newsgroup
 * @retval ptr  NNTP data
 * @retval NULL Error
 */
struct NntpMboxData *mutt_newsgroup_unsubscribe(struct NntpAccountData *adata, char *group)
{
  if (!adata || !adata->groups_hash || !group || !*group)
    return NULL;

  struct NntpMboxData *mdata = mutt_hash_find(adata->groups_hash, group);
  if (!mdata)
    return NULL;

  mdata->subscribed = false;
  if (!C_SaveUnsubscribed)
  {
    mdata->newsrc_len = 0;
    FREE(&mdata->newsrc_ent);
  }
  return mdata;
}

/**
 * mutt_newsgroup_catchup - Catchup newsgroup
 * @param m     Mailbox
 * @param adata NNTP server
 * @param group Newsgroup
 * @retval ptr  NNTP data
 * @retval NULL Error
 */
struct NntpMboxData *mutt_newsgroup_catchup(struct Mailbox *m,
                                            struct NntpAccountData *adata, char *group)
{
  if (!adata || !adata->groups_hash || !group || !*group)
    return NULL;

  struct NntpMboxData *mdata = mutt_hash_find(adata->groups_hash, group);
  if (!mdata)
    return NULL;

  if (mdata->newsrc_ent)
  {
    mutt_mem_realloc(&mdata->newsrc_ent, sizeof(struct NewsrcEntry));
    mdata->newsrc_len = 1;
    mdata->newsrc_ent[0].first = 1;
    mdata->newsrc_ent[0].last = mdata->last_message;
  }
  mdata->unread = 0;
  if (m && (m->mdata == mdata))
  {
    for (unsigned int i = 0; i < m->msg_count; i++)
      mutt_set_flag(m, m->emails[i], MUTT_READ, true);
  }
  return mdata;
}

/**
 * mutt_newsgroup_uncatchup - Uncatchup newsgroup
 * @param m     Mailbox
 * @param adata NNTP server
 * @param group Newsgroup
 * @retval ptr  NNTP data
 * @retval NULL Error
 */
struct NntpMboxData *mutt_newsgroup_uncatchup(struct Mailbox *m,
                                              struct NntpAccountData *adata, char *group)
{
  if (!adata || !adata->groups_hash || !group || !*group)
    return NULL;

  struct NntpMboxData *mdata = mutt_hash_find(adata->groups_hash, group);
  if (!mdata)
    return NULL;

  if (mdata->newsrc_ent)
  {
    mutt_mem_realloc(&mdata->newsrc_ent, sizeof(struct NewsrcEntry));
    mdata->newsrc_len = 1;
    mdata->newsrc_ent[0].first = 1;
    mdata->newsrc_ent[0].last = mdata->first_message - 1;
  }
  if (m && (m->mdata == mdata))
  {
    mdata->unread = m->msg_count;
    for (unsigned int i = 0; i < m->msg_count; i++)
      mutt_set_flag(m, m->emails[i], MUTT_READ, false);
  }
  else
  {
    mdata->unread = mdata->last_message;
    if (mdata->newsrc_ent)
      mdata->unread -= mdata->newsrc_ent[0].last;
  }
  return mdata;
}

/**
 * nntp_mailbox - Get first newsgroup with new messages
 * @param m       Mailbox
 * @param buf     Buffer for result
 * @param buflen  Length of buffer
 */
void nntp_mailbox(struct Mailbox *m, char *buf, size_t buflen)
{
  if (!m)
    return;

  for (unsigned int i = 0; i < CurrentNewsSrv->groups_num; i++)
  {
    struct NntpMboxData *mdata = CurrentNewsSrv->groups_list[i];

    if (!mdata || !mdata->subscribed || !mdata->unread)
      continue;

    if ((m->magic == MUTT_NNTP) &&
        (mutt_str_strcmp(mdata->group, ((struct NntpMboxData *) m->mdata)->group) == 0))
    {
      unsigned int unread = 0;

      for (unsigned int j = 0; j < m->msg_count; j++)
        if (!m->emails[j]->read && !m->emails[j]->deleted)
          unread++;
      if (unread == 0)
        continue;
    }
    mutt_str_strfcpy(buf, mdata->group, buflen);
    break;
  }
}
