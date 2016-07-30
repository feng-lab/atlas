#include "zstitchimagedialog.h"

#include <QtGui>
#include <QtWidgets>
#include <QtAlgorithms>

#include "zimg.h"
#include "zimgio.h"
#include "zimgdisplay.h"
#include "zimgnccmatch.h"
#include "zimgmerge.h"
#include "zstringutils.h"
#include "zsysteminfo.h"
#include "zlog.h"

namespace {

using namespace nim;

class ZStitchException
{
public:
  explicit ZStitchException(const QString& what)
  {
    m_what = what;
  }

  ~ZStitchException() throw()
  {}

  QString what() const throw()
  { return m_what; }

protected:
  QString m_what;
};

void buildConnectionFromGrid(const std::vector<std::vector<size_t>>& grid,
                             std::map<std::pair<size_t, size_t>, ZImgNCCMatch::PositionHint>& conn)
{
  for (size_t i = 0; i < grid.size(); i++) {
    for (size_t j = 0; j < grid[i].size(); j++) {
      if (grid[i][j] > 0) {
        bool connected = false;
        if (j + 1 < grid[0].size() && grid[i][j + 1] > 0) { //right
          size_t idx1 = grid[i][j] - 1;
          size_t idx2 = grid[i][j + 1] - 1;
          std::pair<size_t, size_t> stackPair = std::make_pair(idx1, idx2);
          conn[stackPair] = ZImgNCCMatch::Right;
          connected = true;
        }
        if (i + 1 < grid.size() && grid[i + 1][j] > 0) { //down
          size_t idx1 = grid[i][j] - 1;
          size_t idx2 = grid[i + 1][j] - 1;
          std::pair<size_t, size_t> stackPair = std::make_pair(idx1, idx2);
          conn[stackPair] = ZImgNCCMatch::Down;
          connected = true;
        }
        if (i + 1 < grid.size() && j + 1 < grid[0].size() && grid[i + 1][j + 1] > 0
            && connected == false) {  // down-right, add only if right and down are empty
          size_t idx1 = grid[i][j] - 1;
          size_t idx2 = grid[i + 1][j + 1] - 1;
          std::pair<size_t, size_t> stackPair = std::make_pair(idx1, idx2);
          conn[stackPair] = ZImgNCCMatch::Down | ZImgNCCMatch::Right;
          connected = true;
        }
        if (i + 1 < grid.size() && j >= 1 && grid[i + 1][j - 1] > 0
            && grid[i][j - 1] == 0 && grid[i + 1][j] == 0) {  // down-left, add only if left and down are empty
          size_t idx1 = grid[i][j] - 1;
          size_t idx2 = grid[i + 1][j - 1] - 1;
          std::pair<size_t, size_t> stackPair = std::make_pair(idx1, idx2);
          conn[stackPair] = ZImgNCCMatch::Down | ZImgNCCMatch::Left;
          connected = true;
        }
        if (connected == false) {  // test if this image is connected
          if (i >= 1 && j >= 1 && grid[i - 1][j - 1] > 0) {  // up-left
            size_t idx1 = grid[i][j] - 1;
            size_t idx2 = grid[i - 1][j - 1] - 1;
            if (conn.find(std::make_pair(idx2, idx1)) != conn.end())
              connected = true;
          }
          if (j >= 1 && grid[i][j - 1] > 0) { // left
            size_t idx1 = grid[i][j] - 1;
            size_t idx2 = grid[i][j - 1] - 1;
            if (conn.find(std::make_pair(idx2, idx1)) != conn.end())
              connected = true;
          }
          if (i >= 1 && grid[i - 1][j] > 0) { //up
            size_t idx1 = grid[i][j] - 1;
            size_t idx2 = grid[i - 1][j] - 1;
            if (conn.find(std::make_pair(idx2, idx1)) != conn.end())
              connected = true;
          }
          if (i >= 1 && j + 1 < grid[0].size() && grid[i - 1][j + 1] > 0) {  // up-right
            size_t idx1 = grid[i][j] - 1;
            size_t idx2 = grid[i - 1][j + 1] - 1;
            if (conn.find(std::make_pair(idx2, idx1)) != conn.end())
              connected = true;
          }
        }
        if (connected == false) {
          throw ZStitchException(QString("Can not stitch because images are not connected. Abort."));
        }
      }
    }
  }
}

}

