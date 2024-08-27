#include "zcustomcommand.h"

#include "zimg.h"
#include "zglmutils.h"
#include "zimagematrix3dtransform.h"
#include "zimgregistration.h"
#include "zregistrationnumericdiffcostfunction.h"
#include "zswc.h"
#include "zimgnccmatch.h"
#include "zmesh.h"
#include "zimgautothreshold.h"
#include "zimggraph.h"
#include "zregionontology.h"
#include "zjson.h"
#include "zbenchtimer.h"
#include "zrandom.h"
#include "zstringutils.h"
#include "zmainwindow.h"
#include "zdoc.h"
#include "z3dmainwindow.h"
#include "z3drenderingengine.h"
#include "zview.h"
#include "zvbgmm.h"
#include "zpunctadetection.h"
#include "zstitchimage.h"
#include "z3dmeshview.h"
#include "z3dpunctaview.h"
#include "z3dswcview.h"
// #include "zstructutils.h"
#include <qtcsv/reader.h>
#include <itkMath.h>
#include <QDir>
#include <QApplication>
#include <tbb/global_control.h>

namespace nim {

void zoomPVRawImages()
{
  QDir dir("/Volumes/lq/pvdata/");
  QString outFolder("/Users/feng/Documents/image/High_LowPV_ipsi_contra/");
  QStringList filters;
  filters << "*.raw";
  QFileInfoList list = dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks);
  for (auto& fileInfo : list) {
    QString outname = outFolder + fileInfo.completeBaseName() + "_ds.tif";
    ZImg img(fileInfo.absoluteFilePath(), ZImgRegion(0, -1, 0, -1, 0, -1, 2, 3));
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
  for (auto& fileInfo : list) {
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
  for (auto& fileInfo : list) {
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
  ZAffine3D tform(R[0][0], R[1][0], R[2][0], 0, R[0][1], R[1][1], R[2][1], 0, R[0][2], R[1][2], R[2][2], 0);
  tfm.setTransform(tform);
  ZImg img("/Users/feng/Documents/image/SprengelMovies_Global_local/injection_site/stack_uncompressed/inj.tif");
  double xMin, xMax, yMin, yMax, zMin, zMax;
  tfm.transformRange(0, img.width() - 1, 0, img.height() - 1, 0, img.depth() - 1, xMin, xMax, yMin, yMax, zMin, zMax);
  tfm.setTransform(tform);

  ZImg imgOut(ZImgInfo(std::ceil(xMax) - std::floor(xMin) + 1,
                       std::ceil(yMax) - std::floor(yMin) + 1,
                       std::ceil(zMax) - std::floor(zMin) + 1,
                       3));
  tfm.transformImage(img.channelData<uint8_t>(0),
                     img.width(),
                     img.height(),
                     img.depth(),
                     imgOut.channelData<uint8_t>(0),
                     std::floor(xMin),
                     std::ceil(xMax) + 1,
                     std::floor(yMin),
                     std::ceil(yMax) + 1,
                     std::floor(zMin),
                     std::ceil(zMax) + 1);
  imgOut.save(
    "/Users/feng/Documents/image/SprengelMovies_Global_local/injection_site/stack_uncompressed/resliced_inj_red.tif");

  imgOut = ZImg(ZImgInfo(std::ceil(xMax) - std::floor(xMin) + 1,
                         std::ceil(yMax) - std::floor(yMin) + 1,
                         std::ceil(zMax) - std::floor(zMin) + 1,
                         3));
  tfm.transformImage(img.channelData<uint8_t>(1),
                     img.width(),
                     img.height(),
                     img.depth(),
                     imgOut.channelData<uint8_t>(1),
                     std::floor(xMin),
                     std::ceil(xMax) + 1,
                     std::floor(yMin),
                     std::ceil(yMax) + 1,
                     std::floor(zMin),
                     std::ceil(zMax) + 1);
  imgOut.save(
    "/Users/feng/Documents/image/SprengelMovies_Global_local/injection_site/stack_uncompressed/resliced_inj_green.tif");
}

void atlasStep23()
{
  ZImg fixedImg("/Users/feng/Documents/image/allen/prealign_mask.tif");
  ZImg movingImg("/Users/feng/Documents/image/allen/averageTemplate_cor_mask.tif");

  ZImageToImageMetric metric;
  metric.setType(ZImageToImageMetric::Type::MeanSquaredDifferences);

  ZImageAffine3DTransform transform;
  // transform.setRotationCenter(movingImg.width() / 2.0, movingImg.height() / 2.0, movingImg.depth() / 2.0);
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
  LOG(INFO) << transform.toString();
  transform.transformImage(movingImg.channelData<uint8_t>(0),
                           movingImg.width(),
                           movingImg.height(),
                           movingImg.depth(),
                           res.channelData<uint8_t>(0),
                           0,
                           fixedImg.width(),
                           0,
                           fixedImg.height(),
                           0,
                           fixedImg.depth());
  res.save("/Users/feng/Documents/image/allen/averageTemplate_cor_mask_to_prealign.tif");
}

void convertImages()
{
  QDir dir("/Volumes/lq/image/OsungImage/compressed_raw");
  QString outFolder("/Users/feng/Documents/image/ipsi/");
  QStringList filters;
  filters << "*.tif";
  QFileInfoList list = dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks);
  for (auto& fileInfo : list) {
    QString outname = outFolder + fileInfo.baseName() + ".v3draw";
    ZImg img(fileInfo.absoluteFilePath());
    // img.save(outname);
    outname = outFolder + fileInfo.baseName() + "_axon_ch.tif";
    img = img.extractChannel(0);
    img.save(outname);
  }
}

void downsampleImage()
{
  ZImg img("/Users/feng/Documents/image/bigimage_new/0515_3to33.raw", ZImgRegion(0, -1, 0, -1, 0, -1, 1, 2));
  img.zoom(0.5, 0.5);
  img.save("/Users/feng/Documents/image/bigimage_new/0515_3to33_ds.raw", FileFormat::Vaa3DRaw);
}

void extractNeuronChannel()
{
  //  QDir dir("/Volumes/lq/image/mCA3_CA1_raw");
  //  QString outFolder("/Volumes/Jinny/");
  QDir dir("/Volumes/PVPY/Py");
  QString outFolder("/Users/feng/Downloads/PyNeurons/");
  QStringList filters;
  filters << "*.raw";
  QFileInfoList list = dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks);
  //  dir = QDir("/Volumes/lq/image/devCA3");
  //  list.append(dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks));
  //  dir = QDir("/Volumes/lq/image/1201_devCA3_CA1/35143_01");
  //  list.append(dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks));
  //  dir = QDir("/Volumes/lq/image/1201_devCA3_CA1/35143_02");
  //  list.append(dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks));
  for (index_t i = 0; i < list.size(); ++i) {
    QFileInfo fileInfo = list.at(i);
    LOG(INFO) << i << " " << list.size() << " " << fileInfo.absoluteFilePath();
    ZImg img(fileInfo.absoluteFilePath(), ZImgRegion(0, -1, 0, -1, 0, -1, 1, 2));
    QString outname = outFolder + fileInfo.baseName() + "_ch2.tif";
    img.save(outname);
  }
}

void convertRawToNim()
{
  //  QDir dir("/Volumes/lq/image/mCA3_CA1_raw");
  //  QString outFolder("/Volumes/Jinny/");
  QDir dir("/Volumes/PVPY/Py");
  QStringList filters;
  filters << "*.raw";
  QFileInfoList list = dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks);
  dir = QDir("/Volumes/PVPY/contra");
  list.append(dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks));
  dir = QDir("/Volumes/PVPY/ipsi");
  list.append(dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks));
  //  dir = QDir("/Volumes/lq/image/1201_devCA3_CA1/35143_02");
  //  list.append(dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks));
  for (index_t i = 26; i < list.size(); ++i) {
    QFileInfo fileInfo = list.at(i);
    LOG(INFO) << i << " " << list.size() << " " << fileInfo.absoluteFilePath();
    ZImg img(fileInfo.absoluteFilePath());
    if (fileInfo.baseName().startsWith("Py")) {
      img.infoRef().channelColors[0] = col4{0, 255, 0};
      img.infoRef().channelColors[1] = col4{255, 0, 0};
    } else if (fileInfo.baseName().startsWith("PV")) {
      img.infoRef().channelColors[0] = col4{0, 0, 255};
      img.infoRef().channelColors[1] = col4{0, 255, 0};
      img.infoRef().channelColors[2] = col4{255, 0, 0};
    } else {
      CHECK(false);
    }
    QString outname = fileInfo.absoluteFilePath();
    outname.replace(".raw", ".nim");
    CHECK(outname.endsWith(".nim"));
    img.save(outname);
  }
}

void removeChannel()
{
  ZImg img("/Volumes/PVPY/Py/Py0515_s15_1_1.raw", ZImgRegion(0, -1, 0, -1, 0, -1, 0, 2));
  img.save("/Volumes/PVPY/Py/Py0515_s15_1_1_ch12.tif");
}

