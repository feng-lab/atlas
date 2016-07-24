#ifndef ZIMGMETADATABASE_H
#define ZIMGMETADATABASE_H

#include <cstddef>
#include <vector>
#include <map>

namespace nim {

// a template class that contains metadata of type T of a img
// metadata can attach to different level of img (e.g. location metadata,
// time metadata, channel metadata, plane metadata), each attach point can
// contain many data

template<typename T>
class ZImgMetadataBase
{
protected:
  struct _AttachPoint {
    _AttachPoint(int z, int c, int t)
      : z(z), c(c), t(t)
    {}
    int z;
    int c;
    int t;
    bool operator<(const _AttachPoint& rhs) const
    {
      return t < rhs.t ||
          (t == rhs.t && c < rhs.c) ||
          (t == rhs.t && c == rhs.c && z < rhs.z);
    }
  };

public:
  ZImgMetadataBase()
  {}
  virtual ~ZImgMetadataBase()
  {}

  ZImgMetadataBase(ZImgMetadataBase&&) = default;
  ZImgMetadataBase& operator=(ZImgMetadataBase&&) = default;
  ZImgMetadataBase(const ZImgMetadataBase&) = default;
  ZImgMetadataBase& operator=(const ZImgMetadataBase&) = default;

  inline void swap(ZImgMetadataBase &other) noexcept { m_data.swap(other.m_data); }

  inline void clear() { m_data.clear(); }
  inline bool isEmpty() const { return m_data.empty(); }

  inline void attachToTopLevel(const T& d)
  {
    std::vector<T> &list = m_data[_AttachPoint(-1,-1,-1)];
    list.push_back(d);
  }
  inline void attachToTime(const T& d, size_t t)
  {
    std::vector<T> &list = m_data[_AttachPoint(-1,-1,t)];
    list.push_back(d);
  }
  inline void attachToChannel(const T& d, size_t c, size_t t)
  {
    std::vector<T> &list = m_data[_AttachPoint(-1,c,t)];
    list.push_back(d);
  }
  inline void attachToPlane(const T& d, size_t z, size_t t)
  {
    std::vector<T> &list = m_data[_AttachPoint(z,-1,t)];
    list.push_back(d);
  }
  inline void attachToSingleChannelPlane(const T& d, size_t z, size_t c, size_t t)
  {
    std::vector<T> &list = m_data[_AttachPoint(z,c,t)];
    list.push_back(d);
  }

  inline void attachToTopLevel(const std::vector<T>& d)
  {
    std::vector<T> &list = m_data[_AttachPoint(-1,-1,-1)];
    list.insert(list.end(), d.begin(), d.end());
  }
  inline void attachToTime(const std::vector<T>& d, size_t t)
  {
    std::vector<T> &list = m_data[_AttachPoint(-1,-1,t)];
    list.insert(list.end(), d.begin(), d.end());
  }
  inline void attachToChannel(const std::vector<T>& d, size_t c, size_t t)
  {
    std::vector<T> &list = m_data[_AttachPoint(-1,c,t)];
    list.insert(list.end(), d.begin(), d.end());
  }
  inline void attachToPlane(const std::vector<T>& d, size_t z, size_t t)
  {
    std::vector<T> &list = m_data[_AttachPoint(z,-1,t)];
    list.insert(list.end(), d.begin(), d.end());
  }
  inline void attachToSingleChannelPlane(const std::vector<T>& d, size_t z, size_t c, size_t t)
  {
    std::vector<T> &list = m_data[_AttachPoint(z,c,t)];
    list.insert(list.end(), d.begin(), d.end());
  }

  inline void clearTopLevelAttachments() { m_data.erase(_AttachPoint(-1,-1,-1)); }
  inline void clearTimeAttachments(size_t t) { m_data.erase(_AttachPoint(-1,-1,t)); }
  inline void clearChannelAttachments(size_t c, size_t t) { m_data.erase(_AttachPoint(-1,c,t)); }
  inline void clearPlaneAttachments(size_t z, size_t t) { m_data.erase(_AttachPoint(z,-1,t)); }
  inline void clearSingleChannelPlaneAttachments(size_t z, size_t c, size_t t)
  { m_data.erase(_AttachPoint(z,c,t)); }

  inline bool hasTopLevelAttachment() const
  {
    typename std::map<_AttachPoint, std::vector<T>>::const_iterator it = m_data.find(_AttachPoint(-1,-1,-1));
    return it != m_data.end() && !it->second.empty();
  }
  inline bool hasTimeAttachment(size_t t) const
  {
    typename std::map<_AttachPoint, std::vector<T>>::const_iterator it = m_data.find(_AttachPoint(-1,-1,t));
    return it != m_data.end() && !it->second.empty();
  }
  inline bool hasChannelAttachment(size_t c, size_t t) const
  {
    typename std::map<_AttachPoint, std::vector<T>>::const_iterator it = m_data.find(_AttachPoint(-1,c,t));
    return it != m_data.end() && !it->second.empty();
  }
  inline bool hasPlaneAttachment(size_t z, size_t t) const
  {
    typename std::map<_AttachPoint, std::vector<T>>::const_iterator it = m_data.find(_AttachPoint(z,-1,t));
    return it != m_data.end() && !it->second.empty();
  }
  inline bool hasSingleChannelPlaneAttachment(size_t z, size_t c, size_t t) const
  {
    typename std::map<_AttachPoint, std::vector<T>>::const_iterator it = m_data.find(_AttachPoint(z,c,t));
    return it != m_data.end() && !it->second.empty();
  }

  inline const std::vector<T>& topLevelAttachments() const
  {
    typename std::map<_AttachPoint, std::vector<T>>::const_iterator it = m_data.find(_AttachPoint(-1,-1,-1));
    return it != m_data.end() ? it->second : m_empty;
  }
  inline const std::vector<T>& timeAttachments(size_t t) const
  {
    typename std::map<_AttachPoint, std::vector<T>>::const_iterator it = m_data.find(_AttachPoint(-1,-1,t));
    return it != m_data.end() ? it->second : m_empty;
  }
  inline const std::vector<T>& channelAttachments(size_t c, size_t t) const
  {
    typename std::map<_AttachPoint, std::vector<T>>::const_iterator it = m_data.find(_AttachPoint(-1,c,t));
    return it != m_data.end() ? it->second : m_empty;
  }
  inline const std::vector<T>& planeAttachments(size_t z, size_t t) const
  {
    typename std::map<_AttachPoint, std::vector<T>>::const_iterator it = m_data.find(_AttachPoint(z,-1,t));
    return it != m_data.end() ? it->second : m_empty;
  }
  inline const std::vector<T>& singleChannelPlaneAttachments(size_t z, size_t c, size_t t) const
  {
    typename std::map<_AttachPoint, std::vector<T>>::const_iterator it = m_data.find(_AttachPoint(z,c,t));
    return it != m_data.end() ? it->second : m_empty;
  }

protected:
  std::map<_AttachPoint, std::vector<T>> m_data;
  std::vector<T> m_empty;
};

}

#endif // ZIMGMETADATABASE_H
