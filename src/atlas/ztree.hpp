#pragma once

#include "zlog.h"
#include <boost/iterator/iterator_facade.hpp>
#include <type_traits>
#include <memory>
#include <deque>
#include <vector>
#include "zglobal.h"

namespace nim {

// if Iterator's root node is not nullptr (iterator through subtree), then it can only decrement correctly if root node
// and tree structure is not changed
// Iterator created by endBreadthFirst() can not decrement

namespace impl {

template<typename T>
struct TreeNode
{
  typedef T ValueType;

  TreeNode()
    : parent(nullptr), firstChild(nullptr), lastChild(nullptr), prevSibling(nullptr), nextSibling(nullptr)
  {}

  TreeNode(const T& d)
    : parent(nullptr), firstChild(nullptr), lastChild(nullptr), prevSibling(nullptr), nextSibling(nullptr), data(d)
  {}

  TreeNode<T>* parent;
  TreeNode<T>* firstChild;
  TreeNode<T>* lastChild;
  TreeNode<T>* prevSibling;
  TreeNode<T>* nextSibling;
  T data;
};

template<typename TNode, bool TNodeIsConst = std::is_const<TNode>::value>
class BaseIterator
{
  template<typename>
  friend
  class Iterator;

public:
  typedef TNode NodeType;
  typedef typename TNode::ValueType ValueType;
  NodeType* node;
  NodeType* parent;

protected:
  ValueType& dereference() const
  { return node->data; }

  bool isTail(const NodeType* node)
  { return !node->parent && !node->nextSibling; }
};

template<typename TNode>
class BaseIterator<TNode, true>
{
  template<typename>
  friend
  class Iterator;

public:
  typedef TNode NodeType;
  typedef const typename TNode::ValueType ValueType;
  NodeType* node;
  NodeType* parent;

protected:
  ValueType& dereference() const
  { return node->data; }

  bool isTail(const NodeType* node)
  { return !node->parent && !node->nextSibling; }
};

template<typename TNode>
class PreOrderIterator : public BaseIterator<TNode>
{
public:
  typedef typename BaseIterator<TNode>::NodeType NodeType;
protected:
  void init(NodeType* n, NodeType* p)
  {
    this->node = n;
    this->parent = p;
  }

  void increment()
  {
    CHECK(this->node);
    if (this->node->firstChild) {
      this->node = this->node->firstChild;
    } else {
      if (this->node == this->parent) {
        this->node = nullptr;
      } else {
        while (this->node->nextSibling == nullptr) {
          this->node = this->node->parent;
          if (this->node == this->parent) {
            this->node = nullptr;
            return;
          }
        }
        this->node = this->node->nextSibling;
      }
    }
  }

  void decrement()
  {
    if (this->node) {
      if (this->node->prevSibling) {
        this->node = this->node->prevSibling;
        while (this->node->lastChild)
          this->node = this->node->lastChild;
      } else {
        this->node = this->node->parent;
      }
    } else {
      CHECK(this->parent);
      this->node = this->parent;
      while (this->node->lastChild)
        this->node = this->node->lastChild;
    }
  }
};

template<typename TNode>
class PostOrderIterator : public BaseIterator<TNode>
{
public:
  typedef typename BaseIterator<TNode>::NodeType NodeType;
protected:
  void init(NodeType* n, NodeType* p)
  {
    this->node = n;
    this->parent = p;
  }

  void increment()
  {
    CHECK(this->node);
    if (this->node == this->parent) {
      this->node = nullptr;
    } else {
      if (this->node->nextSibling) {
        this->node = this->node->nextSibling;
        while (this->node->firstChild)
          this->node = this->node->firstChild;
      } else {
        this->node = this->node->parent;
      }
    }
  }

