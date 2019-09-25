#include <config.h>

#include <glib.h>
#include <gio/gio.h>

#include <errno.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <poll.h>
#include <stdlib.h>
#include <stdio.h>

#ifndef __NR_pidfd_open
#define __NR_pidfd_open 434
#endif

#ifndef pidfd_open
static int
pidfd_open(pid_t pid, unsigned int flags)
{
  return syscall(__NR_pidfd_open, pid, flags);
}
#endif


static int
report_error (GError *error)
{
  g_printerr ("%s: error", g_get_application_name ());
  if (error)
    g_printerr (": %s", error->message);
  g_printerr ("\n");

  return EXIT_FAILURE;
}

static int
usage_error (GError *error)
{
  report_error (error);

  g_printerr ("Try \"%s --help\" for more information.", g_get_prgname ());
  g_printerr ("\n");

  return EXIT_FAILURE;
}

int
main(int argc, char **argv)
{
  g_autoptr(GOptionContext) optctx = NULL;
  g_autoptr(GError) err = NULL;
  g_autofree char *path = NULL;
  g_autofree char *data = NULL;
  int pidfd;
  gboolean ok;
  gboolean do_version = FALSE;
  GOptionEntry options[] = {
    { "version", 0, 0, G_OPTION_ARG_NONE, &do_version, "Print version information and exit", NULL },
    { NULL }
  };

  optctx = g_option_context_new ("PID");
  g_option_context_add_main_entries (optctx, options, NULL);

  if (!g_option_context_parse (optctx, &argc, &argv, &err))
    return usage_error (err);

  if (do_version)
    {
      g_print ("%s version: %s\n", g_get_application_name (), PACKAGE_VERSION);
      return EXIT_SUCCESS;
    }

  if (argc < 2)
    {
      g_set_error (&err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		   "missing argument: PID");
      return usage_error (err);
    }

  pidfd = pidfd_open (atoi(argv[1]), 0);

  if (pidfd == -1)
    {
      int code = g_io_error_from_errno (errno);
      g_set_error (&err, G_IO_ERROR, code, "could not open pidfd: %s",
		   g_strerror (errno));
      return report_error (err);
    }

  path = g_strdup_printf ("/proc/%d/fdinfo/%d", getpid(), pidfd);
  if (path == NULL)
    return EXIT_FAILURE;

  ok = g_file_get_contents (path, &data, NULL, NULL);
  if (!ok)
    {
      g_prefix_error (&err, "could not get fdinfo data: ");
      return report_error (err);
    }

  g_print ("%s\n", data);

  return EXIT_SUCCESS;
}
