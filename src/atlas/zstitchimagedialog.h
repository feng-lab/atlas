#pragma once

#include "zstitchimage.h"
#include "zimgprocessdialog.h"
#include <QList>
#include <QPoint>
#include <QRect>
#include <vector>

QT_BEGIN_NAMESPACE
class QCheckBox;

class QGroupBox;

class QToolButton;

class QDoubleSpinBox;

class QSpinBox;

class QPushButton;

class QDialogButtonBox;

class QPaintEvent;

class QLabel;

class QTextEdit;

class QLineEdit;

class QRadioButton;

class QComboBox;

class QRect;

class QImage;

class QRubberBand;

class QVBoxLayout;

class QScrollArea;

class QTabWidget;

QT_END_NAMESPACE

namespace nim {

class ZImg;

class ZLogWidget;

class ZTile
{
public:
  ZTile(int index_, QPoint topleft, QPoint bottomright)
    : index(index_)
  {
    region = QRect(topleft, bottomright);
  }

  bool bIsSelected = true;
  int index;
  QRect region;
};

class ZTileImageWidget : public QWidget
{
Q_OBJECT
public:
  explicit ZTileImageWidget(QWidget* parent, QImage* image,
                            const std::vector<std::vector<int>>& tileMatrix,
                            QList<ZTile>* pTiles = nullptr,
                            const QStringList& filenames = QStringList());

  void paintEvent(QPaintEvent* event) override;

  void zoomIn();

  void zoomOut();

  void clearAllSelected();

  void selectAll();

  void saveAsImage(const QString& fn);

  QSize minimumSizeHint() const override;

  QSize sizeHint() const override;

  void mouseReleaseEvent(QMouseEvent* event) override;

  void mouseMoveEvent(QMouseEvent* event) override;

  void mousePressEvent(QMouseEvent* event) override;

private:
  QPixmap* m_pixmap;
  QImage* m_image;
  const std::vector<std::vector<int>>& m_tileMatrix;
  QList<ZTile>* m_pTiles;
  double m_scaleFactor;
  QRubberBand* m_rubberBand;
  QPoint m_origin;
  QStringList m_filenames;
  QList<QImage> m_tileimages;
};

class ZStitchImageDialog : public ZImgProcessDialog
{
Q_OBJECT
public:
  explicit ZStitchImageDialog(QWidget* parent = nullptr);

  ~ZStitchImageDialog() override;

signals:

  void resultReady(QString path);

protected:
  void createWorker(ZImgProcess*& worker, QString& workerName) override;

private:
  void selectInputStacks1();

  void selectInputStacks2();

  void selectConnFile();

  void selectOutputFile();

  void getConnFromTileImage();

  void editConnFromTileImage();

  void dsCheckBoxChanged(int state);

  void hasTwoInputStackSetCheckBoxChanged(int state);

  void setConnInfoSource();

  void zoomInTileImageWidget();

  void zoomOutTileImageWidget();

  void clearAllSelectedInTileImageWidget();

  void selectAllInTileImageWidget();

  void saveTileImageWidgetAsImage();
  //  void outputCh1ImageComboBoxIndexChanged(int index);
  //  void outputCh2ImageComboBoxIndexChanged(int index);
  //  void outputCh3ImageComboBoxIndexChanged(int index);

  QLayout* createIOLayout();

  QLayout* createConnLayout();

  QLayout* createCommandOutputLayout();

  void createIOGroupBox();

  void createConnGroupBox();

  void createCommandOutputGroupBox();

  QWidget* createIOWidget();

  QWidget* createConnWidget();

  QWidget* createCommandOutputWidget();

  static bool getTileMatrix(ZImg& img, std::vector<std::vector<int>>& tileMatrix, QList<ZTile>& tileList);

  void initScene1ComboBox(int scene);

  void initChannel1ComboBox(int nchannel);

  void initBgsub1ComboBox(int nchannel);

  void initScene2ComboBox(int scene);

  void initChannel2ComboBox(int nchannel);

  void initBgsub2ComboBox(int nchannel);

private:
  std::vector<std::vector<int>> m_tileMatrix;
  QList<ZTile> m_tileList;
  int m_nSel;

  QGroupBox* m_ioGroupBox;
  QGroupBox* m_connGroupBox;
  QGroupBox* m_commandOutputGroupBox;

  QStringList m_inputStack1Filenames;
  QStringList m_inputStack2Filenames;
  QString m_tileSelectionImageFilename;
  QImage m_tileImage;

  QCheckBox* m_dsCheckBox;
  //QCheckBox *m_useLayoutRadioButton;
  QCheckBox* m_concatOnlyCheckBox;
  QCheckBox* m_hasTwoInputStackSetCheckBox;

  QLineEdit* m_outputFileEdit;
  QLineEdit* m_connFileEdit;
  QTextEdit* m_inputStack1FileEdit;
  QTextEdit* m_inputStack2FileEdit;
  QTextEdit* m_connEdit;
  ZLogWidget* m_commandOutputEdit;

  QSpinBox* m_overlapRateSpinBox;
  QRadioButton* m_useConfigRadioButton;
  QRadioButton* m_useTileImageRadioButton;
  QRadioButton* m_useConnFileRadioButton;
  QRadioButton* m_useFullConnectionRadioButton;
  QRadioButton* m_useLayoutRadioButton;
  QRadioButton* m_restitchCZIRadioButton;

  QPushButton* m_selectInputStacks1Button;
  QPushButton* m_selectInputStacks2Button;
  QPushButton* m_openTileImageButton;
  QPushButton* m_editTileImageButton;
  QToolButton* m_selectConnFileButton;
  QToolButton* m_selectOutputButton;
  QComboBox* m_mergeModeComboBox;
  QComboBox* m_scene1ComboBox;
  QComboBox* m_bgsub1ComboBox;
  QComboBox* m_channel1ComboBox;
  QComboBox* m_scene2ComboBox;
  QComboBox* m_bgsub2ComboBox;
  QComboBox* m_channel2ComboBox;
  QSpinBox* m_commonChannel1SpinBox;
  QSpinBox* m_commonChannel2SpinBox;
  //  QSpinBox *m_outputCh1ImageChannelSpinBox;
  //  QSpinBox *m_outputCh2ImageChannelSpinBox;
  //  QSpinBox *m_outputCh3ImageChannelSpinBox;
  //  QComboBox *m_outputCh1ImageComboBox;
  //  QComboBox *m_outputCh2ImageComboBox;
  //  QComboBox *m_outputCh3ImageComboBox;
  QSpinBox* m_layout1SpinBox;
  QSpinBox* m_layout2SpinBox;
  QComboBox* m_configDim1ComboBox;
  QComboBox* m_configDim2ComboBox;
  QComboBox* m_configDim3ComboBox;
  QSpinBox* m_intvXSpinBox;
  QSpinBox* m_intvYSpinBox;
  QSpinBox* m_intvZSpinBox;
  QSpinBox* m_dsXSpinBox;
  QSpinBox* m_dsYSpinBox;
  QSpinBox* m_dsZSpinBox;

  QList<QLabel*> m_labelsForTwoInputs;

  ZTileImageWidget* m_tileImageWidget;
  QScrollArea* m_scrollArea;

  QTabWidget* m_tabWidget;
};

} // namespace nim

