#!/usr/bin/env python
#
#  xml_tests.py:  testing working-copy interactions with XML files.
#
#  Subversion is a tool for revision control. 
#  See http://subversion.tigris.org for more information.
#    
# ====================================================================
# Copyright (c) 2000-2001 CollabNet.  All rights reserved.
#
# This software is licensed as described in the file COPYING, which
# you should have received as part of this distribution.  The terms
# are also available at http://subversion.tigris.org/license-1.html.
# If newer versions of this license are posted there, you may use a
# newer version instead, at your option.
#
######################################################################

import svn_test_main
import svn_tree
import svn_output

import shutil         # for copytree()
import string         # for strip()

#####  Globals

XML_DIR = '../../xml'
ANCESTOR_PATH = 'anni'


######################################################################
# Utilities shared by these tests

# A checkout from xml, with output verification.
def xml_checkout(wc_dir, xml_path, expected_lines):
  """Checkout the xml file at XML_PATH into WC_DIR, and verify against
  the EXPECTED_LINES given.  Return 0 on success."""

  # Remove dir if it's already there.
  svn_test_main.remove_wc(wc_dir)

  # Checkout from xml and make a tree of the output.
  output = svn_test_main.run_svn ('co', '-d', wc_dir, \
                                  '--xml-file', xml_path, \
                                  '-r 1', ANCESTOR_PATH)
  output_tree = svn_output.tree_from_checkout (output)

  # Make a tree out of the expected lines
  expected_tree = svn_tree.build_generic_tree (expected_lines)

  # Verify actual output against expected output.
  return svn_tree.compare_trees (output_tree, expected_tree)

  # TODO:  someday inspect the entries file too?? 



# A commit to xml, with output verification.
#   Note:  don't include the line 'Commit succeeded' in your expected
#          output;  this function already searches for it.

def xml_commit(wc_dir, xml_path, revision, expected_lines):
  """Commit changes in WC_DIR to an xml file XML_PATH, and verify
  against the EXPECTED_LINES given.  WC_DIR will be bumped to
  REVISION.  Return 0 on success."""
  
  output = svn_test_main.run_svn("ci", "--xml-file", xml_path,
                                 "-r", revision, wc_dir)

  # Remove the final output line, return error if it's not 'Commit succeeded'.
  lastline = string.strip(output.pop())
  if lastline != 'Commit succeeded.':
    return 1

  # Now verify the actual vs. expected output.
  return svn_output.compare_sets(expected_lines, output,
                                 svn_output.line_matches_regexp)

  # TODO:  someday inspect the entries file too?? 


######################################################################
# Tests
#
#   Each test must return 0 on success or non-zero on failure.

#----------------------------------------------------------------------

def xml_test_1():
  "checkout a wc from co1-inline.xml"

  wc_dir = 'wc-t1'

  # Generate the expected output lines from a checkout.
  expected_output = []
  for path in svn_test_main.greek_paths:
    item = [ wc_dir + '/' + path,
             None,
             {'status' : 'A '} ]
    expected_output.append(item)

  return xml_checkout(wc_dir, XML_DIR + '/co1-inline.xml', expected_output)

#----------------------------------------------------------------------

def xml_test_2():
  "xml checkout, make many mods, xml commit, update."

  wc_dir = 'wc-t2'
  wc_dir2 = 'wc-t2-copy'

  # Generate the expected output lines from a checkout (as regexps)
  expected_output = []
  for path in greek_paths:
    line = 'A\s+'  + wc_dir + '/' + path
    expected_output.append(line)

  # Checkout from co1-inline.xml and verify the results.
  result = xml_checkout(wc_dir, XML_DIR + '/co1-inline.xml', expected_output)
  if result:
    return result  # a chance for test failure

  # Make a backup of the working copy for later updates.
  svn_test_main.remove_wc(wc_dir2)
  shutil.copytree(wc_dir, wc_dir2)

  # Modify some existing files.
  svn_test_main.file_append(wc_dir + "/A/D/G/pi", "\n a 2nd line in A/D/G/pi")
  svn_test_main.file_append(wc_dir + "/A/mu", "\n a 2nd line in A/mu")

  # Add some files.
  svn_test_main.file_append(wc_dir + "/newfile1",
                            "This is added file newfile1.")
  add_output = svn_test_main.run_svn("add", wc_dir + "/newfile1")
           ### TODO:  verify that the output was "" (nothing)

  svn_test_main.file_append(wc_dir + "/A/B/E/newfile2",
                            "This is added file newfile2.")
  add_output = svn_test_main.run_svn("add", wc_dir + "/A/B/E/newfile2")
           ### TODO:  verify that the output was "" (nothing)

  # Delete omega with --force.
  del_output = svn_test_main.run_svn("del", "--force", wc_dir + "/A/D/H/omega")
           ### TODO:  verify that the output was "" (nothing)

  # Delete added file 'newfile2' without --force.
  del_output = svn_test_main.run_svn("del", wc_dir + "/A/B/E/newfile2")
           ### TODO:  verify that the output was "" (nothing)

  # Commit to xml file (bumping wc to rev. 2), and verify the output.
  commit_expected_output = ['Deleting\s+' + wc_dir + '/A/D/H/omega',
                            'Adding\s+' + wc_dir + '/newfile1',
                            'Changing\s+' + wc_dir + '/A/mu',
                            'Changing\s+' + wc_dir + '/A/D/G/pi']

  result = xml_commit(wc_dir, 'xmltest2-commit.xml', 2, commit_expected_output)
  if result:
    return result  # a chance for test failure

  return 0 # final success

#-----------------------------------------------------------------------------

def xml_test_3():
  """verify the actual wc from co1-inline.xml - 2"""

  import svn_tree

  wc_dir = 'wc-t3'

  # actually do the check out
  svn_test_main.run_svn ('co', '-d', wc_dir, \
                                  '--xml-file', XML_DIR + '/co1-inline.xml', \
                                  '-r 1', ANCESTOR_PATH)


  exp_tree = svn_tree.build_tree_from_paths(greek_paths)
  result_tree = svn_tree.build_tree_from_wc(wc_dir)

  if svn_tree.compare_trees(exp_tree, result_tree):
    return 0
  else:
    return 1

#----------------------------------------------------------------------------

def xml_test_4():
  """verify that the entries files match the wc"""

  import svn_tree, svn_entry

  wc_dir = 'wc-t4'

  # actually do the checkout
  svn_test_main.run_svn ('co', '-d', wc_dir, \
                                  '--xml-file', XML_DIR + '/co1-inline.xml', \
                                  '-r 1', ANCESTOR_PATH)

  exp_tree = svn_tree.build_tree_from_wc(wc_dir)
  result_tree = svn_tree.build_tree_from_entries(wc_dir)

  if svn_tree.compare_trees(exp_tree, result_tree):
    return 0
  else:
    return 1
  
########################################################################
## List all tests here, starting with None:
test_list = [ None,
              xml_test_1,
              xml_test_2,
              xml_test_3,
              xml_test_4
             ]

if __name__ == '__main__':  
  ## And run the main test routine on them:
  svn_test_main.client_test(test_list)

### End of file.
# local variables:
# eval: (load-file "../../../svn-dev.el")
# end:
