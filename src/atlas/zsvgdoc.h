#pragma once

#include "zobjdoc.h"
#include <QSvgRenderer>

namespace nim {

class ZSvgDoc : public ZObjDoc
{
Q_OBJECT
public:
  explicit ZSvgDoc(ZDoc& doc);

  // return info of svg with id, assume svg exist, otherwise crash
  QSvgRenderer& svg(size_t id)
  { return *m_idToSvgPacks.at(id)->svg; }

  // ZObjDoc interface
public:
  bool save(size_t id) override;

  bool saveAs(size_t id) override;

  [[nodiscard]] QString typeName() const override
  { return "Svg"; }

  [[nodiscard]] QString typePluralName() const override
  { return "Svgs"; }

  [[nodiscard]] bool canReadFile(const QString& fileName) const override;

  size_t loadFile(const QString& fileName, QString& errorMsg) override;

  size_t loadFile(const QJsonValue& jValue, QString& errorMsg) override;

  [[nodiscard]] std::vector<QAction*> loadFileActions() const override;

  void removeObj(size_t id) override;

  [[nodiscard]] QString objName(size_t id) const override;

  [[nodiscard]] QString objPath(size_t id) const override;

  [[nodiscard]] bool objHasUnsavedChange(size_t id) const override;

  [[nodiscard]] QString objInfo(size_t id) const override;

  [[nodiscard]] QString objTooltip(size_t id) const override;

  [[nodiscard]] QJsonValue jsonValue(size_t id) const override;

  [[nodiscard]] bool isSameObj(const QJsonValue& v1, const QJsonValue& v2) const override;

  size_t makeAlias(size_t id) override;

  [[nodiscard]] bool isAlias(size_t id) const override;

protected:
  void loadSvg();

  // append another svg into this doc
  size_t addSvg(std::unique_ptr<QSvgRenderer> svg, const QString& path);

private:
  struct SvgPack
  { // svg and its associated data
    explicit SvgPack(std::unique_ptr<QSvgRenderer> svg, const QString& path);

    void updateDerivedData();

    const QString& info() const;

    inline const QString& name() const
    { return m_name; }

    inline const QString& tooltip() const
    { return m_tooltip; }

    std::unique_ptr<QSvgRenderer> svg;
    QString path;
    bool hasUnsavedChange = false;

    // derived data
  private:
    mutable QString m_info;
    QString m_name;
    QString m_tooltip;
  };

  void createActions();

  // notify obj manager about the update
  void packInfoUpdated(SvgPack* pack);

private:
  std::map<size_t, std::shared_ptr<SvgPack>> m_idToSvgPacks;

  QAction* m_loadSvgAction = nullptr;
};

} // namespace nim
