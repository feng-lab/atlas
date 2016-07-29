#include "zanimation.h"

#include "zdoc.h"
#include <QHBoxLayout>
#include <QLabel>
#include "zparameteranimation.h"
#include "zobjdoc.h"
#include <QJsonObject>
#include <QJsonDocument>
#include <QFile>
#include <QFileInfo>
#include "zexception.h"
#include <QMessageBox>
#include <QApplication>
#include <QSettings>
#include <QDir>
#include "zview.h"
#include "z3dview.h"
#include "z3dcanvas.h"
#include "z3dcanvaspainter.h"
#include "zgraphicsview.h"
#include <QProgressDialog>
#include "zvideoencoder.h"

namespace {
// generic solution
template <class T>
int numDigits(T number)
{
  int digits = 0;
  if (number < 0) digits = 1; // remove this line if '-' counts as a digit
  while (number) {
    number /= 10;
    digits++;
  }
  return digits;
}

// partial specialization optimization for 32-bit numbers
template<>
int numDigits(int32_t x)
{
  if (x == std::numeric_limits<int>::min()) return 10 + 1;
  if (x < 0) return numDigits(-x) + 1;

  if (x >= 10000) {
    if (x >= 10000000) {
      if (x >= 100000000) {
        if (x >= 1000000000)
          return 10;
        return 9;
      }
      return 8;
    }
    if (x >= 100000) {
      if (x >= 1000000)
        return 7;
      return 6;
    }
    return 5;
  }
  if (x >= 100) {
    if (x >= 1000)
      return 4;
    return 3;
  }
  if (x >= 10)
    return 2;
  return 1;
}

//// partial-specialization optimization for 8-bit numbers
//template <>
//int numDigits(char n)
//{
//  // if you have the time, replace this with a static initialization to avoid
//  // the initial overhead & unnecessary branch
//  static char x[256] = {0};
//  if (x[0] == 0) {
//    for (char c = 1; c != 0; c++)
//      x[static_cast<int>(c)] = numDigits((int32_t)c);
//    x[0] = 1;
//  }
//  return x[static_cast<int>(n)];
//}
}

