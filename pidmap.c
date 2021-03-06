#include <config.h>

#include <glib.h>
#include <gio/gio.h>

#include <json-glib/json-glib.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

/*  */

static int
parse_pid (const char *str,
	   pid_t      *pid)
{
  char *end;
  guint64 v;
  pid_t p;

  errno = 0;
  v = g_ascii_strtoull (str, &end, 0);
  if (end == str)
    return -ENOENT;
  else if (errno != 0)
    return -errno;

  p = (pid_t) v;

  if (p < 1 || (guint64) p != v)
    return -ERANGE;

  if (pid)
    *pid = p;

  return 0;
}

static int
parse_status_field_pid (const char *val,
			pid_t      *pid)
{
  const char *t;

  t = strrchr (val, '\t');
  if (t == NULL)
    return -ENOENT;

  return parse_pid (t, pid);
}

static int
parse_status_field_uid (const char *val,
			uid_t      *uid)
{
  const char *t;
  char *end;
  guint64 v;
  uid_t u;

  t = strrchr (val, '\t');
  if (t == NULL)
    return -ENOENT;

  errno = 0;
  v = g_ascii_strtoull (t, &end, 0);
  if (end == val)
    return -ENOENT;
  else if (errno != 0)
    return -errno;

  u = (uid_t) v;

  if ((guint64) u != v)
    return -ERANGE;

  if (uid)
    *uid = u;

  return 0;
}

static gboolean
lookup_ns_from_pid_fd (int pid_fd, ino_t *ns, GError **error)
{
  struct stat st;
  int r;

  g_return_val_if_fail (ns != NULL, FALSE);

  r = fstatat (pid_fd, "ns/pid", &st, 0);
  if (r == -1)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
		   "failed to stat '%u/ns/pid': %s", (guint) pid_fd,
		   g_strerror (errno));
      return FALSE;
    }

  *ns = st.st_ino;

  return TRUE;
}

static gboolean
parse_status_file (int pid_fd, pid_t *pid_out, uid_t *uid_out, GError **error)
{
  g_autofree char *key = NULL;
  g_autofree char *val = NULL;
  gboolean have_pid = FALSE;
  gboolean have_uid = FALSE;
  FILE *f;
  size_t keylen = 0;
  size_t vallen = 0;
  ssize_t n;
  int fd;
  int r = 0;

  g_return_val_if_fail (pid_fd > -1, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  fd = openat (pid_fd, "status",  O_RDONLY | O_CLOEXEC);
  if (fd == -1)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
		   "could not open 'status' file: %s",
		   g_strerror (errno));
      return FALSE;
    }

  f = fdopen (fd, "r");

  if (f == NULL)
    {
      int code = g_io_error_from_errno (errno);
      g_set_error (error, G_IO_ERROR, code,
		   "Could not open files: %s",
		   g_strerror (errno));
      (void) close (fd);
      return FALSE;
    }

  fd = -1; /* fd is now owned by f */

  do {
    n = getdelim (&key, &keylen, ':', f);

    if (n == -1)
      break;

    n = getdelim (&val, &vallen, '\n', f);

    if (n == -1)
      break;

    g_strstrip (val);

    if (!strncmp (key, "NSpid", strlen ("NSpid")))
      {
	r = parse_status_field_pid (val, pid_out);
	have_pid = r > -1;
      }
    else if (!strncmp (key, "Uid", strlen ("Uid")))
      {
	r = parse_status_field_uid (val, uid_out);
	have_uid = r > -1;
      }

  } while (r == 0 && (!have_uid || !have_pid));

  fclose (f);

  if (r != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   "Could not parse 'status' file");
      return FALSE;
    }
  else if (!have_uid || !have_pid)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   "Could not parse 'status' file: missing fields");

      return FALSE;
    }

  return TRUE;
}

typedef struct PidEntry_ {
  pid_t inside;
  pid_t outside;
  struct timespec timestamp;
  uid_t uid;

  /* */
  GError *error;
} PidEntry;

static PidEntry *
pid_entry_new (pid_t inside)
{
  PidEntry *entry = g_slice_new0 (PidEntry);
  entry->inside = inside;
  return entry;
}

static void
pid_entry_destroy (PidEntry *entry)
{
  if (entry == NULL)
    return;

  g_clear_error (&entry->error);
  g_slice_free (PidEntry, entry);
}

#define DIR_OPEN_FLAGS (O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOCTTY)

static void
close_fd_ptr (int *fd)
{
  if (fd == NULL || *fd < 0)
    return;

  (void) close (*fd);
}

static GHashTable *
map_pids (DIR *proc, ino_t pidns, pid_t *pids, guint n_pids)
{
  GHashTable *res;
  struct dirent *de;

  res = g_hash_table_new_full (g_int_hash, g_int_equal, NULL, (GDestroyNotify) pid_entry_destroy);

  if (pids != NULL)
    for (guint i = 0; i < n_pids; i++)
      g_hash_table_insert (res, pids + i, NULL);

  while ((de = readdir (proc)) != NULL)
    {
      __attribute__((cleanup(close_fd_ptr))) int pid_fd = -1;
      PidEntry *pe;
      struct stat st;
      gboolean ok;
      pid_t inside;
      ino_t ns;
      int r;

      if (de->d_type != DT_DIR)
      	continue;

      pid_fd = openat (dirfd (proc), de->d_name, DIR_OPEN_FLAGS);
      if (pid_fd == -1)
	{
	  g_warning ("Could not open %s: %s", de->d_name, g_strerror (errno));
	  continue;
	}

      ok = lookup_ns_from_pid_fd (pid_fd, &ns, NULL);
      if (!ok)
	continue;

      if (pidns != ns)
	continue;

      r = parse_pid (de->d_name, &inside);
      if (r < 0)
	continue;

      if (pids && !g_hash_table_contains (res, &inside))
	continue;

      pe = pid_entry_new (inside);
      g_hash_table_replace (res, &pe->inside, pe);

      ok = parse_status_file (pid_fd, &pe->outside, &pe->uid, &pe->error);
      if (!ok)
	continue;

      r = fstat (pid_fd, &st);
      if (r == -1)
	{
	  g_set_error (&pe->error, G_IO_ERROR, g_io_error_from_errno (errno),
		       "could not stat '%s: %s", de->d_name, g_strerror (errno));
	}
      else
	{
	  pe->timestamp = st.st_mtim;
	}
    }

  return res;
}

