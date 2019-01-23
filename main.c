/* fuse-migratefs: Migration Filesystem in Userspace

   Copyright (C) 2018-2019 Stephane Thiell <sthiell@stanford.edu>

   Forked from fuse-overlayfs: Overlay Filesystem in Userspace

   Copyright (C) 2018 Giuseppe Scrivano <giuseppe@scrivano.org>
   Copyright (C) 2018-2019 Red Hat Inc.
   Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#define _GNU_SOURCE
#define FUSE_USE_VERSION 32
#define _FILE_OFFSET_BITS 64
#define ENABLE_IOCTL 0

#define DEBUG 1

#include <config.h>

#include <fuse.h>
#include <fuse_lowlevel.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>
#include <dirent.h>
#include <assert.h>
#include <errno.h>
#include <err.h>
#include <error.h>
#include <inttypes.h>
#include <fcntl.h>
#include <hash.h>
#include <sys/statvfs.h>
#include <sys/file.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>

#include <sys/xattr.h>

#include <linux/fs.h>

#include <sys/time.h>
#include <sys/resource.h>

#define ATTR_TIMEOUT 1
#define ENTRY_TIMEOUT 1

#define NODE_TO_INODE(x) ((fuse_ino_t) x)

#if defined(__GNUC__) && (__GNUC__ > 4 || __GNUC__ == 4 && __GNUC_MINOR__ >= 6) && !defined __cplusplus
_Static_assert (sizeof (fuse_ino_t) >= sizeof (uintptr_t),
		"fuse_ino_t too small to hold uintptr_t values!");
#else
struct _uintptr_to_must_hold_fuse_ino_t_dummy_struct
{
  unsigned _uintptr_to_must_hold_fuse_ino_t:
    ((sizeof (fuse_ino_t) >= sizeof (uintptr_t)) ? 1 : -1);
};
#endif

#define FUSE_ENTER(req) do { \
        const struct fuse_ctx *ctx = fuse_req_ctx (req); \
        setegid(ctx->gid); \
        seteuid(ctx->uid); \
        umask(ctx->umask); \
    } while (0)

#define FUSE_EXIT do { seteuid(getuid()); \
                       setegid(getgid()); \
                         } while (0)


#define debug_print(fmt, ...) \
            do { if (DEBUG) fprintf(stderr, fmt, __VA_ARGS__); } while (0)


struct ovl_layer
{
  struct ovl_layer *next;
  char *path;
  int fd;
  bool low;
};

struct ovl_node
{
  struct ovl_node *parent;
  Hash_table *children;
  struct ovl_layer *layer, *last_layer;
  char *path;
  char *name;
  int lookups;
  ino_t ino;

  unsigned int present_lowerdir : 1;
  //unsigned int do_unlink : 1;
  //unsigned int do_rmdir : 1;
  unsigned int loaded : 1;
  //unsigned int hidden : 1;
  //unsigned int whiteout : 1;
};

struct ovl_data
{
  struct fuse_session *se;
  int debug;
  char *mountpoint;
  char *lowerdir;
  char *context;
  char *upperdir;
  struct ovl_layer *layers;

  struct ovl_node *root;
};

static const struct fuse_opt ovl_opts[] = {
  {"context=%s",
   offsetof (struct ovl_data, context), 0},
  {"lowerdir=%s",
   offsetof (struct ovl_data, lowerdir), 0},
  {"upperdir=%s",
   offsetof (struct ovl_data, upperdir), 0},
  FUSE_OPT_END
};

/* Kernel definitions.  */

typedef unsigned char u8;
typedef unsigned char uuid_t[16];


static struct ovl_data *
ovl_data (fuse_req_t req)
{
  return (struct ovl_data *) fuse_req_userdata (req);
}

static unsigned long
get_next_wd_counter ()
{
  static unsigned long counter = 1;
  return counter++;
}


/* Useful in a gdb session.  */
void
dump_directory (struct ovl_node *node)
{
  struct ovl_node *it;

  if (node->children == NULL)
    return;

  for (it = hash_get_first (node->children); it; it = hash_get_next (node->children, it))
    printf ("ENTRY: %s (%s)\n", it->name, it->path);
}

static bool
ovl_debug (fuse_req_t req)
{
  return ovl_data (req)->debug != 0;
}

static void
ovl_init (void *userdata, struct fuse_conn_info *conn)
{
  conn->want |= FUSE_CAP_DONT_MASK | FUSE_CAP_SPLICE_READ | FUSE_CAP_SPLICE_MOVE;
  conn->want &= ~FUSE_CAP_PARALLEL_DIROPS;
//  conn->want |= FUSE_IOCTL_UNRESTRICTED;
}

static struct ovl_layer *
get_upper_layer (struct ovl_data *lo)
{
  return lo->layers;
}

static struct ovl_layer *
get_lower_layers (struct ovl_data *lo)
{
  return lo->layers->next;
}

static inline bool
node_dirp (struct ovl_node *n)
{
  return n->children != NULL;
}

static int
node_dirfd (struct ovl_node *n)
{
  return n->layer->fd;
}

static bool
has_prefix (const char *str, const char *pref)
{
  while (1)
    {
      if (*pref == '\0')
        return true;
      if (*str == '\0')
        return false;
      if (*pref != *str)
        return false;
      str++;
      pref++;
    }
  return false;
}

static int
hide_node (struct ovl_data *lo, struct ovl_node *node, bool unlink_src)
{
#if 0
  char dest[PATH_MAX];
  char *newpath;

  asprintf (&newpath, "%lu", get_next_wd_counter ());
  if (newpath == NULL)
    {
      unlink (dest);
      return -1;
    }

  assert (node->layer == get_upper_layer (lo));

  /* Might be leftover from a previous run.  */
  unlinkat (lo->workdir_fd, newpath, 0);
  unlinkat (lo->workdir_fd, newpath, AT_REMOVEDIR);

  if (unlink_src)
    {
      /* If the atomic rename+mknod failed, then fallback into doing it in two steps.  */
      if (syscall (SYS_renameat2, node_dirfd (node), node->path, lo->workdir_fd,
                   newpath, RENAME_WHITEOUT) < 0)
        {
          if (renameat (node_dirfd (node), node->path, lo->workdir_fd, newpath) < 0)
            {
              free (newpath);
              return -1;
            }
          if (node->parent)
            {
              if (create_whiteout (lo, node->parent, node->name, false) < 0)
                return -1;
            }
        }
    }
  else
    {
      if (node_dirp (node))
        {
          if (mkdirat (lo->workdir_fd, newpath, 0700) < 0)
            {
              free (newpath);
              return -1;
            }
        }
      else
        {
          if (linkat (node_dirfd (node), node->path, lo->workdir_fd, newpath, 0) < 0)
            {
              free (newpath);
              return -1;
            }
        }
    }
  node->hidden_dirfd = lo->workdir_fd;
  free (node->path);
  node->path = newpath;
  node->hidden = 1;
  node->parent = NULL;

  if (node_dirp (node))
    node->do_rmdir = 1;
  else
    node->do_unlink = 1;
  return 0;
#endif
    return -1;
}


static int
rpl_stat (fuse_req_t req, struct ovl_node *node, struct stat *st)
{
  int ret;
  struct ovl_data *data = ovl_data (req);

  ret = TEMP_FAILURE_RETRY (fstatat (node_dirfd (node), node->path, st, AT_SYMLINK_NOFOLLOW));
  if (ret < 0)
    return ret;

  st->st_ino = node->ino;
  if (ret == 0 && node_dirp (node))
    {
      struct ovl_node *it;

      st->st_nlink = 2;

      for (it = hash_get_first (node->children); it; it = hash_get_next (node->children, it))
        {
          if (node_dirp (it))
            st->st_nlink++;
        }
    }

  return ret;
}

static void
node_mark_all_free (void *p)
{
  struct ovl_node *it, *n = (struct ovl_node *) p;

  n->lookups = 0;

  if (n->children)
    {
      for (it = hash_get_first (n->children); it; it = hash_get_next (n->children, it))
        node_mark_all_free (it);
    }
}

static void
node_free (void *p)
{
  struct ovl_node *n = (struct ovl_node *) p;
  if (n == NULL)
    return;
  if (n->parent)
    {
      if (hash_lookup (n->parent->children, n) == n)
        hash_delete (n->parent->children, n);
      n->parent = NULL;
    }

  if (n->lookups > 0)
    return;

  if (n->children)
    {
      struct ovl_node *it;

      for (it = hash_get_first (n->children); it; it = hash_get_next (n->children, it))
        it->parent = NULL;

      hash_free (n->children);
      n->children = NULL;
    }

#if 0
  if (n->do_unlink || n->do_rmdir)
    {
      struct ovl_layer *it;

      debug_print ("node_free path=%s do_unlink...\n", n->path);
      for (it = n->layer; it; it = it->next)
        {
            debug_print ("node_free path=%s do_unlink layer %s\n",
                              n->path, it->path);
            int ret = unlinkat (it->fd, n->path, n->do_rmdir ? AT_REMOVEDIR : 0);
            debug_print ("node_free unlinkat ret=%d errno=%d\n", ret, errno);
        }
    }
#endif

  free (n->name);
  free (n->path);
  free (n);
  return;
}

static void
do_forget (fuse_ino_t ino, uint64_t nlookup)
{
  struct ovl_node *n;

  if (ino == FUSE_ROOT_ID)
    return;

  n = (struct ovl_node *) ino;

  //node_mark_all_free(n);
  debug_print ("do_forget: path=%s name=%s lookups=%d nlookup=%d\n",
            n->path, n->name, n->lookups, nlookup);
  //node_free (n);
  n->lookups -= nlookup;
  if (n->lookups <= 0)
    {
      debug_print ("do_forget: calling node_free path=%s name=%s\n",
                n->path, n->name);
      node_free (n);
    }
}

static void
ovl_forget (fuse_req_t req, fuse_ino_t ino, uint64_t nlookup)
{
  FUSE_ENTER(req);

  if (ovl_debug (req))
    debug_print ("ovl_forget(ino=%" PRIu64 ", nlookup=%lu)\n",
	     ino, nlookup);
  do_forget (ino, nlookup);
  fuse_reply_none (req);

  FUSE_EXIT;
}

static size_t
node_hasher (const void *p, size_t s)
{
  struct ovl_node *n = (struct ovl_node *) p;
  return hash_string (n->name, s);
}

static bool
node_compare (const void *n1, const void *n2)
{
  struct ovl_node *node1 = (struct ovl_node *) n1;
  struct ovl_node *node2 = (struct ovl_node *) n2;

  return strcmp (node1->name, node2->name) == 0 ? true : false;
}


static struct ovl_node *
make_ovl_node (const char *path, struct ovl_layer *layer, const char *name, ino_t ino, bool dir_p, struct ovl_node *parent)
{
  struct ovl_node *ret = malloc (sizeof (*ret));
  if (ret == NULL)
    {
      errno = ENOMEM;
      return NULL;
    }

  ret->last_layer = NULL;
  ret->parent = parent;
  ret->lookups = 0;
  //ret->do_unlink = 0;
  //ret->hidden = 0;
  //ret->do_rmdir = 0;
  ret->layer = layer;
  ret->ino = ino;
  ret->present_lowerdir = 0;
  //ret->hidden_dirfd = -1;
  ret->name = strdup (name);
  if (ret->name == NULL)
    {
      free (ret);
      errno = ENOMEM;
      return NULL;
    }

  if (has_prefix (path, "./") && path[2])
    path += 2;

  ret->path = strdup (path);
  if (ret->path == NULL)
    {
      free (ret->name);
      free (ret);
      errno = ENOMEM;
      return NULL;
    }

  if (!dir_p)
    ret->children = NULL;
  else
    {
      ret->children = hash_initialize (10, NULL, node_hasher, node_compare, node_free);
      if (ret->children == NULL)
        {
          free (ret->path);
          free (ret->name);
          free (ret);
          errno = ENOMEM;
          return NULL;
        }
    }

  if (ret->ino == 0)
    {
      struct stat st;
      struct ovl_layer *it;
      char path[PATH_MAX];

      strcpy (path, ret->path);
      for (it = layer; it; it = it->next)
        {
          ssize_t s;
          int fd = TEMP_FAILURE_RETRY (openat (it->fd, path, O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_PATH));
          if (fd < 0)
            continue;

          if (fstat (fd, &st) == 0)
            ret->ino = st.st_ino;

          close (fd);

          if (parent && parent->last_layer == it)
            break;
        }
    }

  return ret;
}

static struct ovl_node *
insert_node (struct ovl_node *parent, struct ovl_node *item, bool replace)
{
  struct ovl_node *old = NULL, *prev_parent = item->parent;
  int ret;

  if (prev_parent)
    {
      if (hash_lookup (prev_parent->children, item) == item)
        hash_delete (prev_parent->children, item);
    }

  if (replace)
    {
      old = hash_delete (parent->children, item);
      if (old)
        node_free (old);
    }

  ret = hash_insert_if_absent (parent->children, item, (const void **) &old);
  if (ret < 0)
    {
      node_free (item);
      errno = ENOMEM;
      return NULL;
    }
  if (ret == 0)
    {
      node_free (item);
      return old;
    }

  item->parent = parent;

  return item;
}

