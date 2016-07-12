#ifndef Z3DFILTERVIEW_H
#define Z3DFILTERVIEW_H

#include "z3dobjview.h"

namespace nim {

template<class DocType, class FilterType>
class Z3DFilterView : public Z3DObjView
{
public:
  Z3DFilterView(DocType &doc, Z3DView &view)
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
  virtual const ZObjDoc& doc() const override { return m_doc; }
  virtual bool hasObj(size_t id) const override { return m_idToFilter.find(id) != m_idToFilter.end(); }

  virtual void read(size_t id, const QJsonObject &json) override
  {
    if (hasObj(id)) {
      m_idToFilter.at(id)->read(json);
    }
  }

  virtual void write(size_t id, QJsonObject &json) const override
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
    for (auto it = m_idToFilter.begin(); it != m_idToFilter.end(); ++it) {
      if (m_doc.isObjVisible(it->first) || m_doc.isObjSelected(it->first))
        expandBoundBox(it->second->axisAlignedBoundBox());
    }
    m_view.updateBoundBox();
  }

  virtual void onObjRemoved(size_t id) override
  {
    auto it = m_idToFilter.find(id);
    if (it == m_idToFilter.end())
      return;
    FilterType *viewControl = it->second.get();
    canvas().removeEventListener(viewControl);
    m_view.canvas().getGLFocus();
    m_idToFilter.erase(it);
    networkEvaluator().updateNetwork();
    updateBoundBox();
  }

  virtual void onAllObjsRemoved() override
  {
    if (m_idToFilter.empty())
      return;
    for (auto it = m_idToFilter.begin(); it != m_idToFilter.end(); ++it) {
      FilterType *viewControl = it->second.get();
      canvas().removeEventListener(viewControl);
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

  virtual void onSelectionChanged(const QList<size_t> &selected, const QList<size_t> &deselected) override
  {
    for (int i=0; i<selected.size(); ++i) {
      auto it = m_idToFilter.find(selected[i]);
      if (it == m_idToFilter.end())
        return;
      it->second->setSelected(true);
    }
    for (int i=0; i<deselected.size(); ++i) {
      auto it = m_idToFilter.find(deselected[i]);
      if (it == m_idToFilter.end())
        return;
      it->second->setSelected(false);
    }
    updateBoundBox();
  }

  virtual void onObjSelectedFromView(bool append) override
  {
    FilterType* filter = qobject_cast<FilterType*>(sender());
    if (filter) {
      for (auto it = m_idToFilter.begin(); it != m_idToFilter.end(); ++it) {
        if (it->second.get() == filter) {
          if (append)
            m_doc.doc().appendSelectObj(it->first);
          else
            m_doc.doc().clearAndSelectObj(it->first);
          updateBoundBox();
          return;
        }
      }
    }
  }

  virtual void onObjDeselectedFromView() override
  {
    FilterType* filter = qobject_cast<FilterType*>(sender());
    if (filter) {
      for (auto it = m_idToFilter.begin(); it != m_idToFilter.end(); ++it) {
        if (it->second.get() == filter) {
          m_doc.doc().deselectObj(it->first);
          updateBoundBox();
          return;
        }
      }
    }
  }

  virtual void onObjVisibleChangedFromView(bool v) override
  {
    FilterType* filter = qobject_cast<FilterType*>(sender());
    if (filter) {
      for (auto it = m_idToFilter.begin(); it != m_idToFilter.end(); ++it) {
        if (it->second.get() == filter) {
          if (m_doc.doc().isObjVisible(it->first) != v) {
            m_doc.doc().setObjVisible(it->first, v);
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

#endif // Z3DFILTERVIEW_H