namespace nim {

void ZAnimationChangeDurationCommand::undo()
{
  m_ani->setDurationImpl(m_oldDuration);
}

void ZAnimationChangeDurationCommand::redo()
{
  m_ani->setDurationImpl(m_newDuration);
}

ZAnimation::ZAnimation(ZDoc &doc, QObject *parent)
  : QObject(parent)
  , m_doc(doc)
  , m_view(nullptr)
  , m_duration(10.0)
  , m_nextUniqueId(100)
{
  connect(&m_doc, &ZDoc::objAboutToBeRemoved, this, &ZAnimation::disableAnimationOf);
  m_videoEncoder = new ZVideoEncoder(this);
  connect(m_videoEncoder, &ZVideoEncoder::error, this, &ZAnimation::videoEncoderError);
  connect(m_videoEncoder, &ZVideoEncoder::finished, this, &ZAnimation::videoEncoderFinished);
  connect(m_videoEncoder, &ZVideoEncoder::canceled, this, &ZAnimation::videoEncoderCanceled);
}

ZAnimation::~ZAnimation()
{
  releaseParameters();
}

void ZAnimation::addKeyFrame(double time)
{
  CHECK(time >= 0.0);
  CHECK(m_view);

  bool objChange = false;
  bool sorted = false;

  blockSignals(true);

  addGlobalKey(time);

  QList<size_t> objs = m_doc.objs();
  if (!is2DAnimation()) {
    objs.push_back(1);  //background
    objs.push_back(2);  //axis
    objs.push_back(3);  //lighting
  }
  for (int k=0; k<objs.size(); ++k) {
    size_t id = objs[k];
    QString objTypeName;
    QJsonValue objJsonValue;
    if (id == 1) {
      objTypeName = "Background";
    } else if (id == 2) {
      objTypeName = "Axis";
    } else if (id == 3) {
      objTypeName = "Lighting";
    } else {
      ZObjDoc *objDoc = m_doc.idToDoc(id);
      objTypeName = objDoc->typeName();
      objJsonValue = objDoc->jsonValue(id);
    }
    if (objTypeName.contains("Animation", Qt::CaseInsensitive)) {
      continue;
    }
    std::shared_ptr<ZWidgetsGroup> wg = m_view->viewSettingWidgetsGroupOf(id);
    const std::vector<ZParameter*>& paraList = wg->getParameterList();

    AnimationObj *aniObj = findBoundId(id);
    if (!aniObj) {
      auto aO = std::make_unique<AnimationObj>(objTypeName, objJsonValue);
      aO->uniqueId = m_nextUniqueId++;
      aO->boundId = id;
      objChange = true;
      m_objList.push_back(std::move(aO));
      aniObj = m_objList[m_objList.size()-1].get();
    }
    auto& paraAnimationList = aniObj->objParaAnimations;

    for (size_t i=0; i<paraList.size(); ++i) {
      ZParameterKey *key = new ZParameterKey(time, *paraList[i]);
      bool found = false;
      for (size_t j=0; j<paraAnimationList.size(); ++j) {
        if (paraList[i] == paraAnimationList[j]->boundParameter()) {
          found = true;
          paraAnimationList[j]->addKey(key, false);
          if (j != i) {
            std::swap(paraAnimationList[i], paraAnimationList[j]);
            sorted = true;
          }
          break;
        }
      }
      if (!found) {
        objChange = true;
        auto paraAnimation = std::make_unique<ZParameterAnimation>(paraList[i]->name(), paraList[i]->type());
        paraAnimation->setParent(this);
        paraAnimation->bindParameter(*paraList[i]);
        paraAnimation->addKey(key, false);
        paraAnimationList.insert(paraAnimationList.begin() + i, std::move(paraAnimation));
      }
    }
  }
  blockSignals(false);

  if (objChange) {
    updateObjAnimation();
  } else if (sorted) {
    buildDisplayPacks();
    emit keysChanged();
  } else {
    emit keysChanged();
  }
}

void ZAnimation::setExpanded(size_t id, bool v)
{
  AnimationObj *aniObj = findUniqueId(id);
  if (aniObj && aniObj->isExpanded != v && aniObj->boundId != 0) {
    aniObj->isExpanded = v;
    buildDisplayPacks();
    emit expandChanged();
  }
}

void ZAnimation::toogleExpanded(size_t id)
{
  AnimationObj *aniObj = findUniqueId(id);
  if (aniObj && aniObj->boundId != 0) {
    aniObj->isExpanded = !aniObj->isExpanded;
    buildDisplayPacks();
    emit expandChanged();
  }
}

void ZAnimation::toogleShowAll(size_t id)
{
  AnimationObj *aniObj = findUniqueId(id);
  if (aniObj && aniObj->boundId != 0) {
    aniObj->isShowAll = !aniObj->isShowAll;
    buildDisplayPacks();
    emit expandChanged();
  }
}

void ZAnimation::setDuration(double duration)
{
  duration = std::max(1.0, duration);
  if (m_duration != duration)
    m_undoStack.push(new ZAnimationChangeDurationCommand(this, m_duration, duration));
}

void ZAnimation::setCurrentTime(double time)
{
  time = std::max(0.0, time);

  for (size_t i=0; i<m_objList.size(); ++i) {
    if (m_objList[i]->boundId == 0)
      continue;
    auto& paraAnimationList = m_objList[i]->objParaAnimations;
    for (size_t j=0; j<paraAnimationList.size(); ++j) {
      paraAnimationList[j]->setCurrentTime(time);
    }
  }
  for (size_t i=0; i<m_globalParaAnimations.size(); ++i) {
    m_globalParaAnimations[i]->setCurrentTime(time);
  }
}

void ZAnimation::removeObj(size_t id)
{
  size_t idx;
  AnimationObj *aniObj = findUniqueId(id, &idx);
  if (aniObj) {
    m_objList.erase(m_objList.begin() + idx);
    buildDisplayPacks();
    emit objChanged();
  }
}

void ZAnimation::removeRedundantKeys()
{
  blockSignals(true);
  for (size_t i=0; i<m_globalParaAnimations.size(); ++i) {
    m_globalParaAnimations[i]->removeRedundantKeys();
  }
  for (size_t i=0; i<m_objList.size(); ++i) {
    auto& palist = m_objList[i]->objParaAnimations;
    for (size_t j=0; j<palist.size(); ++j) {
      palist[j]->removeRedundantKeys();
    }
  }
  blockSignals(false);
  emit keysChanged();
}

void ZAnimation::rebindView()
{
  releaseParameters();
  if (!m_view)
    return;

  bindGlobalParameters();

  bool sorted = false;

  for (size_t k=0; k<m_objList.size(); ++k) {
    size_t id = m_objList[k]->boundId;
    if (id == 0)
      continue;
    std::shared_ptr<ZWidgetsGroup> wg = m_view->viewSettingWidgetsGroupOf(id);
    CHECK(wg);
    connect(wg.get(), &ZWidgetsGroup::widgetsGroupChanged, this, &ZAnimation::rebindView);
    sorted = bind(m_objList[k]->objParaAnimations, wg->getParameterList()) || sorted;
  }

  if (sorted) {
    buildDisplayPacks();
    emit objViewChanged();
  }
}

void ZAnimation::releaseView()
{
  if (m_view) {
    releaseParameters();
    m_view = nullptr;
  }
}

void ZAnimation::exportFixedSize3DAnimation(const QDir &dir, const QString &fn, double framePerSecond, int width, int height, Z3DScreenShotType sst)
{
  CHECK(m_view);
  if (!dir.exists()) {
    if (!dir.mkpath(".")) {
      QMessageBox::critical(QApplication::activeWindow(), "Error", QString("Can not create folder %1").arg(dir.path()));
      return;
    }
  }
  if (dir.exists(fn)) {
    QMessageBox msgBox(QApplication::activeWindow());
    msgBox.setText(tr("File %1 exists, overwrite?").arg(fn));
    msgBox.setInformativeText("");
    msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
    msgBox.setDefaultButton(QMessageBox::Cancel);
    int ret = msgBox.exec();

    if (ret == QMessageBox::Cancel) {
      return;
    } else {
      if (!QFile::remove(dir.filePath(fn))) {
        QMessageBox::critical(QApplication::activeWindow(), "Error", QString("Can not replace %1").arg(dir.filePath(fn)));
        return;
      }
    }
  }
  if (width % 2 == 1) {
    --width;
  }
  if (height % 2 == 1) {
    --height;
  }
  m_doc.hideAnimation3DView();
  int numFrame = std::ceil(m_duration * framePerSecond);
  QString title = "Exporting 3D Animation As Images...";
  if (sst == Z3DScreenShotType::HalfSideBySideStereoView)
    title = "Exporting 3D Animation As Half Side-By-Side Stereo Images...";
  else if (sst == Z3DScreenShotType::FullSideBySideStereoView)
    title = "Exporting 3D Animation As Full Side-By-Side Stereo Images...";
  QProgressDialog *progress = new QProgressDialog(title, "Cancel", 0, numFrame, QApplication::activeWindow());
  progress->setWindowModality(Qt::WindowModal);
  progress->setAttribute(Qt::WA_DeleteOnClose);
  progress->show();
  int fieldWidth = numDigits(numFrame);
  double time = 0;
  double timeIncrement = m_duration / numFrame;
  bool checkOverwrite = true;
  QString namePrefix = QFileInfo(fn).completeBaseName();
  for (int i=0; i<numFrame; i++) {
    progress->setValue(i);
    if (progress->wasCanceled())
      break;

    setCurrentTime(time);
    time += timeIncrement;
    QString filename = QString("%1%2.tif").arg(namePrefix).arg(i, fieldWidth, 10, QChar('0'));
    QString filepath = dir.filePath(filename);
    if (checkOverwrite) {
      if (dir.exists(filename)) {
        QMessageBox msgBox(QApplication::activeWindow());
        msgBox.setText(tr("File %1 exists, overwrite?").arg(filepath));
        msgBox.setInformativeText("");
        msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::YesToAll | QMessageBox::Cancel);
        msgBox.setDefaultButton(QMessageBox::Cancel);
        int ret = msgBox.exec();

        if (ret == QMessageBox::Yes) {
        } else if (ret == QMessageBox::YesToAll) {
          checkOverwrite = false;
        } else {
          break;
        }
      }
    }
    if (!static_cast<Z3DView*>(m_view)->takeFixedSizeScreenShot(filepath, width, height, sst)) {
      break;
    }
  }
  if (!progress->wasCanceled()) {
    QString filename = QString("%1%2.tif").arg(namePrefix).arg(numFrame, fieldWidth, 10, QChar('0'));
    QString filepath = dir.filePath(filename);
    if (checkOverwrite) {
      if (dir.exists(filename)) {
        QMessageBox msgBox(QApplication::activeWindow());
        msgBox.setText(tr("File %1 exists, overwrite?").arg(filepath));
        msgBox.setInformativeText("");
        msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::YesToAll | QMessageBox::Cancel);
        msgBox.setDefaultButton(QMessageBox::Cancel);
        int ret = msgBox.exec();

        if (ret != QMessageBox::Cancel) {
          return;
        }
        QFile::remove(dir.filePath(filename));
      }
    }
  }

  if (!progress->wasCanceled()) {
    progress->setLabelText("Compressing Video...");
    connect(m_videoEncoder, &ZVideoEncoder::error, progress, &QProgressDialog::reset);
    connect(m_videoEncoder, &ZVideoEncoder::finished, progress, &QProgressDialog::reset);
    connect(m_videoEncoder, &ZVideoEncoder::canceled, progress, &QProgressDialog::reset);
    connect(progress, &QProgressDialog::canceled, m_videoEncoder, &ZVideoEncoder::cancel);
    m_videoEncoder->encode(dir, namePrefix, fieldWidth, framePerSecond, dir.filePath(fn));
  }
}