void convertImagesFormat()
{
  QDir dir("/Volumes/Jinny");
  QString outFolder("/Volumes/Jinny/");
  QStringList filters;
  filters << "*.v3draw";
  QFileInfoList list = dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks);
  for (index_t i = 0; i < list.size(); ++i) {
    QFileInfo fileInfo = list.at(i);
    LOG(INFO) << i << " " << list.size() << " " << fileInfo.absoluteFilePath();
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
  for (index_t i = 0; i < list.size(); ++i) {
    QFileInfo fileInfo = list.at(i);
    QString fileName = fileInfo.fileName();
    size_t scene = fileName.at(fileName.size() - 5).toLatin1() - '1';
    LOG(INFO) << i << " " << list.size() << " " << fileInfo.absoluteFilePath() << " " << scene;
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

  for (auto& fileInfo : list) {
    ZMesh msh(fileInfo.absoluteFilePath());

    std::vector<glm::vec3> vertices = msh.vertices();
    for (auto& vertice : vertices) {
      vertice = glm::applyMatrix(mat, vertice);
    }
    msh.setVertices(vertices);
    msh.generateNormals();
    msh.save(QString("%1/%2").arg(outFolder).arg(fileInfo.fileName()));
  }
}

void transfromMesh2()
{
  QDir dir(
    "/Users/feng/Library/Application Support/Brain Explorer 2/Atlases/Allen Mouse Brain Common Coordinate Framework/Spaces/P56/Meshes");
  QStringList filters;
  filters << "*.msh";
  QFileInfoList list = dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks);
  list.append(QDir("/Users/feng/Downloads/all_obj").entryInfoList(filters, QDir::Files | QDir::NoSymLinks));
  QString outFolder = "/Users/feng/Downloads/gpe_stn_traj_mesh";
  glm::mat4 mat;
  nim::toVal(QString("[5.96047e-09, 0, -0.1, 490.6; 0, 0.1, 0, -355.6; 0.1, 0, 5.96047e-09, -513.4; 0, 0, 0, 1]"), mat);

  for (auto& fileInfo : list) {
    ZMesh msh(fileInfo.absoluteFilePath());

    std::vector<glm::vec3> vertices = msh.vertices();
    for (auto& vertice : vertices) {
      vertice = glm::applyMatrix(mat, vertice);
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
  QDir outFolder("/Users/feng/Downloads/allen_stn_grid_traj_energy");
  QString resolution = "50";
  tbb::global_control gc(tbb::global_control::max_allowed_parallelism, 8);

  ZTree<RegionNode> ontology;
  readMouseBrainAtlasOntology(ontology);
  ZImg annotation(QString("/Users/feng/Documents/allen/CCFv3/ccf_2015/annotation_%1.nrrd").arg(resolution));
  ZImg isoCortexMask(ZImgInfo(annotation.width(), annotation.height(), annotation.depth()));
  uint32_t stnID = idOfRegionAbbreviation("STN", ontology);
  std::vector<int64_t> cortexIDs = allIDsWithinRegionAbbreviation("Isocortex", ontology);
  std::vector<size_t> stnIdxs;
  for (size_t i = 0; i < annotation.voxelNumber(); ++i) {
    if (annotation.value(i) == stnID) {
      stnIdxs.push_back(i);
    } else if (contains(cortexIDs, annotation.value(i))) {
      isoCortexMask.setValue(1, i);
    }
  }

  for (auto& fileInfo : list) {
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

  tbb::parallel_for(tbb::blocked_range<size_t>(0, exps.size()), [&](const tbb::blocked_range<size_t>& range) {
    for (size_t expIdx = range.begin(); expIdx != range.end(); ++expIdx) {
      const QString& str = exps[expIdx];
      //      if (!str.contains("298000880") &&
      //          !str.contains("306491185")) {
      //        continue;
      //      }

      if (outFolder.exists(str + "_" + resolution + "um_stn_trace.swc")) {
        continue;
      }

      LOG(INFO) << str;
      ZImg mask(dir.filePath(str + "_" + resolution + "um_data_mask.nrrd"));
      ZImg projection(dir.filePath(str + "_" + resolution + "um_projection_density.nrrd"));
      projection *= mask;

      std::vector<size_t> projIdxs;
      for (size_t idx : stnIdxs) {
        if (projection.value(idx) >= projectionThre) {
          projIdxs.push_back(idx);
        }
      }

      if (projIdxs.empty()) {
        continue;
      }

      ZImg injection(dir.filePath(str + "_" + resolution + "um_injection_density.nrrd"));
      injection *= mask;
      // injection *= isoCortexMask;

      std::vector<size_t> injectionIdxs;
      for (size_t i = 0; i < injection.voxelNumber(); ++i) {
        if (injection.value(i) > 0) {
          injectionIdxs.push_back(i);
        }
      }

      if (injectionIdxs.empty()) {
        continue;
      }

#if 1
      projection = ZImg(dir.filePath(str + "_" + resolution + "um_projection_energy.nrrd"));
      projection *= mask;
      projection.typedUnaryOperation<float>([](float proj) {
        return std::max(255.f, proj);
      });
      // projection.typedBinaryOperation<float, float>(injection, [](float proj, float inj) { return inj > 0.f ? 0.f :
      // proj; });
      projection.normalize();
#endif

      ZImgGraph imgGraph(projection);
      imgGraph.setConnectivity(26);
      ZImgAutoThreshold<> imgAutoThre;
      double cent1 = 0;
      double cent2 = 0;
      auto thre1 = imgAutoThre.centroidThre<double>(cent1, cent2, projection);
#if 0
        double scale = cent2 - cent1;
        if (scale < 1.0)
          scale = 1.0;
        scale /= 9.2;
#else
      double scale = 0.5;
      thre1 = 0.15;
#endif
      LOG(INFO) << str << " " << scale << " " << thre1;
      imgGraph.build(ZImgGraph::EdgeWeight3(thre1, scale));

      std::vector<std::vector<glm::dvec3>> lines;
      for (size_t idx : projIdxs) {
#if 0
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
        size_t curIdx = injectionIdxs[std::min_element(injectionMinDists.begin(), injectionMinDists.end()) -
                                      injectionMinDists.begin()];
        std::vector<glm::dvec3> line;
        while (true) {
          ZVoxelCoordinate coord = ZImg::indexToCoord(curIdx, projection.info());
          line.emplace_back(coord.x, coord.y, coord.z);
          if (curIdx == idx) {
            break;
          }
          CHECK(predecessor[curIdx] != curIdx);
          curIdx = predecessor[curIdx];
        }
#endif
        if (line.empty()) {
          LOG(WARNING) << "WTF";
        } else {
          lines.push_back(line);
        }
      }

      ZSwc origSwc;
      for (const auto& line : lines) {
        origSwc.addLine(line, 1);
      }

      ZSwc swc;
      for (ZSwc::ConstRootIterator oit = origSwc.cbeginRoot(); oit != origSwc.cendRoot(); ++oit) {
        ZSwc::Iterator desParent;
        ZSwc::ConstIterator srcChild;
        for (ZSwc::RootIterator it = swc.beginRoot(); it != swc.endRoot(); ++it) {
          if (it->x == oit->x && it->y == oit->y && it->z == oit->z) {
            desParent = it;
            ZSwc::ConstIterator srcParent = oit;
            while (swc.numChildren(desParent) > 0 && origSwc.numChildren(srcParent) > 0) {
              double srcX = ZSwc::firstChild(srcParent)->x;
              double srcY = ZSwc::firstChild(srcParent)->y;
              double srcZ = ZSwc::firstChild(srcParent)->z;

              ZSwc::Iterator matchDesChild;
              for (ZSwc::ChildIterator cit = swc.beginChild(desParent); cit != swc.endChild(desParent); ++cit) {
                if (cit->x == srcX && cit->y == srcY && cit->z == srcZ) {
                  matchDesChild = cit;
                  break;
                }
              }

              if (ZSwc::isNull(matchDesChild)) {
                break;
              } else {
                desParent = matchDesChild;
                srcParent = ZSwc::firstChild(srcParent);
              }
            }
            if (origSwc.numChildren(srcParent) > 0) {
              srcChild = ZSwc::firstChild(srcParent);
            }
            break;
          }
        }
        if (ZSwc::isNull(desParent)) {
          swc.copy(swc.appendRoot(*oit), origSwc, oit);
        } else if (!ZSwc::isNull(srcChild)) {
          swc.copy(swc.appendChild(desParent, *srcChild), origSwc, srcChild);
        }
      }

      swc.resortID();
      if (!outFolder.exists(str + "_" + resolution + "um_stn_trace.swc")) {
        swc.save(outFolder.filePath(str + "_" + resolution + "um_stn_trace.swc"));
      }
    }
  });
}

void mergeTraces()
{
  QDir dir("/Users/feng/Downloads/allen_stn_grid_traj_1");
  QStringList filters;
  filters << "*.swc";
  QFileInfoList list = dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks);
  QDir outFolder("/Users/feng/Downloads/allen_stn_grid_traj_clean");

  for (const auto& fileInfo : list) {
    QString fileName = fileInfo.fileName();
    ZSwc origSwc(fileInfo.absoluteFilePath());
    ZSwc swc;

    if (origSwc.numRoots() != origSwc.numLeafs()) {
      origSwc.save(outFolder.filePath(fileName));
      continue;
    }

    for (ZSwc::ConstRootIterator oit = origSwc.cbeginRoot(); oit != origSwc.cendRoot(); ++oit) {
      ZSwc::Iterator desParent;
      ZSwc::ConstIterator srcChild;
      for (ZSwc::RootIterator it = swc.beginRoot(); it != swc.endRoot(); ++it) {
        if (it->x == oit->x && it->y == oit->y && it->z == oit->z) {
          desParent = it;
          ZSwc::ConstIterator srcParent = oit;
          while (swc.numChildren(desParent) > 0 && origSwc.numChildren(srcParent) > 0) {
            double srcX = ZSwc::firstChild(srcParent)->x;
            double srcY = ZSwc::firstChild(srcParent)->y;
            double srcZ = ZSwc::firstChild(srcParent)->z;

            ZSwc::Iterator matchDesChild;
            for (ZSwc::ChildIterator cit = swc.beginChild(desParent); cit != swc.endChild(desParent); ++cit) {
              if (cit->x == srcX && cit->y == srcY && cit->z == srcZ) {
                matchDesChild = cit;
                break;
              }
            }

            if (ZSwc::isNull(matchDesChild)) {
              break;
            } else {
              desParent = matchDesChild;
              srcParent = ZSwc::firstChild(srcParent);
            }
          }
          if (origSwc.numChildren(srcParent) > 0) {
            srcChild = ZSwc::firstChild(srcParent);
          }
          break;
        }
      }
      if (ZSwc::isNull(desParent)) {
        swc.copy(swc.appendRoot(*oit), origSwc, oit);
      } else if (!ZSwc::isNull(srcChild)) {
        swc.copy(swc.appendChild(desParent, *srcChild), origSwc, srcChild);
      }
    }

    swc.resortID();
    swc.save(outFolder.filePath(fileName));
  }
}

void calcSwcVolume()
{
  QDir dir("/Users/feng/Documents/PV/AnalysisTextFiles");
  QDir outFolder("/Users/feng/Documents/PV/mesh");
  QStringList filters;
  QFileInfoList dirlist = dir.entryInfoList(filters, QDir::Dirs | QDir::NoSymLinks | QDir::NoDotAndDotDot);
  // LOG(INFO) << dirlist.size() << " " << dirlist.at(0).absolutePath();
  ZMesh rootMesh;
  ZMesh somaMesh;
  ZMesh branchMesh;
  filters << "*c.swc";
  LOG(INFO) << "NameOfCell, SomaSurfaceArea, SomaVolume, NeuriteSurfaceArea, NeuriteVolume";
  for (auto& dirInfo : dirlist) {
    QDir subDir(dirInfo.absoluteFilePath());
    QFileInfoList list = subDir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks);
    CHECK(list.size() == 1);
    QFileInfo fileInfo = list.at(0);
    ZMesh::createSwcMesh(ZSwc(fileInfo.absoluteFilePath()), 1, rootMesh, somaMesh, branchMesh);
    somaMesh.save(outFolder.filePath(fileInfo.baseName() + "_soma.stl"));
    branchMesh.save(outFolder.filePath(fileInfo.baseName() + "_neurite.stl"));
    auto rootProp = somaMesh.properties();
    auto branchProp = branchMesh.properties();
    LOG(INFO) << fileInfo.baseName() << ", " << rootProp.surfaceArea * (1.0 / 9.66 / 9.66) << ", "
              << rootProp.volume * (1.0 / 9.66 / 9.66 / 9.66) << ", " << branchProp.surfaceArea * (1.0 / 9.66 / 9.66)
              << ", " << branchProp.volume * (1.0 / 9.66 / 9.66 / 9.66);
  }
}

void changeImgCompressionType()
{
  // QDir dir("/Users/feng/Documents/PY/py_axonregion");
  // QString outFolder("/Users/feng/Documents/PY/py_ar/");
  QDir dir("/Users/feng/Documents/PV/pv_axonregion");
  QString outFolder("/Users/feng/Documents/PV/pv_ar/");
  QStringList filters;
  filters << "*.tif";
  QFileInfoList list = dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks);
  for (index_t i = 0; i < list.size(); ++i) {
    QFileInfo fileInfo = list.at(i);
    LOG(INFO) << i << " " << list.size() << " " << fileInfo.absoluteFilePath();
    ZImg img(fileInfo.absoluteFilePath());
    QString outname = outFolder + fileInfo.baseName() + ".tif";
    ZImgWriteParameters paras;
    paras.compression = Compression::LZW;
    img.save(outname, FileFormat::Tiff, paras);
  }
}

