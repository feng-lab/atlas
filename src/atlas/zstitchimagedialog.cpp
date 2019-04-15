#include "zstitchimagedialog.h"

#include "zimg.h"
#include "zimgio.h"
#include "zimgdisplay.h"
#include "zimgnccmatch.h"
#include "zimgmerge.h"
#include "zstringutils.h"
#include "zsysteminfo.h"
#include "zlog.h"
#include "zfileutils.h"
#include "zcpuinfo.h"
#include "zlogwidget.h"
#include "ztheme.h"
#include <tbb/concurrent_unordered_map.h>
#include <tbb/parallel_for.h>
#include <tbb/task_scheduler_init.h>
#include <qtcsv/reader.h>
#include <fftw3.h>
#include <QtWidgets>

namespace nim {

ZTileImageWidget::ZTileImageWidget(QWidget* parent, QImage* image, const std::vector<std::vector<int>>& tileMatrix,
                                   QList<ZTile>* pTiles, const QStringList& filenames) :
  QWidget(parent), m_image(image), m_tileMatrix(tileMatrix), m_pTiles(pTiles)
{
  m_scaleFactor = 0.6;
  m_rubberBand = nullptr;
  m_filenames = filenames;
  if (filenames.size() == pTiles->size()) {
    for (int i = 0; i < m_filenames.size(); ++i) {
      std::vector<ZImgInfo> infos;
      ZImgIO().readInfos(m_filenames[i], infos);
      if (infos.size() != 1) {
        m_tileimages.clear();
        break;
      }
      ZImgThumbernail tn;
      ZImgIO().readThumbnail(m_filenames[i], tn);
      bool sbreak = false;
      std::vector<ZImg> imgs;
      for (size_t z = 0; z < infos[0].depth; ++z) {
        const auto& tbs = tn.planeAttachments(z, 0);
        if (tbs.empty()) {
          sbreak = true;
          break;
        }
        imgs.push_back(tbs[0]);
      }
      if (sbreak) {
        m_tileimages.clear();
        break;
      }
      ZImg maxProj = ZImg::combine(imgs, ImgMergeMode::Max);
      ZImgDisplay display(maxProj);
      for (size_t ch = 0; ch < maxProj.numChannels(); ++ch) {
        double min;
        double max;
        maxProj.createView(ch, 0).computeMinMax(min, max);
        if ((max - min) / (maxProj.dataRangeMax() - maxProj.dataRangeMin()) < 0.2) { // channel signal too low
          display.showChannel(ch, maxProj.dataRangeMin(), maxProj.dataRangeMax());
        } else {
          display.showChannel(ch, maxProj.dataRangeMin(), max);
        }
      }
      m_tileimages.push_back(display.toQImage());
    }
  }
  if (m_tileimages.size() == m_pTiles->size()) {
    int margin = 15;
    int tileSize = 128;
    int width = margin + (margin + tileSize) * m_tileMatrix[0].size();
    int height = margin + (margin + tileSize) * m_tileMatrix.size();
    *m_image = QImage(width, height, QImage::Format_ARGB32_Premultiplied);
    m_image->fill(0);
    for (size_t c = 0; c < m_tileMatrix[0].size(); ++c) {
      for (size_t r = 0; r < m_tileMatrix.size(); ++r) {
        if (m_tileMatrix[r][c] == 0) {
          continue;
        }
        CHECK(m_pTiles->at(m_tileMatrix[r][c] - 1).index == m_tileMatrix[r][c]);
        (*m_pTiles)[m_tileMatrix[r][c] - 1].region = QRect(margin + (margin + tileSize) * c,
                                                           margin + (margin + tileSize) * r,
                                                           tileSize, tileSize);
      }
    }
    QPainter painter(m_image);
    for (int i = 0; i < m_pTiles->size(); ++i) {
      QPoint tl = m_pTiles->at(i).region.topLeft();
      QPoint br = m_pTiles->at(i).region.bottomRight();
      painter.drawImage(QRectF(tl.x(), tl.y(), br.x() - tl.x() + 1, br.y() - tl.y() + 1),
                        m_tileimages[i],
                        QRectF(0, 0, m_tileimages[i].size().width(), m_tileimages[i].size().height()));
    }
  }
}

QSize ZTileImageWidget::minimumSizeHint() const
{
  if (m_image) {
    return QSize(std::min(600, m_image->width()),
                 std::min(600, m_image->height()));
  } else {
    return QSize(600, 600);
  }
}


QSize ZTileImageWidget::sizeHint() const
{
  if (!m_image) {
    return minimumSizeHint();
  }
  return m_image->size() * m_scaleFactor;
}

void ZTileImageWidget::paintEvent(QPaintEvent* /*event*/)
{
  if (m_image) {
    QPainter painter(this);

    QSize size = m_scaleFactor * m_image->size();
    painter.drawImage(QRectF(0, 0, size.width(), size.height()), *m_image,
                      QRectF(0, 0, m_image->size().width(), m_image->size().height()));

    if (m_tileimages.size() == m_pTiles->size()) {
      painter.setPen(QPen(QBrush(QColor(255, 255, 0, 255)), 4));
      for (int i = 0; i < m_pTiles->size(); ++i) {
        QPoint tl = m_pTiles->at(i).region.topLeft() * m_scaleFactor;
        QPoint br = m_pTiles->at(i).region.bottomRight() * m_scaleFactor;
        if (m_pTiles->at(i).bIsSelected) {
          painter.drawRect(
            QRectF(tl.x() - 4, tl.y() - 4, br.x() - tl.x() + m_scaleFactor + 4, br.y() - tl.y() + m_scaleFactor + 4));
        }
        QString str = QString("Image %1").arg(i + 1);
        painter.drawText(QRect(tl, br), str);
      }
    } else {
      painter.setPen(QPen(QBrush(QColor(255, 255, 0, 255)), 4));
      for (int i = 0; i < m_pTiles->size(); ++i) {
        QRect rect = QRect(m_pTiles->at(i).region.topLeft() * m_scaleFactor,
                           m_pTiles->at(i).region.bottomRight() * m_scaleFactor);
        if (m_pTiles->at(i).bIsSelected) {
          painter.fillRect(rect, QColor(255, 255, 0, 128));
        }
        QString str = QString("Image %1").arg(i + 1);
        painter.drawText(rect, str);
      }
    }
  }
}

void ZTileImageWidget::mousePressEvent(QMouseEvent* event)
{
  m_origin = event->pos();
  if (!m_rubberBand)
    m_rubberBand = new QRubberBand(QRubberBand::Rectangle, this);
  m_rubberBand->setGeometry(QRect(m_origin, QSize()));
  m_rubberBand->show();
}

void ZTileImageWidget::mouseMoveEvent(QMouseEvent* event)
{
  m_rubberBand->setGeometry(QRect(m_origin, event->pos()).normalized());
}

void ZTileImageWidget::mouseReleaseEvent(QMouseEvent* event)
{
  m_rubberBand->hide();
  QRect selRegion = QRect(m_origin / m_scaleFactor, event->pos() / m_scaleFactor).normalized();
  for (auto& tile : *m_pTiles) {
    if (tile.region.intersects(selRegion)) {
      tile.bIsSelected = !tile.bIsSelected;
    }
  }
  repaint();
}

void ZTileImageWidget::clearAllSelected()
{
  for (auto& tile : *m_pTiles) {
    tile.bIsSelected = false;
  }
  repaint();
}

void ZTileImageWidget::selectAll()
{
  for (auto& tile : *m_pTiles) {
    tile.bIsSelected = true;
  }
  repaint();
}

void ZTileImageWidget::saveAsImage(const QString& fn)
{
  QImage img(size(), QImage::Format_RGB32);
  QPainter painter(&img);
  render(&painter);
  img.save(fn);
}

void ZTileImageWidget::zoomIn()
{
  if (m_scaleFactor < 4.9) {
    m_scaleFactor += .2;
    resize(m_scaleFactor * m_image->size());
    repaint();
  }
}

void ZTileImageWidget::zoomOut()
{
  if (m_scaleFactor > 0.3) {
    m_scaleFactor -= .2;
    resize(m_scaleFactor * m_image->size());
    repaint();
  }
}

ZStitchImageDialog::ZStitchImageDialog(QWidget* parent)
  : ZImgProcessDialog(parent)
{
  m_nSel = -100;

  //createIOLayout();
  //createConnLayout();
  //createCommandOutputLayout();

  m_tabWidget = new QTabWidget;
  m_tabWidget->addTab(createIOWidget(), "Inputs and Outputs");
  m_tabWidget->addTab(createConnWidget(), "Tile Configuration");
  m_tabWidget->addTab(createCommandOutputWidget(), "Stitching output");
  auto mainLayout = new QVBoxLayout;
  //mainLayout->addWidget(m_ioGroupBox);
  //mainLayout->addWidget(m_connGroupBox);
  //mainLayout->addWidget(m_commandOutputGroupBox);
  mainLayout->addWidget(m_tabWidget);
  mainLayout->addWidget(createButtonBox("Stitch"));
  setLayout(mainLayout);

  setWindowTitle(tr("Stitch Stacks"));
}

ZStitchImageDialog::~ZStitchImageDialog()
{
  m_tileList.clear();
}

void ZStitchImageDialog::createWorker(ZImgProcess*& worker, QString& workerName)
{
  focusNextChild();

  QString resFilename = m_outputFileEdit->text();

  bool hasStack2 = false;
  if (m_inputStack1Filenames.isEmpty()) {
    throw ZImgException("no input");
  } else if (m_hasTwoInputStackSetCheckBox->isChecked()) {
    if (m_inputStack2Filenames.size() != m_inputStack1Filenames.size()) {
      throw ZImgException("input 2 has different number of files than input 1");
    }
    hasStack2 = true;
  }

  auto info = ZImg::readImgInfo(ZImgSource(m_inputStack1Filenames[0], ZImgRegion(), 0));
  ZImgInfo info2;
  if (hasStack2) {
    info2 = ZImg::readImgInfo(ZImgSource(m_inputStack2Filenames[0], ZImgRegion(), 0));
  }

  auto* workertmp = new ZStitchImage();
  workertmp->setInputFilenames(m_inputStack1Filenames, m_scene1ComboBox->currentIndex());
  workertmp->setResultFilename(resFilename);

  if (m_channel1ComboBox->currentIndex() == 1) { // use ch0 and ch1
    std::vector<size_t> chs;
    chs.push_back(0);
    chs.push_back(1);
    workertmp->setUseChannels(chs);
  } else if (m_channel1ComboBox->currentIndex() > 1) {
    std::vector<size_t> chs;
    chs.push_back(m_channel1ComboBox->currentIndex() - 2);
    workertmp->setUseChannels(chs);
  }

  if (m_bgsub1ComboBox->currentIndex() == 1) {
    std::vector<size_t> chs;
    for (size_t c = 0; c < info.numChannels; ++c) {
      chs.push_back(c);
    }
    workertmp->setRemoveBackgroundForChannels(chs);
  } else if (m_bgsub1ComboBox->currentIndex() > 1) {
    std::vector<size_t> chs;
    chs.push_back(m_bgsub1ComboBox->currentIndex() - 2);
    workertmp->setRemoveBackgroundForChannels(chs);
  }

  if (hasStack2) {
    std::vector<size_t> chsToUse;
    std::vector<size_t> chsToRB;

    if (m_channel2ComboBox->currentIndex() == 1) { // use ch0 and ch1
      chsToUse.push_back(0);
      chsToUse.push_back(1);
    } else if (m_channel2ComboBox->currentIndex() > 1) {
      chsToUse.push_back(m_channel2ComboBox->currentIndex() - 2);
    }

    if (m_bgsub2ComboBox->currentIndex() == 1) {
      for (size_t c = 0; c < info2.numChannels; ++c) {
        chsToRB.push_back(c);
      }
    } else if (m_bgsub2ComboBox->currentIndex() > 1) {
      chsToRB.push_back(m_bgsub2ComboBox->currentIndex() - 2);
    }

    workertmp->set2ndInput(m_inputStack2Filenames, m_scene2ComboBox->currentIndex(), chsToUse, chsToRB,
                           m_commonChannel1SpinBox->value() - 1,
                           m_commonChannel2SpinBox->value() - 1);
  }

  if (m_dsCheckBox->isChecked()) {
    workertmp->setDownsampleBeforeStitching(m_dsXSpinBox->value(), m_dsYSpinBox->value(), m_dsZSpinBox->value());
  }

  ImgMergeMode mergeMode;
  if (m_mergeModeComboBox->currentIndex() == 0) {
    mergeMode = ImgMergeMode::Max;
  } else if (m_mergeModeComboBox->currentIndex() == 1) {
    mergeMode = ImgMergeMode::Min;
  } else if (m_mergeModeComboBox->currentIndex() == 2) {
    mergeMode = ImgMergeMode::Mean;
  } else if (m_mergeModeComboBox->currentIndex() == 3) {
    mergeMode = ImgMergeMode::Median;
  } else {
    mergeMode = ImgMergeMode::First;
  }
  workertmp->setMergeMode(mergeMode);

  if (m_concatOnlyCheckBox->isChecked()) {
    workertmp->setConcatenateOnly();
  }

  workertmp->setStartResolution(m_intvXSpinBox->value(), m_intvYSpinBox->value(), m_intvZSpinBox->value());

  workertmp->setLogFile(resFilename + "_stitching_log.txt");

  workertmp->setMaxOverlapRate(m_overlapRateSpinBox->value() / 100.0);

  if (m_useConfigRadioButton->isChecked()) {
    ZImg tileGrid(ZImgInfo(3, 3, 3, 1, 1, 4, VoxelFormat::Signed));
    tileGrid.fill(0);
    tileGrid.setValue(1, ZVoxelCoordinate(1, 1, 1));  // set first tile into middle of 3x3x3 grid
    ZVoxelCoordinate coord2(1, 1, 1);

    if (m_configDim1ComboBox->currentIndex() == 0)
      coord2.x -= 1;
    else if (m_configDim1ComboBox->currentIndex() == 2)
      coord2.x += 1;

    if (m_configDim2ComboBox->currentIndex() == 0)
      coord2.y -= 1;
    else if (m_configDim2ComboBox->currentIndex() == 2)
      coord2.y += 1;

    if (m_configDim3ComboBox->currentIndex() == 0)
      coord2.z -= 1;
    else if (m_configDim3ComboBox->currentIndex() == 2)
      coord2.z += 1;

    if (coord2.x == 1 && coord2.y == 1 && coord2.z == 1) {
      workertmp->setBlindStitching();
    } else {
      tileGrid.setValue(2, coord2);
      workertmp->setTileGrid(tileGrid);
    }
  } else if (m_useTileImageRadioButton->isChecked()) {
    if (m_tileMatrix.empty() || m_tileList.isEmpty()) {
      throw ZImgException("no tile selection image");
    }
    size_t numCols = m_tileMatrix[0].size();
    size_t numRows = m_tileMatrix.size();
    ZImg tileGrid(ZImgInfo(numCols, numRows, 1, 1, 1, 4, VoxelFormat::Signed));
    tileGrid.fill(0);

    for (size_t r = 0; r < numRows; ++r) {
      for (size_t c = 0; c < numCols; ++c) {
        if (m_tileMatrix[r][c] > 0 && m_tileList[m_tileMatrix[r][c] - 1].bIsSelected) {
          tileGrid.setValue(m_tileMatrix[r][c], ZVoxelCoordinate(c, r));
        }
      }
    }

    workertmp->setTileGrid(tileGrid);
  } else if (m_useConnFileRadioButton->isChecked()) {
    if (m_connFileEdit->text().isEmpty()) {
      throw ZImgException("no conn text file");
    }
    workertmp->setConnInfoFromConnTextFile(m_connFileEdit->text());
  } else if (m_useFullConnectionRadioButton->isChecked()) {
    workertmp->setBlindStitching();
  } else if (m_useLayoutRadioButton->isChecked()) {
    int row = m_layout1SpinBox->value();
    int col = m_layout2SpinBox->value();
    workertmp->setTileGridFromLayout(row, col);
  } else if (m_restitchCZIRadioButton->isChecked()) {
    workertmp->setRestitch();
  }

  connect(workertmp, &ZStitchImage::resultReady, this, &ZStitchImageDialog::resultReady);

  worker = workertmp;
  workerName = "Stitching";

  if (m_useTileImageRadioButton->isChecked() && !m_tileImage.isNull()) {
    QString selectionImageOutputName = m_outputFileEdit->text();
    selectionImageOutputName.append("_TileSelectionInfo.tif");
    QImage image(m_tileImage);

    QPainter painter(&image);
    for (int i = 0; i < m_tileList.size(); ++i) {
      QRect rect = QRect(m_tileList.at(i).region.topLeft(),
                         m_tileList.at(i).region.bottomRight());
      if (m_tileList.at(i).bIsSelected) {
        //painter.fillRect(rect, QColor(255, 255, 0, 128));
        painter.setPen(QPen(QBrush(QColor(255, 255, 0, 255)), 4));
        auto tl = rect.topLeft();
        auto br = rect.bottomRight();
        painter.drawRect(
          QRectF(tl.x() - 4, tl.y() - 4, br.x() - tl.x() + 5, br.y() - tl.y() + 1 + 4));
      }
      QString str = QString("Image %1").arg(i + 1);
      painter.drawText(rect, str);
    }
    QImageWriter writer(selectionImageOutputName);
    if (!writer.write(image)) {
      LOG(ERROR) << writer.errorString();
    } else {
      LOG(INFO) << QString("%1 saved.").arg(selectionImageOutputName);
    }
  }
}

QLayout* ZStitchImageDialog::createIOLayout()
{
  // everything
  auto alllayout = new QVBoxLayout;
  auto allinputlayout = new QHBoxLayout;
  //input1
  auto input1vlayout = new QVBoxLayout;
  m_inputStack1FileEdit = new QTextEdit(this);
  m_inputStack1FileEdit->setReadOnly(true);
  m_selectInputStacks1Button = new QPushButton(tr("select input stacks 1:"), this);
  connect(m_selectInputStacks1Button, &QPushButton::clicked, this, &ZStitchImageDialog::selectInputStacks1);
  input1vlayout->addWidget(m_selectInputStacks1Button);
  input1vlayout->addWidget(m_inputStack1FileEdit);
  auto tmphlayout = new QHBoxLayout;
  QLabel* pl = new QLabel(tr("Use scene: "), this);
  pl->setToolTip(tr("scene used for stitching"));
  m_scene1ComboBox = new QComboBox(this);
  m_scene1ComboBox->addItem(tr("scene 1"));
  m_scene1ComboBox->setCurrentIndex(0);     //default use scene 0
  tmphlayout->addWidget(pl);
  tmphlayout->addWidget(m_scene1ComboBox);
  input1vlayout->addLayout(tmphlayout);
  tmphlayout = new QHBoxLayout;
  pl = new QLabel(tr("Use channel: "), this);
  pl->setToolTip(tr("channel used for stitching"));
  m_channel1ComboBox = new QComboBox(this);
  m_channel1ComboBox->addItem(tr("Average of all channels"));
  m_channel1ComboBox->addItem(tr("Average of Ch1 and Ch2"));
  m_channel1ComboBox->setCurrentIndex(0);     //default average all channels
  tmphlayout->addWidget(pl);
  tmphlayout->addWidget(m_channel1ComboBox);
  input1vlayout->addLayout(tmphlayout);
  tmphlayout = new QHBoxLayout;
  pl = new QLabel(tr("Remove Background: "), this);
  pl->setToolTip(tr("Remove Background (below most common intensity value)"));
  m_bgsub1ComboBox = new QComboBox(this);
  m_bgsub1ComboBox->addItem(tr("None"));
  m_bgsub1ComboBox->addItem(tr("All channels"));
  m_bgsub1ComboBox->setCurrentIndex(0);      //default do not remove background
  tmphlayout->addWidget(pl);
  tmphlayout->addWidget(m_bgsub1ComboBox);
  input1vlayout->addLayout(tmphlayout);
  //input 2
  auto input2vlayout = new QVBoxLayout;
  m_inputStack2FileEdit = new QTextEdit(this);
  m_inputStack2FileEdit->setReadOnly(true);
  m_selectInputStacks2Button = new QPushButton(tr("select input stacks 2:"), this);
  connect(m_selectInputStacks2Button, &QPushButton::clicked, this, &ZStitchImageDialog::selectInputStacks2);
  input2vlayout->addWidget(m_selectInputStacks2Button);
  input2vlayout->addWidget(m_inputStack2FileEdit);
  tmphlayout = new QHBoxLayout;
  pl = new QLabel(tr("Use scene: "), this);
  pl->setToolTip(tr("scene used for stitching"));
  m_labelsForTwoInputs.push_back(pl);
  m_scene2ComboBox = new QComboBox(this);
  m_scene2ComboBox->addItem(tr("scene 0"));
  m_scene2ComboBox->setCurrentIndex(0);     //default use scene 0
  tmphlayout->addWidget(pl);
  tmphlayout->addWidget(m_scene2ComboBox);
  tmphlayout = new QHBoxLayout;
  pl = new QLabel(tr("Use channel: "), this);
  pl->setToolTip(tr("channel used for stitching"));
  m_labelsForTwoInputs.push_back(pl);
  m_channel2ComboBox = new QComboBox(this);
  m_channel2ComboBox->addItem(tr("Average of all channels"));
  m_channel2ComboBox->addItem(tr("Average of Ch1 and Ch2"));
  m_channel2ComboBox->setCurrentIndex(0);     //default average all channels
  tmphlayout->addWidget(pl);
  tmphlayout->addWidget(m_channel2ComboBox);
  input2vlayout->addLayout(tmphlayout);
  tmphlayout = new QHBoxLayout;
  pl = new QLabel(tr("Remove Background: "), this);
  pl->setToolTip(tr("Remove Background (below most common intensity value)"));
  m_labelsForTwoInputs.push_back(pl);
  m_bgsub2ComboBox = new QComboBox(this);
  m_bgsub2ComboBox->addItem(tr("None"));
  m_bgsub2ComboBox->addItem(tr("All channels"));
  m_bgsub2ComboBox->setCurrentIndex(0);      //default do not remove background
  tmphlayout->addWidget(pl);
  tmphlayout->addWidget(m_bgsub2ComboBox);
  input2vlayout->addLayout(tmphlayout);
  //
  allinputlayout->addLayout(input1vlayout);
  allinputlayout->addLayout(input2vlayout);
  alllayout->addLayout(allinputlayout);
  // parameters
  auto layout = new QGridLayout;
  int row = 0;

  pl = new QLabel(tr("Common Channel: "), this);
  m_labelsForTwoInputs.push_back(pl);
  layout->addWidget(pl, row, 0);
  m_commonChannel1SpinBox = new QSpinBox(this);
  m_commonChannel1SpinBox->setRange(1, 10);
  m_commonChannel1SpinBox->setValue(2);
  m_commonChannel2SpinBox = new QSpinBox(this);
  m_commonChannel2SpinBox->setRange(1, 10);
  m_commonChannel2SpinBox->setValue(2);
  pl = new QLabel(tr("Ch"), this);
  pl->setAlignment(Qt::Alignment(Qt::AlignVCenter | Qt::AlignRight));
  m_labelsForTwoInputs.push_back(pl);
  layout->addWidget(pl, row, 1);
  layout->addWidget(m_commonChannel1SpinBox, row, 2);
  pl = new QLabel(tr(" = "), this);
  pl->setAlignment(Qt::Alignment(Qt::AlignVCenter | Qt::AlignLeft));
  m_labelsForTwoInputs.push_back(pl);
  layout->addWidget(pl, row, 3);
  pl = new QLabel(tr("Ch"), this);
  pl->setAlignment(Qt::Alignment(Qt::AlignVCenter | Qt::AlignRight));
  m_labelsForTwoInputs.push_back(pl);
  layout->addWidget(pl, row, 4);
  layout->addWidget(m_commonChannel2SpinBox, row, 5);
  row++;

  m_hasTwoInputStackSetCheckBox = new QCheckBox(tr("has two input stack sets"), this);
  m_hasTwoInputStackSetCheckBox->setToolTip(tr("stitch two stack sets with common channel"));
  connect(m_hasTwoInputStackSetCheckBox, &QCheckBox::stateChanged, this,
          &ZStitchImageDialog::hasTwoInputStackSetCheckBoxChanged);
  layout->addWidget(m_hasTwoInputStackSetCheckBox, row, 0);
  row++;

  pl = new QLabel(tr("Output File:"), this);
  m_outputFileEdit = new QLineEdit(this);
  m_selectOutputButton = new QToolButton(this);
  m_selectOutputButton->setText(tr("..."));
  connect(m_selectOutputButton, &QToolButton::clicked, this, &ZStitchImageDialog::selectOutputFile);
  layout->addWidget(pl, row, 0);
  layout->addWidget(m_outputFileEdit, row, 1, 1, 6);
  layout->addWidget(m_selectOutputButton, row, 7);
  row++;

  pl = new QLabel(tr("merge mode: "), this);
  pl->setToolTip(tr("merge mode"));
  m_mergeModeComboBox = new QComboBox(this);
  m_mergeModeComboBox->addItem(tr("Max"));
  m_mergeModeComboBox->addItem(tr("Min"));
  m_mergeModeComboBox->addItem(tr("Mean"));
  m_mergeModeComboBox->addItem(tr("Median"));
  m_mergeModeComboBox->addItem(tr("First"));
  m_mergeModeComboBox->setCurrentIndex(4);   //default First
  layout->addWidget(pl, row, 0);
  layout->addWidget(m_mergeModeComboBox, row, 2, 1, 2);
  row++;

  m_concatOnlyCheckBox = new QCheckBox(tr("only concatenate image"), this);
  m_concatOnlyCheckBox->setToolTip(tr("do not compute actual offset, just concat image together, for overview"));
  layout->addWidget(m_concatOnlyCheckBox, row, 0);
  row++;

  m_dsCheckBox = new QCheckBox(tr("downsample"), this);
  m_dsCheckBox->setToolTip(tr("Downsample stack before stitching"));
  connect(m_dsCheckBox, &QCheckBox::stateChanged, this, &ZStitchImageDialog::dsCheckBoxChanged);
  layout->addWidget(m_dsCheckBox, row, 0);
  m_dsXSpinBox = new QSpinBox(this);
  m_dsXSpinBox->setRange(1, 10);
  m_dsXSpinBox->setValue(2);
  m_dsYSpinBox = new QSpinBox(this);
  m_dsYSpinBox->setRange(1, 10);
  m_dsYSpinBox->setValue(2);
  m_dsZSpinBox = new QSpinBox(this);
  m_dsZSpinBox->setRange(1, 10);
  m_dsZSpinBox->setValue(1);
  pl = new QLabel(tr("x:"), this);
  pl->setAlignment(Qt::Alignment(Qt::AlignVCenter | Qt::AlignRight));
  layout->addWidget(pl, row, 1);
  layout->addWidget(m_dsXSpinBox, row, 2);
  pl = new QLabel(tr("y:"), this);
  pl->setAlignment(Qt::Alignment(Qt::AlignVCenter | Qt::AlignRight));
  layout->addWidget(pl, row, 3);
  layout->addWidget(m_dsYSpinBox, row, 4);
  pl = new QLabel(tr("z:"), this);
  pl->setAlignment(Qt::Alignment(Qt::AlignVCenter | Qt::AlignRight));
  layout->addWidget(pl, row, 5);
  layout->addWidget(m_dsZSpinBox, row, 6);
  m_dsXSpinBox->setEnabled(false);
  m_dsYSpinBox->setEnabled(false);
  m_dsZSpinBox->setEnabled(false);
  row++;

  pl = new QLabel(tr("interval: "), this);
  pl->setToolTip(tr(
    "Use the interval to downsample stack while stitching (more memory efficient). Note that this interval will be "
    "applied to original stack, not downsampled one."));
  layout->addWidget(pl, row, 0);
  m_intvXSpinBox = new QSpinBox(this);
  m_intvXSpinBox->setRange(0, 10);
  m_intvXSpinBox->setValue(1);
  m_intvYSpinBox = new QSpinBox(this);
  m_intvYSpinBox->setRange(0, 10);
  m_intvYSpinBox->setValue(1);
  m_intvZSpinBox = new QSpinBox(this);
  m_intvZSpinBox->setRange(0, 10);
  m_intvZSpinBox->setValue(1);
  pl = new QLabel(tr("x:"), this);
  pl->setAlignment(Qt::Alignment(Qt::AlignVCenter | Qt::AlignRight));
  layout->addWidget(pl, row, 1);
  layout->addWidget(m_intvXSpinBox, row, 2);
  pl = new QLabel(tr("y:"), this);
  pl->setAlignment(Qt::Alignment(Qt::AlignVCenter | Qt::AlignRight));
  layout->addWidget(pl, row, 3);
  layout->addWidget(m_intvYSpinBox, row, 4);
  pl = new QLabel(tr("z:"), this);
  pl->setAlignment(Qt::Alignment(Qt::AlignVCenter | Qt::AlignRight));
  layout->addWidget(pl, row, 5);
  layout->addWidget(m_intvZSpinBox, row, 6);
  row++;

  alllayout->addLayout(layout);

  hasTwoInputStackSetCheckBoxChanged(Qt::Unchecked);
  return alllayout;
}

QLayout* ZStitchImageDialog::createConnLayout()
{
  auto layout = new QVBoxLayout;
  auto hlayout = new QHBoxLayout;

  hlayout->addWidget(new QLabel("Max Overlap Rate: "));
  m_overlapRateSpinBox = new QSpinBox();
  m_overlapRateSpinBox->setMinimum(1);
  m_overlapRateSpinBox->setMaximum(100);
  m_overlapRateSpinBox->setSuffix("%");
  m_overlapRateSpinBox->setSingleStep(1);
  m_overlapRateSpinBox->setValue(15);
  hlayout->addWidget(m_overlapRateSpinBox);
  layout->addLayout(hlayout);

  hlayout = new QHBoxLayout;
  m_useTileImageRadioButton = new QRadioButton(tr("from tile image"), this);
  connect(m_useTileImageRadioButton, &QRadioButton::clicked, this, &ZStitchImageDialog::setConnInfoSource);
  m_openTileImageButton = new QPushButton(tr("open tile image to get connection info..."), this);
  connect(m_openTileImageButton, &QPushButton::clicked, this, &ZStitchImageDialog::getConnFromTileImage);
  m_useConnFileRadioButton = new QRadioButton(tr("from conn txt file"), this);
  connect(m_useConnFileRadioButton, &QRadioButton::clicked, this, &ZStitchImageDialog::setConnInfoSource);
  m_useConfigRadioButton = new QRadioButton(tr("manual (for two image)"), this);
  connect(m_useConfigRadioButton, &QRadioButton::clicked, this, &ZStitchImageDialog::setConnInfoSource);
  m_useLayoutRadioButton = new QRadioButton(tr("Layout"), this);
  connect(m_useLayoutRadioButton, &QRadioButton::clicked, this, &ZStitchImageDialog::setConnInfoSource);
  m_useFullConnectionRadioButton = new QRadioButton(tr("No (blind stitching)"), this);
  connect(m_useFullConnectionRadioButton, &QRadioButton::clicked, this, &ZStitchImageDialog::setConnInfoSource);
  m_restitchCZIRadioButton = new QRadioButton(tr("Restitch Zeiss CZI file"), this);
  connect(m_restitchCZIRadioButton, &QRadioButton::clicked, this, &ZStitchImageDialog::setConnInfoSource);
  m_editTileImageButton = new QPushButton(tr("edit selection..."), this);
  connect(m_editTileImageButton, &QPushButton::clicked, this, &ZStitchImageDialog::editConnFromTileImage);
  hlayout->addWidget(m_useTileImageRadioButton);
  hlayout->addWidget(m_openTileImageButton);
  hlayout->addWidget(m_editTileImageButton);
  layout->addLayout(hlayout);
  m_connEdit = new QTextEdit(this);
  m_connEdit->setLineWrapMode(QTextEdit::NoWrap);
  layout->addWidget(m_connEdit);

  hlayout = new QHBoxLayout;
  m_connFileEdit = new QLineEdit(this);
  m_connFileEdit->setReadOnly(true);
  m_selectConnFileButton = new QToolButton(this);
  m_selectConnFileButton->setText(tr("..."));
  connect(m_selectConnFileButton, &QToolButton::clicked, this, &ZStitchImageDialog::selectConnFile);
  hlayout->addWidget(m_useConnFileRadioButton);
  hlayout->addWidget(m_connFileEdit);
  hlayout->addWidget(m_selectConnFileButton);
  layout->addLayout(hlayout);

  hlayout = new QHBoxLayout;
  m_configDim1ComboBox = new QComboBox(this);
  m_configDim1ComboBox->addItem(tr("left"));
  m_configDim1ComboBox->addItem(tr("middle"));
  m_configDim1ComboBox->addItem(tr("right"));
  m_configDim2ComboBox = new QComboBox(this);
  m_configDim2ComboBox->addItem(tr("up"));
  m_configDim2ComboBox->addItem(tr("middle"));
  m_configDim2ComboBox->addItem(tr("down"));
  m_configDim3ComboBox = new QComboBox(this);
  m_configDim3ComboBox->addItem(tr("front"));
  m_configDim3ComboBox->addItem(tr("middle"));
  m_configDim3ComboBox->addItem(tr("back"));
  hlayout->addWidget(m_useConfigRadioButton);
  hlayout->addWidget(m_configDim1ComboBox);
  hlayout->addWidget(m_configDim2ComboBox);
  hlayout->addWidget(m_configDim3ComboBox);
  layout->addLayout(hlayout);

  hlayout = new QHBoxLayout;
  hlayout->addWidget(m_useFullConnectionRadioButton);
  layout->addLayout(hlayout);

  hlayout = new QHBoxLayout;
  hlayout->addWidget(m_useLayoutRadioButton);
  m_layout1SpinBox = new QSpinBox(this);
  m_layout1SpinBox->setRange(0, 99);
  m_layout2SpinBox = new QSpinBox(this);
  m_layout2SpinBox->setRange(0, 99);
  QLabel* pl = new QLabel(tr("rows:"), this);
  pl->setAlignment(Qt::Alignment(Qt::AlignVCenter | Qt::AlignRight));
  hlayout->addWidget(pl);
  hlayout->addWidget(m_layout1SpinBox);
  pl = new QLabel(tr("cols:"), this);
  pl->setAlignment(Qt::Alignment(Qt::AlignVCenter | Qt::AlignRight));
  hlayout->addWidget(pl);
  hlayout->addWidget(m_layout2SpinBox);
  m_layout1SpinBox->setEnabled(false);
  m_layout2SpinBox->setEnabled(false);
  layout->addLayout(hlayout);

  hlayout = new QHBoxLayout;
  hlayout->addWidget(m_restitchCZIRadioButton);
  layout->addLayout(hlayout);

  m_useTileImageRadioButton->click();

  return layout;
}

QLayout* ZStitchImageDialog::createCommandOutputLayout()
{
  auto layout = new QVBoxLayout;

  m_commandOutputEdit = new ZLogWidget(false, this);
  layout->addWidget(m_commandOutputEdit);
  return layout;
}

void ZStitchImageDialog::createIOGroupBox()
{
  m_ioGroupBox = new QGroupBox(tr("Inputs and Outputs"), this);
  QLayout* alllayout = createIOLayout();
  m_ioGroupBox->setLayout(alllayout);
}

void ZStitchImageDialog::createConnGroupBox()
{
  m_connGroupBox = new QGroupBox(tr("Connection info"), this);
  QLayout* layout = createConnLayout();
  m_connGroupBox->setLayout(layout);
}

void ZStitchImageDialog::createCommandOutputGroupBox()
{
  m_commandOutputGroupBox = new QGroupBox(tr("Stitch process output"), this);
  QLayout* layout = createCommandOutputLayout();
  m_commandOutputGroupBox->setLayout(layout);
}

QWidget* ZStitchImageDialog::createIOWidget()
{
  auto res = new QWidget(this);
  QLayout* alllayout = createIOLayout();
  res->setLayout(alllayout);
  return res;
}

QWidget* ZStitchImageDialog::createConnWidget()
{
  auto res = new QWidget(this);
  QLayout* alllayout = createConnLayout();
  res->setLayout(alllayout);
  return res;
}

QWidget* ZStitchImageDialog::createCommandOutputWidget()
{
  auto res = new QWidget(this);
  QLayout* alllayout = createCommandOutputLayout();
  res->setLayout(alllayout);
  return res;
}

void ZStitchImageDialog::selectInputStacks1()
{
  QStringList tmp;
  tmp = QFileDialog::getOpenFileNames(
    this, tr("select all input stacks"),
    ZSystemInfo::instance().lastOpenedImagePath(),
    tr("Image Files (*.lsm *.czi *.tif *.v3draw)"));
  if (tmp.count()) {
    ZSystemInfo::instance().setLastOpenedImagePath(tmp[0]);
    try {
      // test image
      auto infos = ZImg::readImgInfos(tmp[0]);

      initScene1ComboBox(infos.size());
      int nchannel = infos[0].numChannels;
      m_commonChannel1SpinBox->setRange(1, nchannel);
      initChannel1ComboBox(nchannel);
      initBgsub1ComboBox(nchannel);
      m_inputStack1Filenames.clear();
      m_inputStack1Filenames = tmp;
      std::sort(m_inputStack1Filenames.begin(), m_inputStack1Filenames.end(), naturalSortLessThan);
      m_inputStack1FileEdit->setText(QString("%1").arg(m_inputStack1Filenames.join("\n")));
    } catch (const ZException& e) {
      QMessageBox::critical(this, "Stitching Error", QString("Can not read image:\n%1").arg(e.what()));
      return;
    }
  }
  if (m_inputStack1Filenames.size() != 2) {
    if (m_useConfigRadioButton->isChecked()) {
      m_useTileImageRadioButton->click();
    }
  }
  m_useConfigRadioButton->setEnabled(m_inputStack1Filenames.size() == 2);
  if (m_inputStack1Filenames.size() != 1) {
    if (m_restitchCZIRadioButton->isChecked()) {
      m_useTileImageRadioButton->click();
    }
  }
  m_restitchCZIRadioButton->setEnabled(m_inputStack1Filenames.size() == 1);

}

void ZStitchImageDialog::selectInputStacks2()
{
  QStringList tmp;
  tmp = QFileDialog::getOpenFileNames(
    this, tr("select all input stacks"),
    ZSystemInfo::instance().lastOpenedImagePath(),
    tr("Image Files (*.lsm *.czi *.tif *.v3draw)"));
  if (tmp.count()) {
    ZSystemInfo::instance().setLastOpenedImagePath(tmp[0]);
    try {
      // test image
      auto infos = ZImg::readImgInfos(tmp[0]);

      initScene2ComboBox(infos.size());
      int nchannel = infos[0].numChannels;
      m_commonChannel2SpinBox->setRange(1, nchannel);
      initChannel2ComboBox(nchannel);
      initBgsub2ComboBox(nchannel);
      m_inputStack2Filenames.clear();
      m_inputStack2Filenames = tmp;
      std::sort(m_inputStack2Filenames.begin(), m_inputStack2Filenames.end(), naturalSortLessThan);
      m_inputStack2FileEdit->setText(QString("%1").arg(m_inputStack2Filenames.join("\n")));
    } catch (const ZException& e) {
      QMessageBox::critical(this, "Stitching Error", QString("Can not read image:\n%1").arg(e.what()));
      return;
    }
  }
  if (m_inputStack2Filenames.size() != 2) {
    if (m_useConfigRadioButton->isChecked()) {
      m_useTileImageRadioButton->click();
    }
  }
  m_useConfigRadioButton->setEnabled(m_inputStack1Filenames.size() == 2);
  if (m_inputStack2Filenames.size() != 1) {
    if (m_restitchCZIRadioButton->isChecked()) {
      m_useTileImageRadioButton->click();
    }
  }
  m_restitchCZIRadioButton->setEnabled(m_inputStack1Filenames.size() == 1);
}

bool ZStitchImageDialog::getTileMatrix(ZImg& img, std::vector<std::vector<int>>& tileMatrix, QList<ZTile>& tileList)
{
  double minvalue;
  double maxvalue;
  img.createView(0, 0).computeMinMax(minvalue, maxvalue);
  double midvalue = (minvalue + maxvalue) / 2;
  double thre1 = (minvalue + midvalue) / 2;
  double thre2 = (midvalue + maxvalue) / 2;
  size_t numTilePerRow = 0;
  size_t numTilePerCol = 0;
  tileMatrix.clear();
  tileList.clear();
  for (size_t h = 0; h < img.height(); h++) {
    double pre = minvalue;
    for (size_t w = 0; w < img.width(); w++) {
      if (img.value<double>(w, h, 0) > thre1 && img.value<double>(w, h, 0) > pre) {
        numTilePerRow++;
      }
      pre = img.value<double>(w, h, 0);
    }
    if (numTilePerRow > 0)
      break;
  }
  for (size_t w = 0; w < img.width(); w++) {
    double pre = minvalue;
    for (size_t h = 0; h < img.height(); h++) {
      if (img.value<double>(w, h, 0) > thre1 && img.value<double>(w, h, 0) > pre) {
        numTilePerCol++;
      }
      pre = img.value<double>(w, h, 0);
    }
    if (numTilePerCol > 0)
      break;
  }
  if (numTilePerRow == 0 || numTilePerCol == 0) {
    return false;
  }
  tileMatrix = std::vector<std::vector<int>>(numTilePerCol, std::vector<int>(numTilePerRow, 0));
  int tileindex = 1;
  int tileindex2 = 1;
  size_t currentrow = 0;
  size_t currentcol = 0;
  for (size_t h = 1; h < img.height() - 1; h++) {
    for (size_t w = 1; w < img.width() - 1; w++) {
      int value = img.value<int>(w, h, 0);
      int pre = img.value<int>(w - 1, h, 0);
      int up = img.value<int>(w, h - 1, 0);
      int post = img.value<int>(w + 1, h, 0);
      int down = img.value<int>(w, h + 1, 0);
      if (value > thre1 && value > pre && value > up) {
        if (value > thre2) {
          if (currentrow + 1 > tileMatrix.size() || currentcol + 1 > tileMatrix[currentrow].size()) {
            return false;
          }
          tileMatrix[currentrow][currentcol] = tileindex++;
          QPoint qp(w, h);
          ZTile tile(tileindex - 1, qp, qp);
          tileList.push_back(tile);
        }
        currentcol++;
        if (currentcol >= numTilePerRow) {
          currentcol = 0;
          currentrow++;
        }
      }
      if (value > thre2 && value > post && value > down) {
        if (tileindex2 > tileList.size()) {
          return false;
        }
        tileList[tileindex2 - 1].region.setBottomRight(QPoint(w, h));
        tileindex2++;
      }
    }
  }
  return tileindex == tileindex2;
}

void ZStitchImageDialog::editConnFromTileImage()
{
  if (!m_tileImage.isNull()) {
    QList<ZTile> tmpList(m_tileList);
    QDialog dia;
    m_scrollArea = new QScrollArea(this);
    m_tileImageWidget = new ZTileImageWidget(this, &m_tileImage, m_tileMatrix, &tmpList, m_inputStack1Filenames);
    m_scrollArea->setWidget(m_tileImageWidget);
    m_scrollArea->ensureWidgetVisible(m_tileImageWidget);
    auto vlayout = new QVBoxLayout;
    auto hlayout = new QHBoxLayout;

    auto m_zoomInAction = new QAction(ZTheme::instance().icon(ZTheme::ZoomInIcon), tr("Zoom &In"), &dia);
    QList<QKeySequence> zoomInKey;
    zoomInKey << QKeySequence::ZoomIn << QKeySequence(Qt::Key_Plus) << QKeySequence(Qt::Key_Equal);
    m_zoomInAction->setShortcuts(zoomInKey);
    m_zoomInAction->setStatusTip(tr("Zoom In"));
    connect(m_zoomInAction, &QAction::triggered, this, &ZStitchImageDialog::zoomInTileImageWidget);

    auto m_zoomOutAction = new QAction(ZTheme::instance().icon(ZTheme::ZoomOutIcon), tr("Zoom &Out"), &dia);
    QList<QKeySequence> zoomOutKey;
    zoomOutKey << QKeySequence::ZoomOut << QKeySequence(Qt::Key_Minus);
    m_zoomOutAction->setShortcuts(zoomOutKey);
    m_zoomOutAction->setStatusTip(tr("Zoom Out"));
    connect(m_zoomOutAction, &QAction::triggered, this, &ZStitchImageDialog::zoomOutTileImageWidget);

    auto zoomInButton = new QToolButton(&dia);
    zoomInButton->setDefaultAction(m_zoomInAction);
    hlayout->addWidget(zoomInButton);

    auto zoomOutButton = new QToolButton(&dia);
    zoomOutButton->setDefaultAction(m_zoomOutAction);
    hlayout->addWidget(zoomOutButton);

    QPushButton* clearAllButton = new QPushButton(tr("Clear All Selected"), this);
    connect(clearAllButton, &QPushButton::clicked, this, &ZStitchImageDialog::clearAllSelectedInTileImageWidget);
    hlayout->addWidget(clearAllButton);
    QPushButton* selectAllButton = new QPushButton(tr("Select All"), this);
    connect(selectAllButton, &QPushButton::clicked, this, &ZStitchImageDialog::selectAllInTileImageWidget);
    hlayout->addWidget(selectAllButton);
    QPushButton* saveButton = new QPushButton(tr("Save"), this);
    connect(saveButton, &QPushButton::clicked, this, &ZStitchImageDialog::saveTileImageWidgetAsImage);
    hlayout->addWidget(saveButton);

    vlayout->addLayout(hlayout);
    vlayout->addWidget(m_scrollArea);

    auto buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, this);

    connect(buttonBox, &QDialogButtonBox::accepted, &dia, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, &dia, &QDialog::reject);
    vlayout->addWidget(buttonBox);

    dia.setLayout(vlayout);
    dia.resize(1024, 1024);
    if (dia.exec() == QDialog::Accepted) {
      m_tileList = tmpList;
      m_nSel = 0;
      for (const auto& tile : m_tileList) {
        if (tile.bIsSelected)
          m_nSel++;
      }
      QString str = QString("%1 images selected to stitch:").arg(m_nSel);
      m_connEdit->setText(str);
      for (const auto& row : m_tileMatrix) {
        str = QString("  ");
        for (auto tileidx : row) {
          if (tileidx > 0 && m_tileList[tileidx - 1].bIsSelected) {
            str += QString("%1\t").arg(tileidx);
          } else {
            str += QString("%1\t").arg(0);
          }
        }
        m_connEdit->append(str);
      }
    }
  }
}