  void decrement()
  {
    if (this->node) {
      if (this->node->lastChild) {
        this->node = this->node->lastChild;
      } else {
        while (this->node->prevSibling == nullptr)
          this->node = this->node->parent;
        this->node = this->node->prevSibling;
      }
    } else {
      CHECK(this->parent);
      this->node = this->parent;
    }
  }
};

template<typename TNode>
class BreadthFirstIterator : public BaseIterator<TNode>
{
public:
  typedef typename BaseIterator<TNode>::NodeType NodeType;
protected:
  void init(NodeType* n, NodeType* p)
  {
    startNode = n ? n : p;
    this->node = n;
    this->parent = p;
    if (this->node) {
      if (this->isTail(this->node)) {
        //CHECK(!this->parent);
        this->node = nullptr;
        return;
      }
      if (!this->parent) {
        deque.push_back(this->node);
        n = n->nextSibling;
        while (n && !this->isTail(n)) {
          deque.push_back(n);
          n = n->nextSibling;
        }
      } else {
        //CHECK(this->node == this->parent);
        deque.push_back(this->node);
      }
    }
  }

  void increment()
  {
    CHECK(this->node);
    NodeType* n = this->node->firstChild;
    while (n) {
      deque.push_back(n);
      n = n->nextSibling;
    }
    deque.pop_front();
    this->node = deque.empty() ? nullptr : deque.front();
  }

  void decrement()
  {
    CHECK(startNode && this->node != startNode);
    std::deque<NodeType*> tmpDeque;
    tmpDeque.push_back(startNode);
    while (!tmpDeque.empty()) {
      NodeType* curNode = tmpDeque.front();
      NodeType* n = curNode->firstChild;
      while (n) {
        tmpDeque.push_back(n);
        n = n->nextSibling;
      }
      tmpDeque.pop_front();
      NodeType* nextNode = tmpDeque.empty() ? nullptr : tmpDeque.front();
      if (nextNode == this->node) {
        this->node = curNode;
        break;
      }
    }
  }

  std::deque<NodeType*> deque;
  NodeType* startNode;
};

template<typename TNode>
class ChildIterator : public BaseIterator<TNode>
{
public:
  typedef typename BaseIterator<TNode>::NodeType NodeType;
protected:
  void init(NodeType* n, NodeType* p)
  {
    this->node = n;
    this->parent = p;
  }

  void increment()
  {
    if (this->node)
      this->node = this->node->nextSibling;
  }

  void decrement()
  {
    if (this->node) {
      this->node = this->node->prevSibling;
    } else {
      this->node = this->parent->lastChild;
    }
  }
};

template<typename TNode>
class AncestorIterator : public BaseIterator<TNode>
{
public:
  typedef typename BaseIterator<TNode>::NodeType NodeType;
protected:
  void init(NodeType* n, NodeType* p)
  {
    this->node = n;
    this->parent = p;
    startNode = n ? n : p;
  }

  void increment()
  {
    if (this->node) {
      this->node = this->node->parent;
    }
  }

  void decrement()
  {
    CHECK(startNode && this->node != startNode);
    NodeType* n = startNode;
    while (n && n->parent != this->node)
      n = n->parent;
    this->node = n;
  }

  NodeType* startNode;
};

template<typename TNode>
class LeafIterator : public BaseIterator<TNode>
{
public:
  typedef typename BaseIterator<TNode>::NodeType NodeType;
protected:
  void init(NodeType* n, NodeType* p)
  {
    this->node = n;
    this->parent = p;
  }

  void increment()
  {
    CHECK(this->node);
    if (this->node->firstChild) {
      while (this->node->firstChild)
        this->node = this->node->firstChild;
      return;
    }
    if (this->node == this->parent) {
      this->node = nullptr;
    } else {
      while (this->node->nextSibling == nullptr) {
        if (this->node->parent == nullptr) // tail
          return;
        this->node = this->node->parent;
        if (this->node == this->parent)
          return;
      }
      this->node = this->node->nextSibling;
      while (this->node->firstChild)
        this->node = this->node->firstChild;
    }
  }

