/**
 *  \file PragmaInterface.cc
 *  \brief A source-level, pragma-based interface to the outliner.
 *
 *  This module implements a high-level wrapper that permits a user to
 *  insert "#pragma rose_outline" statements into the body of the
 *  source, thereby directing what to outline at the source-level.
 *
 *  For Fortran codes, using a special comment, !$rose_outline, 
 *  for the same purpose 
 *
 *  \todo Extend this interface.
 */

#include <string>
#include <rose.h>

#include "Outliner.hh"
#include "ASTtools.hh"
#include "PreprocessingInfo.hh"
#include <boost/algorithm/string/trim.hpp>

//! Simplest outlining directives, applied to a single statement.
static const std::string PRAGMA_OUTLINE ("rose_outline");

//! Stores a list of valid outlining pragma directives.
typedef Rose_STL_Container<SgPragmaDeclaration *> PragmaList_t;
//! Stores a list of valid outlining targets, used only for Fortran for now
typedef Rose_STL_Container<SgStatement*> TargetList_t;

// =====================================================================

using namespace std;

// =====================================================================

/*!
 *  \brief Check whether the specified pragma is an outlining
 *  directive.
 *
 *  This routine checks whether the specified pragma is an outlining
 *  directive, and if so, returns the statement that should be
 *  outlined. Returns NULL if the pragma is not an outlining directive
 *  or no such statement exists.
 */
static
SgStatement *
processPragma (SgPragmaDeclaration* decl)
{
  if (!decl || !decl->get_pragma ())
    return 0;

  string pragmaString = decl->get_pragma ()->get_pragma ();
  if (pragmaString != PRAGMA_OUTLINE) // Not an outlining pragma.
    return 0;
    
  // Get statement to outline
  return const_cast<SgStatement *> (ASTtools::findNextStatement (decl));
}

/*!
 *  \brief Check whether the specified comment is an outlining
 *  directive for Fortran: one of the following three cases: 
 *    !$rose_outline
 *    c$rose_outline
 *    *$rose_outline
 *  This routine checks whether the specified source comment an outlining
 *  directive, and if so, returns the statement that should be
 *  outlined. Returns NULL if the comment is not an outlining directive
 *  or no such statement exists.
 *  Liao, 12/19/2008
 */
static
SgStatement *
processFortranComment(SgLocatedNode* node)
{
  SgStatement* target = NULL;
  ROSE_ASSERT(node);
  AttachedPreprocessingInfoType *comments =
    node->getAttachedPreprocessingInfo ();
  if (comments==NULL)
    return 0;
  AttachedPreprocessingInfoType::iterator i;
  std::vector< PreprocessingInfo* > removeList;
  for (i = comments->begin (); i != comments->end (); i++)
  { 
    if ((*i)->getTypeOfDirective() == PreprocessingInfo::FortranStyleComment)
    {
      string commentString = (*i)->getString();
      boost::algorithm::trim(commentString);
      if (   (commentString == "!$"+PRAGMA_OUTLINE) 
          || (commentString == "c$"+PRAGMA_OUTLINE) 
          || (commentString == "*$"+PRAGMA_OUTLINE)) 
      {
        target = isSgStatement(node);
        if (target==NULL)
        {
          cerr<<"Unhandled case when a Fortran statement is attached to a non-statement node!!"<<endl;
          ROSE_ASSERT(false);
        }
        removeList.push_back(*i);
      }
    } // end if Fortran comment
  } // end for-loop

  // remove those special comments
  for (std::vector<PreprocessingInfo* >::iterator j = removeList.begin();
       j!=removeList.end();j++)
  {
    comments->erase(find(comments->begin(), comments->end(),*j));
    // free memory also
    free(*j);
  }

  return target; // const_cast<SgStatement *> (target);
}


/* =====================================================================
 *  Main routine to outline a single statement immediately following
 *  an outline directive (pragma).
 */

Outliner::Result
Outliner::outline (SgPragmaDeclaration* decl)
{
  SgStatement* s = processPragma (decl);
  if (!s)
    return Result ();

  // Generate outlined function, removing 's' from the tree.
  string name = generateFuncName (s);
  Result result = outline (s, name);
  ROSE_ASSERT (result.isValid ());

  // Remove pragma
  ASTtools::moveBeforePreprocInfo (decl, result.call_);
  ASTtools::moveAfterPreprocInfo (decl, result.call_);
  LowLevelRewrite::remove (decl);
  return result;
}