static struct ovl_node *
load_dir (struct ovl_data *lo, struct ovl_node *n, struct ovl_layer *layer, char *path, char *name)
{
  DIR *dp;
  struct dirent *dent;
  struct stat st;
  struct ovl_layer *it, *upper_layer = get_upper_layer (lo);
  struct ovl_node *nit, *next;

  //fprintf (stderr, "load_dir path=%s name=%s\n", path, name);

  if (n)
    {
      //for (nit = hash_get_first (n->children); nit; nit = hash_get_next (n->children, nit))
      //  nit->loaded = 0;
        //node_mark_all_free (it);
      //node_free(n);
      //jhash_clear(n->children);
      //n->children = hash_initialize (10, NULL, node_hasher, node_compare, node_free);
    }
  else
    {
      n = make_ovl_node (path, layer, name, 0, true, NULL);
      if (n == NULL)
        return NULL;
    }

  for (nit = hash_get_first (n->children); nit; nit = hash_get_next (n->children, nit))
    nit->loaded = 0;

  for (it = lo->layers; it; it = it->next)
    {
      int fd = TEMP_FAILURE_RETRY (openat (it->fd, path, O_DIRECTORY));
      if (fd < 0)
        continue;

      dp = fdopendir (fd);
      if (dp == NULL)
        {
          close (fd);
          continue;
        }

      for (;;)
        {
          struct ovl_node key;
          const char *wh;
          struct ovl_node *child = NULL;
          char node_path[PATH_MAX + 1];

          errno = 0;
          dent = readdir (dp);
          if (dent == NULL)
            {
              if (errno)
                {
                  int saved_errno = errno;
                  closedir (dp);
                  errno = saved_errno;
                  return NULL;
                }

              break;
            }

          key.name = dent->d_name;

          if ((strcmp (dent->d_name, ".") == 0) || strcmp (dent->d_name, "..") == 0)
            continue;

          debug_print ("dent->d_name=%s\n", dent->d_name);
          if (TEMP_FAILURE_RETRY (fstatat (fd, dent->d_name, &st, AT_SYMLINK_NOFOLLOW)) < 0)
            {
              debug_print ("fstatat failed errno=%d\n", errno);
              closedir (dp);
              return NULL;
            }

          child = hash_lookup (n->children, &key);
          if (child)
            {
              child->loaded = 1;
              if (it->low)
                child->present_lowerdir = 1;
              continue;
            }

          sprintf (node_path, "%s/%s", n->path, dent->d_name);

          bool dirp = ((st.st_mode & S_IFMT) == S_IFDIR);

          debug_print ("make_ovl_node %s\n", dent->d_name);
          child = make_ovl_node (node_path, it, dent->d_name, 0, dirp, n);
          if (child == NULL)
            {
              errno = ENOMEM;
              closedir (dp);
              return NULL;
            }

          if (insert_node (n, child, false) == NULL)
            {
              errno = ENOMEM;
              return NULL;
            }

          child->loaded = 1;
        }
      closedir (dp);

      if (n->last_layer == it)
        break;
    }

  for (nit = hash_get_first (n->children); nit; nit = next)
    {
      next = hash_get_next (n->children, nit);
      if (!nit->loaded)
        {
          fprintf (stderr, "load_dir hash_delete orphan path=%s name=%s\n", nit->path, nit->name);
          hash_delete(n->children, nit);
        }
    }


  return n;
}

static void
free_layers (struct ovl_layer *layers)
{
  if (layers == NULL)
    return;
  free_layers (layers->next);
  free (layers->path);
  if (layers->fd >= 0)
    close (layers->fd);
  free (layers);
}

static struct ovl_layer *
read_dirs (char *path, bool low, struct ovl_layer *layers)
{
  char *buf = NULL, *saveptr = NULL, *it;
  struct ovl_layer *last;

  if (path == NULL)
    return NULL;

  buf = strdup (path);
  if (buf == NULL)
    return NULL;

  last = layers;
  while (last && last->next)
    last = last->next;

  for (it = strtok_r (path, ":", &saveptr); it; it = strtok_r (NULL, ":", &saveptr))
    {
      char full_path[PATH_MAX + 1];
      struct ovl_layer *l = NULL;

      if (realpath (it, full_path) < 0)
        return NULL;

      l = malloc (sizeof (*l));
      if (l == NULL)
        {
          free_layers (layers);
          return NULL;
        }

      l->path = strdup (full_path);
      if (l->path == NULL)
        {
          free (l);
          free_layers (layers);
          return NULL;
        }

      l->fd = open (l->path, O_DIRECTORY);
      if (l->fd < 0)
        {
          free (l->path);
          free (l);
          free_layers (layers);
          return NULL;
        }

      l->low = low;
      if (low)
        {
          l->next = NULL;
          if (last == NULL)
            last = layers = l;
          else
            {
              last->next = l;
              last = l;
            }
        }
      else
        {
          l->next = layers;
          layers = l;
        }
    }
  free (buf);
  return layers;
}

static struct ovl_node *
do_lookup_file (struct ovl_data *lo, fuse_ino_t parent, const char *name)
{
  struct ovl_node key;
  struct ovl_node *node, *pnode;

  if (parent == FUSE_ROOT_ID)
    pnode = lo->root;
  else
    pnode = (struct ovl_node *) parent;

  if (name == NULL)
    return pnode;


  key.name = (char *) name;
  node = hash_lookup (pnode->children, &key);
  if (node == NULL)
    {
      int ret;
      char path[PATH_MAX];
      struct ovl_layer *it;
      struct stat st;
      struct ovl_layer *upper_layer = get_upper_layer (lo);

      for (it = lo->layers; it; it = it->next)
        {
          const char *wh_name;

          sprintf (path, "%s/%s", pnode->path, name);
          ret = TEMP_FAILURE_RETRY (fstatat (it->fd, path, &st, AT_SYMLINK_NOFOLLOW));
          if (ret < 0)
            {
              int saved_errno = errno;

              if (errno == ENOENT)
                continue;

              if (node)
                node_free (node);

              errno = saved_errno;
              return NULL;
            }

          /* If we already know the node, simply update the ino.  */
          if (node) {
            node->ino = st.st_ino;
            continue;
            }

          debug_print ("lookup make_ovl_node %s %s\n", path, name);
          node = make_ovl_node (path, it, name, 0, st.st_mode & S_IFDIR, pnode);
          if (node == NULL)
            {
              errno = ENOMEM;
              return NULL;
            }

          if (insert_node (pnode, node, false) == NULL)
            {
              node_free (node);
              errno = ENOMEM;
              return NULL;
            }
          if (node->last_layer)
            break;
          if (pnode && pnode->last_layer == it)
            break;
        }
    }

  if (node == NULL)
    {
      errno = ENOENT;
      return NULL;
    }
  return node;
}

static void
ovl_lookup (fuse_req_t req, fuse_ino_t parent, const char *name)
{
  FUSE_ENTER(req);

  struct fuse_entry_param e;
  int err = 0;
  struct ovl_data *lo = ovl_data (req);
  struct ovl_node *node;

  if (ovl_debug (req))
    fprintf (stderr, "ovl_lookup(parent=%" PRIu64 ", name=%s)\n",
	     parent, name);

  memset (&e, 0, sizeof (e));

  node = do_lookup_file (lo, parent, name);
  if (node == NULL)
    {
      fuse_reply_err (req, errno);
      FUSE_EXIT;
      return;
    }

  err = rpl_stat (req, node, &e.attr);
  if (err)
    {
      fuse_reply_err (req, errno);
      FUSE_EXIT;
      return;
    }

  e.ino = NODE_TO_INODE (node);
  node->lookups++;
  debug_print ("ovl_lookup: inc node->lookups=%d\n", node->lookups);
  e.attr_timeout = ATTR_TIMEOUT;
  e.entry_timeout = ENTRY_TIMEOUT;
  fuse_reply_entry (req, &e);

  FUSE_EXIT;
}

struct ovl_dirp
{
  struct ovl_data *lo;
  struct ovl_node **tbl;
  size_t tbl_size;
  size_t offset;
};

static struct ovl_dirp *
ovl_dirp (struct fuse_file_info *fi)
{
  return (struct ovl_dirp *) (uintptr_t) fi->fh;
}

static void
ovl_opendir (fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
  FUSE_ENTER(req);

  debug_print ("ovl_opendir %llu\n", ino);

  size_t counter = 0;
  struct ovl_node *node;
  struct ovl_data *lo = ovl_data (req);
  struct ovl_node *it;
  struct ovl_dirp *d = calloc (1, sizeof (struct ovl_dirp));

  if (d == NULL)
    {
      errno = ENOENT;
      goto out_errno;
    }

  node = do_lookup_file (lo, ino, NULL);
  if (node == NULL)
    {
      errno = ENOENT;
      goto out_errno;
    }

  if (! node_dirp (node))
    {
      errno = ENOTDIR;
      goto out_errno;
    }

  node = load_dir (lo, node, node->layer, node->path, node->name);
  if (node == NULL)
    goto out_errno;

  d->offset = 0;
  d->tbl_size = hash_get_n_entries (node->children) + 2;
  d->tbl = malloc (sizeof (struct ovl_node *) * d->tbl_size);
  if (d->tbl == NULL)
    {
      errno = ENOMEM;
      goto out_errno;
    }

  d->tbl[counter++] = node;
  d->tbl[counter++] = node->parent;

  for (it = hash_get_first (node->children); it; it = hash_get_next (node->children, it))
    {
      it->lookups++;
      debug_print ("opendir: inc lookups child %s lookups=%d\n", it->name, it->lookups);
      d->tbl[counter++] = it;
    }

  fi->fh = (uintptr_t) d;

  fuse_reply_open (req, fi);
  FUSE_EXIT;
  return;

out_errno:
  debug_print ("ovl_opendir out_errno %d\n", errno);
  if (d)
    {
      if (d->tbl)
        free (d->tbl);
      free (d);
    }
  fuse_reply_err (req, errno);
  FUSE_EXIT;
}

static void
ovl_do_readdir (fuse_req_t req, fuse_ino_t ino, size_t size,
	       off_t offset, struct fuse_file_info *fi, int plus)
{
  struct ovl_dirp *d = ovl_dirp (fi);
  size_t remaining = size;
  char *p, *buffer = calloc (size, 1);

  if (ovl_debug (req))
    fprintf (stderr, "ovl_do_readdir(ino=%" PRIu64 ")\n", ino);

  if (buffer == NULL)
    {
      fuse_reply_err (req, ENOMEM);
      return;
    }
  p = buffer;
  for (; remaining > 0 && offset < d->tbl_size; offset++)
      {
        int ret;
        size_t entsize;
        struct stat st;
        const char *name;
        struct ovl_node *node = d->tbl[offset];

        if (node == NULL)
          continue;

        ret = rpl_stat (req, node, &st);
        if (ret < 0)
          {
            fuse_reply_err (req, errno);
            goto exit;
          }

        if (offset == 0)
          name = ".";
        else if (offset == 1)
          name = "..";
        else
          name = node->name;

        if (!plus)
          entsize = fuse_add_direntry (req, p, remaining, name, &st, offset + 1);
        else
          {
            struct fuse_entry_param e;

            memset (&e, 0, sizeof (e));
            e.attr_timeout = ATTR_TIMEOUT;
            e.entry_timeout = ENTRY_TIMEOUT;
            e.ino = NODE_TO_INODE (node);
            memcpy (&e.attr, &st, sizeof (st));

            entsize = fuse_add_direntry_plus (req, p, remaining, name, &e, offset + 1);
            if (entsize <= remaining)
              {
                /* First two entries are . and .. */
                if (offset >= 2)
                  {
                  node->lookups++;
                  debug_print ("ovl_do_readdir: inc lookups=%d\n", node->lookups);
                  }
              }
          }

        if (entsize > remaining)
          break;

        p += entsize;
        remaining -= entsize;
      }
  fuse_reply_buf (req, buffer, size - remaining);
 exit:
  free (buffer);
}

static void
ovl_readdir (fuse_req_t req, fuse_ino_t ino, size_t size,
	    off_t offset, struct fuse_file_info *fi)
{
  FUSE_ENTER(req);

  ovl_do_readdir (req, ino, size, offset, fi, 0);

  FUSE_EXIT;
}

static void
ovl_readdirplus (fuse_req_t req, fuse_ino_t ino, size_t size,
		off_t offset, struct fuse_file_info *fi)
{
  FUSE_ENTER(req);

  ovl_do_readdir (req, ino, size, offset, fi, 1);

  FUSE_EXIT;
}

static void
ovl_releasedir (fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
  FUSE_ENTER(req);

  size_t s;
  struct ovl_dirp *d = ovl_dirp (fi);

  if (ovl_debug (req))
    fprintf (stderr, "ovl_releasedir(ino=%" PRIu64 ")\n", ino);

  for (s = 2; s < d->tbl_size; s++)
    {
      struct ovl_node *n = d->tbl[s];
      do_forget (NODE_TO_INODE (n), 1);
    }

  //do_forget (ino, 1);

  free (d->tbl);
  free (d);
  fuse_reply_err (req, 0);

  FUSE_EXIT;
}

