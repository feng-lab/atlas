#include "zneutubelocsegchain.h"

#include "zlog.h"

namespace nim {

LocsegNode* LocsegChain::head()
{
  return _nodes.empty() ? nullptr : &*_nodes.begin();
}

const LocsegNode* LocsegChain::head() const
{
  return _nodes.empty() ? nullptr : &*_nodes.begin();
}

LocsegNode* LocsegChain::tail()
{
  return _nodes.empty() ? nullptr : &*_nodes.rbegin();
}

const LocsegNode* LocsegChain::tail() const
{
  return _nodes.empty() ? nullptr : &*_nodes.rbegin();
}

LocalNeuroseg* LocsegChain::headSeg()
{
  auto* node = head();
  return node ? &node->locseg : nullptr;
}

const LocalNeuroseg* LocsegChain::headSeg() const
{
  auto* node = head();
  return node ? &node->locseg : nullptr;
}

LocalNeuroseg* LocsegChain::tailSeg()
{
  auto* node = tail();
  return node ? &node->locseg : nullptr;
}

const LocalNeuroseg* LocsegChain::tailSeg() const
{
  auto* node = tail();
  return node ? &node->locseg : nullptr;
}

LocsegNode* LocsegChain::nodeAt(int index)
{
  if (index < 0 || index >= length()) {
    return nullptr;
  }
  auto it = _nodes.begin();
  std::advance(it, index);
  return &*it;
}

const LocsegNode* LocsegChain::nodeAt(int index) const
{
  if (index < 0 || index >= length()) {
    return nullptr;
  }
  auto it = _nodes.begin();
  std::advance(it, index);
  return &*it;
}

LocalNeuroseg* LocsegChain::segAt(int index)
{
  auto* node = nodeAt(index);
  return node ? &node->locseg : nullptr;
}

const LocalNeuroseg* LocsegChain::segAt(int index) const
{
  auto* node = nodeAt(index);
  return node ? &node->locseg : nullptr;
}

LocsegNode* LocsegChain::insertNodeAt(int index, LocsegNode node)
{
  if (_nodes.empty()) {
    _nodes.push_back(std::move(node));
    return &*_nodes.begin();
  }

  if (index <= 0) {
    _nodes.push_front(std::move(node));
    return &*_nodes.begin();
  }

  if (index >= length()) {
    _nodes.push_back(std::move(node));
    return &*_nodes.rbegin();
  }

  auto it = _nodes.begin();
  std::advance(it, index);
  const auto inserted = _nodes.insert(it, std::move(node));
  return &*inserted;
}

LocsegNode* LocsegChain::addNode(LocsegNode node, LocsegChainEndLegacyLike end)
{
  if (end == LocsegChainEndLegacyLike::Head) {
    _nodes.push_front(std::move(node));
    return &*_nodes.begin();
  }

  CHECK(end == LocsegChainEndLegacyLike::Tail);
  _nodes.push_back(std::move(node));
  return &*_nodes.rbegin();
}

void LocsegChain::removeEnd(LocsegChainEndLegacyLike end)
{
  if (_nodes.empty()) {
    return;
  }

  if (end == LocsegChainEndLegacyLike::Head) {
    _nodes.pop_front();
    return;
  }

  CHECK(end == LocsegChainEndLegacyLike::Tail);
  _nodes.pop_back();
}

int locsegChainHitTestLegacyLike(const LocsegChain& chain, TraceDirection direction, double x, double y, double z)
{
  // Port of tz_locseg_chain.c::Locseg_Chain_Hit_Test().
  CHECK(direction == TraceDirection::Forward || direction == TraceDirection::Backward)
    << "Unexpected TraceDirection for locsegChainHitTestLegacyLike: " << static_cast<int>(direction);

  int i = 1;
  if (direction == TraceDirection::Forward) {
    for (const auto& node : chain) {
      if (localNeurosegHitTestLegacyLike(node.locseg, x, y, z)) {
        return i;
      }
      ++i;
    }
    return 0;
  }

  for (auto it = chain.end(); it != chain.begin();) {
    --it;
    if (localNeurosegHitTestLegacyLike(it->locseg, x, y, z)) {
      return i;
    }
    ++i;
  }

  return 0;
}

bool locsegChainFormLoopLegacyLike(const LocsegChain& chain, const LocalNeuroseg& locseg, TraceDirection direction)
{
  // Port of tz_locseg_chain.c::Locseg_Chain_Form_Loop().
  constexpr double TzPiLegacyLike = 3.14159265358979323846264338328;
  constexpr double TzPi2LegacyLike = TzPiLegacyLike / 2.0;

  if (chain.empty()) {
    return false;
  }

  const std::array<double, 3> center = localNeurosegCenterLegacyLike(locseg);

  if (direction == TraceDirection::Forward) {
    bool skippedTail = false;
    for (auto it = chain.end(); it != chain.begin();) {
      --it;
      if (!skippedTail) {
        skippedTail = true;
        continue;
      }

      const LocalNeuroseg& chainSeg = it->locseg;
      if (localNeurosegHitTestLegacyLike(chainSeg, center[0], center[1], center[2])) {
        const std::array<double, 3> bottom = localNeurosegBottomLegacyLike(locseg);
        if (!localNeurosegHitTestLegacyLike(chainSeg, bottom[0], bottom[1], bottom[2])) {
          return true;
        }

        if (neurosegAngleBetweenLegacyLike(locseg.seg, chainSeg.seg) > TzPi2LegacyLike) {
          return true;
        }
      }
    }
    return false;
  }

  if (direction == TraceDirection::Backward) {
    bool skippedHead = false;
    for (auto it = chain.begin(); it != chain.end(); ++it) {
      if (!skippedHead) {
        skippedHead = true;
        continue;
      }

      const LocalNeuroseg& chainSeg = it->locseg;
      if (localNeurosegHitTestLegacyLike(chainSeg, center[0], center[1], center[2])) {
        const std::array<double, 3> top = localNeurosegTopLegacyLike(locseg);
        if (!localNeurosegHitTestLegacyLike(chainSeg, top[0], top[1], top[2])) {
          return true;
        }

        if (neurosegAngleBetweenLegacyLike(locseg.seg, chainSeg.seg) > TzPi2LegacyLike) {
          return true;
        }
      }
    }
  }

  return false;
}

int locsegChainRemoveOverlapEndsLegacyLike(LocsegChain& chain)
{
  // Port of tz_locseg_chain.c::Locseg_Chain_Remove_Overlap_Ends().
  int nremove = 0;

  if (chain.length() < 2) {
    return nremove;
  }

  {
    auto itHead = chain.begin();
    auto itRunnerUp = itHead;
    ++itRunnerUp;
    CHECK(itRunnerUp != chain.end());

    const LocalNeuroseg& head = itHead->locseg;
    const LocalNeuroseg& runnerUp = itRunnerUp->locseg;

    const std::array<double, 3> top = localNeurosegTopLegacyLike(head);
    const std::array<double, 3> bottom = localNeurosegBottomLegacyLike(head);

    if (localNeurosegHitTestLegacyLike(runnerUp, top[0], top[1], top[2]) &&
        localNeurosegHitTestLegacyLike(runnerUp, bottom[0], bottom[1], bottom[2])) {
      chain.removeEnd(LocsegChainEndLegacyLike::Head);
      ++nremove;
    }
  }

  if (chain.length() < 2) {
    return nremove;
  }

  {
    auto itTail = chain.end();
    --itTail;

    auto itRunnerUp = itTail;
    --itRunnerUp;

    const LocalNeuroseg& tail = itTail->locseg;
    const LocalNeuroseg& runnerUp = itRunnerUp->locseg;

    const std::array<double, 3> top = localNeurosegTopLegacyLike(tail);
    const std::array<double, 3> bottom = localNeurosegBottomLegacyLike(tail);

    if (localNeurosegHitTestLegacyLike(runnerUp, top[0], top[1], top[2]) &&
        localNeurosegHitTestLegacyLike(runnerUp, bottom[0], bottom[1], bottom[2])) {
      chain.removeEnd(LocsegChainEndLegacyLike::Tail);
      ++nremove;
    }
  }

  return nremove;
}

void locsegChainRemoveTurnEndsLegacyLike(LocsegChain& chain, double maxAngle)
{
  // Port of tz_locseg_chain.c::Locseg_Chain_Remove_Turn_Ends().
  constexpr double TzPiLegacyLike = 3.14159265358979323846264338328;
  constexpr double Tz2PiLegacyLike = 2.0 * TzPiLegacyLike;

  if (chain.length() < 2) {
    return;
  }

  {
    auto itHead = chain.begin();
    auto itRunnerUp = itHead;
    ++itRunnerUp;
    CHECK(itRunnerUp != chain.end());

    double angle = neurosegAngleBetweenLegacyLike(itHead->locseg.seg, itRunnerUp->locseg.seg);
    if (angle > TzPiLegacyLike) {
      angle = Tz2PiLegacyLike - angle;
    }
    if (angle > maxAngle) {
      chain.removeEnd(LocsegChainEndLegacyLike::Head);
    }
  }

  if (chain.length() < 2) {
    return;
  }

  {
    auto itTail = chain.end();
    --itTail;

    auto itRunnerUp = itTail;
    --itRunnerUp;

    double angle = neurosegAngleBetweenLegacyLike(itTail->locseg.seg, itRunnerUp->locseg.seg);
    if (angle > TzPiLegacyLike) {
      angle = Tz2PiLegacyLike - angle;
    }
    if (angle > maxAngle) {
      chain.removeEnd(LocsegChainEndLegacyLike::Tail);
    }
  }
}

} // namespace nim