void ZAnimation::export3DAnimation(const QDir &dir, const QString &fn, double framePerSecond, Z3DScreenShotType sst)
{
  CHECK(m_view);
  if (!dir.exists()) {
    if (!dir.mkpath(".")) {
      QMessageBox::critical(QApplication::activeWindow(), "Error", QString("Can not create folder %1").arg(dir.path()));
      return;
    }
  }
  if (dir.exists(fn)) {
    QMessageBox msgBox(QApplication::activeWindow());
    msgBox.setText(tr("File %1 exists, overwrite?").arg(fn));
    msgBox.setInformativeText("");
    msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
    msgBox.setDefaultButton(QMessageBox::Cancel);
    int ret = msgBox.exec();

    if (ret == QMessageBox::Cancel) {
      return;
    } else {
      if (!QFile::remove(dir.filePath(fn))) {
        QMessageBox::critical(QApplication::activeWindow(), "Error", QString("Can not replace %1").arg(dir.filePath(fn)));
        return;
      }
    }
  }
  m_doc.hideAnimation3DView();
  Z3DCanvas& canvas = static_cast<Z3DView*>(m_view)->canvas();
  int h = canvas.height();
  if (h % 2 == 1) {
    --h;
  }
  int w = canvas.width();
  if (w % 2 == 1) {
    --w;
  }
  if (canvas.width() % 2 == 1 || canvas.height() % 2 == 1) {
    LOG(INFO) << "Resize canvas size from (" << canvas.width() << ", " << canvas.height() << ") to (" << w << ", " << h << ").";
    canvas.resize(w, h);
  }
  int numFrame = std::ceil(m_duration * framePerSecond);
  QString title = "Exporting 3D Animation As Images...";
  if (sst == Z3DScreenShotType::HalfSideBySideStereoView)
    title = "Exporting 3D Animation As Half Side-By-Side Stereo Images...";
  else if (sst == Z3DScreenShotType::FullSideBySideStereoView)
    title = "Exporting 3D Animation As Full Side-By-Side Stereo Images...";
  QProgressDialog *progress = new QProgressDialog(title, "Cancel", 0, numFrame, QApplication::activeWindow());
  progress->setWindowModality(Qt::WindowModal);
  progress->show();
  int fieldWidth = numDigits(numFrame);
  double time = 0;
  double timeIncrement = m_duration / numFrame;
  bool checkOverwrite = true;
  QString namePrefix = QFileInfo(fn).completeBaseName();
  for (int i=0; i<numFrame; i++) {
    progress->setValue(i);
    if (progress->wasCanceled())
      break;

    setCurrentTime(time);
    time += timeIncrement;
    QString filename = QString("%1%2.tif").arg(namePrefix).arg(i, fieldWidth, 10, QChar('0'));
    QString filepath = dir.filePath(filename);
    if (checkOverwrite) {
      if (dir.exists(filename)) {
        QMessageBox msgBox(QApplication::activeWindow());
        msgBox.setText(tr("File %1 exists, overwrite?").arg(filepath));
        msgBox.setInformativeText("");
        msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::YesToAll | QMessageBox::Cancel);
        msgBox.setDefaultButton(QMessageBox::Cancel);
        int ret = msgBox.exec();

        if (ret == QMessageBox::Yes) {
        } else if (ret == QMessageBox::YesToAll) {
          checkOverwrite = false;
        } else {
          break;
        }
      }
    }
    if (!static_cast<Z3DView*>(m_view)->takeScreenShot(filepath, sst)) {
      break;
    }
  }
  if (!progress->wasCanceled()) {
    QString filename = QString("%1%2.tif").arg(namePrefix).arg(numFrame, fieldWidth, 10, QChar('0'));
    QString filepath = dir.filePath(filename);
    if (checkOverwrite) {
      if (dir.exists(filename)) {
        QMessageBox msgBox(QApplication::activeWindow());
        msgBox.setText(tr("File %1 exists, overwrite?").arg(filepath));
        msgBox.setInformativeText("");
        msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::YesToAll | QMessageBox::Cancel);
        msgBox.setDefaultButton(QMessageBox::Cancel);
        int ret = msgBox.exec();

        if (ret != QMessageBox::Cancel) {
          return;
        }
        QFile::remove(dir.filePath(filename));
      }
    }
  }

  if (!progress->wasCanceled()) {
    progress->setLabelText("Compressing Video...");
    connect(m_videoEncoder, &ZVideoEncoder::error, progress, &QProgressDialog::reset);
    connect(m_videoEncoder, &ZVideoEncoder::finished, progress, &QProgressDialog::reset);
    connect(m_videoEncoder, &ZVideoEncoder::canceled, progress, &QProgressDialog::reset);
    connect(progress, &QProgressDialog::canceled, m_videoEncoder, &ZVideoEncoder::cancel);
    m_videoEncoder->encode(dir, namePrefix, fieldWidth, framePerSecond, dir.filePath(fn));
  }
}