static void
ovl_listxattr (fuse_req_t req, fuse_ino_t ino, size_t size)
{
  FUSE_ENTER(req);

  ssize_t len;
  struct ovl_node *node;
  struct ovl_data *lo = ovl_data (req);
  char path[PATH_MAX + 10];
  char *buf = NULL;

  if (ovl_debug (req))
    fprintf (stderr, "ovl_listxattr(ino=%" PRIu64 ", size=%zu)\n", ino, size);

  node = do_lookup_file (lo, ino, NULL);
  if (node == NULL)
    {
      fuse_reply_err (req, ENOENT);
      FUSE_EXIT;
      return;
    }

  if (size > 0)
    {
      buf = malloc (size);
      if (buf == NULL)
        {
          fuse_reply_err (req, ENOMEM);
          FUSE_EXIT;
          return;
        }
    }

  sprintf (path, "%s/%s", node->layer->path, node->path);
  len = TEMP_FAILURE_RETRY (listxattr (path, buf, size));
  if (len < 0)
    fuse_reply_err (req, errno);
  else if (size == 0)
    fuse_reply_xattr (req, len);
  else if (len <= size)
    fuse_reply_buf (req, buf, len);

  free (buf);

  FUSE_EXIT;
}

static void
ovl_getxattr (fuse_req_t req, fuse_ino_t ino, const char *name, size_t size)
{
  FUSE_ENTER(req);

  ssize_t len;
  struct ovl_node *node;
  struct ovl_data *lo = ovl_data (req);
  char *buf = NULL;
  char path[PATH_MAX + 10];

  if (ovl_debug (req))
    fprintf (stderr, "ovl_getxattr(ino=%" PRIu64 ", name=%s, size=%zu)\n", ino, name, size);

  node = do_lookup_file (lo, ino, NULL);
  if (node == NULL)
    {
      fuse_reply_err (req, ENOENT);
      FUSE_EXIT;
      return;
    }

  if (size > 0)
    {
      buf = malloc (size);
      if (buf == NULL)
        {
          fuse_reply_err (req, ENOMEM);
          FUSE_EXIT;
          return;
        }
    }

  sprintf (path, "%s/%s", node->layer->path, node->path);
  len = TEMP_FAILURE_RETRY (getxattr (path, name, buf, size));
  if (len < 0)
    fuse_reply_err (req, errno);
  else if (size == 0)
    fuse_reply_xattr (req, len);
  else if (len <= size)
    fuse_reply_buf (req, buf, len);

  free (buf);

  FUSE_EXIT;
}

static void
ovl_access (fuse_req_t req, fuse_ino_t ino, int mask)
{
  FUSE_ENTER(req);

  int ret;
  struct ovl_data *lo = ovl_data (req);
  struct ovl_node *n = do_lookup_file (lo, ino, NULL);

  if (ovl_debug (req))
    fprintf (stderr, "ovl_access(ino=%" PRIu64 ", mask=%d)\n",
	     ino, mask);

  ret = faccessat (node_dirfd (n), n->path, mask, AT_SYMLINK_NOFOLLOW);
  fuse_reply_err (req, ret < 0 ? errno : 0);
}

static int
copy_xattr (int sfd, int dfd, char *buf, size_t buf_size)
{
  size_t xattr_len;

  xattr_len = flistxattr (sfd, buf, buf_size / 2);
  if (xattr_len > 0)
    {
      char *it;
      char *xattr_buf = buf + buf_size / 2;
      for (it = buf; it - buf < xattr_len; it += strlen (it) + 1)
        {
          ssize_t s = fgetxattr (sfd, it, xattr_buf, buf_size / 2);
          if (s < 0)
            return -1;

          if (fsetxattr (dfd, it, xattr_buf, s, 0) < 0)
            return -1;
        }
    }
  return 0;
}

static int create_node_directory (struct ovl_data *lo, struct ovl_node *src);

static int
create_lower_directory (struct ovl_data *lo, int dirfd, struct ovl_node *node)
{
  int parentfd;
  int saved_errno;
  int ret = 0;

  if (!node->parent)
      return 0;

  for (;;)
    {
      ret = parentfd = TEMP_FAILURE_RETRY (openat (dirfd, node->parent->path, O_DIRECTORY));
      debug_print ("create_lower_directory parentfd=%d errno=%d\n", parentfd, errno);
      if (parentfd < 0)
        {
          if (errno == ENOENT)
            {
              debug_print ("create_lower_directory: REC on parent %s\n", node->parent->path);
              ret = create_lower_directory (lo, dirfd, node->parent);
              if (ret != 0)
                 goto out;
              continue;
            }
          else
            goto out;
        }
        break;
    }

  ret = mkdirat (parentfd, node->name, 0777);
  debug_print ("create_lower_directory mkdirat=%d errno=%d\n", ret, errno);
  if (ret < 0 && errno == EEXIST)
    {
      ret = 0;
      errno = 0;
    }

out:
  saved_errno = errno;
  if (parentfd >= 0)
    close (parentfd);
  errno = saved_errno;

  debug_print ("create_lower_directory ret=%d\n", ret);
  return ret;
}

static int
create_directory (struct ovl_data *lo, int dirfd, const char *name, const struct timespec *times,
                  struct ovl_node *parent, int xattr_sfd, mode_t mode)
{
  int ret;
  int dfd = -1;
  int parentfd;
  char *buf = NULL;
  char wd_tmp_file_name[64];

  debug_print ("create_directory name=%s parent->path=%s\n", name, parent->path);

  sprintf (wd_tmp_file_name, ".overlay-tmpdir-%lu", get_next_wd_counter ());

  // recursive creation

  for (;;)
    {
      errno=0;
      parentfd = TEMP_FAILURE_RETRY (openat (dirfd, parent->path, O_DIRECTORY));
      debug_print ("create_directory parentfd=%d errno=%d\n", parentfd, errno);
      if (parentfd < 0)
        {
          if (errno == ENOENT)
            {
              debug_print ("create_directory: name=%s REC calling create_node_directory on parent %s\n",
                 name, parent->path);
              ret = create_node_directory (lo, parent);
              if (ret != 0)
                 goto out;
              continue;
            }
          else
            goto out;
        }
        break;
     }

  ret = mkdirat (parentfd, wd_tmp_file_name, mode);
  debug_print ("create_directory mkdirat=%d errno=%d\n", ret, errno);
  if (ret < 0)
    goto out;

  ret = dfd = TEMP_FAILURE_RETRY (openat (parentfd, wd_tmp_file_name, O_RDONLY));
  debug_print ("create_directory openat wd_tmp fd=%d errno=%d\n", ret, errno);
  if (ret < 0)
    goto out;

  if (times)
    {
      ret = futimens (dfd, times);
      debug_print ("create_directory futimens ret=%d errno=%d\n", ret, errno);
      if (ret < 0)
        goto out;
    }

  if (ret == 0 && xattr_sfd >= 0)
    {
      const size_t buf_size = 1 << 20;
      char *buf = malloc (buf_size);
      if (buf == NULL)
        {
          ret = -1;
          goto out;
        }

      ret = copy_xattr (xattr_sfd, dfd, buf, buf_size);
      if (ret < 0)
        goto out;
    }

  unlinkat (parentfd, name, 0);
  errno = 0;

  ret = renameat (parentfd, wd_tmp_file_name, dirfd, name);
out:
  if (dfd >= 0)
    close (dfd);
  if (buf)
    free (buf);

  if (ret < 0)
      unlinkat (parentfd, wd_tmp_file_name, AT_REMOVEDIR);

  if (parentfd >= 0)
    close (parentfd);

  debug_print ("create_directory ret=%d\n", ret);
  return ret;
}

static int
create_node_directory (struct ovl_data *lo, struct ovl_node *src)
{
  int ret;
  struct stat st;
  int sfd = -1;
  struct timespec times[2];

  if (src == NULL)
    return 0;

  if (src->layer == get_upper_layer (lo))
    return 0;

  ret = sfd = TEMP_FAILURE_RETRY (openat (node_dirfd (src), src->path, O_RDONLY|O_NONBLOCK));
  if (ret < 0)
    return ret;

  ret = TEMP_FAILURE_RETRY (fstat (sfd, &st));
  if (ret < 0)
    return ret;

  times[0] = st.st_atim;
  times[1] = st.st_mtim;

  ret = create_directory (lo, get_upper_layer (lo)->fd, src->path, times, src->parent, sfd, st.st_mode);
  debug_print ("create_node_directory: create_directory %s ret=%d errno=%d\n", src->path, ret, errno);

  close (sfd);

  if (ret == 0)
      src->layer = get_upper_layer (lo);

#if 0
  if (ret == 0)
    {
      src->layer = get_upper_layer (lo);

      if (src->parent)
        delete_whiteout (lo, -1, src->parent, src->name);
    }
#endif

  return ret;
}

static int
copyup (struct ovl_data *lo, struct ovl_node *node)
{
  int saved_errno;
  int ret = -1;
  int dfd = -1, sfd = -1, parentfd;
  struct stat st;
  const size_t buf_size = 1 << 20;
  char *buf = NULL;
  struct timespec times[2];
  char wd_tmp_file_name[32];

  debug_print ("copyup node->path=%s layer=%s\n", node->path, node->layer->path);

  sprintf (wd_tmp_file_name, "%lu", get_next_wd_counter ());

  ret = TEMP_FAILURE_RETRY (fstatat (node_dirfd (node), node->path, &st, AT_SYMLINK_NOFOLLOW));
  if (ret < 0)
    {
      debug_print ("copyup fstatat ret=%d errno=%d\n", ret, errno);
      return ret;
    }

  if (node->parent)
    {
      debug_print ("copyup creating parent directory %s (layer %s) on upper\n",
                    node->parent->path, node->parent->layer->path);
      //assert (node->parent->layer != get_upper_layer (lo));
      ret = create_node_directory (lo, node->parent);
      if (ret < 0)
        {
          debug_print ("copyup create_node_directory ret=%d errno=%d\n",
                            ret, errno);
          return ret;
        }
    }

  if ((st.st_mode & S_IFMT) == S_IFDIR)
    {
      ret = create_node_directory (lo, node);
      if (ret < 0)
        goto exit;
      goto success;
    }

  if ((st.st_mode & S_IFMT) == S_IFLNK)
    {
      char p[PATH_MAX + 1];
      ret = readlinkat (node_dirfd (node), node->path, p, sizeof (p) - 1);
      if (ret < 0)
        goto exit;
      p[ret] = '\0';
      ret = symlinkat (p, get_upper_layer (lo)->fd, node->path);
      if (ret < 0)
        goto exit;
      goto success;
    }

  sfd = TEMP_FAILURE_RETRY (openat (node_dirfd (node), node->path, O_RDONLY|O_NONBLOCK));
  if (sfd < 0)
    goto exit;

  parentfd = TEMP_FAILURE_RETRY (openat (get_upper_layer (lo)->fd, node->parent->path, O_DIRECTORY));
  if (parentfd < 0)
    {
      debug_print ("copyup parentfd failed parentfd=%d errno=%d node->parent->path=%s\n",
                parentfd, errno, node->parent->path);
      goto exit;
    }

  dfd = TEMP_FAILURE_RETRY (openat (parentfd, wd_tmp_file_name, O_CREAT|O_WRONLY, st.st_mode));
  if (dfd < 0)
    goto exit;

  buf = malloc (buf_size);
  if (buf == NULL)
    goto exit;
  for (;;)
    {
      int written;
      int nread;

      nread = TEMP_FAILURE_RETRY (read (sfd, buf, buf_size));
      if (nread < 0)
        goto exit;

      if (nread == 0)
        break;

      written = 0;
      {
        ret = TEMP_FAILURE_RETRY (write (dfd, buf + written, nread));
        if (ret < 0)
          goto exit;
        written += ret;
        nread -= ret;
      }
      debug_print ("copyup xfer written=%llu\n", written);
      while (nread);
    }

  times[0] = st.st_atim;
  times[1] = st.st_mtim;
  ret = futimens (dfd, times);
  if (ret < 0)
    goto exit;

  ret = copy_xattr (sfd, dfd, buf, buf_size);
  if (ret < 0)
    goto exit;

  /* Finally, move the file to its destination.  */
  ret = renameat (parentfd, wd_tmp_file_name, get_upper_layer (lo)->fd, node->path);
  if (ret < 0)
    goto exit;

/*
  if (node->parent)
    {
      char whpath[PATH_MAX + 10];
      sprintf (whpath, "%s/.wh.%s", node->parent->path, node->name);
      if (unlinkat (get_upper_layer (lo)->fd, whpath, 0) < 0 && errno != ENOENT)
        goto exit;
    }
*/

 success:
  ret = 0;

  node->layer = get_upper_layer (lo);

 exit:
  saved_errno = errno;
  free (buf);
  if (sfd >= 0)
    close (sfd);
  if (dfd >= 0)
    close (dfd);
  if (ret < 0)
    unlinkat (parentfd, wd_tmp_file_name, 0);
  if (parentfd >= 0)
    close (parentfd);
  errno = saved_errno;

  return ret;
}

