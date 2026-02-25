#pragma once

#include "zneutubelocalneuroseg.h"
#include "zneutubetracerecord.h"

#include <list>

namespace nim::neutube {

// C++ port of tz_doubly_linked_list_defs.h::Dlist_End_e.
enum class LocsegChainEndLegacyLike : int
{
  Head = -1,
  Tail = 1
};

// C++ port of tz_locseg_chain_com.h::Locseg_Node.
struct LocsegNode
{
  LocalNeuroseg locseg{};
  TraceRecord tr{};
};

// Minimal C++ container for a locseg chain.
// This mirrors the legacy doubly-linked list usage but uses `std::list`
// for stable node addresses.
class LocsegChain
{
public:
  using Container = std::list<LocsegNode>;
  using Iterator = Container::iterator;
  using ConstIterator = Container::const_iterator;

  [[nodiscard]] bool empty() const
  {
    return _nodes.empty();
  }
  [[nodiscard]] int length() const
  {
    return static_cast<int>(_nodes.size());
  }

  [[nodiscard]] LocsegNode* head();
  [[nodiscard]] const LocsegNode* head() const;
  [[nodiscard]] LocsegNode* tail();
  [[nodiscard]] const LocsegNode* tail() const;

  [[nodiscard]] LocalNeuroseg* headSeg();
  [[nodiscard]] const LocalNeuroseg* headSeg() const;
  [[nodiscard]] LocalNeuroseg* tailSeg();
  [[nodiscard]] const LocalNeuroseg* tailSeg() const;

  [[nodiscard]] LocsegNode* nodeAt(int index);
  [[nodiscard]] const LocsegNode* nodeAt(int index) const;

  [[nodiscard]] LocalNeuroseg* segAt(int index);
  [[nodiscard]] const LocalNeuroseg* segAt(int index) const;

  // Inserts a node before the element at `index` (0-based). Out-of-range indices
  // are clamped to the nearest end (front/back), matching legacy usage patterns.
  // Returns a pointer to the inserted node.
  LocsegNode* insertNodeAt(int index, LocsegNode node);

  // Adds a node at the given end and returns a pointer to the inserted node.
  LocsegNode* addNode(LocsegNode node, LocsegChainEndLegacyLike end);

  void removeEnd(LocsegChainEndLegacyLike end);

  [[nodiscard]] Iterator begin()
  {
    return _nodes.begin();
  }
  [[nodiscard]] Iterator end()
  {
    return _nodes.end();
  }
  [[nodiscard]] ConstIterator begin() const
  {
    return _nodes.begin();
  }
  [[nodiscard]] ConstIterator end() const
  {
    return _nodes.end();
  }

private:
  Container _nodes;
};

// Port of tz_locseg_chain.c::Locseg_Chain_Hit_Test().
[[nodiscard]] int
locsegChainHitTestLegacyLike(const LocsegChain& chain, TraceDirection direction, double x, double y, double z);

// Port of tz_locseg_chain.c::Locseg_Chain_Form_Loop().
[[nodiscard]] bool
locsegChainFormLoopLegacyLike(const LocsegChain& chain, const LocalNeuroseg& locseg, TraceDirection direction);

// Port of tz_locseg_chain.c::Locseg_Chain_Remove_Overlap_Ends().
[[nodiscard]] int locsegChainRemoveOverlapEndsLegacyLike(LocsegChain* chain);

// Port of tz_locseg_chain.c::Locseg_Chain_Remove_Turn_Ends().
void locsegChainRemoveTurnEndsLegacyLike(LocsegChain* chain, double maxAngle);

} // namespace nim::neutube
