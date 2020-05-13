#pragma once

#include "zpuncta.h"
#include "zglmutils.h"
#include "zbbox.h"
#include <QUndoStack>
#include <QMenu>
#include <QAction>
#include <utility>
#include <vector>
#include <set>

namespace nim {

class ZPunctaDoc;

class ZPunctaPack : public QObject
{
Q_OBJECT
public:
  ZPunctaPack(ZPuncta puncta, const QString& path, ZPunctaDoc& pd, QObject* parent = nullptr);

  ~ZPunctaPack() override;

  void updateDerivedData();

  const QString& info() const;

  inline const QString& name() const
  { return m_name; }

  inline const QString& tooltip() const
  { return m_tooltip; }

  inline const QString& path() const
  { return m_path; }

  QUndoStack* undoStack()
  { return &m_undoStack; }

  QMenu& contextMenu();

  void save(const QString& fileName, const QString& format = "");

  const std::vector<const ZPunctum*>& punctaPts() const
  { return m_punctaPts; }

  const std::set<const ZPunctum*>& selectedPuncta() const
  { return m_selectedPuncta; }

  void setSelectedPuncta(const std::set<const ZPunctum*>& sp);

  const ZPuncta& puncta() const
  { return m_puncta; }

  void onPunctumSelected(const ZPunctum* p, bool append);

  ZBBox<glm::ivec4> boundBox() const;

  void deleteSelectedPuncta();

  void transferSelectedPuncta();

  void mergeSelectedPuncta();

  void splitSelectedPunctum();

protected:

  void updatePtsAndSelectedPuncta();

  void createContextMenu();

signals:

  void selectionChanged();

  void punctaChanged();

  void undoStackCleanChanged(bool clean);

protected:
  ZPuncta m_puncta;
  QString m_path;
  ZPunctaDoc& m_doc;
  QUndoStack m_undoStack;

  QAction* m_deleteSelectedPunctaAction = nullptr;
  QAction* m_transferSelectedPunctaToAnotherFileAction = nullptr;
  QAction* m_mergeSelectedPuntaAction = nullptr;
  QAction* m_splitSelectedPunctumAction = nullptr;
  QMenu m_contextMenu;

  // derived data
private:
  friend class ZPunctaEditCommand;

  mutable QString m_info;
  QString m_name;
  QString m_tooltip;
  std::vector<const ZPunctum*> m_punctaPts;
  std::set<const ZPunctum*> m_selectedPuncta;
};

class ZPunctaEditCommand : public QUndoCommand
{
public:
  explicit ZPunctaEditCommand(const QString& text, ZPunctaPack& pp,
                              std::set<const ZPunctum*> deletedPuncta = std::set<const ZPunctum*>(),
                              std::list<ZPunctum> addedPuncta = std::list<ZPunctum>())
    : QUndoCommand(text)
    , m_punctaPack(pp)
    , m_deletedSet(std::move(deletedPuncta))
    , m_addedPuncta(std::move(addedPuncta))
  {
    for (auto& p : m_addedPuncta) {
      m_addedSet.insert(&p);
    }
  }

  void undo() override
  {
    if (m_addedSet.empty() && m_deletedSet.empty()) {
      return;
    }
    CHECK(m_addedPuncta.empty());
    for (auto it = m_punctaPack.puncta().begin(); it != m_punctaPack.puncta().end();) {
      if (m_addedSet.find(&*it) != m_addedSet.end()) {
        auto itCopy = it;
        ++it;
        m_addedPuncta.splice(m_addedPuncta.end(), m_punctaPack.m_puncta.data(), itCopy);
      } else {
        ++it;
      }
    }
    CHECK(m_addedPuncta.size() == m_addedSet.size()) << m_addedPuncta.size() << " " << m_addedSet.size();
    m_punctaPack.m_puncta.data().splice(m_punctaPack.m_puncta.data().end(), m_deletedPuncta,
                                        m_deletedPuncta.begin(), m_deletedPuncta.end());
    m_punctaPack.updatePtsAndSelectedPuncta();
    emit m_punctaPack.punctaChanged();
  }

  void redo() override
  {
    if (m_addedSet.empty() && m_deletedSet.empty()) {
      return;
    }
    CHECK(m_deletedPuncta.empty());
    for (auto it = m_punctaPack.puncta().begin(); it != m_punctaPack.puncta().end();) {
      if (m_deletedSet.find(&*it) != m_deletedSet.end()) {
        auto itCopy = it;
        ++it;
        m_deletedPuncta.splice(m_deletedPuncta.end(), m_punctaPack.m_puncta.data(), itCopy);
      } else {
        ++it;
      }
    }
    CHECK(m_deletedPuncta.size() == m_deletedSet.size()) << m_deletedPuncta.size() << " " << m_deletedSet.size();
    m_punctaPack.m_puncta.data().splice(m_punctaPack.m_puncta.data().end(), m_addedPuncta,
                                        m_addedPuncta.begin(), m_addedPuncta.end());
    m_punctaPack.updatePtsAndSelectedPuncta();
    emit m_punctaPack.punctaChanged();
  }

protected:
  ZPunctaPack& m_punctaPack;
  std::set<const ZPunctum*> m_deletedSet;
  std::list<ZPunctum> m_deletedPuncta;
  std::set<const ZPunctum*> m_addedSet;
  std::list<ZPunctum> m_addedPuncta;
};

} // namespace nim




