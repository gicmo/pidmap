
#include <glib.h>

#define _GNU_SOURCE 1

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

static int
parse_pid (char *val, pid_t *pid)
{
  const char *t;
  char *end;
  guint64 p;

  g_debug ("val: [%s]", val);

  t = strrchr (val, '\t');
  if (t == NULL)
    return -ENOENT;

  errno = 0;
  p = g_ascii_strtoull (t, &end, 0);
  if (end == t || errno != 0)
    return -ENOENT;

  if (pid)
    *pid = (pid_t) p;

  return 0;
}

static int
map_pid (int fd, pid_t *out)
{
  g_autofree char *key = NULL;
  g_autofree char *val = NULL;
  FILE *f;
  size_t keylen = 0;
  size_t vallen = 0;
  ssize_t n;
  pid_t p = 0;
  int r = 0;

  f = fdopen (fd, "r");

  if (f == NULL)
    return -errno;

  do {
    n = getdelim (&key, &keylen, ':', f);

    if (n == -1)
      break;

    n = getdelim (&val, &vallen, '\n', f);

    if (n == -1)
      break;

    if (!strncmp (key, "NSpid", strlen ("NSpid")))
      r = parse_pid (val, out);

  } while (r == 0 && p == 0);

  fclose (f);

  if (r != 0)
    return r;
  else if (p == 0)
    return -ENOENT;

  return 0;
}

int
main (int argc, char **argv)
{
  DIR *proc = NULL;
  struct dirent *de = NULL;
  ino_t pidns;
  char *end = NULL;

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
      char buffer[PATH_MAX] = {0, };
      struct stat st;
      pid_t mapped = 0;
      int r;

      snprintf (buffer, sizeof(buffer), "%s/ns/pid", de->d_name);

      r = fstatat (dirfd (proc), buffer, &st, 0);
      if (r == -1)
	continue;

      if (pidns != st.st_ino)
	continue;

      g_print ("%s in %ld\n", de->d_name, pidns);

      snprintf (buffer, sizeof(buffer), "%s/status", de->d_name);
      r = openat (dirfd (proc), buffer,  O_RDONLY | O_CLOEXEC);
      if (r == -1)
	continue;

      r = map_pid (r, &mapped);
      g_print ("\t %lu\n", (long unsigned) mapped);
    }

  closedir (proc);

  return 0;
}
