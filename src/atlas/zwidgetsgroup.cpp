#include "zwidgetsgroup.h"
#include "z3dcameraparameter.h"
#include "z3dtransformparameter.h"
#include "zparameter.h"

#include <QGroupBox>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QPushButton>
#include <QLabel>
#include <QToolBox>

namespace nim {

ZWidgetsGroup::ZWidgetsGroup(QWidget &widget, int visibleLevel)
  : QObject()
  , m_type(Type::Widget)
  , m_groupName("This is not a group")
  , m_widget(&widget)
  , m_parameter(nullptr)
  , m_visibleLevel(visibleLevel)
  , m_isSorted(true)
  , m_isVisible(true)
  , m_useToolBoxStyle(false)
{
}

ZWidgetsGroup::ZWidgetsGroup(const QString &groupName, int visibleLevel)
  : QObject()
  , m_type(Type::Group)
  , m_groupName(groupName)
  , m_widget(nullptr)
  , m_parameter(nullptr)
  , m_visibleLevel(visibleLevel)
  , m_isSorted(false)
  , m_cutOffbetweenBasicAndAdvancedLevel(1)
  , m_isVisible(true)
  , m_useToolBoxStyle(false)
{
}

ZWidgetsGroup::ZWidgetsGroup(ZParameter &parameter, int visibleLevel)
  : QObject()
  , m_type(Type::Parameter)
  , m_groupName("This is not a group")
  , m_widget(nullptr)
  , m_parameter(&parameter)
  , m_visibleLevel(visibleLevel)
  , m_isSorted(true)
  , m_isVisible(true)
  , m_useToolBoxStyle(false)
{
}

QList<ZParameter *> ZWidgetsGroup::getParameterList()
{
  QList<ZParameter*> res;
  if (m_type == Type::Parameter) {
    res.push_back(m_parameter);
  } else if (m_type == Type::Group) {
    if (!m_isSorted) {
      sortChildGroups();
    }
    for (int i=0; i<m_childGroups.size(); ++i) {
      QList<ZParameter*> tmpRes = m_childGroups[i]->getParameterList();
      for (int j=0; j<tmpRes.size(); ++j) {
        res.push_back(tmpRes[j]);
      }
    }
  }
  return res;
}

void ZWidgetsGroup::addChild(QWidget &widget, int visibleLevel)
{
  addChild(std::make_shared<ZWidgetsGroup>(widget, visibleLevel));
}

void ZWidgetsGroup::addChild(ZParameter &parameter, int visibleLevel)
{
  addChild(std::make_shared<ZWidgetsGroup>(parameter, visibleLevel));
}

void ZWidgetsGroup::addChild(std::shared_ptr<ZWidgetsGroup> child, bool atEnd)
{
  if (atEnd)
    m_childGroups.push_back(child);
  else
    m_childGroups.push_front(child);
  connect(child.get(), SIGNAL(widgetsGroupChanged()), this, SLOT(emitWidgetsGroupChangedSignal()));
  m_isSorted = false;
}

void ZWidgetsGroup::removeAllChildren()
{
  for (int i=0; i<m_childGroups.size(); i++) {
    m_childGroups[i]->disconnect(this);
  }
  m_childGroups.clear();
}

void ZWidgetsGroup::removeChild(const ZParameter &para)
{
  for (int i=0; i<m_childGroups.size(); i++) {
    if (m_childGroups[i]->m_type == Type::Parameter &&
        m_childGroups[i]->m_parameter == &para) {
      m_childGroups[i]->disconnect(this);
      m_childGroups.removeAt(i);
    }
  }
}

void ZWidgetsGroup::removeChild(const std::shared_ptr<ZWidgetsGroup> &child)
{
  for (int i=0; i<m_childGroups.size(); i++) {
    if (m_childGroups[i]->m_type == Type::Group &&
        m_childGroups[i] == child) {
      m_childGroups[i]->disconnect(this);
      m_childGroups.removeAt(i);
    }
  }
}

QWidget *ZWidgetsGroup::createWidget(bool createBasic, bool scroll, const QString &label)
{
  QLayout *lw = createLayout(createBasic);
  // if is boxLayout, add strech to fill the space
  QBoxLayout *blo = dynamic_cast<QBoxLayout*>(lw);
  if (blo)
    blo->addStretch();
  //
  if (blo && !label.isEmpty()) {
    blo->insertWidget(0, new QLabel(label));
    blo->insertSpacing(1, 20);
  }
  QWidget *widget = new QWidget();
  widget->setLayout(lw);
  if (scroll) {
    QScrollArea * sa = new QScrollArea();
    sa->setWidgetResizable(true);
    sa->setWidget(widget);

    //sa->setVisible(isVisible());

    return sa;
  } else {
    return widget;
  }
}

QLayout *ZWidgetsGroup::createLayout(bool createBasic)
{
  switch (m_type) {
  case Type::Widget: {
    QHBoxLayout *hbl = new QHBoxLayout;
    hbl->addWidget(m_widget);
    return hbl;
  }
  case Type::Parameter: {
    QHBoxLayout *hbl = new QHBoxLayout;
    if (dynamic_cast<Z3DCameraParameter*>(m_parameter)
        || dynamic_cast<Z3DTransformParameter*>(m_parameter)) {
      QWidget* wg = m_parameter->createWidget();
      hbl->addWidget(wg);
      return hbl;
    }
    QLabel *label = m_parameter->createNameLabel();
    label->setMinimumWidth(125);
    label->setWordWrap(true);
    hbl->addWidget(label);
    QWidget* wg = m_parameter->createWidget();
    wg->setMinimumWidth(175);
    wg->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
    hbl->addWidget(wg);

    return hbl;
  }
  default: /*case GROUP:*/ {
    QVBoxLayout *vbl = new QVBoxLayout;
    if (!m_isSorted)
      sortChildGroups();
    if (createBasic) {
      int i;
      for (i=0; i<m_childGroups.size() && m_childGroups[i]->m_visibleLevel
           <= m_cutOffbetweenBasicAndAdvancedLevel; i++) {
        QLayout *lw = m_childGroups[i]->createLayout(true);
        if (m_childGroups[i]->isGroup()) {
          QGroupBox *groupBox = new QGroupBox(m_childGroups[i]->getGroupName());
          groupBox->setLayout(lw);
          vbl->addWidget(groupBox);
        } else
          vbl->addLayout(lw);
      }
      if (i<m_childGroups.size()) {
        QPushButton *pb = new QPushButton("Advanced...");
        pb->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        vbl->addWidget(pb, 0, Qt::AlignHCenter | Qt::AlignVCenter);
        connect(pb, SIGNAL(clicked()), this, SLOT(emitRequestAdvancedWidgetSignal()));
      }
    } else {
      if (m_useToolBoxStyle) {
        QToolBox *toolBox = new QToolBox();
        for (int i=0; i<m_childGroups.size(); i++) {
          if (m_useToolBoxStyle) {
            if (m_childGroups[i]->isGroup()) {
              QWidget *wg = m_childGroups[i]->createWidget(false, false);
              toolBox->addItem(wg, m_childGroups[i]->getGroupName());
              // fully expand page, no scroll bar
              dynamic_cast<QScrollArea*>(wg->parentWidget()->parentWidget())->setSizePolicy(
                    QSizePolicy::Preferred, QSizePolicy::Fixed);
            } else {
              QLayout *lw = m_childGroups[i]->createLayout(false);
              vbl->addLayout(lw);
            }
          } else {
            if (m_childGroups[i]->isGroup()) {
              QGroupBox *groupBox = new QGroupBox(m_childGroups[i]->getGroupName());
              QLayout *lw = m_childGroups[i]->createLayout(false);
              groupBox->setLayout(lw);
              vbl->addWidget(groupBox);
            } else {
              QLayout *lw = m_childGroups[i]->createLayout(false);
              vbl->addLayout(lw);
            }
          }
        }
        vbl->addWidget(toolBox);
      } else {
        for (int i=0; i<m_childGroups.size(); i++) {
          if (m_childGroups[i]->isGroup()) {
            QGroupBox *groupBox = new QGroupBox(m_childGroups[i]->getGroupName());
            QLayout *lw = m_childGroups[i]->createLayout(false);
            groupBox->setLayout(lw);
            vbl->addWidget(groupBox);
          } else {
            QLayout *lw = m_childGroups[i]->createLayout(false);
            vbl->addLayout(lw);
          }
        }
      }
    }
    return vbl;
  }
  }
}

bool ZWidgetsGroup::operator <(const ZWidgetsGroup &other) const
{
  return m_visibleLevel < other.m_visibleLevel;
}

bool __widgetGroupPtVisibleLevelLessThan(const std::shared_ptr<ZWidgetsGroup> &s1, const std::shared_ptr<ZWidgetsGroup> &s2)
{
  return s1->visibleLevel() < s2->visibleLevel();
}

void ZWidgetsGroup::sortChildGroups()
{
  qStableSort(m_childGroups.begin(), m_childGroups.end(), __widgetGroupPtVisibleLevelLessThan);
  m_isSorted = true;
}

void ZWidgetsGroup::emitRequestAdvancedWidgetSignal()
{
  emit requestAdvancedWidget(m_groupName);
}

void ZWidgetsGroup::emitWidgetsGroupChangedSignal()
{
  emit widgetsGroupChanged();
}

} // namespace nim