void ZAnimation::exportFixedSize2DAnimation(const QDir &dir, const QString &fn, double framePerSecond, int width, int height)
{
  CHECK(m_view);
  if (!dir.exists()) {
    if (!dir.mkpath(".")) {
      QMessageBox::critical(QApplication::activeWindow(), "Error", QString("Can not create folder %1").arg(dir.path()));
      return;
    }
  }
  if (dir.exists(fn)) {
    QMessageBox msgBox(QApplication::activeWindow());
    msgBox.setText(tr("File %1 exists, overwrite?").arg(fn));
    msgBox.setInformativeText("");
    msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
    msgBox.setDefaultButton(QMessageBox::Cancel);
    int ret = msgBox.exec();

    if (ret == QMessageBox::Cancel) {
      return;
    } else {
      if (!QFile::remove(dir.filePath(fn))) {
        QMessageBox::critical(QApplication::activeWindow(), "Error", QString("Can not replace %1").arg(dir.filePath(fn)));
        return;
      }
    }
  }
  if (width % 2 == 1) {
    --width;
  }
  if (height % 2 == 1) {
    --height;
  }
  ZGraphicsView& canvasPainter = static_cast<ZView*>(m_view)->graphicsView();
  int numFrame = std::ceil(m_duration * framePerSecond);
  QString title = "Exporting 2D Animation As Images...";
  QProgressDialog *progress = new QProgressDialog(title, "Cancel", 0, numFrame, QApplication::activeWindow());
  progress->setWindowModality(Qt::WindowModal);
  progress->setAttribute(Qt::WA_DeleteOnClose);
  progress->show();
  int fieldWidth = numDigits(numFrame);
  double time = 0;
  double timeIncrement = m_duration / numFrame;
  bool checkOverwrite = true;
  QString namePrefix = QFileInfo(fn).completeBaseName();
  QString err;
  for (int i=0; i<numFrame; i++) {
    progress->setValue(i);
    if (progress->wasCanceled())
      break;

    setCurrentTime(time);
    time += timeIncrement;
    QString filename = QString("%1%2.tif").arg(namePrefix).arg(i, fieldWidth, 10, QChar('0'));
    QString filepath = dir.filePath(filename);
    if (checkOverwrite) {
      if (dir.exists(filename)) {
        QMessageBox msgBox(QApplication::activeWindow());
        msgBox.setText(tr("File %1 exists, overwrite?").arg(filepath));
        msgBox.setInformativeText("");
        msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::YesToAll | QMessageBox::Cancel);
        msgBox.setDefaultButton(QMessageBox::Cancel);
        int ret = msgBox.exec();

        if (ret == QMessageBox::Yes) {
        } else if (ret == QMessageBox::YesToAll) {
          checkOverwrite = false;
        } else {
          break;
        }
      }
    }
    if (!canvasPainter.renderToImage(filepath, width, height, &err)) {
      QMessageBox::critical(QApplication::activeWindow(), "Error", err);
      break;
    }
  }
  if (!progress->wasCanceled()) {
    QString filename = QString("%1%2.tif").arg(namePrefix).arg(numFrame, fieldWidth, 10, QChar('0'));
    QString filepath = dir.filePath(filename);
    if (checkOverwrite) {
      if (dir.exists(filename)) {
        QMessageBox msgBox(QApplication::activeWindow());
        msgBox.setText(tr("File %1 exists, overwrite?").arg(filepath));
        msgBox.setInformativeText("");
        msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::YesToAll | QMessageBox::Cancel);
        msgBox.setDefaultButton(QMessageBox::Cancel);
        int ret = msgBox.exec();

        if (ret != QMessageBox::Cancel) {
          return;
        }
        QFile::remove(dir.filePath(filename));
      }
    }
  }

  if (!progress->wasCanceled()) {
    progress->setLabelText("Compressing Video...");
    connect(m_videoEncoder, &ZVideoEncoder::error, progress, &QProgressDialog::reset);
    connect(m_videoEncoder, &ZVideoEncoder::finished, progress, &QProgressDialog::reset);
    connect(m_videoEncoder, &ZVideoEncoder::canceled, progress, &QProgressDialog::reset);
    connect(progress, &QProgressDialog::canceled, m_videoEncoder, &ZVideoEncoder::cancel);
    m_videoEncoder->encode(dir, namePrefix, fieldWidth, framePerSecond, dir.filePath(fn));
  }
}

