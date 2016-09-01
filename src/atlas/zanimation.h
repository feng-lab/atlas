#pragma once

#include "z3dgl.h"
#include "zparameteranimation.h"
#include "zviewsettinginterface.h"
#include "zjson.h"
#include <QObject>
#include <QDir>
#include <QUndoStack>
#include <map>

namespace nim {

class ZDoc;

class ZVideoEncoder;

struct ZAnimationDisplayPack
{
  enum class Type
  {
    GlobalPara, Object, ObjectPara, ShowAll
  };

  QString name;
  size_t id;
  size_t boundId;
  int row;
  Type type;
  bool expanded;
  bool showAll;
  ZParameterAnimation* paraAnimation;
  QString objInfo;
};

class ZAnimation;

class ZAnimationChangeDurationCommand : public QUndoCommand
{
public:
  ZAnimationChangeDurationCommand(ZAnimation* ani, double oldDuraiont, double newDuration)
    : QUndoCommand(), m_ani(ani), m_oldDuration(oldDuraiont), m_newDuration(newDuration)
  {
    setText("Change Duration");
  }

  void undo();

  void redo();

private:
  ZAnimation* m_ani;
  double m_oldDuration;
  double m_newDuration;
};

class ZAnimation : public QObject
{
Q_OBJECT
public:
  explicit ZAnimation(ZDoc& doc, QObject* parent = nullptr);

  ~ZAnimation();

  void addKeyFrame(double time);

  inline double duration() const
  { return m_duration; }

  const std::vector<std::unique_ptr<ZParameterAnimation>>& paraAnimationList(size_t id) const
  { return findUniqueId(id)->objParaAnimations; }

  const std::vector<ZAnimationDisplayPack>& displayPacks() const
  { return m_displayPacks; }

  void setExpanded(size_t id, bool v);

  void toogleExpanded(size_t id);

  void toogleShowAll(size_t id);

  QUndoStack* undoStack()
  { return &m_undoStack; }

  virtual bool is2DAnimation() const
  { return false; }

  void setDuration(double duration);

  void setCurrentTime(double time);

  void removeObj(size_t id);

  void removeRedundantKeys();

  void rebindView();

  void releaseView();

  void exportFixedSize3DAnimation(const QDir& dir, const QString& fn, double framePerSecond, int width, int height,
                                  Z3DScreenShotType sst);

  void export3DAnimation(const QDir& dir, const QString& fn, double framePerSecond, Z3DScreenShotType sst);

  void exportFixedSize2DAnimation(const QDir& dir, const QString& fn, double framePerSecond, int width, int height);

  void export2DAnimation(const QDir& dir, const QString& fn, double framePerSecond);

signals:

  void durationChanged(double v);

  void objChanged();

  void objViewChanged();

  void expandChanged();

  void keysChanged();

  void keyChanged(ZParameterKey* key);

  void keyAboutToDelete(ZParameterKey* key);

  void colorChanged(ZParameterAnimation* pa);

protected:
  void disableAnimationOf(size_t id);

  void tryLinkAnimationWith(size_t id);

  void videoEncoderError(const QString& err);

  void videoEncoderFinished();

  void videoEncoderCanceled();

  void updateObjAnimation();

  void buildDisplayPacks();

  void releaseParameters();

  // return if paraAnimationList is resorted
  bool bind(std::vector<std::unique_ptr<ZParameterAnimation>>& paraAnimationList,
            const std::vector<ZParameter*>& paraList);

  void readContent(const QString& fn, const QString& jsonKey);

  void writeContent(const QString& fn, const QString& jsonKey);

  void setDurationImpl(double v);

  // assume view exist
  virtual void bindGlobalParameters() = 0;

  // assume view exist
  virtual void addGlobalKey(double time) = 0;

  struct AnimationObj
  {
    AnimationObj(const QString& type, const QJsonValue& value)
      : objType(type), objJsonValue(value), isExpanded(false), isShowAll(false), boundId(0), uniqueId(0)
    {}

    QString objType;
    QJsonValue objJsonValue;
    std::vector<std::unique_ptr<ZParameterAnimation>> objParaAnimations;
    bool isExpanded;
    bool isShowAll;
    size_t boundId;

    size_t uniqueId;
  };

  AnimationObj* findBoundId(size_t id, size_t* idx = nullptr);

  AnimationObj* findUniqueId(size_t id, size_t* idx = nullptr);

  const AnimationObj* findBoundId(size_t id, size_t* idx = nullptr) const;

  const AnimationObj* findUniqueId(size_t id, size_t* idx = nullptr) const;

protected:
  friend class ZAnimationChangeDurationCommand;

  ZDoc& m_doc;
  ZViewSettingInterface* m_view;

  double m_duration;

  std::vector<std::unique_ptr<ZParameterAnimation>> m_globalParaAnimations;
  std::vector<std::unique_ptr<AnimationObj>> m_objList;
  size_t m_nextUniqueId;

  std::vector<ZAnimationDisplayPack> m_displayPacks;

  QUndoStack m_undoStack;

  ZVideoEncoder* m_videoEncoder;
};

} // namespace nim

