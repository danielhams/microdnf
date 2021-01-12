/* dnf-command-download.c
 *
 * Copyright © 2020-2021 Daniel Hams <daniel.hams@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "dnf-command-download.h"
#include "dnf-utils.h"

/* For MAXPATHLEN */
#include <sys/param.h>

struct _DnfCommandDownload
{
  PeasExtensionBase parent_instance;
};

static void dnf_command_download_iface_init (DnfCommandInterface *iface);

G_DEFINE_DYNAMIC_TYPE_EXTENDED (DnfCommandDownload,
                                dnf_command_download,
                                PEAS_TYPE_EXTENSION_BASE,
                                0,
                                G_IMPLEMENT_INTERFACE (DNF_TYPE_COMMAND,
                                                       dnf_command_download_iface_init))

static void
dnf_command_download_init (DnfCommandDownload *self)
{
}

inline static gboolean str_ends_with (const gchar * buf, const gchar * val)
{
  return strstr (buf, val) == (buf + (strlen (buf) - strlen (val)));
}

static gboolean dnf_command_download_rewriterepotosrc (gchar * repo_buf)
{
  if (str_ends_with (repo_buf, "-rpms"))
    {
      // Add a terminator in the right place before we strcat
      repo_buf[strlen (repo_buf) - 5] = '\0';
      strcat (repo_buf, "-source-rpms");
      return TRUE;
    }
  else if (!str_ends_with (repo_buf, "-source"))
    {
      strcat (repo_buf, "-source");
      return TRUE;
    }
  return FALSE;
}

static gboolean
dnf_command_download_enablesourcerepos (DnfContext *ctx,
                                        DnfRepoLoader *repo_loader,
                                        GError **error)
{
  g_autoptr(GError) local_error = NULL;
  // A place where we can "extend" the repoid with an extension
  gchar tmp_name[MAXPATHLEN];
  g_autoptr(GPtrArray) all_repos = dnf_repo_loader_get_repos (repo_loader, error);

  // We have to activate repos in a second loop to avoid breaking the iterator
  // on the above repo list. We are "strdup"ing strings into it, so need
  // free called on them.
  g_autoptr(GPtrArray) repos_to_enable = g_ptr_array_new_full (all_repos->len,
                                                               free);
  // Look for each enabled repo if there is a corresponding source repo
  // that we need to enable.
  for (guint i = 0 ; i < all_repos->len; ++i )
    {
      DnfRepo * repo = g_ptr_array_index (all_repos, i);
      const gchar * repo_id = dnf_repo_get_id (repo);
      if (dnf_repo_get_enabled (repo))
        {
          int repo_id_len = strlen (repo_id);
          if (repo_id_len >= (MAXPATHLEN - 1 - strlen ("-source-rpms")))
            {
              // Repo name too long when concatenated
              g_set_error (error,
                           DNF_ERROR,
                           DNF_ERROR_INTERNAL_ERROR,
                           "Repository name overflow when -source-rpms added (%1$s)",
                           repo_id);
              return FALSE;
            }
          strcpy (tmp_name, repo_id);
          // Only look for an associated source repo where adding an extension
          // makes sense
          if (dnf_command_download_rewriterepotosrc (tmp_name))
            {
              DnfRepo * src_repo = dnf_repo_loader_get_repo_by_id (repo_loader,
                                                                   tmp_name,
                                                                   &local_error);
              if (local_error != NULL)
                {
                  if (!g_error_matches (local_error, DNF_ERROR, DNF_ERROR_REPO_NOT_FOUND))
                    {
                      g_propagate_error (error, local_error);
                      return FALSE;
                    }
                  // Repo not found is not terminal
                  g_error_free (local_error);
                  local_error = NULL;
                  continue;
                }
              if (!dnf_repo_get_enabled (src_repo))
                {
                  // Repo exists and not enabled, add it to our list to enable
                  g_ptr_array_add (repos_to_enable, strdup (tmp_name));
                }
            }
          // else repo "source" equiv already enabled or doesn't exist
        }
    }

  // Repo enable loop
  for (guint r = 0 ; r < repos_to_enable->len; ++r)
    {
      gchar * repo_to_enable_id = g_ptr_array_index (repos_to_enable, r);
      if (!dnf_context_repo_enable (ctx, repo_to_enable_id, error))
        {
          return FALSE;
        }
      else
        {
          g_print ("enabling %s repository\n", repo_to_enable_id);
        }
    }
  return TRUE;
}

static gint
gptrarr_dnf_package_repopkgcmp (gconstpointer a, gconstpointer b)
{
  // Sort by repo first, then by package
  // so we only need to add repo keys the first time we see a repo
  // in the package list
  DnfPackage **apkg = (DnfPackage**)a;
  DnfPackage **bpkg = (DnfPackage**)b;
  const gchar * areponame = dnf_package_get_reponame (*apkg);
  const gchar * breponame = dnf_package_get_reponame (*bpkg);
  int repocmp = strcmp (areponame, breponame);
  if (repocmp == 0)
    {
      return dnf_package_cmp (*apkg, *bpkg);
    }
  return repocmp;
}