void makeSWCPyramidal()
{
  QDir dir("/Users/feng/Documents/PY/PySWC");
  QString outFolder("/Users/feng/Documents/PY/PySWC/");
  QStringList filters;
  filters << "*c.swc";
  QFileInfoList list = dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks);
  for (index_t i = 0; i < list.size(); ++i) {
    QFileInfo fileInfo = list.at(i);
    LOG(INFO) << i << " " << list.size() << " " << fileInfo.absoluteFilePath();
    ZSwc tree(fileInfo.absoluteFilePath());
    tree.labelSomaAndOthers(3.0 / 0.104); // soma radius at least 3um
    tree.resortPyramidal();
    QString outname = outFolder + fileInfo.baseName() + ".swc";
    tree.save(outname);
  }
}

void makeAxonChannelImages()
{
  QDir dir("/Volumes/PVPY/contra");
  QDir outFolder("/Volumes/PVPY/contra/axon_channel");
  size_t axonChannel = 0;

  QStringList filters;
  filters << "*.raw";
  QFileInfoList list = dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks);
  for (index_t i = 0; i < list.size(); ++i) {
    QFileInfo fileInfo = list.at(i);
    QString outname = outFolder.absoluteFilePath(fileInfo.baseName() + ".tif");
    QString outname1 = outFolder.absoluteFilePath(fileInfo.baseName() + "_rb.tif");
    if (QFileInfo::exists(outname) && QFileInfo::exists(outname1)) {
      continue;
    }
    LOG(INFO) << i << " " << list.size() << " " << fileInfo.absoluteFilePath();
    ZImg img(fileInfo.absoluteFilePath(), ZImgRegion(0, -1, 0, -1, 0, -1, axonChannel, axonChannel + 1));
    img.zoom(0.25, 0.25);
    ZImg img1 = img;
    ZImgWriteParameters paras;
    paras.compression = Compression::LZW;
    img.normalize(0, 50);
    img.save(outname, FileFormat::Tiff, paras);
    img1.normalize(11, 50);
    img1.save(outname1, FileFormat::Tiff, paras);
  }

  dir = QDir("/Volumes/PVPY/ipsi");
  outFolder = QDir("/Volumes/PVPY/ipsi/axon_channel");
  axonChannel = 0;

  list = dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks);
  for (index_t i = 0; i < list.size(); ++i) {
    QFileInfo fileInfo = list.at(i);
    QString outname = outFolder.absoluteFilePath(fileInfo.baseName() + ".tif");
    QString outname1 = outFolder.absoluteFilePath(fileInfo.baseName() + "_rb.tif");
    if (QFileInfo::exists(outname) && QFileInfo::exists(outname1)) {
      continue;
    }
    LOG(INFO) << i << " " << list.size() << " " << fileInfo.absoluteFilePath();
    ZImg img(fileInfo.absoluteFilePath(), ZImgRegion(0, -1, 0, -1, 0, -1, axonChannel, axonChannel + 1));
    img.zoom(0.25, 0.25);
    ZImg img1 = img;
    ZImgWriteParameters paras;
    paras.compression = Compression::LZW;
    img.normalize(0, 50);
    img.save(outname, FileFormat::Tiff, paras);
    img1.normalize(11, 50);
    img1.save(outname1, FileFormat::Tiff, paras);
  }

  dir = QDir("/Volumes/PVPY/Py");
  outFolder = QDir("/Volumes/PVPY/Py/axon_channel");
  axonChannel = 2;

  list = dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks);
  for (index_t i = 0; i < list.size(); ++i) {
    QFileInfo fileInfo = list.at(i);
    QString outname = outFolder.absoluteFilePath(fileInfo.baseName() + ".tif");
    QString outname1 = outFolder.absoluteFilePath(fileInfo.baseName() + "_rb.tif");
    if (QFileInfo::exists(outname) && QFileInfo::exists(outname1)) {
      continue;
    }
    LOG(INFO) << i << " " << list.size() << " " << fileInfo.absoluteFilePath();
    ZImg img(fileInfo.absoluteFilePath(), ZImgRegion(0, -1, 0, -1, 0, -1, axonChannel, axonChannel + 1));
    img.zoom(0.25, 0.25);
    ZImg img1 = img;
    ZImgWriteParameters paras;
    paras.compression = Compression::LZW;
    img.normalize(0, 50);
    img.save(outname, FileFormat::Tiff, paras);
    img1.normalize(11, 50);
    img1.save(outname1, FileFormat::Tiff, paras);
  }
}