  void decrement()
  {
    if (this->node) {
      while (this->node->prevSibling == nullptr) {
        if (this->node->parent == nullptr) // head
          return;
        this->node = this->node->parent;
        if (this->node == this->parent)
          return;
      }
      this->node = this->node->prevSibling;
      while (this->node->lastChild)
        this->node = this->node->lastChild;
    } else {
      CHECK(this->parent);
      this->node = this->parent;
      while (this->node->lastChild)
        this->node = this->node->lastChild;
    }
  }
};

template<typename TBaseIter>
class Iterator : public TBaseIter,
                 public boost::iterator_facade<Iterator<TBaseIter>,
                   typename TBaseIter::ValueType,
                   boost::bidirectional_traversal_tag,
                   typename TBaseIter::ValueType&>
{
  struct enabler
  {
  };
public:
  Iterator()
  { this->init(nullptr, nullptr); }

  template<class OtherTBaseIter>
  Iterator(Iterator<OtherTBaseIter> const& other, typename std::enable_if<
    std::is_convertible<typename OtherTBaseIter::ValueType*, typename TBaseIter::ValueType*>::value, enabler
  >::type = enabler())
  { this->init(other.node, other.parent); }

  explicit Iterator(typename TBaseIter::NodeType* n, typename TBaseIter::NodeType* p = nullptr)
  { this->init(n, p); }

  bool operator<(const Iterator<TBaseIter>& rhs) const
  { return this->node < rhs.node; }

protected:
  friend class boost::iterator_core_access;

  bool equal(Iterator rhs) const
  { return this->node == rhs.node; }
};

} // namespace impl

template<typename T>
class ZTree
{
  typedef impl::TreeNode<T> TreeNode;
public:
  typedef T ValueType;
  typedef impl::Iterator<impl::PreOrderIterator<TreeNode>> PreOrderIterator;
  typedef impl::Iterator<impl::PreOrderIterator<const TreeNode>> ConstPreOrderIterator;
  typedef impl::Iterator<impl::PostOrderIterator<TreeNode>> PostOrderIterator;
  typedef impl::Iterator<impl::PostOrderIterator<const TreeNode>> ConstPostOrderIterator;
  typedef impl::Iterator<impl::BreadthFirstIterator<TreeNode>> BreadthFirstIterator;
  typedef impl::Iterator<impl::BreadthFirstIterator<const TreeNode>> ConstBreadthFirstIterator;
  typedef impl::Iterator<impl::ChildIterator<TreeNode>> ChildIterator;
  typedef impl::Iterator<impl::ChildIterator<const TreeNode>> ConstChildIterator;
  typedef impl::Iterator<impl::AncestorIterator<TreeNode>> AncestorIterator;
  typedef impl::Iterator<impl::AncestorIterator<const TreeNode>> ConstAncestorIterator;
  typedef impl::Iterator<impl::LeafIterator<TreeNode>> LeafIterator;
  typedef impl::Iterator<impl::LeafIterator<const TreeNode>> ConstLeafIterator;
  typedef PreOrderIterator Iterator;
  typedef ConstPreOrderIterator ConstIterator;
  typedef ChildIterator RootIterator;
  typedef ConstChildIterator ConstRootIterator;

  ZTree()
  { init(); }

  template<typename Iter>
  ZTree(const Iter& it)
  {
    init();
    copy(appendRoot(*it), it);
  }

  ZTree(const ZTree& rhs)
  {
    init();
    deepCopy(rhs);
  }

  ZTree(ZTree&& rhs)
  {
    init();
    swap(rhs);
  }

  virtual ~ZTree()
  {
    clear();
    delete m_head;
    delete m_tail;
  }

  ZTree<T>& operator=(ZTree rhs)
  {
    swap(rhs);
    return *this;
  }

  void swap(ZTree<T>& rhs) noexcept
  {
    std::swap(m_head, rhs.m_head);
    std::swap(m_tail, rhs.m_tail);
  }

  Iterator begin()
  { return Iterator(m_head->nextSibling); }

  Iterator end()
  { return Iterator(m_tail); }

  template<typename Iter>
  Iterator begin(const Iter& root)
  {
    CHECK(root.node && root.node != m_head && root.node != m_tail);
    return Iterator(root.node, root.node);
  }

  template<typename Iter>
  Iterator end(const Iter& root)
  {
    CHECK(root.node && root.node != m_head && root.node != m_tail);
    return Iterator(nullptr, root.node);
  }

  ConstIterator begin() const
  { return ConstIterator(m_head->nextSibling); }

  ConstIterator end() const
  { return ConstIterator(m_tail); }

  template<typename Iter>
  ConstIterator begin(const Iter& root) const
  {
    CHECK(root.node && root.node != m_head && root.node != m_tail);
    return ConstIterator(root.node, root.node);
  }