void ZAnimation::export2DAnimation(const QDir &dir, const QString &fn, double framePerSecond)
{
  CHECK(m_view);
  ZGraphicsView& canvasPainter = static_cast<ZView*>(m_view)->graphicsView();
  exportFixedSize2DAnimation(dir, fn, framePerSecond, canvasPainter.viewportSize().width(), canvasPainter.viewportSize().height());
}

void ZAnimation::disableAnimationOf(size_t id)
{
  AnimationObj *aniObj = findBoundId(id);
  if (aniObj) {
    auto& paraAnimationList = aniObj->objParaAnimations;
    for (size_t i=0; i<paraAnimationList.size(); ++i) {
      paraAnimationList[i]->releaseParameter();
    }
    aniObj->boundId = 0;
    buildDisplayPacks();
    emit objViewChanged();
  }
}

void ZAnimation::tryLinkAnimationWith(size_t id)
{
  ZObjDoc* doc = m_doc.idToDoc(id);
  QJsonValue jv = doc->jsonValue(id);
  for (size_t i=0; i<m_objList.size(); ++i) {
    if (m_objList[i]->boundId == 0 && m_objList[i]->objType == doc->typeName() && doc->isSameObj(m_objList[i]->objJsonValue, jv)) {
      m_objList[i]->boundId = id;
      std::shared_ptr<ZWidgetsGroup> wg = m_view->viewSettingWidgetsGroupOf(id);
      CHECK(wg);
      bind(m_objList[i]->objParaAnimations, wg->getParameterList());
      buildDisplayPacks();
      emit objViewChanged();
      return;
    }
  }
}

void ZAnimation::videoEncoderError(const QString &err)
{
  QMessageBox::critical(QApplication::activeWindow(), "Video Encoder Error",
                        tr("Can not encode video: %1").arg(err));
}

void ZAnimation::videoEncoderFinished()
{
  QMessageBox::information(QApplication::activeWindow(), "Finish Encoding Video",
                           "Finish Encoding Video.");
}

void ZAnimation::videoEncoderCanceled()
{
  QMessageBox::warning(QApplication::activeWindow(), "Video Encoder Canceled",
                       "Video Encoding was canceled.");
}

void ZAnimation::updateObjAnimation()
{
  for (size_t i=0; i<m_globalParaAnimations.size(); ++i) {
    connect(m_globalParaAnimations[i].get(), &ZParameterAnimation::keyChanged,
            this, &ZAnimation::keyChanged, Qt::UniqueConnection);
    connect(m_globalParaAnimations[i].get(), &ZParameterAnimation::keysChanged,
            this, &ZAnimation::keysChanged, Qt::UniqueConnection);
    connect(m_globalParaAnimations[i].get(), &ZParameterAnimation::keyAboutToDelete,
            this, &ZAnimation::keyAboutToDelete, Qt::UniqueConnection);
    connect(m_globalParaAnimations[i].get(), &ZParameterAnimation::colorChanged,
            this, &ZAnimation::colorChanged, Qt::UniqueConnection);
  }
  for (size_t i=0; i<m_objList.size(); ++i) {
    auto& palist = m_objList[i]->objParaAnimations;
    for (size_t j=0; j<palist.size(); ++j) {
      connect(palist[j].get(), &ZParameterAnimation::keyChanged,
              this, &ZAnimation::keyChanged, Qt::UniqueConnection);
      connect(palist[j].get(), &ZParameterAnimation::keysChanged,
              this, &ZAnimation::keysChanged, Qt::UniqueConnection);
      connect(palist[j].get(), &ZParameterAnimation::keyAboutToDelete,
              this, &ZAnimation::keyAboutToDelete, Qt::UniqueConnection);
      connect(palist[j].get(), &ZParameterAnimation::colorChanged,
              this, &ZAnimation::colorChanged, Qt::UniqueConnection);
    }
  }

  buildDisplayPacks();

  emit objChanged();
}

