/*
 * status.c: construct a status structure from an entry structure
 *
 * ====================================================================
 * Copyright (c) 2000-2001 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 * ====================================================================
 */



#include <apr_pools.h>
#include <apr_file_io.h>
#include <apr_hash.h>
#include <apr_time.h>
#include "svn_types.h"
#include "svn_string.h"
#include "svn_error.h"
#include "svn_path.h"
#include "svn_wc.h"
#include "wc.h"   



/* Fill in STATUS with ENTRY.
   ENTRY may be null, for non-versioned entities.
   Else, ENTRY's pool must not be shorter-lived than STATUS's, since
   ENTRY will be stored directly, not copied. */
static svn_error_t *
assemble_status (svn_wc_status_t *status,
                 svn_stringbuf_t *path,
                 svn_wc_entry_t *entry,
                 apr_pool_t *pool)
{
  svn_error_t *err;

  svn_boolean_t text_modified_p = FALSE;
  svn_boolean_t prop_modified_p = FALSE;

  /* Copy info from entry struct to status struct */
  status->entry = entry;
  status->repos_rev = SVN_INVALID_REVNUM;  /* caller fills in */
  status->text_status = svn_wc_status_none;       /* default to no status. */
  status->prop_status = svn_wc_status_none;       /* default to no status. */

  if (status->entry)
    {
      svn_stringbuf_t *prop_path;
      enum svn_node_kind prop_kind;
      svn_boolean_t prop_exists = FALSE;
      
      /* Before examining the entry's state, determine if a property
         component exists. */
      err = svn_wc__prop_path (&prop_path, path, 0, pool);
      if (err) return err;      
      err = svn_io_check_path (prop_path, &prop_kind, pool);
      if (err) return err;

      if (prop_kind == svn_node_file)
        prop_exists = TRUE;

      /* Look for local mods, independent of other tests. */
      
      /* If the entry has a property file, see if it has local
         changes. */
      if (prop_exists)
        {
          err = svn_wc_props_modified_p (&prop_modified_p, path, pool);
          if (err) return err;
        }
      
      /* If the entry is a file, check for textual modifications */
      if (entry->kind == svn_node_file)
        {
          err = svn_wc_text_modified_p (&text_modified_p, path, pool);
          if (err) return err;
        }
      
      /* TODO (philosophical).  Does it make sense to talk about a
         directory having "textual" modifications?  I mean, if you
         `svn add' a file to a directory, does the parent dir now
         have local modifications?  Are these modifications
         "textual" in the sense that the "text" of a directory is
         a list of entries, which has now been changed?  And would
         we then show that `M' in the first column?  Ponder,
         ponder.  */
      
      /* Mark `M' in status structure based on tests above. */
      if (text_modified_p)
        status->text_status = svn_wc_status_modified;
      if (prop_modified_p)
        status->prop_status = svn_wc_status_modified;      
      
      if (entry->schedule == svn_wc_schedule_add)
        {
          /* If an entry has been marked for future addition to the
             repository, we *know* it has a textual component: */
          status->text_status = svn_wc_status_added;

          /* However, it may or may not have a property component.  If
             it does, report that portion as "added" too. */
          if (prop_exists)
            status->prop_status = svn_wc_status_added;
        }

      else if (entry->schedule == svn_wc_schedule_replace)
        {
          status->text_status = svn_wc_status_replaced;
          
          if (prop_exists)
            status->prop_status = svn_wc_status_replaced;
        }

      else if (entry->schedule == svn_wc_schedule_delete)
        {
          status->text_status = svn_wc_status_deleted;
          
          if (prop_exists)
            status->prop_status = svn_wc_status_deleted;
        }

      if (entry->conflicted)
        {
          /* We must decide if either component is "conflicted", based
             on whether reject files are mentioned and/or continue to
             exist.  Luckily, we have a function to do this.  :) */
          svn_boolean_t text_conflict_p, prop_conflict_p;
          svn_stringbuf_t *parent_dir;
          
          if (entry->kind == svn_node_file)
            {
              parent_dir = svn_stringbuf_dup (path, pool);
              svn_path_remove_component (parent_dir, svn_path_local_style);
            }
          else if (entry->kind == svn_node_dir)
            parent_dir = path;

          err = svn_wc_conflicted_p (&text_conflict_p,
                                     &prop_conflict_p,
                                     parent_dir,
                                     entry,
                                     pool);
          if (err) return err;
          
          if (text_conflict_p)
            status->text_status = svn_wc_status_conflicted;
          if (prop_conflict_p)
            status->prop_status = svn_wc_status_conflicted;
        }

    }

  return SVN_NO_ERROR;
}


/* Given an ENTRY object representing PATH, build a status structure
   and store it in STATUSHASH.  */
