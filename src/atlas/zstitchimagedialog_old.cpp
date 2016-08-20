#include "zstitchimagedialog.h"

#include <QtGui>

#if (QT_VERSION >= QT_VERSION_CHECK(5, 0, 0))

#include <QtWidgets>

#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zimage.h"
#include "zstack.hxx"
#include "tz_error.h"
#include "tz_farray.h"
#include "tz_darray.h"
#include "tz_iarray.h"
#include "tz_fimage_lib.h"
#include "tz_dimage_lib.h"
#include "tz_stack_lib.h"
#include "tz_stack_stat.h"
#include "tz_image_io.h"
#include "tz_utilities.h"
#include "tz_graph_utils.h"
#include "tz_string.h"
#include "tz_stack_neighborhood.h"

namespace {
int lastInteger(const QString& str)
{
  int size = str.size();
  int endNumPos = -1;
  int startNumPos = -1;
  int index = size - 1;
  while (index >= 0) {
    if (str[index] >= QChar('0') && str[index] <= QChar('9')) {
      if (endNumPos == -1) {
        endNumPos = index;
        startNumPos = index;
      } else {
        startNumPos = index;
      }
    } else {
      if (endNumPos >= 0) {
        break;
      }
    }
    --index;
  }

  if (startNumPos == -1)
    return 0;

  int res = str.mid(startNumPos, endNumPos - startNumPos + 1).toInt();
  if (startNumPos > 0 && str[startNumPos - 1] == QChar('-'))
    res = -res;
  return res;
}

bool numberLessThan(const QString& s1, const QString& s2)
{
  return lastInteger(s1) < lastInteger(s2);
}
}

ZTile::ZTile(int index, QPoint topleft, QPoint bottomright) : index(index)
{
  region = QRect(topleft, bottomright);
  bIsSelected = true;
}

ZTileImageWidget::ZTileImageWidget(QWidget* parent, QImage* image, QList<ZTile>* pTiles, const QStringList& filenames) :
  QWidget(parent), m_image(image), m_pTiles(pTiles)
{
  m_scaleFactor = 1.0;
  m_rubberBand = nullptr;
  m_filenames = filenames;
  for (int i = 0; i < m_filenames.size(); ++i)
    m_tileimages.push_back(QImage(m_filenames[i]));
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
  if (m_image == nullptr) {
    return minimumSizeHint();
  } else {
    return m_image->size();
  }
}

void ZTileImageWidget::init(QImage* image, QList<ZTile>* pTiles)
{
  m_image = image;
  m_pTiles = pTiles;
  resize(image->width(), image->height());
  update();
}

