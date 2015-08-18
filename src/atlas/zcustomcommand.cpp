#include "zcustomcommand.h"

#include "zimg.h"
#include <QDir>
#include "zglmutils.h"
#include "zimagematrix3dtransform.h"
#include "zimgregistration.h"
#include "zregistrationnumericdiffcostfunction.h"
#include "zswc.h"
#include "zimgio.h"

namespace nim {

void zoomPVRawImages()
{
  QDir dir("/Volumes/lq/pvdata/");
  QString outFolder("/Users/feng/Documents/image/High_LowPV_ipsi_contra/");
  QStringList filters;
  filters << "*.raw";
  QFileInfoList list = dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks);
  for (int i=0; i<list.size(); i++) {
    QFileInfo fileInfo = list.at(i);
    QString outname = outFolder + fileInfo.completeBaseName() + "_ds.tif";
    ZImg img(fileInfo.absoluteFilePath(), ZImgRegion(0,-1,0,-1,0,-1,2,3));
    img.zoom(0.248113255269998, 0.248113255269998, 0.414373111494544);
    img.save(outname);
  }
}

void zoomRefBrainSlices()
{
  QDir dir("/Users/feng/Documents/image/sua/From AxioScan/5xbrain/");
  QString outFolder("/Users/feng/Documents/image/sua/From AxioScan/ds/");
  QStringList filters;
  filters << "*.ome.tiff";
  QFileInfoList list = dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks);
  for (int i=0; i<list.size(); i++) {
    QFileInfo fileInfo = list.at(i);
    QString outname = outFolder + fileInfo.fileName();
    ZImg img(fileInfo.absoluteFilePath());
    img.zoom(0.25, 0.25, 1);
    img.save(outname);
  }
}

void zoomRefBrainSlices2()
{
  QDir dir("/Users/feng/Documents/image/sua/5xbrain2/");
  QString outFolder("/Users/feng/Documents/image/sua/ds2/");
  QStringList filters;
  filters << "*.ome.tiff";
  QFileInfoList list = dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks);
  for (int i=0; i<list.size(); i++) {
    QFileInfo fileInfo = list.at(i);
    QString outname = outFolder + fileInfo.fileName();
    ZImg img(fileInfo.absoluteFilePath());
    img.zoom(0.25, 0.25, 1);
    img.save(outname);
  }
}

void resliceImg()
{
  glm::dvec3 b = glm::normalize(glm::dvec3(417, 261, 380) - glm::dvec3(489, 318, 90));
  glm::dvec3 a(0, 0, 1);
  glm::dvec3 v = glm::cross(a, b);
  double s = glm::length(v);
  double c = glm::dot(a, b);
  glm::dmat3 vx(glm::dvec3(0, v[2], -v[1]), glm::dvec3(-v[2], 0, v[0]), glm::dvec3(v[1], -v[0], 0));
  glm::dmat3 R(1.0);
  R = R + vx + vx * vx * ((1 - c) / s / s);
  ZImageMatrix3DTransform tfm;
  tfm.setImageInterpolation(ZImageInterpolation(Interpolant::Cubic, PadOption::Constant));
  ZAffine3D tform(R[0][0], R[1][0], R[2][0], 0,
      R[0][1], R[1][1], R[2][1], 0,
      R[0][2], R[1][2], R[2][2], 0);
  tfm.setTransform(tform);
  ZImg img("/Users/feng/Documents/image/SprengelMovies_Global_local/injection_site/stack_uncompressed/inj.tif");
  double xMin, xMax, yMin, yMax, zMin, zMax;
  tfm.transformRange(0, img.width()-1, 0, img.height()-1, 0, img.depth()-1, xMin, xMax, yMin, yMax, zMin, zMax);
  tfm.setTransform(tform);

  ZImg imgOut(ZImgInfo(std::ceil(xMax)-std::floor(xMin)+1, std::ceil(yMax)-std::floor(yMin)+1, std::ceil(zMax)-std::floor(zMin)+1, 3));
  tfm.transformImage(img.channelData<uint8_t>(0), img.width(), img.height(), img.depth(),
                     imgOut.channelData<uint8_t>(0), std::floor(xMin), std::ceil(xMax)+1,
                     std::floor(yMin), std::ceil(yMax)+1, std::floor(zMin), std::ceil(zMax)+1);
  imgOut.save("/Users/feng/Documents/image/SprengelMovies_Global_local/injection_site/stack_uncompressed/resliced_inj_red.tif");

  imgOut = ZImg(ZImgInfo(std::ceil(xMax)-std::floor(xMin)+1, std::ceil(yMax)-std::floor(yMin)+1, std::ceil(zMax)-std::floor(zMin)+1, 3));
  tfm.transformImage(img.channelData<uint8_t>(1), img.width(), img.height(), img.depth(),
                     imgOut.channelData<uint8_t>(1), std::floor(xMin), std::ceil(xMax)+1,
                     std::floor(yMin), std::ceil(yMax)+1, std::floor(zMin), std::ceil(zMax)+1);
  imgOut.save("/Users/feng/Documents/image/SprengelMovies_Global_local/injection_site/stack_uncompressed/resliced_inj_green.tif");
}

