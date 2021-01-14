#pragma once

#include "zstitchimage.h"
#include "zimgprocessdialog.h"
#include <QPoint>
#include <QRect>
#include <vector>

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

namespace nim {

class ZImg;

class ZLogWidget;

class ZTile
{
public:
  ZTile(size_t index_, QPoint topleft, QPoint bottomright)
    : index(index_)
  {
    region = QRect(topleft, bottomright);
  }

  bool bIsSelected = true;
  size_t index;
  QRect region;
};

class ZTileImageWidget : public QWidget
{
Q_OBJECT
public:
  explicit ZTileImageWidget(QWidget* parent, QImage* image,
                            const std::vector<std::vector<size_t>>& tileMatrix,
                            std::vector<ZTile>* pTiles = nullptr,
                            const QStringList& filenames = QStringList());

  void paintEvent(QPaintEvent* event) override;

  void zoomIn();

  void zoomOut();

  void clearAllSelected();

  void selectAll();

  void saveAsImage(const QString& fn);

  [[nodiscard]] QSize minimumSizeHint() const override;

  [[nodiscard]] QSize sizeHint() const override;

  void mouseReleaseEvent(QMouseEvent* event) override;

  void mouseMoveEvent(QMouseEvent* event) override;

  void mousePressEvent(QMouseEvent* event) override;

private:
  QPixmap* m_pixmap = nullptr;
  QImage* m_image = nullptr;
  const std::vector<std::vector<size_t>>& m_tileMatrix;
  std::vector<ZTile>* m_pTiles;
  double m_scaleFactor;
  QRubberBand* m_rubberBand;
  QPoint m_origin;
  QStringList m_filenames;
  std::vector<QImage> m_tileimages;
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

  static bool getTileMatrix(ZImg& img, std::vector<std::vector<size_t>>& tileMatrix, std::vector<ZTile>& tileList);

  void initScene1ComboBox(int scene);

  void initChannel1ComboBox(int nchannel);

  void initBgsub1ComboBox(int nchannel);

  void initScene2ComboBox(int scene);

  void initChannel2ComboBox(int nchannel);

  void initBgsub2ComboBox(int nchannel);

private:
  std::vector<std::vector<size_t>> m_tileMatrix;
  std::vector<ZTile> m_tileList;
  int m_nSel;

  QGroupBox* m_ioGroupBox = nullptr;
  QGroupBox* m_connGroupBox = nullptr;
  QGroupBox* m_commandOutputGroupBox = nullptr;

  QStringList m_inputStack1Filenames;
  QStringList m_inputStack2Filenames;
  QString m_tileSelectionImageFilename;
  QImage m_tileImage;

  QCheckBox* m_dsCheckBox = nullptr;
  //QCheckBox *m_useLayoutRadioButton = nullptr;
  QCheckBox* m_concatOnlyCheckBox = nullptr;
  QCheckBox* m_hasTwoInputStackSetCheckBox = nullptr;

  QLineEdit* m_outputFileEdit = nullptr;
  QLineEdit* m_connFileEdit = nullptr;
  QTextEdit* m_inputStack1FileEdit = nullptr;
  QTextEdit* m_inputStack2FileEdit = nullptr;
  QTextEdit* m_connEdit = nullptr;
  ZLogWidget* m_commandOutputEdit = nullptr;

  QSpinBox* m_overlapRateSpinBox = nullptr;
  QRadioButton* m_useConfigRadioButton = nullptr;
  QRadioButton* m_useTileImageRadioButton = nullptr;
  QRadioButton* m_useConnFileRadioButton = nullptr;
  QRadioButton* m_useFullConnectionRadioButton = nullptr;
  QRadioButton* m_useLayoutRadioButton = nullptr;
  QRadioButton* m_restitchCZIRadioButton = nullptr;

  QPushButton* m_selectInputStacks1Button = nullptr;
  QPushButton* m_selectInputStacks2Button = nullptr;
  QPushButton* m_openTileImageButton = nullptr;
  QPushButton* m_editTileImageButton = nullptr;
  QToolButton* m_selectConnFileButton = nullptr;
  QToolButton* m_selectOutputButton = nullptr;
  QComboBox* m_mergeModeComboBox = nullptr;
  QComboBox* m_scene1ComboBox = nullptr;
  QComboBox* m_bgsub1ComboBox = nullptr;
  QComboBox* m_channel1ComboBox = nullptr;
  QComboBox* m_scene2ComboBox = nullptr;
  QComboBox* m_bgsub2ComboBox = nullptr;
  QComboBox* m_channel2ComboBox = nullptr;
  QSpinBox* m_commonChannel1SpinBox = nullptr;
  QSpinBox* m_commonChannel2SpinBox = nullptr;
  //  QSpinBox *m_outputCh1ImageChannelSpinBox = nullptr;
  //  QSpinBox *m_outputCh2ImageChannelSpinBox = nullptr;
  //  QSpinBox *m_outputCh3ImageChannelSpinBox = nullptr;
  //  QComboBox *m_outputCh1ImageComboBox = nullptr;
  //  QComboBox *m_outputCh2ImageComboBox = nullptr;
  //  QComboBox *m_outputCh3ImageComboBox = nullptr;
  QSpinBox* m_layout1SpinBox = nullptr;
  QSpinBox* m_layout2SpinBox = nullptr;
  QComboBox* m_configDim1ComboBox = nullptr;
  QComboBox* m_configDim2ComboBox = nullptr;
  QComboBox* m_configDim3ComboBox = nullptr;
  QSpinBox* m_intvXSpinBox = nullptr;
  QSpinBox* m_intvYSpinBox = nullptr;
  QSpinBox* m_intvZSpinBox = nullptr;
  QSpinBox* m_dsXSpinBox = nullptr;
  QSpinBox* m_dsYSpinBox = nullptr;
  QSpinBox* m_dsZSpinBox = nullptr;

  std::vector<QLabel*> m_labelsForTwoInputs;

  ZTileImageWidget* m_tileImageWidget = nullptr;
  QScrollArea* m_scrollArea = nullptr;

  QTabWidget* m_tabWidget = nullptr;
};

} // namespace nim

