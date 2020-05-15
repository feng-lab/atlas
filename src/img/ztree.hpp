#pragma once

#include "zglobal.h"
#include "zlog.h"
#include <boost/iterator/iterator_facade.hpp>
#include <range/v3/view/subrange.hpp>
#include <deque>
#include <vector>

namespace nim {

// all Iterators are standard-conforming BidirectionalIterator
// Iterator can be constructed from different type of Iterator and will behave correctly
// see ztreetest.h for usage

namespace impl {

template<typename T>
struct TreeNode
{
  using ValueType = T;

  TreeNode() = default;

  explicit TreeNode(const T& d)
    : data(d)
  {}

  TreeNode<T>* parent = nullptr;
  TreeNode<T>* firstChild = nullptr;
  TreeNode<T>* lastChild = nullptr;
  TreeNode<T>* prevSibling = nullptr;
  TreeNode<T>* nextSibling = nullptr;
  T data;
};

template<typename TNode, bool TNodeIsConst = std::is_const_v<TNode>>
class BaseIterator
{
  template<typename>
  friend
  class Iterator;

public:
  using NodeType = TNode;
  using ValueType = typename TNode::ValueType;
  NodeType* node;
  NodeType* parent;

protected:
  ValueType& dereference() const
  { return node->data; }

  bool isTail(const NodeType* node_)
  { return !node_->parent && !node_->nextSibling; }
};

template<typename TNode>
class BaseIterator<TNode, true>
{
  template<typename>
  friend
  class Iterator;

public:
  using NodeType = TNode;
  using ValueType = const typename TNode::ValueType;
  NodeType* node;
  NodeType* parent;

protected:
  ValueType& dereference() const
  { return node->data; }

  bool isTail(const NodeType* node_)
  { return !node_->parent && !node_->nextSibling; }
};

template<typename TNode>
class PreOrderIterator : public BaseIterator<TNode>
{
public:
  using NodeType = typename BaseIterator<TNode>::NodeType;
protected:
  void init(NodeType* n, NodeType* p)
  {
    this->node = n;
    this->parent = p;
  }