void ZAnimation::buildDisplayPacks()
{
  int row = 0;
  m_displayPacks.clear();
  for (size_t i=0; i<m_globalParaAnimations.size(); ++i) {
    ZAnimationDisplayPack pack;
    pack.name = m_globalParaAnimations[i]->name();
    pack.id = 0;
    pack.boundId = 0;
    pack.row = row++;
    pack.type = ZAnimationDisplayPack::Type::GlobalPara;
    pack.expanded = false;
    pack.showAll = false;
    pack.paraAnimation = m_globalParaAnimations[i].get();
    pack.objInfo = QString("Global Parameter %1").arg(m_globalParaAnimations[i]->name());
    m_displayPacks.push_back(pack);
  }
  for (size_t j=0; j<m_objList.size(); ++j) {
    ZAnimationDisplayPack pack;
    if (m_objList[j]->boundId == 0)
      pack.name = QString("%1 not loaded").arg(m_objList[j]->objType);
    else
      pack.name = m_doc.objName(m_objList[j]->boundId);
    pack.id = m_objList[j]->uniqueId;
    pack.boundId = m_objList[j]->boundId;
    pack.row = row++;
    pack.type = ZAnimationDisplayPack::Type::Object;
    pack.expanded = m_objList[j]->isExpanded;
    pack.showAll = m_objList[j]->isShowAll;
    pack.paraAnimation = nullptr;
    QString objInfo;
    if (m_objList[j]->objJsonValue.isString())
      objInfo = m_objList[j]->objJsonValue.toString();
    else if (m_objList[j]->objJsonValue.isObject()) {
      QJsonDocument jd(m_objList[j]->objJsonValue.toObject());
      objInfo = jd.toJson();
      QStringList infoList = objInfo.split("\n");
      if (infoList.size() > 6) {
        QStringList::iterator it = infoList.begin() + 6;
        *it = "...";
        infoList.erase(it+1, infoList.end());
        objInfo = infoList.join("\n");
      }
    } else {
      objInfo = m_objList[j]->objType;
    }
    pack.objInfo = objInfo;
    m_displayPacks.push_back(pack);

    if (pack.expanded && m_objList[j]->boundId != 0) {
      auto& paraAnimationList = m_objList[j]->objParaAnimations;
      for (size_t i=0; i<paraAnimationList.size(); ++i) {
        if (paraAnimationList[i]->numKeys() > 1 || m_objList[j]->isShowAll || paraAnimationList[i]->name() == "Visible") {
          ZAnimationDisplayPack pack;
          pack.name = paraAnimationList[i]->name();
          pack.id = m_objList[j]->uniqueId;
          pack.boundId = m_objList[j]->boundId;
          pack.row = row++;
          pack.type = ZAnimationDisplayPack::Type::ObjectPara;
          pack.expanded = false;
          pack.showAll = false;
          pack.paraAnimation = paraAnimationList[i].get();
          pack.objInfo = objInfo;
          m_displayPacks.push_back(pack);
        }
      }
      ZAnimationDisplayPack pack;
      pack.name = m_objList[j]->isShowAll ? QString("Hide ...") : QString("Show All ...");
      pack.id = m_objList[j]->uniqueId;
      pack.boundId = m_objList[j]->boundId;
      pack.row = row++;
      pack.type = ZAnimationDisplayPack::Type::ShowAll;
      pack.expanded = false;
      pack.showAll = false;
      pack.paraAnimation = nullptr;
      pack.objInfo = objInfo;
      m_displayPacks.push_back(pack);
    }
  }
}

void ZAnimation::releaseParameters()
{
  if (m_view) {
    for (size_t i=0; i<m_globalParaAnimations.size(); ++i) {
      m_globalParaAnimations[i]->releaseParameter();
    }
    for (size_t i=0; i<m_objList.size(); ++i) {
      if (m_objList[i]->boundId == 0)
        continue;
      auto& paraAnimationList = m_objList[i]->objParaAnimations;
      for (size_t j=0; j<paraAnimationList.size(); ++j) {
        paraAnimationList[j]->releaseParameter();
      }
    }
  }
}

bool ZAnimation::bind(std::vector<std::unique_ptr<ZParameterAnimation>> &paraAnimationList,
                      const std::vector<ZParameter*> &paraList)
{
  bool sorted = false;
  size_t foundNum = 0;
  for (size_t i=0; i<paraList.size(); ++i) {
    for (size_t j=0; j<paraAnimationList.size(); ++j) {
      if (paraList[i]->name() == paraAnimationList[j]->name() &&
          paraList[i]->type() == paraAnimationList[j]->type()) {
        paraAnimationList[j]->bindParameter(*paraList[i]);
        if (j != foundNum) {
          std::swap(paraAnimationList[j], paraAnimationList[foundNum]);
          sorted = true;
        }
        foundNum++;
        break;
      }
    }
  }
  return sorted;
}

