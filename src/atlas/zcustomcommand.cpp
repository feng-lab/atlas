#include "zcustomcommand.h"

#include "zimg.h"
#include <QDir>
#include "zglmutils.h"
#include "zimagematrix3dtransform.h"
#include "zimgregistration.h"
#include "zregistrationnumericdiffcostfunction.h"
#include "zswc.h"
#include "zimgio.h"
#include "zmesh.h"
#include "itkMath.h"
#include "zimgautothreshold.h"
#include "zimggraph.h"
#include <tbb/task_scheduler_init.h>

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

void transfromMesh()
{
  QDir dir("/Users/feng/Downloads/all_obj/pial_Full");
  QStringList filters;
  filters << "*.obj";
  QFileInfoList list = dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks);
  list.append(QDir("/Users/feng/Downloads/all_obj").entryInfoList(filters, QDir::Files | QDir::NoSymLinks));
  QString outFolder = "/Users/feng/code/Neural-Network/models";
  glm::mat4 mat;
  nim::toVal(QString("[1.03, 0, 0, 0; 0, 6.13928e-08, 1.03, 0; 0, -1.03, 6.13928e-08, 0; 0, 0, 0, 1]"), mat);

  for (int i=0; i<list.size(); i++) {
    QFileInfo fileInfo = list.at(i);
    ZMesh msh(fileInfo.absoluteFilePath());

    std::vector<glm::vec3> vertices = msh.vertices();
    for (size_t i=0; i<vertices.size(); ++i) {
      vertices[i] = glm::applyMatrix(mat, vertices[i]);
    }
    msh.setVertices(vertices);
    msh.generateNormals();
    msh.save(QString("%1/%2").arg(outFolder).arg(fileInfo.fileName()));
  }
}

void transfromMesh2()
{
  QDir dir("/Users/feng/Library/Application Support/Brain Explorer 2/Atlases/Allen Mouse Brain Common Coordinate Framework/Spaces/P56/Meshes");
  QStringList filters;
  filters << "*.msh";
  QFileInfoList list = dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks);
  list.append(QDir("/Users/feng/Downloads/all_obj").entryInfoList(filters, QDir::Files | QDir::NoSymLinks));
  QString outFolder = "/Users/feng/Downloads/gpe_stn_traj_mesh";
  glm::mat4 mat;
  nim::toVal(QString("[5.96047e-09, 0, -0.1, 490.6; 0, 0.1, 0, -355.6; 0.1, 0, 5.96047e-09, -513.4; 0, 0, 0, 1]"), mat);

  for (int i=0; i<list.size(); i++) {
    QFileInfo fileInfo = list.at(i);
    ZMesh msh(fileInfo.absoluteFilePath());

    std::vector<glm::vec3> vertices = msh.vertices();
    for (size_t i=0; i<vertices.size(); ++i) {
      vertices[i] = glm::applyMatrix(mat, vertices[i]);
    }
    msh.setVertices(vertices);
    msh.generateNormals();
    msh.save(QString("%1/%2.obj").arg(outFolder).arg(fileInfo.fileName()));
  }
}

