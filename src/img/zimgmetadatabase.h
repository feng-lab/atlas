#pragma once

#include <cstddef>
#include <map>
#include <vector>

namespace nim {

// a template class that contains metadata of type T of a img
// metadata can attach to different level of img (e.g. location metadata,
// time metadata, channel metadata, plane metadata), each attach point can
// contain many data

template<typename T>
class ZImgMetadataBase
{
protected:
  struct AttachPoint
  {
    AttachPoint(index_t z_, index_t c_, index_t t_)
      : t(t_)
      , c(c_)
      , z(z_)
    {}

    index_t t = -1;
    index_t c = -1;
    index_t z = -1;

    auto operator<=>(const AttachPoint& rhs) const = default;
  };

public:
  ZImgMetadataBase() = default;

  [[nodiscard]] QString toQString() const
  {
    return {};
  }

  void swap(ZImgMetadataBase& other) noexcept
  {
    m_data.swap(other.m_data);
  }

  void merge(const ZImgMetadataBase& other) noexcept
  {
    for (auto& [k, v] : other.m_data) {
      m_data[k].insert(m_data[k].end(), v.begin(), v.end());
    }
  }

  void clear()
  {
    m_data.clear();
  }

  [[nodiscard]] bool isEmpty() const
  {
    return m_data.empty();
  }

  void attachToTopLevel(const T& d)
  {
    std::vector<T>& list = m_data[AttachPoint(-1, -1, -1)];
    list.push_back(d);
  }

  void attachToTime(const T& d, size_t t)
  {
    std::vector<T>& list = m_data[AttachPoint(-1, -1, t)];
    list.push_back(d);
  }

  void attachToChannel(const T& d, size_t c, size_t t)
  {
    std::vector<T>& list = m_data[AttachPoint(-1, c, t)];
    list.push_back(d);
  }

  void attachToPlane(const T& d, size_t z, size_t t)
  {
    std::vector<T>& list = m_data[AttachPoint(z, -1, t)];
    list.push_back(d);
  }

  void attachToSingleChannelPlane(const T& d, size_t z, size_t c, size_t t)
  {
    std::vector<T>& list = m_data[AttachPoint(z, c, t)];
    list.push_back(d);
  }

  void attachToTopLevel(const std::vector<T>& d)
  {
    std::vector<T>& list = m_data[AttachPoint(-1, -1, -1)];
    list.insert(list.end(), d.begin(), d.end());
  }

  void attachToTime(const std::vector<T>& d, size_t t)
  {
    std::vector<T>& list = m_data[AttachPoint(-1, -1, t)];
    list.insert(list.end(), d.begin(), d.end());
  }

  void attachToChannel(const std::vector<T>& d, size_t c, size_t t)
  {
    std::vector<T>& list = m_data[AttachPoint(-1, c, t)];
    list.insert(list.end(), d.begin(), d.end());
  }

  void attachToPlane(const std::vector<T>& d, size_t z, size_t t)
  {
    std::vector<T>& list = m_data[AttachPoint(z, -1, t)];
    list.insert(list.end(), d.begin(), d.end());
  }

  void attachToSingleChannelPlane(const std::vector<T>& d, size_t z, size_t c, size_t t)
  {
    std::vector<T>& list = m_data[AttachPoint(z, c, t)];
    list.insert(list.end(), d.begin(), d.end());
  }

  void clearTopLevelAttachments()
  {
    m_data.erase(AttachPoint(-1, -1, -1));
  }

  void clearTimeAttachments(size_t t)
  {
    m_data.erase(AttachPoint(-1, -1, t));
  }

  void clearChannelAttachments(size_t c, size_t t)
  {
    m_data.erase(AttachPoint(-1, c, t));
  }

  void clearPlaneAttachments(size_t z, size_t t)
  {
    m_data.erase(AttachPoint(z, -1, t));
  }

  void clearSingleChannelPlaneAttachments(size_t z, size_t c, size_t t)
  {
    m_data.erase(AttachPoint(z, c, t));
  }

  [[nodiscard]] bool hasTopLevelAttachment() const
  {
    typename std::map<AttachPoint, std::vector<T>>::const_iterator it = m_data.find(AttachPoint(-1, -1, -1));
    return it != m_data.end() && !it->second.empty();
  }

  [[nodiscard]] bool hasTimeAttachment(size_t t) const
  {
    typename std::map<AttachPoint, std::vector<T>>::const_iterator it = m_data.find(AttachPoint(-1, -1, t));
    return it != m_data.end() && !it->second.empty();
  }

  [[nodiscard]] bool hasChannelAttachment(size_t c, size_t t) const
  {
    typename std::map<AttachPoint, std::vector<T>>::const_iterator it = m_data.find(AttachPoint(-1, c, t));
    return it != m_data.end() && !it->second.empty();
  }

  [[nodiscard]] bool hasPlaneAttachment(size_t z, size_t t) const
  {
    typename std::map<AttachPoint, std::vector<T>>::const_iterator it = m_data.find(AttachPoint(z, -1, t));
    return it != m_data.end() && !it->second.empty();
  }

  [[nodiscard]] bool hasSingleChannelPlaneAttachment(size_t z, size_t c, size_t t) const
  {
    typename std::map<AttachPoint, std::vector<T>>::const_iterator it = m_data.find(AttachPoint(z, c, t));
    return it != m_data.end() && !it->second.empty();
  }

  const std::vector<T>& topLevelAttachments() const
  {
    typename std::map<AttachPoint, std::vector<T>>::const_iterator it = m_data.find(AttachPoint(-1, -1, -1));
    return it != m_data.end() ? it->second : m_empty;
  }

  const std::vector<T>& timeAttachments(size_t t) const
  {
    typename std::map<AttachPoint, std::vector<T>>::const_iterator it = m_data.find(AttachPoint(-1, -1, t));
    return it != m_data.end() ? it->second : m_empty;
  }

  const std::vector<T>& channelAttachments(size_t c, size_t t) const
  {
    typename std::map<AttachPoint, std::vector<T>>::const_iterator it = m_data.find(AttachPoint(-1, c, t));
    return it != m_data.end() ? it->second : m_empty;
  }

  const std::vector<T>& planeAttachments(size_t z, size_t t) const
  {
    typename std::map<AttachPoint, std::vector<T>>::const_iterator it = m_data.find(AttachPoint(z, -1, t));
    return it != m_data.end() ? it->second : m_empty;
  }

  const std::vector<T>& singleChannelPlaneAttachments(size_t z, size_t c, size_t t) const
  {
    typename std::map<AttachPoint, std::vector<T>>::const_iterator it = m_data.find(AttachPoint(z, c, t));
    return it != m_data.end() ? it->second : m_empty;
  }

protected:
  std::map<AttachPoint, std::vector<T>> m_data;
  std::vector<T> m_empty;
};

} // namespace nim