  template<typename Iter>
  ConstIterator end(const Iter& root) const
  {
    CHECK(root.node && root.node != m_head && root.node != m_tail);
    return ConstIterator(nullptr, root.node);
  }

  ConstIterator cbegin() const
  { return begin(); }

  ConstIterator cend() const
  { return end(); }

  template<typename Iter>
  ConstIterator cbegin(const Iter& root) const
  { return begin(root); }

  template<typename Iter>
  ConstIterator cend(const Iter& root) const
  { return end(root); }

  PostOrderIterator beginPost()
  {
    TreeNode* n = m_head->nextSibling;
    if (n != m_tail) {
      while (n->firstChild)
        n = n->firstChild;
    }
    return PostOrderIterator(n);
  }

  PostOrderIterator endPost()
  { return PostOrderIterator(m_tail); }

  template<typename Iter>
  PostOrderIterator beginPost(const Iter& root)
  {
    CHECK(root.node && root.node != m_head && root.node != m_tail);
    TreeNode* n = root.node;
    while (n->firstChild)
      n = n->firstChild;
    return PostOrderIterator(n, root.node);
  }

  template<typename Iter>
  PostOrderIterator endPost(const Iter& root)
  {
    CHECK(root.node && root.node != m_head && root.node != m_tail);
    return PostOrderIterator(nullptr, root.node);
  }

  ConstPostOrderIterator beginPost() const
  {
    TreeNode* n = m_head->nextSibling;
    if (n != m_tail) {
      while (n->firstChild)
        n = n->firstChild;
    }
    return ConstPostOrderIterator(n);
  }

  ConstPostOrderIterator endPost() const
  { return ConstPostOrderIterator(m_tail); }

  template<typename Iter>
  ConstPostOrderIterator beginPost(const Iter& root) const
  {
    CHECK(root.node && root.node != m_head && root.node != m_tail);
    TreeNode* n = root.node;
    while (n->firstChild)
      n = n->firstChild;
    return ConstPostOrderIterator(n, root.node);
  }

  template<typename Iter>
  ConstPostOrderIterator endPost(const Iter& root) const
  {
    CHECK(root.node && root.node != m_head && root.node != m_tail);
    return ConstPostOrderIterator(nullptr, root.node);
  }

  ConstPostOrderIterator cbeginPost() const
  { return beginPost(); }

  ConstPostOrderIterator cendPost() const
  { return endPost(); }

  template<typename Iter>
  ConstPostOrderIterator cbeginPost(const Iter& root) const
  { return beginPost(root); }

  template<typename Iter>
  ConstPostOrderIterator cendPost(const Iter& root) const
  { return endPost(root); }

  BreadthFirstIterator beginBreadthFirst()
  { return BreadthFirstIterator(m_head->nextSibling); }

  BreadthFirstIterator endBreadthFirst()
  { return BreadthFirstIterator(); }

  template<typename Iter>
  BreadthFirstIterator beginBreadthFirst(const Iter& root)
  {
    CHECK(root.node && root.node != m_head && root.node != m_tail);
    return BreadthFirstIterator(root.node, root.node);
  }

  template<typename Iter>
  BreadthFirstIterator endBreadthFirst(const Iter& root)
  {
    CHECK(root.node && root.node != m_head && root.node != m_tail);
    return BreadthFirstIterator(nullptr, root.node);
  }

  ConstBreadthFirstIterator beginBreadthFirst() const
  { return ConstBreadthFirstIterator(m_head->nextSibling); }

  ConstBreadthFirstIterator endBreadthFirst() const
  { return ConstBreadthFirstIterator(); }

  template<typename Iter>
  ConstBreadthFirstIterator beginBreadthFirst(const Iter& root) const
  {
    CHECK(root.node && root.node != m_head && root.node != m_tail);
    return ConstBreadthFirstIterator(root.node, root.node);
  }

  template<typename Iter>
  ConstBreadthFirstIterator endBreadthFirst(const Iter& root) const
  {
    CHECK(root.node && root.node != m_head && root.node != m_tail);
    return ConstBreadthFirstIterator(nullptr, root.node);
  }

  ConstBreadthFirstIterator cbeginBreadthFirst() const
  { return beginBreadthFirst(); }

  ConstBreadthFirstIterator cendBreadthFirst() const
  { return endBreadthFirst(); }

