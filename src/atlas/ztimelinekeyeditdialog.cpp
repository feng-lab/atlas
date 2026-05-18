#include "ztimelinekeyeditdialog.h"

#include "zanimation.h"
#include "zcameraparameterkey.h"
#include "zcameraparameteranimation.h"
#include "zparameterfactory.h"
#include <QVBoxLayout>
#include <QDialogButtonBox>
#include <optional>

namespace nim {

ZTimelineKeyEditDialog::ZTimelineKeyEditDialog(ZParameterAnimation& paraAnimation,
                                               ZParameterKey& paraKey,
                                               QWidget* parent)
  : QDialog(parent)
  , m_paraAnimation(paraAnimation)
  , m_paraKey(paraKey)
  , m_time("Time")
  , m_type("Type")
  , m_posTension("Position Tension", 0, -1, 1)
  , m_posContinuity("Position Continuity", 0, -1, 1)
  , m_posBias("Position Bias", 0, -1, 1)
  , m_rotTension("Rotation Tension", 0, -1, 1)
  , m_rotContinuity("Rotation Continuity", 0, -1, 1)
  , m_rotBias("Rotation Bias", 0, -1, 1)
{
  setModal(false);

  const auto& tcbFields = ZCameraParameterKey::tcbFieldInfos();
  m_posTension.setDescription(QString::fromUtf8(tcbFields[0].description));
  m_posContinuity.setDescription(QString::fromUtf8(tcbFields[1].description));
  m_posBias.setDescription(QString::fromUtf8(tcbFields[2].description));
  m_rotTension.setDescription(QString::fromUtf8(tcbFields[3].description));
  m_rotContinuity.setDescription(QString::fromUtf8(tcbFields[4].description));
  m_rotBias.setDescription(QString::fromUtf8(tcbFields[5].description));

  auto lo = new QGridLayout;

  m_time.set(m_paraKey.time());
  m_time.setStyle("SPINBOX");
  addWidget(m_time.createNameLabel(this), m_time.createWidget(this), lo);
  m_type.setSameAs(m_paraKey.typePara());
  addWidget(m_type.createNameLabel(this), m_type.createWidget(this), lo);

  if (m_paraAnimation.boundParameter()->type().contains("Span")) {
    m_para = ZParameterFactory::instance().create(m_paraAnimation.boundParameter()->name(),
                                                  m_paraAnimation.boundParameter()->type(),
                                                  this);
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
    // connect(m_para, &ZParameter::valueChanged, this, &ZTimelineKeyEditDialog::raiseAndActivate);
  } else {
    addWidget(m_paraAnimation.boundParameter()->createNameLabel(this),
              m_paraAnimation.boundParameter()->createWidget(this),
              lo);
    // connect(m_paraAnimation.boundParameter(), &ZParameter::valueChanged, this,
    //         &ZTimelineKeyEditDialog::raiseAndActivate);
  }

  if (m_paraAnimation.type() == "3DCamera") {
    auto* cpk = static_cast<ZCameraParameterKey*>(&m_paraKey);

    m_posTension.set(cpk->posTension());
    addWidget(m_posTension.createNameLabel(this), m_posTension.createWidget(this), lo);
    m_posContinuity.set(cpk->posContinuity());
    addWidget(m_posContinuity.createNameLabel(this), m_posContinuity.createWidget(this), lo);
    m_posBias.set(cpk->posBias());
    addWidget(m_posBias.createNameLabel(this), m_posBias.createWidget(this), lo);
    m_rotTension.set(cpk->rotTension());
    addWidget(m_rotTension.createNameLabel(this), m_rotTension.createWidget(this), lo);
    m_rotContinuity.set(cpk->rotContinuity());
    addWidget(m_rotContinuity.createNameLabel(this), m_rotContinuity.createWidget(this), lo);
    m_rotBias.set(cpk->rotBias());
    addWidget(m_rotBias.createNameLabel(this), m_rotBias.createWidget(this), lo);
  }

  QVBoxLayout* vlo = new QVBoxLayout(this);
  vlo->addLayout(lo);
  auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  vlo->addWidget(buttonBox);

  connect(buttonBox, &QDialogButtonBox::accepted, this, &ZTimelineKeyEditDialog::accept);
  connect(buttonBox, &QDialogButtonBox::rejected, this, &ZTimelineKeyEditDialog::reject);

  setWindowTitle(QString("Edit '%1' Key").arg(paraAnimation.name()));
}

void ZTimelineKeyEditDialog::setInitialValue()
{
  m_time.set(m_paraKey.time());
  m_type.setSameAs(m_paraKey.typePara());
  if (m_paraAnimation.boundParameter()->type().contains("Span")) {
    m_para->setValueSameAs(m_paraKey.value());
  } else {
    m_paraAnimation.boundParameter()->setValueSameAs(m_paraKey.value());
  }
  if (m_paraAnimation.type() == "3DCamera") {
    auto* cpk = static_cast<ZCameraParameterKey*>(&m_paraKey);

    m_posTension.set(cpk->posTension());
    m_posContinuity.set(cpk->posContinuity());
    m_posBias.set(cpk->posBias());
    m_rotTension.set(cpk->rotTension());
    m_rotContinuity.set(cpk->rotContinuity());
    m_rotBias.set(cpk->rotBias());
  }
}

void ZTimelineKeyEditDialog::accept()
{
  ZAnimation* owningAnimation = nullptr;
  for (QObject* p = &m_paraAnimation; p; p = p->parent()) {
    if (auto* a = qobject_cast<ZAnimation*>(p)) {
      owningAnimation = a;
      break;
    }
  }
  std::optional<ZAnimation::UndoSnapshot> before;
  if (owningAnimation) {
    before = owningAnimation->captureUndoSnapshot();
  }

  // m_paraAnimation.boundParameter()->disconnect(this);
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
    auto* cpk = static_cast<ZCameraParameterKey*>(&m_paraKey);
    cpk->setPosTension(m_posTension.get());
    cpk->setPosContinuity(m_posContinuity.get());
    cpk->setPosBias(m_posBias.get());
    cpk->setRotTension(m_rotTension.get());
    cpk->setRotContinuity(m_rotContinuity.get());
    cpk->setRotBias(m_rotBias.get());
  }
  m_paraAnimation.sortKeys();
  m_paraAnimation.blockSignals(false);
  m_paraAnimation.emitKeyChangedSignal(&m_paraKey);
  if (before) {
    owningAnimation->pushUndoSnapshotCommand("Edit Key", std::move(*before));
  }
  QDialog::accept();
}

void ZTimelineKeyEditDialog::reject()
{
  // m_paraAnimation.boundParameter()->disconnect(this);
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
  // QHBoxLayout *hbl = new QHBoxLayout;
  label->setMinimumWidth(125);
  label->setWordWrap(true);
  // hbl->addWidget(label);
  wg->setMinimumWidth(175);
  wg->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
  // hbl->addWidget(wg);
  // return hbl;
  int row = lo->rowCount() + 1;
  lo->addWidget(label, row, 0);
  lo->addWidget(wg, row, 1);
}

} // namespace nim