void ZStitchImageDialog::getConnFromTileImage()
{
  QString tmpName = QFileDialog::getOpenFileName(this,
                                                 tr("tile selection image file"),
                                                 ZSystemInfo::instance().lastOpenedImagePath(),
                                                 tr("Tile Select Image (*.lsm *.czi *.tif)"));
  if (!tmpName.isEmpty()) {
    ZSystemInfo::instance().setLastOpenedImagePath(tmpName);
    m_tileSelectionImageFilename = tmpName;
    m_tileList.clear();
    try {
      ZImg img(tmpName);
      if (img.width() > ZImgDisplay::toQImageSizeLimit() || img.height() > ZImgDisplay::toQImageSizeLimit()) {
        img.resize(std::min(ZImgDisplay::toQImageSizeLimit(), img.width()),
                   std::min(ZImgDisplay::toQImageSizeLimit(), img.height()), 1, Interpolant::Nearest);
      }

      ZImgDisplay display(img);
      double min;
      double max;
      img.createView(0, 0).computeMinMax(min, max);
      display.showChannel(0, min, max);
      m_tileImage = display.toQImage();

      if (getTileMatrix(img, m_tileMatrix, m_tileList)) {
        editConnFromTileImage();
        m_editTileImageButton->setEnabled(true);
      } else {
        m_tileList.clear();
        QMessageBox::warning(this, "Stitching Error", tr("Failed to parse tile connection image."));
      }
    } catch (const ZException& e) {
      QMessageBox::warning(this, "Stitching Error", QString("Can not read image:\n%1").arg(e.what()));
      m_tileSelectionImageFilename.clear();
    }
  }
}