static gboolean
dnf_command_download_run (DnfCommand      *cmd,
                          int              argc,
                          char            *argv[],
                          GOptionContext  *opt_ctx,
                          DnfContext      *ctx,
                          GError         **error)
{
  g_auto(GStrv) opt_key = NULL;
  gboolean opt_src = FALSE;
  const GOptionEntry opts[] = {
    { "source", '\0', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &opt_src, "download source packages", NULL },
    { G_OPTION_REMAINING, '\0', 0, G_OPTION_ARG_STRING_ARRAY, &opt_key, NULL, NULL },
    { NULL }
  };
  g_option_context_add_main_entries (opt_ctx, opts, NULL);

  if (!g_option_context_parse (opt_ctx, &argc, &argv, error))
    return FALSE;

  if (opt_key == NULL)
    {
      g_set_error_literal (error,
                           G_IO_ERROR,
                           G_IO_ERROR_FAILED,
                           "Packages are not specified");
      return FALSE;
    }

  DnfRepoLoader * repo_loader = dnf_context_get_repo_loader (ctx);
  // If -source arg was passed, auto-enable the source repositories
  if (opt_src && !dnf_command_download_enablesourcerepos (ctx, repo_loader, error))
    {
      return FALSE;
    }

  DnfState * state = dnf_context_get_state (ctx);
  DnfContextSetupSackFlags sack_flags = DNF_CONTEXT_SETUP_SACK_FLAG_SKIP_RPMDB;
  dnf_context_setup_sack_with_flags (ctx, state, sack_flags, error);
  DnfSack *sack = dnf_context_get_sack (ctx);

  hy_autoquery HyQuery query = hy_query_create (sack);

  if (opt_key)
    {
      hy_query_filter_empty (query);
      for (char **pkey = opt_key; *pkey; ++pkey)
        {
          g_auto(HySubject) subject = hy_subject_create (*pkey);
          HyNevra out_nevra;
          hy_autoquery HyQuery key_query =
            hy_subject_get_best_solution (subject, sack, NULL, &out_nevra,
                                          FALSE, TRUE, TRUE, TRUE, opt_src);
          if (out_nevra)
            {
              hy_nevra_free (out_nevra);
            }
          hy_query_filter_num (key_query, HY_PKG_LATEST_PER_ARCH_BY_PRIORITY, HY_EQ, 1);
          hy_query_union (query, key_query);
        }
    }
  if (opt_src)
    {
      hy_query_filter (query, HY_PKG_ARCH, HY_EQ, "src");
    }

  g_autoptr(GPtrArray) pkgs = hy_query_run (query);

  if (pkgs->len == 0)
    {
      g_print ("No packages matched.\n");
      return FALSE;
    }

  g_ptr_array_sort (pkgs, gptrarr_dnf_package_repopkgcmp);
  rpmKeyring keyring = rpmKeyringNew ();
  GError *error_local = NULL;
  gchar prev_repo[MAXPATHLEN];
  prev_repo[0] = '\0';
  for (guint i = 0 ; i < pkgs->len; ++i)
    {
      DnfPackage * pkg = g_ptr_array_index (pkgs, i);
      const gchar * pkg_nevra = dnf_package_get_nevra (pkg);

      const gchar * reponame = dnf_package_get_reponame (pkg);
      if (strlen (reponame) >= MAXPATHLEN)
        {
          g_error ("Reponame exceeds max path len (%s)\n", reponame);
          return FALSE;
        }
      DnfRepo * repo = dnf_repo_loader_get_repo_by_id (repo_loader, reponame, error);
      if (*error != 0)
        {
          g_print ("Failed to find repo(%s)\n", reponame);
          return FALSE;
        }
      // Add repo keys to keyring if this is the first time we see the repo
      if (prev_repo[0] == '\0' || strcmp (prev_repo, reponame) != 0)
        {
          g_auto(GStrv) pubkeys = dnf_repo_get_public_keys (repo);
          if (*pubkeys)
            {
              for (char **iter = pubkeys; iter && *iter; iter++)
                {
                  const char *pubkey = *iter;
                  if (!dnf_keyring_add_public_key (keyring, pubkey, error))
                    {
                      return FALSE;
                    }
                }
            }
          strcpy (prev_repo, reponame);
        }
      dnf_package_set_repo (pkg, repo);
      const gchar *download_path = dnf_package_download (pkg, "./", state, error);
      if (*error != 0)
        {
          g_print ("Failed to download %s\n", pkg_nevra);
          return FALSE;
        }

      g_print ("Downloaded %s\n", pkg_nevra);

      // Check signature (if set on repo)
      if (dnf_repo_get_gpgcheck (repo) &&
          !dnf_keyring_check_untrusted_file (keyring, download_path, &error_local))
        {
          if (!g_error_matches (error_local, DNF_ERROR, DNF_ERROR_GPG_SIGNATURE_INVALID))
            {
              g_set_error (error,
                           DNF_ERROR,
                           DNF_ERROR_FILE_INVALID,
                           "keyring check failure on %1$s "
                           "and repo %2$s is GPG enabled: %3$s",
                           dnf_package_get_nevra (pkg),
                           dnf_repo_get_id (repo),
                           error_local->message);
              g_error_free (error_local);
              return FALSE;
            }
          g_set_error (error,
                       DNF_ERROR,
                       DNF_ERROR_FILE_INVALID,
                       "package %1$s cannot be verified "
                       "and repo %2$s is GPG enabled: %3$s",
                       dnf_package_get_nevra (pkg),
                       dnf_repo_get_id (repo),
                       error_local->message);
          g_error_free (error_local);
          return FALSE;
        }
    }

  g_print ("Complete.\n");

  return TRUE;
}

static void
dnf_command_download_class_init (DnfCommandDownloadClass *klass)
{
}

static void
dnf_command_download_iface_init (DnfCommandInterface *iface)
{
  iface->run = dnf_command_download_run;
}

static void
dnf_command_download_class_finalize (DnfCommandDownloadClass *klass)
{
}

G_MODULE_EXPORT void
dnf_command_download_register_types (PeasObjectModule *module)
{
  dnf_command_download_register_type (G_TYPE_MODULE (module));

  peas_object_module_register_extension_type (module,
                                              DNF_TYPE_COMMAND,
                                              DNF_TYPE_COMMAND_DOWNLOAD);
}