namespace nim {

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
  //for (int i=0; i<m_filenames.size(); ++i)
  //m_tileimages.push_back(QImage(m_filenames[i]));
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
  if (m_image) {
    QPainter painter(this);

    QSize size = m_scaleFactor * m_image->size();
    painter.drawImage(QRectF(0, 0, size.width(), size.height()), *m_image,
                      QRectF(0, 0, m_image->size().width(), m_image->size().height()));

    if (m_tileimages.size() == m_pTiles->size()) {


      for (int i = 0; i < m_pTiles->size(); i++) {
        //        QRect rect = QRect(m_pTiles->at(i).region.topLeft() * m_scaleFactor,
        //                           m_pTiles->at(i).region.bottomRight() * m_scaleFactor);
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
      for (int i = 0; i < m_pTiles->size(); i++) {
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
  for (int i = 0; i < m_pTiles->size(); i++) {
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
  for (int i = 0; i < m_pTiles->size(); i++) {
    (*m_pTiles)[i].bIsSelected = false;
  }
  update();
}

void ZTileImageWidget::selectAll()
{
  for (int i = 0; i < m_pTiles->size(); i++) {
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
  connect(m_exitButton, &QPushButton::clicked, this, &ZStitchImageDialog::reject);
  connect(m_runButton, &QPushButton::clicked, this, &ZStitchImageDialog::stitchStacks);

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
  connect(m_selectInputStacks1Button, &QPushButton::clicked, this, &ZStitchImageDialog::selectInputStacks1);
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
  m_bgsub1ComboBox->setCurrentIndex(0);      //default do not remove background
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
  m_mergeMode1ComboBox->addItem(tr("Median"));
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
  connect(m_selectInputStacks2Button, &QPushButton::clicked, this, &ZStitchImageDialog::selectInputStacks2);
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
  m_bgsub2ComboBox->setCurrentIndex(0);      //default do not remove background
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
  m_mergeMode2ComboBox->addItem(tr("Median"));
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

  pl = new QLabel(tr("Output File:"), this);
  m_outputFileEdit = new QLineEdit(this);
  m_selectOutputButton = new QToolButton(this);
  m_selectOutputButton->setText(tr("..."));
  connect(m_selectOutputButton, &QToolButton::clicked, this, &ZStitchImageDialog::selectOutputFile);
  layout->addWidget(pl, row, 0);
  layout->addWidget(m_outputFileEdit, row, 1, 1, 6);
  layout->addWidget(m_selectOutputButton, row, 7);
  row++;

  m_hasTwoInputStackSetCheckBox = new QCheckBox(tr("has two input stack sets"), this);
  m_hasTwoInputStackSetCheckBox->setToolTip(tr("stitch two stack sets with common channel"));
  connect(m_hasTwoInputStackSetCheckBox, &QCheckBox::stateChanged, this,
          &ZStitchImageDialog::hasTwoInputStackSetCheckBoxChanged);
  layout->addWidget(m_hasTwoInputStackSetCheckBox, row, 0);
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
  m_dsXSpinBox->setValue(1);
  m_dsYSpinBox = new QSpinBox(this);
  m_dsYSpinBox->setRange(1, 10);
  m_dsYSpinBox->setValue(1);
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

  m_d8CheckBox = new QCheckBox(tr("convert result to 8bit"), this);
  m_d8CheckBox->setToolTip(tr("d8"));
  connect(m_d8CheckBox, &QCheckBox::stateChanged, this, &ZStitchImageDialog::d8CheckBoxChanged);
  layout->addWidget(m_d8CheckBox, row, 0);
  m_d8ComboBox = new QComboBox(this);
  m_d8ComboBox->addItem(tr("[min, max] -> [0, 255]"));
  m_d8ComboBox->addItem(tr("[min, q(99.99)] -> [0, 255]"));
  m_d8ComboBox->addItem(tr("equal info map"));
  connect(m_d8ComboBox, qOverload<int>(&QComboBox::activated), this, &ZStitchImageDialog::d8Changed);
  layout->addWidget(m_d8ComboBox, row, 2, 1, 4);
  m_d8ComboBox->setEnabled(false);
  row++;

  alllayout->addLayout(layout);

  hasTwoInputStackSetCheckBoxChanged(Qt::Unchecked);
  return alllayout;
}

QLayout* ZStitchImageDialog::createConnLayout()
{
  QVBoxLayout* layout = new QVBoxLayout;
  QHBoxLayout* hlayout = new QHBoxLayout;

  hlayout->addWidget(new QLabel("Max Overlap Rate: "));
  m_overlapRateSpinBox = new QSpinBox();
  m_overlapRateSpinBox->setMinimum(1);
  m_overlapRateSpinBox->setMaximum(100);
  m_overlapRateSpinBox->setSuffix("%");
  m_overlapRateSpinBox->setSingleStep(1);
  m_overlapRateSpinBox->setValue(10);
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
    ZSystemInfoInstance.lastOpenedImagePath(),
    tr("Image Files (*.lsm *.tif *.raw)"));
  if (tmp.count()) {
    ZSystemInfoInstance.setLastOpenedImagePath(tmp[0]);
    try {
      // test image
      ZImgInfo info = ZImg::readImgInfo(tmp[0]).at(0);

      int nchannel = info.numChannels;
      m_nchannelStack1 = nchannel;
      setStack1ChRange();
      m_commonChannel1SpinBox->setRange(1, nchannel);
      initChannel1ComboBox(nchannel);
      initBgsub1ComboBox(nchannel);
      m_inputStack1Filenames.clear();
      m_inputStack1Filenames = tmp;
      qSort(m_inputStack1Filenames.begin(), m_inputStack1Filenames.end(), naturalSortLessThan);
      m_inputStack1FileEdit->setText(QString("%1").arg(m_inputStack1Filenames.join("\n")));
    } catch (const ZException& e) {
      QMessageBox::critical(this, tr("Can not read image"), e.what());
      return;
    }
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
    ZSystemInfoInstance.lastOpenedImagePath(),
    tr("Image Files (*.lsm *.tif *.raw)"));
  if (tmp.count()) {
    ZSystemInfoInstance.setLastOpenedImagePath(tmp[0]);
    try {
      // test image
      ZImgInfo info = ZImg::readImgInfo(tmp[0]).at(0);

      int nchannel = info.numChannels;
      m_nchannelStack2 = nchannel;
      setStack2ChRange();
      m_commonChannel2SpinBox->setRange(1, nchannel);
      initChannel2ComboBox(nchannel);
      initBgsub2ComboBox(nchannel);
      m_inputStack2Filenames.clear();
      m_inputStack2Filenames = tmp;
      qSort(m_inputStack2Filenames.begin(), m_inputStack2Filenames.end(), naturalSortLessThan);
      m_inputStack2FileEdit->setText(QString("%1").arg(m_inputStack2Filenames.join("\n")));
    } catch (const ZException& e) {
      QMessageBox::critical(this, tr("Can not read image"), e.what());
      return;
    }
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

bool ZStitchImageDialog::getTileMatrix(ZImg& img, QVector<QVector<int>>& tileMatrix,
                                       QList<ZTile>& tileList)
{
  double minvalue;
  double maxvalue;
  img.createView(0, 0).computeMinMax(minvalue, maxvalue);
  double midvalue = (minvalue + maxvalue) / 2;
  double thre1 = (minvalue + midvalue) / 2;
  double thre2 = (midvalue + maxvalue) / 2;
  int numTilePerRow = 0;
  int numTilePerCol = 0;
  tileMatrix.clear();
  tileList.clear();
  for (size_t h = 0; h < img.height(); h++) {
    int pre = minvalue;
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
    int pre = minvalue;
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
  tileMatrix = QVector<QVector<int>>(numTilePerCol, QVector<int>(numTilePerRow, 0));
  int tileindex = 1;
  int tileindex2 = 1;
  int currentrow = 0;
  int currentcol = 0;
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
  if (!m_tileImage.isNull()) {
    QList<ZTile> tmpList(m_tileList);
    QDialog dia;
    m_scrollArea = new QScrollArea(this);
    m_tileImageWidget = new ZTileImageWidget(this, &m_tileImage, &tmpList, m_inputStack1Filenames);
    m_scrollArea->setWidget(m_tileImageWidget);
    m_scrollArea->ensureWidgetVisible(m_tileImageWidget);
    QVBoxLayout* vlayout = new QVBoxLayout;
    QHBoxLayout* hlayout = new QHBoxLayout;
    QPushButton* zoomInButton = new QPushButton(tr("zoom in"), this);
    connect(zoomInButton, &QPushButton::clicked, this, &ZStitchImageDialog::zoomInTileImageWidget);
    hlayout->addWidget(zoomInButton);
    QPushButton* zoomOutButton = new QPushButton(tr("zoom out"), this);
    connect(zoomOutButton, &QPushButton::clicked, this, &ZStitchImageDialog::zoomOutTileImageWidget);
    hlayout->addWidget(zoomOutButton);
    QPushButton* clearAllButton = new QPushButton(tr("clear all selected"), this);
    connect(clearAllButton, &QPushButton::clicked, this, &ZStitchImageDialog::clearAllSelectedInTileImageWidget);
    hlayout->addWidget(clearAllButton);
    QPushButton* selectAllButton = new QPushButton(tr("select all"), this);
    connect(selectAllButton, &QPushButton::clicked, this, &ZStitchImageDialog::selectAllInTileImageWidget);
    hlayout->addWidget(selectAllButton);
    QPushButton* saveButton = new QPushButton(tr("save"), this);
    connect(saveButton, &QPushButton::clicked, this, &ZStitchImageDialog::saveTileImageWidgetAsImage);
    hlayout->addWidget(saveButton);

    vlayout->addLayout(hlayout);
    vlayout->addWidget(m_scrollArea);

    QDialogButtonBox* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok
                                                       | QDialogButtonBox::Cancel, Qt::Horizontal, this);

    connect(buttonBox, &QDialogButtonBox::accepted, &dia, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, &dia, &QDialog::reject);
    vlayout->addWidget(buttonBox);

    dia.setLayout(vlayout);
    dia.resize(m_tileImage.width(), m_tileImage.height());
    if (dia.exec() == QDialog::Accepted) {
      m_tileList = tmpList;
      m_nSel = 0;
      for (int i = 0; i < m_tileList.size(); i++) {
        if (m_tileList[i].bIsSelected)
          m_nSel++;
      }
      QString str = QString("%1 images selected to stitch:").arg(m_nSel);
      m_connEdit->setText(str);
      for (int i = 0; i < m_tileMatrix.size(); i++) {
        str = QString("  ");
        for (int j = 0; j < m_tileMatrix[0].size(); j++) {
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
                                                 ZSystemInfoInstance.lastOpenedImagePath(),
                                                 tr("Tile Select Image (*.lsm *.tif)"));
  if (!tmpName.isEmpty()) {
    ZSystemInfoInstance.setLastOpenedImagePath(tmpName);
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
        QMessageBox::warning(this, tr("Failed"), tr("Failed to parse tile connection image."));
      }
    } catch (const ZException& e) {
      QMessageBox::warning(this, tr("read image failed"), e.what());
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
  QString fn = QFileDialog::getSaveFileName(this, "save tiles image");
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

void ZStitchImageDialog::initChannel1ComboBox(int nchannel)
{
  m_channel1ComboBox->setCurrentIndex(0);    //default value
  while (m_channel1ComboBox->count() > 2) {
    m_channel1ComboBox->removeItem(2);
  }
  for (int i = 0; i < nchannel; i++) {
    m_channel1ComboBox->addItem(QString("Ch%1").arg(i + 1));
  }
}

void ZStitchImageDialog::initBgsub1ComboBox(int nchannel)
{
  m_bgsub1ComboBox->setCurrentIndex(0);  //default value
  while (m_bgsub1ComboBox->count() > 2) {
    m_bgsub1ComboBox->removeItem(2);
  }
  for (int i = 0; i < nchannel; i++) {
    m_bgsub1ComboBox->addItem(QString("Ch%1").arg(i + 1));
  }
}

void ZStitchImageDialog::initChannel2ComboBox(int nchannel)
{
  m_channel2ComboBox->setCurrentIndex(0);    //default value
  while (m_channel2ComboBox->count() > 2) {
    m_channel2ComboBox->removeItem(2);
  }
  for (int i = 0; i < nchannel; i++) {
    m_channel2ComboBox->addItem(QString("Ch%1").arg(i + 1));
  }
}

void ZStitchImageDialog::initBgsub2ComboBox(int nchannel)
{
  m_bgsub2ComboBox->setCurrentIndex(0);  //default value
  while (m_bgsub2ComboBox->count() > 2) {
    m_bgsub2ComboBox->removeItem(2);
  }
  for (int i = 0; i < nchannel; i++) {
    m_bgsub2ComboBox->addItem(QString("Ch%1").arg(i + 1));
  }
}

void ZStitchImageDialog::setStack1ChRange()
{
  //  if (m_outputCh1ImageComboBox->currentIndex() == 1) {
  //    m_outputCh1ImageChannelSpinBox->setRange(1, m_nchannelStack1);
  //  }
  //  if (m_outputCh2ImageComboBox->currentIndex() == 1) {
  //    m_outputCh2ImageChannelSpinBox->setRange(1, m_nchannelStack1);
  //  }
  //  if (m_outputCh3ImageComboBox->currentIndex() == 1) {
  //    m_outputCh3ImageChannelSpinBox->setRange(1, m_nchannelStack1);
  //  }
}

void ZStitchImageDialog::setStack2ChRange()
{
  //  if (m_outputCh1ImageComboBox->currentIndex() == 2) {
  //    m_outputCh1ImageChannelSpinBox->setRange(1, m_nchannelStack2);
  //  }
  //  if (m_outputCh2ImageComboBox->currentIndex() == 2) {
  //    m_outputCh2ImageChannelSpinBox->setRange(1, m_nchannelStack2);
  //  }
  //  if (m_outputCh3ImageComboBox->currentIndex() == 2) {
  //    m_outputCh3ImageChannelSpinBox->setRange(1, m_nchannelStack2);
  //  }
}

void ZStitchImageDialog::stitchStacks2()
{
  if (m_inputStack2Filenames.size() != m_inputStack1Filenames.size()) {
    throw ZStitchException(QString("The number of input files of stack 2 is not equal to input 1, Abort."));
  }
  if (m_inputStack2Filenames.size() < 1) {
    throw ZStitchException(QString("Please add input files."));
  }

  std::vector<ZImgInfo> stack1File1Infos = ZImg::readImgInfo(m_inputStack1Filenames[0]);
  for (size_t s = 1; s < stack1File1Infos.size(); ++s) {
    if (!stack1File1Infos[s].isSameType(stack1File1Infos[0])) {
      throw ZStitchException(QString("Image type of %1 scene 0 <%2> and scene %3 <%4> don't match")
                               .arg(m_inputStack1Filenames[0]).arg(stack1File1Infos[0].toQString())
                               .arg(s).arg(stack1File1Infos[s].toQString()));
    }
  }
  for (int i = 1; i < m_inputStack1Filenames.size(); ++i) {
    std::vector<ZImgInfo> tmpInfos = ZImg::readImgInfo(m_inputStack1Filenames[i]);
    for (size_t s = 0; s < tmpInfos.size(); ++s) {
      if (!tmpInfos[s].isSameType(stack1File1Infos[0])) {
        throw ZStitchException(QString("Image type of %1 <%2> and %3 <%4> don't match")
                                 .arg(m_inputStack1Filenames[0]).arg(stack1File1Infos[0].toQString())
                                 .arg(m_inputStack1Filenames[i]).arg(tmpInfos[s].toQString()));
      }
    }
  }
  std::vector<ZImgInfo> stack2File1Infos;
  for (int i = 0; i < m_inputStack2Filenames.size(); ++i) {
    std::vector<ZImgInfo> tmpInfos = ZImg::readImgInfo(m_inputStack2Filenames[i]);
    for (size_t s = 0; s < tmpInfos.size(); ++s) {
      if (!tmpInfos[s].isSameType(stack1File1Infos[0])) {
        throw ZStitchException(QString("Image type of %1 <%2> and %3 <%4> don't match")
                                 .arg(m_inputStack1Filenames[0]).arg(stack1File1Infos[0].toQString())
                                 .arg(m_inputStack2Filenames[i]).arg(tmpInfos[s].toQString()));
      }
    }
    if (i == 0)
      stack2File1Infos = tmpInfos;
  }

  size_t nstack;
  QList<ZImgSource> inputStack1Sources;
  QList<ZImgSource> inputStack2Sources;

  if (m_inputStack1Filenames.size() == 1 && m_useTileImageRadioButton->isChecked() && m_tileList.size() > 1) {
    // split input into m_tileList.size() tiles
    m_commandOutputEdit->append("Splitting image ...>");
    QApplication::processEvents();
    nstack = m_tileList.size();
    if (stack1File1Infos.size() != nstack && stack1File1Infos[0].numTimes != nstack)
      throw ZStitchException(QString("Can not split image %1. Abort.").arg(m_inputStack1Filenames[0]));
    if (stack2File1Infos.size() != nstack && stack2File1Infos[0].numTimes != nstack)
      throw ZStitchException(QString("Can not split image %1. Abort.").arg(m_inputStack2Filenames[0]));

    if (stack1File1Infos.size() == nstack) {
      for (size_t i = 0; i < nstack; i++) {
        if (m_tileList[i].bIsSelected) {
          inputStack1Sources.push_back(ZImgSource(m_inputStack1Filenames[0], ZImgRegion(), i));
        }
      }
    } else {
      for (size_t i = 0; i < nstack; i++) {
        if (m_tileList[i].bIsSelected) {
          ZImgRegion rgn;
          rgn.start.t = i;
          rgn.end.t = i + 1;
          inputStack1Sources.push_back(ZImgSource(m_inputStack1Filenames[0], rgn, 0));
        }
      }
    }
    if (stack2File1Infos.size() == nstack) {
      for (size_t i = 0; i < nstack; i++) {
        if (m_tileList[i].bIsSelected) {
          inputStack2Sources.push_back(ZImgSource(m_inputStack2Filenames[0], ZImgRegion(), i));
        }
      }
    } else {
      for (size_t i = 0; i < nstack; i++) {
        if (m_tileList[i].bIsSelected) {
          ZImgRegion rgn;
          rgn.start.t = i;
          rgn.end.t = i + 1;
          inputStack2Sources.push_back(ZImgSource(m_inputStack2Filenames[0], rgn, 0));
        }
      }
    }
    nstack = m_nSel;
  } else if (m_inputStack1Filenames.size() == 1 || m_useLayoutRadioButton->isChecked()) {
    nstack = m_inputStack1Filenames.size();
    for (int i = 0; i < m_inputStack1Filenames.size(); i++) {
      inputStack1Sources.push_back(m_inputStack1Filenames[i]);
      inputStack2Sources.push_back(m_inputStack2Filenames[i]);
    }
  } else {
    if (m_useTileImageRadioButton->isChecked()) {
      if (m_nSel >= 0) {
        // first check number of input stacks and selected stacks
        m_commandOutputEdit->setText(tr("checking file numbers..."));
        if (m_inputStack1Filenames.size() != m_nSel && m_inputStack1Filenames.size() != m_tileList.size()) {
          throw ZStitchException(QString("The number of input stacks (%1) is not equal to either "
                                           "number of selected tiles (%2) or number of all tiles (%3). "
                                           "Can not decide which files should be stitiched. "
                                           "Abort.").arg(m_inputStack1Filenames.size()).arg(m_nSel).arg(
            m_tileList.size()));
        }
        nstack = m_nSel;

        if (m_inputStack1Filenames.size() == m_tileList.size()) {
          for (int i = 0; i < m_inputStack1Filenames.size(); i++) {
            if (m_tileList[i].bIsSelected) {
              inputStack1Sources.push_back(m_inputStack1Filenames[i]);
              inputStack2Sources.push_back(m_inputStack2Filenames[i]);
            }
          }
        } else {
          for (int i = 0; i < m_inputStack1Filenames.size(); i++) {
            inputStack1Sources.push_back(m_inputStack1Filenames[i]);
            inputStack2Sources.push_back(m_inputStack2Filenames[i]);
          }
        }
      } else {
        m_commandOutputEdit->setText(QString("<font color=red>No Tile Image, switching to blind stitching.</font>"));
        QApplication::processEvents();
        nstack = m_inputStack1Filenames.size();
        for (int i = 0; i < m_inputStack1Filenames.size(); i++) {
          inputStack1Sources.push_back(m_inputStack1Filenames[i]);
          inputStack2Sources.push_back(m_inputStack2Filenames[i]);
        }
        m_useFullConnectionRadioButton->click();
        QApplication::processEvents();
      }

    } else {
      nstack = m_inputStack1Filenames.size();
      for (int i = 0; i < m_inputStack1Filenames.size(); i++) {
        inputStack1Sources.push_back(m_inputStack1Filenames[i]);
        inputStack2Sources.push_back(m_inputStack2Filenames[i]);
      }
    }
  }

  if (m_useLayoutRadioButton->isChecked()) {
    size_t tmp_nstack = m_layout1SpinBox->value() * m_layout2SpinBox->value();
    if (nstack != 1) {  // check if the number fit
      if (nstack != tmp_nstack) {
        throw ZStitchException(
          QString("Invalid layout: number of input stacks: %1, required input stacks: %2. Abort.").arg(nstack).arg(
            tmp_nstack));
      }
    } else {   // split the input stack (only one) into tmp_nstack stacks
      nstack = tmp_nstack;
      if (stack1File1Infos.size() != nstack && stack1File1Infos[0].numTimes != nstack)
        throw ZStitchException("Invalid layout. Abort.");
      if (stack2File1Infos.size() != nstack && stack2File1Infos[0].numTimes != nstack)
        throw ZStitchException("Invalid layout. Abort.");
      if (stack1File1Infos.size() == nstack) {
        for (size_t i = 0; i < nstack; i++) {
          if (m_tileList[i].bIsSelected) {
            inputStack1Sources.push_back(ZImgSource(m_inputStack1Filenames[0], ZImgRegion(), i));
          }
        }
      } else {
        for (size_t i = 0; i < nstack; i++) {
          if (m_tileList[i].bIsSelected) {
            ZImgRegion rgn;
            rgn.start.t = i;
            rgn.end.t = i + 1;
            inputStack1Sources.push_back(ZImgSource(m_inputStack1Filenames[0], rgn, 0));
          }
        }
      }
      if (stack2File1Infos.size() == nstack) {
        for (size_t i = 0; i < nstack; i++) {
          if (m_tileList[i].bIsSelected) {
            inputStack2Sources.push_back(ZImgSource(m_inputStack2Filenames[0], ZImgRegion(), i));
          }
        }
      } else {
        for (size_t i = 0; i < nstack; i++) {
          if (m_tileList[i].bIsSelected) {
            ZImgRegion rgn;
            rgn.start.t = i;
            rgn.end.t = i + 1;
            inputStack2Sources.push_back(ZImgSource(m_inputStack2Filenames[0], rgn, 0));
          }
        }
      }
    }
  }

  m_commandOutputEdit->append(QString("Stitching %1 images ...").arg(nstack * 2));
  QApplication::processEvents();

  std::map<std::pair<size_t, size_t>, ZImgNCCMatch::PositionHint> conn;

  if (m_useConfigRadioButton->isChecked()) {
    if (nstack == 2) {
      std::pair<size_t, size_t> stackPair = std::pair<size_t, size_t>(0, 1);
      conn[stackPair] = ZImgNCCMatch::None;
      if (m_configDim1ComboBox->currentIndex() == 0)
        conn[stackPair] |= ZImgNCCMatch::Left;
      else if (m_configDim1ComboBox->currentIndex() == 2)
        conn[stackPair] |= ZImgNCCMatch::Right;

      if (m_configDim2ComboBox->currentIndex() == 0)
        conn[stackPair] |= ZImgNCCMatch::Up;
      else if (m_configDim2ComboBox->currentIndex() == 2)
        conn[stackPair] |= ZImgNCCMatch::Down;

      if (m_configDim3ComboBox->currentIndex() == 0)
        conn[stackPair] |= ZImgNCCMatch::Front;
      else if (m_configDim3ComboBox->currentIndex() == 2)
        conn[stackPair] |= ZImgNCCMatch::Back;
    }
  }
    /*generate connection file from tile_selection.lsm file*/
  else if (m_useTileImageRadioButton->isChecked()) {

    std::vector<std::vector<size_t>> tileMatrix(m_tileMatrix.size(), std::vector<size_t>(m_tileMatrix[0].size(), 0));

    int index = 1;
    for (size_t i = 0; i < tileMatrix.size(); i++) {
      for (size_t j = 0; j < tileMatrix[i].size(); j++) {
        if (m_tileMatrix[i][j] > 0 && m_tileList[m_tileMatrix[i][j] - 1].bIsSelected) {
          tileMatrix[i][j] = index++;
        }
      }
    }
    buildConnectionFromGrid(tileMatrix, conn);

    for (int i = 0; i < inputStack1Sources.size(); ++i) {
      m_commandOutputEdit->append(inputStack1Sources[i].toQString());
    }
    for (int i = 0; i < inputStack2Sources.size(); ++i) {
      m_commandOutputEdit->append(inputStack2Sources[i].toQString());
    }
    QApplication::processEvents();

  } else if (m_useConnFileRadioButton->isChecked()) {
    //    m_commandOutputEdit->append("Loading connection file...");
    //    QByteArray connba = m_connFileEdit->text().toUtf8();
    //    conn = load_conn(connba.data(), all_config);
    //    if (!conn) {
    //      m_commandOutputEdit->append(QString("<font color=red>Failed to load connection file: %1. Abort.</font>").arg(m_connFileEdit->text()));
    //      for (int i=0; i<nstack; i++) {
    //        free(all_config[i]);
    //      }
    //      free(all_config);
    //      all_config = nullptr;
    //      free(filepath);
    //      filepath = nullptr;
    //      return;
    //    }
  } else if (m_useFullConnectionRadioButton->isChecked()) {
    m_commandOutputEdit->append("<font color=red>Blind Stitching...</font>");
    QApplication::processEvents();
  } else if (m_useLayoutRadioButton->isChecked()) {

    int row = m_layout1SpinBox->value();
    int col = m_layout2SpinBox->value();

    std::vector<std::vector<size_t>> tileMatrix(row, std::vector<size_t>(col, 0));

    int index = 1;
    for (size_t i = 0; i < tileMatrix.size(); i++) {
      for (size_t j = 0; j < tileMatrix[i].size(); j++) {
        tileMatrix[i][j] = index++;
      }
    }
    buildConnectionFromGrid(tileMatrix, conn);
  }

  std::map<std::pair<size_t, size_t>, std::pair<ZVoxelCoordinate, double>> offsets;
  int intv[3];

  intv[0] = m_intvXSpinBox->value();
  intv[1] = m_intvYSpinBox->value();
  intv[2] = m_intvZSpinBox->value();

  if (m_dsCheckBox->isChecked()) {
    intv[0] /= m_dsXSpinBox->value();
    intv[1] /= m_dsYSpinBox->value();
    intv[2] /= m_dsZSpinBox->value();
  }

  // for every pair of img
  for (size_t f = 0; f < nstack; ++f) {  // fixed
    ZImg fixedImg1(inputStack1Sources[f]);
    ZImg fixedImg2(inputStack2Sources[f]);
    if (m_dsCheckBox->isChecked()) {
      m_commandOutputEdit->append(QString("Downsampling %1").arg(inputStack1Sources[f].toQString()));
      fixedImg1.blockDownsample(m_dsXSpinBox->value(), m_dsYSpinBox->value(), m_dsZSpinBox->value(),
                                ZImg::CombineMode::Mean);
      m_commandOutputEdit->append(QString("Downsampling %1").arg(inputStack2Sources[f].toQString()));
      fixedImg2.blockDownsample(m_dsXSpinBox->value(), m_dsYSpinBox->value(), m_dsZSpinBox->value(),
                                ZImg::CombineMode::Mean);
    }
    // between img1 and img2
    {
      ZImg fixedImg1CCh = fixedImg1.createView(m_commonChannel1SpinBox->value() - 1, 0);
      ZImg fixedImg2CCh = fixedImg2.createView(m_commonChannel2SpinBox->value() - 1, 0);
      ZImgNCCMatch imgNCCMatch(fixedImg1CCh, fixedImg2CCh);
      if (m_bgsub1ComboBox->currentIndex() == 1) {
        imgNCCMatch.enableRemoveBackgroundForAllFixedImgChannels();
      } else if (m_bgsub1ComboBox->currentIndex() > 1) {
        imgNCCMatch.enableRemoveBackgroundForFixedImgChannel(m_bgsub1ComboBox->currentIndex() - 2);
      }
      if (m_bgsub2ComboBox->currentIndex() == 1) {
        imgNCCMatch.enableRemoveBackgroundForAllMovingImgChannels();
      } else if (m_bgsub2ComboBox->currentIndex() > 1) {
        imgNCCMatch.enableRemoveBackgroundForMovingImgChannel(m_bgsub2ComboBox->currentIndex() - 2);
      }
      // fully overlap, no position hint
      imgNCCMatch.setMovingImgPositionHint(ZImgNCCMatch::None, 1.0);

      double maxNCC;
      ZVoxelCoordinate movingImgOffset = imgNCCMatch.computeMovingImgOffsetMR(intv[0], intv[1], intv[2], &maxNCC);
      movingImgOffset[3] = fixedImg1.numChannels();
      offsets[std::make_pair(f, f + nstack)] = std::make_pair(movingImgOffset, maxNCC);

      QString info = QString("img %1 -- img %2, img %2 position hint: None, offset: %3, NCC: %4")
        .arg(f + 1).arg(f + nstack + 1).arg(movingImgOffset.toQString()).arg(maxNCC);
      m_commandOutputEdit->append(info);
      QApplication::processEvents();
      LOG(INFO) << info;
    }
    for (size_t m = f + 1; m < nstack; ++m) { // moving
      // no connection
      if (!conn.empty() &&
          conn.find(std::make_pair(f, m)) == conn.end() &&
          conn.find(std::make_pair(m, f)) == conn.end()) {
        continue;
      }
      // already processed
      if (offsets.find(std::make_pair(m, f)) != offsets.end()) {
        continue;
      }

      ZImg movingImg1(inputStack1Sources[m]);
      ZImg movingImg2(inputStack2Sources[m]);
      if (m_dsCheckBox->isChecked()) {
        m_commandOutputEdit->append(QString("Downsampling %1").arg(inputStack1Sources[m].toQString()));
        movingImg1.blockDownsample(m_dsXSpinBox->value(), m_dsYSpinBox->value(), m_dsZSpinBox->value(),
                                   ZImg::CombineMode::Mean);
        m_commandOutputEdit->append(QString("Downsampling %1").arg(inputStack2Sources[m].toQString()));
        movingImg2.blockDownsample(m_dsXSpinBox->value(), m_dsYSpinBox->value(), m_dsZSpinBox->value(),
                                   ZImg::CombineMode::Mean);
      }

      // img1
      {
        ZImgNCCMatch imgNCCMatch(fixedImg1, movingImg1);
        if (m_bgsub1ComboBox->currentIndex() == 1) {
          imgNCCMatch.enableRemoveBackgroundForAllFixedImgChannels();
          imgNCCMatch.enableRemoveBackgroundForAllMovingImgChannels();
        } else if (m_bgsub1ComboBox->currentIndex() > 1) {
          imgNCCMatch.enableRemoveBackgroundForFixedImgChannel(m_bgsub1ComboBox->currentIndex() - 2);
          imgNCCMatch.enableRemoveBackgroundForMovingImgChannel(m_bgsub1ComboBox->currentIndex() - 2);
        }

        std::map<std::pair<size_t, size_t>, ZImgNCCMatch::PositionHint>::iterator it = conn.find(std::make_pair(f, m));
        ZImgNCCMatch::PositionHint hint = ZImgNCCMatch::None;
        if (it != conn.end()) {
          hint = it->second;
        } else {
          it = conn.find(std::make_pair(m, f));
          if (it != conn.end()) {
            hint = it->second;
            ZImgNCCMatch::reversePositionHint(hint);
          }
        }
        imgNCCMatch.setMovingImgPositionHint(hint, m_overlapRateSpinBox->value() / 100.0);

        double maxNCC;
        ZVoxelCoordinate movingImgOffset = imgNCCMatch.computeMovingImgOffsetMR(intv[0], intv[1], intv[2], &maxNCC);
        offsets[std::make_pair(f, m)] = std::make_pair(movingImgOffset, maxNCC);

        QString info = QString("img %1 -- img %2, img %2 position hint: %3, offset: %4, NCC: %5")
          .arg(f + 1).arg(m + 1).arg(imgNCCMatch.positionHintToQString()).arg(movingImgOffset.toQString()).arg(maxNCC);
        m_commandOutputEdit->append(info);
        QApplication::processEvents();
        LOG(INFO) << info;
      }
      // img2
      {
        ZImgNCCMatch imgNCCMatch(fixedImg2, movingImg2);
        if (m_bgsub2ComboBox->currentIndex() == 1) {
          imgNCCMatch.enableRemoveBackgroundForAllFixedImgChannels();
          imgNCCMatch.enableRemoveBackgroundForAllMovingImgChannels();
        } else if (m_bgsub2ComboBox->currentIndex() > 1) {
          imgNCCMatch.enableRemoveBackgroundForFixedImgChannel(m_bgsub2ComboBox->currentIndex() - 2);
          imgNCCMatch.enableRemoveBackgroundForMovingImgChannel(m_bgsub2ComboBox->currentIndex() - 2);
        }

        std::map<std::pair<size_t, size_t>, ZImgNCCMatch::PositionHint>::iterator it = conn.find(std::make_pair(f, m));
        ZImgNCCMatch::PositionHint hint = ZImgNCCMatch::None;
        if (it != conn.end())
          hint = it->second;
        else {
          it = conn.find(std::make_pair(m, f));
          if (it != conn.end()) {
            hint = it->second;
            ZImgNCCMatch::reversePositionHint(hint);
          }
        }
        imgNCCMatch.setMovingImgPositionHint(hint, m_overlapRateSpinBox->value() / 100.0);

        double maxNCC;
        ZVoxelCoordinate movingImgOffset = imgNCCMatch.computeMovingImgOffsetMR(intv[0], intv[1], intv[2], &maxNCC);
        offsets[std::make_pair(f + nstack, m + nstack)] = std::make_pair(movingImgOffset, maxNCC);

        QString info = QString("img %1 -- img %2, img %2 position hint: %3, offset: %4, NCC: %5")
          .arg(f + nstack + 1).arg(m + nstack + 1).arg(imgNCCMatch.positionHintToQString()).arg(
          movingImgOffset.toQString()).arg(maxNCC);
        m_commandOutputEdit->append(info);
        QApplication::processEvents();
        LOG(INFO) << info;
      }
    }
  }

  ZImgMerge::Mode mergeMode = ZImgMerge::Mode::Max;
  if (m_mergeMode1ComboBox->currentIndex() == 0) {
    mergeMode = ZImgMerge::Mode::Max;
  } else if (m_mergeMode1ComboBox->currentIndex() == 1) {
    mergeMode = ZImgMerge::Mode::Min;
  } else if (m_mergeMode1ComboBox->currentIndex() == 2) {
    mergeMode = ZImgMerge::Mode::Mean;
  } else if (m_mergeMode1ComboBox->currentIndex() == 3) {
    mergeMode = ZImgMerge::Mode::Median;
  } else {
    mergeMode = ZImgMerge::Mode::First;
  }

  ZImgMerge imgMerge;
  std::vector<ZImg> imgs(nstack * 2);
  for (size_t i = 0; i < imgs.size(); ++i) {
    if (i < nstack) {
      imgs[i].load(inputStack1Sources[i]);
      if (m_dsCheckBox->isChecked() &&
          (m_dsXSpinBox->value() > 1 || m_dsYSpinBox->value() > 1 || m_dsZSpinBox->value() > 1)) {
        m_commandOutputEdit->append(QString("Downsampling %1").arg(inputStack1Sources[i].toQString()));
        imgs[i].blockDownsample(m_dsXSpinBox->value(), m_dsYSpinBox->value(), m_dsZSpinBox->value(),
                                ZImg::CombineMode::Mean);
      }
    } else {
      imgs[i].load(inputStack2Sources[i - nstack]);
      if (m_dsCheckBox->isChecked() &&
          (m_dsXSpinBox->value() > 1 || m_dsYSpinBox->value() > 1 || m_dsZSpinBox->value() > 1)) {
        m_commandOutputEdit->append(QString("Downsampling %1").arg(inputStack2Sources[i - nstack].toQString()));
        imgs[i].blockDownsample(m_dsXSpinBox->value(), m_dsYSpinBox->value(), m_dsZSpinBox->value(),
                                ZImg::CombineMode::Mean);
      }
    }
  }
  for (std::map<std::pair<size_t, size_t>, std::pair<ZVoxelCoordinate, double>>::iterator it = offsets.begin();
       it != offsets.end(); ++it) {
    size_t f = it->first.first;
    size_t m = it->first.second;
    imgMerge.addImgPair(imgs[f], imgs[m], it->second.first, -(it->second.second),
                        QString::number(f + 1), QString::number(m + 1));
  }

  QString summary;
  ZImg res = imgMerge.merge(mergeMode, &summary);
  m_commandOutputEdit->append(summary);
  res.save(m_outputFileEdit->text());
  emit resultReady(&res, m_outputFileEdit->text());

  m_commandOutputEdit->append(QString("%1 saved.").arg(m_outputFileEdit->text()));
  if (m_useTileImageRadioButton->isChecked() && !m_tileImage.isNull()) {
    QString selectionImageOutputName = m_outputFileEdit->text();
    selectionImageOutputName.append("_TileSelectionInfo.tif");
    QImage image(m_tileImage);

    QPainter painter(&image);
    for (int i = 0; i < m_tileList.size(); i++) {
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
                                                      ZSystemInfoInstance.lastOpenedImagePath(),
                                                      tr("Conn File (*.txt)"));
  if (!connFileName.isEmpty()) {
    ZSystemInfoInstance.setLastOpenedImagePath(connFileName);
    m_connFileEdit->setText(connFileName);
  }
}

void ZStitchImageDialog::selectOutputFile()
{
  QString outputFileName = QFileDialog::getSaveFileName(this,
                                                        tr("specify output file"),
                                                        ZSystemInfoInstance.lastOpenedImagePath(),
                                                        tr("Output Image (*.tif *.v3draw)"));
  if (!outputFileName.isEmpty()) {
    ZSystemInfoInstance.setLastOpenedImagePath(outputFileName);
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

    //    m_outputCh1ImageChannelSpinBox->show();
    //    m_outputCh2ImageChannelSpinBox->show();
    //    m_outputCh3ImageChannelSpinBox->show();
    //    m_outputCh1ImageComboBox->show();
    //    m_outputCh2ImageComboBox->show();
    //    m_outputCh3ImageComboBox->show();
    for (int i = 0; i < m_labelsForTwoInputs.size(); i++) {
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

    //    m_outputCh1ImageChannelSpinBox->hide();
    //    m_outputCh2ImageChannelSpinBox->hide();
    //    m_outputCh3ImageChannelSpinBox->hide();
    //    m_outputCh1ImageComboBox->hide();
    //    m_outputCh2ImageComboBox->hide();
    //    m_outputCh3ImageComboBox->hide();
    for (int i = 0; i < m_labelsForTwoInputs.size(); i++) {
      m_labelsForTwoInputs[i]->hide();
    }
  }
}

void ZStitchImageDialog::setConnInfoSource()
{
  if (m_useTileImageRadioButton->isChecked()) {
    m_openTileImageButton->setEnabled(true);
    m_connEdit->setVisible(true);
    if (!m_tileImage.isNull()) {
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
  try {
    m_tabWidget->setCurrentIndex(2);
    QApplication::processEvents();

    QFileInfo outputFI(m_outputFileEdit->text());
    if (m_outputFileEdit->text().isEmpty() || !outputFI.absoluteDir().exists()) {
      throw ZStitchException("Please make sure the ouput folder exists.");
    }
    if (m_inputStack1Filenames.size() < 1) {
      throw ZStitchException("Please add input files.");
    }

    std::vector<ZImgInfo> stack1File1Infos = ZImg::readImgInfo(m_inputStack1Filenames[0]);
    for (size_t s = 1; s < stack1File1Infos.size(); ++s) {
      if (!stack1File1Infos[s].isSameType(stack1File1Infos[0])) {
        throw ZStitchException(QString("Image type of %1 scene 0 <%2> and scene %3 <%4> don't match")
                                 .arg(m_inputStack1Filenames[0]).arg(stack1File1Infos[0].toQString())
                                 .arg(s).arg(stack1File1Infos[s].toQString()));
      }
    }
    for (int i = 1; i < m_inputStack1Filenames.size(); ++i) {
      std::vector<ZImgInfo> tmpInfos = ZImg::readImgInfo(m_inputStack1Filenames[i]);
      for (size_t s = 0; s < tmpInfos.size(); ++s) {
        if (!tmpInfos[s].isSameType(stack1File1Infos[0])) {
          throw ZStitchException(QString("Image type of %1 <%2> and %3 <%4> don't match")
                                   .arg(m_inputStack1Filenames[0]).arg(stack1File1Infos[0].toQString())
                                   .arg(m_inputStack1Filenames[i]).arg(tmpInfos[s].toQString()));
        }
      }
    }

    if (m_hasTwoInputStackSetCheckBox->isChecked()) {
      stitchStacks2();
      return;
    }

    size_t nstack;
    QList<ZImgSource> inputStackSources;

    if (m_inputStack1Filenames.size() == 1 && m_useTileImageRadioButton->isChecked() && m_tileList.size() > 1) {
      // split input into m_tileList.size() tiles
      m_commandOutputEdit->append("Splitting image ...>");
      QApplication::processEvents();
      nstack = m_tileList.size();
      if (stack1File1Infos.size() != nstack && stack1File1Infos[0].numTimes != nstack)
        throw ZStitchException("Can not split this image. Abort.");

      if (stack1File1Infos.size() == nstack) {
        for (size_t i = 0; i < nstack; i++) {
          if (m_tileList[i].bIsSelected) {
            inputStackSources.push_back(ZImgSource(m_inputStack1Filenames[0], ZImgRegion(), i));
          }
        }
      } else {
        for (size_t i = 0; i < nstack; i++) {
          if (m_tileList[i].bIsSelected) {
            ZImgRegion rgn;
            rgn.start.t = i;
            rgn.end.t = i + 1;
            inputStackSources.push_back(ZImgSource(m_inputStack1Filenames[0], rgn, 0));
          }
        }
      }
      nstack = m_nSel;
    } else if (m_inputStack1Filenames.size() == 1 || m_useLayoutRadioButton->isChecked()) {
      nstack = m_inputStack1Filenames.size();
      for (int i = 0; i < m_inputStack1Filenames.size(); i++) {
        inputStackSources.push_back(m_inputStack1Filenames[i]);
      }
    } else {
      if (m_useTileImageRadioButton->isChecked()) {
        if (m_nSel >= 0) {
          // first check number of input stacks and selected stacks
          m_commandOutputEdit->setText(tr("checking file numbers..."));
          if (m_inputStack1Filenames.size() != m_nSel && m_inputStack1Filenames.size() != m_tileList.size()) {
            throw ZStitchException(QString("The number of input stacks (%1) is not equal to either "
                                             "number of selected tiles (%2) or number of all tiles (%3). "
                                             "Can not decide which files should be stitiched. "
                                             "Abort.").arg(m_inputStack1Filenames.size()).arg(m_nSel).arg(
              m_tileList.size()));
          }
          nstack = m_nSel;

          if (m_inputStack1Filenames.size() == m_tileList.size()) {
            for (int i = 0; i < m_inputStack1Filenames.size(); i++) {
              if (m_tileList[i].bIsSelected) {
                inputStackSources.push_back(m_inputStack1Filenames[i]);
              }
            }
          } else {
            for (int i = 0; i < m_inputStack1Filenames.size(); i++) {
              inputStackSources.push_back(m_inputStack1Filenames[i]);
            }
          }
        } else {
          m_commandOutputEdit->setText(QString("<font color=red>No Tile Image, switching to blind stitching.</font>"));
          QApplication::processEvents();
          nstack = m_inputStack1Filenames.size();
          for (int i = 0; i < m_inputStack1Filenames.size(); i++) {
            inputStackSources.push_back(m_inputStack1Filenames[i]);
          }
          m_useFullConnectionRadioButton->click();
          QApplication::processEvents();
        }

      } else {
        nstack = m_inputStack1Filenames.size();
        for (int i = 0; i < m_inputStack1Filenames.size(); i++) {
          inputStackSources.push_back(m_inputStack1Filenames[i]);
        }
      }
    }

    if (m_useLayoutRadioButton->isChecked()) {
      size_t tmp_nstack = m_layout1SpinBox->value() * m_layout2SpinBox->value();
      if (nstack != 1) {  // check if the number fit
        if (nstack != tmp_nstack) {
          throw ZStitchException(
            QString("Invalid layout: number of input stacks: %1, required input stacks: %2. Abort.").arg(nstack).arg(
              tmp_nstack));
        }
      } else {   // split the input stack (only one) into tmp_nstack stacks
        nstack = tmp_nstack;
        if (stack1File1Infos.size() != nstack && stack1File1Infos[0].numTimes != nstack)
          throw ZStitchException("Invalid layout. Abort.");
        if (stack1File1Infos.size() == nstack) {
          for (size_t i = 0; i < nstack; i++) {
            if (m_tileList[i].bIsSelected) {
              inputStackSources.push_back(ZImgSource(m_inputStack1Filenames[0], ZImgRegion(), i));
            }
          }
        } else {
          for (size_t i = 0; i < nstack; i++) {
            if (m_tileList[i].bIsSelected) {
              ZImgRegion rgn;
              rgn.start.t = i;
              rgn.end.t = i + 1;
              inputStackSources.push_back(ZImgSource(m_inputStack1Filenames[0], rgn, 0));
            }
          }
        }
      }
    }

    m_commandOutputEdit->append(QString("Stitching %1 images ...").arg(nstack));
    QApplication::processEvents();

    if (nstack == 1) {
      ZImg img(inputStackSources[0]);

      if (m_dsCheckBox->isChecked()) {
        m_commandOutputEdit->append("Downsampling ...");
        QApplication::processEvents();

        img.blockDownsample(m_dsXSpinBox->value(),
                            m_dsYSpinBox->value(),
                            m_dsZSpinBox->value(),
                            ZImg::CombineMode::Mean);
      }
      img.save(m_outputFileEdit->text());
      emit resultReady(&img, m_outputFileEdit->text());

      m_commandOutputEdit->append(QString("%1 saved.").arg(m_outputFileEdit->text()));
      return;
    }

    std::map<std::pair<size_t, size_t>, ZImgNCCMatch::PositionHint> conn;

    if (m_useConfigRadioButton->isChecked()) {
      if (nstack == 2) {
        std::pair<size_t, size_t> stackPair = std::pair<size_t, size_t>(0, 1);
        conn[stackPair] = ZImgNCCMatch::None;
        if (m_configDim1ComboBox->currentIndex() == 0)
          conn[stackPair] |= ZImgNCCMatch::Left;
        else if (m_configDim1ComboBox->currentIndex() == 2)
          conn[stackPair] |= ZImgNCCMatch::Right;

        if (m_configDim2ComboBox->currentIndex() == 0)
          conn[stackPair] |= ZImgNCCMatch::Up;
        else if (m_configDim2ComboBox->currentIndex() == 2)
          conn[stackPair] |= ZImgNCCMatch::Down;

        if (m_configDim3ComboBox->currentIndex() == 0)
          conn[stackPair] |= ZImgNCCMatch::Front;
        else if (m_configDim3ComboBox->currentIndex() == 2)
          conn[stackPair] |= ZImgNCCMatch::Back;
      }
    }
      /*generate connection file from tile_selection.lsm file*/
    else if (m_useTileImageRadioButton->isChecked()) {

      std::vector<std::vector<size_t>> tileMatrix(m_tileMatrix.size(), std::vector<size_t>(m_tileMatrix[0].size(), 0));

      int index = 1;
      for (size_t i = 0; i < tileMatrix.size(); i++) {
        for (size_t j = 0; j < tileMatrix[i].size(); j++) {
          if (m_tileMatrix[i][j] > 0 && m_tileList[m_tileMatrix[i][j] - 1].bIsSelected) {
            tileMatrix[i][j] = index++;
          }
        }
      }
      buildConnectionFromGrid(tileMatrix, conn);

      for (int i = 0; i < inputStackSources.size(); ++i) {
        m_commandOutputEdit->append(inputStackSources[i].toQString());
      }
      QApplication::processEvents();

    } else if (m_useConnFileRadioButton->isChecked()) {
      //    m_commandOutputEdit->append("Loading connection file...");
      //    QByteArray connba = m_connFileEdit->text().toUtf8();
      //    conn = load_conn(connba.data(), all_config);
      //    if (!conn) {
      //      m_commandOutputEdit->append(QString("<font color=red>Failed to load connection file: %1. Abort.</font>").arg(m_connFileEdit->text()));
      //      for (int i=0; i<nstack; i++) {
      //        free(all_config[i]);
      //      }
      //      free(all_config);
      //      all_config = nullptr;
      //      free(filepath);
      //      filepath = nullptr;
      //      return;
      //    }
    } else if (m_useFullConnectionRadioButton->isChecked()) {
      m_commandOutputEdit->append("<font color=red>Blind Stitching...</font>");
      QApplication::processEvents();
    } else if (m_useLayoutRadioButton->isChecked()) {

      int row = m_layout1SpinBox->value();
      int col = m_layout2SpinBox->value();

      std::vector<std::vector<size_t>> tileMatrix(row, std::vector<size_t>(col, 0));

      int index = 1;
      for (size_t i = 0; i < tileMatrix.size(); i++) {
        for (size_t j = 0; j < tileMatrix[i].size(); j++) {
          tileMatrix[i][j] = index++;
        }
      }
      buildConnectionFromGrid(tileMatrix, conn);
    }

    ZImgMerge::Mode mergeMode = ZImgMerge::Mode::Max;
    if (m_mergeMode1ComboBox->currentIndex() == 0) {
      mergeMode = ZImgMerge::Mode::Max;
    } else if (m_mergeMode1ComboBox->currentIndex() == 1) {
      mergeMode = ZImgMerge::Mode::Min;
    } else if (m_mergeMode1ComboBox->currentIndex() == 2) {
      mergeMode = ZImgMerge::Mode::Mean;
    } else if (m_mergeMode1ComboBox->currentIndex() == 3) {
      mergeMode = ZImgMerge::Mode::Median;
    } else {
      mergeMode = ZImgMerge::Mode::First;
    }

    if (m_concatOnlyCheckBox->isChecked()) {

      if (conn.empty()) {
        throw ZStitchException("Position information incomplete for image concatenate, Abort.");
      }

      std::vector<ZImg> imgs(nstack);
      for (size_t i = 0; i < imgs.size(); ++i) {
        imgs[i].load(inputStackSources[i]);
        if (m_dsCheckBox->isChecked() &&
            (m_dsXSpinBox->value() > 1 || m_dsYSpinBox->value() > 1 || m_dsZSpinBox->value() > 1)) {
          m_commandOutputEdit->append(QString("Downsampling %1").arg(inputStackSources[i].toQString()));
          imgs[i].blockDownsample(m_dsXSpinBox->value(), m_dsYSpinBox->value(), m_dsZSpinBox->value(),
                                  ZImg::CombineMode::Mean);
        }
      }

      ZImgMerge imgMerge;
      for (std::map<std::pair<size_t, size_t>, ZImgNCCMatch::PositionHint>::iterator it = conn.begin();
           it != conn.end(); ++it) {
        ZImgNCCMatch::PositionHint posHint = it->second;
        if (posHint == ZImgNCCMatch::None) {
          throw ZStitchException("Position information incomplete for image concatenate, Abort.");
        }
        const ZImg& fixedImg = imgs[it->first.first];
        const ZImg& movingImg = imgs[it->first.second];
        ZVoxelCoordinate movingImgOffset;
        if (posHint.testFlag(ZImgNCCMatch::Left)) {
          movingImgOffset.x = -static_cast<ZVoxelCoordinate::value_type>(movingImg.width());
        } else if (posHint.testFlag(ZImgNCCMatch::Right)) {
          movingImgOffset.x = fixedImg.width();
        }
        if (posHint.testFlag(ZImgNCCMatch::Up)) {
          movingImgOffset.y = -static_cast<ZVoxelCoordinate::value_type>(movingImg.height());
        } else if (posHint.testFlag(ZImgNCCMatch::Down)) {
          movingImgOffset.y = fixedImg.height();
        }
        if (posHint.testFlag(ZImgNCCMatch::Front)) {
          movingImgOffset.z = -static_cast<ZVoxelCoordinate::value_type>(movingImg.depth());
        } else if (posHint.testFlag(ZImgNCCMatch::Back)) {
          movingImgOffset.z = fixedImg.depth();
        }
        imgMerge.addImgPair(fixedImg, movingImg, movingImgOffset, 0,
                            QString::number(it->first.first + 1), QString::number(it->first.second + 1));
      }

      QString summary;
      ZImg res = imgMerge.merge(mergeMode, &summary);
      m_commandOutputEdit->append(summary);
      res.save(m_outputFileEdit->text());
      emit resultReady(&res, m_outputFileEdit->text());
    } else {
      std::map<std::pair<size_t, size_t>, std::pair<ZVoxelCoordinate, double>> offsets;
      int intv[3];

      intv[0] = m_intvXSpinBox->value();
      intv[1] = m_intvYSpinBox->value();
      intv[2] = m_intvZSpinBox->value();

      if (m_dsCheckBox->isChecked()) {
        intv[0] /= m_dsXSpinBox->value();
        intv[1] /= m_dsYSpinBox->value();
        intv[2] /= m_dsZSpinBox->value();
      }

      // for every pair of img
      for (size_t f = 0; f < nstack; ++f) {  // fixed
        ZImg fixedImg(inputStackSources[f]);
        if (m_dsCheckBox->isChecked()) {
          m_commandOutputEdit->append(QString("Downsampling %1").arg(inputStackSources[f].toQString()));
          fixedImg.blockDownsample(m_dsXSpinBox->value(), m_dsYSpinBox->value(), m_dsZSpinBox->value(),
                                   ZImg::CombineMode::Mean);
        }
        for (size_t m = f + 1; m < nstack; ++m) { // moving
          // no connection
          if (!conn.empty() &&
              conn.find(std::make_pair(f, m)) == conn.end() &&
              conn.find(std::make_pair(m, f)) == conn.end()) {
            continue;
          }
          // already processed
          if (offsets.find(std::make_pair(m, f)) != offsets.end()) {
            continue;
          }

          ZImg movingImg(inputStackSources[m]);
          if (m_dsCheckBox->isChecked()) {
            m_commandOutputEdit->append(QString("Downsampling %1").arg(inputStackSources[m].toQString()));
            movingImg.blockDownsample(m_dsXSpinBox->value(), m_dsYSpinBox->value(), m_dsZSpinBox->value(),
                                      ZImg::CombineMode::Mean);
          }

          ZImgNCCMatch imgNCCMatch(fixedImg, movingImg);
          if (m_bgsub1ComboBox->currentIndex() == 1) {
            imgNCCMatch.enableRemoveBackgroundForAllFixedImgChannels();
            imgNCCMatch.enableRemoveBackgroundForAllMovingImgChannels();
          } else if (m_bgsub1ComboBox->currentIndex() > 1) {
            imgNCCMatch.enableRemoveBackgroundForFixedImgChannel(m_bgsub1ComboBox->currentIndex() - 2);
            imgNCCMatch.enableRemoveBackgroundForMovingImgChannel(m_bgsub1ComboBox->currentIndex() - 2);
          }

          std::map<std::pair<size_t, size_t>, ZImgNCCMatch::PositionHint>::iterator it = conn.find(
            std::make_pair(f, m));
          ZImgNCCMatch::PositionHint hint = ZImgNCCMatch::None;
          if (it != conn.end())
            hint = it->second;
          else {
            it = conn.find(std::make_pair(m, f));
            if (it != conn.end()) {
              hint = it->second;
              ZImgNCCMatch::reversePositionHint(hint);
            }
          }
          imgNCCMatch.setMovingImgPositionHint(hint, m_overlapRateSpinBox->value() / 100.0);

          double maxNCC;
          ZVoxelCoordinate movingImgOffset = imgNCCMatch.computeMovingImgOffsetMR(intv[0], intv[1], intv[2], &maxNCC);
          offsets[std::make_pair(f, m)] = std::make_pair(movingImgOffset, maxNCC);

          QString info = QString("img %1 -- img %2, img %2 position hint: %3, offset: %4, NCC: %5")
            .arg(f + 1).arg(m + 1).arg(imgNCCMatch.positionHintToQString()).arg(movingImgOffset.toQString()).arg(
            maxNCC);
          m_commandOutputEdit->append(info);
          QApplication::processEvents();
          LOG(INFO) << info;
        }
      }

      ZImgMerge imgMerge;
      std::vector<ZImg> imgs(nstack);
      for (size_t i = 0; i < imgs.size(); ++i) {
        imgs[i].load(inputStackSources[i]);
        if (m_dsCheckBox->isChecked() &&
            (m_dsXSpinBox->value() > 1 || m_dsYSpinBox->value() > 1 || m_dsZSpinBox->value() > 1)) {
          m_commandOutputEdit->append(QString("Downsampling %1").arg(inputStackSources[i].toQString()));
          imgs[i].blockDownsample(m_dsXSpinBox->value(), m_dsYSpinBox->value(), m_dsZSpinBox->value(),
                                  ZImg::CombineMode::Mean);
        }
      }
      for (std::map<std::pair<size_t, size_t>, std::pair<ZVoxelCoordinate, double>>::iterator it = offsets.begin();
           it != offsets.end(); ++it) {
        size_t f = it->first.first;
        size_t m = it->first.second;
        imgMerge.addImgPair(imgs[f], imgs[m], it->second.first, -(it->second.second),
                            QString::number(f + 1), QString::number(m + 1));
      }

      QString summary;
      ZImg res = imgMerge.merge(mergeMode, &summary);
      m_commandOutputEdit->append(summary);
      res.save(m_outputFileEdit->text());
      emit resultReady(&res, m_outputFileEdit->text());
    }

    m_commandOutputEdit->append(QString("%1 saved.").arg(m_outputFileEdit->text()));
    if (m_useTileImageRadioButton->isChecked() && !m_tileImage.isNull()) {
      QString selectionImageOutputName = m_outputFileEdit->text();
      selectionImageOutputName.append("_TileSelectionInfo.tif");
      QImage image(m_tileImage);

      QPainter painter(&image);
      for (int i = 0; i < m_tileList.size(); i++) {
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
  catch (const ZException& e) {
    m_commandOutputEdit->append(QString("<font color=red>%1</font>").arg(e.what()));
    QMessageBox::critical(this, "error", e.what());
  }
  catch (const ZStitchException& e) {
    m_commandOutputEdit->append(QString("<font color=red>%1</font>").arg(e.what()));
    QMessageBox::critical(this, "error", e.what());
  }
}

} // namespace nim