void ZStitchImageDialog::clearAllSelectedInTileImageWidget()
{
  m_tileImageWidget->clearAllSelected();
}

void ZStitchImageDialog::selectAllInTileImageWidget()
{
  m_tileImageWidget->selectAll();
}

void ZStitchImageDialog::saveTileImageWidgetAsImage()
{
  QString fn = ZFileUtils::getSaveFileName(this, "save tiles image");
  if (!fn.isEmpty())
    m_tileImageWidget->saveAsImage(fn);
}

//void ZStitchImageDialog::outputCh1ImageComboBoxIndexChanged(int index)
//{
//  if (index == 0)
//    m_outputCh1ImageChannelSpinBox->setEnabled(false);
//  else
//    m_outputCh1ImageChannelSpinBox->setEnabled(true);
//}

//void ZStitchImageDialog::outputCh2ImageComboBoxIndexChanged(int index)
//{
//  if (index == 0)
//    m_outputCh2ImageChannelSpinBox->setEnabled(false);
//  else
//    m_outputCh2ImageChannelSpinBox->setEnabled(true);
//}

//void ZStitchImageDialog::outputCh3ImageComboBoxIndexChanged(int index)
//{
//  if (index == 0)
//    m_outputCh3ImageChannelSpinBox->setEnabled(false);
//  else
//    m_outputCh3ImageChannelSpinBox->setEnabled(true);
//}