  template<typename Iter>
  ConstBreadthFirstIterator cbeginBreadthFirst(const Iter& root) const
  { return beginBreadthFirst(root); }

  template<typename Iter>
  ConstBreadthFirstIterator cendBreadthFirst(const Iter& root) const
  { return endBreadthFirst(root); }

  RootIterator beginRoot()
  { return RootIterator(m_head->nextSibling); }

  RootIterator endRoot()
  { return RootIterator(m_tail); }

  ConstRootIterator beginRoot() const
  { return ConstRootIterator(m_head->nextSibling); }

  ConstRootIterator endRoot() const
  { return ConstRootIterator(m_tail); }

  ConstRootIterator cbeginRoot() const
  { return beginRoot(); }

  ConstRootIterator cendRoot() const
  { return endRoot(); }

  template<typename Iter>
  ChildIterator beginChild(const Iter& parent)
  {
    CHECK(parent.node && parent.node != m_head && parent.node != m_tail);
    return ChildIterator(parent.node->firstChild, parent.node);
  }

  template<typename Iter>
  ChildIterator endChild(const Iter& parent)
  {
    CHECK(parent.node && parent.node != m_head && parent.node != m_tail);
    return ChildIterator(nullptr, parent.node);
  }

  template<typename Iter>
  ConstChildIterator beginChild(const Iter& parent) const
  {
    CHECK(parent.node && parent.node != m_head && parent.node != m_tail);
    return ConstChildIterator(parent.node->firstChild, parent.node);
  }

  template<typename Iter>
  ConstChildIterator endChild(const Iter& parent) const
  {
    CHECK(parent.node && parent.node != m_head && parent.node != m_tail);
    return ConstChildIterator(nullptr, parent.node);
  }

  template<typename Iter>
  ConstChildIterator cbeginChild(const Iter& parent) const
  { return beginChild(parent); }

  template<typename Iter>
  ConstChildIterator cendChild(const Iter& parent) const
  { return endChild(parent); }

  template<typename Iter>
  AncestorIterator beginAncestor(const Iter& child, bool includeChild = false)
  {
    CHECK(child.node && child.node != m_head && child.node != m_tail);
    return includeChild ? AncestorIterator(child.node, child.node) : AncestorIterator(child.node->parent, child.node);
  }

  template<typename Iter>
  AncestorIterator endAncestor(const Iter& child)
  {
    CHECK(child.node && child.node != m_head && child.node != m_tail);
    return AncestorIterator(nullptr, child.node);
  }

  template<typename Iter>
  ConstAncestorIterator beginAncestor(const Iter& child, bool includeChild = false) const
  {
    CHECK(child.node && child.node != m_head && child.node != m_tail);
    return includeChild ? ConstAncestorIterator(child.node, child.node) : ConstAncestorIterator(child.node->parent,
                                                                                                child.node);
  }

  template<typename Iter>
  ConstAncestorIterator endAncestor(const Iter& child) const
  {
    CHECK(child.node && child.node != m_head && child.node != m_tail);
    return ConstAncestorIterator(nullptr, child.node);
  }

  template<typename Iter>
  ConstAncestorIterator cbeginAncestor(const Iter& child, bool includeChild = false) const
  { return beginAncestor(child, includeChild); }

  template<typename Iter>
  ConstAncestorIterator cendAncestor(const Iter& child) const
  { return endAncestor(child); }

  LeafIterator beginLeaf()
  {
    TreeNode* n = m_head->nextSibling;
    if (n != m_tail) {
      while (n->firstChild)
        n = n->firstChild;
    }
    return LeafIterator(n);
  }

  LeafIterator endLeaf()
  { return LeafIterator(m_tail); }

  template<typename Iter>
  LeafIterator beginLeaf(const Iter& root)
  {
    CHECK(root.node && root.node != m_head && root.node != m_tail);
    TreeNode* n = root.node;
    while (n->firstChild)
      n = n->firstChild;
    return LeafIterator(n, root.node);
  }

  template<typename Iter>
  LeafIterator endLeaf(const Iter& root)
  {
    CHECK(root.node && root.node != m_head && root.node != m_tail);
    return LeafIterator(nullptr, root.node);
  }

