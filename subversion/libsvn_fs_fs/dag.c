/* dag.c : DAG-like interface filesystem, private to libsvn_fs
 *
 * ====================================================================
 * Copyright (c) 2000-2004 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 *
 * This software consists of voluntary contributions made by many
 * individuals.  For exact contribution history, see the revision
 * history and logs, available at http://subversion.tigris.org/.
 * ====================================================================
 */

#include <string.h>
#include <assert.h>

#include "svn_pools.h"
#include "svn_path.h"
#include "svn_time.h"
#include "svn_error.h"
#include "svn_md5.h"
#include "svn_fs.h"

#include "dag.h"
#include "err.h"
#include "fs.h"
#include "key-gen.h"
#include "node-rev.h"
#include "id.h"
#include "revs-txns.h"
#include "fs_fs.h"


/* Initializing a filesystem.  */

struct dag_node_t
{
  /* The filesystem this dag node came from. */
  svn_fs_t *fs;

  /* The pool in which this dag_node_t was allocated.  Unlike
     filesystem and root pools, this is not a private pool for this
     structure!  The caller may have allocated other objects of their
     own in it.  */
  apr_pool_t *pool;

  /* The node revision ID for this dag node, allocated in POOL.  */
  svn_fs_id_t *id;

  /* The node's type (file, dir, etc.) */
  svn_node_kind_t kind;

  /* The node's NODE-REVISION, or NULL if we haven't read it in yet.
     This is allocated in this node's POOL.

     If you're willing to respect all the rules above, you can munge
     this yourself, but you're probably better off just calling
     `get_node_revision' and `set_node_revision', which take care of
     things for you.  */
  svn_fs__node_revision_t *node_revision;

  /* the path at which this node was created. */
  const char *created_path;
};



/* Trivial helper/accessor functions. */
svn_node_kind_t svn_fs__dag_node_kind (dag_node_t *node)
{
  return node->kind;
}


const svn_fs_id_t *
svn_fs__dag_get_id (dag_node_t *node)
{
  return node->id;
}


const char *
svn_fs__dag_get_created_path (dag_node_t *node)
{
  return node->created_path;
}


svn_fs_t *
svn_fs__dag_get_fs (dag_node_t *node)
{
  return node->fs;
}


/* Dup NODEREV and all associated data into POOL */
static svn_fs__node_revision_t *
copy_node_revision (svn_fs__node_revision_t *noderev,
                    apr_pool_t *pool)
{
  svn_fs__node_revision_t *nr = apr_pcalloc (pool, sizeof (*nr));
  nr->kind = noderev->kind;
  if (noderev->predecessor_id)
    nr->predecessor_id = svn_fs__id_copy (noderev->predecessor_id, pool);
  nr->predecessor_count = noderev->predecessor_count;
  if (noderev->copyfrom_path)
    nr->copyfrom_path = apr_pstrdup (pool, noderev->copyfrom_path);
  nr->copyfrom_rev = noderev->copyfrom_rev;
  if (noderev->copyroot)
    nr->copyroot = svn_fs__id_copy (noderev->copyroot, pool);
  nr->predecessor_count = noderev->predecessor_count;
  nr->data_rep = svn_fs__fs_rep_copy (noderev->data_rep, pool);
  nr->prop_rep = svn_fs__fs_rep_copy (noderev->prop_rep, pool);
  
  if (noderev->edit_key)
    nr->edit_key = apr_pstrdup (pool, noderev->edit_key);
  if (noderev->created_path)
    nr->created_path = apr_pstrdup (pool, noderev->created_path);
  return nr;
}


/* Set *NODEREV_P to the cached node-revision for NODE.  If NODE is
   immutable, the node-revision is allocated in NODE->pool.  If NODE
   is mutable, the node-revision is allocated in POOL.

   If you plan to change the contents of NODE, be careful!  We're
   handing you a pointer directly to our cached node-revision, not
   your own copy.  If you change it as part of some operation, but
   then some Berkeley DB function deadlocks or gets an error, you'll
   need to back out your changes, or else the cache will reflect
   changes that never got committed.  It's probably best not to change
   the structure at all.  */