void atlasStep23()
{
  ZImg fixedImg("/Users/feng/Documents/image/allen/prealign_mask.tif");
  ZImg movingImg("/Users/feng/Documents/image/allen/averageTemplate_cor_mask.tif");

  ZImageToImageMetric metric;
  metric.setType(ZImageToImageMetric::Type::MeanSquaredDifferences);

  ZImageAffine3DTransform transform;
  //transform.setRotationCenter(movingImg.width() / 2.0, movingImg.height() / 2.0, movingImg.depth() / 2.0);
  std::vector<double> paras = transform.parameters();
  paras[8] = 2;
  transform.setParameters(paras);
  transform.setImageInterpolation(ZImageInterpolation(Interpolant::Linear, PadOption::Constant));

  ZRegistrationNumericDiffCostFunction costFunction(1e-6);
  costFunction.setMetric(metric);

  ZImgRegistration registration;
  registration.setCostFunction(costFunction);
  registration.setOptimizer("LBFGS");
  registration.setNumScales(3);
  registration.setInitialTransform(transform);

  registration.setFixedImg(fixedImg);
  registration.setMovingImg(movingImg);
  registration.run();

  ZImg res = fixedImg;
  LINFO() << transform.toQString();
  transform.transformImage(movingImg.channelData<uint8_t>(0), movingImg.width(), movingImg.height(),
                           movingImg.depth(), res.channelData<uint8_t>(0), 0, fixedImg.width(),
                           0, fixedImg.height(), 0, fixedImg.depth());
  res.save("/Users/feng/Documents/image/allen/averageTemplate_cor_mask_to_prealign.tif");
}

void convertImages()
{
  QDir dir("/Volumes/lq/image/OsungImage/compressed_raw");
  QString outFolder("/Users/feng/Documents/image/ipsi/");
  QStringList filters;
  filters << "*.tif";
  QFileInfoList list = dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks);
  for (int i=0; i<list.size(); i++) {
    QFileInfo fileInfo = list.at(i);
    QString outname = outFolder + fileInfo.baseName() + ".v3draw";
    ZImg img(fileInfo.absoluteFilePath());
    //img.save(outname);
    outname = outFolder + fileInfo.baseName() + "_axon_ch.tif";
    img = img.extractChannel(0);
    img.save(outname);
  }
}

void downsampleImage()
{
  ZImg img("/Users/feng/Documents/image/bigimage_new/0515_3to33.raw", ZImgRegion(0,-1,0,-1,0,-1,1,2));
  img.zoom(0.5, 0.5);
  img.save("/Users/feng/Documents/image/bigimage_new/0515_3to33_ds.raw", FileFormat::Vaa3DRaw);
}

void extractNeuronChannel()
{
  QDir dir("/Volumes/lq/image/mCA3_CA1_raw");
  QString outFolder("/Volumes/Jinny/");
  QStringList filters;
  filters << "*.raw";
  QFileInfoList list = dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks);
  dir = QDir("/Volumes/lq/image/devCA3");
  list.append(dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks));
  dir = QDir("/Volumes/lq/image/1201_devCA3_CA1/35143_01");
  list.append(dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks));
  dir = QDir("/Volumes/lq/image/1201_devCA3_CA1/35143_02");
  list.append(dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks));
  for (int i=0; i<list.size(); i++) {
    QFileInfo fileInfo = list.at(i);
    LINFO() << i << list.size() << fileInfo.absoluteFilePath();
    ZImg img(fileInfo.absoluteFilePath(), ZImgRegion(0,-1,0,-1,0,-1,1,2));
    QString outname = outFolder + fileInfo.baseName() + "_ch2.v3draw";
    img.save(outname);
  }
}

void convertImagesFormat()
{
  QDir dir("/Volumes/Jinny");
  QString outFolder("/Volumes/Jinny/");
  QStringList filters;
  filters << "*.v3draw";
  QFileInfoList list = dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks);
  for (int i=0; i<list.size(); i++) {
    QFileInfo fileInfo = list.at(i);
    LINFO() << i << list.size() << fileInfo.absoluteFilePath();
    ZImg img(fileInfo.absoluteFilePath());
    QString outname = outFolder + fileInfo.baseName() + ".tif";
    img.save(outname);
  }
}

void testSWC()
{
  ZSwc swc("/Users/feng/Documents/image/bigimage_new/020910_B09ATbgsub.swc");
  swc.labelSomaAndOthers(30);
  swc.save("/Users/feng/Documents/image/bigimage_new/020910_B09ATbgsub_2.swc");
}

void resizeInjectionCoreImgs()
{
  QDir dir("/Volumes/GPe_Vol2/KIST/HJ/Injection core");
  QString outFolder("/Volumes/lq/InjectionCores/");
  QStringList filters;
  filters << "*.czi";
  QFileInfoList list = dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks);
  for (int i=0; i<list.size(); i++) {
    QFileInfo fileInfo = list.at(i);
    QString fileName = fileInfo.fileName();
    size_t scene = fileName.at(fileName.size()-5).toLatin1() - '1';
    LINFO() << i << list.size() << fileInfo.absoluteFilePath() << scene;
    ZImg img(fileInfo.absoluteFilePath(), ZImgRegion(), scene);
    img.zoom(0.3, 0.3);
    QString outname = outFolder + fileInfo.baseName() + ".ome.tif";
    img.save(outname);
  }
}

void tmp()
{
}

}

namespace nim {

ZCustomCommand::ZCustomCommand()
{
}

void ZCustomCommand::run()
{
  tmp();
  LINFO() << "done";
}

} // namespace nim