  void increment()
  {
    // assume this->node or this->parent
    CHECK(this->node && !this->isTail(this->node)); // crash on purpose if we are increasing past-the-end
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
    // assume this->node or this->parent
    if (this->node) {
      if (this->node->prevSibling) {
        this->node = this->node->prevSibling;
        while (this->node->lastChild)
          this->node = this->node->lastChild;
      } else {
        this->node = this->node->parent;
      }
    } else {
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
  using NodeType = typename BaseIterator<TNode>::NodeType;
protected:
  void init(NodeType* n, NodeType* p)
  {
    this->node = n;
    this->parent = p;
  }

  void increment()
  {
    // assume this->node or this->parent
    CHECK(this->node && !this->isTail(this->node)); // crash on purpose if we are increasing past-the-end
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
    // assume this->node or this->parent
    if (this->node) {
      if (this->node->lastChild) {
        this->node = this->node->lastChild;
      } else {
        while (this->node->prevSibling == nullptr)
          this->node = this->node->parent;
        this->node = this->node->prevSibling;
      }
    } else {
      this->node = this->parent;
    }
  }
};

template<typename TNode>
class BreadthFirstIterator : public BaseIterator<TNode>
{
public:
  using NodeType = typename BaseIterator<TNode>::NodeType;
protected:
  void init(NodeType* n, NodeType* p)
  {
    this->node = n;
    this->parent = p;
    if (!n && !p)  // default constructed
      return;

    NodeType* startNode;
    if (p) { // start node is p (must be valid), current node is n, end node is nullptr
      startNode = p;
      endNode = nullptr;
    } else { // p is nullptr, start node is first root of tree, current node is n, end node is m_tail
      // use n to find start node
      startNode = n;
      while (startNode->parent) {
        startNode = startNode->parent;
      }
      CHECK(startNode->prevSibling);
      while (startNode->prevSibling->prevSibling)
        startNode = startNode->prevSibling;
      // find m_tail as end node
      endNode = startNode;
      while (!this->isTail(endNode))
        endNode = endNode->nextSibling;
    }

    if (!this->isTail(startNode)) {
      deque.push_back(startNode);
      if (endNode) {
        n = startNode->nextSibling;
        while (n && n != endNode) {
          deque.push_back(n);
          n = n->nextSibling;
        }
      }

      // if current node is equal to end node, usually it is just for equality comparison,
      //  so we do nothing and let decrement() takes care of the rare decrement case.
      // if current node is not equal to start node, we need to walk the tree to current node
      if (this->node != endNode && this->node != startNode) {
        while (deque.front() != this->node) {
          n = deque.front()->firstChild;
          while (n) {
            deque.push_back(n);
            n = n->nextSibling;
          }
          prevDeque.push_back(deque.front());
          deque.pop_front();
          CHECK(!deque.empty());
        }
      }
    }
  }

  void increment()
  {
    // assume this->node or this->parent
    CHECK(this->node != endNode && !deque.empty()); // crash on purpose if we are increasing past-the-end
    CHECK(this->node == deque.front());
    NodeType* n = this->node->firstChild;
    while (n) {
      deque.push_back(n);
      n = n->nextSibling;
    }
    prevDeque.push_back(deque.front());
    deque.pop_front();
    this->node = deque.empty() ? endNode : deque.front();
  }

  void decrement()
  {
    // assume this->node or this->parent
    if (this->node == endNode && prevDeque.empty() && !deque.empty()) {
      // decrement from endBreadthFirst() iterator, we do a forward transverse from start
      while (!deque.empty()) {
        NodeType* n = deque.front()->firstChild;
        while (n) {
          deque.push_back(n);
          n = n->nextSibling;
        }
        prevDeque.push_back(deque.front());
        deque.pop_front();
      }
    }

    CHECK(!prevDeque.empty());
    NodeType* n = prevDeque.back();
    while (!deque.empty() && deque.back()->parent == n) {
      deque.pop_back();
    }
    deque.push_front(n);
    prevDeque.pop_back();
    this->node = deque.front();
  }

  std::deque<NodeType*> deque;
  std::deque<NodeType*> prevDeque;
  NodeType* endNode = nullptr;
};

template<typename TNode>
class ChildIterator : public BaseIterator<TNode>
{
public:
  using NodeType = typename BaseIterator<TNode>::NodeType;
protected:
  void init(NodeType* n, NodeType* p)
  {
    this->node = n;
    this->parent = p;
  }

  void increment()
  {
    // assume this->node or this->parent
    CHECK(this->node && !this->isTail(this->node)); // crash on purpose if we are increasing past-the-end
    this->node = this->node->nextSibling;
  }

  void decrement()
  {
    // assume this->node or this->parent
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
  using NodeType = typename BaseIterator<TNode>::NodeType;
protected:
  void init(NodeType* n, NodeType* p)
  {
    this->node = n;
    this->parent = p;
  }

  void increment()
  {
    // assume this->parent
    CHECK(this->node); // crash on purpose if we are increasing past-the-end
    this->node = this->node->parent;
  }

  void decrement()
  {
    // assume this->parent
    CHECK(this->parent->parent != this->node);  // crash on purpose if we are decreasing begin iterator
    NodeType* n = this->parent->parent;
    while (n && n->parent != this->node)
      n = n->parent;
    this->node = n;
    CHECK(this->node);
  }
};

template<typename TNode>
class LeafIterator : public BaseIterator<TNode>
{
public:
  using NodeType = typename BaseIterator<TNode>::NodeType;
protected:
  void init(NodeType* n, NodeType* p)
  {
    this->node = n;
    this->parent = p;
  }

  void increment()
  {
    // assume this->node or this->parent
    CHECK(this->node && !this->isTail(this->node)); // crash on purpose if we are increasing past-the-end
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
        if (this->node == this->parent) {
          this->node = nullptr;
          return;
        }
      }
      this->node = this->node->nextSibling;
      while (this->node->firstChild)
        this->node = this->node->firstChild;
    }
  }

  void decrement()
  {
    // assume this->node or this->parent
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
    std::is_convertible_v<typename OtherTBaseIter::ValueType*, typename TBaseIter::ValueType*>, enabler
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
  using TreeNode = impl::TreeNode<T>;

  template<class Iterator>
  using reverse_iterator = std::reverse_iterator<Iterator>;
public:
  using ValueType = T;

  using PreOrderIterator = impl::Iterator<impl::PreOrderIterator<TreeNode>>;
  using ConstPreOrderIterator = impl::Iterator<impl::PreOrderIterator<const TreeNode>>;
  using ReversePreOrderIterator = reverse_iterator<PreOrderIterator>;
  using ConstReversePreOrderIterator = reverse_iterator<ConstPreOrderIterator>;

  using PostOrderIterator = impl::Iterator<impl::PostOrderIterator<TreeNode>>;
  using ConstPostOrderIterator = impl::Iterator<impl::PostOrderIterator<const TreeNode>>;
  using ReversePostOrderIterator = reverse_iterator<PostOrderIterator>;
  using ConstReversePostOrderIterator = reverse_iterator<ConstPostOrderIterator>;

  using BreadthFirstIterator = impl::Iterator<impl::BreadthFirstIterator<TreeNode>>;
  using ConstBreadthFirstIterator = impl::Iterator<impl::BreadthFirstIterator<const TreeNode>>;
  using ReverseBreadthFirstIterator = reverse_iterator<BreadthFirstIterator>;
  using ConstReverseBreadthFirstIterator = reverse_iterator<ConstBreadthFirstIterator>;

  using ChildIterator = impl::Iterator<impl::ChildIterator<TreeNode>>;
  using ConstChildIterator = impl::Iterator<impl::ChildIterator<const TreeNode>>;
  using ReverseChildIterator = reverse_iterator<ChildIterator>;
  using ConstReverseChildIterator = reverse_iterator<ConstChildIterator>;

  using AncestorIterator = impl::Iterator<impl::AncestorIterator<TreeNode>>;
  using ConstAncestorIterator = impl::Iterator<impl::AncestorIterator<const TreeNode>>;
  using ReverseAncestorIterator = reverse_iterator<AncestorIterator>;
  using ConstReverseAncestorIterator = reverse_iterator<ConstAncestorIterator>;

  using LeafIterator = impl::Iterator<impl::LeafIterator<TreeNode>>;
  using ConstLeafIterator = impl::Iterator<impl::LeafIterator<const TreeNode>>;
  using ReverseLeafIterator = reverse_iterator<LeafIterator>;
  using ConstReverseLeafIterator = reverse_iterator<ConstLeafIterator>;

  using Iterator = PreOrderIterator;
  using ConstIterator = ConstPreOrderIterator;
  using ReverseIterator = ReversePreOrderIterator;
  using ConstReverseIterator = ConstReversePreOrderIterator;

  using RootIterator = ChildIterator;
  using ConstRootIterator = ConstChildIterator;
  using ReverseRootIterator = ReverseChildIterator;
  using ConstReverseRootIterator = ConstReverseChildIterator;

  ZTree()
  { init(); }

  template<typename Iter>
  explicit ZTree(const Iter& it)
  {
    init();
    copy(appendRoot(*it), it);
  }

  ZTree(const ZTree& rhs)
  {
    init();
    deepCopy(rhs);
  }

  ZTree(ZTree&& rhs) noexcept
  {
    init();
    swap(rhs);
  }

  virtual ~ZTree()
  {
    clear();
  }

  ZTree<T>& operator=(ZTree rhs) noexcept
  {
    swap(rhs);
    return *this;
  }

  void swap(ZTree<T>& rhs) noexcept
  {
    std::swap(m_head, rhs.m_head);
    std::swap(m_tail, rhs.m_tail);
  }

  Iterator begin() noexcept
  { return Iterator(m_head->nextSibling); }

  Iterator end() noexcept
  { return Iterator(m_tail.get()); }

  ReverseIterator rbegin() noexcept
  { return std::make_reverse_iterator(end()); }

  ReverseIterator rend() noexcept
  { return std::make_reverse_iterator(begin()); }

  template<typename Iter>
  Iterator begin(const Iter& root) noexcept
  {
    CHECK(isValid(root));
    return Iterator(root.node, root.node);
  }

  template<typename Iter>
  Iterator end(const Iter& root) noexcept
  {
    CHECK(isValid(root));
    return Iterator(nullptr, root.node);
  }

  template<typename Iter>
  ReverseIterator rbegin(const Iter& root) noexcept
  { return std::make_reverse_iterator(end(root)); }

  template<typename Iter>
  ReverseIterator rend(const Iter& root) noexcept
  { return std::make_reverse_iterator(begin(root)); }

  ConstIterator begin() const noexcept
  { return ConstIterator(m_head->nextSibling); }

  ConstIterator end() const noexcept
  { return ConstIterator(m_tail.get()); }

  ConstReverseIterator rbegin() const noexcept
  { return std::make_reverse_iterator(end()); }

  ConstReverseIterator rend() const noexcept
  { return std::make_reverse_iterator(begin()); }

  template<typename Iter>
  ConstIterator begin(const Iter& root) const noexcept
  {
    CHECK(isValid(root));
    return ConstIterator(root.node, root.node);
  }

  template<typename Iter>
  ConstIterator end(const Iter& root) const noexcept
  {
    CHECK(isValid(root));
    return ConstIterator(nullptr, root.node);
  }

  template<typename Iter>
  ConstReverseIterator rbegin(const Iter& root) const noexcept
  { return std::make_reverse_iterator(end(root)); }

  template<typename Iter>
  ConstReverseIterator rend(const Iter& root) const noexcept
  { return std::make_reverse_iterator(begin(root)); }

  ConstIterator cbegin() const noexcept
  { return begin(); }

  ConstIterator cend() const noexcept
  { return end(); }

  ConstReverseIterator crbegin() const noexcept
  { return std::make_reverse_iterator(cend()); }

  ConstReverseIterator crend() const noexcept
  { return std::make_reverse_iterator(cbegin()); }

  template<typename Iter>
  ConstIterator cbegin(const Iter& root) const noexcept
  { return begin(root); }

  template<typename Iter>
  ConstIterator cend(const Iter& root) const noexcept
  { return end(root); }

  template<typename Iter>
  ConstReverseIterator crbegin(const Iter& root) const noexcept
  { return std::make_reverse_iterator(cend(root)); }

  template<typename Iter>
  ConstReverseIterator crend(const Iter& root) const noexcept
  { return std::make_reverse_iterator(cbegin(root)); }

  PostOrderIterator beginPostOrder() noexcept
  {
    TreeNode* n = m_head->nextSibling;
    if (n != m_tail.get()) {
      while (n->firstChild)
        n = n->firstChild;
    }
    return PostOrderIterator(n);
  }

  PostOrderIterator endPostOrder() noexcept
  { return PostOrderIterator(m_tail.get()); }

  ReversePostOrderIterator rbeginPostOrder() noexcept
  { return std::make_reverse_iterator(endPostOrder()); }

  ReversePostOrderIterator rendPostOrder() noexcept
  { return std::make_reverse_iterator(beginPostOrder()); }

  template<typename Iter>
  PostOrderIterator beginPostOrder(const Iter& root) noexcept
  {
    CHECK(isValid(root));
    TreeNode* n = root.node;
    while (n->firstChild)
      n = n->firstChild;
    return PostOrderIterator(n, root.node);
  }

  template<typename Iter>
  PostOrderIterator endPostOrder(const Iter& root) noexcept
  {
    CHECK(isValid(root));
    return PostOrderIterator(nullptr, root.node);
  }

  template<typename Iter>
  ReversePostOrderIterator rbeginPostOrder(const Iter& root) noexcept
  { return std::make_reverse_iterator(endPostOrder(root)); }

  template<typename Iter>
  ReversePostOrderIterator rendPostOrder(const Iter& root) noexcept
  { return std::make_reverse_iterator(beginPostOrder(root)); }

  ConstPostOrderIterator beginPostOrder() const noexcept
  {
    TreeNode* n = m_head->nextSibling;
    if (n != m_tail.get()) {
      while (n->firstChild)
        n = n->firstChild;
    }
    return ConstPostOrderIterator(n);
  }

  ConstPostOrderIterator endPostOrder() const noexcept
  { return ConstPostOrderIterator(m_tail.get()); }

  ConstReversePostOrderIterator rbeginPostOrder() const noexcept
  { return std::make_reverse_iterator(endPostOrder()); }

  ConstReversePostOrderIterator rendPostOrder() const noexcept
  { return std::make_reverse_iterator(beginPostOrder()); }

  template<typename Iter>
  ConstPostOrderIterator beginPostOrder(const Iter& root) const noexcept
  {
    CHECK(isValid(root));
    const TreeNode* n = root.node;
    while (n->firstChild)
      n = n->firstChild;
    return ConstPostOrderIterator(n, root.node);
  }

  template<typename Iter>
  ConstPostOrderIterator endPostOrder(const Iter& root) const noexcept
  {
    CHECK(isValid(root));
    return ConstPostOrderIterator(nullptr, root.node);
  }

  template<typename Iter>
  ConstReversePostOrderIterator rbeginPostOrder(const Iter& root) const noexcept
  { return std::make_reverse_iterator(endPostOrder(root)); }

  template<typename Iter>
  ConstReversePostOrderIterator rendPostOrder(const Iter& root) const noexcept
  { return std::make_reverse_iterator(beginPostOrder(root)); }

  ConstPostOrderIterator cbeginPostOrder() const noexcept
  { return beginPostOrder(); }

  ConstPostOrderIterator cendPostOrder() const noexcept
  { return endPostOrder(); }

  ConstReversePostOrderIterator crbeginPostOrder() const noexcept
  { return std::make_reverse_iterator(cendPostOrder()); }

  ConstReversePostOrderIterator crendPostOrder() const noexcept
  { return std::make_reverse_iterator(cbeginPostOrder()); }

  template<typename Iter>
  ConstPostOrderIterator cbeginPostOrder(const Iter& root) const noexcept
  { return beginPostOrder(root); }

  template<typename Iter>
  ConstPostOrderIterator cendPostOrder(const Iter& root) const noexcept
  { return endPostOrder(root); }

  template<typename Iter>
  ConstReversePostOrderIterator crbeginPostOrder(const Iter& root) const noexcept
  { return std::make_reverse_iterator(cendPostOrder(root)); }

  template<typename Iter>
  ConstReversePostOrderIterator crendPostOrder(const Iter& root) const noexcept
  { return std::make_reverse_iterator(cbeginPostOrder(root)); }

  BreadthFirstIterator beginBreadthFirst() noexcept
  { return BreadthFirstIterator(m_head->nextSibling); }

  BreadthFirstIterator endBreadthFirst() noexcept
  { return BreadthFirstIterator(m_tail.get()); }

  ReverseBreadthFirstIterator rbeginBreadthFirst() noexcept
  { return std::make_reverse_iterator(endBreadthFirst()); }

  ReverseBreadthFirstIterator rendBreadthFirst() noexcept
  { return std::make_reverse_iterator(beginBreadthFirst()); }

  template<typename Iter>
  BreadthFirstIterator beginBreadthFirst(const Iter& root) noexcept
  {
    CHECK(isValid(root));
    return BreadthFirstIterator(root.node, root.node);
  }

  template<typename Iter>
  BreadthFirstIterator endBreadthFirst(const Iter& root) noexcept
  {
    CHECK(isValid(root));
    return BreadthFirstIterator(nullptr, root.node);
  }

  template<typename Iter>
  ReverseBreadthFirstIterator rbeginBreadthFirst(const Iter& root) noexcept
  { return std::make_reverse_iterator(endBreadthFirst(root)); }

  template<typename Iter>
  ReverseBreadthFirstIterator rendBreadthFirst(const Iter& root) noexcept
  { return std::make_reverse_iterator(beginBreadthFirst(root)); }

  ConstBreadthFirstIterator beginBreadthFirst() const noexcept
  { return ConstBreadthFirstIterator(m_head->nextSibling); }

  ConstBreadthFirstIterator endBreadthFirst() const noexcept
  { return ConstBreadthFirstIterator(m_tail.get()); }

  ConstReverseBreadthFirstIterator rbeginBreadthFirst() const noexcept
  { return std::make_reverse_iterator(endBreadthFirst()); }

  ConstReverseBreadthFirstIterator rendBreadthFirst() const noexcept
  { return std::make_reverse_iterator(beginBreadthFirst()); }

  template<typename Iter>
  ConstBreadthFirstIterator beginBreadthFirst(const Iter& root) const noexcept
  {
    CHECK(isValid(root));
    return ConstBreadthFirstIterator(root.node, root.node);
  }

  template<typename Iter>
  ConstBreadthFirstIterator endBreadthFirst(const Iter& root) const noexcept
  {
    CHECK(isValid(root));
    return ConstBreadthFirstIterator(nullptr, root.node);
  }

  template<typename Iter>
  ConstReverseBreadthFirstIterator rbeginBreadthFirst(const Iter& root) const noexcept
  { return std::make_reverse_iterator(endBreadthFirst(root)); }

  template<typename Iter>
  ConstReverseBreadthFirstIterator rendBreadthFirst(const Iter& root) const noexcept
  { return std::make_reverse_iterator(beginBreadthFirst(root)); }

  ConstBreadthFirstIterator cbeginBreadthFirst() const noexcept
  { return beginBreadthFirst(); }

  ConstBreadthFirstIterator cendBreadthFirst() const noexcept
  { return endBreadthFirst(); }

  ConstReverseBreadthFirstIterator crbeginBreadthFirst() const noexcept
  { return std::make_reverse_iterator(cendBreadthFirst()); }

  ConstReverseBreadthFirstIterator crendBreadthFirst() const noexcept
  { return std::make_reverse_iterator(cbeginBreadthFirst()); }

  template<typename Iter>
  ConstBreadthFirstIterator cbeginBreadthFirst(const Iter& root) const noexcept
  { return beginBreadthFirst(root); }

  template<typename Iter>
  ConstBreadthFirstIterator cendBreadthFirst(const Iter& root) const noexcept
  { return endBreadthFirst(root); }

  template<typename Iter>
  ConstReverseBreadthFirstIterator crbeginBreadthFirst(const Iter& root) const noexcept
  { return std::make_reverse_iterator(cendBreadthFirst(root)); }

  template<typename Iter>
  ConstReverseBreadthFirstIterator crendBreadthFirst(const Iter& root) const noexcept
  { return std::make_reverse_iterator(cbeginBreadthFirst(root)); }

  RootIterator beginRoot() noexcept
  { return RootIterator(m_head->nextSibling); }

  RootIterator endRoot() noexcept
  { return RootIterator(m_tail.get()); }

  ReverseRootIterator rbeginRoot() noexcept
  { return std::make_reverse_iterator(endRoot()); }

  ReverseRootIterator rendRoot() noexcept
  { return std::make_reverse_iterator(beginRoot()); }

  ConstRootIterator beginRoot() const noexcept
  { return ConstRootIterator(m_head->nextSibling); }

  ConstRootIterator endRoot() const noexcept
  { return ConstRootIterator(m_tail.get()); }

  ConstReverseRootIterator rbeginRoot() const noexcept
  { return std::make_reverse_iterator(endRoot()); }

  ConstReverseRootIterator rendRoot() const noexcept
  { return std::make_reverse_iterator(beginRoot()); }

  ConstRootIterator cbeginRoot() const noexcept
  { return beginRoot(); }

  ConstRootIterator cendRoot() const noexcept
  { return endRoot(); }

  ConstReverseRootIterator crbeginRoot() const noexcept
  { return std::make_reverse_iterator(cendRoot()); }

  ConstReverseRootIterator crendRoot() const noexcept
  { return std::make_reverse_iterator(cbeginRoot()); }

  template<typename Iter>
  ChildIterator beginChild(const Iter& parent) noexcept
  {
    CHECK(isValid(parent));
    return ChildIterator(parent.node->firstChild, parent.node);
  }

  template<typename Iter>
  ChildIterator endChild(const Iter& parent) noexcept
  {
    CHECK(isValid(parent));
    return ChildIterator(nullptr, parent.node);
  }

  template<typename Iter>
  ReverseChildIterator rbeginChild(const Iter& parent) noexcept
  { return std::make_reverse_iterator(endChild(parent)); }

  template<typename Iter>
  ReverseChildIterator rendChild(const Iter& parent) noexcept
  { return std::make_reverse_iterator(beginChild(parent)); }

  template<typename Iter>
  ConstChildIterator beginChild(const Iter& parent) const noexcept
  {
    CHECK(isValid(parent));
    return ConstChildIterator(parent.node->firstChild, parent.node);
  }

  template<typename Iter>
  ConstChildIterator endChild(const Iter& parent) const noexcept
  {
    CHECK(isValid(parent));
    return ConstChildIterator(nullptr, parent.node);
  }

  template<typename Iter>
  ConstReverseChildIterator rbeginChild(const Iter& parent) const noexcept
  { return std::make_reverse_iterator(endChild(parent)); }

  template<typename Iter>
  ConstReverseChildIterator rendChild(const Iter& parent) const noexcept
  { return std::make_reverse_iterator(beginChild(parent)); }

  template<typename Iter>
  ConstChildIterator cbeginChild(const Iter& parent) const noexcept
  { return beginChild(parent); }

  template<typename Iter>
  ConstChildIterator cendChild(const Iter& parent) const noexcept
  { return endChild(parent); }

  template<typename Iter>
  ConstReverseChildIterator crbeginChild(const Iter& parent) const noexcept
  { return std::make_reverse_iterator(cendChild(parent)); }

  template<typename Iter>
  ConstReverseChildIterator crendChild(const Iter& parent) const noexcept
  { return std::make_reverse_iterator(cbeginChild(parent)); }

  template<typename Iter>
  AncestorIterator beginAncestor(const Iter& child) noexcept
  {
    CHECK(isValid(child));
    return AncestorIterator(child.node->parent, child.node);
  }

  template<typename Iter>
  AncestorIterator endAncestor(const Iter& child) noexcept
  {
    CHECK(isValid(child));
    return AncestorIterator(nullptr, child.node);
  }

  template<typename Iter>
  ReverseAncestorIterator rbeginAncestor(const Iter& child) noexcept
  { return std::make_reverse_iterator(endAncestor(child)); }

  template<typename Iter>
  ReverseAncestorIterator rendAncestor(const Iter& child) noexcept
  { return std::make_reverse_iterator(beginAncestor(child)); }

  template<typename Iter>
  ConstAncestorIterator beginAncestor(const Iter& child) const noexcept
  {
    CHECK(isValid(child));
    return ConstAncestorIterator(child.node->parent, child.node);
  }

  template<typename Iter>
  ConstAncestorIterator endAncestor(const Iter& child) const noexcept
  {
    CHECK(isValid(child));
    return ConstAncestorIterator(nullptr, child.node);
  }

  template<typename Iter>
  ConstReverseAncestorIterator rbeginAncestor(const Iter& child) const noexcept
  { return std::make_reverse_iterator(endAncestor(child)); }

  template<typename Iter>
  ConstReverseAncestorIterator rendAncestor(const Iter& child) const noexcept
  { return std::make_reverse_iterator(beginAncestor(child)); }

  template<typename Iter>
  ConstAncestorIterator cbeginAncestor(const Iter& child) const noexcept
  { return beginAncestor(child); }

  template<typename Iter>
  ConstAncestorIterator cendAncestor(const Iter& child) const noexcept
  { return endAncestor(child); }

  template<typename Iter>
  ConstReverseAncestorIterator crbeginAncestor(const Iter& child) const noexcept
  { return std::make_reverse_iterator(cendAncestor(child)); }

  template<typename Iter>
  ConstReverseAncestorIterator crendAncestor(const Iter& child) const noexcept
  { return std::make_reverse_iterator(cbeginAncestor(child)); }

  LeafIterator beginLeaf() noexcept
  {
    TreeNode* n = m_head->nextSibling;
    if (n != m_tail.get()) {
      while (n->firstChild)
        n = n->firstChild;
    }
    return LeafIterator(n);
  }

  LeafIterator endLeaf() noexcept
  { return LeafIterator(m_tail.get()); }

  ReverseLeafIterator rbeginLeaf() noexcept
  { return std::make_reverse_iterator(endLeaf()); }

  ReverseLeafIterator rendLeaf() noexcept
  { return std::make_reverse_iterator(beginLeaf()); }

  template<typename Iter>
  LeafIterator beginLeaf(const Iter& root) noexcept
  {
    CHECK(isValid(root));
    TreeNode* n = root.node;
    while (n->firstChild)
      n = n->firstChild;
    return LeafIterator(n, root.node);
  }

  template<typename Iter>
  LeafIterator endLeaf(const Iter& root) noexcept
  {
    CHECK(isValid(root));
    return LeafIterator(nullptr, root.node);
  }

  template<typename Iter>
  ReverseLeafIterator rbeginLeaf(const Iter& root) noexcept
  { return std::make_reverse_iterator(endLeaf(root)); }

  template<typename Iter>
  ReverseLeafIterator rendLeaf(const Iter& root) noexcept
  { return std::make_reverse_iterator(beginLeaf(root)); }

  ConstLeafIterator beginLeaf() const noexcept
  {
    TreeNode* n = m_head->nextSibling;
    if (n != m_tail.get()) {
      while (n->firstChild)
        n = n->firstChild;
    }
    return ConstLeafIterator(n);
  }

  ConstLeafIterator endLeaf() const noexcept
  { return ConstLeafIterator(m_tail.get()); }

  ConstReverseLeafIterator rbeginLeaf() const noexcept
  { return std::make_reverse_iterator(endLeaf()); }

  ConstReverseLeafIterator rendLeaf() const noexcept
  { return std::make_reverse_iterator(beginLeaf()); }

  template<typename Iter>
  ConstLeafIterator beginLeaf(const Iter& root) const noexcept
  {
    CHECK(isValid(root));
    const TreeNode* n = root.node;
    while (n->firstChild)
      n = n->firstChild;
    return ConstLeafIterator(n, root.node);
  }

  template<typename Iter>
  ConstLeafIterator endLeaf(const Iter& root) const noexcept
  {
    CHECK(isValid(root));
    return ConstLeafIterator(nullptr, root.node);
  }

  template<typename Iter>
  ConstReverseLeafIterator rbeginLeaf(const Iter& root) const noexcept
  { return std::make_reverse_iterator(endLeaf(root)); }

  template<typename Iter>
  ConstReverseLeafIterator rendLeaf(const Iter& root) const noexcept
  { return std::make_reverse_iterator(beginLeaf(root)); }

  ConstLeafIterator cbeginLeaf() const noexcept
  { return beginLeaf(); }

  ConstLeafIterator cendLeaf() const noexcept
  { return endLeaf(); }

  ConstReverseLeafIterator crbeginLeaf() const noexcept
  { return std::make_reverse_iterator(cendLeaf()); }

  ConstReverseLeafIterator crendLeaf() const noexcept
  { return std::make_reverse_iterator(cbeginLeaf()); }

  template<typename Iter>
  ConstLeafIterator cbeginLeaf(const Iter& root) const noexcept
  { return beginLeaf(root); }

  template<typename Iter>
  ConstLeafIterator cendLeaf(const Iter& root) const noexcept
  { return endLeaf(root); }

  template<typename Iter>
  ConstReverseLeafIterator crbeginLeaf(const Iter& root) const noexcept
  { return std::make_reverse_iterator(cendLeaf(root)); }

  template<typename Iter>
  ConstReverseLeafIterator crendLeaf(const Iter& root) const noexcept
  { return std::make_reverse_iterator(cbeginLeaf(root)); }

  // for range for loop, otherwise range for loop only works for pre order (the default iterator and begin()/end())
  // preOrder
  auto preOrderRange() noexcept
  { return ranges::make_subrange(begin(), end()); }

  auto rPreOrderRange() noexcept
  { return ranges::make_subrange(rbegin(), rend()); }

  auto preOrderRange() const noexcept
  { return ranges::make_subrange(begin(), end()); }

  auto rPreOrderRange() const noexcept
  { return ranges::make_subrange(rbegin(), rend()); }

  auto cPreOrderRange() const noexcept
  { return ranges::make_subrange(cbegin(), cend()); }

  auto crPreOrderRange() const noexcept
  { return ranges::make_subrange(crbegin(), crend()); }

  template<typename Iter>
  auto preOrderRange(const Iter& root) noexcept
  { return ranges::make_subrange(begin(root), end(root)); }

  template<typename Iter>
  auto rPreOrderRange(const Iter& root) noexcept
  { return ranges::make_subrange(rbegin(root), rend(root)); }

  template<typename Iter>
  auto preOrderRange(const Iter& root) const noexcept
  { return ranges::make_subrange(begin(root), end(root)); }

  template<typename Iter>
  auto rPreOrderRange(const Iter& root) const noexcept
  { return ranges::make_subrange(rbegin(root), rend(root)); }

  template<typename Iter>
  auto cPreOrderRange(const Iter& root) const noexcept
  { return ranges::make_subrange(cbegin(root), cend(root)); }

  template<typename Iter>
  auto crPreOrderRange(const Iter& root) const noexcept
  { return ranges::make_subrange(crbegin(root), crend(root)); }

  // postOrder
  auto postOrderRange() noexcept
  { return ranges::make_subrange(beginPostOrder(), endPostOrder()); }

  auto rPostOrderRange() noexcept
  { return ranges::make_subrange(rbeginPostOrder(), rendPostOrder()); }

  auto postOrderRange() const noexcept
  { return ranges::make_subrange(beginPostOrder(), endPostOrder()); }

  auto rPostOrderRange() const noexcept
  { return ranges::make_subrange(rbeginPostOrder(), rendPostOrder()); }

  auto cPostOrderRange() const noexcept
  { return ranges::make_subrange(cbeginPostOrder(), cendPostOrder()); }

  auto crPostOrderRange() const noexcept
  { return ranges::make_subrange(crbeginPostOrder(), crendPostOrder()); }

  template<typename Iter>
  auto postOrderRange(const Iter& root) noexcept
  { return ranges::make_subrange(beginPostOrder(root), endPostOrder(root)); }

  template<typename Iter>
  auto rPostOrderRange(const Iter& root) noexcept
  { return ranges::make_subrange(rbeginPostOrder(root), rendPostOrder(root)); }

  template<typename Iter>
  auto postOrderRange(const Iter& root) const noexcept
  { return ranges::make_subrange(beginPostOrder(root), endPostOrder(root)); }

  template<typename Iter>
  auto rPostOrderRange(const Iter& root) const noexcept
  { return ranges::make_subrange(rbeginPostOrder(root), rendPostOrder(root)); }

  template<typename Iter>
  auto cPostOrderRange(const Iter& root) const noexcept
  { return ranges::make_subrange(cbeginPostOrder(root), cendPostOrder(root)); }

  template<typename Iter>
  auto crPostOrderRange(const Iter& root) const noexcept
  { return ranges::make_subrange(crbeginPostOrder(root), crendPostOrder(root)); }

  // breadth first order
  auto breadthFirstRange() noexcept
  { return ranges::make_subrange(beginBreadthFirst(), endBreadthFirst()); }

  auto rBreadthFirstRange() noexcept
  { return ranges::make_subrange(rbeginBreadthFirst(), rendBreadthFirst()); }

  auto breadthFirstRange() const noexcept
  { return ranges::make_subrange(beginBreadthFirst(), endBreadthFirst()); }

  auto rBreadthFirstRange() const noexcept
  { return ranges::make_subrange(rbeginBreadthFirst(), rendBreadthFirst()); }

  auto cBreadthFirstRange() const noexcept
  { return ranges::make_subrange(cbeginBreadthFirst(), cendBreadthFirst()); }

  auto crBreadthFirstRange() const noexcept
  { return ranges::make_subrange(crbeginBreadthFirst(), crendBreadthFirst()); }

  template<typename Iter>
  auto breadthFirstRange(const Iter& root) noexcept
  { return ranges::make_subrange(beginBreadthFirst(root), endBreadthFirst(root)); }

  template<typename Iter>
  auto rBreadthFirstRange(const Iter& root) noexcept
  { return ranges::make_subrange(rbeginBreadthFirst(root), rendBreadthFirst(root)); }

  template<typename Iter>
  auto breadthFirstRange(const Iter& root) const noexcept
  { return ranges::make_subrange(beginBreadthFirst(root), endBreadthFirst(root)); }

  template<typename Iter>
  auto rBreadthFirstRange(const Iter& root) const noexcept
  { return ranges::make_subrange(rbeginBreadthFirst(root), rendBreadthFirst(root)); }

  template<typename Iter>
  auto cBreadthFirstRange(const Iter& root) const noexcept
  { return ranges::make_subrange(cbeginBreadthFirst(root), cendBreadthFirst(root)); }

  template<typename Iter>
  auto crBreadthFirstRange(const Iter& root) const noexcept
  { return ranges::make_subrange(crbeginBreadthFirst(root), crendBreadthFirst(root)); }

  // root
  auto rootRange() noexcept
  { return ranges::make_subrange(beginRoot(), endRoot()); }

  auto rRootRange() noexcept
  { return ranges::make_subrange(rbeginRoot(), rendRoot()); }

  auto rootRange() const noexcept
  { return ranges::make_subrange(beginRoot(), endRoot()); }

  auto rRootRange() const noexcept
  { return ranges::make_subrange(rbeginRoot(), rendRoot()); }

  auto cRootRange() const noexcept
  { return ranges::make_subrange(cbeginRoot(), cendRoot()); }

  auto crRootRange() const noexcept
  { return ranges::make_subrange(crbeginRoot(), crendRoot()); }

  // child
  template<typename Iter>
  auto childRange(const Iter& parent) noexcept
  { return ranges::make_subrange(beginChild(parent), endChild(parent)); }

  template<typename Iter>
  auto rChildRange(const Iter& parent) noexcept
  { return ranges::make_subrange(rbeginChild(parent), rendChild(parent)); }

  template<typename Iter>
  auto childRange(const Iter& parent) const noexcept
  { return ranges::make_subrange(beginChild(parent), endChild(parent)); }

  template<typename Iter>
  auto rChildRange(const Iter& parent) const noexcept
  { return ranges::make_subrange(rbeginChild(parent), rendChild(parent)); }

  template<typename Iter>
  auto cChildRange(const Iter& parent) const noexcept
  { return ranges::make_subrange(cbeginChild(parent), cendChild(parent)); }

  template<typename Iter>
  auto crChildRange(const Iter& parent) const noexcept
  { return ranges::make_subrange(crbeginChild(parent), crendChild(parent)); }

  // Ancestor
  template<typename Iter>
  auto ancestorRange(const Iter& child) noexcept
  { return ranges::make_subrange(beginAncestor(child), endAncestor(child)); }

  template<typename Iter>
  auto rAncestorRange(const Iter& child) noexcept
  { return ranges::make_subrange(rbeginAncestor(child), rendAncestor(child)); }

  template<typename Iter>
  auto ancestorRange(const Iter& child) const noexcept
  { return ranges::make_subrange(beginAncestor(child), endAncestor(child)); }

  template<typename Iter>
  auto rAncestorRange(const Iter& child) const noexcept
  { return ranges::make_subrange(rbeginAncestor(child), rendAncestor(child)); }

  template<typename Iter>
  auto cAncestorRange(const Iter& child) const noexcept
  { return ranges::make_subrange(cbeginAncestor(child), cendAncestor(child)); }

  template<typename Iter>
  auto crAncestorRange(const Iter& child) const noexcept
  { return ranges::make_subrange(crbeginAncestor(child), crendAncestor(child)); }

  // Leaf
  auto leafRange() noexcept
  { return ranges::make_subrange(beginLeaf(), endLeaf()); }

  auto rLeafRange() noexcept
  { return ranges::make_subrange(rbeginLeaf(), rendLeaf()); }

  auto leafRange() const noexcept
  { return ranges::make_subrange(beginLeaf(), endLeaf()); }

  auto rLeafRange() const noexcept
  { return ranges::make_subrange(rbeginLeaf(), rendLeaf()); }

  auto cLeafRange() const noexcept
  { return ranges::make_subrange(cbeginLeaf(), cendLeaf()); }

  auto crLeafRange() const noexcept
  { return ranges::make_subrange(crbeginLeaf(), crendLeaf()); }

  template<typename Iter>
  auto leafRange(const Iter& root) noexcept
  { return ranges::make_subrange(beginLeaf(root), endLeaf(root)); }

  template<typename Iter>
  auto rLeafRange(const Iter& root) noexcept
  { return ranges::make_subrange(rbeginLeaf(root), rendLeaf(root)); }

  template<typename Iter>
  auto leafRange(const Iter& root) const noexcept
  { return ranges::make_subrange(beginLeaf(root), endLeaf(root)); }

  template<typename Iter>
  auto rLeafRange(const Iter& root) const noexcept
  { return ranges::make_subrange(rbeginLeaf(root), rendLeaf(root)); }

  template<typename Iter>
  auto cLeafRange(const Iter& root) const noexcept
  { return ranges::make_subrange(cbeginLeaf(root), cendLeaf(root)); }

  template<typename Iter>
  auto crLeafRange(const Iter& root) const noexcept
  { return ranges::make_subrange(crbeginLeaf(root), crendLeaf(root)); }

  [[nodiscard]] size_t size() const
  { return std::distance(begin(), end()); }

  // size of subtree
  template<typename Iter>
  size_t size(const Iter& parent) const
  { return std::distance(begin(parent), end(parent)); }

  [[nodiscard]] size_t numRoots() const
  { return std::distance(beginRoot(), endRoot()); }

  [[nodiscard]] size_t numLeafs() const
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

  [[nodiscard]] bool empty() const
  { return m_head->nextSibling == m_tail.get(); }

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
      PostOrderIterator it = beginPostOrder();
      PostOrderIterator tmp;
      PostOrderIterator end = endPostOrder();
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
    PostOrderIterator it = beginPostOrder(root);
    PostOrderIterator tmp;
    PostOrderIterator end = endPostOrder(root);
    while (it != end) {
      tmp = it++;
      erase(tmp);
    }
  }

  template<typename Iter>
  void eraseChildren(Iter root)
  {
    PostOrderIterator it = beginPostOrder(root);
    PostOrderIterator tmp;
    PostOrderIterator end = endPostOrder(root);
    --end;
    while (it != end) {
      tmp = it++;
      erase(tmp);
    }
  }

  Iterator appendRoot(const T& v)
  {
    auto node = new TreeNode(v);
    node->prevSibling = m_tail->prevSibling;
    node->nextSibling = m_tail.get();
    m_tail->prevSibling->nextSibling = node;
    m_tail->prevSibling = node;
    return Iterator(node);
  }

  Iterator prependRoot(const T& v)
  {
    auto node = new TreeNode(v);
    node->prevSibling = m_head;
    node->nextSibling = m_head->nextSibling;
    m_head->nextSibling->prevSibling = node;
    m_head->nextSibling = node;
    return Iterator(node);
  }

  template<typename Iter>
  Iter appendChild(Iter parent, const T& v)
  {
    CHECK(isValid(parent));
    auto node = new TreeNode(v);
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
    CHECK(isValid(parent) && isValid(child));
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
    CHECK(isValid(parent));
    auto node = new TreeNode(v);
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
    CHECK(isValid(parent) && isValid(child));
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
    CHECK(isValid(to) && isValid(from));
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
    CHECK(isValid(pos));
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
    CHECK(isValid(pos));
    if (isRoot(pos))
      return;
    std::vector<AncestorIterator> chain;
    chain.push_back(pos);
    for (AncestorIterator it = beginAncestor(pos); it != endAncestor(pos); ++it) {
      chain.push_back(it);
    }
    for (size_t i = chain.size(); i-- > 1;) {
      reverseChildRoot(chain[i - 1], chain[i]);
    }
  }

  // note: return Null Iter if n1 and n2 has different root
  template<typename Iter, typename Iter2>
  Iter lowestCommonAncestor(Iter n1, Iter2 n2)
  {
    std::vector<AncestorIterator> chain1;
    for (AncestorIterator it = beginAncestor(n1); it != endAncestor(n1); ++it) {
      chain1.push_back(it);
    }
    std::vector<AncestorIterator> chain2;
    for (AncestorIterator it = beginAncestor(n2); it != endAncestor(n2); ++it) {
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
    m_head = std::make_unique<TreeNode>();
    m_tail = std::make_unique<TreeNode>();

    m_head->nextSibling = m_tail.get();
    m_tail->prevSibling = m_head.get();
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
    CHECK(isValid(child) && isValid(root));
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

  template<typename Iter>
  inline bool isValid(Iter it) const
  {
    return it.node && it.node != m_head.get() && it.node != m_tail.get();
  }

protected:
  // head --- root1 --- root2 --- root3 --- ... --- tail
  //          / | \     / | \     / |
  //         nodes...  nodes...  nodes...
  //
  std::unique_ptr<TreeNode> m_head;
  std::unique_ptr<TreeNode> m_tail;
};

}  // namespace nim