void ZAnimation::readContent(const QString &fn, const QString &jsonKey)
{
  try {
  QFile file(fn);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    throw ZIOException(tr("Can not open file"));
  }

  m_objList.clear();
  m_nextUniqueId = 100;

  QByteArray saveData = file.readAll();

  QJsonDocument loadDoc(QJsonDocument::fromJson(saveData));
  if (loadDoc.isNull() || loadDoc.isEmpty() || !loadDoc.isObject()) {
    throw ZIOException(tr("File format is incorrect"));
  }

  QJsonObject loadObj = loadDoc.object();
  if (!loadObj.contains(jsonKey) || !loadObj[jsonKey].isObject()) {
    throw ZIOException(tr("File is not %1 format").arg(jsonKey));
  }

  QDir::setCurrent(QFileInfo(fn).absolutePath());

  QString err;
  QJsonObject animationObj = loadObj[jsonKey].toObject();
  QJsonObject docObj = animationObj["Doc"].toObject();
  for (QJsonObject::const_iterator it = docObj.begin(); it != docObj.end(); ++it) {
    if (!it.key().contains(QChar(' '))) {
      err += QString("Invalid obj key %1\n").arg(it.key());
      continue;
    }
    int spaceIdx = it.key().indexOf(QChar(' '));
    QString type = it.key().left(spaceIdx);
    QString idString = it.key().mid(spaceIdx+1);
    if (idString.trimmed().isEmpty()) {
      err += QString("Invalid obj key %1\n").arg(it.key());
      continue;
    }
    bool ok;
    size_t id = idString.toLongLong(&ok);
    if (!ok || id == 0) {
      err += QString("Invalid obj key %1\n").arg(it.key());
      continue;
    }
    auto aniObj = std::make_unique<AnimationObj>(type, it.value());
    aniObj->uniqueId = id;
    m_nextUniqueId = std::max(m_nextUniqueId, aniObj->uniqueId + 1);
    m_objList.push_back(std::move(aniObj));
  }

  std::vector<std::unique_ptr<ZParameterAnimation>> globalParaAnimations;
  for (QJsonObject::const_iterator it = animationObj.begin(); it != animationObj.end(); ++it) {
    if (it.key() == "Duration") {
      setDuration(it.value().toDouble(1.));
    } else if (it.key() == "Background" || it.key() == "Axis" || it.key() == "Lighting") {
      auto aniObj = std::make_unique<AnimationObj>(it.key(), QJsonValue());
      aniObj->uniqueId = m_nextUniqueId++;
      if (it.key() == "Background")
        aniObj->boundId = 1;
      else if (it.key() == "Axis")
        aniObj->boundId = 2;
      else if (it.key() == "Lighting")
        aniObj->boundId = 3;
      QJsonObject jObj = it.value().toObject();
      for (QJsonObject::const_iterator it1 = jObj.begin(); it1 != jObj.end(); ++it1) {
        std::unique_ptr<ZParameterAnimation> pa(ZParameterAnimation::create(it1.key(), it1.value()));
        if (pa) {
          aniObj->objParaAnimations.push_back(std::move(pa));
        }
      }
      m_objList.push_back(std::move(aniObj));
    } else if (it.key() != "Doc" && it.key() != "Version") {
      bool isObj = !it.key().contains(' ');
      if (isObj) {
        bool ok;
        size_t objectId = it.key().toLongLong(&ok);
        if (ok) {
          AnimationObj* aniObj = findUniqueId(objectId);
          if (aniObj) {
            QJsonObject jObj = it.value().toObject();
            for (QJsonObject::const_iterator it1 = jObj.begin(); it1 != jObj.end(); ++it1) {
              std::unique_ptr<ZParameterAnimation> pa(ZParameterAnimation::create(it1.key(), it1.value()));
              if (pa) {
                aniObj->objParaAnimations.push_back(std::move(pa));
              }
            }
          }
        } else {
          err += QString("Unknown animation object %1\n").arg(it.key());
        }
      } else {
        std::unique_ptr<ZParameterAnimation> pa(ZParameterAnimation::create(it.key(), it.value()));
        if (pa) {
          globalParaAnimations.push_back(std::move(pa));
        } else {
          err += QString("Can not parse key %1\n").arg(it.key());
        }
      }
    }
  }

  // match global parameters
  for (size_t i=0; i<globalParaAnimations.size(); ++i) {
    for (size_t j=0; j<m_globalParaAnimations.size(); ++j) {
      if (globalParaAnimations[i]->name() == m_globalParaAnimations[j]->name() &&
          globalParaAnimations[i]->type() == m_globalParaAnimations[j]->type()) {
        m_globalParaAnimations[j] = std::move(globalParaAnimations[i]);
        break;
      }
    }
  }

  // read files
  std::map<size_t, size_t> idmap = m_doc.read(docObj, err);
  if (idmap.empty()) {
    err += QString("%1 %2 contains zero valid objects.\n").arg(jsonKey).arg(fn);
  } else {
    for (size_t i=0; i<m_objList.size(); ++i) {
      if (idmap.find(m_objList[i]->uniqueId) != idmap.end()) {
        m_objList[i]->boundId = idmap.at(m_objList[i]->uniqueId);
        // original jsonvalue might be relative path, we need to convert them to absolute path (if file exist)
        m_objList[i]->objJsonValue = m_doc.idToDoc(m_objList[i]->boundId)->jsonValue(m_objList[i]->boundId);
      }
    }
  }

  if (!err.isEmpty()) {
    QMessageBox::critical(QApplication::activeWindow(), "load file error", err);
    LOG(WARNING) << err;
  }

  updateObjAnimation();
  QApplication::processEvents();
  }
  catch (const ZException & e) {
    throw ZIOException(QString("Can not load animation %1: %2").arg(fn).arg(e.what()));
  }
}

