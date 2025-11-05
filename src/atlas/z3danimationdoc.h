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

  // Create a new animation and return its id
  size_t createNewAnimationAndReturnId(const QString& name = "");

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

  void removeObj(size_t id) override;

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
