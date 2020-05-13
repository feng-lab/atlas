#pragma once

#include "z3danimation.h"
#include "zobjdoc.h"

namespace nim {

class Z3DAnimationDoc : public ZObjDoc
{
Q_OBJECT
public:
  explicit Z3DAnimationDoc(ZDoc& doc);

  void bindView(Z3DView* v);

  void createNewAnimation(const QString& name = "");

  // return info of animation with id, animation mesh exist, otherwise crash
  Z3DAnimation& animation(size_t id)
  { return *m_idToAnimationPacks.at(id)->animation; }

  // ZObjDoc interface
public:
  bool save(size_t id) override;

  bool saveAs(size_t id) override;

  QString typeName() const override
  { return "Animation3D"; }

  QString typePluralName() const override
  { return "Animation3Ds"; }

  bool canReadFile(const QString& fileName) const override;

  size_t loadFile(const QString& fileName, QString& errorMsg) override;

  size_t loadFile(const QJsonValue& jValue, QString& errorMsg) override;

  QList<QAction*> loadFileActions() const override;

  void removeObj(size_t id) override;

  QString objName(size_t id) const override;

  QString objPath(size_t id) const override;

  bool objHasUnsavedChange(size_t id) const override;

  QString objInfo(size_t id) const override;

  QString objTooltip(size_t id) const override;

  const QUndoStack* objUndoStack(size_t id) const override;

  QJsonValue jsonValue(size_t id) const override;

  bool isSameObj(const QJsonValue& v1, const QJsonValue& v2) const override;

  size_t makeAlias(size_t id) override;

  bool isAlias(size_t id) const override;

  QWidget* createObjEditWidget(size_t id) override;

protected:
  void loadAnimation();

  void setModified();

  void releaseView();

  // append another 3d animation into this doc
  size_t addAnimation(Z3DAnimation* animation, const QString& path, const QString& name = "");

private:
  struct AnimationPack
  { // animation and its associated data
    AnimationPack(Z3DAnimation* animation_, const QString& path_, const QString& name = "");

    void updateDerivedData();

    const QString& info() const;

    inline const QString& name() const
    { return m_name; }

    inline const QString& tooltip() const
    { return m_tooltip; }

    std::unique_ptr<Z3DAnimation> animation;
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

  QAction* m_loadAnimationsAction;

  Z3DView* m_view = nullptr;
};

} // namespace nim