  ConstLeafIterator beginLeaf() const
  {
    TreeNode* n = m_head->nextSibling;
    if (n != m_tail) {
      while (n->firstChild)
        n = n->firstChild;
    }
    return ConstLeafIterator(n);
  }

  ConstLeafIterator endLeaf() const
  { return ConstLeafIterator(m_tail); }

  template<typename Iter>
  ConstLeafIterator beginLeaf(const Iter& root) const
  {
    CHECK(root.node && root.node != m_head && root.node != m_tail);
    TreeNode* n = root.node;
    while (n->firstChild)
      n = n->firstChild;
    return ConstLeafIterator(n, root.node);
  }

  template<typename Iter>
  ConstLeafIterator endLeaf(const Iter& root) const
  {
    CHECK(root.node && root.node != m_head && root.node != m_tail);
    return ConstLeafIterator(nullptr, root.node);
  }

  ConstLeafIterator cbeginLeaf() const
  { return beginLeaf(); }

  ConstLeafIterator cendLeaf() const
  { return endLeaf(); }

  template<typename Iter>
  ConstLeafIterator cbeginLeaf(const Iter& root) const
  { return beginLeaf(root); }

  template<typename Iter>
  ConstLeafIterator cendLeaf(const Iter& root) const
  { return endLeaf(root); }

  size_t size() const
  { return std::distance(begin(), end()); }

  // size of subtree
  template<typename Iter>
  size_t size(const Iter& parent)
  { return std::distance(begin(parent), end(parent)); }

  size_t numRoots() const
  { return std::distance(beginRoot(), endRoot()); }

  size_t numLeafs() const
  { return std::distance(beginLeaf(), endLeaf()); }

  template<typename Iter>
  size_t numChildren(const Iter& parent) const
  { return std::distance(beginChild(parent), endChild(parent)); }

  template<typename Iter>
  size_t numAncestors(const Iter& child) const
  { return std::distance(beginAncestor(child), endAncestor(child)); }

  template<typename Iter>
  size_t numDescendants(const Iter& parent) const
  { return size(parent) - 1; }

  bool empty() const
  { return m_head->nextSibling == m_tail; }

  template<typename Iter>
  static bool isRoot(const Iter& pos)
  { return !pos.node->parent; }

  template<typename Iter>
  static bool isBranchNode(const Iter& pos)
  { return pos.node->firstChild != pos.node->lastChild; }

  template<typename Iter>
  static bool isLeaf(const Iter& pos)
  { return !pos.node->firstChild; }

  template<typename Iter>
  static bool isNull(const Iter& pos)
  { return !pos.node; }

  template<typename Iter>
  static Iter parent(const Iter& pos)
  { return Iter(pos.node->parent); }

  template<typename Iter>
  static Iter firstChild(const Iter& pos)
  { return Iter(pos.node->firstChild); }

  void clear()
  {
    if (m_head) {
      PostOrderIterator it = beginPost();
      PostOrderIterator tmp;
      PostOrderIterator end = endPost();
      while (it != end) {
        tmp = it++;
        erase(tmp);
      }
    }
  }

  template<typename Iter>
  void erase(Iter pos)
  {
    flatten(pos);
    detachParent(pos);
    delete pos.node;
  }

  template<typename Iter>
  void eraseSubtree(Iter root)
  {
    PostOrderIterator it = beginPost(root);
    PostOrderIterator tmp;
    PostOrderIterator end = endPost(root);
    while (it != end) {
      tmp = it++;
      erase(tmp);
    }
  }

  template<typename Iter>
  void eraseChildren(Iter root)
  {
    PostOrderIterator it = beginPost(root);
    PostOrderIterator tmp;
    PostOrderIterator end = endPost(root);
    --end;
    while (it != end) {
      tmp = it++;
      erase(tmp);
    }
  }

  Iterator appendRoot(const T& v)
  {
    TreeNode* node = new TreeNode(v);
    node->prevSibling = m_tail->prevSibling;
    node->nextSibling = m_tail;
    m_tail->prevSibling->nextSibling = node;
    m_tail->prevSibling = node;
    return Iterator(node);
  }

  Iterator prependRoot(const T& v)
  {
    TreeNode* node = new TreeNode(v);
    node->prevSibling = m_head;
    node->nextSibling = m_head->nextSibling;
    m_head->nextSibling->prevSibling = node;
    m_head->nextSibling = node;
    return Iterator(node);
  }

