#pragma once

#include "z3dobjview.h"

#include <QMetaObject>
#include <QPointer>
#include <QThread>

namespace nim {

template<class DocType, class FilterType>
class Z3DFilterView : public Z3DObjView
{
public:
  Z3DFilterView(DocType& doc, Z3DRenderingEngine& engine)
    : Z3DObjView(engine)
    , m_doc(doc)
  {
    // Object removal is initiated on the UI thread (ZDoc) but must detach 3D filters on the rendering thread before
    // the underlying object data is destroyed. Use a blocking queued connection across threads to avoid use-after-free
    // when 3D paging/rendering tasks still reference the object.
    const Qt::ConnectionType removeConnType =
      (m_doc.thread() == this->thread()) ? Qt::DirectConnection : Qt::BlockingQueuedConnection;
    connect(&m_doc, &DocType::objAboutToBeRemoved, this, &Z3DFilterView::onObjAboutToBeRemoved, removeConnType);
    connect(&m_doc, &DocType::objVisibleChanged, this, &Z3DFilterView::onObjVisibleChanged);
    connect(&m_doc, &DocType::selectionChangedFromDoc, this, &Z3DFilterView::onSelectionChanged);
  }

  // Z3DObjView interface

public:
  [[nodiscard]] const ZObjDoc& doc() const override
  {
    return m_doc;
  }

  [[nodiscard]] bool hasObj(size_t id) const override
  {
    return m_idToFilter.contains(id);
  }

  [[nodiscard]] ZBBox<glm::dvec3> boundBoxOfObj(size_t id) const override
  {
    ZBBox<glm::dvec3> res;
    if (hasObj(id)) {
      res.expand(m_idToFilter.at(id)->axisAlignedBoundBox());
    }
    return res;
  }

  [[nodiscard]] ZBBox<glm::dvec3> boundBoxOfObjAfterClipping(size_t id) const override
  {
    ZBBox<glm::dvec3> res;
    if (hasObj(id)) {
      res.expand(m_idToFilter.at(id)->axisAlignedBoundBoxAfterClipping());
    }
    return res;
  }

  void read(size_t id, const json::object& json) override
  {
    if (hasObj(id)) {
      m_idToFilter.at(id)->read(json);
    }
  }

  void write(size_t id, json::object& json) const override
  {
    if (hasObj(id)) {
      m_idToFilter.at(id)->write(json);
    }
  }

  std::shared_ptr<ZWidgetsGroup> viewSettingWidgetsGroupOf(size_t id) override
  {
    auto it = m_idToFilter.find(id);
    if (it != m_idToFilter.end()) {
      return it->second->widgetsGroup();
    }
    return {};
  }

  [[nodiscard]] std::vector<Z3DFilter*> filters() const override
  {
    std::vector<Z3DFilter*> filters;
    filters.reserve(m_idToFilter.size());
    for (const auto& idFilter : m_idToFilter) {
      filters.push_back(idFilter.second.get());
    }
    return filters;
  }

  const std::map<size_t, std::unique_ptr<FilterType>>& idToFilter()
  {
    return m_idToFilter;
  }

protected:
  void updateBoundBox() override
  {
    resetBoundBox();
    for (const auto& idFilter : m_idToFilter) {
      if (m_doc.isObjVisible(idFilter.first) || m_doc.isObjSelected(idFilter.first)) {
        expandBoundBox(idFilter.second->axisAlignedBoundBox());
      }
    }
    m_engine.updateBoundBox();
  }

  void onObjAboutToBeRemoved(size_t id) override
  {
    auto it = m_idToFilter.find(id);
    if (it == m_idToFilter.end()) {
      return;
    }
    FilterType* viewControl = it->second.get();
    m_engine.removeEventListener(*viewControl);
    // m_engine.canvas().getGLFocus();
    m_idToFilter.erase(it);
    m_engine.updatePipeline();
    updateBoundBox();
  }