void ZAnimation::writeContent(const QString &fn, const QString &jsonKey)
{
  try {
  QSettings settings;
  if (settings.value(QString("Animation/removeRedundantKeysWhenSaving"), QVariant(true)).toBool())
    removeRedundantKeys();

  QFileInfo fi(fn);
  QString nName;
  for (int i=0; i<10000000; ++i) {
#ifdef _MSC_VER
    nName = fi.absolutePath() + QString("\\") + fi.baseName() + QString("_WritingTmp%1_.").arg(i) + fi.completeSuffix();
#else
    nName = fi.absolutePath() + QString("/") + fi.baseName() + QString("_WritingTmp%1_.").arg(i) + fi.completeSuffix();
#endif
    if (!QFile::exists(nName)) {
      break;
    }
  }

  QFile file(nName);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
    throw ZIOException(tr("Can not open file"));
  }

  QJsonObject animationObj;
  animationObj.insert("Version", QJsonValue(1.0));

  QJsonObject docObj;
  for (size_t i=0; i<m_objList.size(); ++i) {
    if (m_objList[i]->objType == "Axis" || m_objList[i]->objType == "Background" || m_objList[i]->objType == "Lighting")
      continue;
    docObj.insert(QString("%1 %2").arg(m_objList[i]->objType).arg(m_objList[i]->uniqueId),
                  m_objList[i]->objJsonValue);
  }
  animationObj.insert("Doc", docObj);

  animationObj.insert("Duration", m_duration);
  for (size_t i=0; i<m_globalParaAnimations.size(); ++i) {
    m_globalParaAnimations[i]->write(animationObj);
  }
  for (size_t i=0; i<m_objList.size(); ++i) {
    size_t id = m_objList[i]->uniqueId;
    const auto& pas = m_objList[i]->objParaAnimations;
    if (!pas.empty()) {
      QJsonObject jObj;
      for (size_t j=0; j<pas.size(); ++j) {
        pas[j]->write(jObj);
      }
      QString name;
      if (m_objList[i]->objType == "Background") {
        name = m_objList[i]->objType;
      } else if (m_objList[i]->objType == "Axis") {
        name = m_objList[i]->objType;
      } else if (m_objList[i]->objType == "Lighting") {
        name = m_objList[i]->objType;
      } else {
        name = QString("%1").arg(id);
      }
      animationObj.insert(name, jObj);
    }
  }

  QJsonObject saveObj;
  saveObj.insert(jsonKey, animationObj);

  QJsonDocument saveDoc(saveObj);
  if (file.write(saveDoc.toJson()) == -1) {
    throw ZIOException(file.errorString());
  }

  if ((QFile::exists(fn) && !QFile::remove(fn)) || !QFile::rename(nName, fn)) {
    throw ZIOException(QString("Can not replace old file with new file %2").arg(nName));
  }
  }
  catch (const ZException & e) {
    throw ZIOException(QString("Can not save animation %1: %2").arg(fn).arg(e.what()));
  }
}

void ZAnimation::setDurationImpl(double duration)
{
  duration = std::max(1.0, duration);
  if (m_duration != duration) {
    m_duration = duration;
    emit durationChanged(m_duration);
  }
}

ZAnimation::AnimationObj *ZAnimation::findBoundId(size_t id, size_t *idx)
{
  for (size_t i=0; i<m_objList.size(); ++i) {
    if (m_objList[i]->boundId == id) {
      if (idx) *idx = i;
      return m_objList[i].get();
    }
  }
  return nullptr;
}

ZAnimation::AnimationObj *ZAnimation::findUniqueId(size_t id, size_t *idx)
{
  for (size_t i=0; i<m_objList.size(); ++i) {
    if (m_objList[i]->uniqueId == id) {
      if (idx) *idx = i;
      return m_objList[i].get();
    }
  }
  return nullptr;
}

const ZAnimation::AnimationObj *ZAnimation::findBoundId(size_t id, size_t *idx) const
{
  for (size_t i=0; i<m_objList.size(); ++i) {
    if (m_objList[i]->boundId == id) {
      if (idx) *idx = i;
      return m_objList[i].get();
    }
  }
  return nullptr;
}

const ZAnimation::AnimationObj *ZAnimation::findUniqueId(size_t id, size_t *idx) const
{
  for (size_t i=0; i<m_objList.size(); ++i) {
    if (m_objList[i]->uniqueId == id) {
      if (idx) *idx = i;
      return m_objList[i].get();
    }
  }
  return nullptr;
}

} // namespace nim