void moveObjectToCorrectLocation(const QString& fn,
                                 const QString& resfn,
                                 const QStringList& metaFiles,
                                 const QStringList& swcFolders,
                                 int mode)
{
  auto loadObj = loadJsonObject(fn);
  if (!loadObj.contains("Scene") || !loadObj.at("Scene").is_object()) {
    return;
  }
  auto& sceneVal = loadObj.at("Scene");
  auto& sceneObj = sceneVal.as_object();

  QDir::setCurrent(QFileInfo(fn).absolutePath());

  QList<QStringList> metaData = QtCSV::Reader::readToList(metaFiles[0]);
  metaData.removeFirst();
  for (index_t i = 1; i < metaFiles.size(); ++i) {
    QList<QStringList> tmp = QtCSV::Reader::readToList(metaFiles[i]);
    tmp.removeFirst();
    metaData += tmp;
  }

  std::map<QString, glm::dvec3> cellNameToLocations;
  glm::dvec3 imagescale = glm::dvec3(71, 71, 71);
  double imagePixelPerUm = 0.136;
  double swcPixelPerUmxy = 9.66;
  double swcPixelPerUmz = 2.0;
  glm::dvec3 refSwcLocInBrain = glm::dvec3(-2.25, 1.27, 2.5); // in mm
  glm::dvec3 refSwcRootImageLoc = glm::dvec3(373, 291, 202); // roughly get from image
  for (auto& metaIdx : metaData) {
    QString cellName = metaIdx[0];
    double Anterior_Posterior = metaIdx[1].toDouble();
    double Medial_Lateral = metaIdx[2].toDouble();
    double Deep_Superficial = metaIdx[3].toDouble();

    glm::dvec3 swcLoc(-Medial_Lateral, Deep_Superficial, -Anterior_Posterior);
    glm::dvec3 swcRootImageLoc = (swcLoc - refSwcLocInBrain) * imagePixelPerUm * 1000. + refSwcRootImageLoc;
    QString swcFolder;
    for (const auto& i : swcFolders) {
      if (QFile::exists(QString("%1/%2.swc").arg(i).arg(cellName))) {
        swcFolder = i;
        break;
      }
    }
    ZSwc swc(QString("%1/%2.swc").arg(swcFolder).arg(cellName));
    ZSwc::SwcTreeNode rootn = swc.thickestNode();
    glm::dvec3 rootLoc(rootn->x, rootn->y, rootn->z * swcPixelPerUmxy / swcPixelPerUmz);

    if (mode == 1) {
      glm::dvec3 rootTrans = swcRootImageLoc - rootLoc * imagePixelPerUm / swcPixelPerUmxy;
      rootTrans = rootTrans * imagescale;
      cellNameToLocations[cellName] = rootTrans;
    } else if (mode == 2) {
      cellNameToLocations[cellName] = -rootLoc;
    }
  }

  auto& docObject = sceneObj.at("Doc").as_object();
  for (auto& [key, value] : docObject) {
    QString qkey = QString::fromUtf8(key.data(), key.size());
    QStringList typeAndID = qkey.split(" ");
    QString IDString = typeAndID[1].trimmed();
    QFileInfo docPath(asQString(value));
    QString filename = docPath.completeBaseName();
    if (typeAndID[0] == "Swc") {
      if (filename.endsWith("layer")) {
        filename.chop(6);
      } else {
        LOG(FATAL) << "..";
      }
      *JsonValueProxy(sceneVal)[IDString.toStdString()]["View3D"]["Color Mode StringIntOption"] = "Single Color";
      if (filename.startsWith("Py")) {
        *JsonValueProxy(sceneVal)[IDString.toStdString()]["View3D"]["Color Vec4"] =
          json::value_from(glm::vec4(1, 0, 0, 1));
      } else if (filename.startsWith("PV169") || filename.startsWith("PV40") || filename.startsWith("PV41") ||
                 filename.startsWith("PV42")) {
        *JsonValueProxy(sceneVal)[IDString.toStdString()]["View3D"]["Color Vec4"] =
          json::value_from(glm::vec4(0, 1, 1, 1));
      } else {
        *JsonValueProxy(sceneVal)[IDString.toStdString()]["View3D"]["Color Vec4"] =
          json::value_from(glm::vec4(0, 0, 1, 1));
      }
    } else if (typeAndID[0] == "Puncta") {
      if (filename.endsWith("puncta")) {
        filename.chop(7);
      } else if (filename.endsWith("neurite")) {
        filename.chop(8);
      } else {
        LOG(FATAL) << "..";
      }
      *JsonValueProxy(sceneVal)[IDString.toStdString()]["View3D"]["Use Same Size Bool"] = true;
      *JsonValueProxy(sceneVal)[IDString.toStdString()]["View3D"]["Size Scale Float"] = 4.f;
      *JsonValueProxy(sceneVal)[IDString.toStdString()]["View3D"]["Color Mode StringIntOption"] = "Single Color";
      *JsonValueProxy(sceneVal)[IDString.toStdString()]["View3D"]["Puncta Color Vec4"] =
        json::value_from(glm::vec4(0, 1, 0, 1));
    }
    glm::dvec3 loc = cellNameToLocations.at(filename);

    *JsonValueProxy(sceneVal)[IDString.toStdString()]["View3D"]["Coord Transform 3DTransform"]["Scale Vec3"] =
      json::value_from(glm::dvec3(1, 1, 5));
    *JsonValueProxy(sceneVal)[IDString.toStdString()]["View3D"]["Coord Transform 3DTransform"]["Translation Vec3"] =
      json::value_from(loc);
    *JsonValueProxy(sceneVal)[IDString.toStdString()]["View2D"]["Offset DVec4"] = json::value_from(glm::dvec4(loc, 0));
  }

  json::object saveObj;
  saveObj["Scene"] = sceneObj;

  saveJsonObject(saveObj, resfn);
}

void createCellTable()
{
  QList<QStringList> metaData =
    QtCSV::Reader::readToList("/Users/feng/code/mgrasp-analysis/pv_figs/orig_cell_props.csv");
  metaData.removeFirst();

  std::map<QString, int> somaLocationMap;
  somaLocationMap["Ori"] = 1;
  somaLocationMap["Deep"] = 2;
  somaLocationMap["Superficial"] = 3;
  somaLocationMap["Rad"] = 4;

  ZMainWindow* mainWin = nullptr;
  for (auto widget : QApplication::topLevelWidgets()) {
    mainWin = qobject_cast<ZMainWindow*>(widget);
    if (mainWin) {
      break;
    }
  }

  auto loadObj = loadJsonObject("/Users/feng/Downloads/template_cell.scene");
  if (!loadObj.contains("Scene") || !loadObj.at("Scene").is_object()) {
    return;
  }
  auto& sceneVal = loadObj.at("Scene");
  auto& sceneObj = sceneVal.as_object();
  auto& docObject = sceneObj["Doc"].as_object();

  std::map<std::tuple<QString, int, double, double, QString>, std::tuple<QString, double>> cells;

  for (auto& metaIdx : metaData) {
    QString cellType = metaIdx[1];
    QString cellName = metaIdx[2];
    QString somaLocation = metaIdx[3];
    double AP = metaIdx[4].toDouble();
    double ML = metaIdx[5].toDouble();
    double r2 = metaIdx[27].toDouble();
    auto somaLocationOrder = somaLocationMap[somaLocation];
    assert(somaLocationOrder > 0 && somaLocationOrder < 5);

    if (cellType == "Pyr") {
      continue;
    }
    cells[std::make_tuple(cellType, somaLocationOrder, AP, ML, cellName)] = std::make_tuple(somaLocation, r2);

    continue;
    QString swcName = QString("/Users/feng/Documents/PV/PVSWC/%1_layer.swc").arg(cellName);
    QString punctaName = QString("/Users/feng/Documents/PV/PVSWC/%1_neurite.nimp").arg(cellName);

    for (auto& [key, value] : docObject) {
      QString qkey = QString::fromUtf8(key.data(), key.size());
      QStringList typeAndID = qkey.split(" ");
      QString IDString = typeAndID[1].trimmed();
      QFileInfo docPath(asQString(value));
      QString filename = docPath.completeBaseName();
      if (typeAndID[0] == "Swc") {
        sceneObj["Doc"].as_object().at(key) = json::value_from(swcName);
      } else if (typeAndID[0] == "Puncta") {
        sceneObj["Doc"].as_object().at(key) = json::value_from(punctaName);
      }
      sceneObj[IDString.toStdString()].as_object().at("View3D").as_object().erase("X Cut FloatSpan");
      sceneObj[IDString.toStdString()].as_object().at("View3D").as_object().erase("Y Cut FloatSpan");
      sceneObj[IDString.toStdString()].as_object().at("View3D").as_object().erase("Z Cut FloatSpan");
    }
    QString scnName = QString("/Users/feng/Downloads/cell_table/%1.scene").arg(cellName);

    json::object saveObj;
    saveObj["Scene"] = sceneObj;

    saveJsonObject(saveObj, scnName);

    mainWin->removeAllObjs();
    mainWin->loadJsonScene(scnName);
    QApplication::processEvents();

    Z3DRenderingEngine* view3d = mainWin->get3DWindow()->engine();
    view3d->resetCamera();
    view3d->zoomIn();
    view3d->zoomIn();
    QApplication::processEvents();
    QString imgName = QString("/Users/feng/Downloads/cell_table/%1.tif").arg(cellName);
    view3d->takeFixedSizeScreenShot(imgName, 512, 512, Z3DScreenShotType::MonoView);
    QApplication::processEvents();
  }

  for (auto it : cells) {
    QString cellType = std::get<0>(it.first);
    // cellType.chop(2);
    QString cellName = std::get<4>(it.first);
    QString somaLocation = std::get<0>(it.second);
    if (somaLocation == "Ori") {
      somaLocation = "oriens";
    }
    if (somaLocation == "Rad") {
      somaLocation = "radiatum";
    }
    double AP = std::get<2>(it.first);
    double ML = std::get<3>(it.first);
    double r2 = std::get<1>(it.second);
    QString row = QString("%1 & (%2, %3)$mm$ & %4 & %5 & \\parbox[c]{0.5in}{\\includegraphics[height=0.5in]{%6}} \\\\")
                    .arg(cellType)
                    .arg(AP)
                    .arg(ML, 0, 'g', 3)
                    .arg(somaLocation.toLower())
                    .arg(r2, 0, 'g', 3)
                    .arg(cellName);
    LOG(INFO) << row;
  }
}

void checkSWC()
{
  ZMainWindow* mainWin = nullptr;
  for (auto widget : QApplication::topLevelWidgets()) {
    mainWin = qobject_cast<ZMainWindow*>(widget);
    if (mainWin) {
      break;
    }
  }
  ZView* view = mainWin->view();

  ZSwc swc("/Users/feng/Documents/bigimage_new/0515_15Py.swc");

  for (auto node : swc) {
    view->gotoPosition(node.x, node.y, node.z);
    QApplication::processEvents();
  }
}

void testLogSpeed()
{
  QStringList logList;
  for (auto i = 0; i < 500000; ++i) {
    logList << randomString(10, 100);
  }
  ZBenchTimer bt;
  for (const auto& i : logList) {
    LOG(INFO) << i;
  }
  STOP_AND_VLOG(bt)
}

void tmp()
{
  float v;
  bool ok;
  float fm;
  QString fms;

  v = std::numeric_limits<float>::max();
  fms = QString::number(v, 'g', QLocale::FloatingPointShortest);
  fm = fms.toFloat(&ok);
  LOG(INFO) << fms << " " << fm << " " << ok << "  " << (fm == v) << " " << (fm == double(v));

  v = 0.1;
  fms = QString::number(v, 'g', QLocale::FloatingPointShortest);
  fm = fms.toFloat(&ok);
  LOG(INFO) << fms << " " << fm << " " << ok << "  " << (fm == v) << " " << (fm == double(v));

  v = 0.2;
  fms = QString::number(v, 'g', QLocale::FloatingPointShortest);
  fm = fms.toFloat(&ok);
  LOG(INFO) << fms << " " << fm << " " << ok << "  " << (fm == v) << " " << (fm == double(v));

  v = 0.8;
  fms = QString::number(v, 'g', QLocale::FloatingPointShortest);
  fm = fms.toFloat(&ok);
  LOG(INFO) << fms << " " << fm << " " << ok << "  " << (fm == v) << " " << (fm == double(v));

  //  using namespace boost::multiprecision;
  //
  //  boost::multiprecision::int128_t res =
  //    static_cast<boost::multiprecision::int128_t>(INT64_MIN) * static_cast<boost::multiprecision::int128_t>(1);
  //  int64_t r = res < static_cast<boost::multiprecision::int128_t>(INT64_MIN) ? INT64_MIN :
  //              res > static_cast<boost::multiprecision::int128_t>(INT64_MAX) ? INT64_MAX :
  //              static_cast<int64_t>(res);
  //  std::cout << res << std::endl;
  //
  //  std::cout << r << " "
  //            << (res < static_cast<boost::multiprecision::int128_t>(INT64_MIN)) << " "
  //            << INT64_MIN << " "
  //            << (res > static_cast<boost::multiprecision::int128_t>(INT64_MAX)) << " "
  //            << static_cast<int64_t>(res) << " "
  //            << res << std::endl;
}