static struct ovl_node *
get_node_up (struct ovl_data *lo, struct ovl_node *node)
{
  int ret;

  debug_print ("get_node_up node->path=%s, node->layer->path=%s\n",
        node->path, node->layer->path);

  if (node->layer == get_upper_layer (lo))
    return node;

  ret = copyup (lo, node);
  if (ret < 0)
    return NULL;

  assert (node->layer == get_upper_layer (lo));

  return node;
}

static size_t
count_dir_entries (struct ovl_node *node, size_t *whiteouts)
{
  size_t c = 0;
  struct ovl_node *it;

  if (whiteouts)


  for (it = hash_get_first (node->children); it; it = hash_get_next (node->children, it))
    {
#if 0
      if (it->whiteout)
        {
          if (whiteouts)
            (*whiteouts)++;
          continue;
        }
#endif
      if (strcmp (it->name, ".") == 0)
        continue;
      if (strcmp (it->name, "..") == 0)
        continue;
      c++;
    }
  return c;
}

static int
update_paths (struct ovl_node *node)
{
  struct ovl_node *it;

  if (node == NULL)
    return 0;

  if (node->parent)
    {
      free (node->path);
      if (asprintf (&node->path, "%s/%s", node->parent->path, node->name) < 0)
        {
          node->path = NULL;
          return -1;
        }
    }

  if (node->children)
    {
      for (it = hash_get_first (node->children); it; it = hash_get_next (node->children, it))
        {
          if (update_paths (it) < 0)
            return -1;
        }
    }

  return 0;
}

static int
empty_dir (struct ovl_data *lo, struct ovl_node *node)
{
  DIR *dp;
  int fd;
  struct dirent *dent;

  struct ovl_layer *it;
  for (it = lo->layers; it; it = it->next)
    {
      debug_print ("emptydir node %s layer %s\n", node->path, it->path);

      fd = TEMP_FAILURE_RETRY (openat (it->fd, node->path, O_DIRECTORY));
      if (fd < 0)
        {
          if (node->last_layer != it && errno == ENOENT)
            continue;
          return -1;
        }

      dp = fdopendir (fd);
      if (dp == NULL)
        {
          close (fd);
          if (errno == ENOENT)
            continue;
          return -1;
        }

      for (;;)
        {
          errno = 0;
          dent = readdir (dp);
          if (dent == NULL)
            {
              if (errno)
                {
                  int saved_errno = errno;
                  closedir (dp);
                  errno = saved_errno;
                  return -1;
                }

              break;
            }
          if (strcmp (dent->d_name, ".") == 0)
            continue;
          if (strcmp (dent->d_name, "..") == 0)
            continue;
          if (unlinkat (dirfd (dp), dent->d_name, 0) < 0)
            unlinkat (dirfd (dp), dent->d_name, AT_REMOVEDIR);
        }

      closedir (dp);
    }

  return 0;
}

static int
do_node_rm (fuse_req_t req, fuse_ino_t parent, const char *name, bool dirp)
{
  struct ovl_node *node;
  struct ovl_data *lo = ovl_data (req);
  struct ovl_node *pnode;
  int ret = 0;
  struct ovl_node key, *rm;

  node = do_lookup_file (lo, parent, name);
  if (node == NULL)
    return ENOENT;

  debug_print ("do_node_rm node->path=%s\n", node->path);

  if (dirp)
    {
      size_t c;

      /* Re-load the directory.  */
      node = load_dir (lo, node, node->layer, node->path, node->name);
      if (node == NULL)
          return errno;

      c = count_dir_entries (node, NULL);
      if (c)
        return ENOTEMPTY;
    }

  debug_print ("do_node_rm %s path %s layer %s\n", name, node->path,  node->layer->path);

  if (! dirp)
    {
      debug_print ("do_node_rm %s path %s  do_unlink!\n", name, node->path);
      struct ovl_layer *it;
      int r = 0;
      ret = ENOENT;

      for (it = node->layer; it; it = it->next)
        {
            debug_print ("do_node_rm path=%s do_unlink layer %s\n",
                              node->path, it->path);
            errno = 0;
            r = unlinkat (it->fd, node->path, 0);
            debug_print ("do_node_rm unlinkat ret=%d errno=%d\n", r, errno);
            if (r == 0)
              ret = 0;
            else if (ret == ENOENT)
              ret = errno;
        }
        debug_print ("do_node_rm unlinkat ret=%d\n", ret);
    }
  else
    {
      if (empty_dir (lo, node) < 0)
        {
          debug_print ("do_node_rm emptydir %s errno=%d\n", name, errno);
          return errno;
        }

      //node->do_rmdir = 1;
      struct ovl_layer *it;
      int r = 0;
      ret = ENOENT;

      for (it = node->layer; it; it = it->next)
        {
            debug_print ("do_rm path=%s do_unlink layer %s\n",
                              node->path, it->path);
            r = unlinkat (it->fd, node->path, AT_REMOVEDIR);
            debug_print ("do_rm unlinkat ret=%d errno=%d\n", r, errno);
            if (r == 0)
              ret = 0;
            else if (ret == ENOENT)
              ret = errno;
        }
    }
 //   }

    //node_free(node);
    //
  pnode = do_lookup_file (lo, parent, NULL);
  if (pnode == NULL)
    return ENOENT;

  key.name = (char *) name;
  rm = hash_delete (pnode->children, &key);
  if (rm)
    {
/*
      ret = hide_node (lo, rm, true);
      if (ret < 0)
        {
          fuse_reply_err (req, errno);
          return;
        }

*/
      node_free (rm);
    }

  return ret;
}

static void
do_rm (fuse_req_t req, fuse_ino_t parent, const char *name, bool dirp)
{
  fuse_reply_err (req, do_node_rm(req, parent, name, dirp));
}


static void
ovl_unlink (fuse_req_t req, fuse_ino_t parent, const char *name)
{
  FUSE_ENTER(req);

  if (ovl_debug (req))
    fprintf (stderr, "ovl_unlink(parent=%" PRIu64 ", name=%s)\n",
	     parent, name);
  do_rm (req, parent, name, false);

  FUSE_EXIT;
}

static void
ovl_rmdir (fuse_req_t req, fuse_ino_t parent, const char *name)
{
  FUSE_ENTER(req);

  if (ovl_debug (req))
    fprintf (stderr, "ovl_rmdir(parent=%" PRIu64 ", name=%s)\n",
	     parent, name);
  do_rm (req, parent, name, true);

  FUSE_EXIT;
}

static void
ovl_setxattr (fuse_req_t req, fuse_ino_t ino, const char *name,
             const char *value, size_t size, int flags)
{
  FUSE_ENTER(req);

  struct ovl_data *lo = ovl_data (req);
  struct ovl_node *node;
  char path[PATH_MAX + 10];

  if (ovl_debug (req))
    fprintf (stderr, "ovl_setxattr(ino=%" PRIu64 "s, name=%s, value=%s, size=%zu, flags=%d)\n", ino, name,
             value, size, flags);


  node = do_lookup_file (lo, ino, NULL);
  if (node == NULL)
    {
      fuse_reply_err (req, ENOENT);
      goto exit;
    }

  node = get_node_up (lo, node);
  if (node == NULL)
    {
      fuse_reply_err (req, errno);
      goto exit;
    }

  sprintf (path, "%s/%s", node->layer->path, node->path);
  if (TEMP_FAILURE_RETRY( setxattr (path, name, value, size, flags) < 0))
    {
      fuse_reply_err (req, errno);
      goto exit;
    }
  fuse_reply_err (req, 0);
exit:
  FUSE_EXIT;
}

static void
ovl_removexattr (fuse_req_t req, fuse_ino_t ino, const char *name)
{
  FUSE_ENTER(req);

  struct ovl_node *node;
  struct ovl_data *lo = ovl_data (req);
  char path[PATH_MAX + 10];

  if (ovl_debug (req))
    fprintf (stderr, "ovl_removexattr(ino=%" PRIu64 "s, name=%s)\n", ino, name);

  node = do_lookup_file (lo, ino, NULL);
  if (node == NULL)
    {
      fuse_reply_err (req, ENOENT);
      goto exit;
    }

  node = get_node_up (lo, node);
  if (node == NULL)
    {
      fuse_reply_err (req, errno);
      goto exit;
    }

  sprintf (path, "%s/%s", node->layer->path, node->path);
  if (TEMP_FAILURE_RETRY (removexattr (path, name)))
    {
      fuse_reply_err (req, errno);
      goto exit;
    }

  fuse_reply_err (req, 0);
exit:
  FUSE_EXIT;
}

static int
ovl_do_open (fuse_req_t req, fuse_ino_t parent, const char *name, int flags, mode_t mode)
{
  struct ovl_data *lo = ovl_data (req);
  struct ovl_node *n;
  bool readonly = (flags & (O_APPEND | O_RDWR | O_WRONLY | O_CREAT | O_TRUNC)) == 0;
  char path[PATH_MAX + 10];
  int fd;
  const struct fuse_ctx *ctx = fuse_req_ctx (req);

  debug_print ("ovl_do_open %s parent=0x%x readonly=%d\n", name, parent, readonly);

  flags |= O_NOFOLLOW;

  n = do_lookup_file (lo, parent, name);
  if (n && (flags & O_CREAT))
    {
      debug_print ("ovl_do_open: %s EEXIST\n", name);
      errno = EEXIST;
      return -1;
    }

  if (!n)
    {
      struct ovl_node *p;

      if ((flags & O_CREAT) == 0)
        {
          errno = ENOENT;
          return -1;
        }

      p = do_lookup_file (lo, parent, NULL);
      if (p == NULL)
        {
          fprintf(stderr, "ovl_do_open: do_lookup_file errno=%d\n", errno);
          errno = ENOENT;
          return -1;
        }

      debug_print ("ovl_do_open get_node_up p->path=%s\n", p->path);

      p = get_node_up (lo, p);
      if (p == NULL)
        return -1;

      sprintf (path, "%s/%s", p->path, name);

      debug_print ("ovl_do_open %s creating %s on upper layer\n", name, path);
      fd = TEMP_FAILURE_RETRY (openat (get_upper_layer (lo)->fd, path, flags, mode & ~ctx->umask));
      if (fd < 0) {
        debug_print ("ovl_do_open openat failed with errno %d\n", errno);
        return -1;
      }

      debug_print ("ovl_do_open %s new node %s\n", name, path);

      n = make_ovl_node (path, get_upper_layer (lo), name, 0, false, p);
      if (n == NULL)
        {
          errno = ENOMEM;
          close (fd);
          return -1;
        }
      n = insert_node (p, n, true);
      if (n == NULL)
        {
          errno = ENOMEM;
          close (fd);
          return -1;
        }
      return fd;
    }

  /* readonly, we can use both lowerdir and upperdir.  */
  if (readonly)
    {
      return TEMP_FAILURE_RETRY (openat (node_dirfd (n), n->path, flags, mode & ~ctx->umask));
    }
  else
    {
      debug_print ("ovl_do_open read/write calling get_node_up (%s, layer %s\n",
            n->path, n->layer->path);
      n = get_node_up (lo, n);
      if (n == NULL)
        return -1;

      return TEMP_FAILURE_RETRY (openat (node_dirfd (n), n->path, flags, mode & ~ctx->umask));
    }
}

static void
ovl_read (fuse_req_t req, fuse_ino_t ino, size_t size,
	 off_t offset, struct fuse_file_info *fi)
{
  struct fuse_bufvec buf = FUSE_BUFVEC_INIT (size);
  if (ovl_debug (req))
    fprintf (stderr, "ovl_read(ino=%" PRIu64 ", size=%zd, "
	     "off=%lu)\n", ino, size, (unsigned long) offset);
  buf.buf[0].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK;
  buf.buf[0].fd = fi->fh;
  buf.buf[0].pos = offset;
  fuse_reply_data (req, &buf, FUSE_BUF_SPLICE_MOVE);
}

static void
ovl_write_buf (fuse_req_t req, fuse_ino_t ino,
	      struct fuse_bufvec *in_buf, off_t off,
	      struct fuse_file_info *fi)
{
  (void) ino;
  ssize_t res;
  struct fuse_bufvec out_buf = FUSE_BUFVEC_INIT (fuse_buf_size (in_buf));
  out_buf.buf[0].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK;
  out_buf.buf[0].fd = fi->fh;
  out_buf.buf[0].pos = off;

  if (ovl_debug (req))
    fprintf (stderr, "ovl_write_buf(ino=%" PRIu64 ", size=%zd, off=%lu, fd=%d)\n",
	     ino, out_buf.buf[0].size, (unsigned long) off, (int) fi->fh);

  errno = 0;
  res = fuse_buf_copy (&out_buf, in_buf, 0);
  if (res < 0)
    fuse_reply_err (req, errno);
  else
    fuse_reply_write (req, (size_t) res);
}

static void
ovl_release (fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
  (void) ino;
  close (fi->fh);
  fuse_reply_err (req, 0);
}

static int
do_getattr (fuse_req_t req, struct fuse_entry_param *e, struct ovl_node *node)
{
  int err = 0;

  memset (e, 0, sizeof (*e));

  err = rpl_stat (req, node, &e->attr);
  if (err < 0)
    return err;

  e->ino = (fuse_ino_t) node;
  e->attr_timeout = ATTR_TIMEOUT;
  e->entry_timeout = ENTRY_TIMEOUT;

  return 0;
}

