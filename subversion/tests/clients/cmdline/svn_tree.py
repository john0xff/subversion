#!/usr/bin/env python
#
#  svn_tree.py: tools for comparing directory trees
#
#  Subversion is a tool for revision control. 
#  See http://subversion.tigris.org for more information.
#    
# ====================================================================
# Copyright (c) 2001 Sam Tobin-Hochstadt.  All rights reserved.
#
# This software is licensed as described in the file COPYING, which
# you should have received as part of this distribution.  The terms
# are also available at http://subversion.tigris.org/license-1.html.
# If newer versions of this license are posted there, you may use a
# newer version instead, at your option.
#
######################################################################


# A node in a tree.
#
# If CHILDREN is None, then the node is a file.  Otherwise, CHILDREN
# is a list of the nodes making up that directory's children.
#
# NAME is simply the name of the file or directory.  CONTENTS is a
# string that contains the file's contents (if a file), and PROPS is a
# dictionary of other metadata attached to the node.

class SVNTreeNode:

  def __init__(self, name, children=None, contents=None, props={}):
    self.name = name
    self.children = children
    self.contents = contents
    self.props = props

  def add_child(self, newchild):
    if self.children is None:  # if you're a file,
      self.children = []     # become an empty dir.
    n = 0
    for a in self.children:
      if a.name == newchild.name:
        n = 1
        break
    if n == 0:  # append child only if we don't already have it.
      self.children.append(newchild)

    # If you already have the node,
    else:
      if newchild.children is None:
        # this is the 'end' of the chain, so copy any content here.
        a.contents = newchild.contents
        a.props = newchild.props
      else:
        # try to add dangling children to your matching node
        for i in newchild.children:
          a.add_child(i)
      

  def pprint(self):
    print " * Node name: ", self.name
    print "    Contents:  ", self.contents
    print "    Properties:", self.props
    if self.children:
      print "    Children:  ", len(self.children)
    else:
      print "    Children: is a file."

# reserved name of the root of the tree

root_node_name = "__SVN_ROOT_NODE" 

# Exception raised if you screw up in this module.

class SVNTreeError(Exception): pass

# Exception raised if two trees are unequal

class SVNTreeUnequal(Exception): pass


# helper func
def add_elements_as_path(top_node, element_list):
  """Add the elements in ELEMENT_LIST as if they were a single path
  below TOP_NODE."""

  # The idea of this function is to take a list like so:
  # ['A', 'B', 'C'] and a top node, say 'Z', and generate a tree
  # like this:
  #
  #             Z -> A -> B -> C
  #
  # where 1 -> 2 means 2 is a child of 1.
  #

  prev_node = top_node
  for i in element_list:
    new_node = SVNTreeNode(i, None)
    prev_node.add_child(new_node)
    prev_node = new_node


# Sorting function -- sort 2 nodes by their names.
def node_is_greater(a, b):
  "Sort the names of two nodes."
  # Interal use only
  if a.name == b.name:
    return 0
  if a.name > b.name:
    return 1
  else:
    return -1

# Comparison:  are two tree nodes the same?
def compare_nodes(a, b):
  "Compare two nodes' contents, ignoring children.  Return 0 if the same."

  if a.name != b.name:
    return 1
  if a.contents != b.contents:
    return 1
  if a.props != b.props:  ## is it legal to compare hashes like this?!?
    return 1
  
  # We don't need to compare lists of children, since that's being
  # done recursively by compare_trees() -- to which this function is a
  # helper.



###########################################################################
# EXPORTED ROUTINES


# General utility:  change one path into a linked list of nodes.
#
# (Take the output and .add_child() it to a root node.)

def create_from_path(path, contents=None, props={}):
  """Create and return a linked list of treenodes, given a PATH
  representing a single entry into that tree.  CONTENTS and PROPS are
  optional arguments that will be deposited in the tail node."""

  # get a list of all the names in the path
  # each of these will be a child of the former
  elements = path.split("/")
  if len(elements) == 0:
    raise SVNTreeError

  root_node = SVNTreeNode(elements[0], None)

  add_elements_as_path(root_node, elements[1:])

  # deposit contents in the very last node.
  node = root_node
  while 1:
    if node.children is None:
      node.contents = contents
      node.props = props
      break
    node = node.children[0]

  return root_node


# Change a list of paths into an entire tree. (no contents or props)
 
def build_tree_from_paths(paths):
  "Take a list of PATHS, and return an assembled tree of nodes."

  root = SVNTreeNode(root_node_name, None)

  for i in paths:
    root.add_child(create_from_path(i))

  return root


# Main comparison routine!  

def compare_trees(a, b):
  "Return 0 iff two trees are identical."
  
  try:
    if compare_nodes(a, b):
      print "Error: '%s' differs from '%s'." % (a.name, b.name)
      a.pprint()
      b.pprint()
      raise SVNTreeUnequal
    if a.children is None:
      if b.children is None:
        return 0
      else:
        raise SVNTreeUnequal
    if b.children is None:
      raise SVNTreeUnequal
    a.children.sort(node_is_greater)
    b.children.sort(node_is_greater)
    for i in range(max(len(a.children), len(b.children))):
      compare_trees(a.children[i], b.children[i])
  except IndexError:
    print "Error: unequal number of children"
    raise SVNTreeUnequal
  except SVNTreeUnequal:
    if a.name == root_node_name:
      return 1
    else:
      print "Unequal at node %s" % a.name
      raise SVNTreeUnequal
  return 0


# Visually show a tree's structure

def dump_tree(n,indent=""):
  "Print out a nice representation of the tree's structure."

  # Code partially stolen from Dave Beazley
  if n.children is None:
    tmp_children = []
  else:
    tmp_children = n.children

  if n.name == root_node_name:
    print "%s%s" % (indent, "ROOT")
  else:
    print "%s%s" % (indent, n.name)

  indent = indent.replace("-"," ")
  indent = indent.replace("+"," ")
  for i in range(len(tmp_children)):
    c = tmp_children[i]
    if i == len(tmp_children
                )-1:
      dump_tree(c,indent + "  +-- ")
    else:
      dump_tree(c,indent + "  |-- ")


# How to explicitly build an "expected" tree.
#
# Create a list of lists, of the form:
#
#  [ [path, contents, props], ... ]
#
#  and run it through this parser.  PATH is a string, a path to the
#  object.  CONTENTS is either a string or None, and PROPS is a
#  populated dictionary or {}.  Each CONTENTS and PROPS will be
#  attached to the basename-node of the associated PATH.

def build_generic_tree(nodelist):
  "Given a list of lists of a specific format, return a tree."
  
  root = SVNTreeNode(root_node_name)
  
  for list in nodelist:
    new_branch = create_from_path(list[0], list[1], list[2])
    root.add_child(new_branch)

  return root




### End of file.
# local variables:
# eval: (load-file "../../../svn-dev.el")
# end:




