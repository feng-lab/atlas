#pragma once

#include "z3danimation.h"
#include "zobjdoc.h"

namespace nim {

class Z3DAnimationDoc : public ZObjDoc
{
  Q_OBJECT

public:
  explicit Z3DAnimationDoc(ZDoc& doc);

  void bindView(Z3DRenderingEngine* v);

  void createNewAnimation(const QString& name = "");

  // Convenience helpers for programmatic control
  // Return IDs of all animations managed by this doc
  [[nodiscard]] std::vector<size_t> animationIds() const;

  // Return raw pointer to animation by id or nullptr if not found
  [[nodiscard]] Z3DAnimation* animationPtr(size_t id);
  [[nodiscard]] const Z3DAnimation* animationPtr(size_t id) const;

  // Create a new animation and return its id
  size_t createNewAnimationAndReturnId(const QString& name = "");

  struct KeyOpResult
  {
    bool ok = false;
    enum class ErrorKind
    {
      FailedPrecondition,
      InvalidArgument,
    };
    ErrorKind errorKind = ErrorKind::FailedPrecondition;
    QString error;
  };

  // Set or replace a key for a target parameter track.
  // targetId follows the same convention as RPC/UI: 0=camera, 1=background,
  // 2=axis, 3=global/lighting, >=4 object id.
  [[nodiscard]] KeyOpResult setKey(size_t animationId,
                                   size_t targetId,
                                   const QString& jsonKey,
                                   double timeSec,
                                   const QString& easing,
                                   const json::value& value);

  [[nodiscard]] KeyOpResult removeKey(size_t animationId,
                                      size_t targetId,
                                      const QString& jsonKey,
                                      double timeSec);

  [[nodiscard]] KeyOpResult clearKeys(size_t animationId, size_t targetId, const QString& jsonKey);

  struct ListedKey
  {
    double timeSec = 0.0;
    QString parameterType; // e.g. "Vec3", "3DCamera"
    QString keyJson; // full key json (time/type/value, etc.) serialized as string
  };

  struct ListKeysResult
  {
    bool ok = false;
    KeyOpResult::ErrorKind errorKind = KeyOpResult::ErrorKind::FailedPrecondition;
    QString error;
    std::vector<ListedKey> keys;
  };

  [[nodiscard]] ListKeysResult listKeys(size_t animationId,
                                        size_t targetId,
                                        const QString& jsonKey,
                                        bool includeValues);

  struct BatchRemoveKeyOp
  {
    size_t targetId = 0;
    QString jsonKey;
    double timeSec = 0.0;
  };

  struct BatchSetKeyOp
  {
    size_t targetId = 0;
    QString jsonKey;
    double timeSec = 0.0;
    QString easing;
    json::value value;
  };

  [[nodiscard]] KeyOpResult batchKeys(size_t animationId,
                                      const std::vector<BatchRemoveKeyOp>& removeOps,
                                      const std::vector<BatchSetKeyOp>& setOps,
                                      bool commit);

  struct TimeStatusResult
  {
    bool ok = false;
    KeyOpResult::ErrorKind errorKind = KeyOpResult::ErrorKind::FailedPrecondition;
    QString error;
    double duration = 0.0;
    double seconds = 0.0;
  };

  [[nodiscard]] TimeStatusResult timeStatus(size_t animationId) const;

  [[nodiscard]] KeyOpResult setTime(size_t animationId, double seconds, bool cancelRendering);

  [[nodiscard]] KeyOpResult setCameraInterpolationMethod(size_t animationId, const QString& method);

  struct CameraInterpolationMethodResult
  {
    bool ok = false;
    KeyOpResult::ErrorKind errorKind = KeyOpResult::ErrorKind::FailedPrecondition;
    QString error;
    QString method;
  };

  [[nodiscard]] CameraInterpolationMethodResult cameraInterpolationMethod(size_t animationId) const;

  // return info of animation with id, animation mesh exist, otherwise crash
  Z3DAnimation& animation(size_t id)
  {
    return *m_idToAnimationPacks.at(id)->animation;
  }

  // ZObjDoc interface

public:
  bool save(size_t id) override;

  bool saveAs(size_t id) override;

  // Programmatic save to a specific path (no UI). Mirrors UI behavior:
  // writes the file, updates pack path/name/tooltip, clears unsaved flag,
  // and updates doc info so UI reflects the new name.
  bool saveToPath(size_t id, const QString& path);

  [[nodiscard]] QString typeName() const override
  {
    return "Animation3D";
  }

  [[nodiscard]] QString typePluralName() const override
  {
    return "Animation3Ds";
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

  // append another 3d animation into this doc
  size_t addAnimation(Z3DAnimation* animation, const QString& path, const QString& name = "");

private:
  struct AnimationPack
  { // animation and its associated data
    AnimationPack(Z3DAnimation* animation_, const QString& path_, QString name = "");

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

  QAction* m_loadAnimationsAction = nullptr;

  Z3DRenderingEngine* m_view = nullptr;
};

} // namespace nim