void ZStitchImageDialog::initScene1ComboBox(int nscene)
{
  m_scene1ComboBox->setCurrentIndex(0);    //default value
  while (m_scene1ComboBox->count() > nscene) {
    m_scene1ComboBox->removeItem(m_scene1ComboBox->count() - 1);
  }
  for (int i = m_scene1ComboBox->count(); i < nscene; ++i) {
    m_scene1ComboBox->addItem(QString("scene %1").arg(i + 1));
  }
}

void ZStitchImageDialog::initChannel1ComboBox(int nchannel)
{
  m_channel1ComboBox->setCurrentIndex(0);    //default value
  while (m_channel1ComboBox->count() > 2) {
    m_channel1ComboBox->removeItem(2);
  }
  for (int i = 0; i < nchannel; ++i) {
    m_channel1ComboBox->addItem(QString("Ch%1").arg(i + 1));
  }
}

void ZStitchImageDialog::initBgsub1ComboBox(int nchannel)
{
  m_bgsub1ComboBox->setCurrentIndex(0);  //default value
  while (m_bgsub1ComboBox->count() > 2) {
    m_bgsub1ComboBox->removeItem(2);
  }
  for (int i = 0; i < nchannel; ++i) {
    m_bgsub1ComboBox->addItem(QString("Ch%1").arg(i + 1));
  }
}