void stnTrajectory()
{
  std::set<QString> expBaseNames;
  QDir dir("/Users/feng/Downloads/allen_stn_grid");
  QStringList filters;
  filters << "*.nrrd";
  QFileInfoList list = dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks);
  QDir outFolder("/Users/feng/Downloads/allen_stn_grid_traj");
  ZImg annotation("/Users/feng/Documents/allen/CCFv3/ccf_2015/annotation_50.nrrd");
  std::vector<size_t> stnIdxs;
  for (size_t i=0; i<annotation.voxelNumber(); ++i) {
    if (annotation.value(i) == 470) {
      stnIdxs.push_back(i);
    }
  }

  for (int i=0; i<list.size(); i++) {
    QFileInfo fileInfo = list.at(i);
    QStringList tokens = fileInfo.fileName().split("_");
    for (auto it = tokens.begin(); it != tokens.end(); ++it) {
      if (it->contains("um")) {
        tokens.erase(it, tokens.end());
        break;
      }
    }
    if (!tokens.empty()) {
      expBaseNames.insert(tokens.join('_'));
    }
  }
  std::vector<QString> exps;
  exps.insert(exps.end(), expBaseNames.begin(), expBaseNames.end());

  double projectionThre = 0.2;

  tbb::task_scheduler_init init(8);
  tbb::parallel_for(
        tbb::blocked_range<size_t>(0, exps.size()),
        [&](const tbb::blocked_range<size_t>& range){
    for (size_t expIdx = range.begin(); expIdx != range.end(); ++expIdx) {
      const QString& str = exps[expIdx];
      LINFO() << str;
      ZImg mask(dir.filePath(str + "_50um_data_mask.nrrd"));
      ZImg projection(dir.filePath(str + "_50um_projection_density.nrrd"));
      projection *= mask;

      std::vector<size_t> projIdxs;
      for (size_t idx : stnIdxs) {
        if (projection.value(idx) >= projectionThre) {
          projIdxs.push_back(idx);
        }
      }

      if (projIdxs.empty())
        continue;

      ZImg injection(dir.filePath(str + "_50um_injection_density.nrrd"));
      injection *= mask;

      std::vector<size_t> injectionIdxs;
      for (size_t i=0; i<injection.voxelNumber(); ++i) {
        if (injection.value(i) > 0) {
          injectionIdxs.push_back(i);
        }
      }

      if (injectionIdxs.empty())
        continue;

      ZImgGraph imgGraph(projection);
      imgGraph.setConnectivity(26);
      ZImgAutoThreshold<> imgAutoThre;
      double cent1 = 0;
      double cent2 = 0;
      double thre1 = imgAutoThre.centroidThre<double>(cent1, cent2, projection);
      double scale = cent2 - cent1;
      if (scale < 1.0)
        scale = 1.0;
      scale /= 9.2;
      imgGraph.build(ZImgGraph::EdgeWeight2(thre1, scale));

      std::vector<std::vector<glm::dvec3>> lines;
      for (size_t idx : projIdxs) {
  #if 1
        std::vector<size_t> path;
        imgGraph.shortestPath(idx, injectionIdxs, &path);
        std::vector<glm::dvec3> line;
        for (size_t idx : path) {
          ZVoxelCoordinate coord = ZImg::indexToCoord(idx, projection.info());
          line.push_back(glm::dvec3(coord.x, coord.y, coord.z));
        }
  #else
        std::vector<size_t> predecessor;
        std::vector<double> dist = imgGraph.shortestPaths(idx, &predecessor);
        std::vector<double> injectionMinDists;
        for (size_t injectionIdx : injectionIdxs) {
          injectionMinDists.push_back(dist[injectionIdx]);
        }
        size_t curIdx = injectionIdxs[std::min_element(injectionMinDists.begin(), injectionMinDists.end()) - injectionMinDists.begin()];
        std::vector<glm::dvec3> line;
        while (true) {
          ZVoxelCoordinate coord = ZImg::indexToCoord(curIdx, projection.info());
          line.push_back(glm::dvec3(coord.x, coord.y, coord.z));
          if (curIdx == idx) {
            break;
          }
          assert(predecessor[curIdx] != curIdx);
          curIdx = predecessor[curIdx];
        }
  #endif
        if (line.empty()) {
          LWARN() << "WTF";
        } else {
          lines.push_back(line);
        }
      }

      ZSwc swc;
      for (const auto& line : lines) {
        swc.addLine(line, 1);
      }
      swc.resortID();
      swc.save(outFolder.filePath(str + "_50um_stn_trace.swc"));
    }
  }
  );
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
  stnTrajectory();
  LINFO() << "done";
}

} // namespace nim