  void onObjVisibleChanged(size_t id, bool v) override
  {
    auto it = m_idToFilter.find(id);
    if (it == m_idToFilter.end()) {
      return;
    }
    // Avoid feedback loops: Doc-driven visibility updates should not re-enter
    // the view->doc synchronization path. Do not block signals here because
    // setVisible() must still invalidate rendering.
    const size_t prevSyncId = m_docDrivenVisibilitySyncId;
    m_docDrivenVisibilitySyncId = id;
    it->second->setVisible(v);
    m_docDrivenVisibilitySyncId = prevSyncId;
    updateBoundBox();
  }

  void onSelectionChanged(const std::vector<size_t>& selected, const std::vector<size_t>& deselected) override
  {
    for (auto id : selected) {
      auto it = m_idToFilter.find(id);
      if (it == m_idToFilter.end()) {
        return;
      }
      it->second->setSelected(true);
    }
    for (auto id : deselected) {
      auto it = m_idToFilter.find(id);
      if (it == m_idToFilter.end()) {
        return;
      }
      it->second->setSelected(false);
    }
    updateBoundBox();
  }

  void onObjSelectedFromView(bool append) override
  {
    if (FilterType* filter = qobject_cast<FilterType*>(sender())) {
      for (const auto& idFilter : m_idToFilter) {
        if (idFilter.second.get() == filter) {
          // Selection changes originate from the 3D view on the rendering thread, but
          // ZDoc + selection models live on the UI thread. Hop across threads via Qt.
          ZDoc& doc = m_doc.doc();
          const QPointer<ZDoc> docPtr(&doc);
          const size_t id = idFilter.first;
          auto applySelection = [docPtr, id, append]() {
            if (!docPtr) {
              return;
            }
            if (append) {
              docPtr->appendSelectObj(id);
            } else {
              docPtr->clearAndSelectObj(id);
            }
          };

          if (QThread::currentThread() == doc.thread()) {
            applySelection();
          } else {
            QMetaObject::invokeMethod(&doc, std::move(applySelection), Qt::QueuedConnection);
          }
          // updateBoundBox();
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
          ZDoc& doc = m_doc.doc();
          const QPointer<ZDoc> docPtr(&doc);
          const size_t id = idFilter.first;
          auto applyDeselection = [docPtr, id]() {
            if (!docPtr) {
              return;
            }
            docPtr->deselectObj(id);
          };

          if (QThread::currentThread() == doc.thread()) {
            applyDeselection();
          } else {
            QMetaObject::invokeMethod(&doc, std::move(applyDeselection), Qt::QueuedConnection);
          }
          // updateBoundBox();
          return;
        }
      }
    }
  }

  void onObjVisibleChangedFromView(bool v) override
  {
    if (FilterType* filter = qobject_cast<FilterType*>(sender())) {
      for (const auto& idFilter : m_idToFilter) {
        if (idFilter.second.get() == filter) {
          if (m_docDrivenVisibilitySyncId == idFilter.first) {
            return;
          }
          // View-driven visibility updates originate on the rendering thread, but
          // ZDoc + the object model live on the UI thread.
          ZDoc& doc = m_doc.doc();
          const QPointer<ZDoc> docPtr(&doc);
          const size_t id = idFilter.first;
          auto applyVisibility = [docPtr, id, v]() {
            if (!docPtr) {
              return;
            }
            docPtr->setObjVisible(id, v);
          };

          if (QThread::currentThread() == doc.thread()) {
            applyVisibility();
          } else {
            QMetaObject::invokeMethod(&doc, std::move(applyVisibility), Qt::QueuedConnection);
          }
          return;
        }
      }
    }
  }

protected:
  DocType& m_doc;
  std::map<size_t, std::unique_ptr<FilterType>> m_idToFilter;
  size_t m_docDrivenVisibilitySyncId = 0;
};

} // namespace nim