void fixImg()
{
  ZImgRegion rgn1;
  rgn1.start.z = 0;
  rgn1.end.z = 76;
  ZImg img1("/Users/feng/Downloads/ChaehyunImage/c_fix.tif", rgn1);
  ZImgRegion rgn2;
  rgn2.start.z = 76;
  ZImg img2("/Users/feng/Downloads/ChaehyunImage/c_fix.tif", rgn2);
  ZImg img = ZImg::cat(std::vector<const ZImg*>{&img2, &img1}, Dimension::Z);
  img.save("/Users/feng/Downloads/ChaehyunImage/c_fix2.tif");
}

void qFileInfoTest()
{
  QString fn("/Users/feng/Downloads/non-ext.tif");
  QString fon("/Users/feng/Downloads/non-exist/");
  QString fon1("/Users/feng/Downloads/");
  QString fon2("/Users/feng/Downloads");
  LOG(INFO) << QFileInfo(fn).canonicalPath();
  LOG(INFO) << QFileInfo(fn).canonicalFilePath();
  LOG(INFO) << QFileInfo(fn).absolutePath();
  LOG(INFO) << QFileInfo(fn).absoluteFilePath();
  LOG(INFO) << QFileInfo(fon).isDir();
  LOG(INFO) << QFileInfo(fon).canonicalPath();
  LOG(INFO) << QFileInfo(fon).canonicalFilePath();
  LOG(INFO) << QFileInfo(fon).absolutePath();
  LOG(INFO) << QFileInfo(fon).absoluteFilePath();
  LOG(INFO) << QFileInfo(fon1).isDir();
  LOG(INFO) << QFileInfo(fon1).canonicalPath();
  LOG(INFO) << QFileInfo(fon1).canonicalFilePath();
  LOG(INFO) << QFileInfo(fon1).absolutePath();
  LOG(INFO) << QFileInfo(fon1).absoluteFilePath();
  LOG(INFO) << QFileInfo(fon2).isDir();
  LOG(INFO) << QFileInfo(fon2).canonicalPath();
  LOG(INFO) << QFileInfo(fon2).canonicalFilePath();
  LOG(INFO) << QFileInfo(fon2).absolutePath();
  LOG(INFO) << QFileInfo(fon2).absoluteFilePath();
}

void GMMFail()
{
  Eigen::MatrixXd data(20, 3);
  data << 0, 0, 1, 0, 1, 0, 0, 1, 1, 0, 1, 2, 0, 1, 3, 0, 2, 0, 0, 2, 1, 0, 2, 2, 0, 2, 3, 0, 2, 4, 0, 3, 1, 0, 3, 2, 0,
    3, 3, 0, 3, 4, 0, 3, 5, 0, 4, 2, 0, 4, 3, 0, 4, 4, 0, 4, 5, 1, 2, 3;
  Eigen::VectorXd weight(20);
  weight << 22, 41, 42, 35, 25, 39, 25, 30, 26, 19, 25, 29, 31, 43, 35, 27, 35, 34, 36, 37;

  Eigen::RowVectorXd dataCentre = ZEigenUtils::featureMean(data, weight);
  Eigen::MatrixXd m = dataCentre.colwise().replicate(3);

  ZVBGMM<double, double>
    vbgmm(data, weight, 3, 10, m, 0.001, ZTermCriteria<double>(200, 1e-5), IterAlgorithmLogLevel::Iter);
  vbgmm.runEM(false);

  Eigen::MatrixXd mat(3, 3);
  mat << 08.99617e-239, -1.79923e-238, 08.78989e-248, -1.79923e-238, 03.59847e-238, -1.75798e-247, 08.78989e-248,
    -1.75798e-247, 08.78989e-248;
  Eigen::JacobiSVD<Eigen::MatrixXd> svd(mat);
  LOG(INFO) << svd.rank();
  LOG(INFO) << std::numeric_limits<double>::min();
  LOG(INFO) << std::numeric_limits<double>::epsilon();
  LOG(INFO) << svd.threshold();
  LOG(INFO) << svd.singularValues();
}

void detectPuncta()
{
  //  QDir dir("/Volumes/shared/Jiwon/Zeiss Confocal Microscopy/RNAscope/JK574-1_RNAscope_PV-Gria1-Gabra1");
  //
  //  QStringList filters;
  //  filters << "JK574-1-7_left*";
  //  QFileInfoList fdlist = dir.entryInfoList(filters, QDir::Dirs | QDir::NoSymLinks);
  //  filters.clear();
  //  filters << "JK584-nonono*";
  //  QFileInfoList fdlist2 = QDir(
  //    "/Volumes/shared/Jiwon/Zeiss Confocal
  //    Microscopy/RNAscope/JK584-6_RNAscope_PV-Gria1-Gabra1").entryInfoList(filters,
  //                                                                                                               QDir::Dirs
  //                                                                                                               |
  //                                                                                                               QDir::NoSymLinks);
  //  fdlist.append(fdlist2);

  QDir dir(
    "/Volumes/shared/Jiwon/Zeiss Confocal Microscopy/RNAscope/JK584-6_RNAscope_PV-Gria1-Gabra1/180727_JK586-6_referenceRegions");

  QStringList filters;
  filters << "JK586-6*";
  QFileInfoList fdlist = dir.entryInfoList(filters, QDir::Dirs | QDir::NoSymLinks);

  filters.clear();
  filters << "*.nim";
  for (const auto& i : fdlist) {
    QDir fdir(i.absoluteFilePath());
    QFileInfoList list = fdir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks);
    if (list.size() == 1) {
      LOG(INFO) << list.at(0).absoluteFilePath();

      for (size_t ch = 1; ch < 4; ++ch) {
        QString pfn =
          QString("%1/%2_ch%3.nimp").arg(i.absoluteFilePath()).arg(list.at(0).completeBaseName()).arg(ch + 1);
        if (QFile::exists(pfn)) {
          continue;
        }
        QString lfn =
          QString("%1/%2_ch%3_log.txt").arg(i.absoluteFilePath()).arg(list.at(0).completeBaseName()).arg(ch + 1);
        LOG(INFO) << pfn;
        ZPunctaDetection pd(list.at(0).absoluteFilePath(), ch);
        pd.setLogFile(lfn);
        pd.setResultPunctaFilename(pfn);
        pd.run();
      }
    }
  }
}

void changeDapifileType()
{
  QDir dir("/Users/feng/Documents/dapi_detection_dataset/JK575-1_RNAscope_PV-Gria1-Gabra1/");

  QStringList filters;
  filters << "JK575-*";
  QFileInfoList fdlist = dir.entryInfoList(filters, QDir::Dirs | QDir::NoSymLinks);
  filters.clear();
  filters << "*.nim";
  for (const auto& i : fdlist) {
    QDir fdir(i.absoluteFilePath());
    QFileInfoList list = fdir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks);
    if (list.size() == 1) {
      LOG(INFO) << list.at(0).absoluteFilePath();

      ZImg img(list.at(0).absoluteFilePath());
      img = img.maximumZProjection();
      img.save(fdir.filePath(list.at(0).completeBaseName() + ".tif"));
    }
  }
}

void convertPVRawToNim()
{
  QList<QStringList> metaData = QtCSV::Reader::readToList("/Volumes/shared/feng/Chris/pv_img/img_srclist.txt");
  ZImg refPYImg("/Volumes/shared/feng/Chris/slice15/slice15_L18_Sum.lsm");
  ZImg refPVImg("/Volumes/shared/os/PV/201602_contraPV/PV139/5/s9/PV139_5_s9_R1_GR1_B1_L19.lsm");
  for (index_t i = 0; i < metaData.size(); ++i) {
    QFileInfo fileInfo = QFileInfo(metaData[i][0]);
    LOG(INFO) << i << " " << metaData.size() << " " << fileInfo.absoluteFilePath();

    ZImg img(fileInfo.absoluteFilePath());
    if (fileInfo.baseName().startsWith("Py")) {
      img.infoRef().channelColors[0] = col4{0, 255, 0};
      img.infoRef().channelColors[1] = col4{255, 0, 0};
      img.infoRef().channelColors[2] = col4{0, 0, 255};
      img.infoRef().channelNames[0] = refPYImg.channelName(0);
      img.infoRef().channelNames[1] = refPYImg.channelName(1);
      img.infoRef().channelNames[2] = refPYImg.channelName(2);
      img.infoRef().voxelSizeUnit = refPYImg.voxelSizeUnit();
      img.infoRef().voxelSizeX = refPYImg.voxelSizeX();
      img.infoRef().voxelSizeY = refPYImg.voxelSizeY();
      img.infoRef().voxelSizeZ = refPYImg.voxelSizeZ();
    } else if (fileInfo.baseName().startsWith("PV")) {
      img.infoRef().channelColors[0] = col4{0, 0, 255};
      img.infoRef().channelColors[1] = col4{0, 255, 0};
      img.infoRef().channelColors[2] = col4{255, 0, 0};
      img.infoRef().channelNames[0] = refPVImg.channelName(0);
      img.infoRef().channelNames[1] = refPVImg.channelName(1);
      img.infoRef().channelNames[2] = refPVImg.channelName(2);
      img.infoRef().voxelSizeUnit = refPVImg.voxelSizeUnit();
      img.infoRef().voxelSizeX = refPVImg.voxelSizeX();
      img.infoRef().voxelSizeY = refPVImg.voxelSizeY();
      img.infoRef().voxelSizeZ = refPVImg.voxelSizeZ();
    } else {
      CHECK(false);
    }
    QDir outpath("/Volumes/shared/feng/Chris/pv_img");
    QString outname = outpath.filePath(fileInfo.baseName() + ".nim");
    CHECK(outname.endsWith(".nim"));
    LOG(INFO) << "to " << outname;
    img.save(outname);
  }
}

