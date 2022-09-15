#ifdef _NEUTUBE_

#include <gtest/gtest.h>

#include "zimggraph.h"
#include "zimgautothreshold.h"
#include "zrandom.h"
#include "c_stack.h"
#include "tz_stack_graph.h"
#include "tz_int_histogram.h"
#include "tz_stack_threshold.h"

TEST(ImgGraph, shortestPath)
{
  using namespace nim;
  ZImgInfo info(20, 15, 5);
  ZImg img(info);
  img.fillRandom();
  ZVoxelCoordinate startCoord(4, 10, 1);
  //LOG(INFO) << img.showContentAsQString();
  //LOG(INFO) << "start Loc" << startX << startY << startZ;

  Stack stackShell;
  Stack *stack = &stackShell;
  stack->kind = img.voxelByteNumber();
  stack->width = img.width();
  stack->height = img.height();
  stack->depth = img.depth();
  stack->array = img.data<uint8_t>(0,0,0,0,0,0);
  //create graph
  Stack_Graph_Workspace *sgw = New_Stack_Graph_Workspace();
  sgw->conn = 26;
  sgw->wf = Stack_Voxel_Weight_S;
  int *hist = C_Stack::hist(stack);
  double c1, c2;
  int thre = Hist_Rcthre_R(hist, Int_Histogram_Min(hist),
                           Int_Histogram_Max(hist), &c1, &c2);
  double scale = c2 - c1;
  if (scale < 1.0)
    scale = 1.0;
  scale /= 9.2;
  //LOG(INFO) << thre << scale;
  free(hist);
  sgw->argv[3] = thre;
  sgw->argv[4] = scale;
  sgw->gw = New_Graph_Workspace();

  Stack_Graph_Workspace_Set_Range(sgw, 0, img.width()-1, 0, img.height()-1, 0, img.depth()-1);
  int start = C_Stack::offset(startCoord.x, startCoord.y, startCoord.z, img.width(), img.height(), img.depth());
  Graph *graph = Stack_Graph_W(stack, sgw);
  Graph_Shortest_Path(graph, start, sgw->gw);
  Kill_Graph(graph);

  ZImg tingRes;
  tingRes.wrapData(sgw->gw->dlist, img.width(), img.height(), img.depth());
  //LOG(INFO) << "Ting res" << tingRes.showContentAsQString();

  ZImgGraph imgGraph(img);
  imgGraph.setConnectivity(26);
  imgGraph.setUseVoxelSize(false);
  ZImgAutoThreshold<> imgAutoThre;
  int thre1 = imgAutoThre.centroidThre<int>(c1, c2, img);
  double scale1 = c2 - c1;
  if (scale1 < 1.0)
    scale1 = 1.0;
  scale1 /= 9.2;
  ASSERT_EQ(thre, thre1);
  ASSERT_NEAR(scale, scale1, 1e-13);
  imgGraph.build(ZImgGraph::EdgeWeight2(thre1, scale1));
  std::vector<double> dist = imgGraph.shortestPaths(startCoord);

  ZImg res;
  res.wrapData(dist.data(), img.width(), img.height(), img.depth());
  //LOG(INFO) << "res" << res.showContentAsQString();

  for (size_t i=0; i<dist.size(); ++i) {
    ASSERT_NEAR(dist[i], sgw->gw->dlist[i], 1e-13);
  }

  Kill_Stack_Graph_Workspace(sgw);
}

#endif