  template<typename Iter>
  Iter appendChild(Iter parent, const T& v)
  {
    CHECK(parent.node && parent.node != m_head && parent.node != m_tail);
    TreeNode* node = new TreeNode(v);
    node->parent = parent.node;
    if (parent.node->lastChild) {
      parent.node->lastChild->nextSibling = node;
      node->prevSibling = parent.node->lastChild;
      parent.node->lastChild = node;
    } else {
      parent.node->firstChild = node;
      parent.node->lastChild = node;
    }
    return Iter(node);
  }

  // child will be detached from previous parent (or siblings in case it is root)
  template<typename Iter>
  void appendChild(Iter parent, Iter child)
  {
    CHECK(parent.node && parent.node != m_head && parent.node != m_tail);
    CHECK(child.node && child.node != m_head && child.node != m_tail);
    if (parent == this->parent(child))
      return;
    detachParent(child);
    CHECK(!child.node->parent && !child.node->prevSibling && !child.node->nextSibling);
    child.node->parent = parent.node;
    if (parent.node->lastChild) {
      parent.node->lastChild->nextSibling = child.node;
      child.node->prevSibling = parent.node->lastChild;
      parent.node->lastChild = child.node;
    } else {
      parent.node->firstChild = child.node;
      parent.node->lastChild = child.node;
    }
  }

  template<typename Iter>
  Iter prependChild(Iter parent, const T& v)
  {
    CHECK(parent.node && parent.node != m_head && parent.node != m_tail);
    TreeNode* node = new TreeNode(v);
    node->parent = parent.node;
    if (parent.node->firstChild) {
      parent.node->firstChild->prevSibling = node;
      node->nextSibling = parent.node->firstChild;
      parent.node->firstChild = node;
    } else {
      parent.node->firstChild = node;
      parent.node->lastChild = node;
    }
    return Iter(node);
  }

  // child will be detached from previous parent (if any)
  template<typename Iter>
  void prependChild(Iter parent, Iter child)
  {
    CHECK(parent.node && parent.node != m_head && parent.node != m_tail);
    CHECK(child.node && child.node != m_head && child.node != m_tail);
    if (parent == this->parent(child))
      return;
    if (!isRoot(child))
      detachParent(child);
    CHECK(!child.node->parent && !child.node->prevSibling && !child.node->nextSibling);
    child.node->parent = parent.node;
    if (parent.node->firstChild) {
      parent.node->firstChild->prevSibling = child.node;
      child.node->nextSibling = parent.node->firstChild;
      parent.node->firstChild = child.node;
    } else {
      parent.node->firstChild = child.node;
      parent.node->lastChild = child.node;
    }
  }

  template<typename IterTo, typename IterFrom>
  void copy(IterTo to, const IterFrom& from)
  {
    CHECK(to.node && to.node != m_head && to.node != m_tail && from.node);
    eraseChildren(to);
    to.node->data = from.node->data;
    // pre order copy
    const TreeNode* fromNode = from.node;
    Iterator toIter = to;
    while (fromNode) {
      if (fromNode->firstChild) {
        fromNode = fromNode->firstChild;
        toIter = appendChild(toIter, fromNode->data);
      } else {
        if (fromNode == from.node) {
          fromNode = nullptr;
        } else {
          while (fromNode->nextSibling == nullptr) {
            fromNode = fromNode->parent;
            toIter = parent(toIter);
            if (fromNode == from.node) {
              fromNode = nullptr;
              return;
            }
          }
          fromNode = fromNode->nextSibling;
          toIter = appendChild(parent(toIter), fromNode->data);
        }
      }
    }
  }

  // make pos a leaf, pos's first child becomes pos's next slibing
  template<typename Iter>
  void flatten(Iter pos)
  {
    CHECK(pos.node && pos.node != m_head && pos.node != m_tail);
    if (!pos.node->firstChild)
      return;
    TreeNode* child = pos.node->firstChild;
    while (child) {
      child->parent = pos.node->parent;
      child = child->nextSibling;
    }
    if (pos.node->nextSibling) {
      pos.node->nextSibling->prevSibling = pos.node->lastChild;
      pos.node->lastChild->nextSibling = pos.node->nextSibling;
    } else {
      pos.node->parent->lastChild = pos.node->lastChild;
    }
    pos.node->nextSibling = pos.node->firstChild;
    pos.node->firstChild->prevSibling = pos.node;
    pos.node->firstChild = nullptr;
    pos.node->lastChild = nullptr;
  }