static void
ovl_create (fuse_req_t req, fuse_ino_t parent, const char *name,
	   mode_t mode, struct fuse_file_info *fi)
{
  FUSE_ENTER(req);

  int fd;
  struct fuse_entry_param e;
  struct ovl_data *lo = ovl_data (req);
  struct ovl_node *node;

  if (ovl_debug (req))
    fprintf (stderr, "ovl_create(parent=%" PRIu64 ", name=%s)\n",
	     parent, name);

  fi->flags = fi->flags | O_CREAT;

  fd = ovl_do_open (req, parent, name, fi->flags, mode);
  if (fd < 0)
    {
      fuse_reply_err (req, errno);
      FUSE_EXIT;
      return;
    }

  node = do_lookup_file (lo, parent, name);
  if (node == NULL || do_getattr (req, &e, node) < 0)
    {
      close (fd);
      fuse_reply_err (req, errno);
      FUSE_EXIT;
      return;
    }
  fi->fh = fd;

  node->lookups++;
  debug_print ("ovl_create: inc lookups=%d\n", node->lookups);
  fuse_reply_create (req, &e, fi);
  FUSE_EXIT;
}

static void
ovl_open (fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
  FUSE_ENTER(req);

  int fd;

  //if (ovl_debug (req))
  fprintf (stderr, "ovl_open(ino=%" PRIu64 "s)\n", ino);

  fd = ovl_do_open (req, ino, NULL, fi->flags, 0700);
  if (fd < 0)
    {
      fuse_reply_err (req, errno);
      FUSE_EXIT;
      return;
    }
  fi->fh = fd;
  fuse_reply_open (req, fi);
  FUSE_EXIT;
}

static void
ovl_getattr (fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
  FUSE_ENTER(req);

  struct ovl_data *lo = ovl_data (req);
  struct ovl_node *node;
  struct fuse_entry_param e;

  if (ovl_debug (req))
    fprintf (stderr, "ovl_getattr(ino=%" PRIu64 "s)\n", ino);

  node = do_lookup_file (lo, ino, NULL);
  if (node == NULL)
    {
      fuse_reply_err (req, ENOENT);
      FUSE_EXIT;
      return;
    }

  if (do_getattr (req, &e, node) < 0)
    {
      fuse_reply_err (req, errno);
      FUSE_EXIT;
      return;
    }

  fuse_reply_attr (req, &e.attr, ENTRY_TIMEOUT);
  FUSE_EXIT;
}

static void
ovl_setattr (fuse_req_t req, fuse_ino_t ino, struct stat *attr, int to_set, struct fuse_file_info *fi)
{
  FUSE_ENTER(req);

  struct ovl_data *lo = ovl_data (req);
  struct ovl_node *node;
  struct fuse_entry_param e;
  struct stat old_st;
  struct timespec times[2];
  int dirfd;
  int err;

  debug_print ("ovl_setattr(ino=%" PRIu64 "s, to_set=%d)\n", ino, to_set);

  node = do_lookup_file (lo, ino, NULL);
  if (node == NULL)
    {
      debug_print ("ovl_setattr: do_lookup_file failed errno=%d\n", errno);
      fuse_reply_err (req, ENOENT);
      FUSE_EXIT;
      return;
    }

  node = get_node_up (lo, node);
  if (node == NULL)
    {
      fuse_reply_err (req, errno);
      FUSE_EXIT;
      return;
    }

  dirfd = node_dirfd (node);

  if (TEMP_FAILURE_RETRY (fstatat (dirfd, node->path, &old_st, AT_SYMLINK_NOFOLLOW)) < 0)
    {
      debug_print ("ovl_setattr failed with errno=%d\n", errno);
      fuse_reply_err (req, errno);
      FUSE_EXIT;
      return;
    }

  if (to_set & FUSE_SET_ATTR_CTIME)
    {
      fuse_reply_err (req, EPERM);
      FUSE_EXIT;
      return;
    }

  memset (times, 0, sizeof (times));
  times[0].tv_sec = UTIME_OMIT;
  times[1].tv_sec = UTIME_OMIT;
  if (to_set & FUSE_SET_ATTR_ATIME)
    times[0] = attr->st_atim;
  else if (to_set & FUSE_SET_ATTR_ATIME_NOW)
    times[0].tv_sec = UTIME_NOW;

  if (to_set & FUSE_SET_ATTR_MTIME)
    times[1] = attr->st_mtim;
  else if (to_set & FUSE_SET_ATTR_MTIME_NOW)
    times[1].tv_sec = UTIME_NOW;

  if (times[0].tv_sec != UTIME_OMIT || times[1].tv_sec != UTIME_OMIT)
    {
      if ((utimensat (dirfd, node->path, times, AT_SYMLINK_NOFOLLOW) < 0))
        {
          fuse_reply_err (req, errno);
          FUSE_EXIT;
          return;
        }
    }

  if ((to_set & FUSE_SET_ATTR_MODE) && fchmodat (dirfd, node->path, attr->st_mode, 0) < 0)
    {
      debug_print ("ovl_setattr chmodat failed with errno=%d\n", errno);
      fuse_reply_err (req, errno);
      FUSE_EXIT;
      return;
    }

  if ((to_set & (FUSE_SET_ATTR_UID|FUSE_SET_ATTR_UID)))
    {
      uid_t uid=-1, gid=-1;
      if (to_set & FUSE_SET_ATTR_UID)
        uid = attr->st_uid;
      if (to_set & FUSE_SET_ATTR_GID)
        gid = attr->st_gid;

      if (fchownat (dirfd, node->path, uid, gid, AT_SYMLINK_NOFOLLOW) < 0)
        {
          err = errno;
          debug_print ("ovl_setattr fchownat failed with errno=%d\n", err);
          fuse_reply_err (req, err);
          FUSE_EXIT;
          return;
        }
    }

  if ((to_set & FUSE_SET_ATTR_SIZE))
    {
      int fd = TEMP_FAILURE_RETRY (openat (dirfd, node->path, O_WRONLY|O_NONBLOCK));
      if (fd < 0)
        {
          err = errno;
          debug_print ("ovl_setattr FUSE_SET_ATTR_SIZE: openat failed with errno=%d\n", err);
          fuse_reply_err (req, err);
          FUSE_EXIT;
          return;
        }

      if (ftruncate (fd, attr->st_size) < 0)
        {
          err = errno;
          debug_print ("ovl_setattr ftruncate failed with errno=%d\n", err);
          close (fd);
          fuse_reply_err (req, err);
          FUSE_EXIT;
          return;
        }
      close (fd);
    }

  if (do_getattr (req, &e, node) < 0)
    {
      fuse_reply_err (req, errno);
      FUSE_EXIT;
      return;
    }

  fuse_reply_attr (req, &e.attr, ENTRY_TIMEOUT);
  FUSE_EXIT;
}

static void
ovl_link (fuse_req_t req, fuse_ino_t ino, fuse_ino_t newparent, const char *newname)
{
  struct ovl_data *lo = ovl_data (req);
  struct ovl_node *node, *newparentnode, *destnode;
  struct ovl_layer *layer;
  char path[PATH_MAX + 10];
  int ret;
  int destfd;
  int saved_errno;
  struct fuse_entry_param e;

  if (ovl_debug (req))
    fprintf (stderr, "ovl_link(ino=%" PRIu64 "s, newparent=%" PRIu64 "s, newname=%s)\n", ino, newparent, newname);

  node = do_lookup_file (lo, ino, NULL);
  if (node == NULL)
    {
      fuse_reply_err (req, ENOENT);
      return;
    }

  debug_print ("ovl_link node path=%s layer=%s\n",
                node->path, node->layer->path);

  layer = node->layer;
  //pnode = node->parent;

#if 0
  node = get_node_up (lo, node);
  if (node == NULL)
    {
      fuse_reply_err (req, errno);
      return;
    }
#endif

  newparentnode = do_lookup_file (lo, newparent, NULL);
  if (newparentnode == NULL)
    {
      fuse_reply_err (req, ENOENT);
      goto exit;
    }

  debug_print ("ovl_link newparentnode path=%s layer=%s\n",
            newparentnode->path, newparentnode->layer->path);

  if (newparentnode->layer != layer)
    {
      debug_print ("ovl_link newparentnode %s is in different layer\n",
        newparentnode->path);
      if (layer == get_upper_layer (lo))
        {
          // normal copyup when we work in upper
          newparentnode = get_node_up (lo, newparentnode);
          if (newparentnode == NULL)
            goto error;
          debug_print ("ovk_link newparentnode %s is now in layer %s\n",
            newparentnode->path, newparentnode->layer->path);
        }
      else
        {
          debug_print ("ovl_link creating tree in lower layer %s for %s\n",
                       layer->path, newparentnode->path);
          ret = create_lower_directory (lo, layer->fd, newparentnode);
          if (ret < 0)
            {
              debug_print ("create_node_directory: create_lower_directory %s ret=%d errno=%d\n",
                    newparentnode->path, ret, errno);
              goto error;
            }
        }
    }

  ret = TEMP_FAILURE_RETRY (openat (layer->fd, newparentnode->path, O_DIRECTORY));
  if (ret < 0)
    goto error;
  destfd = ret;

  sprintf (path, "%s/%s", newparentnode->path, newname);
  debug_print("ovl_link: node->path=%s\n", node->path);
  debug_print("ovl_link: path=%s\n", path);

  if (linkat (layer->fd, node->path, layer->fd, path, 0) < 0)
    {
      debug_print("ovl_link: linkat failed errno=%d\n", errno);
      goto error;
    }

  node = make_ovl_node (path, layer, newname, node->ino, false, newparentnode);
  if (node == NULL)
    {
      errno = ENOMEM;
      goto error;
    }

  node = insert_node (newparentnode, node, true);
  if (node == NULL)
    {
      errno = ENOMEM;
      goto error;
    }

  memset (&e, 0, sizeof (e));

  ret = rpl_stat (req, node, &e.attr);
  if (ret)
    {
      goto error;
    }

  e.ino = NODE_TO_INODE (node);
  node->lookups++;
  debug_print ("ovl_link: inc lookups=%d\n", node->lookups);
  e.attr_timeout = ATTR_TIMEOUT;
  e.entry_timeout = ENTRY_TIMEOUT;
  fuse_reply_entry (req, &e);

  close (destfd);

  ret = 0;
  goto exit;

error:
  ret = -1;

cleanup:
  saved_errno = errno;
  if (destfd >= 0)
    close (destfd);
  errno = saved_errno;

  fuse_reply_err (req, ret == 0 ? 0 : errno);
exit:
  FUSE_EXIT;

#if 0
  destnode = do_lookup_file (lo, newparent, newname);
  if (destnode && !destnode->whiteout)
    {
      fuse_reply_err (req, EEXIST);
      return;
    }

  newparentnode = get_node_up (lo, newparentnode);
  if (newparentnode == NULL)
    {
      fuse_reply_err (req, errno);
      return;
    }

  if (delete_whiteout (lo, -1, newparentnode, newname) < 0)
    {
      fuse_reply_err (req, errno);
      return;
    }

  sprintf (wd_tmp_file_name, "%lu", get_next_wd_counter ());

  sprintf (path, "%s/%s", newparentnode->path, newname);

  if (linkat (node_dirfd (newparentnode), node->path, lo->workdir_fd, wd_tmp_file_name, 0) < 0)
    {
      fuse_reply_err (req, errno);
      return;
    }

  if (renameat (lo->workdir_fd, wd_tmp_file_name, node_dirfd (newparentnode), path) < 0)
    {
      fuse_reply_err (req, errno);
      return;
    }
  else
    {
      int dfd = TEMP_FAILURE_RETRY (openat (node_dirfd (newparentnode), path, O_WRONLY|O_NONBLOCK));
      if (dfd >= 0)
        {
          bool set = false;
          int sfd = TEMP_FAILURE_RETRY (openat (node_dirfd (node), node->path, O_RDONLY|O_NONBLOCK));
          if (sfd >= 0)
            {
              char origin_path[PATH_MAX + 10];
              size_t s;

              s = fgetxattr (sfd, PRIVILEGED_ORIGIN_XATTR, origin_path, sizeof (origin_path));
              if (s > 0)
                fsetxattr (dfd, PRIVILEGED_ORIGIN_XATTR, origin_path, s, 0);

              s = fgetxattr (sfd, ORIGIN_XATTR, origin_path, sizeof (origin_path));
              if (s > 0)
                set = fsetxattr (dfd, ORIGIN_XATTR, origin_path, s, 0) == 0;

              close (sfd);
            }

          if (! set)
            fsetxattr (dfd, ORIGIN_XATTR, node->path, strlen (node->path), 0);
          close (dfd);
        }
    }

  node = make_ovl_node (path, get_upper_layer (lo), newname, node->ino, false, newparentnode);
  if (node == NULL)
    {
      fuse_reply_err (req, ENOMEM);
      return;
    }

  node = insert_node (newparentnode, node, true);
  if (node == NULL)
    {
      fuse_reply_err (req, ENOMEM);
      return;
    }

  memset (&e, 0, sizeof (e));

  ret = rpl_stat (req, node, &e.attr);
  if (ret)
    {
      fuse_reply_err (req, errno);
      return;
    }

  e.ino = NODE_TO_INODE (node);
  node->lookups++;
  debug_print ("ovl_link: inc lookups=%d\n", node->lookups);
  e.attr_timeout = ATTR_TIMEOUT;
  e.entry_timeout = ENTRY_TIMEOUT;
  fuse_reply_entry (req, &e);
#endif
}

