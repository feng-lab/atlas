#pragma once

#include "z2danimation.h"
#include "zobjdoc.h"

namespace nim {

class Z2DAnimationDoc : public ZObjDoc
{
  Q_OBJECT

public:
  explicit Z2DAnimationDoc(ZDoc& doc);

  void bindView(ZView* v);

  void createNewAnimation(const QString& name = "");

  // ZObjDoc interface

public:
  bool save(size_t id) override;

  bool saveAs(size_t id) override;

  [[nodiscard]] QString typeName() const override
  {
    return "Animation2D";
  }

  [[nodiscard]] QString typePluralName() const override
  {
    return "Animation2Ds";
  }

  [[nodiscard]] bool canReadFile(const QString& fileName) const override;

  size_t loadFile(const QString& fileName, QString& errorMsg) override;

  size_t loadFile(const json::value& jValue, QString& errorMsg) override;

  [[nodiscard]] std::vector<QAction*> loadFileActions() const override;

  [[nodiscard]] QString objName(size_t id) const override;

  [[nodiscard]] QString objPath(size_t id) const override;

  [[nodiscard]] bool objHasUnsavedChange(size_t id) const override;

  [[nodiscard]] QString objInfo(size_t id) const override;

  [[nodiscard]] QString objTooltip(size_t id) const override;

  [[nodiscard]] const QUndoStack* objUndoStack(size_t id) const override;

  [[nodiscard]] json::value jsonValue(size_t id) const override;

  [[nodiscard]] bool isSameObj(const json::value& v1, const json::value& v2) const override;

  size_t makeAlias(size_t id) override;

  [[nodiscard]] bool isAlias(size_t id) const override;

  QWidget* createObjEditWidget(size_t id) override;

protected:
  void removeObj(size_t id) override;

  void loadAnimation();

  void setModified();

  void releaseView();

  // append another 2D animation into this doc
  size_t addAnimation(Z2DAnimation* animation, const QString& path, const QString& name = "");

private:
  struct AnimationPack
  { // animation and its associated data
    AnimationPack(Z2DAnimation* animation_, const QString& path_, QString name = "");

    void updateDerivedData();

    const QString& info() const;

    const QString& name() const
    {
      return m_name;
    }

    const QString& tooltip() const
    {
      return m_tooltip;
    }

    std::unique_ptr<Z2DAnimation> animation;
    QString path;
    bool hasUnsavedChange = false;

  protected:
    QString m_tmpName;

    // derived data

  private:
    mutable QString m_info;
    QString m_name;
    QString m_tooltip;
  };

  void createActions();

  bool saveAnimation(AnimationPack* pack, const QString& fileName, QString& errorMsg);

private:
  std::map<size_t, std::shared_ptr<AnimationPack>> m_idToAnimationPacks;

  QAction* m_loadAnimationsAction = nullptr;

  ZView* m_view = nullptr;
};

} // namespace nim
