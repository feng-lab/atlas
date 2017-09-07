#pragma once

#include "zobjview.h"
#include "zexception.h"
#include <QMessageBox>
#include <QApplication>

namespace nim {

template<class DocType, class FilterType>
class ZFilterView : public ZObjView
{
public:
  ZFilterView(DocType& doc, ZView& view)
    : ZObjView(view)
    , m_doc(doc)
  {
    connect(&m_doc, &DocType::objRemoved, this, &ZFilterView::onObjRemoved);
    connect(&m_doc, &DocType::allObjsRemoved, this, &ZFilterView::onAllObjsRemoved);
    connect(&m_doc, &DocType::objVisibleChanged, this, &ZFilterView::onObjVisibleChanged);
    connect(&m_doc, &DocType::selectionChangedFromDoc, this, &ZFilterView::onSelectionChanged);
  }

  ~ZFilterView()
  {
    for (const auto& idFilter : m_idToFilter) {
      idFilter.second->releaseItemsOwnership();  // destroy view means mainwindow is closing, this will speed up the closing process
    }
  }

  // ZObjView interface
public:
  const ZObjDoc& doc() const override
  { return m_doc; }

  bool hasObj(size_t id) const override
  { return m_idToFilter.find(id) != m_idToFilter.end(); }

  void read(size_t id, const QJsonObject& json) override
  {
    if (hasObj(id)) {
      m_idToFilter.at(id)->read(json);
    }
  }

  void write(size_t id, QJsonObject& json) const override
  {
    if (hasObj(id)) {
      m_idToFilter.at(id)->write(json);
    }
  }

  void setNormalView(int slice, int time) override
  {
    for (const auto& idFilter : m_idToFilter) {
      try {
        idFilter.second->setNormalView(slice, time);
      }
      catch (const ZException& e) {
        QMessageBox::critical(QApplication::activeWindow(), qApp->applicationName(), e.what());
      }
    }
  }

  void setMaxZProjView(int time) override
  {
    for (const auto& idFilter : m_idToFilter) {
      try {
        idFilter.second->setMaxZProjView(time);
      }
      catch (const ZException& e) {
        QMessageBox::critical(QApplication::activeWindow(), qApp->applicationName(), e.what());
      }
    }
  }

  std::shared_ptr<ZWidgetsGroup> viewSettingWidgetsGroupOf(size_t id) override
  {
    auto it = m_idToFilter.find(id);
    if (it != m_idToFilter.end()) {
      return it->second->viewSettingWidgetsGroup();
    }
    return std::shared_ptr<ZWidgetsGroup>();
  }

  void deleteKeyPressed() override
  {
    for (const auto& idFilter : m_idToFilter) {
      try {
        idFilter.second->deleteKeyPressed();
      }
      catch (const ZException& e) {
        QMessageBox::critical(QApplication::activeWindow(), qApp->applicationName(), e.what());
      }
    }
  }

  void mousePressed(const QPointF& scenePos) override
  {
    for (const auto& idFilter : m_idToFilter) {
      try {
        idFilter.second->mousePressed(scenePos);
      }
      catch (const ZException& e) {
        QMessageBox::critical(QApplication::activeWindow(), qApp->applicationName(), e.what());
      }
    }
  }

  void mouseReleased(const QPointF& scenePos) override
  {
    for (const auto& idFilter : m_idToFilter) {
      try {
        idFilter.second->mouseReleased(scenePos);
      }
      catch (const ZException& e) {
        QMessageBox::critical(QApplication::activeWindow(), qApp->applicationName(), e.what());
      }
    }
  }

  void selectionChanged(const QList<QGraphicsItem*>& items) override
  {

  }

  void rotateClockwise() override
  {
    for (const auto& idFilter : m_idToFilter) {
      try {
        idFilter.second->rotateClockwise();
      }
      catch (const ZException& e) {
        QMessageBox::critical(QApplication::activeWindow(), qApp->applicationName(), e.what());
      }
    }
  }

  void rotateCounterclockwise() override
  {
    for (const auto& idFilter : m_idToFilter) {
      try {
        idFilter.second->rotateCounterclockwise();
      }
      catch (const ZException& e) {
        QMessageBox::critical(QApplication::activeWindow(), qApp->applicationName(), e.what());
      }
    }
  }

  int minViewPrecedence() const override
  {
    int res = std::numeric_limits<int>::max();
    for (const auto& idFilter : m_idToFilter) {
      res = std::min(res, idFilter.second->viewPrecedence());
    }
    return res;
  }

  int maxViewPrecedence() const override
  {
    int res = std::numeric_limits<int>::min();
    for (const auto& idFilter : m_idToFilter) {
      res = std::max(res, idFilter.second->viewPrecedence());
    }
    return res;
  }

protected:
  void updateBoundBox() override
  {
    resetBoundBox();
    for (const auto& idFilter : m_idToFilter) {
      expandBoundBox(idFilter.second->boundBox());
    }
    m_view.updateBoundBox();
  }

  void onObjRemoved(size_t id) override
  {
    auto it = m_idToFilter.find(id);
    if (it == m_idToFilter.end())
      return;
    m_idToFilter.erase(it);
    updateBoundBox();
  }

  void onAllObjsRemoved() override
  {
    if (m_idToFilter.empty())
      return;
    m_idToFilter.clear();
    updateBoundBox();
  }

  void onObjVisibleChanged(size_t id, bool v) override
  {
    auto it = m_idToFilter.find(id);
    if (it == m_idToFilter.end())
      return;
    it->second->setVisible(v);
  }

  void onSelectionChanged(const QList<size_t>& selected, const QList<size_t>& deselected) override
  {
    for (auto id : selected) {
      auto it = m_idToFilter.find(id);
      if (it == m_idToFilter.end())
        return;
      it->second->setSelected(true);
    }
    for (auto id : deselected) {
      auto it = m_idToFilter.find(id);
      if (it == m_idToFilter.end())
        return;
      it->second->setSelected(false);
    }
  }

  void onObjSelectedFromView(bool append) override
  {
    if (FilterType* filter = qobject_cast<FilterType*>(sender())) {
      for (const auto& idFilter : m_idToFilter) {
        if (idFilter.second.get() == filter) {
          if (append)
            m_doc.doc().appendSelectObj(idFilter.first);
          else
            m_doc.doc().clearAndSelectObj(idFilter.first);
          return;
        }
      }
    }
  }

  void onObjDeselectedFromView() override
  {
    if (FilterType* filter = qobject_cast<FilterType*>(sender())) {
      for (const auto& idFilter : m_idToFilter) {
        if (idFilter.second.get() == filter) {
          m_doc.doc().deselectObj(idFilter.first);
          return;
        }
      }
    }
  }

protected:
  DocType& m_doc;
  std::map<size_t, std::unique_ptr<FilterType>> m_idToFilter;
};

} // namespace nim