static svn_error_t *
add_status_structure (apr_hash_t *statushash,
                      svn_stringbuf_t *path,
                      svn_wc_entry_t *entry,
                      apr_pool_t *pool)
{
  svn_error_t *err;
  svn_wc_status_t *statstruct = apr_pcalloc (pool, sizeof (*statstruct));

  err = assemble_status (statstruct, path, entry, pool);
  if (err)
    return err;

  apr_hash_set (statushash, path->data, path->len, statstruct);
  
  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc_status (svn_wc_status_t **status,
               svn_stringbuf_t *path,
               apr_pool_t *pool)
{
  svn_error_t *err;
  svn_wc_status_t *s = apr_pcalloc (pool, sizeof (*s));
  svn_wc_entry_t *entry = NULL;

  err = svn_wc_entry (&entry, path, pool);
  if (err)
    return err;

  err = assemble_status (s, path, entry, pool);
  if (err)
    return err;
  
  *status = s;
  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc_statuses (apr_hash_t *statushash,
                 svn_stringbuf_t *path,
                 svn_boolean_t descend,
                 apr_pool_t *pool)
{
  svn_error_t *err;
  enum svn_node_kind kind;
  apr_hash_t *entries;
  svn_wc_entry_t *entry;
  void *value;
  
  /* Is PATH a directory or file? */
  err = svn_io_check_path (path, &kind, pool);
  if (err) return err;
  
  /* kff todo: this has to deal with the case of a type-changing edit,
     i.e., someone removed a file under vc and replaced it with a dir,
     or vice versa.  In such a case, when you ask for the status, you
     should get mostly information about the now-vanished entity, plus
     some information about what happened to it.  The same situation
     is handled in entries.c:svn_wc_entry. */

  /* Read the appropriate entries file */
  
  /* If path points to only one file, return just one status structure
     in the STATUSHASH */
  if (kind == svn_node_file)
    {
      svn_stringbuf_t *dirpath, *basename;

      /* Figure out file's parent dir */
      svn_path_split (path, &dirpath, &basename,
                      svn_path_local_style, pool);      

      /* Load entries file for file's parent */
      err = svn_wc_entries_read (&entries, dirpath, pool);
      if (err) return err;

      /* Get the entry by looking up file's basename */
      value = apr_hash_get (entries, basename->data, basename->len);

      if (value)  
        entry = (svn_wc_entry_t *) value;
      else
        return svn_error_createf (SVN_ERR_BAD_FILENAME, 0, NULL, pool,
                                  "svn_wc_statuses:  bogus path `%s'",
                                  path->data);

      /* Convert the entry into a status structure, store in the hash */
      err = add_status_structure (statushash, path, entry, pool);
      if (err) return err;
    }


  /* Fill the hash with a status structure for *each* entry in PATH */
  else if (kind == svn_node_dir)
    {
      apr_hash_index_t *hi;

      /* Load entries file for the directory */
      err = svn_wc_entries_read (&entries, path, pool);
      if (err) return err;

      /* Loop over entries hash */
      for (hi = apr_hash_first (entries); hi; hi = apr_hash_next (hi))
        {
          const void *key;
          void *val;
          const char *basename;
          apr_size_t keylen;
          svn_stringbuf_t *fullpath = svn_stringbuf_dup (path, pool);

          /* Get the next dirent */
          apr_hash_this (hi, &key, &keylen, &val);
          basename = (const char *) key;
          if (strcmp (basename, SVN_WC_ENTRY_THIS_DIR) != 0)
            {
              svn_path_add_component_nts (fullpath, basename,
                                          svn_path_local_style);
            }

          entry = (svn_wc_entry_t *) val;

          err = svn_io_check_path (fullpath, &kind, pool);
          if (err) return err;

          /* In deciding whether or not to descend, we use the actual
             kind of the entity, not the kind claimed by the entries
             file.  The two are usually the same, but where they are
             not, its usually because some directory got moved, and
             one would still want a status report on its contents.
             kff todo: However, must handle mixed working copies.
             What if the subdir is not under revision control, or is
             from another repository? */
          
          /* Do *not* store THIS_DIR in the statushash, unless this
             path has never been seen before.  We don't want to add
             the path key twice. */
          if (! strcmp (basename, SVN_WC_ENTRY_THIS_DIR))
            {
              svn_wc_status_t *s = apr_hash_get (statushash,
                                                 fullpath->data,
                                                 fullpath->len);
              if (! s)
                SVN_ERR (add_status_structure (statushash, fullpath,
                                               entry, pool));              
            }
          else
            {
              if (kind == svn_node_dir)
                {
                  /* Directory entries are incomplete.  We must get
                     their full entry from their own THIS_DIR entry.
                     svn_wc_entry does this for us if it can.  */
                  svn_wc_entry_t *subdir;

                  SVN_ERR (svn_wc_entry (&subdir, fullpath, pool));
                  SVN_ERR (add_status_structure (statushash, fullpath,
                                                 subdir, pool));
                  if (descend)
                    {
                      /* If ask to descent, we do not contend. */
                      SVN_ERR (svn_wc_statuses (statushash, fullpath,
                                                descend, pool)); 
                    }
                }
              else if (kind == svn_node_file)
                {
                  /* File entries are ... just fine! */
                  SVN_ERR (add_status_structure (statushash, fullpath,
                                                 entry, pool));
                }
            }
        }
    }
  
  return SVN_NO_ERROR;
}



/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end:
 */
