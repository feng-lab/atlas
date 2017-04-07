#include "zcustomcommand.h"

#include "zimg.h"
#include "zglmutils.h"
#include "zimagematrix3dtransform.h"
#include "zimgregistration.h"
#include "zregistrationnumericdiffcostfunction.h"
#include "zswc.h"
#include "zimgio.h"
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
#include "z3dview.h"
#include <include/qtcsv/reader.h>
#include <itkMath.h>
#include <QDir>
#include <QApplication>
#include <tbb/task_scheduler_init.h>

namespace nim {

void zoomPVRawImages()
{
  QDir dir("/Volumes/lq/pvdata/");
  QString outFolder("/Users/feng/Documents/image/High_LowPV_ipsi_contra/");
  QStringList filters;
  filters << "*.raw";
  QFileInfoList list = dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks);
  for (int i = 0; i < list.size(); ++i) {
    QFileInfo fileInfo = list.at(i);
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
  for (int i = 0; i < list.size(); ++i) {
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
  for (int i = 0; i < list.size(); ++i) {
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
  tfm.transformRange(0, img.width() - 1, 0, img.height() - 1, 0, img.depth() - 1, xMin, xMax, yMin, yMax, zMin, zMax);
  tfm.setTransform(tform);

  ZImg imgOut(ZImgInfo(std::ceil(xMax) - std::floor(xMin) + 1, std::ceil(yMax) - std::floor(yMin) + 1,
                       std::ceil(zMax) - std::floor(zMin) + 1, 3));
  tfm.transformImage(img.channelData<uint8_t>(0), img.width(), img.height(), img.depth(),
                     imgOut.channelData<uint8_t>(0), std::floor(xMin), std::ceil(xMax) + 1,
                     std::floor(yMin), std::ceil(yMax) + 1, std::floor(zMin), std::ceil(zMax) + 1);
  imgOut.save(
    "/Users/feng/Documents/image/SprengelMovies_Global_local/injection_site/stack_uncompressed/resliced_inj_red.tif");

  imgOut = ZImg(ZImgInfo(std::ceil(xMax) - std::floor(xMin) + 1, std::ceil(yMax) - std::floor(yMin) + 1,
                         std::ceil(zMax) - std::floor(zMin) + 1, 3));
  tfm.transformImage(img.channelData<uint8_t>(1), img.width(), img.height(), img.depth(),
                     imgOut.channelData<uint8_t>(1), std::floor(xMin), std::ceil(xMax) + 1,
                     std::floor(yMin), std::ceil(yMax) + 1, std::floor(zMin), std::ceil(zMax) + 1);
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
  LOG(INFO) << transform.toQString();
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
  for (int i = 0; i < list.size(); ++i) {
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
  ZImg img("/Users/feng/Documents/image/bigimage_new/0515_3to33.raw", ZImgRegion(0, -1, 0, -1, 0, -1, 1, 2));
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
  for (int i = 0; i < list.size(); ++i) {
    QFileInfo fileInfo = list.at(i);
    LOG(INFO) << i << " " << list.size() << " " << fileInfo.absoluteFilePath();
    ZImg img(fileInfo.absoluteFilePath(), ZImgRegion(0, -1, 0, -1, 0, -1, 1, 2));
    QString outname = outFolder + fileInfo.baseName() + "_ch2.v3draw";
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
  for (int i = 0; i < list.size(); ++i) {
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
  for (int i = 0; i < list.size(); ++i) {
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

  for (int i = 0; i < list.size(); ++i) {
    QFileInfo fileInfo = list.at(i);
    ZMesh msh(fileInfo.absoluteFilePath());

    std::vector<glm::vec3> vertices = msh.vertices();
    for (size_t j = 0; j < vertices.size(); ++j) {
      vertices[j] = glm::applyMatrix(mat, vertices[j]);
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

  for (int i = 0; i < list.size(); ++i) {
    QFileInfo fileInfo = list.at(i);
    ZMesh msh(fileInfo.absoluteFilePath());

    std::vector<glm::vec3> vertices = msh.vertices();
    for (size_t j = 0; j < vertices.size(); ++j) {
      vertices[j] = glm::applyMatrix(mat, vertices[j]);
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
  tbb::task_scheduler_init init(8);

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
    } else if (std::find(cortexIDs.begin(), cortexIDs.end(), annotation.value(i)) != cortexIDs.end()) {
      isoCortexMask.setValue(1, i);
    }
  }

  for (int i = 0; i < list.size(); ++i) {
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

  tbb::parallel_for(
    tbb::blocked_range<size_t>(0, exps.size()),
    [&](const tbb::blocked_range<size_t>& range) {
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

        if (projIdxs.empty())
          continue;

        ZImg injection(dir.filePath(str + "_" + resolution + "um_injection_density.nrrd"));
        injection *= mask;
        //injection *= isoCortexMask;

        std::vector<size_t> injectionIdxs;
        for (size_t i = 0; i < injection.voxelNumber(); ++i) {
          if (injection.value(i) > 0) {
            injectionIdxs.push_back(i);
          }
        }

        if (injectionIdxs.empty())
          continue;

#if 1
        projection = ZImg(dir.filePath(str + "_" + resolution + "um_projection_energy.nrrd"));
        projection *= mask;
        projection.typedUnaryOperation<float>([](float proj) { return std::max(255.f, proj); });
        //projection.typedBinaryOperation<float, float>(injection, [](float proj, float inj) { return inj > 0.f ? 0.f : proj; });
        projection.normalize();
#endif

        ZImgGraph imgGraph(projection);
        imgGraph.setConnectivity(26);
        ZImgAutoThreshold<> imgAutoThre;
        double cent1 = 0;
        double cent2 = 0;
        double thre1 = imgAutoThre.centroidThre<double>(cent1, cent2, projection);
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
            line.push_back(glm::dvec3(coord.x, coord.y, coord.z));
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
            swc.copy(swc.appendRoot(*oit), oit);
          } else if (!ZSwc::isNull(srcChild)) {
            swc.copy(swc.appendChild(desParent, *srcChild), srcChild);
          }
        }

        swc.resortID();
        if (!outFolder.exists(str + "_" + resolution + "um_stn_trace.swc")) {
          swc.save(outFolder.filePath(str + "_" + resolution + "um_stn_trace.swc"));
        }
      }
    }
  );
}

void mergeTraces()
{
  QDir dir("/Users/feng/Downloads/allen_stn_grid_traj_1");
  QStringList filters;
  filters << "*.swc";
  QFileInfoList list = dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks);
  QDir outFolder("/Users/feng/Downloads/allen_stn_grid_traj_clean");

  for (QFileInfo fileInfo : list) {
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
        swc.copy(swc.appendRoot(*oit), oit);
      } else if (!ZSwc::isNull(srcChild)) {
        swc.copy(swc.appendChild(desParent, *srcChild), srcChild);
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
  //LOG(INFO) << dirlist.size() << " " << dirlist.at(0).absolutePath();
  ZMesh rootMesh;
  ZMesh branchMesh;
  filters << "*c.swc";
  LOG(INFO) << "NameOfCell, SomaSurfaceArea, SomaVolume, NeuriteSurfaceArea, NeuriteVolume";
  for (int i = 0; i < dirlist.size(); ++i) {
    QFileInfo dirInfo = dirlist.at(i);
    QDir subDir(dirInfo.absoluteFilePath());
    QFileInfoList list = subDir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks);
    CHECK(list.size() == 1);
    QFileInfo fileInfo = list.at(0);
    ZMesh::createSwcMesh(ZSwc(fileInfo.absoluteFilePath()), 5, 1, rootMesh, branchMesh);
    rootMesh.save(outFolder.filePath(fileInfo.baseName() + "_soma.obj"));
    branchMesh.save(outFolder.filePath(fileInfo.baseName() + "_neurite.obj"));
    auto rootProp = rootMesh.properties();
    auto branchProp = branchMesh.properties();
    LOG(INFO) << fileInfo.baseName() << ", "
              << rootProp.surfaceArea * (1.0 / 9.66 / 9.66) << ", "
              << rootProp.volume * (1.0 / 9.66 / 9.66 / 9.66) << ", "
              << branchProp.surfaceArea * (1.0 / 9.66 / 9.66) << ", "
              << branchProp.volume * (1.0 / 9.66 / 9.66 / 9.66);
  }
}

void changeImgCompressionType()
{
  //QDir dir("/Users/feng/Documents/PY/py_axonregion");
  //QString outFolder("/Users/feng/Documents/PY/py_ar/");
  QDir dir("/Users/feng/Documents/PV/pv_axonregion");
  QString outFolder("/Users/feng/Documents/PV/pv_ar/");
  QStringList filters;
  filters << "*.tif";
  QFileInfoList list = dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks);
  for (int i = 0; i < list.size(); ++i) {
    QFileInfo fileInfo = list.at(i);
    LOG(INFO) << i << " " << list.size() << " " << fileInfo.absoluteFilePath();
    ZImg img(fileInfo.absoluteFilePath());
    QString outname = outFolder + fileInfo.baseName() + ".tif";
    img.save(outname, FileFormat::Tiff, Compression::LZW);
  }
}

void makeSWCPyramidal()
{
  QDir dir("/Users/feng/Documents/PY/PySWC");
  QString outFolder("/Users/feng/Documents/PY/PySWC/");
  QStringList filters;
  filters << "*c.swc";
  QFileInfoList list = dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks);
  for (int i = 0; i < list.size(); ++i) {
    QFileInfo fileInfo = list.at(i);
    LOG(INFO) << i << " " << list.size() << " " << fileInfo.absoluteFilePath();
    ZSwc tree(fileInfo.absoluteFilePath());
    tree.labelSomaAndOthers(3.0 / 0.104);  // soma radius at least 3um
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
  for (int i = 0; i < list.size(); ++i) {
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
    img.normalize(0, 50);
    img.save(outname, FileFormat::Tiff, Compression::LZW);
    img1.normalize(11, 50);
    img1.save(outname1, FileFormat::Tiff, Compression::LZW);
  }

  dir = QDir("/Volumes/PVPY/ipsi");
  outFolder = QDir("/Volumes/PVPY/ipsi/axon_channel");
  axonChannel = 0;

  list = dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks);
  for (int i = 0; i < list.size(); ++i) {
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
    img.normalize(0, 50);
    img.save(outname, FileFormat::Tiff, Compression::LZW);
    img1.normalize(11, 50);
    img1.save(outname1, FileFormat::Tiff, Compression::LZW);
  }

  dir = QDir("/Volumes/PVPY/Py");
  outFolder = QDir("/Volumes/PVPY/Py/axon_channel");
  axonChannel = 2;

  list = dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks);
  for (int i = 0; i < list.size(); ++i) {
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
    img.normalize(0, 50);
    img.save(outname, FileFormat::Tiff, Compression::LZW);
    img1.normalize(11, 50);
    img1.save(outname1, FileFormat::Tiff, Compression::LZW);
  }
}

void moveObjectToCorrectLocation(const QString& fn, const QString& resfn,
                                 const QStringList& metaFiles,
                                 const QStringList& swcFolders,
                                 int mode)
{
  QFile file(fn);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return;
  }

  QByteArray saveData = file.readAll();

  QJsonParseError jsonError;
  QJsonDocument loadDoc(QJsonDocument::fromJson(saveData, &jsonError));
  if (loadDoc.isNull() || loadDoc.isEmpty() || !loadDoc.isObject()) {
    return;
  }

  QJsonObject loadObj = loadDoc.object();
  if (!loadObj.contains("Scene") || !loadObj["Scene"].isObject()) {
    return;
  }

  QJsonObject sceneObj = loadObj["Scene"].toObject();

  QDir::setCurrent(QFileInfo(fn).absolutePath());

  QList<QStringList> metaData = QtCSV::Reader::readToList(metaFiles[0]);
  metaData.removeFirst();
  for (int i = 1; i < metaFiles.size(); ++i) {
    QList<QStringList> tmp = QtCSV::Reader::readToList(metaFiles[i]);
    tmp.removeFirst();
    metaData += tmp;
  }

  std::map<QString, glm::dvec3> cellNameToLocations;
  glm::dvec3 imagescale = glm::dvec3(71, 71, 71);
  double imagePixelPerUm = 0.136;
  double swcPixelPerUmxy = 9.66;
  double swcPixelPerUmz = 2.0;
  glm::dvec3 refSwcLocInBrain = glm::dvec3(-2.25, 1.27, 2.5);   //in mm
  glm::dvec3 refSwcRootImageLoc = glm::dvec3(373, 291, 202);   //roughly get from image
  for (int metaIdx = 0; metaIdx < metaData.size(); ++metaIdx) {
    QString cellName = metaData[metaIdx][0];
    double Anterior_Posterior = metaData[metaIdx][1].toDouble();
    double Medial_Lateral = metaData[metaIdx][2].toDouble();
    double Deep_Superficial = metaData[metaIdx][3].toDouble();

    glm::dvec3 swcLoc(-Medial_Lateral, Deep_Superficial, -Anterior_Posterior);
    glm::dvec3 swcRootImageLoc = (swcLoc - refSwcLocInBrain) * imagePixelPerUm * 1000. + refSwcRootImageLoc;
    QString swcFolder;
    for (int i = 0; i < swcFolders.size(); ++i) {
      if (QFile::exists(QString("%1/%2.swc").arg(swcFolders[i]).arg(cellName))) {
        swcFolder = swcFolders[i];
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

  QJsonObject docObject = sceneObj["Doc"].toObject();
  for (QJsonObject::iterator it = docObject.begin(); it != docObject.end(); ++it) {
    QStringList typeAndID = it.key().split(" ");
    QString IDString = typeAndID[1].trimmed();
    QFileInfo docPath(it.value().toString());
    QString filename = docPath.completeBaseName();
    if (typeAndID[0] == "Swc") {
      if (filename.endsWith("layer"))
        filename.chop(6);
      else
        LOG(FATAL) << "..";
      modifyJsonValue(sceneObj, IDString + ".View3D.Color Mode StringIntOption", QJsonValue("Single Color"));
      if (filename.startsWith("Py"))
        modifyJsonValue(sceneObj, IDString + ".View3D.Color Vec4", QJsonValue(toQString(glm::vec4(1, 0, 0, 1))));
      else if (filename.startsWith("PV169") || filename.startsWith("PV40") || filename.startsWith("PV41") ||
               filename.startsWith("PV42")) {
        modifyJsonValue(sceneObj, IDString + ".View3D.Color Vec4", QJsonValue(toQString(glm::vec4(0, 1, 1, 1))));
      } else {
        modifyJsonValue(sceneObj, IDString + ".View3D.Color Vec4", QJsonValue(toQString(glm::vec4(0, 0, 1, 1))));
      }
    } else if (typeAndID[0] == "Puncta") {
      if (filename.endsWith("puncta"))
        filename.chop(7);
      else if (filename.endsWith("neurite"))
        filename.chop(8);
      else
        LOG(FATAL) << "..";
      modifyJsonValue(sceneObj, IDString + ".View3D.Use Same Size Bool", QJsonValue(true));
      modifyJsonValue(sceneObj, IDString + ".View3D.Size Scale Float", QJsonValue("4"));
      modifyJsonValue(sceneObj, IDString + ".View3D.Color Mode StringIntOption", QJsonValue("Single Color"));
      modifyJsonValue(sceneObj, IDString + ".View3D.Puncta Color Vec4", QJsonValue(toQString(glm::vec4(0, 1, 0, 1))));
    }
    glm::dvec3 loc = cellNameToLocations.at(filename);
    QString locString = toQString(loc);
    QString scaleString = toQString(glm::dvec3(1, 1, 5));

    modifyJsonValue(sceneObj, IDString + ".View3D.Coord Transform 3DTransform.Scale Vec3", scaleString);
    modifyJsonValue(sceneObj, IDString + ".View3D.Coord Transform 3DTransform.Translation Vec3", locString);
    modifyJsonValue(sceneObj, IDString + ".View2D.Offset DVec4", toQString(glm::dvec4(loc, 0)));
  }

  QFile resfile(resfn);
  if (!resfile.open(QIODevice::WriteOnly | QIODevice::Text)) {
    return;
  }

  QJsonObject saveObj;
  saveObj.insert("Scene", sceneObj);

  QJsonDocument saveDoc(saveObj);
  if (resfile.write(saveDoc.toJson()) == -1) {
    return;
  }
}

void createCellTable()
{
  QList<QStringList> metaData = QtCSV::Reader::readToList("/Users/feng/code/mgrasp-analysis/pv_figs/orig_cell_props.csv");
  metaData.removeFirst();

  std::map<QString, int> somaLocationMap;
  somaLocationMap["Ori"] = 1;
  somaLocationMap["Deep"] = 2;
  somaLocationMap["Superficial"] = 3;
  somaLocationMap["Rad"] = 4;

  ZMainWindow* mainWin = nullptr;
  for (auto widget : QApplication::topLevelWidgets()) {
    mainWin = qobject_cast<ZMainWindow*>(widget);
    if (mainWin)
      break;
  }


  QFile file("/Users/feng/Downloads/template_cell.scene");
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return;
  }
  QByteArray saveData = file.readAll();
  QJsonParseError jsonError;
  QJsonDocument loadDoc(QJsonDocument::fromJson(saveData, &jsonError));
  if (loadDoc.isNull() || loadDoc.isEmpty() || !loadDoc.isObject()) {
    return;
  }
  QJsonObject loadObj = loadDoc.object();
  if (!loadObj.contains("Scene") || !loadObj["Scene"].isObject()) {
    return;
  }
  QJsonObject sceneObj = loadObj["Scene"].toObject();
  QJsonObject docObject = sceneObj["Doc"].toObject();

  std::map<std::tuple<QString, int, double, double, QString>, std::tuple<QString, double>> cells;

  for (int metaIdx = 0; metaIdx < metaData.size(); ++metaIdx) {
    QString cellType = metaData[metaIdx][1];
    QString cellName = metaData[metaIdx][2];
    QString somaLocation = metaData[metaIdx][3];
    double AP = metaData[metaIdx][4].toDouble();
    double ML = metaData[metaIdx][5].toDouble();
    double r2 = metaData[metaIdx][27].toDouble();
    int somaLocationOrder = somaLocationMap[somaLocation];
    assert(somaLocationOrder > 0 && somaLocationOrder < 5);

    if (cellType == "Pyr") {
      continue;
    }
    cells[std::make_tuple(cellType, somaLocationOrder, AP, ML, cellName)] = std::make_tuple(somaLocation, r2);

    continue;
    QString swcName = QString("/Users/feng/Documents/PV/PVSWC/%1_layer.swc").arg(cellName);
    QString punctaName = QString("/Users/feng/Documents/PV/PVSWC/%1_neurite.nimp").arg(cellName);

    for (QJsonObject::iterator it = docObject.begin(); it != docObject.end(); ++it) {
      QStringList typeAndID = it.key().split(" ");
      QString IDString = typeAndID[1].trimmed();
      QFileInfo docPath(it.value().toString());
      QString filename = docPath.completeBaseName();
      if (typeAndID[0] == "Swc") {
        modifyJsonValue(sceneObj, "Doc." + it.key(), QJsonValue(swcName));
      } else if (typeAndID[0] == "Puncta") {
        modifyJsonValue(sceneObj, "Doc." + it.key(), QJsonValue(punctaName));
      }
      removeJsonValue(sceneObj, IDString + ".View3D.X Cut FloatSpan");
      removeJsonValue(sceneObj, IDString + ".View3D.Y Cut FloatSpan");
      removeJsonValue(sceneObj, IDString + ".View3D.Z Cut FloatSpan");
    }
    QString scnName = QString("/Users/feng/Downloads/cell_table/%1.scene").arg(cellName);
    QFile resfile(scnName);
    if (!resfile.open(QIODevice::WriteOnly | QIODevice::Text)) {
      return;
    }

    QJsonObject saveObj;
    saveObj.insert("Scene", sceneObj);

    QJsonDocument saveDoc(saveObj);
    if (resfile.write(saveDoc.toJson()) == -1) {
      return;
    }
    resfile.flush();

    mainWin->removeAllObjs();
    mainWin->loadJsonScene(scnName);
    QApplication::processEvents();

    Z3DView *view3d = mainWin->get3DWindow()->view();
    view3d->resetCameraAction()->trigger();
    view3d->zoomInAction()->trigger();
    view3d->zoomInAction()->trigger();
    QApplication::processEvents();
    QString imgName = QString("/Users/feng/Downloads/cell_table/%1.tif").arg(cellName);
    view3d->takeFixedSizeScreenShot(imgName, 512, 512, Z3DScreenShotType::MonoView);
    QApplication::processEvents();
  }

  for (auto it : cells) {
    QString cellType = std::get<0>(it.first);
    cellType.chop(2);
    QString cellName = std::get<4>(it.first);
    QString somaLocation = std::get<0>(it.second);
    if (somaLocation == "ori")
      somaLocation = "oriens";
    if (somaLocation == "rad")
      somaLocation = "radiatum";
    double AP = std::get<2>(it.first);
    double ML = std::get<3>(it.first);
    double r2 = std::get<1>(it.second);
    QString row = QString("%1 & (%2, %3)$mm$ & %4 & %5 & \\parbox[c]{0.5in}{\\includegraphics[height=0.5in]{%6}} \\\\").arg(
      cellType).arg(AP).arg(ML, 0, 'g', 3).arg(somaLocation.toLower()).arg(r2, 0, 'g', 3).arg(cellName);
    LOG(INFO) << row;
  }
}

void testLogSpeed()
{
  ZBenchTimer bt;
  QStringList logList;
  for (int i = 0; i < 500000; ++i)
    logList << randomString(10, 100);
  bt.start();
  for (int i = 0; i < logList.size(); ++i)
    LOG(INFO) << logList.at(i);
  STOP_AND_LOG(bt);
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


}  // namespace nim

namespace nim {

void ZCustomCommand::run()
{
  createCellTable();
  LOG(INFO) << "done";
}

} // namespace nim
