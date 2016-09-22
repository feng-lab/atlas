#include "ztimelinekeyeditdialog.h"

#include "zoptionparameter.h"
#include "zcameraparameterkey.h"
#include "zcameraparameteranimation.h"
#include "zparameterfactory.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QDialogButtonBox>

namespace nim {

ZTimelineKeyEditDialog::ZTimelineKeyEditDialog(ZParameterAnimation& paraAnimation, ZParameterKey& paraKey,
                                               QWidget* parent)
  : QDialog(parent)
  , m_paraAnimation(paraAnimation)
  , m_paraKey(paraKey)
  , m_time("Time")
  , m_type("Type")
  , m_posTension("Tension", 0, -1, 1)
  , m_posContinuity("Continuity", 0, -1, 1)
  , m_posBias("Bias", 0, -1, 1)
{
  setModal(false);

  auto lo = new QGridLayout;

  m_time.set(m_paraKey.time());
  m_time.setStyle("SPINBOX");
  addWidget(m_time.createNameLabel(this), m_time.createWidget(this), lo);
  m_type.setSameAs(*m_paraKey.typePara());
  addWidget(m_type.createNameLabel(this), m_type.createWidget(this), lo);

  if (m_paraAnimation.boundParameter()->type().contains("Span")) {
    m_para = ZParameterFactory::instance().create(m_paraAnimation.boundParameter()->name(),
                                                  m_paraAnimation.boundParameter()->type(), this);
    m_para->setSameAs(*m_paraAnimation.boundParameter());
    connect(m_para, &ZParameter::valueChanged, m_paraAnimation.boundParameter(), &ZParameter::updateFromSender);
    m_para->setStyle("SPINBOX");
    if (m_para->type() == "IntSpan") {
      static_cast<ZIntSpanParameter*>(m_para)->setRange(std::numeric_limits<int>::lowest(),
                                                        std::numeric_limits<int>::max());
    } else if (m_para->type() == "FloatSpan") {
      static_cast<ZFloatSpanParameter*>(m_para)->setRange(std::numeric_limits<float>::lowest(),
                                                          std::numeric_limits<float>::max());
    } else if (m_para->type() == "DoubleSpan") {
      static_cast<ZDoubleSpanParameter*>(m_para)->setRange(std::numeric_limits<double>::lowest(),
                                                           std::numeric_limits<double>::max());
    }
    addWidget(m_para->createNameLabel(this), m_para->createWidget(this), lo);
    //connect(m_para, &ZParameter::valueChanged, this, &ZTimelineKeyEditDialog::raiseAndActivate);
  } else {
    addWidget(m_paraAnimation.boundParameter()->createNameLabel(this),
              m_paraAnimation.boundParameter()->createWidget(this), lo);
    //connect(m_paraAnimation.boundParameter(), &ZParameter::valueChanged, this,
    //        &ZTimelineKeyEditDialog::raiseAndActivate);
  }

  if (m_paraAnimation.type() == "3DCamera") {
    ZCameraParameterKey* cpk = static_cast<ZCameraParameterKey*>(&m_paraKey);

    m_posTension.set(cpk->posTension());
    addWidget(m_posTension.createNameLabel(this), m_posTension.createWidget(this), lo);
    m_posContinuity.set(cpk->posContinuity());
    addWidget(m_posContinuity.createNameLabel(this), m_posContinuity.createWidget(this), lo);
    m_posBias.set(cpk->posBias());
    addWidget(m_posBias.createNameLabel(this), m_posBias.createWidget(this), lo);
  }

  QVBoxLayout* vlo = new QVBoxLayout(this);
  vlo->addLayout(lo);
  QDialogButtonBox* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  vlo->addWidget(buttonBox);

  connect(buttonBox, &QDialogButtonBox::accepted, this, &ZTimelineKeyEditDialog::accept);
  connect(buttonBox, &QDialogButtonBox::rejected, this, &ZTimelineKeyEditDialog::reject);

  setWindowTitle(QString("Edit '%1' Key").arg(paraAnimation.name()));
}

void ZTimelineKeyEditDialog::setInitialValue()
{
  if (m_paraAnimation.boundParameter()->type().contains("Span")) {
    m_para->setValueSameAs(m_paraKey.value());
  } else {
    m_paraAnimation.boundParameter()->setValueSameAs(m_paraKey.value());
  }
}

void ZTimelineKeyEditDialog::accept()
{
  //m_paraAnimation.boundParameter()->disconnect(this);
  m_paraAnimation.blockSignals(true);
  m_paraKey.setTime(m_time.get());
  m_paraKey.setType(m_type.get());
  //
  if (m_paraAnimation.boundParameter()->type().contains("Span")) {
    m_paraKey.setValue(*m_para);
  } else {
    m_paraKey.setValue(*m_paraAnimation.boundParameter());
  }
  if (m_paraAnimation.type() == "3DCamera") {
    ZCameraParameterKey* cpk = static_cast<ZCameraParameterKey*>(&m_paraKey);
    cpk->setPosTension(m_posTension.get());
    cpk->setPosContinuity(m_posContinuity.get());
    cpk->setPosBias(m_posBias.get());
    ZCameraParameterAnimation* cpa = static_cast<ZCameraParameterAnimation*>(&m_paraAnimation);
    cpa->buildSpline();
  }
  m_paraAnimation.blockSignals(false);
  m_paraAnimation.emitKeyChangedSignal(&m_paraKey);
  QDialog::accept();
}

void ZTimelineKeyEditDialog::reject()
{
  //m_paraAnimation.boundParameter()->disconnect(this);
  if (m_paraAnimation.boundParameter()->type().contains("Span")) {
    m_para->setValueSameAs(m_paraKey.value());
  } else {
    m_paraAnimation.boundParameter()->setValueSameAs(m_paraKey.value());
  }
  QDialog::reject();
}

void ZTimelineKeyEditDialog::raiseAndActivate()
{
  showNormal();
  raise();
  activateWindow();
}

void ZTimelineKeyEditDialog::addWidget(QLabel* label, QWidget* wg, QGridLayout* lo)
{
  //QHBoxLayout *hbl = new QHBoxLayout;
  label->setMinimumWidth(125);
  label->setWordWrap(true);
  //hbl->addWidget(label);
  wg->setMinimumWidth(175);
  wg->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
  //hbl->addWidget(wg);
  //return hbl;
  int row = lo->rowCount() + 1;
  lo->addWidget(label, row, 0);
  lo->addWidget(wg, row, 1);
}

} // namespace nim