void ZStitchImageDialog::initScene2ComboBox(int nscene)
{
  m_scene2ComboBox->setCurrentIndex(0);    //default value
  while (m_scene2ComboBox->count() > nscene) {
    m_scene2ComboBox->removeItem(m_scene2ComboBox->count() - 1);
  }
  for (int i = m_scene2ComboBox->count(); i < nscene; ++i) {
    m_scene2ComboBox->addItem(QString("scene %1").arg(i + 1));
  }
}

void ZStitchImageDialog::initChannel2ComboBox(int nchannel)
{
  m_channel2ComboBox->setCurrentIndex(0);    //default value
  while (m_channel2ComboBox->count() > 2) {
    m_channel2ComboBox->removeItem(2);
  }
  for (int i = 0; i < nchannel; ++i) {
    m_channel2ComboBox->addItem(QString("Ch%1").arg(i + 1));
  }
}

void ZStitchImageDialog::initBgsub2ComboBox(int nchannel)
{
  m_bgsub2ComboBox->setCurrentIndex(0);  //default value
  while (m_bgsub2ComboBox->count() > 2) {
    m_bgsub2ComboBox->removeItem(2);
  }
  for (int i = 0; i < nchannel; ++i) {
    m_bgsub2ComboBox->addItem(QString("Ch%1").arg(i + 1));
  }
}

