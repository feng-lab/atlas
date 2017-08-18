#pragma once

#include "z3dobjview.h"

namespace nim {

template<class DocType, class FilterType>
class Z3DFilterView : public Z3DObjView
{
public:
  Z3DFilterView(DocType& doc, Z3DView& view)
    : Z3DObjView(view)
    , m_doc(doc)
  {
    connect(&m_doc, &DocType::objRemoved, this, &Z3DFilterView::onObjRemoved);
    connect(&m_doc, &DocType::allObjsRemoved, this, &Z3DFilterView::onAllObjsRemoved);
    connect(&m_doc, &DocType::objVisibleChanged, this, &Z3DFilterView::onObjVisibleChanged);
    connect(&m_doc, &DocType::selectionChangedFromDoc, this, &Z3DFilterView::onSelectionChanged);
  }

  // Z3DObjView interface
public:
  virtual const ZObjDoc& doc() const override
  { return m_doc; }

  virtual bool hasObj(size_t id) const override
  { return m_idToFilter.find(id) != m_idToFilter.end(); }

  virtual void read(size_t id, const QJsonObject& json) override
  {
    if (hasObj(id)) {
      m_idToFilter.at(id)->read(json);
    }
  }

  virtual void write(size_t id, QJsonObject& json) const override
  {
    if (hasObj(id)) {
      m_idToFilter.at(id)->write(json);
    }
  }

  virtual std::shared_ptr<ZWidgetsGroup> viewSettingWidgetsGroupOf(size_t id) override
  {
    auto it = m_idToFilter.find(id);
    if (it != m_idToFilter.end()) {
      return it->second->widgetsGroup();
    }
    return std::shared_ptr<ZWidgetsGroup>();
  }

protected:
  virtual void updateBoundBox() override
  {
    resetBoundBox();
    for (const auto& idFilter : m_idToFilter) {
      if (m_doc.isObjVisible(idFilter.first) || m_doc.isObjSelected(idFilter.first))
        expandBoundBox(idFilter.second->axisAlignedBoundBox());
    }
    m_view.updateBoundBox();
  }

  virtual void onObjRemoved(size_t id) override
  {
    auto it = m_idToFilter.find(id);
    if (it == m_idToFilter.end())
      return;
    FilterType* viewControl = it->second.get();
    canvas().removeEventListener(*viewControl);
    m_view.canvas().getGLFocus();
    m_idToFilter.erase(it);
    networkEvaluator().updateNetwork();
    updateBoundBox();
  }

  virtual void onAllObjsRemoved() override
  {
    if (m_idToFilter.empty())
      return;
    for (const auto& idFilter : m_idToFilter) {
      canvas().removeEventListener(*idFilter.second);
    }
    m_idToFilter.clear();
    networkEvaluator().updateNetwork();
    updateBoundBox();
  }

  virtual void onObjVisibleChanged(size_t id, bool v) override
  {
    auto it = m_idToFilter.find(id);
    if (it == m_idToFilter.end())
      return;
    it->second->setVisible(v);
    updateBoundBox();
  }

  virtual void onSelectionChanged(const QList<size_t>& selected, const QList<size_t>& deselected) override
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
    updateBoundBox();
  }

  virtual void onObjSelectedFromView(bool append) override
  {
    if (FilterType* filter = qobject_cast<FilterType*>(sender())) {
      for (const auto& idFilter : m_idToFilter) {
        if (idFilter.second.get() == filter) {
          if (append)
            m_doc.doc().appendSelectObj(idFilter.first);
          else
            m_doc.doc().clearAndSelectObj(idFilter.first);
          updateBoundBox();
          return;
        }
      }
    }
  }

  virtual void onObjDeselectedFromView() override
  {
    if (FilterType* filter = qobject_cast<FilterType*>(sender())) {
      for (const auto& idFilter : m_idToFilter) {
        if (idFilter.second.get() == filter) {
          m_doc.doc().deselectObj(idFilter.first);
          updateBoundBox();
          return;
        }
      }
    }
  }

  virtual void onObjVisibleChangedFromView(bool v) override
  {
    if (FilterType* filter = qobject_cast<FilterType*>(sender())) {
      for (const auto& idFilter : m_idToFilter) {
        if (idFilter.second.get() == filter) {
          if (m_doc.doc().isObjVisible(idFilter.first) != v) {
            m_doc.doc().setObjVisible(idFilter.first, v);  // slow
            updateBoundBox();
          }
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