static void
ovl_symlink (fuse_req_t req, const char *link, fuse_ino_t parent, const char *name)
{
  FUSE_ENTER(req);

  struct ovl_data *lo = ovl_data (req);
  struct ovl_node *pnode, *node;
  char path[PATH_MAX + 10];
  int ret;
  struct fuse_entry_param e;

  if (ovl_debug (req))
    fprintf (stderr, "ovl_symlink(link=%s, ino=%" PRIu64 "s, name=%s)\n", link, parent, name);

  pnode = do_lookup_file (lo, parent, NULL);
  if (pnode == NULL)
    {
      fuse_reply_err (req, ENOENT);
      goto exit;
    }

  pnode = get_node_up (lo, pnode);
  if (pnode == NULL)
    {
      fuse_reply_err (req, errno);
      goto exit;
    }

  node = do_lookup_file (lo, parent, name);
  if (node != NULL)
    {
      fuse_reply_err (req, EEXIST);
      goto exit;
    }

#if 0
  if (delete_whiteout (lo, -1, pnode, name) < 0)
    {
      fuse_reply_err (req, errno);
      return;
    }
#endif

  sprintf (path, "%s/%s", pnode->path, name);
  ret = symlinkat (link, get_upper_layer (lo)->fd, path);
  if (ret < 0)
    {
      fuse_reply_err (req, errno);
      goto exit;
    }

  node = make_ovl_node (path, get_upper_layer (lo), name, 0, false, pnode);
  if (node == NULL)
    {
      fuse_reply_err (req, ENOMEM);
      goto exit;
    }

  node = insert_node (pnode, node, true);
  if (node == NULL)
    {
      fuse_reply_err (req, ENOMEM);
      goto exit;
    }

  memset (&e, 0, sizeof (e));

  ret = rpl_stat (req, node, &e.attr);
  if (ret)
    {
      fuse_reply_err (req, errno);
      goto exit;
    }

  e.ino = NODE_TO_INODE (node);
  node->lookups++;
  debug_print ("ovl_symlink: inc lookups=%d\n", node->lookups);
  e.attr_timeout = ATTR_TIMEOUT;
  e.entry_timeout = ENTRY_TIMEOUT;
  fuse_reply_entry (req, &e);

exit:
  FUSE_EXIT;
}

static void
ovl_flock (fuse_req_t req, fuse_ino_t ino,
          struct fuse_file_info *fi, int op)
{
  FUSE_ENTER(req);

  int ret, fd;

  if (ovl_debug (req))
    fprintf (stderr, "ovl_flock(ino=%" PRIu64 "s, op=%d)\n", ino, op);

  fd = fi->fh;

  ret = flock (fd, op);

  fuse_reply_err (req, ret == 0 ? 0 : errno);

  FUSE_EXIT;
}

static void
ovl_rename_exchange (fuse_req_t req, fuse_ino_t parent, const char *name,
                     fuse_ino_t newparent, const char *newname,
                     unsigned int flags)
{
  struct ovl_node *pnode, *node, *destnode, *destpnode;
  struct ovl_data *lo = ovl_data (req);
  int ret;
  int saved_errno;
  int srcfd = -1;
  int destfd = -1;
  struct ovl_node *rm1, *rm2;
  char *tmp;

  node = do_lookup_file (lo, parent, name);
  if (node == NULL)
    {
      fuse_reply_err (req, ENOENT);
      return;
    }

  if (node_dirp (node))
    {
      node = load_dir (lo, node, node->layer, node->path, node->name);
      if (node == NULL)
        {
          fuse_reply_err (req, errno);
          return;
        }

      if (node->layer != get_upper_layer (lo) || node->present_lowerdir)
        {
          fuse_reply_err (req, EXDEV);
          return;
        }
    }
  pnode = node->parent;

  destpnode = do_lookup_file (lo, newparent, NULL);
  destnode = NULL;

  pnode = get_node_up (lo, pnode);
  if (pnode == NULL)
    goto error;

  ret = TEMP_FAILURE_RETRY (openat (node_dirfd (pnode), pnode->path, O_DIRECTORY));
  if (ret < 0)
    goto error;
  srcfd = ret;

  destpnode = get_node_up (lo, destpnode);
  if (destpnode == NULL)
    goto error;

  ret = TEMP_FAILURE_RETRY (openat (node_dirfd (destpnode), destpnode->path, O_DIRECTORY));
  if (ret < 0)
    goto error;
  destfd = ret;

  destnode = do_lookup_file (lo, newparent, newname);

  node = get_node_up (lo, node);
  if (node == NULL)
    goto error;

  if (destnode == NULL)
    {
      errno = ENOENT;
      goto error;
    }
  if (node_dirp (node) && destnode->present_lowerdir)
    {
      fuse_reply_err (req, EXDEV);
      return;
    }
  destnode = get_node_up (lo, destnode);
  if (destnode == NULL)
    goto error;


  ret = syscall (SYS_renameat2, srcfd, name, destfd, newname, flags);
  if (ret < 0)
    goto error;

  rm1 = hash_delete (destpnode->children, destnode);
  rm2 = hash_delete (pnode->children, node);

  tmp = node->path;
  node->path = destnode->path;
  destnode->path = tmp;

  tmp = node->name;
  node->name = destnode->name;
  destnode->name = tmp;

  node = insert_node (destpnode, node, true);
  if (node == NULL)
    {
      node_free (rm1);
      node_free (rm2);
      goto error;
    }
  destnode = insert_node (pnode, destnode, true);
  if (destnode == NULL)
    {
      node_free (rm1);
      node_free (rm2);
      goto error;
    }
  if ((update_paths (node) < 0) || (update_paths (destnode) < 0))
    goto error;

#if 0
  if (delete_whiteout (lo, destfd, NULL, newname) < 0)
    goto error;
#endif

  ret = 0;
  goto cleanup;

 error:
  ret = -1;

 cleanup:
  saved_errno = errno;
  if (srcfd >= 0)
    close (srcfd);
  if (destfd >= 0)
    close (destfd);
  errno = saved_errno;

  fuse_reply_err (req, ret == 0 ? 0 : errno);
}