void ZStitchImageDialog::zoomInTileImageWidget()
{
  m_tileImageWidget->zoomIn();
}

void ZStitchImageDialog::zoomOutTileImageWidget()
{
  m_tileImageWidget->zoomOut();
}

void ZStitchImageDialog::selectConnFile()
{
  QString connFileName = QFileDialog::getOpenFileName(this,
                                                      tr("specify conn txt file"),
                                                      ZSystemInfo::instance().lastOpenedImagePath(),
                                                      tr("Conn File (*.txt)"));
  if (!connFileName.isEmpty()) {
    ZSystemInfo::instance().setLastOpenedImagePath(connFileName);
    m_connFileEdit->setText(connFileName);
  }
}

void ZStitchImageDialog::selectOutputFile()
{
  QString outputFileName = ZFileUtils::getSaveFileName(this,
                                                       tr("specify output file"),
                                                       ZSystemInfo::instance().lastOpenedImagePath(),
                                                       tr("Output Image (*.nim *.tif *.v3draw)"));
  if (!outputFileName.isEmpty()) {
    ZSystemInfo::instance().setLastOpenedImagePath(outputFileName);
    m_outputFileEdit->setText(outputFileName);
  }
}

void ZStitchImageDialog::dsCheckBoxChanged(int state)
{
  m_dsXSpinBox->setEnabled(state == Qt::Checked);
  m_dsYSpinBox->setEnabled(state == Qt::Checked);
  m_dsZSpinBox->setEnabled(state == Qt::Checked);
}