/*  */
pid_t
flatpak_get_child_pid (const char *instance, GError **error)
{
  g_autoptr(JsonParser) parser = NULL;
  g_autofree char *data = NULL;
  JsonNode *root;
  JsonObject *cpo;
  gsize len;
  char *path;
  pid_t pid;

  g_return_val_if_fail (instance != NULL, 0);

  path = g_build_filename (g_get_user_runtime_dir (),
			   ".flatpak",
			   instance,
			   "bwrapinfo.json",
			   NULL);

  if (!g_file_get_contents (path, &data, &len, error))
    {
      g_prefix_error (error, "could not load '%s': ", path);
      return 0;
    }

  parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, data, len, error))
    {
      g_prefix_error (error, "could not parse '%s': ", path);
      return 0;
    }

  root = json_parser_get_root (parser);
  if (!root)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   "unexepcted empty file at '%s'", path);
      return 0;
    }

  cpo = json_node_get_object (root);
  if (cpo == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   "unexepcted empty file at '%s'", path);
      return 0;
    }

  pid = json_object_get_int_member (cpo, "child-pid");
  if (pid == 0)
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   "failed to get child pid member of '%s'", path);

  return pid;
}

/*  */
int
usage_error (GError *error)
{
  g_printerr ("%s: error", g_get_application_name ());
  if (error)
    g_printerr (": %s", error->message);
  g_printerr ("\n");
  g_printerr ("Try \"%s --help\" for more information.", g_get_prgname ());
  g_printerr ("\n");

  return EXIT_FAILURE;
}

int
usage_error_need_arg (const char *arg)
{
  g_autoptr(GError) error = NULL;

  g_set_error (&error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
               "missing argument '%s'", arg);
  return usage_error (error);
}


int
main (int argc, char **argv)
{
  g_autoptr(GOptionContext) optctx = NULL;
  g_autoptr(GError) err = NULL;
  g_autoptr(GHashTable) mapped = NULL;
  g_autofree char *fp_instance = NULL;
  DIR *proc = NULL;
  ino_t pidns;
  char *end = NULL;
  GHashTableIter iter;
  gpointer key, value;
  gboolean ok;
  gboolean do_version = FALSE;
  GOptionEntry options[] = {
    { "version", 0, 0, G_OPTION_ARG_NONE, &do_version, "Print version information and exit", NULL },
    { "flatpak", 0, 0, G_OPTION_ARG_STRING, &fp_instance, "Map pids for the running flatpak", NULL },
    { NULL }
  };

  optctx = g_option_context_new ("[COMMAND]");
  g_option_context_add_main_entries (optctx, options, NULL);

  if (!g_option_context_parse (optctx, &argc, &argv, &err))
    return usage_error (err);

  if (do_version)
    {
      g_print ("%s version: %s\n", PACKAGE_NAME, PACKAGE_VERSION);
      return EXIT_SUCCESS;
    }

  proc = opendir ("/proc");

  if (fp_instance)
    {
      char buf[1024] = {0, };
      pid_t pid;
      int pid_fd;

      pid = flatpak_get_child_pid (fp_instance, &err);
      if (pid == 0)
	{
	  g_printerr ("Could not find flatpak instance: %s\n",
		      err->message);
	  return -1;
	}

      snprintf (buf, sizeof (buf), "%u", pid);
      pid_fd = openat (dirfd (proc), buf, DIR_OPEN_FLAGS);

      ok = lookup_ns_from_pid_fd (pid_fd, &pidns, &err);

      (void) close (pid_fd);
      if (!ok)
	{
	  g_printerr ("Could not resolve pid namespace: %s\n",
		      err->message);
	  return -1;
	}
    }
  else
    {

      if (argc < 2)
	{
	  g_printerr ("usage: %s <PIDNS>\n", argv[0]);
	  return -1;
	}

      errno = 0;
      pidns = (ino_t) strtoull (argv[1], &end, 10);

      if (argv[1] == end || errno != 0)
	{
	  g_printerr ("PIDNS format error\n");
	  g_printerr ("usage: %s <PIDNS>\n", argv[0]);
	  return -1;
	}
    }

  mapped = map_pids (proc, pidns, NULL, 0);

  g_hash_table_iter_init (&iter, mapped);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      PidEntry *e = (PidEntry *) key;

      if (e->error)
	{
	  g_printerr ("failed to map: %d; %s\n", e->inside, e->error->message);
	  continue;
	}

      g_print (" %d -> %d [%d]\n", e->inside, e->outside, e->uid);
    }

  closedir (proc);

  return 0;
}