static svn_error_t *
get_node_revision (svn_fs__node_revision_t **noderev_p,
                   dag_node_t *node,
                   apr_pool_t *pool)
{
  svn_fs__node_revision_t *noderev;

  /* If we've already got a copy, there's no need to read it in.  */
  if (! node->node_revision)
    {
      SVN_ERR (svn_fs__fs_get_node_revision (&noderev, node->fs,
                                             node->id, pool));
      node->node_revision = noderev;
    }
          
  /* Now NODE->node_revision is set.  */
  *noderev_p = node->node_revision;
  return SVN_NO_ERROR;
}


svn_boolean_t svn_fs__dag_check_mutable (dag_node_t *node, 
                                         const char *txn_id)
{
  return (svn_fs__id_txn_id (svn_fs__dag_get_id (node)) != NULL);
}


svn_error_t *
svn_fs__dag_get_node (dag_node_t **node,
                      svn_fs_t *fs,
                      const svn_fs_id_t *id,
                      apr_pool_t *pool)
{
  dag_node_t *new_node;
  svn_fs__node_revision_t *noderev;

  /* Construct the node. */
  new_node = apr_pcalloc (pool, sizeof (*new_node));
  new_node->fs = fs;
  new_node->id = svn_fs__id_copy (id, pool); 
  new_node->pool = pool;

  /* Grab the contents so we can inspect the node's kind and created path. */
  SVN_ERR (get_node_revision (&noderev, new_node, pool));

  /* Initialize the KIND and CREATED_PATH attributes */
  new_node->kind = noderev->kind;
  new_node->created_path = apr_pstrdup (pool, noderev->created_path);

  /* Return a fresh new node */
  *node = new_node;
  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__dag_get_revision (svn_revnum_t *rev,
                          dag_node_t *node,
                          apr_pool_t *pool)
{
  /* Look up the committed revision from the Node-ID. */
  *rev = svn_fs__id_rev (node->id);

  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__dag_get_predecessor_id (const svn_fs_id_t **id_p,
                                dag_node_t *node,
                                apr_pool_t *pool)
{
  svn_fs__node_revision_t *noderev;
  
  SVN_ERR (get_node_revision (&noderev, node, pool));
  *id_p = noderev->predecessor_id;
  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__dag_get_predecessor_count (int *count,
                                   dag_node_t *node,
                                   apr_pool_t *pool)
{
  svn_fs__node_revision_t *noderev;
  
  SVN_ERR (get_node_revision (&noderev, node, pool));
  *count = noderev->predecessor_count;
  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__dag_walk_predecessors (dag_node_t *node,
                               svn_fs__dag_pred_func_t callback,
                               void *baton,
                               apr_pool_t *pool)
{
  svn_fs_t *fs = svn_fs__dag_get_fs (node);
  dag_node_t *this_node;
  svn_boolean_t done = FALSE;

  this_node = node;
  while ((! done) && this_node)
    {
      svn_fs__node_revision_t *noderev;

      /* Get the node revision for THIS_NODE so we can examine its
         predecessor id.  */
      SVN_ERR (get_node_revision (&noderev, this_node, pool));

      /* If THIS_NODE has a predecessor, replace THIS_NODE with the
         precessor, else set it to NULL.  */
      if (noderev->predecessor_id)
        SVN_ERR (svn_fs__dag_get_node (&this_node, fs, 
                                       noderev->predecessor_id, pool));
      else
        this_node = NULL;

      /* Now call the user-supplied callback with our predecessor
         node. */
      if (callback)
        SVN_ERR (callback (baton, this_node, &done, pool));
    }

  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__dag_init_fs (svn_fs_t *fs)
{
  abort ();
}



/*** Directory node functions ***/

/* Some of these are helpers for functions outside this section. */

/* Given directory NODEREV in FS, set *ENTRIES_P to its entries list
   hash, or to NULL if NODEREV has no entries.  The entries list will
   be allocated in POOL, and the entries in that list will not have
   interesting value in their 'kind' fields.  If NODEREV is not a
   directory, return the error SVN_ERR_FS_NOT_DIRECTORY. */
static svn_error_t *
get_dir_entries (apr_hash_t **entries_p,
                 svn_fs_t *fs,
                 svn_fs__node_revision_t *noderev,
                 apr_pool_t *pool)
{
  apr_hash_t *entries;

  if (noderev->kind != svn_node_dir )
    return svn_error_create
      (SVN_ERR_FS_NOT_DIRECTORY, NULL,
       "Attempted to create entry in non-directory parent");

  SVN_ERR (svn_fs__fs_rep_contents_dir (&entries, fs,
                                        noderev, pool));

  *entries_p = entries;

  /* Return our findings. */
  return SVN_NO_ERROR;
}


/* Set *ID_P to the node-id for entry NAME in PARENT.  If no such
   entry, set *ID_P to NULL but do not error.  The entry is allocated
   in POOL or in the same pool as PARENT; the caller should copy if it
   cares.  */
static svn_error_t *
dir_entry_id_from_node (const svn_fs_id_t **id_p, 
                        dag_node_t *parent,
                        const char *name,
                        apr_pool_t *pool)
{
  apr_hash_t *entries;
  svn_fs_dirent_t *dirent;

  SVN_ERR (svn_fs__dag_dir_entries (&entries, parent, pool));
  if (entries)
    dirent = apr_hash_get (entries, name, APR_HASH_KEY_STRING);
  else
    dirent = NULL;
    
  *id_p = dirent ? dirent->id : NULL;
  return SVN_NO_ERROR;
}


/* Add or set in PARENT a directory entry NAME pointing to ID.
   Allocations are done in POOL.

   Assumptions:
   - PARENT is a mutable directory.
   - ID does not refer to an ancestor of parent
   - NAME is a single path component
*/
static svn_error_t *
set_entry (dag_node_t *parent,
           const char *name,
           const svn_fs_id_t *id,
           svn_node_kind_t kind,
           const char *txn_id,
           apr_pool_t *pool)
{
  svn_fs__node_revision_t *parent_noderev;

  /* Get the parent's node-revision. */
  SVN_ERR (get_node_revision (&parent_noderev, parent, pool));

  /* Set the new entry. */
  SVN_ERR (svn_fs__fs_set_entry (parent->fs, txn_id, parent_noderev, name, id,
                                 kind, pool));
  
  return SVN_NO_ERROR;
}


/* Make a new entry named NAME in PARENT.  If IS_DIR is true, then the
   node revision the new entry points to will be a directory, else it
   will be a file.  The new node will be allocated in POOL.  PARENT
   must be mutable, and must not have an entry named NAME.  */
static svn_error_t *
make_entry (dag_node_t **child_p,
            dag_node_t *parent,
            const char *parent_path,
            const char *name,
            svn_boolean_t is_dir,
            const char *txn_id,
            apr_pool_t *pool)
{
  const svn_fs_id_t *new_node_id;
  svn_fs__node_revision_t new_noderev;

  /* Make sure that NAME is a single path component. */
  if (! svn_path_is_single_path_component (name))
    return svn_error_createf
      (SVN_ERR_FS_NOT_SINGLE_PATH_COMPONENT, NULL,
       "Attempted to create a node with an illegal name '%s'", name);

  /* Make sure that parent is a directory */
  if (parent->kind != svn_node_dir)
    return svn_error_create
      (SVN_ERR_FS_NOT_DIRECTORY, NULL,
       "Attempted to create entry in non-directory parent");

  /* Check that the parent is mutable. */
  if (! svn_fs__dag_check_mutable (parent, txn_id))
    return svn_error_createf
      (SVN_ERR_FS_NOT_MUTABLE, NULL,
       "Attempted to clone child of non-mutable node");

  /* Check that parent does not already have an entry named NAME. */
  SVN_ERR (dir_entry_id_from_node (&new_node_id, parent, name, pool));
  if (new_node_id)
    return svn_error_createf
      (SVN_ERR_FS_ALREADY_EXISTS, NULL,
       "Attempted to create entry that already exists");

  /* Create the new node's NODE-REVISION */
  memset (&new_noderev, 0, sizeof (new_noderev));
  new_noderev.kind = is_dir ? svn_node_dir : svn_node_file;
  new_noderev.created_path = svn_path_join (parent_path, name, pool);
  SVN_ERR (svn_fs__fs_create_node
           (&new_node_id, svn_fs__dag_get_fs (parent),
            &new_noderev, svn_fs__id_copy_id (svn_fs__dag_get_id (parent)),
            txn_id, pool));

  /* Create a new dag_node_t for our new node */
  SVN_ERR (svn_fs__dag_get_node (child_p, svn_fs__dag_get_fs (parent),
                                 new_node_id, pool));

  /* We can safely call set_entry because we already know that
          PARENT is mutable, and we just created CHILD, so we know it has
          no ancestors (therefore, PARENT cannot be an ancestor of CHILD) */
  SVN_ERR (set_entry (parent, name, svn_fs__dag_get_id (*child_p),
                      new_noderev.kind, txn_id, pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__dag_dir_entries (apr_hash_t **entries,
                         dag_node_t *node,
                         apr_pool_t *pool)
{
  svn_fs__node_revision_t *noderev;

  SVN_ERR (get_node_revision (&noderev, node, pool));
  return get_dir_entries (entries, svn_fs__dag_get_fs (node), noderev, pool);
}


svn_error_t *
svn_fs__dag_set_entry (dag_node_t *node,
                       const char *entry_name,
                       const svn_fs_id_t *id,
                       const char *txn_id,
                       apr_pool_t *pool)
{
  /* Check it's a directory. */
  if (node->kind != svn_node_dir)
    return svn_error_create
      (SVN_ERR_FS_NOT_DIRECTORY, NULL,
       "Attempted to set entry in non-directory node");
  
  /* Check it's mutable. */
  if (! svn_fs__dag_check_mutable (node, txn_id))
    return svn_error_create
      (SVN_ERR_FS_NOT_DIRECTORY, NULL,
       "Attempted to set entry in immutable node");

  return set_entry (node, entry_name, id, node->kind, txn_id, pool);
}



/*** Proplists. ***/

svn_error_t *
svn_fs__dag_get_proplist (apr_hash_t **proplist_p,
                          dag_node_t *node,
                          apr_pool_t *pool)
{
  svn_fs__node_revision_t *noderev;
  apr_hash_t *proplist = NULL;

  SVN_ERR (get_node_revision (&noderev, node, pool));

  SVN_ERR (svn_fs__fs_get_proplist (&proplist, node->fs,
                                    noderev, pool));

  *proplist_p = proplist;
  
  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__dag_set_proplist (dag_node_t *node,
                          apr_hash_t *proplist,
                          const char *txn_id,
                          apr_pool_t *pool)
{
  abort ();

  return SVN_NO_ERROR;
}



/*** Roots. ***/

svn_error_t *
svn_fs__dag_revision_root (dag_node_t **node_p,
                           svn_fs_t *fs,
                           svn_revnum_t rev,
                           apr_pool_t *pool)
{
  svn_fs_id_t *root_id;

  SVN_ERR (svn_fs__fs_rev_get_root (&root_id, fs, rev, pool));
  return svn_fs__dag_get_node (node_p, fs, root_id, pool);
}


svn_error_t *
svn_fs__dag_txn_root (dag_node_t **node_p,
                      svn_fs_t *fs,
                      const char *txn_id,
                      apr_pool_t *pool)
{
  const svn_fs_id_t *root_id, *ignored;
  
  SVN_ERR (svn_fs__get_txn_ids (&root_id, &ignored, fs, txn_id, pool));
  return svn_fs__dag_get_node (node_p, fs, root_id, pool);
}


svn_error_t *
svn_fs__dag_txn_base_root (dag_node_t **node_p,
                           svn_fs_t *fs,
                           const char *txn_id,
                           apr_pool_t *pool)
{
  const svn_fs_id_t *base_root_id, *ignored;
  
  SVN_ERR (svn_fs__get_txn_ids (&ignored, &base_root_id, fs, txn_id, pool));
  return svn_fs__dag_get_node (node_p, fs, base_root_id, pool);
}


svn_error_t *
svn_fs__dag_clone_child (dag_node_t **child_p,
                         dag_node_t *parent,
                         const char *parent_path,
                         const char *name,
                         const char *copy_id,
                         const char *txn_id,
                         apr_pool_t *pool)
{
  dag_node_t *cur_entry; /* parent's current entry named NAME */
  const svn_fs_id_t *new_node_id; /* node id we'll put into NEW_NODE */
  svn_fs_t *fs = svn_fs__dag_get_fs (parent);

  /* First check that the parent is mutable. */
  if (! svn_fs__dag_check_mutable (parent, txn_id))
    return svn_error_createf 
      (SVN_ERR_FS_NOT_MUTABLE, NULL,
       "Attempted to clone child of non-mutable node");

  /* Make sure that NAME is a single path component. */
  if (! svn_path_is_single_path_component (name))
    return svn_error_createf 
      (SVN_ERR_FS_NOT_SINGLE_PATH_COMPONENT, NULL,
       "Attempted to make a child clone with an illegal name '%s'", name);

  /* Find the node named NAME in PARENT's entries list if it exists. */
  SVN_ERR (svn_fs__dag_open (&cur_entry, parent, name, pool));

  /* Check for mutability in the node we found.  If it's mutable, we
     don't need to clone it. */
  if (svn_fs__dag_check_mutable (cur_entry, txn_id))
    {
      /* This has already been cloned */
      new_node_id = cur_entry->id;
    }
  else
    {
      svn_fs__node_revision_t *noderev;
      
      /* Go get a fresh NODE-REVISION for current child node. */
      SVN_ERR (get_node_revision (&noderev, cur_entry, pool));
      
      /* Do the clone thingy here. */
      noderev->predecessor_id = svn_fs__id_copy (cur_entry->id, pool);
      if (noderev->predecessor_count != -1)
        noderev->predecessor_count++;
      noderev->created_path = svn_path_join (parent_path, name, pool);
      SVN_ERR (svn_fs__fs_create_successor (&new_node_id, fs, cur_entry->id, 
                                            noderev, copy_id, txn_id, pool));
      
      /* Replace the ID in the parent's ENTRY list with the ID which
         refers to the mutable clone of this child. */
      SVN_ERR (set_entry (parent, name, new_node_id, noderev->kind, txn_id,
                          pool));
    }

  /* Initialize the youngster. */
  return svn_fs__dag_get_node (child_p, fs, new_node_id, pool);
}



svn_error_t *
svn_fs__dag_clone_root (dag_node_t **root_p,
                        svn_fs_t *fs,
                        const char *txn_id,
                        apr_pool_t *pool)
{
  const svn_fs_id_t *base_root_id, *root_id;
  
  /* Get the node ID's of the root directories of the transaction and
     its base revision.  */
  SVN_ERR (svn_fs__get_txn_ids (&root_id, &base_root_id, fs, txn_id, pool));

  /* Oh, give me a clone...
     (If they're the same, we haven't cloned the transaction's root
     directory yet.)  */
  if (svn_fs__id_eq (root_id, base_root_id)) 
    {
      abort ();
    }

  /* One way or another, root_id now identifies a cloned root node. */
  SVN_ERR (svn_fs__dag_get_node (root_p, fs, root_id, pool));

  /*
   * (Sung to the tune of "Home, Home on the Range", with thanks to
   * Randall Garrett and Isaac Asimov.)
   */

  return SVN_NO_ERROR;
}


/* Delete the directory entry named NAME from PARENT, allocating from
   POOL.  PARENT must be mutable.  NAME must be a single path
   component.  If REQUIRE_EMPTY is true and the node being deleted is
   a directory, it must be empty.

   If return SVN_ERR_FS_NO_SUCH_ENTRY, then there is no entry NAME in
   PARENT.  */
svn_error_t *
svn_fs__dag_delete (dag_node_t *parent,
                    const char *name,
                    const char *txn_id,
                    apr_pool_t *pool)
{
  abort ();
    
  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__dag_remove_node (svn_fs_t *fs,
                         const svn_fs_id_t *id,
                         const char *txn_id,
                         apr_pool_t *pool)
{
  abort ();
  
  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__dag_delete_if_mutable (svn_fs_t *fs,
                               const svn_fs_id_t *id,
                               const char *txn_id,
                               apr_pool_t *pool)
{
  abort ();
  
  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__dag_make_file (dag_node_t **child_p,
                       dag_node_t *parent,
                       const char *parent_path,
                       const char *name,
                       const char *txn_id, 
                       apr_pool_t *pool)
{
  /* Call our little helper function */
  return make_entry (child_p, parent, parent_path, name, FALSE, txn_id, pool);
}


svn_error_t *
svn_fs__dag_make_dir (dag_node_t **child_p,
                      dag_node_t *parent,
                      const char *parent_path,
                      const char *name,
                      const char *txn_id, 
                      apr_pool_t *pool)
{
  /* Call our little helper function */
  return make_entry (child_p, parent, parent_path, name, TRUE, txn_id, pool);
}


svn_error_t *
svn_fs__dag_get_contents (svn_stream_t **contents_p,
                          dag_node_t *file,
                          apr_pool_t *pool)
{ 
  svn_fs__node_revision_t *noderev;
  svn_stream_t *contents;

  /* Make sure our node is a file. */
  if (file->kind != svn_node_file)
    return svn_error_createf 
      (SVN_ERR_FS_NOT_FILE, NULL,
       "Attempted to get textual contents of a *non*-file node");
  
  /* Go get a fresh node-revision for FILE. */
  SVN_ERR (get_node_revision (&noderev, file, pool));

  /* Get a stream to the contents. */
  SVN_ERR (svn_fs__fs_get_contents (&contents, file->fs,
                                    noderev, pool));

  *contents_p = contents;

  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__dag_file_length (svn_filesize_t *length,
                         dag_node_t *file,
                         apr_pool_t *pool)
{ 
  svn_fs__node_revision_t *noderev;

  /* Make sure our node is a file. */
  if (file->kind != svn_node_file)
    return svn_error_createf 
      (SVN_ERR_FS_NOT_FILE, NULL,
       "Attempted to get length of a *non*-file node");

  /* Go get a fresh node-revision for FILE, and . */
  SVN_ERR (get_node_revision (&noderev, file, pool));

  SVN_ERR (svn_fs__fs_file_length (length, noderev, pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__dag_file_checksum (unsigned char digest[],
                           dag_node_t *file,
                           apr_pool_t *pool)
{ 
  svn_fs__node_revision_t *noderev;

  if (file->kind != svn_node_file)
    return svn_error_createf 
      (SVN_ERR_FS_NOT_FILE, NULL,
       "Attempted to get checksum of a *non*-file node");

  SVN_ERR (get_node_revision (&noderev, file, pool));

  SVN_ERR (svn_fs__fs_file_checksum (digest, noderev, pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__dag_get_edit_stream (svn_stream_t **contents,
                             dag_node_t *file,
                             const char *txn_id,
                             apr_pool_t *pool)
{
  svn_fs__node_revision_t *noderev;
  svn_stream_t *ws;

  /* Make sure our node is a file. */
  if (file->kind != svn_node_file)
    return svn_error_createf 
      (SVN_ERR_FS_NOT_FILE, NULL,
       "Attempted to set textual contents of a *non*-file node");
  
  /* Make sure our node is mutable. */
  if (! svn_fs__dag_check_mutable (file, txn_id))
    return svn_error_createf 
      (SVN_ERR_FS_NOT_MUTABLE, NULL,
       "Attempted to set textual contents of an immutable node");

  /* Get the node revision. */
  SVN_ERR (get_node_revision (&noderev, file, pool));

  SVN_ERR (svn_fs__fs_set_contents (&ws, file->fs, noderev, pool));

  *contents = ws;
  
  return SVN_NO_ERROR;
}



svn_error_t *
svn_fs__dag_finalize_edits (dag_node_t *file,
                            const char *checksum,
                            const char *txn_id, 
                            apr_pool_t *pool)
{
  svn_fs__node_revision_t *noderev;
  
  /* Make sure our node is a file. */
  if (file->kind != svn_node_file)
    return svn_error_createf 
      (SVN_ERR_FS_NOT_FILE, NULL,
       "Attempted to set textual contents of a *non*-file node");
  
  /* Make sure our node is mutable. */
  if (! svn_fs__dag_check_mutable (file, txn_id))
    return svn_error_createf 
      (SVN_ERR_FS_NOT_MUTABLE, NULL,
       "Attempted to set textual contents of an immutable node");

  /* Get the node revision. */
  SVN_ERR (get_node_revision (&noderev, file, pool));

  /* If this node has no EDIT-DATA-KEY, this is a no-op. */
  if (! noderev->edit_key)
    return SVN_NO_ERROR;

  abort ();
  
  return SVN_NO_ERROR;
}


dag_node_t *
svn_fs__dag_dup (dag_node_t *node,
                 apr_pool_t *pool)
{
  /* Allocate our new node. */
  dag_node_t *new_node = apr_pcalloc (pool, sizeof (*new_node));

  new_node->fs = node->fs;
  new_node->pool = pool;
  new_node->id = svn_fs__id_copy (node->id, pool);
  new_node->kind = node->kind;
  new_node->created_path = apr_pstrdup (pool, node->created_path);

  /* Leave new_node->node_revision zero for now, so it'll get read in.
     We can get fancy and duplicate node's cache later.  */

  return new_node;
}


svn_error_t *
svn_fs__dag_open (dag_node_t **child_p,
                  dag_node_t *parent,
                  const char *name,
                  apr_pool_t *pool)
{
  const svn_fs_id_t *node_id;

  /* Ensure that NAME exists in PARENT's entry list. */
  SVN_ERR (dir_entry_id_from_node (&node_id, parent, name, pool));
  if (! node_id)
    return svn_error_createf 
      (SVN_ERR_FS_NOT_FOUND, NULL,
       "Attempted to open non-existant child node '%s'", name);
  
  /* Make sure that NAME is a single path component. */
  if (! svn_path_is_single_path_component (name))
    return svn_error_createf 
      (SVN_ERR_FS_NOT_SINGLE_PATH_COMPONENT, NULL,
       "Attempted to open node with an illegal name '%s'", name);

  /* Now get the node that was requested. */
  return svn_fs__dag_get_node (child_p, svn_fs__dag_get_fs (parent),
                               node_id, pool);
}


svn_error_t *
svn_fs__dag_copy (dag_node_t *to_node,
                  const char *entry,
                  dag_node_t *from_node,
                  svn_boolean_t preserve_history,
                  svn_revnum_t from_rev,
                  const char *from_path,
                  const char *txn_id, 
                  apr_pool_t *pool)
{
  const svn_fs_id_t *id;
  
  if (preserve_history)
    {
      svn_fs__node_revision_t *from_noderev, *to_noderev;

      /* Make a copy of the original node revision. */
      SVN_ERR (get_node_revision (&from_noderev, from_node, pool));
      to_noderev = copy_node_revision (from_noderev, pool);

      abort ();
    }
  else  /* don't preserve history */
    {
      id = svn_fs__dag_get_id (from_node);
    }
      
  /* Set the entry in to_node to the new id. */
  SVN_ERR (svn_fs__dag_set_entry (to_node, entry, id, txn_id, pool));

  return SVN_NO_ERROR;
}



/*** Deltification ***/

svn_error_t *
svn_fs__dag_deltify (dag_node_t *target,
                     dag_node_t *source,
                     svn_boolean_t props_only,
                     apr_pool_t *pool)
{
  abort ();

  return SVN_NO_ERROR;
}




/*** Committing ***/

svn_error_t *
svn_fs__dag_commit_txn (svn_revnum_t *new_rev,
                        svn_fs_t *fs,
                        const char *txn_id,
                        apr_pool_t *pool)
{
  abort ();

  return SVN_NO_ERROR;
}



/*** Comparison. ***/

svn_error_t *
svn_fs__things_different (svn_boolean_t *props_changed,
                          svn_boolean_t *contents_changed,
                          dag_node_t *node1,
                          dag_node_t *node2,
                          apr_pool_t *pool)
{
  svn_fs__node_revision_t *noderev1, *noderev2;

  /* If we have no place to store our results, don't bother doing
     anything. */
  if (! props_changed && ! contents_changed)
    return SVN_NO_ERROR;

  /* The the node revision skels for these two nodes. */
  SVN_ERR (get_node_revision (&noderev1, node1, pool));
  SVN_ERR (get_node_revision (&noderev2, node2, pool));

  /* Compare property keys. */
  if (props_changed != NULL)
    *props_changed = (! svn_fs__fs_noderev_same_rep_key (noderev1->prop_rep,
                                                         noderev2->prop_rep));

  /* Compare contents keys. */
  if (contents_changed != NULL)
    *contents_changed = 
      (! svn_fs__fs_noderev_same_rep_key (noderev1->data_rep,
                                          noderev2->data_rep));
  
  return SVN_NO_ERROR;
}



struct is_ancestor_baton
{
  const svn_fs_id_t *node1_id;
  svn_boolean_t is_ancestor;
  svn_boolean_t need_parent; /* TRUE if we only care about parenthood, not
                                full ancestry */
};


static svn_error_t *
is_ancestor_callback (void *baton,
                      dag_node_t *node,
                      svn_boolean_t *done,
                      apr_pool_t *pool)
{
  struct is_ancestor_baton *b = baton;

  /* If there is no NODE, then this is the last call, and we didn't
     find an ancestor.  But if there is ... */
  if (node)
    {
      /* ... compare NODE's ID with the ID we're looking for. */
      if (svn_fs__id_eq (b->node1_id, svn_fs__dag_get_id (node)))
        b->is_ancestor = TRUE;

      /* Now, if we only are interested in parenthood, we don't care
         to look any further than this. */
      if (b->need_parent)
        *done = TRUE;
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs__dag_is_ancestor (svn_boolean_t *is_ancestor,
                         dag_node_t *node1,
                         dag_node_t *node2,
                         apr_pool_t *pool)
{
  struct is_ancestor_baton baton;
  const svn_fs_id_t 
    *id1 = svn_fs__dag_get_id (node1),
    *id2 = svn_fs__dag_get_id (node2);

  /* Pessimism. */
  *is_ancestor = FALSE;

  /* Ancestry holds relatedness as a prerequisite. */
  if (! svn_fs_check_related (id1, id2))
    return SVN_NO_ERROR;

  baton.is_ancestor = FALSE;
  baton.need_parent = FALSE;
  baton.node1_id = id1;

  SVN_ERR (svn_fs__dag_walk_predecessors (node2, is_ancestor_callback,
                                          &baton, pool));
  if (baton.is_ancestor)
    *is_ancestor = TRUE;

  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__dag_is_parent (svn_boolean_t *is_parent,
                       dag_node_t *node1,
                       dag_node_t *node2,
                       apr_pool_t *pool)
{
  struct is_ancestor_baton baton;
  const svn_fs_id_t 
    *id1 = svn_fs__dag_get_id (node1),
    *id2 = svn_fs__dag_get_id (node2);

  /* Pessimism. */
  *is_parent = FALSE;

  /* Parentry holds relatedness as a prerequisite. */
  if (! svn_fs_check_related (id1, id2))
    return SVN_NO_ERROR;

  baton.is_ancestor = FALSE;
  baton.need_parent = TRUE;
  baton.node1_id = id1;

  SVN_ERR (svn_fs__dag_walk_predecessors (node2, is_ancestor_callback,
                                          &baton, pool));
  if (baton.is_ancestor)
    *is_parent = TRUE;

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs__dag_get_copyroot (const svn_fs_id_t **id,
                          dag_node_t *node,
                          apr_pool_t *pool)
{
  svn_fs__node_revision_t *noderev;
  
  /* Go get a fresh node-revision for FILE. */
  SVN_ERR (get_node_revision (&noderev, node, pool));

  *id = noderev->copyroot;

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs__dag_get_copyfrom_rev (svn_revnum_t *rev,
                              dag_node_t *node,
                              apr_pool_t *pool)
{
  svn_fs__node_revision_t *noderev;
  
  /* Go get a fresh node-revision for FILE. */
  SVN_ERR (get_node_revision (&noderev, node, pool));

  *rev = noderev->copyfrom_rev;

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs__dag_get_copyfrom_path (const char **path,
                               dag_node_t *node,
                               apr_pool_t *pool)
{
  svn_fs__node_revision_t *noderev;
  
  /* Go get a fresh node-revision for FILE. */
  SVN_ERR (get_node_revision (&noderev, node, pool));

  *path = noderev->copyfrom_path;
  
  return SVN_NO_ERROR;
}