void convertPYRawToNim()
{
  QDir dir("/Volumes/shared/feng/Chris/py_img_old");
  QStringList filters;
  filters << "*.nim";
  QFileInfoList list = dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks);
  ZImg refPYImg("/Volumes/shared/feng/Chris/slice15/slice15_L18_Sum.lsm");
  ZImg refPVImg("/Volumes/shared/os/PV/201602_contraPV/PV139/5/s9/PV139_5_s9_R1_GR1_B1_L19.lsm");

  for (index_t i = 0; i < list.size(); ++i) {
    auto fileInfo = list.at(i);
    LOG(INFO) << i << " " << list.size() << " " << fileInfo.absoluteFilePath();
    ZImg img(fileInfo.absoluteFilePath());
    if (fileInfo.baseName().startsWith("Py")) {
      img.infoRef().channelColors[0] = col4{0, 255, 0};
      img.infoRef().channelColors[1] = col4{255, 0, 0};
      img.infoRef().channelColors[2] = col4{0, 0, 255};
      img.infoRef().channelNames[0] = refPYImg.channelName(0);
      img.infoRef().channelNames[1] = refPYImg.channelName(1);
      img.infoRef().channelNames[2] = refPYImg.channelName(2);
      img.infoRef().voxelSizeUnit = refPYImg.voxelSizeUnit();
      img.infoRef().voxelSizeX = refPYImg.voxelSizeX();
      img.infoRef().voxelSizeY = refPYImg.voxelSizeY();
      img.infoRef().voxelSizeZ = refPYImg.voxelSizeZ();

      auto tmpimg = ZImg::cat(std::vector<ZImg>{img.createView(2), img.createView(0), img.createView(1)}, Dimension::C);
      img.swap(tmpimg);
    } else if (fileInfo.baseName().startsWith("PV")) {
      img.infoRef().channelColors[0] = col4{0, 0, 255};
      img.infoRef().channelColors[1] = col4{0, 255, 0};
      img.infoRef().channelColors[2] = col4{255, 0, 0};
      img.infoRef().channelNames[0] = refPVImg.channelName(0);
      img.infoRef().channelNames[1] = refPVImg.channelName(1);
      img.infoRef().channelNames[2] = refPVImg.channelName(2);
      img.infoRef().voxelSizeUnit = refPVImg.voxelSizeUnit();
      img.infoRef().voxelSizeX = refPVImg.voxelSizeX();
      img.infoRef().voxelSizeY = refPVImg.voxelSizeY();
      img.infoRef().voxelSizeZ = refPVImg.voxelSizeZ();
    } else {
      CHECK(false);
    }
    QDir outpath("/Volumes/shared/feng/Chris/py_img");
    QString outname = outpath.filePath(fileInfo.fileName());
    CHECK(outname.endsWith(".nim"));
    LOG(INFO) << "to " << outname;
    img.save(outname);
  }
}

void channelCalibration()
{
  QDir dir("/Users/feng/Google Drive/confocal calibration");
  QStringList filters;
  filters << "*.lsm";
  QFileInfoList list = dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks);

  for (index_t i = 0; i < list.size(); ++i) {
    auto fileInfo = list.at(i);
    LOG(INFO) << i << " " << list.size() << " " << fileInfo.absoluteFilePath();
    ZImg img(fileInfo.absoluteFilePath());
    ZImg fixed = img.extractChannel(0);
    fixed.save(fileInfo.absoluteFilePath() + "_ch0.tif");
    ZImg moving = img.extractChannel(1);
    moving.save(fileInfo.absoluteFilePath() + "_ch1.tif");
    ZImgNCCMatch mat(fixed, moving);
    LOG(INFO) << fmt::format("{}", mat.computeMovingImgOffset());
  }
}

void createPunctaMesh()
{
  ZPuncta pun("/Users/feng/Documents/PY/PySWC/Py0515_s15_1_1_1c_puncta.apo");
  ZMesh mesh;
  glm::mat4 tfm(1.f);
  tfm[2][2] = 5.f;
  ZMesh::createPunctaMesh(pun, mesh, 8, tfm);
  mesh.save("/Users/feng/Google Drive/eeum/scene/Py0515_s15_1_1_1c_puncta.stl");
}

void createGlanceThumbnails()
{
  ZMainWindow* mainWin = nullptr;
  for (auto widget : QApplication::topLevelWidgets()) {
    mainWin = qobject_cast<ZMainWindow*>(widget);
    if (mainWin) {
      break;
    }
  }

  std::vector<QString> templateName;
  templateName.emplace_back("pc_thumbnail_template.scene");
  templateName.emplace_back("pv_thumbnail_template.scene");
  std::vector<QString> swcDirs;
  swcDirs.emplace_back("/Users/feng/Documents/PY/PySWC");
  swcDirs.emplace_back("/Users/feng/Documents/PV/PVSWC");
  std::vector<QString> punctaSuffix;
  punctaSuffix.emplace_back("_puncta.apo");
  punctaSuffix.emplace_back("_neurite.nimp");

  for (size_t ti = 0; ti < templateName.size(); ++ti) {
    auto loadObj = loadJsonObject(QString("/Users/feng/Google Drive/eeum/raw/%1").arg(templateName[ti]));
    if (!loadObj.contains("Scene") || !loadObj.at("Scene").is_object()) {
      return;
    }
    auto& sceneObj = loadObj["Scene"].as_object();
    auto& docObject = sceneObj["Doc"].as_object();

    QDir dir(swcDirs[ti]);
    QStringList filters;
    filters << "*_layer.swc";
    QFileInfoList list = dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks);

    for (index_t i = 0; i < list.size(); ++i) {
      auto fileInfo = list.at(i);
      LOG(INFO) << i << " " << list.size() << " " << fileInfo.absoluteFilePath();

      QString cellName = fileInfo.completeBaseName();
      cellName.replace("_layer", "");

      QString swcName = QString("%1/%2.swc").arg(swcDirs[ti]).arg(cellName);
      QString punctaName = QString("%1/%2%3").arg(swcDirs[ti]).arg(cellName).arg(punctaSuffix[ti]);

      for (auto& [key, value] : docObject) {
        QString qkey = QString::fromUtf8(key.data(), key.size());
        QStringList typeAndID = qkey.split(" ");
        QString IDString = typeAndID[1].trimmed();
        QFileInfo docPath(asQString(value));
        QString filename = docPath.completeBaseName();
        if (typeAndID[0] == "Swc") {
          sceneObj["Doc"].as_object().at(key) = json::value_from(swcName);
        } else if (typeAndID[0] == "Puncta") {
          sceneObj["Doc"].as_object().at(key) = json::value_from(punctaName);
        }
        sceneObj[IDString.toStdString()].as_object().at("View3D").as_object().erase("X Cut FloatSpan");
        sceneObj[IDString.toStdString()].as_object().at("View3D").as_object().erase("Y Cut FloatSpan");
        sceneObj[IDString.toStdString()].as_object().at("View3D").as_object().erase("Z Cut FloatSpan");
      }
      QString scnName = QString("/Users/feng/Google Drive/eeum/raw/thumbnails/%1.scene").arg(cellName);

      json::object saveObj;
      saveObj["Scene"] = sceneObj;
      saveJsonObject(saveObj, scnName);

      mainWin->removeAllObjs();
      mainWin->loadJsonScene(scnName);
      QApplication::processEvents();

      Z3DRenderingEngine* view3d = mainWin->get3DWindow()->engine();
      view3d->resetCamera();
      view3d->zoomIn();
      // view3d->zoomInAction()->trigger();
      QApplication::processEvents();
      QString imgName = QString("/Users/feng/Google Drive/eeum/raw/thumbnails/%1.tif").arg(cellName);
      view3d->takeFixedSizeScreenShot(imgName, 1024, 1024, Z3DScreenShotType::MonoView);
      QApplication::processEvents();
    }
  }
}

