#include "zwidgetsgroup.h"

#include "z3dcameraparameter.h"
#include "z3dtransformparameter.h"
#include "z2dtransformparameter.h"
#include "zparameter.h"
#include <QGroupBox>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QPushButton>
#include <QLabel>
#include <utility>

namespace {

bool widgetGroupPtVisibleLevelLessThan(const std::shared_ptr<nim::ZWidgetsGroup>& s1,
                                       const std::shared_ptr<nim::ZWidgetsGroup>& s2)
{
  return s1->visibleLevel() < s2->visibleLevel();
}

} // namespace

namespace nim {

ZWidgetsGroup::ZWidgetsGroup(QWidget& widget, int visibleLevel)
  : m_type(Type::Widget)
  , m_groupName("This is not a group")
  , m_widget(&widget)
  , m_visibleLevel(visibleLevel)
{
}

ZWidgetsGroup::ZWidgetsGroup(QString groupName, int visibleLevel)
  : m_type(Type::Group)
  , m_groupName(std::move(groupName))
  , m_visibleLevel(visibleLevel)
{
}

ZWidgetsGroup::ZWidgetsGroup(ZParameter& parameter, int visibleLevel)
  : m_type(Type::Parameter)
  , m_groupName("This is not a group")
  , m_parameter(&parameter)
  , m_visibleLevel(visibleLevel)
{
}

std::vector<ZParameter*> ZWidgetsGroup::getParameterList()
{
  std::vector<ZParameter*> res;
  if (m_type == Type::Parameter) {
    res.push_back(m_parameter);
  } else if (m_type == Type::Group) {
    if (!m_isSorted) {
      sortChildGroups();
    }
    for (const auto& childGroup : m_childGroups) {
      std::vector<ZParameter*> tmpRes = childGroup->getParameterList();
      for (auto pp : tmpRes) {
        res.push_back(pp);
      }
    }
  }
  return res;
}

void ZWidgetsGroup::addChild(QWidget& widget, int visibleLevel)
{
  addChild(std::make_shared<ZWidgetsGroup>(widget, visibleLevel));
}

void ZWidgetsGroup::addChild(ZParameter& parameter, int visibleLevel)
{
  addChild(std::make_shared<ZWidgetsGroup>(parameter, visibleLevel));
}

void ZWidgetsGroup::addChild(const std::shared_ptr<ZWidgetsGroup>& child, bool atEnd)
{
  if (atEnd) {
    m_childGroups.push_back(child);
  } else {
    m_childGroups.insert(m_childGroups.begin(), child);
  }
  connect(child.get(), &ZWidgetsGroup::widgetsGroupChanged, this, &ZWidgetsGroup::widgetsGroupChanged);
  m_isSorted = false;
}

void ZWidgetsGroup::removeAllChildren()
{
  for (const auto& childGroup : m_childGroups) {
    childGroup->disconnect(this);
  }
  m_childGroups.clear();
}

void ZWidgetsGroup::removeChild(const QWidget& widget)
{
  const auto origSize = m_childGroups.size();
  std::erase_if(m_childGroups, [&widget, this](const auto& child) {
    if (child->m_type == Type::Widget && child->m_widget == &widget) {
      child->disconnect(this);
      return true;
    }
    return false;
  });
  CHECK(m_childGroups.size() < origSize);
}

void ZWidgetsGroup::removeChild(const ZParameter& para)
{
  const auto origSize = m_childGroups.size();
  std::erase_if(m_childGroups, [&para, this](const auto& child) {
    if (child->m_type == Type::Parameter && child->m_parameter == &para) {
      child->disconnect(this);
      return true;
    }
    return false;
  });
  CHECK(m_childGroups.size() < origSize);
}

void ZWidgetsGroup::removeChild(const std::shared_ptr<ZWidgetsGroup>& childIn)
{
  const auto origSize = m_childGroups.size();
  std::erase_if(m_childGroups, [&childIn, this](const auto& child) {
    if (child == childIn) {
      child->disconnect(this);
      return true;
    }
    return false;
  });
  CHECK(m_childGroups.size() < origSize);
}

void ZWidgetsGroup::protectWidgetChildren()
{
  for (const auto& childGroup : m_childGroups) {
    if (childGroup->m_type == Type::Widget) {
      childGroup->m_widget->setParent(nullptr);
    }
  }
}

QWidget* ZWidgetsGroup::createWidget(bool createBasic, bool scroll, QLabel* label, bool noStretch)
{
  QLayout* lw = createLayout(createBasic);
  // if is boxLayout, add strech to fill the space
  if (auto blo = qobject_cast<QBoxLayout*>(lw)) {
    if (!noStretch) {
      blo->addStretch();
    }
    //
    if (label) {
      blo->insertWidget(0, label);
      blo->insertSpacing(1, 20);
    }
  }
  auto widget = new QWidget();
  widget->setLayout(lw);
  if (scroll) {
    auto sa = new QScrollArea();
    sa->setWidgetResizable(true);
    sa->setWidget(widget);
    sa->setFrameShape(QFrame::NoFrame);
    sa->setContentsMargins(0, 0, 0, 0);

    //sa->setVisible(isVisible());

    return sa;
  } else {
    return widget;
  }
}

QLayout* ZWidgetsGroup::createLayout(bool createBasic)
{
  switch (m_type) {
    case Type::Widget: {
      auto hbl = new QHBoxLayout;
      hbl->addWidget(m_widget);
      return hbl;
    }
    case Type::Parameter: {
      auto hbl = new QHBoxLayout;
      if (qobject_cast<Z3DCameraParameter*>(m_parameter) ||
        qobject_cast<Z3DTransformParameter*>(m_parameter) ||
        qobject_cast<Z2DTransformParameter*>(m_parameter)) {
        QWidget* wg = m_parameter->createWidget();
        hbl->addWidget(wg);
        return hbl;
      }
      QLabel* label = m_parameter->createNameLabel();
      label->setTextInteractionFlags(Qt::TextSelectableByMouse);
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
      auto vbl = new QVBoxLayout;
      if (!m_isSorted)
        sortChildGroups();
      if (createBasic) {
        size_t i;
        for (i = 0; i < m_childGroups.size() &&
                    m_childGroups[i]->m_visibleLevel <= m_cutOffbetweenBasicAndAdvancedLevel; ++i) {
          QLayout* lw = m_childGroups[i]->createLayout(true);
          if (m_childGroups[i]->isGroup()) {
            auto groupBox = new QGroupBox(m_childGroups[i]->getGroupName());
            groupBox->setLayout(lw);
            vbl->addWidget(groupBox);
          } else
            vbl->addLayout(lw);
        }
        if (i < m_childGroups.size()) {
          auto pb = new QPushButton("Advanced...");
          pb->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
          vbl->addWidget(pb, 0, Qt::AlignHCenter | Qt::AlignVCenter);
          connect(pb, &QPushButton::clicked, this, &ZWidgetsGroup::emitRequestAdvancedWidgetSignal);
        }
      } else {
        for (const auto& childGroup : m_childGroups) {
          if (childGroup->isGroup()) {
            auto groupBox = new QGroupBox(childGroup->getGroupName());
            QLayout* lw = childGroup->createLayout(false);
            groupBox->setLayout(lw);
            vbl->addWidget(groupBox);
          } else {
            QLayout* lw = childGroup->createLayout(false);
            vbl->addLayout(lw);
          }
        }
      }
      return vbl;
    }
  }
}

bool ZWidgetsGroup::operator<(const ZWidgetsGroup& other) const
{
  return m_visibleLevel < other.m_visibleLevel;
}

void ZWidgetsGroup::sortChildGroups()
{
  std::stable_sort(m_childGroups.begin(), m_childGroups.end(), widgetGroupPtVisibleLevelLessThan);
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
