#pragma once

#include <QUrl>
#include <QString>
#include <QStringList>

#include <folly/coro/Task.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nim {

class ZRemoteObjectStore;
class ZNeuroglancerRemoteContext;

class ZNeuroglancerPrecomputedSegmentProperties
{
public:
  enum class PropertyType
  {
    Label,
    Description,
    String,
    Tags,
    Number,
  };

  enum class NumberDataType
  {
    Uint8,
    Int8,
    Uint16,
    Int16,
    Uint32,
    Int32,
    Float32,
  };

  struct Property
  {
    QString id;
    PropertyType type = PropertyType::String;
    QString description;

    // For type == Tags
    std::vector<QString> tags;
    std::vector<QString> tagDescriptions;

    // For type == Number
    NumberDataType numberDataType = NumberDataType::Uint32;

    // Values (exactly one is used depending on `type`)
    std::vector<QString> stringValues;               // label/description/string
    std::vector<double> numberValues;                // number
    std::vector<std::vector<uint32_t>> tagIndicesValues; // tags
  };

  static std::shared_ptr<ZNeuroglancerPrecomputedSegmentProperties>
  open(const QUrl& dirUrl, std::shared_ptr<const ZNeuroglancerRemoteContext> remoteContext);

  static folly::coro::Task<std::shared_ptr<ZNeuroglancerPrecomputedSegmentProperties>>
  openAsync(const QUrl& dirUrl, std::shared_ptr<const ZNeuroglancerRemoteContext> remoteContext);

  static std::shared_ptr<ZNeuroglancerPrecomputedSegmentProperties>
  open(const QUrl& dirUrl,
       std::chrono::milliseconds timeout,
       std::shared_ptr<const ZRemoteObjectStore> objectStore = nullptr);

  static folly::coro::Task<std::shared_ptr<ZNeuroglancerPrecomputedSegmentProperties>>
  openAsync(const QUrl& dirUrl,
            std::chrono::milliseconds timeout,
            std::shared_ptr<const ZRemoteObjectStore> objectStore = nullptr);

  // Parses the contents of a `segment_properties/info` JSON file.
  // This does not perform any network I/O. `dirUrl` is used for error messages and bookkeeping only.
  static std::shared_ptr<ZNeuroglancerPrecomputedSegmentProperties> parseInfoJsonText(const QUrl& dirUrl,
                                                                                      std::string_view infoJsonText);

  [[nodiscard]] const QUrl& dirUrl() const
  {
    return m_dirUrl;
  }

  [[nodiscard]] size_t numIds() const
  {
    return m_ids.size();
  }

  [[nodiscard]] bool hasLabel() const
  {
    return m_labelPropIndex.has_value();
  }

  [[nodiscard]] bool hasDescription() const
  {
    return m_descriptionPropIndex.has_value();
  }

  [[nodiscard]] bool hasTags() const
  {
    return m_tagsPropIndex.has_value();
  }

  [[nodiscard]] std::optional<QString> labelForId(uint64_t id) const;
  [[nodiscard]] std::optional<QString> descriptionForId(uint64_t id) const;
  [[nodiscard]] QStringList tagsForId(uint64_t id) const;

  [[nodiscard]] const std::vector<Property>& properties() const
  {
    return m_properties;
  }

  [[nodiscard]] const std::vector<uint64_t>& ids() const
  {
    return m_ids;
  }

private:
  struct IdIndex
  {
    uint64_t id = 0;
    uint32_t index = 0;
  };

  [[nodiscard]] std::optional<size_t> findIndex(uint64_t id) const;

private:
  QUrl m_dirUrl;

  bool m_idsSorted = false;
  std::vector<uint64_t> m_ids; // file order
  std::vector<IdIndex> m_sortedIds; // only used when !m_idsSorted

  std::vector<Property> m_properties;
  std::optional<size_t> m_labelPropIndex;
  std::optional<size_t> m_descriptionPropIndex;
  std::optional<size_t> m_tagsPropIndex;
};

} // namespace nim
