#include <config.h>

#include <glib.h>
#include <gio/gio.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

static int
parse_pid (const char *str, pid_t *pid)
{
  char *end;
  guint64 p;

 errno = 0;
  p = g_ascii_strtoull (str, &end, 0);
  if (end == str || errno != 0)
    return -ENOENT;

  if (pid)
    *pid = (pid_t) p;

  return 0;
}

static int
parse_status_field_pid (char *val, pid_t *pid)
{
  const char *t;

  t = strrchr (val, '\t');
  if (t == NULL)
    return -ENOENT;

  return parse_pid (t, pid);
}

static int
parse_status_field_uid (char *val, uid_t *uid)
{
  const char *t;
  char *end;
  guint64 p;

  t = strrchr (val, '\t');
  if (t == NULL)
    return -ENOENT;

  errno = 0;
  p = g_ascii_strtoull (t, &end, 0);
  if (end == t || errno != 0)
    return -ENOENT;

  if (uid)
    *uid = (uid_t) p;

  return 0;
}

static gboolean
map_pid (int fd, pid_t *pid_out, uid_t *uid_out, GError **error)
{
  g_autofree char *key = NULL;
  g_autofree char *val = NULL;
  gboolean have_pid = FALSE;
  gboolean have_uid = FALSE;
  FILE *f;
  size_t keylen = 0;
  size_t vallen = 0;
  ssize_t n;
  int r = 0;

  g_return_val_if_fail (fd > -1, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  f = fdopen (fd, "r");

  if (f == NULL)
    {
      int code = g_io_error_from_errno (errno);
      g_set_error (error, G_IO_ERROR, code,
		   "Could not open files: %s",
		   g_strerror (errno));
      return FALSE;
    }

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
  DIR *proc = NULL;
  struct dirent *de = NULL;
  ino_t pidns;
  char *end = NULL;
  gboolean ok;
  gboolean do_version = FALSE;
  GOptionEntry options[] = {
    { "version", 0, 0, G_OPTION_ARG_NONE, &do_version, "Print version information and exit", NULL },
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

  proc = opendir ("/proc");

  while ((de = readdir (proc)) != NULL)
    {
      struct stat st;
      pid_t mapped = 0;
      uid_t uid = 0;
      int pid_fd;
      int r;

      if (de->d_type != DT_DIR)
      	continue;

      pid_fd = openat (dirfd (proc), de->d_name, DIR_OPEN_FLAGS);
      if (pid_fd == -1)
	{
	  g_warning ("Could not open %s: %s", de->d_name, g_strerror (errno));
	  continue;
	}

      r = fstatat (pid_fd, "ns/pid", &st, 0);
      if (r == -1)
	{
	  g_debug ("no pidns for %s", de->d_name);
	  continue;
	}

      if (pidns != st.st_ino)
	continue;

      g_print ("%s in %ld\n", de->d_name, pidns);

      r = openat (pid_fd, "status",  O_RDONLY | O_CLOEXEC);
      if (r == -1)
	continue;

      ok = map_pid (r, &mapped, &uid, &err);
      if (!ok)
	{
	  g_printerr ("Failed to map '%s': %s\n", de->d_name, err->message);
	  g_clear_error (&err);
	  continue;
	}

      g_print ("\t %lu [uid: %lu]\n",
	       (unsigned long) mapped, (unsigned long) uid);
    }

  closedir (proc);

  return 0;
}
