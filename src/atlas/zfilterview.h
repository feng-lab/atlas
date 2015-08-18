#ifndef ZFILTERVIEW_H
#define ZFILTERVIEW_H

#include "zobjview.h"
#include "zexception.h"
#include <QMessageBox>
#include <QApplication>

namespace nim {

template<class DocType, class FilterType>
class ZFilterView : public ZObjView
{
public:
  ZFilterView(DocType &doc, ZView &view)
    : ZObjView(view)
    , m_doc(doc)
  {
    connect(&m_doc, SIGNAL(objRemoved(size_t,ZObjDoc*)), this, SLOT(onObjRemoved(size_t)));
    connect(&m_doc, SIGNAL(allObjsRemoved(ZObjDoc*)), this, SLOT(onAllObjsRemoved()));
    connect(&m_doc, SIGNAL(objVisibleChanged(size_t,bool)), this, SLOT(onObjVisibleChanged(size_t,bool)));
    connect(&m_doc, SIGNAL(selectionChangedFromDoc(QList<size_t>,QList<size_t>)),
            this, SLOT(onSelectionChanged(QList<size_t>,QList<size_t>)));
  }

  ~ZFilterView()
  {
    for (typename std::map<size_t, FilterType*>::iterator it = m_idToFilter.begin();
         it != m_idToFilter.end(); ++it) {
      it->second->releaseItemsOwnership();  // destroy view means mainwindow is closing, this will speed up the closing process
      delete it->second;
    }
  }

  // ZObjView interface
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

  virtual void setNormalView(int slice, int time) override
  {
    for (typename std::map<size_t, FilterType*>::iterator it = m_idToFilter.begin();
         it != m_idToFilter.end(); ++it) {
      try {
        it->second->setNormalView(slice, time);
      }
      catch (const ZException& e) {
        QMessageBox::critical(QApplication::activeWindow(), "Error", e.what());
      }
    }
  }

  virtual void setMaxZProjView(int time) override
  {
    for (typename std::map<size_t, FilterType*>::iterator it = m_idToFilter.begin();
         it != m_idToFilter.end(); ++it) {
      try {
        it->second->setMaxZProjView(time);
      }
      catch (const ZException& e) {
        QMessageBox::critical(QApplication::activeWindow(), "Error", e.what());
      }
    }
  }

  virtual void setViewport(const QRectF &rect, double scale) override
  {
    for (typename std::map<size_t, FilterType*>::iterator it = m_idToFilter.begin();
         it != m_idToFilter.end(); ++it) {
      try {
        it->second->setViewport(rect, scale);
      }
      catch (const ZException& e) {
        QMessageBox::critical(QApplication::activeWindow(), "Error", e.what());
      }
    }
  }

  virtual ZWidgetsGroup* viewSettingWidgetsGroupOf(size_t id) override
  {
    typename std::map<size_t, FilterType*>::iterator it = m_idToFilter.find(id);
    if (it != m_idToFilter.end()) {
      return it->second->viewSettingWidgetsGroup();
    }
    return nullptr;
  }

  virtual void deleteKeyPressed() override
  {
    for (typename std::map<size_t, FilterType*>::iterator it = m_idToFilter.begin();
         it != m_idToFilter.end(); ++it) {
      try {
        it->second->deleteKeyPressed();
      }
      catch (const ZException& e) {
        QMessageBox::critical(QApplication::activeWindow(), "Error", e.what());
      }
    }
  }

  virtual void mousePressed(const QPointF &scenePos) override
  {
    for (typename std::map<size_t, FilterType*>::iterator it = m_idToFilter.begin();
         it != m_idToFilter.end(); ++it) {
      try {
        it->second->mousePressed(scenePos);
      }
      catch (const ZException& e) {
        QMessageBox::critical(QApplication::activeWindow(), "Error", e.what());
      }
    }
  }

  virtual void mouseReleased(const QPointF &scenePos) override
  {
    for (typename std::map<size_t, FilterType*>::iterator it = m_idToFilter.begin();
         it != m_idToFilter.end(); ++it) {
      try {
        it->second->mouseReleased(scenePos);
      }
      catch (const ZException& e) {
        QMessageBox::critical(QApplication::activeWindow(), "Error", e.what());
      }
    }
  }

public slots:
  virtual void updateBoundBox() override
  {
    resetBoundBox();
    for (typename std::map<size_t, FilterType*>::iterator it = m_idToFilter.begin();
         it != m_idToFilter.end(); ++it) {
      expandBoundBox(it->second->boundBox());
    }
    m_view.updateBoundBox();
  }

protected slots:
  virtual void onObjRemoved(size_t id) override
  {
    typename std::map<size_t, FilterType*>::iterator it = m_idToFilter.find(id);
    if (it == m_idToFilter.end())
      return;
    FilterType *viewControl = it->second;
    delete viewControl;
    m_idToFilter.erase(it);
    updateBoundBox();
  }

  virtual void onAllObjsRemoved() override
  {
    if (m_idToFilter.empty())
      return;
    for (typename std::map<size_t, FilterType*>::iterator it = m_idToFilter.begin();
         it != m_idToFilter.end(); ++it) {
      FilterType *viewControl = it->second;
      delete viewControl;
    }
    m_idToFilter.clear();
    updateBoundBox();
  }

  virtual void onObjVisibleChanged(size_t id, bool v) override
  {
    typename std::map<size_t, FilterType*>::iterator it = m_idToFilter.find(id);
    if (it == m_idToFilter.end())
      return;
    it->second->setVisible(v);
  }

  virtual void onSelectionChanged(const QList<size_t> &selected, const QList<size_t> &deselected) override
  {
    for (int i=0; i<selected.size(); ++i) {
      typename std::map<size_t, FilterType*>::iterator it = m_idToFilter.find(selected[i]);
      if (it == m_idToFilter.end())
        return;
      it->second->setSelected(true);
    }
    for (int i=0; i<deselected.size(); ++i) {
      typename std::map<size_t, FilterType*>::iterator it = m_idToFilter.find(deselected[i]);
      if (it == m_idToFilter.end())
        return;
      it->second->setSelected(false);
    }
  }

  virtual void onObjSelectedFromView(bool append) override
  {
    FilterType* filter = qobject_cast<FilterType*>(sender());
    if (filter) {
      for (typename std::map<size_t, FilterType*>::iterator it = m_idToFilter.begin();
           it != m_idToFilter.end(); ++it) {
        if (it->second == filter) {
          if (append)
            m_doc.doc().appendSelectObj(it->first);
          else
            m_doc.doc().clearAndSelectObj(it->first);
          return;
        }
      }
    }
  }

  virtual void onObjDeselectedFromView() override
  {
    FilterType* filter = qobject_cast<FilterType*>(sender());
    if (filter) {
      for (typename std::map<size_t, FilterType*>::iterator it = m_idToFilter.begin();
           it != m_idToFilter.end(); ++it) {
        if (it->second == filter) {
          m_doc.doc().deselectObj(it->first);
          return;
        }
      }
    }
  }

protected:
  DocType& m_doc;
  std::map<size_t, FilterType*> m_idToFilter;
};

} // namespace nim

#endif // ZFILTERVIEW_H
