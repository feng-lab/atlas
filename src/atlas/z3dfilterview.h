#pragma once

#include "z3dobjview.h"

namespace nim {

template<class DocType, class FilterType>
class Z3DFilterView : public Z3DObjView
{
public:
  Z3DFilterView(DocType& doc, Z3DRenderingEngine& engine)
    : Z3DObjView(engine)
    , m_doc(doc)
  {
    connect(&m_doc, &DocType::objAboutToBeRemoved, this, &Z3DFilterView::onObjAboutToBeRemoved);
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
    return m_idToFilter.find(id) != m_idToFilter.end();
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
    m_engine.networkEvaluator().updateNetwork();
    updateBoundBox();
  }

  void onObjVisibleChanged(size_t id, bool v) override
  {
    auto it = m_idToFilter.find(id);
    if (it == m_idToFilter.end()) {
      return;
    }
    it->second->setVisible(v);
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
          if (append) {
            m_doc.doc().appendSelectObj(idFilter.first);
          } else {
            m_doc.doc().clearAndSelectObj(idFilter.first);
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
          m_doc.doc().deselectObj(idFilter.first);
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
          if (m_doc.doc().isObjVisible(idFilter.first) != v) {
            m_doc.doc().setObjVisible(idFilter.first, v); // slow
            // updateBoundBox();
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
