#include "zimgconnectedcomponents.h"

#include "zimgneighborhooditerator.h"
#include <stack>

namespace nim {

namespace impl {

struct EqualToLabel
{
  explicit EqualToLabel(size_t label)
    : m_label(label)
  {}

  template<typename TVoxel>
  bool operator()(TVoxel v) const
  {
    return v == static_cast<TVoxel>(m_label);
  }

private:
  size_t m_label;
};

} // namespace impl

ConnComp::ConnComp()
{
  clear();
}

void ConnComp::clear()
{
  connectivity = 0;
  channel = 0;
  time = 0;
  imgInfo.clear();
  voxelIdxList.clear();
}

void ConnComp::removeSmallObject(size_t sizeThre, bool includeThre)
{
  if (sizeThre == 0) {
    voxelIdxList.clear();
    return;
  }
  if (includeThre) {
    sizeThre++;
  }
  erase_if(voxelIdxList, [sizeThre](const auto& v) { return v.size() < sizeThre; });
}

size_t ConnComp::toatalNumVoxels() const
{
  size_t res = 0;
  for (const auto& o : voxelIdxList) {
    res += o.size();
  }
  return res;
}

size_t ConnComp::labelImgBytesPerVoxel() const
{
  if (voxelIdxList.size() <= UINT8_MAX) {
    return 1;
  }
  if (voxelIdxList.size() <= UINT16_MAX) {
    return 2;
  }
  if (voxelIdxList.size() <= UINT32_MAX) {
    return 3;
  }
  return 4;
}

// return a label img with smallest possible voxel type, use labelImgBytesPerVoxel to get type
ZImg ConnComp::createLabelImg() const
{
  if (voxelIdxList.size() <= UINT8_MAX) {
    return createTypedLabelImg<uint8_t>();
  } else if (voxelIdxList.size() <= UINT16_MAX) {
    return createTypedLabelImg<uint16_t>();
  } else if (voxelIdxList.size() <= UINT32_MAX) {
    return createTypedLabelImg<uint32_t>();
  } else {
    return createTypedLabelImg<uint64_t>();
  }
}

// user decide what type
template<typename TVoxel>
ZImg ConnComp::createTypedLabelImg() const
{
  ZImgInfo info(imgInfo.width, imgInfo.height, imgInfo.depth);
  info.setVoxelFormat<TVoxel>();
  info.createDefaultDescriptions();
  ZImg res(info);
  auto data = res.channelData<TVoxel>(0, 0);
  for (size_t o = 0; o < voxelIdxList.size(); ++o) {
    for (size_t v = 0; v < voxelIdxList[o].size(); ++v) {
      data[voxelIdxList[o][v]] = o + 1;
    }
  }
  return res;
}

template ZImg ConnComp::createTypedLabelImg<uint8_t>() const;

template ZImg ConnComp::createTypedLabelImg<uint16_t>() const;

template ZImg ConnComp::createTypedLabelImg<uint32_t>() const;

template ZImg ConnComp::createTypedLabelImg<uint64_t>() const;

template ZImg ConnComp::createTypedLabelImg<int8_t>() const;

template ZImg ConnComp::createTypedLabelImg<int16_t>() const;

template ZImg ConnComp::createTypedLabelImg<int32_t>() const;

template ZImg ConnComp::createTypedLabelImg<int64_t>() const;

template ZImg ConnComp::createTypedLabelImg<float>() const;

template ZImg ConnComp::createTypedLabelImg<double>() const;

template<bool ReportProgress>
ConnComp ZImgConnectedComponents<ReportProgress>::run(const ZImg& img, size_t conn, size_t c, size_t t)
{
  ConnComp res = createRes(img, conn, c, t);
  ZImg bimg = img.createView(c, t).binarized();

  getConnectedComponents_Impl(bimg, res, 1);

  return res;
}

template<bool ReportProgress>
ConnComp
ZImgConnectedComponents<ReportProgress>::runLabel(const ZImg& img, size_t conn, size_t label, size_t c, size_t t)
{
  if (img.isType<uint8_t>()) {
    ConnComp res = createRes(img, conn, c, t);
    ZImg bimg = img.extractChannel(c, t);
    getConnectedComponents_Impl(bimg, res, label);
    return res;
  }
  return run(img, conn, c, t, impl::EqualToLabel(label));
}

template<bool ReportProgress>
ConnComp
ZImgConnectedComponents<ReportProgress>::runLabelModifyInput(ZImg& img, size_t conn, size_t label, size_t c, size_t t)
{
  if (img.isType<uint8_t>()) {
    ConnComp res = createRes(img, conn, c, t);
    ZImg bimg = img.createView(c, t);
    getConnectedComponents_Impl(bimg, res, label);
    return res;
  }
  return run(img, conn, c, t, impl::EqualToLabel(label));
}

template<bool ReportProgress>
ConnComp ZImgConnectedComponents<ReportProgress>::createRes(const ZImg& img, size_t conn, size_t c, size_t t) const
{
  if (conn == 0) {
    conn = img.is2DImg() ? 8 : 26;
  } else if (conn != 4 && conn != 8 && conn != 6 && conn != 18 && conn != 26) {
    throw ZException(QString("invalid conn input: %1").arg(conn));
  }
  if (img.is2DImg() && conn != 4 && conn != 8) {
    if (conn == 6) {
      conn = 4;
    } else {
      conn = 8;
    }
  }

  ConnComp res;
  res.channel = c;
  res.time = t;
  res.imgInfo = img.info();
  res.connectivity = conn;

  return res;
}

template<bool ReportProgress>
void ZImgConnectedComponents<ReportProgress>::getConnectedComponents_Impl(ZImg& markerImg, ConnComp& res, size_t label)
{
  if (label > 255) {
    return;
  }

  std::stack<index_t, std::vector<index_t>> stk;
  size_t conn = res.connectivity;
  size_t voxelNumber = markerImg.voxelNumber();
  auto nit = ZImgNeighborhoodConstIterator<uint8_t>(ZNeighborhood(conn), markerImg);
  auto marker = markerImg.timeData<uint8_t>(0);

  std::vector<size_t> idxList;
  index_t idx;
  size_t n;
  uint8_t objLabel = label;
  for (size_t v = 0; v < voxelNumber; ++v) {
    this->reportProgress(static_cast<double>(v) / voxelNumber);

    if (marker[v] == objLabel) { // we found a new object
      idxList.clear();
      idxList.push_back(v);
      // reposition the neighborhood iterator
      nit.goToIndex(v);
      --marker[v]; // mark as visited
      // check each neighbor
      for (n = 0; n < conn; ++n) {
        if (nit.isInBound(n)) {
          idx = nit.index(n);
          if (marker[idx] == objLabel) {
            stk.push(idx);
            --marker[idx];
          }
        }
      }
      while (!stk.empty()) {
        // position the iterator
        nit.goToIndex(stk.top());
        idxList.push_back(stk.top());
        stk.pop();

        // check neighbors
        for (n = 0; n < conn; ++n) {
          if (nit.isInBound(n)) {
            idx = nit.index(n);
            if (marker[idx] == objLabel) {
              stk.push(idx);
              --marker[idx];
            }
          }
        }
      }
      std::sort(idxList.begin(), idxList.end());
      res.voxelIdxList.push_back(idxList);
    }
  }
  this->reportProgress(1.0);
}

template class ZImgConnectedComponents<true>;

template class ZImgConnectedComponents<false>;

} // namespace nim