void ZStitchImageDialog::hasTwoInputStackSetCheckBoxChanged(int state)
{
  m_inputStack2FileEdit->setVisible(state == Qt::Checked);
  m_selectInputStacks2Button->setVisible(state == Qt::Checked);
  m_scene2ComboBox->setVisible(state == Qt::Checked);
  m_channel2ComboBox->setVisible(state == Qt::Checked);
  m_bgsub2ComboBox->setVisible(state == Qt::Checked);

  m_commonChannel1SpinBox->setVisible(state == Qt::Checked);
  m_commonChannel2SpinBox->setVisible(state == Qt::Checked);

  for (auto label : m_labelsForTwoInputs) {
    label->setVisible(state == Qt::Checked);
  }
}

void ZStitchImageDialog::setConnInfoSource()
{
  m_openTileImageButton->setEnabled(m_useTileImageRadioButton->isChecked());
  m_connEdit->setVisible(m_useTileImageRadioButton->isChecked());
  m_connEdit->setEnabled(m_useTileImageRadioButton->isChecked());
  m_editTileImageButton->setEnabled(m_useTileImageRadioButton->isChecked() && !m_tileImage.isNull());
  m_configDim1ComboBox->setEnabled(m_useConfigRadioButton->isChecked());
  m_configDim2ComboBox->setEnabled(m_useConfigRadioButton->isChecked());
  m_configDim3ComboBox->setEnabled(m_useConfigRadioButton->isChecked());
  m_connFileEdit->setEnabled(m_useConnFileRadioButton->isChecked());
  m_selectConnFileButton->setEnabled(m_useConnFileRadioButton->isChecked());
  m_layout1SpinBox->setEnabled(m_useLayoutRadioButton->isChecked());
  m_layout2SpinBox->setEnabled(m_useLayoutRadioButton->isChecked());
}

} // namespace nim