static void
ovl_rename_direct (fuse_req_t req, fuse_ino_t parent, const char *name,
                   fuse_ino_t newparent, const char *newname,
                   unsigned int flags)
{
  FUSE_ENTER(req);

  struct ovl_node *pnode, *node, *destnode, *destpnode;
  struct ovl_data *lo = ovl_data (req);
  struct ovl_layer *layer;
  int ret;
  int saved_errno;
  int srcfd = -1;
  int destfd = -1;
  struct ovl_node key;

  debug_print ("ovl_rename_direct path=%s name=%s newparent=%s newname=%s\n",
                (parent==FUSE_ROOT_ID)?"ROOT":((struct ovl_node *)parent)->path,
                name,
                (newparent==FUSE_ROOT_ID)?"ROOT":((struct ovl_node *)newparent)->path,
                newname);

  node = do_lookup_file (lo, parent, name);
  if (node == NULL)
    {
      fuse_reply_err (req, ENOENT);
      goto exit;
    }

  layer = node->layer;
  pnode = node->parent;

/*
  if (layer == get_upper_layer (lo))
    debug_print ("ovl_rename_direct work in upper layer %s\n", layer->path);
  else
    debug_print ("ovl_rename_direct stay in lower layer=%s\n", layer->path);
*/

  destpnode = do_lookup_file (lo, newparent, NULL);
  if (destpnode == NULL)
    {
      fuse_reply_err (req, ENOENT);
      goto exit;
    }

  debug_print ("ovl_rename_direct destpnode path=%s layer=%s\n",
            destpnode->path, destpnode->layer->path);


  if (node_dirp (node))
    {
      debug_print ("ovl_rename_direct %s is directory\n", name);
      node = load_dir (lo, node, node->layer, node->path, node->name);
      if (node == NULL)
        {
          debug_print ("ovl_rename_direct load_dir failed errno=%d\n", errno);
          fuse_reply_err (req, errno);
          return;
        }
    }

  /*
   * RENAME IS DONE IN THE SAME LAYER, ALWAYS
   * IF DEST IS PRESENT IN DIFFERENT LAYER -> UNLINK IN DIFFERENT LAYER
   *
   */
  //if (layer == get_upper_layer (lo))
   // {
  debug_print ("ovl_rename_direct work in layer %s\n", layer->path);

  ret = TEMP_FAILURE_RETRY (openat (layer->fd, pnode->path, O_DIRECTORY));
  if (ret < 0)
    goto error;
  srcfd = ret;

  debug_print ("ovl_rename_direct destpnode layer is %s\n", destpnode->layer->path);

  if (destpnode->layer != layer)
    {
      debug_print ("ovl_rename_direct destpnode %s is in different layer\n",
        destpnode->path);
      if (layer == get_upper_layer (lo))
        {
          // normal copyup when we work in upper
          destpnode = get_node_up (lo, destpnode);
          if (destpnode == NULL)
            goto error;
          debug_print ("ovl_rename_direct destpnode %s is now in layer %s\n",
            destpnode->path, destpnode->layer->path);
        }
      else
        {
          debug_print ("ovl_rename_direct creating tree in lower layer %s for %s\n",
                       layer->path, destpnode->path);
          ret = create_lower_directory (lo, layer->fd, destpnode);
          if (ret < 0)
            {
              debug_print ("create_node_directory: create_lower_directory %s ret=%d errno=%d\n",
                    destpnode->path, ret, errno);
              goto error;
            }
        }
    }

/*
  destpnode = get_node_up (lo, destpnode);
  if (destpnode == NULL)
    goto error;
*/

  ret = TEMP_FAILURE_RETRY (openat (layer->fd, destpnode->path, O_DIRECTORY));
  if (ret < 0)
    goto error;
  destfd = ret;

  key.name = (char *) newname;
  destnode = hash_lookup (destpnode->children, &key);

/*
      node = get_node_up (lo, node);
      if (node == NULL)
        goto error;
*/

      if (flags & RENAME_NOREPLACE && destnode)
        {
          errno = EEXIST;
          debug_print ("ovl_rename_direct NOREPLACE error destnode %s already exists\n", destnode->path);
          goto error;
        }

      if (destnode)
        {
          debug_print ("ovl_rename_direct destnode %s already exists in layer %s\n",
                       destnode->path, destnode->layer->path);
          ret = do_node_rm(req, NODE_TO_INODE(destpnode), newname, node_dirp(destnode));
          debug_print ("ovl_rename_direct do_node_rm ret=%d\n", ret);
          if (ret < 0)
              goto error;
        }

      ret = renameat (srcfd, name, destfd, newname);
      if (ret < 0)
        {
          debug_print ("ovl_rename_direct renameat failed errno=%d\n", errno);
          goto error;
        }


    //}

#if 0
  pnode = node->parent;

  destpnode = do_lookup_file (lo, newparent, NULL);
  destnode = NULL;

  //pnode = get_node_up (lo, pnode);
  //if (pnode == NULL)
  //  goto error;

  ret = TEMP_FAILURE_RETRY (openat (node_dirfd (pnode), pnode->path, O_DIRECTORY));
  if (ret < 0)
    goto error;
  srcfd = ret;

  destpnode = get_node_up (lo, destpnode);
  if (destpnode == NULL)
    goto error;

  ret = TEMP_FAILURE_RETRY (openat (node_dirfd (destpnode), destpnode->path, O_DIRECTORY));
  if (ret < 0)
    goto error;
  destfd = ret;

  key.name = (char *) newname;
  destnode = hash_lookup (destpnode->children, &key);

  node = get_node_up (lo, node);
  if (node == NULL)
    goto error;

  if (flags & RENAME_NOREPLACE && destnode && !destnode->whiteout)
    {
      errno = EEXIST;
      goto error;
    }

  if (destnode)
    {
      size_t destnode_whiteouts = 0;

      if (!destnode->whiteout && destnode->ino == node->ino)
        goto error;

      destnode_is_whiteout = destnode->whiteout;

      if (!destnode->whiteout && node_dirp (destnode))
        {
          destnode = load_dir (lo, destnode, destnode->layer, destnode->path, destnode->name);
          if (destnode == NULL)
            goto error;

          if (count_dir_entries (destnode, &destnode_whiteouts) > 0)
            {
              errno = ENOTEMPTY;
              goto error;
            }
          if (destnode_whiteouts && empty_dir (lo, destnode) < 0)
            goto error;
        }

      if (node_dirp (node) && create_missing_whiteouts (lo, node, destnode->path) < 0)
        goto error;

      if (destnode->lookups > 0)
        node_free (destnode);
      else
        {
          node_free (destnode);
          destnode = NULL;
        }

      if (destnode)
        {
          /* If the node is still accessible then be sure we
             can write to it.  Fix it to be done when a write is
             really done, not now.  */
          destnode = get_node_up (lo, destnode);
          if (destnode == NULL)
            {
              fuse_reply_err (req, errno);
              return;
            }

          if (hide_node (lo, destnode, false) < 0)
            goto error;
        }
    }

  /* If the destnode is a whiteout, first attempt to EXCHANGE the source and the destination,
   so that with one operation we get both the rename and the whiteout created.  */
  if (destnode_is_whiteout)
    {
      ret = syscall (SYS_renameat2, srcfd, name, destfd, newname, flags|RENAME_EXCHANGE);
      if (ret == 0)
        goto done;

      /* If it fails for any reason, fallback to the more articulated method.  */
    }

  /* If the node is a directory we must ensure there is no whiteout at the
     destination, otherwise the renameat2 will fail.  Create a .wh.$NAME style
     whiteout file until the renameat2 is completed.  */
  if (node_dirp (node))
    {
      ret = create_whiteout (lo, destpnode, newname, true);
      if (ret < 0)
        goto error;
      unlinkat (destfd, newname, 0);
    }

  /* Try to create the whiteout atomically, if it fails do the
     rename+mknod separately.  */
  ret = syscall (SYS_renameat2, srcfd, name, destfd,
                 newname, flags|RENAME_WHITEOUT);
  if (ret < 0)
    {
      ret = syscall (SYS_renameat2, srcfd, name, destfd, newname, flags);
      if (ret < 0)
        goto error;

      ret = create_whiteout (lo, pnode, name, false);
      if (ret < 0)
        goto error;
    }

  if (delete_whiteout (lo, destfd, NULL, newname) < 0)
    goto error;

 done:
  hash_delete (pnode->children, node);

  free (node->name);
  node->name = strdup (newname);
  if (node->name == NULL)
    {
      ret = -1;
      goto error;
    }

  node = insert_node (destpnode, node, true);
  if (node == NULL)
    goto error;
  if (update_paths (node) < 0)
    goto error;

  ret = 0;
  goto cleanup;

 error:
  ret = -1;

 cleanup:
  saved_errno = errno;
  if (srcfd >= 0)
    close (srcfd);
  if (destfd >= 0)
    close (destfd);
  errno = saved_errno;

  fuse_reply_err (req, ret == 0 ? 0 : errno);


#####j

  struct ovl_layer *it;
  struct ovl_node *pnode, *node, *destnode, *destpnode;
  struct ovl_data *lo = ovl_data (req);
  int ret;
  int saved_errno;
  int srcfd = -1;
  int destfd = -1;
  struct ovl_node key;
  bool destnode_is_whiteout = false;

  debug_print ("ovl_rename_direct path=%s name=%s newname=%s\n",
                ((struct ovl_node *)parent)->path,
                name, newname);

  node = do_lookup_file (lo, parent, name);
  if (node == NULL)
    {
      fuse_reply_err (req, ENOENT);
      goto exit;
    }
  destpnode = do_lookup_file (lo, newparent, NULL);
  if (destpnode == NULL)
    {
      fuse_reply_err (req, ENOENT);
      goto exit;
    }

  ret = ENOENT;
  for (it = node->layer; it; it = it->next)
    {
      debug_print ("ovl_rename_direct path=%s layer=%s destpnode=%s newname=%s\n",
                        node->path, it->path, destpnode, newname);

      if (TEMP_FAILURE_RETRY(renameat (it->fd, node->path, it->fd, newname)) < 0)
        {
          debug_print ("ovl_rename_direct renameat errno=%d\n", errno);
          if (ret != 0)
            ret = errno;
        }
      else
          ret = 0;  // one rename successfull is enough
    }

  if (ret == 0)
    {
      pnode = node->parent;

      hash_delete (pnode->children, node);
      free (node->name);
      node->name = strdup (newname);
      if (node->name == NULL)
        {
          ret = errno;
          goto error;
        }

      node = insert_node (destpnode, node, true);
      if (node == NULL)
        {
          ret = errno;
          goto error;
        }
      if (update_paths (node) < 0)
        {
          ret = errno;
          goto error;
        }
    }

error:
  fuse_reply_err (req, ret);
  FUSE_EXIT;




  if (node_dirp (node))
    {
      node = load_dir (lo, node, node->layer, node->path, node->name);
      if (node == NULL)
        {
          fuse_reply_err (req, errno);
          goto exit;
        }

      if (node->layer != get_upper_layer (lo) || node->present_lowerdir)
        {
          fuse_reply_err (req, EXDEV);
          goto exit;
        }
    }
  pnode = node->parent;

  destpnode = do_lookup_file (lo, newparent, NULL);
  destnode = NULL;

  pnode = get_node_up (lo, pnode);
  if (pnode == NULL)
    goto error;

  ret = TEMP_FAILURE_RETRY (openat (node_dirfd (pnode), pnode->path, O_DIRECTORY));
  if (ret < 0)
    goto error;
  srcfd = ret;

  destpnode = get_node_up (lo, destpnode);
  if (destpnode == NULL)
    goto error;

  ret = TEMP_FAILURE_RETRY (openat (node_dirfd (destpnode), destpnode->path, O_DIRECTORY));
  if (ret < 0)
    goto error;
  destfd = ret;

  key.name = (char *) newname;
  destnode = hash_lookup (destpnode->children, &key);

  node = get_node_up (lo, node);
  if (node == NULL)
    goto error;

  if (flags & RENAME_NOREPLACE && destnode)
    {
      errno = EEXIST;
      goto error;
    }

  if (destnode)
    {
      size_t destnode_whiteouts = 0;

      if (destnode->ino == node->ino)
        goto error;

      destnode_is_whiteout = false; //destnode->whiteout;

      if (node_dirp (destnode))
        {
          destnode = load_dir (lo, destnode, destnode->layer, destnode->path, destnode->name);
          if (destnode == NULL)
            goto error;

          if (count_dir_entries (destnode, &destnode_whiteouts) > 0)
            {
              errno = ENOTEMPTY;
              goto error;
            }
          if (destnode_whiteouts && empty_dir (lo, destnode) < 0)
            goto error;
        }


      if (destnode->lookups > 0)
        node_free (destnode);
      else
        {
          node_free (destnode);
          destnode = NULL;
        }

      if (destnode)
        {
          /* If the node is still accessible then be sure we
             can write to it.  Fix it to be done when a write is
             really done, not now.  */
          destnode = get_node_up (lo, destnode);
          if (destnode == NULL)
            {
              fuse_reply_err (req, errno);
              goto exit;
            }

          if (hide_node (lo, destnode, false) < 0)
            goto error;
        }
    }

  /* If the destnode is a whiteout, first attempt to EXCHANGE the source and the destination,
   so that with one operation we get both the rename and the whiteout created.  */
  if (destnode_is_whiteout)
    {
      ret = syscall (SYS_renameat2, srcfd, name, destfd, newname, flags|RENAME_EXCHANGE);
      if (ret == 0)
        goto done;

      /* If it fails for any reason, fallback to the more articulated method.  */
    }


 done:
  hash_delete (pnode->children, node);

  free (node->name);
  node->name = strdup (newname);
  if (node->name == NULL)
      goto error;

  node = insert_node (destpnode, node, true);
  if (node == NULL)
    goto error;
  if (update_paths (node) < 0)
    goto error;

  ret = 0;
  goto cleanup;

 error:
  ret = -1;

 cleanup:
  saved_errno = errno;
  if (srcfd >= 0)
    close (srcfd);
  if (destfd >= 0)
    close (destfd);
  errno = saved_errno;

#endif
 done:
  hash_delete (pnode->children, node);

  free (node->name);
  node->name = strdup (newname);
  if (node->name == NULL)
    {
      ret = -1;
      goto error;
    }

  node = insert_node (destpnode, node, true);
  if (node == NULL)
    goto error;
  if (update_paths (node) < 0)
    goto error;

  ret = 0;
  goto cleanup;

error:
  ret = -1;

cleanup:
  saved_errno = errno;
  if (srcfd >= 0)
    close (srcfd);
  if (destfd >= 0)
    close (destfd);
  errno = saved_errno;

  fuse_reply_err (req, ret == 0 ? 0 : errno);
exit:
  FUSE_EXIT;
}

static void
ovl_rename (fuse_req_t req, fuse_ino_t parent, const char *name,
           fuse_ino_t newparent, const char *newname,
           unsigned int flags)
{
  if (ovl_debug (req))
    fprintf (stderr, "ovl_rename(ino=%" PRIu64 "s, name=%s , ino=%" PRIu64 "s, name=%s)\n", parent, name, newparent, newname);

  if (flags & RENAME_EXCHANGE)
    ovl_rename_exchange (req, parent, name, newparent, newname, flags);
  else
    ovl_rename_direct (req, parent, name, newparent, newname, flags);
}

static void
ovl_statfs (fuse_req_t req, fuse_ino_t ino)
{
  FUSE_ENTER(req);

  int ret;
  struct statvfs lo_sfs, up_sfs;
  struct ovl_data *lo = ovl_data (req);

  if (ovl_debug (req))
    fprintf (stderr, "ovl_statfs(ino=%" PRIu64 "s)\n", ino);

  ret = statvfs (lo->upperdir, &up_sfs);
  if (ret < 0)
    {
      fuse_reply_err (req, errno);
      goto exit;
    }
  ret = statvfs (lo->lowerdir, &lo_sfs);
  if (ret < 0)
    {
      fuse_reply_err (req, errno);
      goto exit;
    }

  up_sfs.f_blocks += lo_sfs.f_blocks;
  //up_sfs.f_bfree += lo_sfs.f_bfree;
  //up_sfs.f_bavail += lo_sfs.f_bavail;
  up_sfs.f_files += lo_sfs.f_files;
  //up_sfs.f_ffree += lo_sfs.f_ffree;
  //up_sfs.f_favail += lo_sfs.f_favail;

  fuse_reply_statfs (req, &up_sfs);
exit:
  FUSE_EXIT;
}

static void
ovl_readlink (fuse_req_t req, fuse_ino_t ino)
{
  FUSE_ENTER(req);

  struct ovl_node *node;
  struct ovl_data *lo = ovl_data (req);
  int ret = 0;
  char buf[PATH_MAX + 1];

  if (ovl_debug (req))
    fprintf (stderr, "ovl_readlink(ino=%" PRIu64 "s)\n", ino);

  node = do_lookup_file (lo, ino, NULL);
  if (node == NULL)
    {
      fuse_reply_err (req, ENOENT);
      FUSE_EXIT;
      return;
    }

  // copyup all symlinks read
  node = get_node_up (lo, node);
  if (node == NULL)
    {
      fuse_reply_err (req, errno);
      return;
    }

  ret = readlinkat (node_dirfd (node), node->path, buf, sizeof (buf));
  if (ret == -1)
    {
      fuse_reply_err (req, errno);
      FUSE_EXIT;
      return;
    }
  if (ret == sizeof (buf))
    {
      fuse_reply_err (req, ENAMETOOLONG);
      FUSE_EXIT;
      return;
    }

  buf[ret] = '\0';
  fuse_reply_readlink (req, buf);
  FUSE_EXIT;
}

static int
hide_all (struct ovl_data *lo, struct ovl_node *node)
{
  struct ovl_node **nodes;
  size_t i, nodes_size;

  node = load_dir (lo, node, node->layer, node->path, node->name);
  if (node == NULL)
    return -1;

  nodes_size = hash_get_n_entries (node->children) + 2;
  nodes = malloc (sizeof (struct ovl_node *) * nodes_size);
  if (nodes == NULL)
    return -1;

  nodes_size = hash_get_entries (node->children, (void **) nodes, nodes_size);
  for (i = 0; i < nodes_size; i++)
    {
      struct ovl_node *it;
      int ret;

      it = nodes[i];
      ret = create_whiteout (lo, node, it->name, false);
      node_free (it);

      if (ret < 0)
        {
          free(nodes);
          return ret;
        }
    }

  free (nodes);
  return 0;
}

static void
ovl_mknod (fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode, dev_t rdev)
{
  FUSE_ENTER(req);

  struct ovl_node *node;
  struct ovl_data *lo = ovl_data (req);
  struct ovl_node *pnode;
  int ret = 0;
  char path[PATH_MAX + 10];
  struct fuse_entry_param e;
  const struct fuse_ctx *ctx = fuse_req_ctx (req);

  if (ovl_debug (req))
    fprintf (stderr, "ovl_mknod(ino=%" PRIu64 ", name=%s, mode=%d, rdev=%lu)\n",
         parent, name, mode, rdev);

  node = do_lookup_file (lo, parent, name);
  if (node != NULL)
    {
      fuse_reply_err (req, EEXIST);
      goto exit;
    }

  pnode = do_lookup_file (lo, parent, NULL);
  if (pnode == NULL)
    {
      fuse_reply_err (req, ENOENT);
      goto exit;
    }

  pnode = get_node_up (lo, pnode);
  if (pnode == NULL)
    {
      fuse_reply_err (req, errno);
      goto exit;
    }
  sprintf (path, "%s/%s", pnode->path, name);
  ret = mknodat (get_upper_layer (lo)->fd, path, mode & ~ctx->umask, rdev);
  if (ret < 0)
    {
      fuse_reply_err (req, errno);
      goto exit;
    }

  node = make_ovl_node (path, get_upper_layer (lo), name, 0, false, pnode);
  if (node == NULL)
    {
      fuse_reply_err (req, ENOMEM);
      goto exit;
    }

  node = insert_node (pnode, node, true);
  if (node == NULL)
    {
      fuse_reply_err (req, ENOMEM);
      goto exit;
    }

#if 0
  if (delete_whiteout (lo, -1, pnode, name) < 0)
    {
      fuse_reply_err (req, errno);
      return;
    }
#endif

  memset (&e, 0, sizeof (e));

  ret = rpl_stat (req, node, &e.attr);
  if (ret)
    {
      fuse_reply_err (req, errno);
      goto exit;
    }

  e.ino = NODE_TO_INODE (node);
  e.attr_timeout = ATTR_TIMEOUT;
  e.entry_timeout = ENTRY_TIMEOUT;
  node->lookups++;
  debug_print ("ovl_mknod: inc lookups=%d\n", node->lookups);
  fuse_reply_entry (req, &e);
exit:
  FUSE_EXIT;
}

