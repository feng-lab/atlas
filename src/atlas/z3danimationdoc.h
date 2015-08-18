#ifndef Z3DANIMATIONDOC_H
#define Z3DANIMATIONDOC_H

#include "zobjdoc.h"
#include "z3danimation.h"

namespace nim {

class Z3DAnimationDoc : public ZObjDoc
{
  Q_OBJECT
public:
  explicit Z3DAnimationDoc(ZDoc &doc);
  ~Z3DAnimationDoc();

  void bindView(Z3DView *v);
  void createNewAnimation(const QString &name = "");

  // return info of animation with id, animation mesh exist, otherwise crash
  Z3DAnimation* animation(size_t id) { return m_idToAnimationPacks.at(id)->animation; }

  // ZObjDoc interface
public:
  virtual bool save(size_t id) override;
  virtual bool saveAs(size_t id) override;
  virtual QString typeName() const override { return "Animation3D"; }
  virtual QString typePluralName() const override { return "Animation3Ds"; }
  virtual bool canReadFile(const QString &fileName) override;
  virtual size_t loadFile(const QString &fileName, QString &errorMsg) override;
  virtual size_t loadFile(const QJsonValue &jValue, QString &errorMsg) override;
  virtual QList<QAction*> loadFileActions() const override;
  virtual void removeObj(size_t id) override;
  virtual const QString& objName(size_t id) const override;
  virtual QString objPath(size_t id) const override;
  virtual bool objHasUnsavedChange(size_t id) const override;
  virtual const QString& objInfo(size_t id) const override;
  virtual const QString& objTooltip(size_t id) const override;
  virtual QUndoStack* objUndoStack(size_t id) override;
  virtual QJsonValue jsonValue(size_t id) const override;
  virtual bool isSameObj(const QJsonValue& v1, const QJsonValue& v2) const override;
  virtual size_t makeAlias(size_t id) override;
  virtual bool isAlias(size_t id) const override;
  virtual QWidget *createObjEditWidget(size_t id) override;

protected slots:
  void loadAnimation();
  void setModified();
  void releaseView();

protected:
  // append another 3d animation into this doc
  size_t addAnimation(Z3DAnimation *animation, const QString &path, const QString &name = "");

signals:

private:
  struct AnimationPack { // animation and its associated data
    AnimationPack(Z3DAnimation *animation, const QString &path, const QString &name = "");
    ~AnimationPack();

    void updateDerivedData();
    const QString& info() const;
    inline const QString& name() const { return m_name; }
    inline const QString& tooltip() const { return m_tooltip; }

    Z3DAnimation *animation;
    QString path;
    bool hasUnsavedChange;

  protected:
    QString m_tmpName;

    // derived data
  private:
    mutable QString m_info;
    QString m_name;
    QString m_tooltip;
  };

  void createActions();
  bool saveAnimation(AnimationPack *pack, const QString &fileName, QString &errorMsg);

private:
  std::map<size_t, AnimationPack*> m_idToAnimationPacks;

  QAction *m_loadAnimationsAction;

  Z3DView *m_view;
};

} // namespace nim

#endif // Z3DANIMATIONDOC_H