void exportSceneForGlance()
{
  ZMainWindow* mainWin = nullptr;
  for (auto widget : QApplication::topLevelWidgets()) {
    mainWin = qobject_cast<ZMainWindow*>(widget);
    if (mainWin) {
      break;
    }
  }

  QStringList cellnames;

  Z3DRenderingEngine* view3d = mainWin->get3DWindow()->engine();
  for (auto& ojbview : view3d->objViews()) {
    if (auto meshView = dynamic_cast<Z3DMeshView*>(ojbview.get())) {
      auto doc = const_cast<ZMeshDoc*>(qobject_cast<const ZMeshDoc*>(&meshView->doc()));
      CHECK(doc);
      for (auto& kv : meshView->idToFilter()) {
        size_t id = kv.first;
        auto filter = kv.second.get();
        LOG(INFO) << doc->objName(id);
        LOG(INFO) << filter->coordTransform();
        auto meshList = doc->meshList(id);
        QString name = QString("/Users/feng/Google Drive/eeum/static/data/%1.vtp").arg(doc->objName(id));
        ZMesh mesh = *meshList->at(0);
        mesh.transformVerticesByMatrix(filter->coordTransform());
        mesh.generateNormals();
        mesh.save(name);
      }
    } else if (auto swcView = dynamic_cast<Z3DSwcView*>(ojbview.get())) {
      auto doc = const_cast<ZSwcDoc*>(qobject_cast<const ZSwcDoc*>(&swcView->doc()));
      CHECK(doc);
      for (auto& kv : swcView->idToFilter()) {
        size_t id = kv.first;
        auto filter = kv.second.get();
        LOG(INFO) << doc->objName(id);
        LOG(INFO) << filter->coordTransform();

        ZSwc swc = doc->swcPack(id).swc();
        swc.labelSomaAndOthers();
        ZMesh rootMesh;
        ZMesh somaMesh;
        ZMesh branchMesh;
        ZMesh::createSwcMesh(swc, 1, rootMesh, somaMesh, branchMesh, filter->coordTransform());
        rootMesh.generateNormals();
        somaMesh.generateNormals();
        branchMesh.generateNormals();
        rootMesh.save(QString("/Users/feng/Google Drive/eeum/static/data/%1_root.vtp").arg(doc->objName(id)));
        somaMesh.save(QString("/Users/feng/Google Drive/eeum/static/data/%1_soma.vtp").arg(doc->objName(id)));
        branchMesh.save(QString("/Users/feng/Google Drive/eeum/static/data/%1_neurite.vtp").arg(doc->objName(id)));
        QString cellname = doc->objName(id);
        cellnames.push_back(cellname);
      }
    } else if (auto punctaView = dynamic_cast<Z3DPunctaView*>(ojbview.get())) {
      auto doc = const_cast<ZPunctaDoc*>(qobject_cast<const ZPunctaDoc*>(&punctaView->doc()));
      CHECK(doc);
      for (auto& kv : punctaView->idToFilter()) {
        size_t id = kv.first;
        auto filter = kv.second.get();
        LOG(INFO) << doc->objName(id);
        LOG(INFO) << filter->coordTransform();

        auto& puncta = doc->punctaPack(id);
        ZMesh mesh;
        ZMesh::createPunctaMesh(puncta.puncta(), mesh, 8, filter->coordTransform());
        mesh.generateNormals();
        mesh.save(QString("/Users/feng/Google Drive/eeum/static/data/%1.vtp").arg(doc->objName(id)));
      }
    }
  }

  json::array allObjs;
  for (auto cellname : cellnames) {
    QStringList fileList;
    QString somaMeshName = QString("/Users/feng/Google Drive/eeum/static/data/%1_soma.vtp").arg(cellname);
    fileList.push_back(somaMeshName);
    QString somaMeshFileName = QString("%1_soma.vtp").arg(cellname);
    QString branchMeshName = QString("/Users/feng/Google Drive/eeum/static/data/%1_neurite.vtp").arg(cellname);
    fileList.push_back(branchMeshName);
    QString branchMeshFileName = QString("%1_neurite.vtp").arg(cellname);
    QString rootMeshName = QString("/Users/feng/Google Drive/eeum/static/data/%1_root.vtp").arg(cellname);
    QString rootMeshFileName = QString("%1_root.vtp").arg(cellname);
    LOG(INFO) << cellname;
    auto truncatePos = cellname.indexOf("_layer.swc", Qt::CaseInsensitive);
    if (truncatePos < 0) {
      truncatePos = cellname.indexOf(".swc", Qt::CaseInsensitive);
    }
    LOG(INFO) << truncatePos;
    cellname.truncate(truncatePos);
    LOG(INFO) << cellname;
    QString punctaMeshName = QString("/Users/feng/Google Drive/eeum/static/data/%1_puncta.apo.vtp").arg(cellname);
    QString punctaMeshFileName = QString("%1_puncta.apo.vtp").arg(cellname);
    if (!QFile::exists(punctaMeshName)) {
      punctaMeshName = QString("/Users/feng/Google Drive/eeum/static/data/%1_neurite.nimp.vtp").arg(cellname);
      punctaMeshFileName = QString("%1_neurite.nimp.vtp").arg(cellname);
    }
    CHECK(QFile::exists(punctaMeshName)) << punctaMeshName;
    fileList.push_back(punctaMeshName);

    mainWin->removeAllObjs();
    mainWin->loadFileList(fileList);
    QApplication::processEvents();

    view3d->resetCamera();
    view3d->zoomIn();
    // view3d->zoomInAction()->trigger();
    QApplication::processEvents();

    LOG(INFO) << "position: " << view3d->camera().get().eye();
    LOG(INFO) << "focalPoint: " << view3d->camera().get().center();

    json::object obj;
    obj["cellname"] = json::value_from(cellname);
    obj["soma"] = json::value_from(somaMeshFileName);
    obj["neurite"] = json::value_from(branchMeshFileName);
    obj["root"] = json::value_from(rootMeshFileName);
    obj["puncta"] = json::value_from(punctaMeshFileName);
    json::array arr;
    arr.push_back(view3d->camera().get().eye().x);
    arr.push_back(view3d->camera().get().eye().y);
    arr.push_back(view3d->camera().get().eye().z);
    obj["camera position"] = arr;

    json::array arr2;
    arr2.push_back(view3d->camera().get().center().x);
    arr2.push_back(view3d->camera().get().center().y);
    arr2.push_back(view3d->camera().get().center().z);
    obj["camera focalPoint"] = arr2;

    allObjs.push_back(obj);
  }

  QString scnName = QString("/Users/feng/Google Drive/eeum/raw/meshes/allObjs.json");

  saveJsonArray(allObjs, scnName);
}

void stitchAndDetectPuncta()
{
  QDir dir("/Volumes/shared/Jiwon/Zeiss Confocal Microscopy/RNAscope/JK574-1_RNAscope_PV-Gria1-Gabra1");
  QStringList filters;
  filters << "JK574*";
  QFileInfoList fdlist = dir.entryInfoList(filters, QDir::Dirs | QDir::NoSymLinks);

  dir = QDir("/Volumes/shared/Jiwon/Zeiss Confocal Microscopy/RNAscope/JK575-1_RNAscope_PV-Gria1-Gabra1");
  filters.clear();
  filters << "JK575*";
  QFileInfoList fdlist2 = dir.entryInfoList(filters, QDir::Dirs | QDir::NoSymLinks);
  fdlist.append(fdlist2);

  dir = QDir("/Volumes/shared/Jiwon/Zeiss Confocal Microscopy/RNAscope/JK584-6_RNAscope_PV-Gria1-Gabra1");
  filters.clear();
  filters << "JK584*";
  fdlist2 = dir.entryInfoList(filters, QDir::Dirs | QDir::NoSymLinks);
  fdlist.append(fdlist2);

  dir = QDir("/Volumes/shared/Jiwon/Zeiss Confocal Microscopy/RNAscope/JK636-1_RNAscope_PV-Chrnb2-Chrna7");
  filters.clear();
  filters << "JK636*";
  fdlist2 = dir.entryInfoList(filters, QDir::Dirs | QDir::NoSymLinks);
  fdlist.append(fdlist2);

  dir = QDir("/Volumes/shared/Jiwon/Zeiss Confocal Microscopy/RNAscope/JK636-1_RNAscope_PV-Drd1-Drd2");
  filters.clear();
  filters << "JK636*";
  fdlist2 = dir.entryInfoList(filters, QDir::Dirs | QDir::NoSymLinks);
  fdlist.append(fdlist2);

  dir = QDir("/Volumes/shared/Jiwon/Zeiss Confocal Microscopy/RNAscope/JK656-1_RNAscope_PV-Gria23-Gabra1");
  filters.clear();
  filters << "JK656*";
  fdlist2 = dir.entryInfoList(filters, QDir::Dirs | QDir::NoSymLinks);
  fdlist.append(fdlist2);

  dir = QDir("/Volumes/shared/Jiwon/Zeiss Confocal Microscopy/RNAscope/JK699-4_RNAscope_Slc17a6-Rbfox3-Slc32a1");
  filters.clear();
  filters << "JK699*";
  fdlist2 = dir.entryInfoList(filters, QDir::Dirs | QDir::NoSymLinks);
  fdlist.append(fdlist2);

  dir = QDir("/Volumes/shared/Jiwon/Zeiss Confocal Microscopy/RNAscope/JK699-5_RNAscope_PV-Slc17a6-Slc32a1");
  filters.clear();
  filters << "JK699*";
  fdlist2 = dir.entryInfoList(filters, QDir::Dirs | QDir::NoSymLinks);
  fdlist.append(fdlist2);

  filters.clear();
  filters << "*_Sum.lsm";
  for (const auto& i : fdlist) {
    QDir fdir(i.absoluteFilePath());
    QString tsfn = fdir.filePath("TileSelection.lsm");
    if (!QFile::exists(tsfn)) {
      LOG(WARNING) << fdir.absolutePath() << " no tile selection file";
      continue;
    }
    QFileInfoList list = fdir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks);
    if (list.empty()) {
      LOG(WARNING) << fdir.absolutePath() << " not enough lsm file for stitching";
      continue;
    }

    QStringList inputFiles;
    for (const auto& fi : list) {
      inputFiles.push_back(fi.absoluteFilePath());
    }

    QString outputName = inputFiles[0];
    outputName.truncate(outputName.lastIndexOf("_L"));
    for (index_t j = 1; j < list.size(); ++j) {
      QString tmpfn = inputFiles[j];
      tmpfn.truncate(tmpfn.lastIndexOf("_L"));
      CHECK(outputName == tmpfn);
    }
    outputName += ".nim";
    LOG(INFO) << "output: " << outputName;
    if (QFile::exists(outputName)) {
      LOG(INFO) << "finished, skip stitching.";
    } else {
      ZStitchImage stitch;
      stitch.setInputFilenames(inputFiles);
      stitch.setTileGridFromTileSelectionImage(tsfn);
      stitch.setMergeMode(ImgMergeMode::First);
      stitch.setResultFilename(outputName);
      std::vector<size_t> chs;
      chs.push_back(0_uz);
      stitch.setUseChannels(chs);
      stitch.setMaxOverlapRate(0.15);
      QString lfn = QString("%1_stitching_log.txt").arg(outputName);
      stitch.setLogFile(lfn);

      stitch.run();
    }

    for (size_t ch = 1; ch < 4; ++ch) {
      QString pfn =
        QString("%1/%2_ch%3.nimp").arg(fdir.absolutePath()).arg(QFileInfo(outputName).completeBaseName()).arg(ch + 1);
      QString lfn = QString("%1/%2_ch%3_log.txt")
                      .arg(fdir.absolutePath())
                      .arg(QFileInfo(outputName).completeBaseName())
                      .arg(ch + 1);
      if (QFile::exists(pfn) && QFile::exists(lfn)) {
        LOG(INFO) << "finished, skip puncta detection.";
        continue;
      }
      LOG(INFO) << pfn;
      ZPunctaDetection pd(outputName, ch);
      pd.setLogFile(lfn);
      pd.setResultPunctaFilename(pfn);
      pd.run();
    }
  }
}