static void
ovl_mkdir (fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode)
{
  FUSE_ENTER(req);

  struct ovl_node *node;
  struct ovl_data *lo = ovl_data (req);
  struct ovl_node *pnode;
  int ret = 0;
  char path[PATH_MAX + 10];
  struct fuse_entry_param e;
  const struct fuse_ctx *ctx = fuse_req_ctx (req);

  if (ovl_debug (req))
    fprintf (stderr, "ovl_mkdir(ino=%" PRIu64 ", name=%s, mode=%d)\n",
	     parent, name, mode);

  node = do_lookup_file (lo, parent, name);
  if (node != NULL)
    {
      fuse_reply_err (req, EEXIST);
      FUSE_EXIT;
      return;
    }

  pnode = do_lookup_file (lo, parent, NULL);
  if (pnode == NULL)
    {
      fuse_reply_err (req, ENOENT);
      FUSE_EXIT;
      return;
    }

  pnode = get_node_up (lo, pnode);
  if (pnode == NULL)
    {
      fuse_reply_err (req, errno);
      FUSE_EXIT;
      return;
    }
  sprintf (path, "%s/%s", pnode->path, name);

  ret = create_directory (lo, get_upper_layer (lo)->fd, path, NULL, pnode, -1,
                          mode & ~ctx->umask);
  if (ret < 0)
    {
      fuse_reply_err (req, errno);
      FUSE_EXIT;
      return;
    }

  node = make_ovl_node (path, get_upper_layer (lo), name, 0, true, pnode);
  if (node == NULL)
    {
      fuse_reply_err (req, ENOMEM);
      FUSE_EXIT;
      return;
    }

  node = insert_node (pnode, node, true);
  if (node == NULL)
    {
      fuse_reply_err (req, ENOMEM);
      FUSE_EXIT;
      return;
    }
#if 0
  ret = hide_all (lo, node);
  if (ret < 0)
    {
      fuse_reply_err (req, errno);
      FUSE_EXIT;
      return;
    }

  if (delete_whiteout (lo, -1, pnode, name) < 0)
    {
      fuse_reply_err (req, errno);
      FUSE_EXIT;
      return;
    }
#endif

  memset (&e, 0, sizeof (e));

  ret = rpl_stat (req, node, &e.attr);
  if (ret)
    {
      fuse_reply_err (req, errno);
      FUSE_EXIT;
      return;
    }

  e.ino = NODE_TO_INODE (node);
  e.attr_timeout = ATTR_TIMEOUT;
  e.entry_timeout = ENTRY_TIMEOUT;
  node->lookups++;
  debug_print ("ovl_mkdir: inc lookups=%d\n", node->lookups);
  fuse_reply_entry (req, &e);
  FUSE_EXIT;
}

static void
ovl_fsync (fuse_req_t req, fuse_ino_t ino, int datasync, struct fuse_file_info *fi)
{
  FUSE_ENTER(req);

  int ret, fd;

  if (ovl_debug (req))
    fprintf (stderr, "ovl_fsync(ino=%" PRIu64 ", datasync=%d, fi=%p)\n",
             ino, datasync, fi);

  fd = fi->fh;
  ret = datasync ? fdatasync (fd) : fsync (fd);
  fuse_reply_err (req, ret == 0 ? 0 : errno);

  FUSE_EXIT;
}

#if ENABLE_IOCTL
static void
ovl_ioctl (fuse_req_t req, fuse_ino_t ino, int cmd, void *arg,
           struct fuse_file_info *fi, unsigned flags, const void *in_buf,
           size_t in_bufsz, size_t out_bufsz)
{
  FUSE_ENTER(req);

  debug_print("ioctl %0x%x: insize: %u outsize: %u\n", cmd, in_bufsz, out_bufsz);

  struct ovl_data *lo = ovl_data (req);
  struct ovl_node *node;
  int fd;
  int err;
  char *mybuf = malloc(in_bufsz+out_bufsz);
  memset(mybuf, 0, in_bufsz+out_bufsz);
  memcpy(mybuf, in_buf, in_bufsz);

  if (flags & FUSE_IOCTL_COMPAT)
    {
      fuse_reply_err (req, ENOSYS);
      goto exit;
    }

  node = do_lookup_file (lo, ino, NULL);
  if (node == NULL)
    {
      fuse_reply_err (req, ENOENT);
      goto exit;
    }
  debug_print("ioctl node->path=%s\n", node->path);

  /*
  fd = TEMP_FAILURE_RETRY (openat (node_dirfd (node), node->path, O_RDONLY|O_NONBLOCK));
  if (fd < 0)
    {
      fuse_reply_err (req, errno);
      goto exit;
    }
  debug_print("ioctl openat successful fd=%d path=%s\n", fd, node->path);
  */

  debug_print("ioctl fi->fh=%d\n", fi->fh);

  err = TEMP_FAILURE_RETRY( ioctl (fi->fh, cmd, mybuf) );
  if (err < 0)
    {
      debug_print("ioctl failed with err=%d errno=%d\n", err, errno);
      //close(fd);
      //fuse_reply_ioctl(req, err, arg, in_bufsz);
      fuse_reply_err (req, errno);
      goto exit;
    }
  debug_print("ioctl successfull!!! err=%d\n", err);
  //struct iovec iov = { arg, sizeof(size_t) };
  //fuse_reply_ioctl_retry(req, NULL, 0, &iov, 1);

  fuse_reply_ioctl(req, err, mybuf, in_bufsz);

  //close(fd);
  //fuse_reply_ioctl(req, 0, arg, 0);
  //fuse_reply_ioctl(req, err, arg, in_bufsz);
  //fuse_reply_err (req, 0);
exit:
  //free(mybuf);
  FUSE_EXIT;
}
#endif

static struct fuse_lowlevel_ops ovl_oper =
  {
   .statfs = ovl_statfs,
   .access = ovl_access,
   .getxattr = ovl_getxattr,
   .removexattr = ovl_removexattr,
   .setxattr = ovl_setxattr,
   .listxattr = ovl_listxattr,
   .init = ovl_init,
   .lookup = ovl_lookup,
   .forget = ovl_forget,
   .getattr = ovl_getattr,
   .readlink = ovl_readlink,
   .opendir = ovl_opendir,
   .readdir = ovl_readdir,
   .readdirplus = ovl_readdirplus,
   .releasedir = ovl_releasedir,
   .create = ovl_create,
   .open = ovl_open,
   .release = ovl_release,
   .read = ovl_read,
   .write_buf = ovl_write_buf,
   .unlink = ovl_unlink,
   .rmdir = ovl_rmdir,
   .setattr = ovl_setattr,
   .symlink = ovl_symlink,
   .rename = ovl_rename,
   .mkdir = ovl_mkdir,
   .mknod = ovl_mknod,
   .link = ovl_link,
   .fsync = ovl_fsync,
   .flock = ovl_flock,
#if ENABLE_IOCTL
   .ioctl = ovl_ioctl,
#endif
  };

static int
fuse_opt_proc (void *data, const char *arg, int key, struct fuse_args *outargs)
{
  struct ovl_data *ovl_data = data;

  if (strcmp (arg, "-f") == 0)
    return 1;
  if (strcmp (arg, "--debug") == 0)
    return 1;

  if (strcmp (arg, "allow_root") == 0)
    return 1;
  if (strcmp (arg, "default_permissions") == 0)
    return 1;
  if (strcmp (arg, "allow_other") == 0)
    return 1;

  if (key == FUSE_OPT_KEY_NONOPT)
    {
      if (ovl_data->mountpoint)
        free (ovl_data->mountpoint);

      ovl_data->mountpoint = strdup (arg);
      return 0;
    }
  /* Ignore unknown arguments.  */
  if (key == -1)
    return 0;

  return 1;
}

char **
get_new_args (int *argc, char **argv)
{
  int i;
  char **newargv = malloc (sizeof (char *) * (*argc + 2));
  newargv[0] = argv[0];
  newargv[1] = "-odefault_permissions,allow_other";
  for (i = 1; i < *argc; i++)
    newargv[i + 1] = argv[i];
  (*argc)++;
  return newargv;
}

static void
set_limits ()
{
  struct rlimit l;

  if (getrlimit (RLIMIT_NOFILE, &l) < 0)
    error (EXIT_FAILURE, errno, "cannot read process rlimit");

  /* Set the soft limit to the hard limit.  */
  l.rlim_cur = l.rlim_max;

  if (setrlimit (RLIMIT_NOFILE, &l) < 0)
    error (EXIT_FAILURE, errno, "cannot set process rlimit");
}

int
main (int argc, char *argv[])
{
  struct fuse_session *se;
  struct fuse_cmdline_opts opts;
  char **newargv = get_new_args (&argc, argv);
  struct ovl_data lo = {.debug = 0,
                        .root = NULL,
                        .lowerdir = NULL,
                        .mountpoint = NULL,
  };
  int ret = -1;
  struct fuse_args args = FUSE_ARGS_INIT (argc, newargv);

  memset (&opts, 0, sizeof (opts));
  if (fuse_opt_parse (&args, &lo, ovl_opts, fuse_opt_proc) == -1)
    error (EXIT_FAILURE, 0, "error parsing options");
  if (fuse_parse_cmdline (&args, &opts) != 0)
    error (EXIT_FAILURE, 0, "error parsing cmdline");

  if (opts.mountpoint)
    free (opts.mountpoint);

  if (opts.show_help)
    {
      printf ("usage: %s [options] <mountpoint>\n\n", argv[0]);
      fuse_cmdline_help ();
      fuse_lowlevel_help ();
      exit (EXIT_SUCCESS);
    }
  else if (opts.show_version)
    {
      printf ("FUSE library version %s\n", fuse_pkgversion ());
      fuse_lowlevel_version ();
      exit (EXIT_SUCCESS);
    }

  lo.debug = opts.debug;

  if (lo.upperdir == NULL)
    error (EXIT_FAILURE, 0, "upperdir not specified");
  else
    {
      char full_path[PATH_MAX + 1];

      if (realpath (lo.upperdir, full_path) < 0)
        error (EXIT_FAILURE, errno, "cannot retrieve path for %s", lo.upperdir);

      lo.upperdir = strdup (full_path);
      if (lo.upperdir == NULL)
        error (EXIT_FAILURE, errno, "cannot allocate memory");
    }

  set_limits ();

  printf ("UPPERDIR=%s\n", lo.upperdir);
  printf ("LOWERDIR=%s\n", lo.lowerdir);
  printf ("MOUNTPOINT=%s\n", lo.mountpoint);

  lo.layers = read_dirs (lo.lowerdir, true, NULL);
  if (lo.layers == NULL)
    error (EXIT_FAILURE, errno, "cannot read lower dirs");

  lo.layers = read_dirs (lo.upperdir, false, lo.layers);
  if (lo.layers == NULL)
    error (EXIT_FAILURE, errno, "cannot read upper dir");

  lo.root = load_dir (&lo, NULL, get_upper_layer (&lo), ".", "");
  if (lo.root == NULL)
    error (EXIT_FAILURE, errno, "cannot read upper dir");
  lo.root->lookups = 2;

  se = fuse_session_new (&args, &ovl_oper, sizeof (ovl_oper), &lo);
  lo.se = se;
  if (se == NULL)
    {
      error (0, errno, "cannot create FUSE session");
      goto err_out1;
    }
  if (fuse_set_signal_handlers (se) != 0)
    {
      error (0, errno, "cannot set signal handler");
      goto err_out2;
    }
  if (fuse_session_mount (se, lo.mountpoint) != 0)
    {
      error (0, errno, "cannot mount");
      goto err_out3;
    }
  fuse_daemonize (opts.foreground);
  ret = fuse_session_loop (se);
  fuse_session_unmount (se);
err_out3:
  fuse_remove_signal_handlers (se);
err_out2:
  fuse_session_destroy (se);
err_out1:

  node_mark_all_free (lo.root);

  node_free (lo.root);

  free_layers (lo.layers);
  fuse_opt_free_args (&args);

  return ret ? 1 : 0;
}
