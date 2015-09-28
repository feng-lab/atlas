#ifndef Z2DANIMATIONDOC_H
#define Z2DANIMATIONDOC_H

#include "zobjdoc.h"
#include "z2danimation.h"

namespace nim {

class Z2DAnimationDoc : public ZObjDoc
{
  Q_OBJECT
public:
  explicit Z2DAnimationDoc(ZDoc &doc);

  void bindView(ZView *v);
  void createNewAnimation(const QString &name = "");

  // ZObjDoc interface
public:
  virtual bool save(size_t id) override;
  virtual bool saveAs(size_t id) override;
  virtual QString typeName() const override { return "Animation2D"; }
  virtual QString typePluralName() const override { return "Animation2Ds"; }
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
  // append another 2D animation into this doc
  size_t addAnimation(Z2DAnimation *animation, const QString &path, const QString &name = "");

signals:

private:
  struct AnimationPack { // animation and its associated data
    AnimationPack(Z2DAnimation *animation, const QString &path, const QString &name = "");

    void updateDerivedData();
    const QString& info() const;
    inline const QString& name() const { return m_name; }
    inline const QString& tooltip() const { return m_tooltip; }


    std::unique_ptr<Z2DAnimation> animation;
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
  std::map<size_t, std::shared_ptr<AnimationPack>> m_idToAnimationPacks;

  QAction *m_loadAnimationsAction;

  ZView *m_view;
};

} // namespace nim


#endif // Z2DANIMATIONDOC_H