void testDetectPuncta()
{
  ZPunctaDetection pd("/Users/feng/Documents/bigimage_new/0515_3to33_crop.raw", 0);
  pd.setLogFile("/Users/feng/Documents/bigimage_new/0515_3to33_crop.raw_puncta.log");
  pd.setResultPunctaFilename("/Users/feng/Documents/bigimage_new/0515_3to33_crop.raw_puncta.nimp");
  pd.run();

  ZPunctaDetection pd2("/Users/feng/Documents/bigimage_new/0515_3to33_crop.raw", 0);
  pd2.setLogFile("/Users/feng/Documents/bigimage_new/0515_3to33_crop.raw_puncta.log");
  pd2.setResultPunctaFilename("/Users/feng/Documents/bigimage_new/0515_3to33_crop.raw_puncta.nimp");
  pd2.run();

  LOG(WARNING) << "abc";
}

void cutZeroRegion()
{
  QDir dir("/Volumes/shared/HJ/Confocal/CLARITY image/20190107_PV+_STN");
  QStringList filters;
  filters << "*.nim";
  QFileInfoList list = dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks);

  for (index_t i = 0; i < list.size(); ++i) {
    auto fileInfo = list.at(i);
    LOG(INFO) << i << " " << list.size() << " " << fileInfo.absoluteFilePath();
    ZImg img(fileInfo.absoluteFilePath(), ZImgRegion(0, -1, 0, -1, 1100, 1800));
    img.save(fileInfo.absoluteFilePath() + "_zcut.nim");
  }
}

void swapMeshXY()
{
  QDir dir(
    "/Users/feng/Library/Application Support/Brain Explorer 2/Atlases/Allen Mouse Brain Common Coordinate Framework/Spaces/P56/Meshes");
  QStringList filters;
  filters << "*.msh";
  QFileInfoList list = dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks);
  QString outFolder = "/Volumes/fs3017/eeum/AllenMouseBrainCommonCoordinateFrameworkSpacesP56MeshesSwapXY";

  for (auto& fileInfo : list) {
    ZMesh msh(fileInfo.absoluteFilePath());
    msh.swapXY();

    msh.save(QString("%1/%2.stl").arg(outFolder).arg(fileInfo.fileName()));
  }
}

void createPCCellTable()
{
  QList<QStringList> metaData =
    QtCSV::Reader::readToList("/Users/feng/code/mgrasp-analysis/pv_figs_1.0/orig_cell_props.csv");
  metaData.removeFirst();

  std::map<QString, int> somaLocationMap;
  somaLocationMap["Ori"] = 1;
  somaLocationMap["Deep"] = 2;
  somaLocationMap["Superficial"] = 3;
  somaLocationMap["Rad"] = 4;

  ZMainWindow* mainWin = nullptr;
  for (auto widget : QApplication::topLevelWidgets()) {
    mainWin = qobject_cast<ZMainWindow*>(widget);
    if (mainWin) {
      break;
    }
  }

  auto loadObj = loadJsonObject("/Users/feng/Downloads/template_cell.scene");
  if (!loadObj.contains("Scene") || !loadObj.at("Scene").is_object()) {
    return;
  }
  auto& sceneObj = loadObj["Scene"].as_object();
  auto& docObject = sceneObj["Doc"].as_object();

  std::map<std::tuple<QString, int, double, double, QString>, std::tuple<QString, double>> cells;

  for (auto& metaIdx : metaData) {
    QString cellType = metaIdx[1];
    QString cellName = metaIdx[2];
    QString somaLocation = metaIdx[3];
    double AP = metaIdx[4].toDouble();
    double ML = metaIdx[5].toDouble();
    double r2 = metaIdx[27].toDouble();
    auto somaLocationOrder = somaLocationMap[somaLocation];
    assert(somaLocationOrder > 0 && somaLocationOrder < 5);

    if (cellType == "contraPV" || cellType == "ipsiPV") {
      continue;
    }
    cells[std::make_tuple(cellType, somaLocationOrder, AP, ML, cellName)] = std::make_tuple(somaLocation, r2);

    QString swcName = QString("/Volumes/shared/feng/PVPY/PY/PySWC/%1_layer.swc").arg(cellName);
    QString punctaName = QString("/Volumes/shared/feng/PVPY/PY/PySWC/%1_puncta_small.apo").arg(cellName);

    for (auto& [key, value] : docObject) {
      QString qkey = QString::fromUtf8(key.data(), key.size());
      QStringList typeAndID = qkey.split(" ");
      QString IDString = typeAndID[1].trimmed();
      QFileInfo docPath(asQString(value));
      QString filename = docPath.completeBaseName();
      if (typeAndID[0] == "Swc") {
        sceneObj["Doc"].as_object().at(key) = json::value_from(swcName);
      } else if (typeAndID[0] == "Puncta") {
        sceneObj["Doc"].as_object().at(key) = json::value_from(punctaName);
      }
      sceneObj[IDString.toStdString()].as_object().at("View3D").as_object().erase("X Cut FloatSpan");
      sceneObj[IDString.toStdString()].as_object().at("View3D").as_object().erase("Y Cut FloatSpan");
      sceneObj[IDString.toStdString()].as_object().at("View3D").as_object().erase("Z Cut FloatSpan");
    }
    QString scnName = QString("/Users/feng/Desktop/cell_table_pc/%1.scene").arg(cellName);

    json::object saveObj;
    saveObj["Scene"] = sceneObj;
    saveJsonObject(saveObj, scnName);

    mainWin->removeAllObjs();
    mainWin->loadJsonScene(scnName);
    QApplication::processEvents();

    Z3DRenderingEngine* view3d = mainWin->get3DWindow()->engine();
    view3d->resetCamera();
    view3d->zoomIn();
    view3d->zoomIn();
    QApplication::processEvents();
    QString imgName = QString("/Users/feng/Desktop/cell_table_pc/%1.tif").arg(cellName);
    view3d->takeFixedSizeScreenShot(imgName, 512, 512, Z3DScreenShotType::MonoView);
    QApplication::processEvents();
  }

  for (auto it : cells) {
    QString cellType = std::get<0>(it.first);
    // cellType.chop(2);
    QString cellName = std::get<4>(it.first);
    QString somaLocation = std::get<0>(it.second);
    if (somaLocation == "Ori") {
      somaLocation = "oriens";
    }
    if (somaLocation == "Rad") {
      somaLocation = "radiatum";
    }
    double AP = std::get<2>(it.first);
    double ML = std::get<3>(it.first);
    double r2 = std::get<1>(it.second);
    QString row = QString(R"(%1 & (%2, %3)$mm$ & %4 & %5 & \parbox[c]{0.5in}{\includegraphics[height=0.5in]{%6}} \\)")
                    .arg(cellType)
                    .arg(AP)
                    .arg(ML, 0, 'g', 3)
                    .arg(somaLocation.toLower())
                    .arg(r2, 0, 'g', 3)
                    .arg(cellName);
    LOG(INFO) << row;
  }
}

void testLogDataSupport()
{
  glm::vec4 v4(3.5, 2, 0, 1.);
  glm::mat4 m4;
  std::array<int, 3> a2 = {1, 2, 3};
  LOG(INFO) << v4;
  LOG(INFO) << m4;
  LOG(INFO) << json::value_from(a2);
  LOG(INFO) << fmt::format("{}", a2);
  auto t = std::make_tuple(1.7, 'D', std::string("Ralph Wiggum"));
  LOG(INFO) << json::value_from(t);
  LOG(INFO) << fmt::format("{}", t);
  const std::map<std::string, int> init{
    {"this",  100},
    {"can",   100},
    {"be",    100},
    {"const", 100},
  };
  LOG(INFO) << json::value_from(init);
  LOG(INFO) << fmt::format("{}", init);
  LOG(INFO) << v4 << m4 << json::value_from(a2) << json::value_from(t) << json::value_from(init);
  QString str("abdefwfgwtwfwfwfwgwgw");
  LOG(INFO) << json::value_from(str);
  LOG(INFO) << fmt::format("{}", str);
  QList<QKeySequence> zoomInKey;
  zoomInKey << QKeySequence::ZoomIn << QKeySequence(Qt::Key_Plus) << QKeySequence(Qt::Key_Equal);
  LOG(INFO) << zoomInKey;
  // LOG(INFO) << fmt::format("{}", zoomInKey);
}

void createEeumIndexImages()
{
  ZMainWindow* mainWin = nullptr;
  for (auto widget : QApplication::topLevelWidgets()) {
    mainWin = qobject_cast<ZMainWindow*>(widget);
    if (mainWin) {
      break;
    }
  }

  for (size_t ti = 0; ti < 180; ++ti) {
    mainWin->view()->slicePara().set(ti);
    QApplication::processEvents();
    auto imgName = QString("/Users/feng/code/eeum/static/slices/slice_%1.png").arg(ti);
    mainWin->view()->takeFixedSizeScreenShot(imgName, 512, 512);
    QApplication::processEvents();
  }
}

void imgResizeBenchmark()
{
  for (auto depth : std::vector({75, 175, 275, 375, 475, 600, 800, 1200, 1400, 2000, 2500, 5000, 10000})) {
    ZImg img = ZImg(ZImgInfo(64, 64, depth));
    img.fillRandom();
    ZBenchTimer bt;
    img.resize(64, 64, 64, Interpolant::Cubic, true, false, false);
    STOP_AND_VLOG(bt)
    if (img.depth() < 24) {
      LOG(INFO) << img.depth();
    }

    img = ZImg(ZImgInfo(64, 64, depth));
    img.fillRandom();
    bt.resetAndStart();
    img.resize(64, 64, 64);
    STOP_AND_VLOG(bt)
    if (img.depth() < 24) {
      LOG(INFO) << img.depth();
    }
  }
}

void someTest()
{
  // printStruct<EventListSegment>();
}

} // namespace nim

namespace nim {

void ZCustomCommand::run()
{
  testLogDataSupport();
  LOG(INFO) << "done";
}

} // namespace nim