void ZTileImageWidget::paintEvent(QPaintEvent*)
{
  if (m_image != nullptr) {
    QPainter painter(this);

    QSize size = m_scaleFactor * m_image->size();
    painter.drawImage(QRectF(0, 0, size.width(), size.height()), *m_image,
                      QRectF(0, 0, m_image->size().width(), m_image->size().height()));

    if (m_tileimages.size() == m_pTiles->size()) {


      for (int i = 0; i < m_pTiles->size(); ++i) {
        QPoint tl = m_pTiles->at(i).region.topLeft() * m_scaleFactor;
        QPoint br = m_pTiles->at(i).region.bottomRight() * m_scaleFactor;
        painter.drawImage(QRectF(tl.x(), tl.y(), br.x() - tl.x() + m_scaleFactor, br.y() - tl.y() + m_scaleFactor),
                          m_tileimages[i],
                          QRectF(0, 0, m_tileimages[i].size().width(), m_tileimages[i].size().height()));

        if (m_pTiles->at(i).bIsSelected) {

          painter.setPen(QPen(QBrush(QColor(255, 255, 0, 255)), 4));
          painter.drawRect(
            QRectF(tl.x() - 4, tl.y() - 4, br.x() - tl.x() + m_scaleFactor + 4, br.y() - tl.y() + m_scaleFactor + 4));
          //painter.fillRect(rect, QColor(255, 255, 0, 128));
        }
        //QString str = QString("Image %1").arg(i+1);
        //painter.drawText(rect, str);
      }
    } else {
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
  for (int i = 0; i < m_pTiles->size(); ++i) {
    if (m_pTiles->at(i).region.intersects(selRegion)) {
      if ((*m_pTiles)[i].bIsSelected) {
        (*m_pTiles)[i].bIsSelected = false;
      } else {
        (*m_pTiles)[i].bIsSelected = true;
      }
    }
  }
  update();
}

void ZTileImageWidget::clearAllSelected()
{
  for (int i = 0; i < m_pTiles->size(); ++i) {
    (*m_pTiles)[i].bIsSelected = false;
  }
  update();
}

void ZTileImageWidget::selectAll()
{
  for (int i = 0; i < m_pTiles->size(); ++i) {
    (*m_pTiles)[i].bIsSelected = true;
  }
  update();
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
  if (m_scaleFactor < 5) {
    m_scaleFactor += 1;
    resize(m_scaleFactor * m_image->size());
  }
}

void ZTileImageWidget::zoomOut()
{
  if (m_scaleFactor > 1) {
    m_scaleFactor -= 1;
    resize(m_scaleFactor * m_image->size());
  }
}

ZStitchImageDialog::ZStitchImageDialog(QWidget* parent) :
  QDialog(parent)
{
  // modeless version
  //setModal(false);
  //connect(this, SIGNAL(finished(int)), this, SLOT(deleteLater()));

  m_zstack = nullptr;
  m_tileImage = nullptr;
  m_tileSelectionInfoImage = nullptr;
  m_nSel = -100;
  m_nchannelStack1 = 1;
  m_nchannelStack2 = 1;

  //createIOLayout();
  //createConnLayout();
  //createCommandOutputLayout();

  m_runButton = new QPushButton(tr("Stitch"), this);
  m_exitButton = new QPushButton(tr("Exit"), this);
  m_buttonBox = new QDialogButtonBox(Qt::Horizontal, this);
  m_buttonBox->addButton(m_exitButton, QDialogButtonBox::RejectRole);
  m_buttonBox->addButton(m_runButton, QDialogButtonBox::ActionRole);
  connect(m_exitButton, SIGNAL(clicked()), this, SLOT(reject()));
  connect(m_runButton, SIGNAL(clicked()), this, SLOT(stitchStacks()));

  m_tabWidget = new QTabWidget;
  m_tabWidget->addTab(createIOWidget(), "Inputs and Outputs");
  m_tabWidget->addTab(createConnWidget(), "Connection info");
  m_tabWidget->addTab(createCommandOutputWidget(), "Stitch process output");
  QVBoxLayout* mainLayout = new QVBoxLayout;
  //mainLayout->addWidget(m_ioGroupBox);
  //mainLayout->addWidget(m_connGroupBox);
  //mainLayout->addWidget(m_commandOutputGroupBox);
  mainLayout->addWidget(m_tabWidget);
  mainLayout->addWidget(m_buttonBox);
  setLayout(mainLayout);

  setWindowTitle(tr("Stitch Stacks"));
}

ZStitchImageDialog::~ZStitchImageDialog()
{
  cleanup();
}

void ZStitchImageDialog::cleanup()
{
  if (m_tileImage != nullptr) {
    delete m_tileImage;
    m_tileImage = nullptr;
  }
  if (m_zstack != nullptr) {
    delete m_zstack;
    m_zstack = nullptr;
  }
  m_tileList.clear();

}

QLayout* ZStitchImageDialog::createIOLayout()
{
  // everything
  QVBoxLayout* alllayout = new QVBoxLayout;
  QHBoxLayout* allinputlayout = new QHBoxLayout;
  //input1
  QVBoxLayout* input1vlayout = new QVBoxLayout;
  m_inputStack1FileEdit = new QTextEdit(this);
  m_inputStack1FileEdit->setReadOnly(true);
  m_selectInputStacks1Button = new QPushButton(tr("select input stacks 1:"), this);
  connect(m_selectInputStacks1Button, SIGNAL(clicked()), this, SLOT(selectInputStacks1()));
  input1vlayout->addWidget(m_selectInputStacks1Button);
  input1vlayout->addWidget(m_inputStack1FileEdit);
  QHBoxLayout* tmphlayout = new QHBoxLayout;
  QLabel* pl = new QLabel(tr("Use channel: "), this);
  pl->setToolTip(tr("channel used for stitch"));
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
  m_bgsub1ComboBox->addItem(tr("After read"));
  m_bgsub1ComboBox->addItem(tr("After downsample"));
  m_bgsub1ComboBox->setCurrentIndex(1);      //default remove background for all channels
  tmphlayout->addWidget(pl);
  tmphlayout->addWidget(m_bgsub1ComboBox);
  input1vlayout->addLayout(tmphlayout);
  tmphlayout = new QHBoxLayout;
  pl = new QLabel(tr("merge mode: "), this);
  pl->setToolTip(tr("merge mode"));
  m_mergeMode1ComboBox = new QComboBox(this);
  m_mergeMode1ComboBox->addItem(tr("Max"));
  m_mergeMode1ComboBox->addItem(tr("Min"));
  m_mergeMode1ComboBox->addItem(tr("Mean"));
  m_mergeMode1ComboBox->addItem(tr("First"));
  m_mergeMode1ComboBox->setCurrentIndex(0);   //default max
  tmphlayout->addWidget(pl);
  tmphlayout->addWidget(m_mergeMode1ComboBox);
  input1vlayout->addLayout(tmphlayout);
  //input 2
  QVBoxLayout* input2vlayout = new QVBoxLayout;
  m_inputStack2FileEdit = new QTextEdit(this);
  m_inputStack2FileEdit->setReadOnly(true);
  m_selectInputStacks2Button = new QPushButton(tr("select input stacks 2:"), this);
  connect(m_selectInputStacks2Button, SIGNAL(clicked()), this, SLOT(selectInputStacks2()));
  input2vlayout->addWidget(m_selectInputStacks2Button);
  input2vlayout->addWidget(m_inputStack2FileEdit);
  tmphlayout = new QHBoxLayout;
  pl = new QLabel(tr("Use channel: "), this);
  pl->setToolTip(tr("channel used for stitch"));
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
  m_bgsub2ComboBox->addItem(tr("After read"));
  m_bgsub2ComboBox->addItem(tr("After downsample"));
  m_bgsub2ComboBox->setCurrentIndex(1);      //default remove background for all channels
  tmphlayout->addWidget(pl);
  tmphlayout->addWidget(m_bgsub2ComboBox);
  input2vlayout->addLayout(tmphlayout);
  tmphlayout = new QHBoxLayout;
  pl = new QLabel(tr("merge mode: "), this);
  pl->setToolTip(tr("merge mode"));
  m_labelsForTwoInputs.push_back(pl);
  m_mergeMode2ComboBox = new QComboBox(this);
  m_mergeMode2ComboBox->addItem(tr("Max"));
  m_mergeMode2ComboBox->addItem(tr("Min"));
  m_mergeMode2ComboBox->addItem(tr("Mean"));
  m_mergeMode2ComboBox->addItem(tr("First"));
  m_mergeMode2ComboBox->setCurrentIndex(0);   //default max
  tmphlayout->addWidget(pl);
  tmphlayout->addWidget(m_mergeMode2ComboBox);
  input2vlayout->addLayout(tmphlayout);
  //
  allinputlayout->addLayout(input1vlayout);
  allinputlayout->addLayout(input2vlayout);
  alllayout->addLayout(allinputlayout);
  // parameters
  QGridLayout* layout = new QGridLayout;
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
  pl->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
  m_labelsForTwoInputs.push_back(pl);
  layout->addWidget(pl, row, 1);
  layout->addWidget(m_commonChannel1SpinBox, row, 2);
  pl = new QLabel(tr(" = "), this);
  pl->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
  m_labelsForTwoInputs.push_back(pl);
  layout->addWidget(pl, row, 3);
  pl = new QLabel(tr("Ch"), this);
  pl->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
  m_labelsForTwoInputs.push_back(pl);
  layout->addWidget(pl, row, 4);
  layout->addWidget(m_commonChannel2SpinBox, row, 5);
  row++;

  pl = new QLabel(tr("Output File:"), this);
  m_outputFileEdit = new QLineEdit(this);
  m_selectOutputButton = new QToolButton(this);
  m_selectOutputButton->setText(tr("..."));
  connect(m_selectOutputButton, SIGNAL(clicked()), this, SLOT(selectOutputFile()));
  layout->addWidget(pl, row, 0);
  layout->addWidget(m_outputFileEdit, row, 1, 1, 6);
  layout->addWidget(m_selectOutputButton, row, 7);
  row++;

  pl = new QLabel(tr("With Ch1 from "), this);
  m_labelsForTwoInputs.push_back(pl);
  layout->addWidget(pl, row, 1);
  pl = new QLabel(tr("Ch"), this);
  pl->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
  m_labelsForTwoInputs.push_back(pl);
  layout->addWidget(pl, row, 2);
  m_outputCh1ImageChannelSpinBox = new QSpinBox(this);
  m_outputCh1ImageChannelSpinBox->setRange(1, 10);
  m_outputCh1ImageChannelSpinBox->setValue(1);
  layout->addWidget(m_outputCh1ImageChannelSpinBox, row, 3);
  m_outputCh1ImageComboBox = new QComboBox(this);
  m_outputCh1ImageComboBox->addItem(tr("none"));
  m_outputCh1ImageComboBox->addItem(tr("Stack 1"));
  m_outputCh1ImageComboBox->addItem(tr("Stack 2"));
  m_outputCh1ImageComboBox->setCurrentIndex(1);
  connect(m_outputCh1ImageComboBox, SIGNAL(currentIndexChanged(int)), this,
          SLOT(outputCh1ImageComboBoxIndexChanged(int)));
  pl = new QLabel(tr("of"), this);
  pl->setAlignment(Qt::AlignVCenter | Qt::AlignCenter);
  m_labelsForTwoInputs.push_back(pl);
  layout->addWidget(pl, row, 4);
  layout->addWidget(m_outputCh1ImageComboBox, row, 5, 1, 2);
  row++;

  pl = new QLabel(tr("With Ch2 from "), this);
  m_labelsForTwoInputs.push_back(pl);
  layout->addWidget(pl, row, 1);
  pl = new QLabel(tr("Ch"), this);
  pl->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
  m_labelsForTwoInputs.push_back(pl);
  layout->addWidget(pl, row, 2);
  m_outputCh2ImageChannelSpinBox = new QSpinBox(this);
  m_outputCh2ImageChannelSpinBox->setRange(1, 10);
  m_outputCh2ImageChannelSpinBox->setValue(2);
  layout->addWidget(m_outputCh2ImageChannelSpinBox, row, 3);
  m_outputCh2ImageComboBox = new QComboBox(this);
  m_outputCh2ImageComboBox->addItem(tr("none"));
  m_outputCh2ImageComboBox->addItem(tr("Stack 1"));
  m_outputCh2ImageComboBox->addItem(tr("Stack 2"));
  m_outputCh2ImageComboBox->setCurrentIndex(1);
  connect(m_outputCh2ImageComboBox, SIGNAL(currentIndexChanged(int)), this,
          SLOT(outputCh2ImageComboBoxIndexChanged(int)));
  pl = new QLabel(tr("of"), this);
  pl->setAlignment(Qt::AlignVCenter | Qt::AlignCenter);
  m_labelsForTwoInputs.push_back(pl);
  layout->addWidget(pl, row, 4);
  layout->addWidget(m_outputCh2ImageComboBox, row, 5, 1, 2);
  row++;

  pl = new QLabel(tr("With Ch3 from "), this);
  m_labelsForTwoInputs.push_back(pl);
  layout->addWidget(pl, row, 1);
  pl = new QLabel(tr("Ch"), this);
  pl->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
  m_labelsForTwoInputs.push_back(pl);
  layout->addWidget(pl, row, 2);
  m_outputCh3ImageChannelSpinBox = new QSpinBox(this);
  m_outputCh3ImageChannelSpinBox->setRange(1, 10);
  m_outputCh3ImageChannelSpinBox->setValue(1);
  layout->addWidget(m_outputCh3ImageChannelSpinBox, row, 3);
  m_outputCh3ImageComboBox = new QComboBox(this);
  m_outputCh3ImageComboBox->addItem(tr("none"));
  m_outputCh3ImageComboBox->addItem(tr("Stack 1"));
  m_outputCh3ImageComboBox->addItem(tr("Stack 2"));
  m_outputCh3ImageComboBox->setCurrentIndex(2);
  connect(m_outputCh3ImageComboBox, SIGNAL(currentIndexChanged(int)), this,
          SLOT(outputCh3ImageComboBoxIndexChanged(int)));
  pl = new QLabel(tr("of"), this);
  pl->setAlignment(Qt::AlignVCenter | Qt::AlignCenter);
  m_labelsForTwoInputs.push_back(pl);
  layout->addWidget(pl, row, 4);
  layout->addWidget(m_outputCh3ImageComboBox, row, 5, 1, 2);
  row++;

  m_hasTwoInputStackSetCheckBox = new QCheckBox(tr("has two input stack sets"), this);
  m_hasTwoInputStackSetCheckBox->setToolTip(tr("stitch two stack sets with common channel"));
  connect(m_hasTwoInputStackSetCheckBox, SIGNAL(stateChanged(int)), this,
          SLOT(hasTwoInputStackSetCheckBoxChanged(int)));
  layout->addWidget(m_hasTwoInputStackSetCheckBox, row, 0);
  row++;

  m_concatOnlyCheckBox = new QCheckBox(tr("only concatenate image"), this);
  m_concatOnlyCheckBox->setToolTip(tr("do not compute actual offset, just concat image together, for overview"));
  layout->addWidget(m_concatOnlyCheckBox, row, 0);
  row++;

  m_dsCheckBox = new QCheckBox(tr("downsample"), this);
  m_dsCheckBox->setToolTip(tr("Downsample stack before stitching"));
  connect(m_dsCheckBox, SIGNAL(stateChanged(int)), this, SLOT(dsCheckBoxChanged(int)));
  layout->addWidget(m_dsCheckBox, row, 0);
  m_dsXSpinBox = new QSpinBox(this);
  m_dsXSpinBox->setRange(1, 10);
  m_dsXSpinBox->setValue(1);
  m_dsYSpinBox = new QSpinBox(this);
  m_dsYSpinBox->setRange(1, 10);
  m_dsYSpinBox->setValue(1);
  m_dsZSpinBox = new QSpinBox(this);
  m_dsZSpinBox->setRange(1, 10);
  m_dsZSpinBox->setValue(1);
  pl = new QLabel(tr("x:"), this);
  pl->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
  layout->addWidget(pl, row, 1);
  layout->addWidget(m_dsXSpinBox, row, 2);
  pl = new QLabel(tr("y:"), this);
  pl->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
  layout->addWidget(pl, row, 3);
  layout->addWidget(m_dsYSpinBox, row, 4);
  pl = new QLabel(tr("z:"), this);
  pl->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
  layout->addWidget(pl, row, 5);
  layout->addWidget(m_dsZSpinBox, row, 6);
  m_dsXSpinBox->setEnabled(false);
  m_dsYSpinBox->setEnabled(false);
  m_dsZSpinBox->setEnabled(false);
  row++;

  pl = new QLabel(tr("interval: "), this);
  pl->setToolTip(tr(
    "Use the interval to downsample stack while stitching (more memory efficient). Note that this interval will be applied to original stack, not downsampled one."));
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
  pl->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
  layout->addWidget(pl, row, 1);
  layout->addWidget(m_intvXSpinBox, row, 2);
  pl = new QLabel(tr("y:"), this);
  pl->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
  layout->addWidget(pl, row, 3);
  layout->addWidget(m_intvYSpinBox, row, 4);
  pl = new QLabel(tr("z:"), this);
  pl->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
  layout->addWidget(pl, row, 5);
  layout->addWidget(m_intvZSpinBox, row, 6);
  row++;

  m_d8CheckBox = new QCheckBox(tr("convert result to 8bit"), this);
  m_d8CheckBox->setToolTip(tr("d8"));
  connect(m_d8CheckBox, SIGNAL(stateChanged(int)), this, SLOT(d8CheckBoxChanged(int)));
  layout->addWidget(m_d8CheckBox, row, 0);
  m_d8ComboBox = new QComboBox(this);
  m_d8ComboBox->addItem(tr("[min, max] -> [0, 255]"));
  m_d8ComboBox->addItem(tr("[min, q(99.99)] -> [0, 255]"));
  m_d8ComboBox->addItem(tr("equal info map"));
  connect(m_d8ComboBox, SIGNAL(activated(int)), this, SLOT(d8Changed(int)));
  layout->addWidget(m_d8ComboBox, row, 2, 1, 4);
  m_d8ComboBox->setEnabled(false);
  row++;

  alllayout->addLayout(layout);

  hasTwoInputStackSetCheckBoxChanged(Qt::Unchecked);
  return alllayout;
}


//void ZStitchImageDialog::createIOGroupBox() {
//  m_ioGroupBox = new QGroupBox(tr("Inputs and Output"), this);
//  QGridLayout *layout = new QGridLayout(this);
//  int row = 0;

//  m_inputStackFileEdit = new QTextEdit(this);
//  m_inputStackFileEdit->setReadOnly(true);
//  m_selectInputStacksButton = new QPushButton(tr("select input stacks :"), this);
//  connect(m_selectInputStacksButton, SIGNAL(clicked()), this, SLOT(selectInputStacks()));
//  layout->addWidget(m_selectInputStacksButton, row, 0, 1, 2);
//  layout->addWidget(m_inputStackFileEdit, row, 2, 5, 5);
//  row++;

//  QLabel * pl = new QLabel(tr("Use channel: "), this);
//  pl->setToolTip(tr("channel used for stitch"));
//  m_channelComboBox = new QComboBox(this);
//  m_channelComboBox->addItem(tr("Average of all channels"));
//  m_channelComboBox->addItem(tr("Average of Ch1 and Ch2"));
//  layout->addWidget(pl, row, 0);
//  layout->addWidget(m_channelComboBox, row, 1);
//  m_channelComboBox->setCurrentIndex(0);     //default average all channels
//  row++;

//  pl = new QLabel(tr("Remove Background: "), this);
//  pl->setToolTip(tr("Remove Background (below most common intensity value)"));
//  m_bgsubComboBox = new QComboBox(this);
//  m_bgsubComboBox->addItem(tr("None"));
//  m_bgsubComboBox->addItem(tr("All channels"));
//  m_bgsubComboBox->addItem(tr("After read"));
//  m_bgsubComboBox->addItem(tr("After downsample"));
//  layout->addWidget(pl, row, 0);
//  layout->addWidget(m_bgsubComboBox, row, 1);
//  m_bgsubComboBox->setCurrentIndex(1);      //default remove background for all channels
//  row++;

//  pl = new QLabel(tr("merge mode: "), this);
//  pl->setToolTip(tr("merge mode"));
//  m_mergeModeComboBox = new QComboBox(this);
//  m_mergeModeComboBox->addItem(tr("Max"));
//  m_mergeModeComboBox->addItem(tr("Min"));
//  m_mergeModeComboBox->addItem(tr("Mean"));
//  m_mergeModeComboBox->addItem(tr("First"));
//  layout->addWidget(pl, row, 0);
//  layout->addWidget(m_mergeModeComboBox, row, 1);
//  m_mergeModeComboBox->setCurrentIndex(3);   //default first
//  row++;

//  m_concatOnlyCheckBox = new QCheckBox(tr("only concatenate image"), this);
//  m_concatOnlyCheckBox->setToolTip(tr("do not compute actual offset, just concat image together, for overview"));
//  layout->addWidget(m_concatOnlyCheckBox, row, 0);
//  row++;

//  m_d8CheckBox = new QCheckBox(tr("d8"), this);
//  m_d8CheckBox->setToolTip(tr("d8"));
//  connect(m_d8CheckBox, SIGNAL(stateChanged(int)), this, SLOT(d8CheckBoxChanged(int)));
//  layout->addWidget(m_d8CheckBox, row, 0);
//  m_d8ComboBox = new QComboBox(this);
//  m_d8ComboBox->addItem(tr("[min, max] -> [0, 255]"));
//  m_d8ComboBox->addItem(tr("[min, q(99.99)] -> [0, 255]"));
//  m_d8ComboBox->addItem(tr("equal info map"));
//  connect(m_d8ComboBox, SIGNAL(activated(int)), this, SLOT(d8Changed(int)));
//  layout->addWidget(m_d8ComboBox, row, 1, 1, 3);
//  m_d8ComboBox->setEnabled(false);
//  row++;

//  m_layoutCheckBox = new QCheckBox(tr("layout"), this);
//  m_layoutCheckBox->setToolTip(tr("layout."));
//  connect(m_layoutCheckBox, SIGNAL(stateChanged(int)), this, SLOT(layoutCheckBoxChanged(int)));
//  layout->addWidget(m_layoutCheckBox, row, 0);
//  m_layout1SpinBox = new QSpinBox(this);
//  m_layout1SpinBox->setRange(0,1E9);
//  m_layout2SpinBox = new QSpinBox(this);
//  m_layout2SpinBox->setRange(0,1E9);
//  layout->addWidget(m_layout1SpinBox, row, 2);
//  layout->addWidget(m_layout2SpinBox, row, 4);
//  m_layout1SpinBox->setEnabled(false);
//  m_layout2SpinBox->setEnabled(false);
//  row++;

//  m_dsCheckBox = new QCheckBox(tr("downsample"), this);
//  m_dsCheckBox->setToolTip(tr("Downsample stack."));
//  connect(m_dsCheckBox, SIGNAL(stateChanged(int)), this, SLOT(dsCheckBoxChanged(int)));
//  layout->addWidget(m_dsCheckBox, row, 0);
//  m_dsXSpinBox = new QSpinBox(this);
//  m_dsXSpinBox->setRange(0,10);
//  m_dsXSpinBox->setValue(1);
//  m_dsYSpinBox = new QSpinBox(this);
//  m_dsYSpinBox->setRange(0,10);
//  m_dsYSpinBox->setValue(1);
//  m_dsZSpinBox = new QSpinBox(this);
//  m_dsZSpinBox->setRange(0,10);
//  m_dsZSpinBox->setValue(1);
//  pl = new QLabel(tr("x:"), this);
//  pl->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
//  layout->addWidget(pl, row, 1);
//  layout->addWidget(m_dsXSpinBox, row, 2);
//  pl = new QLabel(tr("y:"), this);
//  pl->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
//  layout->addWidget(pl, row, 3);
//  layout->addWidget(m_dsYSpinBox, row, 4);
//  pl = new QLabel(tr("z:"), this);
//  pl->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
//  layout->addWidget(pl, row, 5);
//  layout->addWidget(m_dsZSpinBox, row, 6);
//  m_dsXSpinBox->setEnabled(false);
//  m_dsYSpinBox->setEnabled(false);
//  m_dsZSpinBox->setEnabled(false);
//  row++;

//  pl = new QLabel(tr("interval: "), this);
//  pl->setToolTip(tr("Use the interval to downsample stack while stitching, low accuracy but faster"));
//  layout->addWidget(pl, row, 0);
//  m_intvXSpinBox = new QSpinBox(this);
//  m_intvXSpinBox->setRange(0,10);
//  m_intvXSpinBox->setValue(1);
//  m_intvYSpinBox = new QSpinBox(this);
//  m_intvYSpinBox->setRange(0,10);
//  m_intvYSpinBox->setValue(1);
//  m_intvZSpinBox = new QSpinBox(this);
//  m_intvZSpinBox->setRange(0,10);
//  m_intvZSpinBox->setValue(1);
//  pl = new QLabel(tr("x:"), this);
//  pl->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
//  layout->addWidget(pl, row, 1);
//  layout->addWidget(m_intvXSpinBox, row, 2);
//  pl = new QLabel(tr("y:"), this);
//  pl->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
//  layout->addWidget(pl, row, 3);
//  layout->addWidget(m_intvYSpinBox, row, 4);
//  pl = new QLabel(tr("z:"), this);
//  pl->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
//  layout->addWidget(pl, row, 5);
//  layout->addWidget(m_intvZSpinBox, row, 6);
//  row++;

//  pl = new QLabel(tr("Output File:"), this);
//  m_outputFileEdit = new QLineEdit(this);
//  m_selectOutputButton = new QToolButton(this);
//  m_selectOutputButton->setText(tr("..."));
//  connect(m_selectOutputButton, SIGNAL(clicked()), this, SLOT(selectOutputFile()));
//  layout->addWidget(pl, row, 0);
//  layout->addWidget(m_outputFileEdit, row, 1, 1, 6);
//  layout->addWidget(m_selectOutputButton, row, 7);
//  row++;

//  m_ioGroupBox->setLayout(layout);
//}

QLayout* ZStitchImageDialog::createConnLayout()
{
  QVBoxLayout* layout = new QVBoxLayout;
  QHBoxLayout* hlayout = new QHBoxLayout;

  m_useTileImageRadioButton = new QRadioButton(tr("from tile image"), this);
  connect(m_useTileImageRadioButton, SIGNAL(clicked()), this, SLOT(setConnInfoSource()));
  m_openTileImageButton = new QPushButton(tr("open tile image to get connection info..."), this);
  connect(m_openTileImageButton, SIGNAL(clicked()), this, SLOT(getConnFromTileImage()));
  m_useConnFileRadioButton = new QRadioButton(tr("from conn txt file"), this);
  connect(m_useConnFileRadioButton, SIGNAL(clicked()), this, SLOT(setConnInfoSource()));
  m_useConfigRadioButton = new QRadioButton(tr("manual (for two image)"), this);
  connect(m_useConfigRadioButton, SIGNAL(clicked()), this, SLOT(setConnInfoSource()));
  m_useLayoutRadioButton = new QRadioButton(tr("Layout"), this);
  connect(m_useLayoutRadioButton, SIGNAL(clicked()), this, SLOT(setConnInfoSource()));
  m_useFullConnectionRadioButton = new QRadioButton(tr("No (blind stitching)"), this);
  connect(m_useFullConnectionRadioButton, SIGNAL(clicked()), this, SLOT(setConnInfoSource()));
  m_editTileImageButton = new QPushButton(tr("edit selection..."), this);
  connect(m_editTileImageButton, SIGNAL(clicked()), this, SLOT(editConnFromTileImage()));
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
  connect(m_selectConnFileButton, SIGNAL(clicked()), this, SLOT(selectConnFile()));
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
  pl->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
  hlayout->addWidget(pl);
  hlayout->addWidget(m_layout1SpinBox);
  pl = new QLabel(tr("cols:"), this);
  pl->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
  hlayout->addWidget(pl);
  hlayout->addWidget(m_layout2SpinBox);
  m_layout1SpinBox->setEnabled(false);
  m_layout2SpinBox->setEnabled(false);
  layout->addLayout(hlayout);

  m_useTileImageRadioButton->click();

  return layout;
}

QLayout* ZStitchImageDialog::createCommandOutputLayout()
{
  QVBoxLayout* layout = new QVBoxLayout;

  m_commandOutputEdit = new QTextEdit(this);
  layout->addWidget(m_commandOutputEdit);
  m_commandOutputEdit->setReadOnly(true);
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
  QWidget* res = new QWidget(this);
  QLayout* alllayout = createIOLayout();
  res->setLayout(alllayout);
  return res;
}

QWidget* ZStitchImageDialog::createConnWidget()
{
  QWidget* res = new QWidget(this);
  QLayout* alllayout = createConnLayout();
  res->setLayout(alllayout);
  return res;
}

QWidget* ZStitchImageDialog::createCommandOutputWidget()
{
  QWidget* res = new QWidget(this);
  QLayout* alllayout = createCommandOutputLayout();
  res->setLayout(alllayout);
  return res;
}

void ZStitchImageDialog::selectInputStacks1()
{
  QStringList tmp;
  tmp = QFileDialog::getOpenFileNames(
    this, tr("select all input stacks"),
    m_openFilesPath,
    tr("Image Files (*.lsm *.tif *.raw)"));
  if (tmp.count()) {
    m_openFilesPath = tmp[0];
    // test image
    int nchannel = ZStack::getChannelNumber(tmp[0].toStdString());
    if (nchannel == 0) {
      QMessageBox::warning(this, tr("Can not read image"), tr("Can not read image"));
      return;
    }
    m_nchannelStack1 = nchannel;
    setStack1ChRange();
    m_commonChannel1SpinBox->setRange(1, nchannel);
    initChannel1ComboBox(nchannel);
    initBgsub1ComboBox(nchannel);
    m_inputStack1Filenames.clear();
    m_inputStack1Filenames = tmp;
    std::sort(m_inputStack1Filenames.begin(), m_inputStack1Filenames.end(), numberLessThan);
    m_inputStack1FileEdit->setText(QString("%1").arg(m_inputStack1Filenames.join("\n")));
  }
  if (m_inputStack1Filenames.size() != 2) {
    if (m_useConfigRadioButton->isChecked()) {
      m_useTileImageRadioButton->click();
    }
    m_useConfigRadioButton->setEnabled(false);
  } else if (m_inputStack1Filenames.size() == 2) {
    m_useConfigRadioButton->setEnabled(true);
  }
}

void ZStitchImageDialog::selectInputStacks2()
{
  QStringList tmp;
  tmp = QFileDialog::getOpenFileNames(
    this, tr("select all input stacks"),
    m_openFilesPath,
    tr("Image Files (*.lsm *.tif *.raw)"));
  if (tmp.count()) {
    m_openFilesPath = tmp[0];
    // test image
    int nchannel = ZStack::getChannelNumber(tmp[0].toStdString());
    if (nchannel == 0) {
      QMessageBox::warning(this, tr("Can not read image"), tr("Can not read image"));
      return;
    }
    m_nchannelStack2 = nchannel;
    setStack2ChRange();
    m_commonChannel2SpinBox->setRange(1, nchannel);
    initChannel2ComboBox(nchannel);
    initBgsub2ComboBox(nchannel);
    m_inputStack2Filenames.clear();
    m_inputStack2Filenames = tmp;
    std::sort(m_inputStack2Filenames.begin(), m_inputStack2Filenames.end(), numberLessThan);
    m_inputStack2FileEdit->setText(QString("%1").arg(m_inputStack2Filenames.join("\n")));
  }
  if (m_inputStack2Filenames.size() != 2) {
    if (m_useConfigRadioButton->isChecked()) {
      m_useTileImageRadioButton->click();
    }
    m_useConfigRadioButton->setEnabled(false);
  } else if (m_inputStack1Filenames.size() == 2) {
    m_useConfigRadioButton->setEnabled(true);
  }
}

bool ZStitchImageDialog::getTileMatrix(ZStack* stack, QVector<QVector<int> >& tileMatrix,
                                       QList<ZTile>& tileList)
{
  int minvalue = stack->min();
  int maxvalue = stack->max();
  int midvalue = (minvalue + maxvalue) / 2;
  int thre1 = (minvalue + midvalue) / 2;
  int thre2 = (midvalue + maxvalue) / 2;
  int numTilePerRow = 0;
  int numTilePerCol = 0;
  tileMatrix.clear();
  tileList.clear();
  for (int h = 0; h < stack->height(); h++) {
    int pre = minvalue;
    for (int w = 0; w < stack->width(); w++) {
      if (stack->value(w, h, 0) > thre1 && stack->value(w, h, 0) > pre) {
        numTilePerRow++;
      }
      pre = stack->value(w, h, 0);
    }
    if (numTilePerRow > 0)
      break;
  }
  for (int w = 0; w < stack->width(); w++) {
    int pre = minvalue;
    for (int h = 0; h < stack->height(); h++) {
      if (stack->value(w, h, 0) > thre1 && stack->value(w, h, 0) > pre) {
        numTilePerCol++;
      }
      pre = stack->value(w, h, 0);
    }
    if (numTilePerCol > 0)
      break;
  }
  if (numTilePerRow == 0 || numTilePerCol == 0) {
    return false;
  }
  tileMatrix = QVector<QVector<int> >(numTilePerCol, QVector<int>(numTilePerRow, 0));
  int tileindex = 1;
  int tileindex2 = 1;
  int currentrow = 0;
  int currentcol = 0;
  for (int h = 1; h < stack->height() - 1; h++) {
    for (int w = 1; w < stack->width() - 1; w++) {
      int value = stack->value(w, h, 0);
      int pre = stack->value(w - 1, h, 0);
      int up = stack->value(w, h - 1, 0);
      int post = stack->value(w + 1, h, 0);
      int down = stack->value(w, h + 1, 0);
      if (value > thre1 && value > pre && value > up) {
        if (value > thre2) {
          if (currentrow + 1 > tileMatrix.size() || currentcol + 1 > tileMatrix[currentrow].size()) {
            return false;
          } else {
            tileMatrix[currentrow][currentcol] = tileindex++;
            QPoint qp(w, h);
            ZTile tile(tileindex - 1, qp, qp);
            tileList.push_back(tile);
          }
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
        } else {
          tileList[tileindex2 - 1].region.setBottomRight(QPoint(w, h));
          tileindex2++;
        }
      }
    }
  }
  if (tileindex != tileindex2) {
    return false;
  } else {
    return true;
  }
}

void ZStitchImageDialog::editConnFromTileImage()
{
  if (m_tileImage != nullptr) {
    QList<ZTile> tmpList(m_tileList);
    QDialog dia;
    m_scrollArea = new QScrollArea(this);
    m_tileImageWidget = new ZTileImageWidget(this, m_tileImage, &tmpList, m_inputStack1Filenames);
    m_scrollArea->setWidget(m_tileImageWidget);
    m_scrollArea->ensureWidgetVisible(m_tileImageWidget);
    QVBoxLayout* vlayout = new QVBoxLayout;
    QHBoxLayout* hlayout = new QHBoxLayout;
    QPushButton* zoomInButton = new QPushButton(tr("zoom in"), this);
    connect(zoomInButton, SIGNAL(clicked()), this, SLOT(zoomInTileImageWidget()));
    hlayout->addWidget(zoomInButton);
    QPushButton* zoomOutButton = new QPushButton(tr("zoom out"), this);
    connect(zoomOutButton, SIGNAL(clicked()), this, SLOT(zoomOutTileImageWidget()));
    hlayout->addWidget(zoomOutButton);
    QPushButton* clearAllButton = new QPushButton(tr("clear all selected"), this);
    connect(clearAllButton, SIGNAL(clicked()), this, SLOT(clearAllSelectedInTileImageWidget()));
    hlayout->addWidget(clearAllButton);
    QPushButton* selectAllButton = new QPushButton(tr("select all"), this);
    connect(selectAllButton, SIGNAL(clicked()), this, SLOT(selectAllInTileImageWidget()));
    hlayout->addWidget(selectAllButton);
    QPushButton* saveButton = new QPushButton(tr("save"), this);
    connect(saveButton, SIGNAL(clicked()), this, SLOT(saveTileImageWidgetAsImage()));
    hlayout->addWidget(saveButton);

    vlayout->addLayout(hlayout);
    vlayout->addWidget(m_scrollArea);

    QDialogButtonBox* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok
                                                       | QDialogButtonBox::Cancel, Qt::Horizontal, this);

    connect(buttonBox, SIGNAL(accepted()), &dia, SLOT(accept()));
    connect(buttonBox, SIGNAL(rejected()), &dia, SLOT(reject()));
    vlayout->addWidget(buttonBox);

    dia.setLayout(vlayout);
    dia.resize(m_tileImage->width(), m_tileImage->height());
    if (dia.exec() == QDialog::Accepted) {
      m_tileList = tmpList;
      m_nSel = 0;
      for (int i = 0; i < m_tileList.size(); ++i) {
        if (m_tileList[i].bIsSelected)
          m_nSel++;
      }
      QString str = QString("%1 images selected to stitch:").arg(m_nSel);
      m_connEdit->setText(str);
      for (int i = 0; i < m_tileMatrix.size(); ++i) {
        str = QString("  ");
        for (int j = 0; j < m_tileMatrix[0].size(); ++j) {
          if (m_tileMatrix[i][j] > 0 && m_tileList[m_tileMatrix[i][j] - 1].bIsSelected) {
            str += QString("%1\t").arg(m_tileMatrix[i][j]);
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
                                                 m_openFilesPath,
                                                 tr("Tile Select Image (*.lsm *.tif)"));
  if (!tmpName.isEmpty()) {
    m_openFilesPath = tmpName;
    m_tileSelectionImageFilename = tmpName;
    cleanup();
    QByteArray ba = tmpName.toLocal8Bit();
    m_zstack = new ZStack();
    m_zstack->load(ba.data());
    double scale, offset;
    m_zstack->bcAdjustHint(&scale, &offset);
    if (m_zstack->channelNumber() == 1) {
      switch (m_zstack->kind()) {
        case GREY:
          m_tileImage = new ZImage(m_zstack->width(), m_zstack->height());
          m_tileImage->setData(m_zstack->array8(), scale, offset, -1);
          break;
        case GREY16:
          m_tileImage = new ZImage(m_zstack->width(), m_zstack->height());
          m_tileImage->setData(m_zstack->array16(), scale, offset, -1);
          break;
      }
    } else {
      QMessageBox::warning(this, tr("picture type not supported"), tr("picture type not supported"));
      m_tileSelectionImageFilename.clear();
      delete m_zstack;
      m_zstack = nullptr;
      return;
    }

    if (getTileMatrix(m_zstack, m_tileMatrix, m_tileList)) {
      editConnFromTileImage();
      m_editTileImageButton->setEnabled(true);
    } else {
      cleanup();
      QMessageBox::warning(this, tr("Failed"), tr("Failed to read tile connection file."));
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
  QString fn = QFileDialog::getSaveFileName(this, "save tiles image");
  if (!fn.isEmpty())
    m_tileImageWidget->saveAsImage(fn);
}

void ZStitchImageDialog::outputCh1ImageComboBoxIndexChanged(int index)
{
  if (index == 0)
    m_outputCh1ImageChannelSpinBox->setEnabled(false);
  else
    m_outputCh1ImageChannelSpinBox->setEnabled(true);
}

void ZStitchImageDialog::outputCh2ImageComboBoxIndexChanged(int index)
{
  if (index == 0)
    m_outputCh2ImageChannelSpinBox->setEnabled(false);
  else
    m_outputCh2ImageChannelSpinBox->setEnabled(true);
}

void ZStitchImageDialog::outputCh3ImageComboBoxIndexChanged(int index)
{
  if (index == 0)
    m_outputCh3ImageChannelSpinBox->setEnabled(false);
  else
    m_outputCh3ImageChannelSpinBox->setEnabled(true);
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
  m_bgsub1ComboBox->setCurrentIndex(1);  //default value
  while (m_bgsub1ComboBox->count() > 4) {
    m_bgsub1ComboBox->removeItem(4);
  }
  for (int i = 0; i < nchannel; ++i) {
    m_bgsub1ComboBox->addItem(QString("Ch%1").arg(i + 1));
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
  m_bgsub2ComboBox->setCurrentIndex(1);  //default value
  while (m_bgsub2ComboBox->count() > 4) {
    m_bgsub2ComboBox->removeItem(4);
  }
  for (int i = 0; i < nchannel; ++i) {
    m_bgsub2ComboBox->addItem(QString("Ch%1").arg(i + 1));
  }
}

void ZStitchImageDialog::setStack1ChRange()
{
  if (m_outputCh1ImageComboBox->currentIndex() == 1) {
    m_outputCh1ImageChannelSpinBox->setRange(1, m_nchannelStack1);
  }
  if (m_outputCh2ImageComboBox->currentIndex() == 1) {
    m_outputCh2ImageChannelSpinBox->setRange(1, m_nchannelStack1);
  }
  if (m_outputCh3ImageComboBox->currentIndex() == 1) {
    m_outputCh3ImageChannelSpinBox->setRange(1, m_nchannelStack1);
  }
}

void ZStitchImageDialog::setStack2ChRange()
{
  if (m_outputCh1ImageComboBox->currentIndex() == 2) {
    m_outputCh1ImageChannelSpinBox->setRange(1, m_nchannelStack2);
  }
  if (m_outputCh2ImageComboBox->currentIndex() == 2) {
    m_outputCh2ImageChannelSpinBox->setRange(1, m_nchannelStack2);
  }
  if (m_outputCh3ImageComboBox->currentIndex() == 2) {
    m_outputCh3ImageChannelSpinBox->setRange(1, m_nchannelStack2);
  }
}

void ZStitchImageDialog::stitchStacks2()
{
  int noutputchannel = 3;
  Stack ch1, ch2, ch3;
  if (m_outputCh3ImageComboBox->currentIndex() == 0) {
    noutputchannel--;
    if (m_outputCh2ImageComboBox->currentIndex() == 0) {
      noutputchannel--;
      if (m_outputCh1ImageComboBox->currentIndex() == 0) {
        noutputchannel--;
      }
    }
  }
  if (noutputchannel < 2) {
    m_commandOutputEdit->append(QString("<font color=red>Do not need two inputs, Abort.</font>"));
  }

  if (m_inputStack2Filenames.size() != m_inputStack1Filenames.size()) {
    m_commandOutputEdit->append(QString("<font color=red>The number of input files of stack 2 is not equal to input 1 \
                                        Abort.</font>"));
    return;
  }
  if (m_inputStack2Filenames.size() < 1) {
    m_commandOutputEdit->append(QString("<font color=red>Please add input files.</font>"));
    return;
  }

  int nstack;
  QStringList inputStack1Filenames;
  QStringList inputStack2Filenames;
  char** filepath1 = nullptr;
  char** filepath2 = nullptr;
  QByteArray input1ba = m_inputStack1Filenames[0].toLocal8Bit();
  QByteArray input2ba = m_inputStack2Filenames[0].toLocal8Bit();

  if (m_inputStack1Filenames.size() == 1 && m_useTileImageRadioButton->isChecked() && m_tileList.size() > 1) {
    // split input into m_tileList.size() tiles
    m_commandOutputEdit->append("Splitting image ...>");
    QApplication::processEvents();
    nstack = m_tileList.size();
    Mc_Stack* stack = Read_Mc_Stack(input1ba.data(), -1);
    if (stack->depth % nstack != 0) {
      m_commandOutputEdit->append("<font color=red>Can not split this image 1. Abort.</font>");
      QApplication::processEvents();
      Kill_Mc_Stack(stack);
      return;
    } else {
      filepath1 = splitstack(stack, input1ba.data(), nstack);
      if (filepath1 == nullptr) {
        Kill_Mc_Stack(stack);
        return;
      }
    }
    Kill_Mc_Stack(stack);
    stack = Read_Mc_Stack(input2ba.data(), -1);
    if (stack->depth % nstack != 0) {
      m_commandOutputEdit->append("<font color=red>Can not split this image 2. Abort.</font>");
      QApplication::processEvents();
      Kill_Mc_Stack(stack);
      for (int i = 0; i < nstack; ++i) {
        free(filepath1[i]);
      }
      free(filepath1);
      return;
    } else {
      filepath2 = splitstack(stack, input2ba.data(), nstack);
      if (filepath2 == nullptr) {
        for (int i = 0; i < nstack; ++i) {
          free(filepath1[i]);
        }
        free(filepath1);
        Kill_Mc_Stack(stack);
        return;
      }
    }
    Kill_Mc_Stack(stack);
    for (int i = 0; i < nstack; ++i) {
      if (m_tileList[i].bIsSelected) {
        inputStack1Filenames.push_back(filepath1[i]);
        inputStack2Filenames.push_back(filepath2[i]);
      }
    }
    for (int i = 0; i < nstack; ++i) {
      free(filepath1[i]);
      free(filepath2[i]);
    }
    free(filepath1);
    free(filepath2);
    filepath1 = nullptr;
    filepath2 = nullptr;
    nstack = m_nSel;
  } else if ((m_inputStack1Filenames.size() == 1 && m_inputStack2Filenames.size() == 1) ||
             m_useLayoutRadioButton->isChecked()) {
    nstack = m_inputStack1Filenames.size();
    inputStack1Filenames = m_inputStack1Filenames;
    inputStack2Filenames = m_inputStack2Filenames;
  } else {
    if (m_useTileImageRadioButton->isChecked()) {
      if (m_nSel >= 0) {
        // first check number of input stacks and selected stacks
        m_commandOutputEdit->setText(tr("checking file numbers for stack 1..."));
        if (m_inputStack1Filenames.size() != m_nSel && m_inputStack1Filenames.size() != m_tileList.size()) {
          m_commandOutputEdit->append(QString("<font color=red>The number of input stacks 1 (%1) is not equal to either \
                                              number of selected tiles (%2) or number of all tiles (%3). \
                                              Can not decide which files should be stitiched.  \
                                              Abort.</font>").arg(m_inputStack1Filenames.size()).arg(m_nSel).arg(
            m_tileList.size()));
          return;
        }
        nstack = m_nSel;

        if (m_inputStack1Filenames.size() == m_tileList.size()) {
          for (int i = 0; i < m_inputStack1Filenames.size(); ++i) {
            if (m_tileList[i].bIsSelected) {
              inputStack1Filenames.push_back(m_inputStack1Filenames[i]);
            }
          }
        } else {
          inputStack1Filenames = m_inputStack1Filenames;
        }
      } else {
        m_commandOutputEdit->setText(QString("<font color=red>No Tile Image, switching to blind stitching.</font>"));
        QApplication::processEvents();
        nstack = m_inputStack1Filenames.size();
        inputStack1Filenames = m_inputStack1Filenames;
        m_useFullConnectionRadioButton->click();
        QApplication::processEvents();
      }

    } else {
      nstack = m_inputStack1Filenames.size();
      inputStack1Filenames = m_inputStack1Filenames;
    }

    //check input file of stack set 2
    if (m_useTileImageRadioButton->isChecked()) {
      if (m_nSel >= 0) {
        // first check number of input stacks and selected stacks
        m_commandOutputEdit->setText(tr("checking file numbers for stack 2..."));
        if (m_inputStack2Filenames.size() != m_nSel && m_inputStack2Filenames.size() != m_tileList.size()) {
          m_commandOutputEdit->append(QString("<font color=red>The number of input stacks 2 (%1) is not equal to either \
                                              number of selected tiles (%2) or number of all tiles (%3). \
                                              Can not decide which files should be stitiched.  \
                                              Abort.</font>").arg(m_inputStack2Filenames.size()).arg(m_nSel).arg(
            m_tileList.size()));
          return;
        }

        if (m_inputStack2Filenames.size() == m_tileList.size()) {
          for (int i = 0; i < m_inputStack2Filenames.size(); ++i) {
            if (m_tileList[i].bIsSelected) {
              inputStack2Filenames.push_back(m_inputStack2Filenames[i]);
            }
          }
        } else {
          inputStack2Filenames = m_inputStack2Filenames;
        }
      } else {
        if (m_inputStack2Filenames.size() != nstack) {
          m_commandOutputEdit->append(QString("<font color=red>The number of input files of stack 2 is not equal to input 1 \
                                              Abort.</font>"));
          return;
        }
        inputStack2Filenames = m_inputStack2Filenames;
        m_commandOutputEdit->setText(QString("<font color=red>No Tile Image, switching to blind stitching.</font>"));
        QApplication::processEvents();
        m_useFullConnectionRadioButton->click();
        QApplication::processEvents();
      }

    } else {
      if (m_inputStack2Filenames.size() != nstack) {
        m_commandOutputEdit->append(QString("<font color=red>The number of input files of stack 2 is not equal to input 1 \
                                            Abort.</font>"));
        return;
      }
      inputStack2Filenames = m_inputStack2Filenames;
    }
  }


  if (m_useLayoutRadioButton->isChecked()) {
    int tmp_nstack = m_layout1SpinBox->value() * m_layout2SpinBox->value();
    if (nstack != 1) {  // check if the number fit
      if (nstack != tmp_nstack) {
        m_commandOutputEdit->append(QString(
          "<font color=red>Invalid layout: number of input stacks: %1, required input stacks: %2. Abort.</font>").arg(
          nstack).arg(tmp_nstack));
        return;
      }
    } else {   // split the input stack (only one) into tmp_nstack stacks
      nstack = tmp_nstack;
      Mc_Stack* stack = Read_Mc_Stack(input1ba.data(), -1);
      if (stack->depth % tmp_nstack != 0) {
        m_commandOutputEdit->append("<font color=red>Invalid layout. Abort.</font>");
        QApplication::processEvents();
        Kill_Mc_Stack(stack);
        return;
      } else {
        filepath1 = splitstack(stack, input1ba.data(), nstack);
        if (filepath1 == nullptr) {
          Kill_Mc_Stack(stack);
          return;
        }
      }
      Kill_Mc_Stack(stack);
      stack = Read_Mc_Stack(input2ba.data(), -1);
      if (stack->depth % tmp_nstack != 0) {
        m_commandOutputEdit->append("<font color=red>Invalid layout. Abort.</font>");
        QApplication::processEvents();
        Kill_Mc_Stack(stack);
        for (int i = 0; i < nstack; ++i) {
          free(filepath1[i]);
        }
        free(filepath1);
        return;
      } else {
        filepath2 = splitstack(stack, input2ba.data(), nstack);
        if (filepath2 == nullptr) {
          for (int i = 0; i < nstack; ++i) {
            free(filepath1[i]);
          }
          free(filepath1);
          Kill_Mc_Stack(stack);
          return;
        }
      }
      Kill_Mc_Stack(stack);
    }
  }

  m_commandOutputEdit->append(QString("Stitching %1 images ...").arg(nstack * 2));
  QApplication::processEvents();

  //QByteArray outputba = m_outputFileEdit->text().toLocal8Bit();

  int intv[3];

  intv[0] = m_intvXSpinBox->value();
  intv[1] = m_intvYSpinBox->value();
  intv[2] = m_intvZSpinBox->value();

  if (m_dsCheckBox->isChecked()) {
    intv[0] /= m_dsXSpinBox->value();
    intv[1] /= m_dsYSpinBox->value();
    intv[2] /= m_dsZSpinBox->value();
  }

  int** final_offset = nullptr;
  int** stackSizes = nullptr;

  QList<QByteArray> filepath1List;
  if (filepath1 == nullptr) {
    filepath1 = (char**) malloc(sizeof(char*) * nstack);
    for (int i = 0; i < nstack; ++i) {
      filepath1List.push_front(inputStack1Filenames[i].toLocal8Bit());
      filepath1[i] = filepath1List[0].data();
    }
  }

  QList<QByteArray> filepath2List;
  if (filepath2 == nullptr) {
    filepath2 = (char**) malloc(sizeof(char*) * nstack);
    for (int i = 0; i < nstack; ++i) {
      filepath2List.push_front(inputStack2Filenames[i].toLocal8Bit());
      filepath2[i] = filepath2List[0].data();
    }
  }

  //first get conn and all_config (position relationship)
  int*** all_config;
  GUARDED_MALLOC_ARRAY(all_config, nstack, int * *);
  for (int i = 0; i < nstack; ++i) {
    GUARDED_MALLOC_ARRAY(all_config[i], nstack, int * );
    for (int j = 0; j < nstack; ++j) {
      all_config[i][j] = nullptr;
    }
  }

  int** conn = nullptr;

  if (!m_useLayoutRadioButton->isChecked() && nstack == 1) {
    conn = nullptr;
  }
  else if (m_useConfigRadioButton->isChecked()) {
    if (nstack == 2) {
      int i;
      MALLOC_2D_ARRAY(conn, nstack, nstack, int, i);
      conn[0][1] = 1;
      all_config[0][1] = iarray_calloc(3);
      all_config[0][1][0] = m_configDim1ComboBox->currentIndex() - 1;
      all_config[0][1][1] = m_configDim2ComboBox->currentIndex() - 1;
      all_config[0][1][2] = m_configDim3ComboBox->currentIndex() - 1;
    }
  }
    /*generate connection file from tile_selection.lsm file*/
  else if (m_useTileImageRadioButton->isChecked()) {
    //QVector<QVector<QVector<int> > > all_config(m_nSel, QVector<QVector<int> >(m_nSel, QVector<int>(3, 0)));
    //QVector<QVector<int> > conn(m_nSel, QVector<int>(m_nSel, 0));
    int i;
    MALLOC_2D_ARRAY(conn, nstack, nstack, int, i);
    for (int i = 0; i < nstack; ++i) {
      for (int j = 0; j < nstack; ++j) {
        conn[i][j] = 0;
      }
    }
    QVector<QVector<int> > tileMatrix(m_tileMatrix.size(), QVector<int>(m_tileMatrix[0].size(), 0));

    int index = 1;
    for (int i = 0; i < m_tileMatrix.size(); ++i) {
      for (int j = 0; j < m_tileMatrix[0].size(); ++j) {
        if (m_tileMatrix[i][j] > 0 && m_tileList[m_tileMatrix[i][j] - 1].bIsSelected) {
          tileMatrix[i][j] = index++;
        }
      }
    }
    for (int i = 0; i < tileMatrix.size(); ++i) {
      for (int j = 0; j < tileMatrix[0].size(); ++j) {
        if (tileMatrix[i][j] > 0) {
          bool connected = false;
          if (j + 1 < tileMatrix[0].size() && tileMatrix[i][j + 1] > 0) { //right
            int idx1 = tileMatrix[i][j] - 1;
            int idx2 = tileMatrix[i][j + 1] - 1;
            conn[idx1][idx2] = 1;
            conn[idx2][idx1] = 1;
            all_config[idx1][idx2] = iarray_calloc(3);
            all_config[idx1][idx2][0] = -1;
            all_config[idx1][idx2][1] = 0;
            all_config[idx1][idx2][2] = 0;
            all_config[idx2][idx1] = iarray_calloc(3);
            all_config[idx2][idx1][0] = 1;
            all_config[idx2][idx1][1] = 0;
            all_config[idx2][idx1][2] = 0;
            connected = true;
          }
          if (i + 1 < tileMatrix.size() && tileMatrix[i + 1][j] > 0) { //down
            int idx1 = tileMatrix[i][j] - 1;
            int idx2 = tileMatrix[i + 1][j] - 1;
            conn[idx1][idx2] = 1;
            conn[idx2][idx1] = 1;
            all_config[idx1][idx2] = iarray_calloc(3);
            all_config[idx1][idx2][0] = 0;
            all_config[idx1][idx2][1] = -1;
            all_config[idx1][idx2][2] = 0;
            all_config[idx2][idx1] = iarray_calloc(3);
            all_config[idx2][idx1][0] = 0;
            all_config[idx2][idx1][1] = 1;
            all_config[idx2][idx1][2] = 0;
            connected = true;
          }
          if (i + 1 < tileMatrix.size() && j + 1 < tileMatrix[0].size() && tileMatrix[i + 1][j + 1] > 0
              && connected == false) {  // down-right, add only if right and down are empty
            int idx1 = tileMatrix[i][j] - 1;
            int idx2 = tileMatrix[i + 1][j + 1] - 1;
            conn[idx1][idx2] = 1;
            conn[idx2][idx1] = 1;
            all_config[idx1][idx2] = iarray_calloc(3);
            all_config[idx1][idx2][0] = -1;
            all_config[idx1][idx2][1] = -1;
            all_config[idx1][idx2][2] = 0;
            all_config[idx2][idx1] = iarray_calloc(3);
            all_config[idx2][idx1][0] = 1;
            all_config[idx2][idx1][1] = 1;
            all_config[idx2][idx1][2] = 0;
            connected = true;
          }
          if (i + 1 < tileMatrix.size() && j - 1 >= 0 && tileMatrix[i + 1][j - 1] > 0
              && tileMatrix[i][j - 1] == 0 &&
              tileMatrix[i + 1][j] == 0) {  // down-left, add only if left and down are empty
            int idx1 = tileMatrix[i][j] - 1;
            int idx2 = tileMatrix[i + 1][j - 1] - 1;
            conn[idx1][idx2] = 1;
            conn[idx2][idx1] = 1;
            all_config[idx1][idx2] = iarray_calloc(3);
            all_config[idx1][idx2][0] = 1;
            all_config[idx1][idx2][1] = -1;
            all_config[idx1][idx2][2] = 0;
            all_config[idx2][idx1] = iarray_calloc(3);
            all_config[idx2][idx1][0] = -1;
            all_config[idx2][idx1][1] = 1;
            all_config[idx2][idx1][2] = 0;
            connected = true;
          }
          if (connected == false) {  // test if this image is connected
            if (i - 1 >= 0 && j - 1 >= 0 && tileMatrix[i - 1][j - 1] > 0) {  // up-left
              int idx1 = tileMatrix[i][j] - 1;
              int idx2 = tileMatrix[i - 1][j - 1] - 1;
              if (conn[idx1][idx2] == 1)
                connected = true;
            }
            if (j - 1 >= 0 && tileMatrix[i][j - 1] > 0) { // left
              int idx1 = tileMatrix[i][j] - 1;
              int idx2 = tileMatrix[i][j - 1] - 1;
              if (conn[idx1][idx2] == 1)
                connected = true;
            }
            if (i - 1 >= 0 && tileMatrix[i - 1][j] > 0) { //up
              int idx1 = tileMatrix[i][j] - 1;
              int idx2 = tileMatrix[i - 1][j] - 1;
              if (conn[idx1][idx2] == 1)
                connected = true;
            }
            if (i - 1 >= 0 && j + 1 < tileMatrix[0].size() && tileMatrix[i - 1][j + 1] > 0) {  // up-right
              int idx1 = tileMatrix[i][j] - 1;
              int idx2 = tileMatrix[i - 1][j + 1] - 1;
              if (conn[idx1][idx2] == 1)
                connected = true;
            }
          }
          if (connected == false) {
            m_commandOutputEdit->append(
              QString("<font color=red>Can not stitch because images are not connected. Abort.</font>"));
            QApplication::processEvents();
            free(filepath1);   //filepath do not own the memory
            free(filepath2);
            filepath1 = nullptr;
            filepath2 = nullptr;
            if (conn != nullptr) {
              FREE_2D_ARRAY(conn, nstack);
              conn = nullptr;
            }
            for (int i = 0; i < nstack; ++i) {
              for (int j = 0; j < nstack; ++j) {
                free(all_config[i][j]);
              }
            }
            for (int i = 0; i < nstack; ++i) {
              free(all_config[i]);
            }
            free(all_config);
            all_config = nullptr;
            return;
          }
        }
      }
    }

    m_commandOutputEdit->append(inputStack1Filenames.join("\n"));
    m_commandOutputEdit->append(inputStack2Filenames.join("\n"));
    QApplication::processEvents();

  } else if (m_useConnFileRadioButton->isChecked()) {
    m_commandOutputEdit->append("Loading connection file...");
    QByteArray connba = m_connFileEdit->text().toLocal8Bit();
    conn = load_conn(connba.data(), all_config);
    if (conn == nullptr) {
      m_commandOutputEdit->append(
        QString("<font color=red>Failed to load connection file: %1. Abort.</font>").arg(m_connFileEdit->text()));
      for (int i = 0; i < nstack; ++i) {
        free(all_config[i]);
      }
      free(all_config);
      all_config = nullptr;
      free(filepath1);
      free(filepath2);
      filepath1 = nullptr;
      filepath2 = nullptr;
      return;
    }
  } else if (m_useFullConnectionRadioButton->isChecked()) {
    m_commandOutputEdit->append("<font color=red>Blind Stitching...</font>");
    QApplication::processEvents();
  } else if (m_useLayoutRadioButton->isChecked()) {
    int i;
    MALLOC_2D_ARRAY(conn, nstack, nstack, int, i);
    for (int i = 0; i < nstack; ++i) {
      for (int j = 0; j < nstack; ++j) {
        conn[i][j] = 0;
      }
    }
    int row = m_layout1SpinBox->value();
    int col = m_layout2SpinBox->value();

    int neighbor[4];
    int is_in_bound[4];
    //Stack_Neighbor_Offset(4, row, col, neighbor);
    Stack_Neighbor_Offset(4, col, row, neighbor);
    for (int i = 0; i < nstack; ++i) {
      //int nbound = Stack_Neighbor_Bound_Test_I(4, row, col, 1, i,
      //                                         is_in_bound);
      int nbound = Stack_Neighbor_Bound_Test_I(4, col, row, 1, i, is_in_bound);
      if (nbound == 4) {
        for (int j = 0; j < 4; ++j) {
          int nbr = i + neighbor[j];
          if (nbr > i) {
            conn[i][nbr] = 1;
          }
        }
      } else {
        for (int j = 0; j < 4; ++j) {
          int nbr = i + neighbor[j];
          if (nbr > i) {
            if (is_in_bound[j]) {
              conn[i][nbr] = 1;
            }
          }
        }
      }
    }
  }

  final_offset = (int**) malloc(sizeof(int*) * nstack * 2);
  for (int i = 0; i < nstack * 2; ++i) {
    final_offset[i] = iarray_calloc(3);
  }

  stackSizes = (int**) malloc(sizeof(int*) * nstack * 2);
  for (int i = 0; i < nstack * 2; ++i) {
    stackSizes[i] = (int*) malloc(3 * sizeof(int));
  }

  /* number of possible pairs */
  int npair = 2 * nstack * (nstack * 2 + 1);

  /* allocate space for correlation scores */
  double* max_corr;
  max_corr = (double*) malloc(sizeof(double) * npair);
  double* unnorm_maxcorr;
  unnorm_maxcorr = (double*) malloc(sizeof(double) * npair);

  int** offset;

  int i;
  MALLOC_2D_ARRAY(offset, npair, 3, int, i);

  int** pairs;
  pairs = (int**) malloc(sizeof(int*) * npair);
  for (int i = 0; i < npair; ++i) {
    pairs[i] = (int*) malloc(2 * sizeof(int));
  }

  int idx = 0;

  Stack** downstacks;
  downstacks = (Stack**) malloc(sizeof(Stack * ) * nstack * 2);

  Stack* stack1 = nullptr;
  Stack* stack2 = nullptr;

  int channel1Index = m_channel1ComboBox->currentIndex();
  int bgsub1Index = m_bgsub1ComboBox->currentIndex();
  int channel2Index = m_channel2ComboBox->currentIndex();
  int bgsub2Index = m_bgsub2ComboBox->currentIndex();

  m_commandOutputEdit->append("Load stacks ...");
  for (int i = 0; i < nstack * 2; ++i) {
    m_commandOutputEdit->append(QString("Stack %1 ...").arg(i));
    if (i < nstack) {
      stack1 = readStack(filepath1[i], bgsub1Index, channel1Index);
    } else {
      stack1 = readStack(filepath2[i - nstack], bgsub2Index, channel2Index);
    }

    if (m_dsCheckBox->isChecked()) {
      m_commandOutputEdit->append("Downsampling ...");
      Downsample_Stack_Mean(stack1, m_dsXSpinBox->value() - 1,
                            m_dsYSpinBox->value() - 1,
                            m_dsZSpinBox->value() - 1,
                            stack1);
    }

    //!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
    //if (stack1->kind == COLOR) {
    //  Translate_Stack(stack1, GREY, 1);
    //}

    stackSizes[i][0] = stack1->width;
    stackSizes[i][1] = stack1->height;
    stackSizes[i][2] = stack1->depth;

    if ((i < nstack && bgsub1Index == 2) || (i >= nstack && bgsub2Index == 2)) {    //After read
      Pixel_Range* pr = Stack_Range(stack1, 0);
      Stack_Sub_Common(stack1, 0, (int) ((pr->minval + pr->maxval) / 2));
    }

    m_commandOutputEdit->append("Downsample stack ...");
    if (stack1->depth > 3) {
      downstacks[i] = Downsample_Stack_Mean(stack1, intv[0], intv[1], intv[2],
                                            nullptr);
    } else { /* no z downsampling for thin stacks */
      intv[2] = 0;
      downstacks[i] = Downsample_Stack_Mean(stack1, intv[0], intv[1], intv[2],
                                            nullptr);
    }

    if ((i < nstack && bgsub1Index == 3) || (i >= nstack && bgsub2Index == 3)) {   //After downsample
      Pixel_Range* pr = Stack_Range(downstacks[i], 0);
      Stack_Sub_Common(downstacks[i], 0, (int) ((pr->minval + pr->maxval) / 2));
    }

    Kill_Stack(stack1);
    QApplication::processEvents();
  }
  Reset_Stack();


#ifdef _DEBUG_
  tic();
#endif

  /* rough estimation */
  for (int i = 0; i < nstack - 1; ++i) {
    for (int j = i + 1; j < nstack; ++j) {
      if ((conn == nullptr) || (conn[i][j] == 1)) {
        max_corr[idx] = Align_Stack_MR_D(downstacks[i], downstacks[j], intv,
                                         -1, all_config[i][j], offset[idx],
                                         unnorm_maxcorr + idx);
        max_corr[idx] = -max_corr[idx];

        pairs[idx][0] = i;
        pairs[idx][1] = j;

        m_commandOutputEdit->append(QString("(%1,%2) : (%3,%4,%5) : %6").
          arg(pairs[idx][0]).arg(pairs[idx][1]).
          arg(offset[idx][0]).arg(offset[idx][1]).
          arg(offset[idx][2]).arg(max_corr[idx]));
        QApplication::processEvents();

        idx++;
      }
    }
    Kill_Stack(downstacks[i]);
  }
  Kill_Stack(downstacks[nstack - 1]);

  /* rough estimation */
  for (int i = nstack; i < 2 * nstack - 1; ++i) {
    for (int j = i + 1; j < 2 * nstack; ++j) {
      if ((conn == nullptr) || (conn[i - nstack][j - nstack] == 1)) {
        max_corr[idx] = Align_Stack_MR_D(downstacks[i], downstacks[j], intv,
                                         -1, all_config[i - nstack][j - nstack], offset[idx],
                                         unnorm_maxcorr + idx);
        max_corr[idx] = -max_corr[idx];

        pairs[idx][0] = i;
        pairs[idx][1] = j;

        m_commandOutputEdit->append(QString("(%1,%2) : (%3,%4,%5) : %6").
          arg(pairs[idx][0]).arg(pairs[idx][1]).
          arg(offset[idx][0]).arg(offset[idx][1]).
          arg(offset[idx][2]).arg(max_corr[idx]));
        QApplication::processEvents();

        idx++;
      }
    }
    Kill_Stack(downstacks[i]);
  }
  Kill_Stack(downstacks[2 * nstack - 1]);

  free(downstacks);

  int stack1kind;
  int stack2kind;
  // rough estimate inter stack corr
  for (int i = 0; i < nstack; ++i) {
    stack1 = Read_Sc_Stack(filepath1[i], m_commonChannel1SpinBox->value() - 1);
    stack2 = Read_Sc_Stack(filepath2[i], m_commonChannel2SpinBox->value() - 1);
    stack1kind = stack1->kind;
    stack2kind = stack2->kind;
    if (m_dsCheckBox->isChecked()) {
      m_commandOutputEdit->append("Downsampling ...");
      Downsample_Stack_Mean(stack1, m_dsXSpinBox->value() - 1,
                            m_dsYSpinBox->value() - 1,
                            m_dsZSpinBox->value() - 1,
                            stack1);
      Downsample_Stack_Mean(stack2, m_dsXSpinBox->value() - 1,
                            m_dsYSpinBox->value() - 1,
                            m_dsZSpinBox->value() - 1,
                            stack2);
    }
    if (stack1->kind > 1)
      Translate_Stack(stack1, GREY, 1);
    if (stack2->kind > 1)
      Translate_Stack(stack2, GREY, 1);

    m_commandOutputEdit->append("Downsample stack ...");
    if (stack1->depth > 3) {
      Downsample_Stack_Mean(stack1, intv[0], intv[1], intv[2],
                            stack1);
      Downsample_Stack_Mean(stack2, intv[0], intv[1], intv[2],
                            stack2);
    } else { /* no z downsampling for thin stacks */
      intv[2] = 0;
      Downsample_Stack_Mean(stack1, intv[0], intv[1], intv[2],
                            stack1);
      Downsample_Stack_Mean(stack2, intv[0], intv[1], intv[2],
                            stack2);
    }

    max_corr[idx] = Align_Stack_MR_D(stack1, stack2, intv,
                                     -1, nullptr, offset[idx],
                                     unnorm_maxcorr + idx);
    max_corr[idx] = -max_corr[idx];

    pairs[idx][0] = i;
    pairs[idx][1] = i + nstack;

    m_commandOutputEdit->append(QString("(%1,%2) : (%3,%4,%5) : %6").
      arg(pairs[idx][0]).arg(pairs[idx][1]).
      arg(offset[idx][0]).arg(offset[idx][1]).
      arg(offset[idx][2]).arg(max_corr[idx]));
    QApplication::processEvents();

    idx++;
    Kill_Stack(stack1);
    Kill_Stack(stack2);
  }

  /* actual number of pairs */
  npair = idx;

  int* permidx;
  permidx = (int*) malloc(sizeof(int) * npair);
  for (int i = 0; i < npair; ++i) {
    permidx[i] = i;
  }
  int* labels;
  labels = (int*) malloc(sizeof(int) * nstack * 2);
  for (int i = 0; i < nstack * 2; ++i) {
    labels[i] = 0;
  }
  int** selpairs;
  selpairs = (int**) malloc(sizeof(int*) * (2 * nstack - 1));
  for (int i = 0; i < 2 * nstack - 1; ++i)
    selpairs[i] = (int*) malloc(sizeof(int) * 2);

  int** seloffset;
  seloffset = (int**) malloc(sizeof(int*) * (2 * nstack - 1));
  for (int i = 0; i < 2 * nstack - 1; ++i)
    seloffset[i] = (int*) malloc(sizeof(int) * 3);

  darray_qsort(max_corr, permidx, npair);
  free(max_corr);
  free(unnorm_maxcorr);

  idx = 0;
  labels[pairs[permidx[0]][0]] = 1;

  i = 0;
  while (idx < 2 * nstack - 1) {
    //one and only one idx has not been added
    if (labels[pairs[permidx[i]][0]] != labels[pairs[permidx[i]][1]]) {
      m_commandOutputEdit->append(QString("(%1,%2)").arg(pairs[permidx[i]][0]).arg(pairs[permidx[i]][1]));
      QApplication::processEvents();

      selpairs[idx][0] = pairs[permidx[i]][0];
      selpairs[idx][1] = pairs[permidx[i]][1];
      seloffset[idx][0] = offset[permidx[i]][0];
      seloffset[idx][1] = offset[permidx[i]][1];
      seloffset[idx][2] = offset[permidx[i]][2];

      int v1 = selpairs[idx][0];
      int v2 = selpairs[idx][1];

      if (v1 < nstack && v2 < nstack) {
        stack1 = readStack(filepath1[v1], 0, channel1Index);   // no background sub
        stack2 = readStack(filepath1[v2], 0, channel1Index);   // no background sub
      } else if (v1 >= nstack && v2 >= nstack) {
        stack1 = readStack(filepath2[v1 - nstack], 0, channel2Index);   // no background sub
        stack2 = readStack(filepath2[v2 - nstack], 0, channel2Index);   // no background sub
      } else {   // v1 < v2
        stack1 = Read_Sc_Stack(filepath1[v1], m_commonChannel1SpinBox->value() - 1);
        stack2 = Read_Sc_Stack(filepath2[v2 - nstack], m_commonChannel2SpinBox->value() - 1);
      }
      Translate_Stack(stack1, GREY, 1);
      Translate_Stack(stack2, GREY, 1);

      if (m_dsCheckBox->isChecked()) {
        Downsample_Stack_Mean(stack1, m_dsXSpinBox->value() - 1,
                              m_dsYSpinBox->value() - 1,
                              m_dsZSpinBox->value() - 1,
                              stack1);
        Downsample_Stack_Mean(stack2, m_dsXSpinBox->value() - 1,
                              m_dsYSpinBox->value() - 1,
                              m_dsZSpinBox->value() - 1,
                              stack2);
      }

      Align_Stack_MR_D(stack1, stack2, intv, 2, nullptr, seloffset[idx], nullptr);

      if (labels[pairs[permidx[i]][0]] == 1) {
        final_offset[v2][0] =
          final_offset[v1][0] - seloffset[idx][0] + stack1->width - 1;
        final_offset[v2][1] =
          final_offset[v1][1] - seloffset[idx][1] + stack1->height - 1;
        final_offset[v2][2] =
          final_offset[v1][2] - seloffset[idx][2] + stack1->depth - 1;
      } else {
        final_offset[v1][0] =
          final_offset[v2][0] + seloffset[idx][0] - stack2->width + 1;
        final_offset[v1][1] =
          final_offset[v2][1] + seloffset[idx][1] - stack2->height + 1;
        final_offset[v1][2] =
          final_offset[v2][2] + seloffset[idx][2] - stack2->depth + 1;
      }

      Kill_Stack(stack1);
      Kill_Stack(stack2);

      labels[pairs[permidx[i]][0]] = 1;
      labels[pairs[permidx[i]][1]] = 1;
      idx++;
      i = 0;
    }
    ++i;
    qDebug() << "hasfw: " << i;
  }
  FREE_2D_ARRAY(pairs, npair);
  FREE_2D_ARRAY(offset, npair);
  FREE_2D_ARRAY(selpairs, 2 * nstack - 1);
  FREE_2D_ARRAY(seloffset, 2 * nstack - 1);
  //Kill_Stack(stack1);
  //Kill_Stack(stack2);
  free(permidx);
  free(labels);

#ifdef _DEBUG_
  qDebug() << "Time passed: " << toc();
#endif

  Reset_Stack();

  if (conn != nullptr) {
    FREE_2D_ARRAY(conn, nstack);
    conn = nullptr;
  }
  for (int i = 0; i < nstack; ++i) {
    for (int j = 0; j < nstack; ++j) {
      free(all_config[i][j]);
    }
  }
  for (int i = 0; i < nstack; ++i) {
    free(all_config[i]);
  }
  free(all_config);
  all_config = nullptr;

  for (int i = 0; i < 2 * nstack; ++i) {
    if (i < nstack) {
      m_commandOutputEdit->append(filepath1[i]);
    } else {
      m_commandOutputEdit->append(filepath2[i - nstack]);
    }
    m_commandOutputEdit->append(QString("(%1,%2,%3) (%4,%5,%6)").
        arg(final_offset[i][0]).arg(final_offset[i][1]).arg(final_offset[i][2])
                                  .arg(stackSizes[i][0]).arg(stackSizes[i][1]).arg(stackSizes[i][2]));
  }

  int merge_mode = m_mergeMode1ComboBox->currentIndex() + 1;

  //bool large_stack = true;
  Mc_Stack* new_stack = nullptr;

  //????????????????????????????????????????
  //  for (int i = 0; i < nstack; ++i) {
  //    if (m_dsCheckBox->isChecked()) {
  //      int ds[3];
  //      ds[0] = m_dsXSpinBox->value();
  //      ds[1] = m_dsYSpinBox->value();
  //      ds[2] = m_dsZSpinBox->value();
  //      for (int j = 0; j < 3; ++j) {
  //        if (final_offset[i][j] < 0) {
  //          final_offset[i][j]--;
  //        }

  //        final_offset[i][j] /= ds[j];
  //      }
  //    }
  //    m_commandOutputEdit->append(QString("(%1,%2,%3)").arg(final_offset[i][0]).arg(final_offset[i][1]).arg(final_offset[i][2]));
  //    QApplication::processEvents();
  //  }

  //  if (large_stack == false) {
  //    Mc_Stack **stacks = (Mc_Stack **) malloc(sizeof(Mc_Stack*) * nstack);

  //    for (int i = 0; i < nstack; ++i) {
  //      stacks[i] = Read_Mc_Stack(filepath1[i], -1);
  //      if (m_dsCheckBox->isChecked()) {
  //        Mc_Stack_Downsample_Mean(stacks[i], m_dsXSpinBox->value() - 1, m_dsYSpinBox->value() - 1,
  //                                 m_dsZSpinBox->value() - 1,
  //                                 stacks[i]);
  //      }
  //    }

  //    new_stack = Mc_Stack_Merge(stacks, nstack, final_offset,
  //                               merge_mode);

  //    for (int i = 0; i< nstack; ++i) {
  //      Kill_Mc_Stack(stacks[i]);
  //    }
  //    free(stacks);
  //  } else {
//  if (m_dsCheckBox->isChecked()) {
//    int ds[3];
//    ds[0] = m_dsXSpinBox->value();
//    ds[1] = m_dsYSpinBox->value();
//    ds[2] = m_dsZSpinBox->value();

//    new_stack = Mc_Stack_Merge_F(filepath1, nstack, final_offset,
//                                 merge_mode, ds);
//  } else {
//    new_stack = Mc_Stack_Merge_F(filepath1, nstack, final_offset,
//                                 merge_mode, nullptr);
//  }
  //  }



  int** start;
  int** end;
  int corner1[3];
  int corner2[3];
  MALLOC_2D_ARRAY(start, 3, nstack * 2, int, i);
  MALLOC_2D_ARRAY(end, 3, nstack * 2, int, i);
  for (i = 0; i < nstack * 2; ++i) {
    for (int j = 0; j < 3; ++j) {
      start[j][i] = final_offset[i][j];
    }
  }
  for (i = 0; i < nstack * 2; ++i) {
    end[0][i] = start[0][i] + stackSizes[i][0] - 1;
    end[1][i] = start[1][i] + stackSizes[i][1] - 1;
    end[2][i] = start[2][i] + stackSizes[i][2] - 1;
  }
  for (i = 0; i < 3; ++i) {
    corner1[i] = iarray_min(start[i], nstack * 2, nullptr);
  }
  for (i = 0; i < 3; ++i) {
    corner2[i] = iarray_max(end[i], nstack * 2, nullptr);
  }
  FREE_2D_ARRAY(start, 3);
  FREE_2D_ARRAY(end, 3);
  int width = corner2[0] - corner1[0] + 1;
  int height = corner2[1] - corner1[1] + 1;
  int depth = corner2[2] - corner1[2] + 1;

  new_stack = Make_Mc_Stack(stack1kind, width, height, depth, noutputchannel);

  Stack** stacks = (Stack**) malloc(sizeof(Stack * ) * nstack);

  if (noutputchannel == 3) {
    ch3 = Mc_Stack_Channel(new_stack, 2);
    if (m_outputCh3ImageComboBox->currentIndex() == 1) {
      int channel = m_outputCh3ImageChannelSpinBox->value() - 1;
      for (int i = 0; i < nstack; ++i) {
        stacks[i] = Read_Sc_Stack(filepath1[i], channel);
        if (m_dsCheckBox->isChecked()) {
          Downsample_Stack_Mean(stacks[i], m_dsXSpinBox->value() - 1, m_dsYSpinBox->value() - 1,
                                m_dsZSpinBox->value() - 1, stacks[i]);
        }
      }
      Stack_Merge_M_FC(stacks, nstack, final_offset, merge_mode, &ch3, corner1, corner2);
      for (int i = 0; i < nstack; ++i) {
        Kill_Stack(stacks[i]);
      }
    } else if (m_outputCh3ImageComboBox->currentIndex() == 2) {
      int channel = m_outputCh3ImageChannelSpinBox->value() - 1;
      for (int i = 0; i < nstack; ++i) {
        stacks[i] = Read_Sc_Stack(filepath2[i], channel);
        if (m_dsCheckBox->isChecked()) {
          Downsample_Stack_Mean(stacks[i], m_dsXSpinBox->value() - 1, m_dsYSpinBox->value() - 1,
                                m_dsZSpinBox->value() - 1, stacks[i]);
        }
        if (stack2kind != stack1kind) {
          Translate_Stack(stacks[i], stack1kind, 1);
        }
      }
      Stack_Merge_M_FC(stacks, nstack, final_offset + nstack, merge_mode, &ch3, corner1, corner2);
      for (int i = 0; i < nstack; ++i) {
        Kill_Stack(stacks[i]);
      }
    }
  }

  ch2 = Mc_Stack_Channel(new_stack, 1);
  if (m_outputCh2ImageComboBox->currentIndex() == 1) {
    int channel = m_outputCh2ImageChannelSpinBox->value() - 1;
    for (int i = 0; i < nstack; ++i) {
      stacks[i] = Read_Sc_Stack(filepath1[i], channel);
      if (m_dsCheckBox->isChecked()) {
        Downsample_Stack_Mean(stacks[i], m_dsXSpinBox->value() - 1, m_dsYSpinBox->value() - 1,
                              m_dsZSpinBox->value() - 1, stacks[i]);
      }
    }
    Stack_Merge_M_FC(stacks, nstack, final_offset, merge_mode, &ch2, corner1, corner2);
    for (int i = 0; i < nstack; ++i) {
      Kill_Stack(stacks[i]);
    }
  } else if (m_outputCh2ImageComboBox->currentIndex() == 2) {
    int channel = m_outputCh2ImageChannelSpinBox->value() - 1;
    for (int i = 0; i < nstack; ++i) {
      stacks[i] = Read_Sc_Stack(filepath2[i], channel);
      if (m_dsCheckBox->isChecked()) {
        Downsample_Stack_Mean(stacks[i], m_dsXSpinBox->value() - 1, m_dsYSpinBox->value() - 1,
                              m_dsZSpinBox->value() - 1, stacks[i]);
      }
      if (stack2kind != stack1kind) {
        Translate_Stack(stacks[i], stack1kind, 1);
      }
    }
    Stack_Merge_M_FC(stacks, nstack, final_offset + nstack, merge_mode, &ch2, corner1, corner2);
    for (int i = 0; i < nstack; ++i) {
      Kill_Stack(stacks[i]);
    }
  }

  ch1 = Mc_Stack_Channel(new_stack, 0);
  if (m_outputCh1ImageComboBox->currentIndex() == 1) {
    int channel = m_outputCh1ImageChannelSpinBox->value() - 1;
    for (int i = 0; i < nstack; ++i) {
      stacks[i] = Read_Sc_Stack(filepath1[i], channel);
      if (m_dsCheckBox->isChecked()) {
        Downsample_Stack_Mean(stacks[i], m_dsXSpinBox->value() - 1, m_dsYSpinBox->value() - 1,
                              m_dsZSpinBox->value() - 1, stacks[i]);
      }
    }
    Stack_Merge_M_FC(stacks, nstack, final_offset, merge_mode, &ch1, corner1, corner2);
    for (int i = 0; i < nstack; ++i) {
      Kill_Stack(stacks[i]);
    }
  } else if (m_outputCh1ImageComboBox->currentIndex() == 2) {
    int channel = m_outputCh1ImageChannelSpinBox->value() - 1;
    for (int i = 0; i < nstack; ++i) {
      stacks[i] = Read_Sc_Stack(filepath2[i], channel);
      if (m_dsCheckBox->isChecked()) {
        Downsample_Stack_Mean(stacks[i], m_dsXSpinBox->value() - 1, m_dsYSpinBox->value() - 1,
                              m_dsZSpinBox->value() - 1, stacks[i]);
      }
      if (stack2kind != stack1kind) {
        Translate_Stack(stacks[i], stack1kind, 1);
      }
    }
    Stack_Merge_M_FC(stacks, nstack, final_offset + nstack, merge_mode, &ch1, corner1, corner2);
    for (int i = 0; i < nstack; ++i) {
      Kill_Stack(stacks[i]);
    }
  }


  if (m_d8CheckBox->isChecked()) {
    if (new_stack->kind == 2) {
      Mc_Stack_Grey16_To_8(new_stack, m_d8ComboBox->currentIndex());
    }
  }

  //Write_Mc_Stack(outputba.data(), new_stack, filepath[0]);
  QString outname = save(m_outputFileEdit->text(), new_stack);

  Kill_Mc_Stack(new_stack);
  m_commandOutputEdit->append(QString("%1 saved.").arg(outname));
  if (m_useTileImageRadioButton->isChecked() && m_tileImage != nullptr) {
    QString selectionImageOutputName = m_outputFileEdit->text();
    selectionImageOutputName.append("_TileSelectionInfo.tif");
    QImage image(*m_tileImage);

    QPainter painter(&image);
    for (int i = 0; i < m_tileList.size(); ++i) {
      QRect rect = QRect(m_tileList.at(i).region.topLeft(),
                         m_tileList.at(i).region.bottomRight());
      if (m_tileList.at(i).bIsSelected) {
        painter.fillRect(rect, QColor(255, 255, 0, 128));
      }
      QString str = QString("Image %1").arg(i + 1);
      painter.drawText(rect, str);
    }
    QImageWriter writer(selectionImageOutputName);
    if (!writer.write(image)) {
      m_commandOutputEdit->append(writer.errorString());
    } else {
      m_commandOutputEdit->append(QString("%1 saved.").arg(selectionImageOutputName));
    }

  }

  // cleanup
  if (m_useLayoutRadioButton->isChecked() && m_inputStack1Filenames.size() == 1 &&
      m_layout1SpinBox->value() * m_layout2SpinBox->value() != 1) {
    for (int i = 0; i < nstack; ++i) {
      free(filepath1[i]);
      free(filepath2[i]);
    }
    free(filepath1);
    free(filepath2);
    filepath1 = nullptr;
    filepath2 = nullptr;
  } else {
    free(filepath1);   //filepath do not own the memory
    free(filepath2);
    filepath1 = nullptr;
    filepath2 = nullptr;
  }
  FREE_2D_ARRAY(final_offset, nstack * 2);
  for (int i = 0; i < 2 * nstack; ++i) {
    free(stackSizes[i]);
  }
  free(stackSizes);
  stackSizes = nullptr;
}

QString ZStitchImageDialog::save(const QString& filepath, const Mc_Stack* mc_stack)
{
  if (mc_stack->nchannel == 1 || (mc_stack->nchannel == 3 && mc_stack->kind == 1)) {  //should be fine
    Write_Mc_Stack(filepath.toLocal8Bit().constData(), mc_stack, nullptr);
    return filepath;
  } else {  //save as raw
    QString str = filepath;
    if (!str.endsWith(".raw", Qt::CaseInsensitive)) {
      m_commandOutputEdit->append(
        "<font color=red>Current stack can not be saved as tif, use raw format instead.</font>");
      str += ".raw";
    }
    Write_Mc_Stack(str.toLocal8Bit().constData(), mc_stack, nullptr);
    return str;
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
                                                      m_openFilesPath,
                                                      tr("Conn File (*.txt)"));
  if (!connFileName.isEmpty()) {
    m_openFilesPath = connFileName;
    m_connFileEdit->setText(connFileName);
  }
}

void ZStitchImageDialog::selectOutputFile()
{
  QString outputFileName = QFileDialog::getSaveFileName(this,
                                                        tr("specify output file"),
                                                        m_openFilesPath,
                                                        tr("Output Image (*.raw *.tif)"));
  if (!outputFileName.isEmpty()) {
    m_openFilesPath = outputFileName;
    m_outputFileEdit->setText(outputFileName);
  }
}

void ZStitchImageDialog::d8Changed(int)
{

}

void ZStitchImageDialog::configDim1Changed(int)
{

}

void ZStitchImageDialog::configDim2Changed(int)
{

}

void ZStitchImageDialog::configDim3Changed(int)
{

}

void ZStitchImageDialog::fixCheckBoxChanged(int)
{

}

void ZStitchImageDialog::d8CheckBoxChanged(int state)
{
  if (state == Qt::Checked) {
    m_d8ComboBox->setEnabled(true);
  } else {
    m_d8ComboBox->setEnabled(false);
  }
}

void ZStitchImageDialog::dsCheckBoxChanged(int state)
{
  if (state == Qt::Checked) {
    m_dsXSpinBox->setEnabled(true);
    m_dsYSpinBox->setEnabled(true);
    m_dsZSpinBox->setEnabled(true);
  } else {
    m_dsXSpinBox->setEnabled(false);
    m_dsYSpinBox->setEnabled(false);
    m_dsZSpinBox->setEnabled(false);
  }
}

void ZStitchImageDialog::hasTwoInputStackSetCheckBoxChanged(int state)
{
  if (state == Qt::Checked) {
    m_inputStack2FileEdit->show();
    m_selectInputStacks2Button->show();
    m_channel2ComboBox->show();
    m_bgsub2ComboBox->show();
    m_mergeMode2ComboBox->show();

    m_concatOnlyCheckBox->hide();

    m_commonChannel1SpinBox->show();
    m_commonChannel2SpinBox->show();

    m_outputCh1ImageChannelSpinBox->show();
    m_outputCh2ImageChannelSpinBox->show();
    m_outputCh3ImageChannelSpinBox->show();
    m_outputCh1ImageComboBox->show();
    m_outputCh2ImageComboBox->show();
    m_outputCh3ImageComboBox->show();
    for (int i = 0; i < m_labelsForTwoInputs.size(); ++i) {
      m_labelsForTwoInputs[i]->show();
    }
  } else {
    m_inputStack2FileEdit->hide();
    m_selectInputStacks2Button->hide();
    m_channel2ComboBox->hide();
    m_bgsub2ComboBox->hide();
    m_mergeMode2ComboBox->hide();

    m_concatOnlyCheckBox->show();

    m_commonChannel1SpinBox->hide();
    m_commonChannel2SpinBox->hide();

    m_outputCh1ImageChannelSpinBox->hide();
    m_outputCh2ImageChannelSpinBox->hide();
    m_outputCh3ImageChannelSpinBox->hide();
    m_outputCh1ImageComboBox->hide();
    m_outputCh2ImageComboBox->hide();
    m_outputCh3ImageComboBox->hide();
    for (int i = 0; i < m_labelsForTwoInputs.size(); ++i) {
      m_labelsForTwoInputs[i]->hide();
    }
  }
}

void ZStitchImageDialog::setConnInfoSource()
{
  if (m_useTileImageRadioButton->isChecked()) {
    m_openTileImageButton->setEnabled(true);
    m_connEdit->setVisible(true);
    if (m_tileImage != nullptr) {
      m_editTileImageButton->setEnabled(true);
    } else {
      m_editTileImageButton->setEnabled(false);
    }
    m_connEdit->setEnabled(true);
    m_configDim1ComboBox->setEnabled(false);
    m_configDim2ComboBox->setEnabled(false);
    m_configDim3ComboBox->setEnabled(false);
    m_connFileEdit->setEnabled(false);
    m_selectConnFileButton->setEnabled(false);
    m_layout1SpinBox->setEnabled(false);
    m_layout2SpinBox->setEnabled(false);
  } else if (m_useConnFileRadioButton->isChecked()) {
    m_openTileImageButton->setEnabled(false);
    m_connEdit->setEnabled(false);
    m_editTileImageButton->setEnabled(false);
    m_connEdit->setVisible(false);
    m_configDim1ComboBox->setEnabled(false);
    m_configDim2ComboBox->setEnabled(false);
    m_configDim3ComboBox->setEnabled(false);
    m_connFileEdit->setEnabled(true);
    m_selectConnFileButton->setEnabled(true);
    m_layout1SpinBox->setEnabled(false);
    m_layout2SpinBox->setEnabled(false);
  } else if (m_useConfigRadioButton->isChecked()) {
    m_openTileImageButton->setEnabled(false);
    m_connEdit->setEnabled(false);
    m_editTileImageButton->setEnabled(false);
    m_connEdit->setVisible(false);
    m_configDim1ComboBox->setEnabled(true);
    m_configDim2ComboBox->setEnabled(true);
    m_configDim3ComboBox->setEnabled(true);
    m_connFileEdit->setEnabled(false);
    m_selectConnFileButton->setEnabled(false);
    m_layout1SpinBox->setEnabled(false);
    m_layout2SpinBox->setEnabled(false);
  } else if (m_useFullConnectionRadioButton->isChecked()) {
    m_openTileImageButton->setEnabled(false);
    m_connEdit->setEnabled(false);
    m_editTileImageButton->setEnabled(false);
    m_connEdit->setVisible(false);
    m_configDim1ComboBox->setEnabled(false);
    m_configDim2ComboBox->setEnabled(false);
    m_configDim3ComboBox->setEnabled(false);
    m_connFileEdit->setEnabled(false);
    m_selectConnFileButton->setEnabled(false);
    m_layout1SpinBox->setEnabled(false);
    m_layout2SpinBox->setEnabled(false);
  } else if (m_useLayoutRadioButton->isChecked()) {
    m_openTileImageButton->setEnabled(false);
    m_connEdit->setEnabled(false);
    m_editTileImageButton->setEnabled(false);
    m_connEdit->setVisible(false);
    m_configDim1ComboBox->setEnabled(false);
    m_configDim2ComboBox->setEnabled(false);
    m_configDim3ComboBox->setEnabled(false);
    m_connFileEdit->setEnabled(false);
    m_selectConnFileButton->setEnabled(false);
    m_layout1SpinBox->setEnabled(true);
    m_layout2SpinBox->setEnabled(true);
  }
}

void ZStitchImageDialog::stitchStacks()
{
  m_tabWidget->setCurrentIndex(2);
  QApplication::processEvents();

  QFileInfo outputFI(m_outputFileEdit->text());
  if (m_outputFileEdit->text().isEmpty() || !outputFI.absoluteDir().exists()) {
    m_commandOutputEdit->append("<font color=red>Please make sure the ouput folder exists.</font>");
    return;
  }
  if (m_inputStack1Filenames.size() < 1) {
    m_commandOutputEdit->append("<font color=red>Please add input files.</font>");
    return;
  }

//  if (m_fixCheckBox->isChecked()) {
//    if (fexist(outputba.data())) {
//      if (Is_Lsm(outputba.data())) {
//        Fix_Lsm_File(outputba.data());
//        return;
//      }
//    }
//  }

  if (m_hasTwoInputStackSetCheckBox->isChecked()) {
    stitchStacks2();
    return;
  }

  int nstack;
  QStringList inputStackFilenames;
  char** filepath = nullptr;
  QByteArray input1ba = m_inputStack1Filenames[0].toLocal8Bit();

  if (m_inputStack1Filenames.size() == 1 && m_useTileImageRadioButton->isChecked() && m_tileList.size() > 1) {
    // split input into m_tileList.size() tiles
    m_commandOutputEdit->append("Splitting image ...>");
    QApplication::processEvents();
    nstack = m_tileList.size();
    Mc_Stack* stack = Read_Mc_Stack(input1ba.data(), -1);
    if (stack->depth % nstack != 0) {
      m_commandOutputEdit->append("<font color=red>Can not split this image. Abort.</font>");
      QApplication::processEvents();
      Kill_Mc_Stack(stack);
      return;
    } else {
      filepath = splitstack(stack, input1ba.data(), nstack);
      if (filepath == nullptr) {
        Kill_Mc_Stack(stack);
        return;
      }
    }
    Kill_Mc_Stack(stack);
    for (int i = 0; i < nstack; ++i) {
      if (m_tileList[i].bIsSelected) {
        inputStackFilenames.push_back(filepath[i]);
      }
    }
    for (int i = 0; i < nstack; ++i) {
      free(filepath[i]);
    }
    free(filepath);
    filepath = nullptr;
    nstack = m_nSel;
  } else if (m_inputStack1Filenames.size() == 1 || m_useLayoutRadioButton->isChecked()) {
    nstack = m_inputStack1Filenames.size();
    inputStackFilenames = m_inputStack1Filenames;
  } else {
    if (m_useTileImageRadioButton->isChecked()) {
      if (m_nSel >= 0) {
        // first check number of input stacks and selected stacks
        m_commandOutputEdit->setText(tr("checking file numbers..."));
        if (m_inputStack1Filenames.size() != m_nSel && m_inputStack1Filenames.size() != m_tileList.size()) {
          m_commandOutputEdit->append(QString("<font color=red>The number of input stacks (%1) is not equal to either \
                                              number of selected tiles (%2) or number of all tiles (%3). \
                                              Can not decide which files should be stitiched.  \
                                              Abort.</font>").arg(m_inputStack1Filenames.size()).arg(m_nSel).arg(
            m_tileList.size()));
          return;
        }
        nstack = m_nSel;

        if (m_inputStack1Filenames.size() == m_tileList.size()) {
          for (int i = 0; i < m_inputStack1Filenames.size(); ++i) {
            if (m_tileList[i].bIsSelected) {
              inputStackFilenames.push_back(m_inputStack1Filenames[i]);
            }
          }
        } else {
          inputStackFilenames = m_inputStack1Filenames;
        }
      } else {
        m_commandOutputEdit->setText(QString("<font color=red>No Tile Image, switching to blind stitching.</font>"));
        QApplication::processEvents();
        nstack = m_inputStack1Filenames.size();
        inputStackFilenames = m_inputStack1Filenames;
        m_useFullConnectionRadioButton->click();
        QApplication::processEvents();
      }

    } else {
      nstack = m_inputStack1Filenames.size();
      inputStackFilenames = m_inputStack1Filenames;
    }
  }


  if (m_useLayoutRadioButton->isChecked()) {
    int tmp_nstack = m_layout1SpinBox->value() * m_layout2SpinBox->value();
    if (nstack != 1) {  // check if the number fit
      if (nstack != tmp_nstack) {
        m_commandOutputEdit->append(QString(
          "<font color=red>Invalid layout: number of input stacks: %1, required input stacks: %2. Abort.</font>").arg(
          nstack).arg(tmp_nstack));
        return;
      }
    } else {   // split the input stack (only one) into tmp_nstack stacks
      nstack = tmp_nstack;
      Mc_Stack* stack = Read_Mc_Stack(input1ba.data(), -1);
      if (stack->depth % tmp_nstack != 0) {
        m_commandOutputEdit->append("<font color=red>Invalid layout. Abort.</font>");
        QApplication::processEvents();
        Kill_Mc_Stack(stack);
        return;
      } else {
        filepath = splitstack(stack, input1ba.data(), nstack);
        if (filepath == nullptr) {
          Kill_Mc_Stack(stack);
          return;
        }
      }
      Kill_Mc_Stack(stack);
    }
  }

  m_commandOutputEdit->append(QString("Stitching %1 images ...").arg(nstack));
  QApplication::processEvents();


  QByteArray outputba = m_outputFileEdit->text().toLocal8Bit();

  if (nstack == 1) {
    if (m_dsCheckBox->isChecked()) {
      Mc_Stack* stack = Read_Mc_Stack(input1ba.data(), -1);
      m_commandOutputEdit->append("Downsampling ...");
      QApplication::processEvents();
      Mc_Stack_Downsample_Mean(stack, m_dsXSpinBox->value() - 1,
                               m_dsYSpinBox->value() - 1,
                               m_dsZSpinBox->value() - 1,
                               stack);
      //Write_Mc_Stack(outputba.data(), stack, input1ba.data());
      save(m_outputFileEdit->text(), stack);
      Kill_Mc_Stack(stack);
    } else {
      if ((Is_Tiff(input1ba.data())) && Is_Tiff(outputba.data())) {
        fcopy(input1ba.data(), outputba.data());
      } else if (Is_Lsm(outputba.data()) && Is_Lsm(input1ba.data())) {
        fcopy(input1ba.data(), outputba.data());
      } else if (Is_Raw(outputba.data()) && Is_Raw(input1ba.data())) {
        fcopy(input1ba.data(), outputba.data());
      } else {
        Mc_Stack* stack = Read_Mc_Stack(input1ba.data(), -1);
        //Write_Mc_Stack(outputba.data(), stack, nullptr);
        save(m_outputFileEdit->text(), stack);
        Kill_Mc_Stack(stack);
      }
    }
    m_commandOutputEdit->append(QString("%1 saved.").arg(m_outputFileEdit->text()));
    return;
  }

  int** final_offset = nullptr;

  QList<QByteArray> filepathList;
  if (filepath == nullptr) {
    filepath = (char**) malloc(sizeof(char*) * nstack);
    for (int i = 0; i < nstack; ++i) {
      filepathList.push_front(inputStackFilenames[i].toLocal8Bit());
      filepath[i] = filepathList[0].data();
    }
  }


  //first get conn and all_config (position relationship)
  int*** all_config;
  GUARDED_MALLOC_ARRAY(all_config, nstack, int * *);
  for (int i = 0; i < nstack; ++i) {
    GUARDED_MALLOC_ARRAY(all_config[i], nstack, int * );
    for (int j = 0; j < nstack; ++j) {
      all_config[i][j] = nullptr;
    }
  }

  int** conn = nullptr;

  if (m_useConfigRadioButton->isChecked()) {
    if (nstack == 2) {
      int i;
      MALLOC_2D_ARRAY(conn, nstack, nstack, int, i);
      conn[0][1] = 1;
      all_config[0][1] = iarray_calloc(3);
      all_config[0][1][0] = m_configDim1ComboBox->currentIndex() - 1;
      all_config[0][1][1] = m_configDim2ComboBox->currentIndex() - 1;
      all_config[0][1][2] = m_configDim3ComboBox->currentIndex() - 1;
    }
  }
    /*generate connection file from tile_selection.lsm file*/
  else if (m_useTileImageRadioButton->isChecked()) {
    //QVector<QVector<QVector<int> > > all_config(m_nSel, QVector<QVector<int> >(m_nSel, QVector<int>(3, 0)));
    //QVector<QVector<int> > conn(m_nSel, QVector<int>(m_nSel, 0));
    int i;
    MALLOC_2D_ARRAY(conn, nstack, nstack, int, i);
    for (int i = 0; i < nstack; ++i) {
      for (int j = 0; j < nstack; ++j) {
        conn[i][j] = 0;
      }
    }
    QVector<QVector<int> > tileMatrix(m_tileMatrix.size(), QVector<int>(m_tileMatrix[0].size(), 0));

    int index = 1;
    for (int i = 0; i < m_tileMatrix.size(); ++i) {
      for (int j = 0; j < m_tileMatrix[0].size(); ++j) {
        if (m_tileMatrix[i][j] > 0 && m_tileList[m_tileMatrix[i][j] - 1].bIsSelected) {
          tileMatrix[i][j] = index++;
        }
      }
    }
    for (int i = 0; i < tileMatrix.size(); ++i) {
      for (int j = 0; j < tileMatrix[0].size(); ++j) {
        if (tileMatrix[i][j] > 0) {
          bool connected = false;
          if (j + 1 < tileMatrix[0].size() && tileMatrix[i][j + 1] > 0) { //right
            int idx1 = tileMatrix[i][j] - 1;
            int idx2 = tileMatrix[i][j + 1] - 1;
            conn[idx1][idx2] = 1;
            conn[idx2][idx1] = 1;
            all_config[idx1][idx2] = iarray_calloc(3);
            all_config[idx1][idx2][0] = -1;
            all_config[idx1][idx2][1] = 0;
            all_config[idx1][idx2][2] = 0;
            all_config[idx2][idx1] = iarray_calloc(3);
            all_config[idx2][idx1][0] = 1;
            all_config[idx2][idx1][1] = 0;
            all_config[idx2][idx1][2] = 0;
            connected = true;
          }
          if (i + 1 < tileMatrix.size() && tileMatrix[i + 1][j] > 0) { //down
            int idx1 = tileMatrix[i][j] - 1;
            int idx2 = tileMatrix[i + 1][j] - 1;
            conn[idx1][idx2] = 1;
            conn[idx2][idx1] = 1;
            all_config[idx1][idx2] = iarray_calloc(3);
            all_config[idx1][idx2][0] = 0;
            all_config[idx1][idx2][1] = -1;
            all_config[idx1][idx2][2] = 0;
            all_config[idx2][idx1] = iarray_calloc(3);
            all_config[idx2][idx1][0] = 0;
            all_config[idx2][idx1][1] = 1;
            all_config[idx2][idx1][2] = 0;
            connected = true;
          }
          if (i + 1 < tileMatrix.size() && j + 1 < tileMatrix[0].size() && tileMatrix[i + 1][j + 1] > 0
              && connected == false) {  // down-right, add only if right and down are empty
            int idx1 = tileMatrix[i][j] - 1;
            int idx2 = tileMatrix[i + 1][j + 1] - 1;
            conn[idx1][idx2] = 1;
            conn[idx2][idx1] = 1;
            all_config[idx1][idx2] = iarray_calloc(3);
            all_config[idx1][idx2][0] = -1;
            all_config[idx1][idx2][1] = -1;
            all_config[idx1][idx2][2] = 0;
            all_config[idx2][idx1] = iarray_calloc(3);
            all_config[idx2][idx1][0] = 1;
            all_config[idx2][idx1][1] = 1;
            all_config[idx2][idx1][2] = 0;
            connected = true;
          }
          if (i + 1 < tileMatrix.size() && j - 1 >= 0 && tileMatrix[i + 1][j - 1] > 0
              && tileMatrix[i][j - 1] == 0 &&
              tileMatrix[i + 1][j] == 0) {  // down-left, add only if left and down are empty
            int idx1 = tileMatrix[i][j] - 1;
            int idx2 = tileMatrix[i + 1][j - 1] - 1;
            conn[idx1][idx2] = 1;
            conn[idx2][idx1] = 1;
            all_config[idx1][idx2] = iarray_calloc(3);
            all_config[idx1][idx2][0] = 1;
            all_config[idx1][idx2][1] = -1;
            all_config[idx1][idx2][2] = 0;
            all_config[idx2][idx1] = iarray_calloc(3);
            all_config[idx2][idx1][0] = -1;
            all_config[idx2][idx1][1] = 1;
            all_config[idx2][idx1][2] = 0;
            connected = true;
          }
          if (connected == false) {  // test if this image is connected
            if (i - 1 >= 0 && j - 1 >= 0 && tileMatrix[i - 1][j - 1] > 0) {  // up-left
              int idx1 = tileMatrix[i][j] - 1;
              int idx2 = tileMatrix[i - 1][j - 1] - 1;
              if (conn[idx1][idx2] == 1)
                connected = true;
            }
            if (j - 1 >= 0 && tileMatrix[i][j - 1] > 0) { // left
              int idx1 = tileMatrix[i][j] - 1;
              int idx2 = tileMatrix[i][j - 1] - 1;
              if (conn[idx1][idx2] == 1)
                connected = true;
            }
            if (i - 1 >= 0 && tileMatrix[i - 1][j] > 0) { //up
              int idx1 = tileMatrix[i][j] - 1;
              int idx2 = tileMatrix[i - 1][j] - 1;
              if (conn[idx1][idx2] == 1)
                connected = true;
            }
            if (i - 1 >= 0 && j + 1 < tileMatrix[0].size() && tileMatrix[i - 1][j + 1] > 0) {  // up-right
              int idx1 = tileMatrix[i][j] - 1;
              int idx2 = tileMatrix[i - 1][j + 1] - 1;
              if (conn[idx1][idx2] == 1)
                connected = true;
            }
          }
          if (connected == false) {
            m_commandOutputEdit->append(
              QString("<font color=red>Can not stitch because images are not connected. Abort.</font>"));
            QApplication::processEvents();
            free(filepath);   //filepath do not own the memory
            filepath = nullptr;
            if (conn != nullptr) {
              FREE_2D_ARRAY(conn, nstack);
              conn = nullptr;
            }
            for (int i = 0; i < nstack; ++i) {
              for (int j = 0; j < nstack; ++j) {
                free(all_config[i][j]);
              }
            }
            for (int i = 0; i < nstack; ++i) {
              free(all_config[i]);
            }
            free(all_config);
            all_config = nullptr;
            return;
          }
        }
      }
    }

    m_commandOutputEdit->append(inputStackFilenames.join("\n"));
    QApplication::processEvents();

  } else if (m_useConnFileRadioButton->isChecked()) {
    m_commandOutputEdit->append("Loading connection file...");
    QByteArray connba = m_connFileEdit->text().toLocal8Bit();
    conn = load_conn(connba.data(), all_config);
    if (conn == nullptr) {
      m_commandOutputEdit->append(
        QString("<font color=red>Failed to load connection file: %1. Abort.</font>").arg(m_connFileEdit->text()));
      for (int i = 0; i < nstack; ++i) {
        free(all_config[i]);
      }
      free(all_config);
      all_config = nullptr;
      free(filepath);
      filepath = nullptr;
      return;
    }
  } else if (m_useFullConnectionRadioButton->isChecked()) {
    m_commandOutputEdit->append("<font color=red>Blind Stitching...</font>");
    QApplication::processEvents();
  } else if (m_useLayoutRadioButton->isChecked()) {
    int i;
    MALLOC_2D_ARRAY(conn, nstack, nstack, int, i);
    for (int i = 0; i < nstack; ++i) {
      for (int j = 0; j < nstack; ++j) {
        conn[i][j] = 0;
      }
    }
    int row = m_layout1SpinBox->value();
    int col = m_layout2SpinBox->value();

    int neighbor[4];
    int is_in_bound[4];
    //Stack_Neighbor_Offset(4, row, col, neighbor);
    Stack_Neighbor_Offset(4, col, row, neighbor);
    for (int i = 0; i < nstack; ++i) {
      //int nbound = Stack_Neighbor_Bound_Test_I(4, row, col, 1, i,
      //                                         is_in_bound);
      int nbound = Stack_Neighbor_Bound_Test_I(4, col, row, 1, i, is_in_bound);
      if (nbound == 4) {
        for (int j = 0; j < 4; ++j) {
          int nbr = i + neighbor[j];
          if (nbr > i) {
            conn[i][nbr] = 1;
          }
        }
      } else {
        for (int j = 0; j < 4; ++j) {
          int nbr = i + neighbor[j];
          if (nbr > i) {
            if (is_in_bound[j]) {
              conn[i][nbr] = 1;
            }
          }
        }
      }
    }
  }

  int** stackSizes = (int**) malloc(sizeof(int*) * nstack);
  for (int i = 0; i < nstack; ++i) {
    stackSizes[i] = (int*) malloc(3 * sizeof(int));
  }

  final_offset = (int**) malloc(sizeof(int*) * nstack);
  for (int i = 0; i < nstack; ++i) {
    final_offset[i] = iarray_calloc(3);
  }

  if (m_concatOnlyCheckBox->isChecked()) {

    if (conn == nullptr) {
      m_commandOutputEdit->append(
        "<font color=red>Position information incomplete for image concatenate, Abort.</font>");
      for (int i = 0; i < nstack; ++i) {
        free(all_config[i]);
      }
      free(all_config);
      return;
    }
    //check if stack size is equal
    Stack_Size_F(filepath[0], stackSizes[0]);

    int stackwidth = stackSizes[0][0];
    int stackheight = stackSizes[0][1];
    int stackdepth = stackSizes[0][2];
    for (int i = 1; i < nstack; ++i) {
      Stack_Size_F(filepath[i], stackSizes[i]);
      if (!std::equal(stackSizes[0], stackSizes[0] + 3, stackSizes[i])) {
        m_commandOutputEdit->append(
          "<font color=red>For image concatenate, all the stack size must be same. Abort.</font>");

        for (int j = 0; j < nstack; ++j) {
          free(all_config[j]);
        }
        free(all_config);
        return;
      }
    }

    int* labels = new int[nstack];
    for (int i = 0; i < nstack; ++i)
      labels[i] = 0;
    labels[0] = 1;
    int nfinished = 1;
    while (nfinished != nstack) {
      for (int i = 0; i < nstack - 1; ++i) {
        for (int j = i + 1; j < nstack; ++j) {
          if (labels[i] == 1 && labels[j] == 0 && conn[i][j] == 1) {
            if (all_config[i][j] == nullptr) {
              m_commandOutputEdit->append(
                "<font color=red>Position information incomplete for image concatenate, Abort.</font>");
              FREE_2D_ARRAY(conn, nstack);
              for (int ii = 0; ii < nstack; ii++) {
                free(all_config[ii]);
              }
              free(all_config);
              delete[] labels;
              return;
            } else {
              final_offset[j][0] = final_offset[i][0] - all_config[i][j][0] * stackwidth;
              final_offset[j][1] = final_offset[i][1] - all_config[i][j][1] * stackheight;
              final_offset[j][2] = final_offset[i][2] - all_config[i][j][2] * stackdepth;
              labels[j] = 1;
              nfinished++;
            }
          }
        }
      }
    }
    delete[] labels;
  }
  else {
    /* number of possible pairs */
    int npair = nstack * (nstack + 1);

    /* allocate space for correlation scores */
    double* max_corr;
    max_corr = (double*) malloc(sizeof(double) * npair);
    double* unnorm_maxcorr;
    unnorm_maxcorr = (double*) malloc(sizeof(double) * npair);

    int** offset;

    int i;
    MALLOC_2D_ARRAY(offset, npair, 3, int, i);

    int** pairs;
    pairs = (int**) malloc(sizeof(int*) * npair);
    for (int i = 0; i < npair; ++i) {
      pairs[i] = (int*) malloc(2 * sizeof(int));
    }

    int idx = 0;

    Stack** downstacks;
    downstacks = (Stack**) malloc(sizeof(Stack * ) * nstack);

    Stack* stack1 = nullptr;
    Stack* stack2 = nullptr;

    int intv[3];

    intv[0] = m_intvXSpinBox->value();
    intv[1] = m_intvYSpinBox->value();
    intv[2] = m_intvZSpinBox->value();

    if (m_dsCheckBox->isChecked()) {
      intv[0] /= m_dsXSpinBox->value();
      intv[1] /= m_dsYSpinBox->value();
      intv[2] /= m_dsZSpinBox->value();
    }

    int channelIndex = m_channel1ComboBox->currentIndex();   //wrong
    int bgsubIndex = m_bgsub1ComboBox->currentIndex();

    m_commandOutputEdit->append("Load stacks ...");
    for (int i = 0; i < nstack; ++i) {
      m_commandOutputEdit->append(QString("Stack %1 ...").arg(i));

      stack1 = readStack(filepath[i], bgsubIndex, channelIndex);
      if (m_dsCheckBox->isChecked()) {
        m_commandOutputEdit->append("Downsampling ...");
        Downsample_Stack_Mean(stack1, m_dsXSpinBox->value() - 1,
                              m_dsYSpinBox->value() - 1,
                              m_dsZSpinBox->value() - 1,
                              stack1);
      }

      if (stack1->kind == COLOR) {
        Translate_Stack(stack1, GREY, 1);
      }

      stackSizes[i][0] = stack1->width;
      stackSizes[i][1] = stack1->height;
      stackSizes[i][2] = stack1->depth;

      if (bgsubIndex == 2) {    //After read
        Pixel_Range* pr = Stack_Range(stack1, 0);
        Stack_Sub_Common(stack1, 0, (int) ((pr->minval + pr->maxval) / 2));
      }

      m_commandOutputEdit->append("Downsample stack ...");
      if (stack1->depth > 3) {
        downstacks[i] = Downsample_Stack_Mean(stack1, intv[0], intv[1], intv[2],
                                              nullptr);
      } else { /* no z downsampling for thin stacks */
        intv[2] = 0;
        downstacks[i] = Downsample_Stack_Mean(stack1, intv[0], intv[1], intv[2],
                                              nullptr);
      }

      if (bgsubIndex == 3) {   //After downsample
        Pixel_Range* pr = Stack_Range(downstacks[i], 0);
        Stack_Sub_Common(downstacks[i], 0, (int) ((pr->minval + pr->maxval) / 2));
      }

      Kill_Stack(stack1);
      QApplication::processEvents();
    }
    Reset_Stack();


#ifdef _DEBUG_
    tic();
#endif

    /* rough estimation */
    for (int i = 0; i < nstack - 1; ++i) {
      for (int j = i + 1; j < nstack; ++j) {
        if ((conn == nullptr) || (conn[i][j] == 1)) {
          max_corr[idx] = Align_Stack_MR_D(downstacks[i], downstacks[j], intv,
                                           -1, all_config[i][j], offset[idx],
                                           unnorm_maxcorr + idx);
          max_corr[idx] = -max_corr[idx];

          pairs[idx][0] = i;
          pairs[idx][1] = j;

          m_commandOutputEdit->append(QString("(%1,%2) : (%3,%4,%5) : %6").
            arg(pairs[idx][0]).arg(pairs[idx][1]).
            arg(offset[idx][0]).arg(offset[idx][1]).
            arg(offset[idx][2]).arg(max_corr[idx]));
          QApplication::processEvents();

          idx++;
        }
      }
      Kill_Stack(downstacks[i]);
    }

    Kill_Stack(downstacks[nstack - 1]);
    free(downstacks);

    /* actual number of pairs */
    npair = idx;

    int* permidx;
    permidx = (int*) malloc(sizeof(int) * npair);
    for (int i = 0; i < npair; ++i) {
      permidx[i] = i;
    }
    int* labels;
    labels = (int*) malloc(sizeof(int) * nstack);
    for (int i = 0; i < nstack; ++i) {
      labels[i] = 0;
    }
    int** selpairs;
    selpairs = (int**) malloc(sizeof(int*) * (nstack - 1));
    for (int i = 0; i < nstack - 1; ++i)
      selpairs[i] = (int*) malloc(sizeof(int) * 2);

    int** seloffset;
    seloffset = (int**) malloc(sizeof(int*) * (nstack - 1));
    for (int i = 0; i < nstack - 1; ++i)
      seloffset[i] = (int*) malloc(sizeof(int) * 3);

    darray_qsort(max_corr, permidx, npair);
    free(max_corr);
    free(unnorm_maxcorr);

    idx = 0;
    labels[pairs[permidx[0]][0]] = 1;


    i = 0;
    while (idx < nstack - 1) {
      //one and only one idx has not been added
      if (labels[pairs[permidx[i]][0]] != labels[pairs[permidx[i]][1]]) {
        m_commandOutputEdit->append(QString("(%1,%2)").arg(pairs[permidx[i]][0]).arg(pairs[permidx[i]][1]));
        QApplication::processEvents();

        selpairs[idx][0] = pairs[permidx[i]][0];
        selpairs[idx][1] = pairs[permidx[i]][1];
        seloffset[idx][0] = offset[permidx[i]][0];
        seloffset[idx][1] = offset[permidx[i]][1];
        seloffset[idx][2] = offset[permidx[i]][2];

        int v1 = selpairs[idx][0];
        int v2 = selpairs[idx][1];

        stack1 = readStack(filepath[v1], 0, channelIndex);   // no background sub
        Translate_Stack(stack1, GREY, 1);
        stack2 = readStack(filepath[v2], 0, channelIndex);   // no background sub
        Translate_Stack(stack2, GREY, 1);
        if (m_dsCheckBox->isChecked()) {
          Downsample_Stack_Mean(stack1, m_dsXSpinBox->value() - 1,
                                m_dsYSpinBox->value() - 1,
                                m_dsZSpinBox->value() - 1,
                                stack1);
          Downsample_Stack_Mean(stack2, m_dsXSpinBox->value() - 1,
                                m_dsYSpinBox->value() - 1,
                                m_dsZSpinBox->value() - 1,
                                stack2);
        }

        Align_Stack_MR_D(stack1, stack2, intv, 2, nullptr, seloffset[idx], nullptr);

        if (labels[pairs[permidx[i]][0]] == 1) {
          final_offset[v2][0] =
            final_offset[v1][0] - seloffset[idx][0] + stack1->width - 1;
          final_offset[v2][1] =
            final_offset[v1][1] - seloffset[idx][1] + stack1->height - 1;
          final_offset[v2][2] =
            final_offset[v1][2] - seloffset[idx][2] + stack1->depth - 1;
        } else {
          final_offset[v1][0] =
            final_offset[v2][0] + seloffset[idx][0] - stack2->width + 1;
          final_offset[v1][1] =
            final_offset[v2][1] + seloffset[idx][1] - stack2->height + 1;
          final_offset[v1][2] =
            final_offset[v2][2] + seloffset[idx][2] - stack2->depth + 1;
        }

        Kill_Stack(stack1);
        Kill_Stack(stack2);

        labels[pairs[permidx[i]][0]] = 1;
        labels[pairs[permidx[i]][1]] = 1;
        idx++;
        i = 0;
      }
      ++i;
      qDebug() << "hasfw: " << i;
    }
    FREE_2D_ARRAY(pairs, npair);
    FREE_2D_ARRAY(offset, npair);
    FREE_2D_ARRAY(selpairs, nstack - 1);
    FREE_2D_ARRAY(seloffset, nstack - 1);
    //Kill_Stack(stack1);
    //Kill_Stack(stack2);
    free(permidx);
    free(labels);

#ifdef _DEBUG_
    qDebug() << "Time passed: " << toc();
#endif

    Reset_Stack();
  }
  if (conn != nullptr) {
    FREE_2D_ARRAY(conn, nstack);
    conn = nullptr;
  }
  for (int i = 0; i < nstack; ++i) {
    for (int j = 0; j < nstack; ++j) {
      free(all_config[i][j]);
    }
  }
  for (int i = 0; i < nstack; ++i) {
    free(all_config[i]);
  }
  free(all_config);
  all_config = nullptr;


  for (int i = 0; i < nstack; ++i) {
    m_commandOutputEdit->append(filepath[i]);
    m_commandOutputEdit->append(QString("(%1,%2,%3) (%4,%5,%6)").
        arg(final_offset[i][0]).arg(final_offset[i][1]).arg(final_offset[i][2])
                                  .arg(stackSizes[i][0]).arg(stackSizes[i][1]).arg(stackSizes[i][2]));
  }

  if (fhasext(outputba.data(), "txt")) {
    FILE* fp = GUARDED_FOPEN(outputba.data(), "w");
    for (int i = 0; i < nstack; ++i) {
      fprintf(fp, "%s ", filepath[i]);
      fprintf(fp, "(%d,%d,%d) (%d,%d,%d)\n",
              final_offset[i][0], final_offset[i][1], final_offset[i][2],
              stackSizes[i][0], stackSizes[i][1], stackSizes[i][2]);
    }
    fclose(fp);
  } else {
    int merge_mode = m_mergeMode1ComboBox->currentIndex() + 1;

    bool large_stack = true;
    Mc_Stack* new_stack = nullptr;

//    //????????????????????????????
//    for (int i = 0; i < nstack; ++i) {
//      if (m_dsCheckBox->isChecked()) {
//        int ds[3];
//        ds[0] = m_dsXSpinBox->value();
//        ds[1] = m_dsYSpinBox->value();
//        ds[2] = m_dsZSpinBox->value();
//        for (int j = 0; j < 3; ++j) {
//          if (final_offset[i][j] < 0) {
//            final_offset[i][j]--;
//          }

//          final_offset[i][j] /= ds[j];
//        }
//      }
//      m_commandOutputEdit->append(QString("(%1,%2,%3)").arg(final_offset[i][0]).arg(final_offset[i][1]).arg(final_offset[i][2]));
//      QApplication::processEvents();
//    }

    if (large_stack == false) {
      Mc_Stack** stacks = (Mc_Stack**) malloc(sizeof(Mc_Stack * ) * nstack);

      for (int i = 0; i < nstack; ++i) {
        stacks[i] = Read_Mc_Stack(filepath[i], -1);
        if (m_dsCheckBox->isChecked()) {
          Mc_Stack_Downsample_Mean(stacks[i], m_dsXSpinBox->value() - 1, m_dsYSpinBox->value() - 1,
                                   m_dsZSpinBox->value() - 1,
                                   stacks[i]);
        }
      }

      new_stack = Mc_Stack_Merge(stacks, nstack, final_offset,
                                 merge_mode);

      for (int i = 0; i < nstack; ++i) {
        Kill_Mc_Stack(stacks[i]);
      }
      free(stacks);
    } else {
      if (m_dsCheckBox->isChecked()) {
        int ds[3];
        ds[0] = m_dsXSpinBox->value();
        ds[1] = m_dsYSpinBox->value();
        ds[2] = m_dsZSpinBox->value();

        new_stack = Mc_Stack_Merge_F(filepath, nstack, final_offset,
                                     merge_mode, ds);
      } else {
        new_stack = Mc_Stack_Merge_F(filepath, nstack, final_offset,
                                     merge_mode, nullptr);
      }
    }

    if (m_d8CheckBox->isChecked()) {
      if (new_stack->kind == 2) {
        Mc_Stack_Grey16_To_8(new_stack, m_d8ComboBox->currentIndex());
      }
    }

    //Write_Mc_Stack(outputba.data(), new_stack, filepath[0]);
    QString outname = save(m_outputFileEdit->text(), new_stack);

    Kill_Mc_Stack(new_stack);
    m_commandOutputEdit->append(QString("%1 saved.").arg(outname));
    if (m_useTileImageRadioButton->isChecked() && m_tileImage != nullptr) {
      QString selectionImageOutputName = m_outputFileEdit->text();
      selectionImageOutputName.append("_TileSelectionInfo.tif");
      QImage image(*m_tileImage);

      QPainter painter(&image);
      for (int i = 0; i < m_tileList.size(); ++i) {
        QRect rect = QRect(m_tileList.at(i).region.topLeft(),
                           m_tileList.at(i).region.bottomRight());
        if (m_tileList.at(i).bIsSelected) {
          painter.fillRect(rect, QColor(255, 255, 0, 128));
        }
        QString str = QString("Image %1").arg(i + 1);
        painter.drawText(rect, str);
      }
      QImageWriter writer(selectionImageOutputName);
      if (!writer.write(image)) {
        m_commandOutputEdit->append(writer.errorString());
      } else {
        m_commandOutputEdit->append(QString("%1 saved.").arg(selectionImageOutputName));
      }

    }
  }
  // cleanup
  if (m_useLayoutRadioButton->isChecked() && m_inputStack1Filenames.size() == 1 &&
      m_layout1SpinBox->value() * m_layout2SpinBox->value() != 1) {
    for (int i = 0; i < nstack; ++i) {
      free(filepath[i]);
    }
    free(filepath);
    filepath = nullptr;
  } else {
    free(filepath);   //filepath do not own the memory
    filepath = nullptr;
  }
  FREE_2D_ARRAY(final_offset, nstack);
  for (int i = 0; i < nstack; ++i) {
    free(stackSizes[i]);
  }
  free(stackSizes);
  stackSizes = nullptr;
}

char** ZStitchImageDialog::splitstack(Mc_Stack* stack, const char* filepath, int nstack)
{
  Mc_Stack* tmpstack = Make_Mc_Stack(stack->kind, stack->width, stack->height,
                                     stack->depth / nstack, stack->nchannel);

  size_t channel_size = stack->kind * stack->width * stack->height
                        * stack->depth;
  size_t channel_size2 = tmpstack->kind * tmpstack->width * tmpstack->height
                         * tmpstack->depth;

  const char* prefix = "neurolabi_stitch";
  QFileInfo inputFile(filepath);
  QDir fileDir = inputFile.dir();
  if (!fileDir.cd("tmp")) {  //create tmp folder
    if (!fileDir.mkdir("tmp")) {
      m_commandOutputEdit->append(
        QString("<font color=red>Can not create folder: %1. Abort.</font>").arg(fileDir.absolutePath() + "/tmp"));
      return nullptr;
    }
  } else {
    fileDir = inputFile.dir();
  }
  QString folder = fileDir.path() + QDir::separator() + "tmp";
  //char cmd[500];
  //sprintf(cmd, "rm tmp/%s*.lsm", prefix);
  //system(cmd);

  char** outpath = (char**) malloc(sizeof(char*) * nstack);

  int i;
  int k;
  uint8_t* array = stack->array;
  char filename[100];
  fname(filepath, filename);
  for (k = 0; k < nstack; ++k) {
    int offset = 0;
    int offset2 = 0;
    for (i = 0; i < stack->nchannel; ++i) {
      memcpy(tmpstack->array + offset2, array + offset, channel_size2);
      offset += channel_size;
      offset2 += channel_size2;
    }
    array += channel_size2;

    outpath[k] = (char*) malloc(sizeof(char) * 500);

    //sprintf(outpath[k], "%s/%s_%s_%03d.lsm", folder.toLocal8Bit().data(), prefix, filename, k);
    sprintf(outpath[k], "%s/%s_%s_%03d.tif", folder.toLocal8Bit().data(), prefix, filename, k);
    Write_Mc_Stack(outpath[k], tmpstack, filepath);
  }
  Kill_Mc_Stack(tmpstack);

  return outpath;
}

Stack* ZStitchImageDialog::readStack(char* filepath, int bgsubIndex, int channelIndex)
{
  if (channelIndex > 1) {   // read only one channel
    Stack* stack = Read_Sc_Stack(filepath, channelIndex - 2);
    if (bgsubIndex == 1 || bgsubIndex - 4 == channelIndex - 2) { //need remove background
      Pixel_Range* pr = Stack_Range(stack, 0);
      Stack_Sub_Common(stack, 0, (int) ((pr->minval + pr->maxval) / 2));
    }
    return stack;
  } else if (m_channel1ComboBox->count() == 3) {   //has only one channel
    Stack* stack = Read_Sc_Stack(filepath, 0);
    if (bgsubIndex == 1 || bgsubIndex - 4 == 0) {  //need remove background
      Pixel_Range* pr = Stack_Range(stack, 0);
      Stack_Sub_Common(stack, 0, (int) ((pr->minval + pr->maxval) / 2));
    }
    return stack;
  } else { //read all channels
    ZStack* zstack = new ZStack();
    zstack->load(filepath);
    //Mc_Stack *mc_stack = Read_Mc_Stack(filepath, -1);

    if (channelIndex == 1) { /* exclude blue channel */  // use first two channels
      while (zstack->channelNumber() > 2) {
        zstack->removeChannel(2);
      }
    }

    int mask[zstack->channelNumber()];
    for (int i = 0; i < zstack->channelNumber(); ++i) {
      if (bgsubIndex == 1) {  // remove background for all channels
        mask[i] = 1;
      } else {
        mask[i] = 0;
      }
    }

    if (bgsubIndex > 3) {
      mask[bgsubIndex - 4] = 1;
    }

    if ((bgsubIndex == 1) || (bgsubIndex > 3)) {
      for (int i = 0; i < zstack->channelNumber(); ++i) {
        if (mask[i] == 1) {
          Stack* channel = zstack->c_stack(i);
          Pixel_Range* pr = Stack_Range(channel, 0);
          Stack_Sub_Common(channel, 0, (int) ((pr->minval + pr->maxval) / 2));
        }
      }
    }

    //Stack *stack = Mc_Stack_To_Stack(mc_stack, -1, nullptr);
    Stack* stack = zstack->averageOfAllChannels();
    delete zstack;

    /* It seems there is no good way to reuse the memory block in Mc_Stack
     * due to Gene's memory management */
    //Kill_Mc_Stack(mc_stack);

    return stack;
  }
}

int ZStitchImageDialog::load_align(const char* filepath, char*** stack_file, int*** offset)
{
  FILE* fp = fopen(filepath, "r");

  String_Workspace* sw = New_String_Workspace();

  char* line = nullptr;

  int nstack = 0;
  while ((line = Read_Line(fp, sw)) != nullptr) {
    if (Is_Space(line) == FALSE) {
      nstack++;
    }
  }

  if (nstack > 0) {
    GUARDED_MALLOC_ARRAY(*stack_file, nstack, char * );
    GUARDED_MALLOC_ARRAY(*offset, nstack, int * );

    fseek(fp, 0, SEEK_SET);
    int i = 0;
    while ((line = Read_Line(fp, sw)) != nullptr) {
      if (Is_Space(line) == FALSE) {
        GUARDED_MALLOC_ARRAY((*stack_file)[i], strlen(line) + 1, char);
        strcpy((*stack_file)[i], line);
        strrmspc((*stack_file)[i]);
        strsplit((*stack_file)[i], '(', 1);
        int n;
        (*offset)[i] = String_To_Integer_Array(strsplit(line, '(', 1), nullptr,
                                               &n);
        ++i;
      }
    }
  }

  Kill_String_Workspace(sw);

  fclose(fp);

  return nstack;
}

int** ZStitchImageDialog::load_conn(char* filepath, int*** config)
{
  int ntile;
  FILE* fp = fopen(filepath, "r");

  if (fp == nullptr) {
    fprintf(stderr, "Cannot open the connection file.\n");
    return nullptr;
  }

  if (fscanf(fp, "%d", &ntile) != 1) {
    return nullptr;
  }

  int** conn;
  int i, j;
  MALLOC_2D_ARRAY(conn, ntile, ntile, int, i);
  for (i = 0; i < ntile; ++i) {
    for (j = 0; j < ntile; ++j) {
      conn[i][j] = 0;
    }
  }

  String_Workspace* sw = New_String_Workspace();

  char* line = nullptr;
  while ((line = Read_Line(fp, sw)) != nullptr) {
    int n;
    int array[5];
    int idx1, idx2;
    String_To_Integer_Array(line, array, &n);
    if (n >= 2) {
      idx1 = array[0];
      idx2 = array[1];

      if (IS_IN_CLOSE_RANGE(idx1, 1, ntile) &&
          IS_IN_CLOSE_RANGE(idx2, 1, ntile)) {
        idx1--;
        idx2--;
        conn[idx1][idx2] = 1;
        conn[idx2][idx1] = 1;

        if (n >= 5) {
          config[idx1][idx2] = iarray_calloc(3);
          config[idx1][idx2][0] = array[2];
          config[idx1][idx2][1] = array[3];
          config[idx1][idx2][2] = array[4];

          config[idx2][idx1] = iarray_calloc(3);
          config[idx2][idx1][0] = -array[2];
          config[idx2][idx1][1] = -array[3];
          config[idx2][idx1][2] = -array[4];
        }
      }
    }
  }

  Kill_String_Workspace(sw);

  /*
      int idx1, idx2;
      while (fscanf(fp, "%d %d", &idx1,  &idx2) == 2) {
      if (IS_IN_CLOSE_RANGE(idx1, 1, ntile) &&
      IS_IN_CLOSE_RANGE(idx2, 1, ntile)) {
      conn[idx1 - 1][idx2 - 1] = 1;
      conn[idx2 - 1][idx1 - 1] = 1;
      } else {
      FREE_2D_ARRAY(conn, ntile);
      break;
      }
      }
    */

  fclose(fp);

  if (conn != nullptr) { /* test connectivity */
    if (Is_Adjmat_Connected(conn, ntile) == 0) {
      fprintf(stderr, "Tiles are not fully connected.\n");
      FREE_2D_ARRAY(conn, ntile);
    }
  }

  return conn;
}

