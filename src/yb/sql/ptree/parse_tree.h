//--------------------------------------------------------------------------------------------------
// Copyright (c) YugaByte, Inc.
//
// Parse Tree Declaration.
//
// This modules includes declarations for parse tree. The parser whose rules are defined in
// parser_gram.y will link the tree nodes together to form this parse tree.
//--------------------------------------------------------------------------------------------------

#ifndef YB_SQL_PTREE_PARSE_TREE_H_
#define YB_SQL_PTREE_PARSE_TREE_H_

#include "yb/sql/ptree/tree_node.h"

namespace yb {
namespace sql {

// Parse Tree
class ParseTree {
 public:
  //------------------------------------------------------------------------------------------------
  // Public types.
  typedef std::unique_ptr<ParseTree> UniPtr;
  typedef std::unique_ptr<const ParseTree> UniPtrConst;

  //------------------------------------------------------------------------------------------------
  // Public functions.
  explicit ParseTree(std::shared_ptr<MemTracker> mem_tracker = nullptr);
  ~ParseTree();

  // Run semantics analysis.
  CHECKED_STATUS Analyze(SemContext *sem_context);

  // Access functions to root_.
  void set_root(const TreeNode::SharedPtr& root) {
    root_ = root;
  }
  const TreeNode::SharedPtr& root() const {
    return root_;
  }

  // Access function to ptree_mem_.
  MemoryContext *PTreeMem() const {
    return ptree_mem_.get();
  }

  // Access function to psem_mem_.
  MemoryContext *PSemMem() const {
    return psem_mem_.get();
  }

 private:
  //------------------------------------------------------------------------------------------------
  // Private data members.
  TreeNode::SharedPtr root_;

  // Parse tree memory pool. This pool is used to allocate parse tree and its nodes. This pool
  // should be part of the generated parse tree that is stored within parse_context. Once the
  // parse tree is destructed, it's also gone.
  MemoryContext::UniPtr ptree_mem_;

  // Semantic analysis memory pool. This pool is used to allocate memory for storing semantic
  // analysis results in the parse tree. When a parse tree is analyzed, the parse tree is reset to
  // release the previous analysis result and this pool should be reset to free the associated
  // memory. This pool should be part of the generated parse tree also that is stored within
  // sem_context. Once the parse tree is destructed, it's also gone too.
  MemoryContext::UniPtr psem_mem_;
};

}  // namespace sql
}  // namespace yb

#endif  // YB_SQL_PTREE_PARSE_TREE_H_
