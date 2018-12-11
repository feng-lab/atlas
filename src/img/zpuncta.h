#pragma once

#include "zpunctum.h"
#include <list>

namespace nim {

class ZPuncta
{
  using container_type = std::list<ZPunctum>;
public:
  using reference = container_type::reference;
  using const_reference = container_type::const_reference;
  using iterator = container_type::iterator;
  using const_iterator = container_type::const_iterator;
  using size_type = container_type::size_type;
  using reverse_iterator = container_type::reverse_iterator;
  using const_reverse_iterator = container_type::const_reverse_iterator;

  ZPuncta() = default;

  // might throw ZIOException
  explicit ZPuncta(const QString& filename);

  explicit ZPuncta(const std::list<ZPunctum>& p);

  ZPuncta(ZPuncta&&) = default;

  ZPuncta& operator=(ZPuncta&&) = default;

  ZPuncta(const ZPuncta&) = default;

  ZPuncta& operator=(const ZPuncta&) = default;

  inline iterator begin() noexcept
  { return m_d.begin(); }

  inline const_iterator begin() const noexcept
  { return m_d.begin(); }

  inline iterator end() noexcept
  { return m_d.end(); }

  inline const_iterator end() const noexcept
  { return m_d.end(); }

  inline reverse_iterator rbegin() noexcept
  { return m_d.rbegin(); }

  inline const_reverse_iterator rbegin() const noexcept
  { return m_d.rbegin(); }

  inline reverse_iterator rend() noexcept
  { return m_d.rend(); }

  inline const_reverse_iterator rend() const noexcept
  { return m_d.rend(); }

  inline const_iterator cbegin() const noexcept
  { return m_d.cbegin(); }

  inline const_iterator cend() const noexcept
  { return m_d.cend(); }

  inline const_reverse_iterator crbegin() const noexcept
  { return m_d.crbegin(); }

  inline const_reverse_iterator crend() const noexcept
  { return m_d.crend(); }

  inline reference front()
  { return m_d.front(); }

  inline const_reference front() const
  { return m_d.front(); }

  inline reference back()
  { return m_d.back(); }

  inline const_reference back() const
  { return m_d.back(); }

  inline bool empty() const noexcept
  { return m_d.empty(); }

  inline size_type size() const noexcept
  { return m_d.size(); }

  template<class... Args>
  inline void emplace_front(Args&& ... args)
  { m_d.emplace_front(std::forward<Args>(args)...); }

  inline void pop_front()
  { m_d.pop_front(); }

  template<class... Args>
  inline void emplace_back(Args&& ... args)
  { m_d.emplace_back(std::forward<Args>(args)...); }

  inline void pop_back()
  { m_d.pop_back(); }

  inline void push_front(const ZPunctum& x)
  { m_d.push_front(x); }

  inline void push_front(ZPunctum&& x)
  { m_d.push_front(std::move(x)); }

  inline void push_back(const ZPunctum& x)
  { m_d.push_back(x); }

  inline void push_back(ZPunctum&& x)
  { m_d.push_back(std::move(x)); }

  inline iterator erase(const_iterator position)
  { return m_d.erase(position); }

  inline iterator erase(const_iterator position, const_iterator last)
  { return m_d.erase(position, last); }

  inline void clear() noexcept
  { m_d.clear(); }

  inline void swap(ZPuncta& other) noexcept
  { m_d.swap(other.m_d); }

  template<class Pred>
  inline void remove_if(Pred pred)
  { m_d.remove_if(std::forward<Pred>(pred)); }

  inline bool operator==(const ZPuncta& l) const
  { return m_d == l.m_d; }

  inline bool operator!=(const ZPuncta& l) const
  { return !(*this == l); }

  inline const std::list<ZPunctum>& data() const
  { return m_d; }

  // qt style read write name filter for filedialog
  static bool canReadFile(const QString& filename);

  static bool canWriteFile(const QString& filename);

  static const QString& getQtReadNameFilter();

  static void getQtWriteNameFilter(QStringList& filters, QStringList& formats);

  // might throw ZIOException
  void load(const QString& filename);

  void save(const QString& filename, const QString& format = "") const;

  QString toQString() const;

private:
  friend class ZPunctaIO;

  std::list<ZPunctum> m_d;
};

} // namespace nim

