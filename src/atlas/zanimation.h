#pragma once

#include "z3dtypes.h"
#include "zparameteranimation.h"
#include "zviewsettinginterface.h"
#include "zjson.h"
#include <QObject>
#include <QDir>
#include <QUndoStack>
#include <QTemporaryDir>
#include <map>
#include <utility>

namespace nim {

class ZDoc;

class ZVideoEncoder;

struct ZAnimationDisplayPack
{
  enum class Type
  {
    GlobalPara,
    Object,
    ObjectPara,
    ShowAll
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
    : QUndoCommand()
    , m_ani(ani)
    , m_oldDuration(oldDuraiont)
    , m_newDuration(newDuration)
  {
    setText("Change Duration");
  }

  void undo() override;

  void redo() override;

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

  ~ZAnimation() override;

  void addKeyFrame(double time);

  [[nodiscard]] double duration() const
  {
    return m_duration;
  }

  [[nodiscard]] const std::vector<std::unique_ptr<ZParameterAnimation>>& paraAnimationList(size_t id) const
  {
    return findUniqueId(id)->objParaAnimations;
  }

  struct EnsureParameterAnimationResult
  {
    ZParameterAnimation* animation = nullptr; // non-owning
    bool createdObject = false;
    bool createdParameter = false;
    QString error;
  };

  // Lookup a parameter animation track by the runtime-bound object id (same ids
  // used by the UI and RPC: 1=background,2=axis,3=global/lighting,>=4 object id).
  // Returns nullptr when the object/track doesn't exist in this animation.
  [[nodiscard]] ZParameterAnimation* parameterAnimationForBoundId(size_t boundId, const QString& jsonKey);
  [[nodiscard]] const ZParameterAnimation* parameterAnimationForBoundId(size_t boundId, const QString& jsonKey) const;

  // Return all parameter animation tracks currently present for a bound id.
  [[nodiscard]] std::vector<ZParameterAnimation*> parameterAnimationsForBoundId(size_t boundId);

  // Ensure the parameter animation track exists for the given bound id + jsonKey.
  // This is a programmatic equivalent of a user creating a track by adding/editing
  // keys in the timeline UI: it only creates the requested object/track, not a full
  // global keyframe for all objects.
  [[nodiscard]] EnsureParameterAnimationResult ensureParameterAnimationForBoundId(size_t boundId, const QString& jsonKey);

  [[nodiscard]] const std::vector<ZAnimationDisplayPack>& displayPacks() const
  {
    return m_displayPacks;
  }

  void setExpanded(size_t id, bool v);

  void toogleExpanded(size_t id);

  void toogleShowAll(size_t id);

  QUndoStack* undoStack()
  {
    return &m_undoStack;
  }

  // Undo/redo support for animation building/editing.
  // This snapshot format is internal (not the .animation2d/.animation3d file format).
  struct UndoSnapshot
  {
    json::object state;
  };

  [[nodiscard]] UndoSnapshot captureUndoSnapshot() const;
  void restoreFromUndoSnapshot(const UndoSnapshot& snapshot);
  void pushUndoSnapshotCommand(const QString& text, UndoSnapshot&& beforeSnapshot);

  [[nodiscard]] virtual bool is2DAnimation() const
  {
    return false;
  }

  void setDuration(double duration);

  void setCurrentTime(double time) const;

  void cancelRenderingAndSetCurrentTime(double time) const;

  void removeObj(size_t id);

  void removeRedundantKeys();

  // UI/agent-facing variant: records the change on the undo stack.
  void removeRedundantKeysUndoable();

  void rebindView();

  void releaseView();

  void exportFixedSize3DAnimation(const QString& fn,
                                  int framePerSecond,
                                  int startFrame,
                                  int endFrame,
                                  int width,
                                  int height,
                                  Z3DScreenShotType sst,
                                  int tileSize = 0,
                                  int tileBorder = 0);

  void export3DAnimation(const QString& fn, int framePerSecond, int startFrame, int endFrame, Z3DScreenShotType sst);

  void exportFixedSize2DAnimation(const QString& fn,
                                  int framePerSecond,
                                  int startFrame,
                                  int endFrame,
                                  int width,
                                  int height);

  void export2DAnimation(const QString& fn, int framePerSecond, int startFrame, int endFrame);

  // Introspection helper for RPC: last applied time
  [[nodiscard]] double currentTime() const { return m_currentTime; }

Q_SIGNALS:
  void durationChanged(double v);

  void objChanged();

  void objViewChanged();

  void expandChanged();

  void keysChanged();

  void keyChanged(ZParameterKey* key);

  void keyAboutToDelete(ZParameterKey* key);

  void colorChanged(ZParameterAnimation* pa);

  void exportFixedSize3DAnimationInEngine(const ZAnimation* animation,
                                          const QString& fn,
                                          int framePerSecond,
                                          int startFrame,
                                          int endFrame,
                                          int width,
                                          int height,
                                          bool overwriteFileIfExist,
                                          Z3DScreenShotType sst,
                                          const QString* imageOuputFolder,
                                          bool skipVideoCompression,
                                          int tileSize,
                                          int tileBorder);

protected:
  void disableAnimationOf(size_t id);

  void tryLinkAnimationWith(size_t id);

  void videoEncoderError(const QString& err);

  void videoEncoderFinished();

  void videoEncoderCanceled();

  void cancelButtonPressed();

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
    AnimationObj(QString type, json::value value)
      : objType(std::move(type))
      , objJsonValue(std::move(value))
      , isExpanded(false)
      , isShowAll(false)
      , boundId(0)
      , uniqueId(0)
    {}

    QString objType;
    json::value objJsonValue;
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
  ZViewSettingInterface* m_engine;

  double m_duration;
  // Remember last applied time so late-linked objects can be synced immediately
  mutable double m_currentTime = 0.0;

  std::vector<std::unique_ptr<ZParameterAnimation>> m_globalParaAnimations;
  std::vector<std::unique_ptr<AnimationObj>> m_objList;
  size_t m_nextUniqueId;

  std::vector<ZAnimationDisplayPack> m_displayPacks;

  QUndoStack m_undoStack;

  ZVideoEncoder* m_videoEncoder;

  std::shared_ptr<QTemporaryDir> m_tempDir;
};

} // namespace nim