SgBasicBlock *
Outliner::preprocess (SgPragmaDeclaration* decl)
{
  SgStatement* s = processPragma (decl);
  if (s)
    return preprocess (s);
  else
    return 0;
}


// =====================================================================

/*!
 *  \brief Collects all outlining pragmas.
 *
 * This routine scans the given project for all outlining pragmas, and
 * returns them in the order in which they should be processed.
 *
 * The ordering is important because neither preorder nor postorder
 * tree traversals yield the desired bottomup processing for outlining
 * pragmas. To see why, consider the following code example:
 *
 * \code
 * #pragma rose_outline
 * {
 *   #pragma rose_outline
 *   statement1;
 * }
 * \endcode
 *
 * The corresponding AST is:
 *
 *   SgBasicBlock1
 *     /      \
 *    /        \
 * SgPragma1  SgBasicBlock2
 *              /      \
 *          SgPragma2  SgStatement1
 *
 * The standard traversal orders are:
 *
 * - Preorder: bb1, pragma1, bb2, pragma2, stmt1
 * - Postorder: pragma1, pragma2, stmt1,bb2, bb1
 *
 * In both cases, pragma1 is always visited before pragma2.
 *
 * The routine obtains a "correct" ordering by using the default
 * preorder AST query and then reversing the results.  In this we, we
 * obtain the ordering:
 *
 * - stmt1, pragma2, bb2,pragma1, bb1
 */
static size_t
collectPragmas (SgProject* proj, PragmaList_t& pragmas)
{
  typedef Rose_STL_Container<SgNode *> NodeList_t;
  NodeList_t raw_list = NodeQuery::querySubTree (proj, V_SgPragmaDeclaration);
  size_t count = 0;
  for (NodeList_t::reverse_iterator i = raw_list.rbegin ();
       i != raw_list.rend (); ++i)
    {
      SgPragmaDeclaration* decl = isSgPragmaDeclaration (*i);
      if (processPragma (decl))
        {
          pragmas.push_back (decl);
          ++count;
        }
    }
  return count;
}
//! Collect target Fortran statement with matching comments for rose_outline
// Save them into targetList
static size_t
collectFortranTarget (SgProject* proj, TargetList_t& targets)
{
  typedef Rose_STL_Container<SgNode *> NodeList_t;
  NodeList_t raw_list = NodeQuery::querySubTree (proj, V_SgStatement);
  size_t count = 0;
  for (NodeList_t::reverse_iterator i = raw_list.rbegin ();
      i != raw_list.rend (); ++i)
  {
    SgStatement* decl = isSgStatement(*i);
    if (processFortranComment(decl))
    {
      targets.push_back (decl);
      ++count;
    }
  }
  return count;
}

//-------------------top level drivers----------------------------------------
size_t
Outliner::outlineAll (SgProject* project)
{
  size_t num_outlined = 0;
  if (SageInterface::is_Fortran_language ()) 
  { // Search for the special source comments for Fortran input
    TargetList_t targets;
    if (collectFortranTarget(project, targets))
    {
      for (TargetList_t::iterator i = targets.begin ();
          i != targets.end (); ++i)
        if (outline(*i).isValid())
          ++num_outlined;
    }
  } 
  else // Search for pragmas for C/C++
  {
    PragmaList_t pragmas;
    if (collectPragmas (project, pragmas))
    {
      for (PragmaList_t::iterator i = pragmas.begin ();
          i != pragmas.end (); ++i)
        if (outline (*i).isValid ())
          ++num_outlined;
    }
  }
  return num_outlined;
}
//! The driver for preprocessing
size_t
Outliner::preprocessAll (SgProject* proj)
{
  size_t count = 0;
  if (SageInterface::is_Fortran_language ()) 
  { // Search for the special source comments for Fortran input
    TargetList_t targets;
    if (collectFortranTarget(proj, targets))
    {
      for (TargetList_t::iterator i = targets.begin ();
          i != targets.end (); ++i)
        if (preprocess (*i))
          ++count;
    }

  }
  else{  // Pragmas exist Only for C/C++ code
    PragmaList_t pragmas;
    if (collectPragmas (proj, pragmas))
    {
      for (PragmaList_t::iterator i = pragmas.begin ();
          i != pragmas.end (); ++i)
        if (preprocess (*i))
          ++count;
    }
  }
  return count;
}

// eof