  // make pos's parent as last child of pos
  template<typename Iter>
  void setAsRoot(Iter pos)
  {
    CHECK(pos.node && pos.node != m_head && pos.node != m_tail);
    if (isRoot(pos))
      return;
    std::vector<AncestorIterator> chain;
    for (AncestorIterator it = beginAncestor(pos, true); it != endAncestor(pos); ++it) {
      chain.push_back(it);
    }
    for (size_t i = chain.size() - 1; i >= 1; --i) {
      reverseChildRoot(chain[i - 1], chain[i]);
    }
  }

  // note: return Null Iter if n1 and n2 has different root
  template<typename Iter, typename Iter2>
  Iter lowestCommonAncestor(Iter n1, Iter2 n2)
  {
    std::vector<AncestorIterator> chain1;
    for (AncestorIterator it = beginAncestor(n1, false); it != endAncestor(n1); ++it) {
      chain1.push_back(it);
    }
    std::vector<AncestorIterator> chain2;
    for (AncestorIterator it = beginAncestor(n2, false); it != endAncestor(n2); ++it) {
      chain2.push_back(it);
    }
    size_t i1 = chain1.size() - 1;
    size_t i2 = chain2.size() - 1;
    TreeNode* res = nullptr;
    while (i1 != static_cast<size_t>(-1) && i2 != static_cast<size_t>(-1) && chain1[i1] == chain2[i2]) {
      res = chain1[i1].node;
      --i1;
      --i2;
    }
    return Iter(res);
  }

protected:
  void init()
  {
    m_head = new TreeNode();
    m_tail = new TreeNode();

    m_head->nextSibling = m_tail;
    m_tail->prevSibling = m_head;
  }

  void deepCopy(const ZTree<T>& rhs)
  {
    clear();
    for (ConstRootIterator it = rhs.beginRoot(); it != rhs.endRoot(); ++it) {
      appendRoot(*it);
    }
    RootIterator to = beginRoot();
    ConstRootIterator from = rhs.beginRoot();
    while (to != endRoot()) {
      copy(to, from);
      ++to;
      ++from;
    }
  }

  template<typename Iter>
  void detachParent(Iter pos)
  {
    if (pos.node->prevSibling) {
      pos.node->prevSibling->nextSibling = pos.node->nextSibling;
    }
    if (pos.node->nextSibling) {
      pos.node->nextSibling->prevSibling = pos.node->prevSibling;
    }
    if (pos.node->parent && pos.node->parent->firstChild == pos.node) {
      pos.node->parent->firstChild = pos.node->nextSibling;
    }
    if (pos.node->parent && pos.node->parent->lastChild == pos.node) {
      pos.node->parent->lastChild = pos.node->prevSibling;
    }
    pos.node->parent = nullptr;
    pos.node->prevSibling = nullptr;
    pos.node->nextSibling = nullptr;
  }

  template<typename Iter>
  void reverseChildRoot(Iter child, Iter root)
  {
    CHECK(child.node && child.node != m_head && child.node != m_tail);
    CHECK(root.node && root.node != m_head && root.node != m_tail);
    CHECK(root == this->parent(child) && isRoot(root));
    detachParent(child);
    // move child to root's position, isolate root
    child.node->nextSibling = root.node->nextSibling;
    child.node->prevSibling = root.node->prevSibling;
    root.node->prevSibling->nextSibling = child.node;
    root.node->nextSibling->prevSibling = child.node;
    root.node->prevSibling = nullptr;
    root.node->nextSibling = nullptr;
    // add isolated root to child's children list
    appendChild(child, root);
  }

protected:
  // head --- root1 --- root2 --- root3 --- ... --- tail
  //          / | \     / | \     / |
  //         nodes...  nodes...  nodes...
  //
  TreeNode* m_head;
  TreeNode* m_tail;
};

}  // namespace nim
