#include "mex.h"
#include "zimgio.h"

#include <utility>

using namespace nim;

void mexFunction(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[])
{
  /* Check for proper number of input and  output arguments */
  if (nrhs != 1) {
    mexErrMsgTxt("Require one input arguments.");
  }
  if (nlhs > 1) {
    mexErrMsgTxt("Too many output arguments.");
  }

  /* input 1 must be a string */
  if (!mxIsChar(prhs[0]) || mxGetM(prhs[0]) != 1)
    mexErrMsgTxt("Input 1 must be a string and row vector.");

  char *filename = mxArrayToString(prhs[0]);

  if (!filename)
    mexErrMsgTxt("Could not convert input 1 to string.");

  std::vector<ZImgInfo> infos;

  try {
    ZImgIO::instance().readInfos(QString(filename), infos, nullptr, nullptr);
  } catch (const ZException & e) {
    mexErrMsgTxt(qPrintable(e.what()));
  }

  /* Create a 1-by-n array of structs. */
  const char *field_names[] = {"width", "height", "depth", "num_channels", "num_times", "classname", "valid_bit_count",
                              "voxel_size_unit", "voxel_size_X", "voxel_size_Y", "voxel_size_Z", "alpha_channel_index",
                              "channel_names", "position"};
  mwSize dims[2];
  dims[0] = 1;
  dims[1] = infos.size();
  plhs[0] = mxCreateStructArray(2, dims, 14, field_names);

  int width_field = mxGetFieldNumber(plhs[0], "width");
  int height_field = mxGetFieldNumber(plhs[0], "height");
  int depth_field = mxGetFieldNumber(plhs[0], "depth");
  int num_channels_field = mxGetFieldNumber(plhs[0], "num_channels");
  int num_times_field = mxGetFieldNumber(plhs[0], "num_times");
  int classname_field = mxGetFieldNumber(plhs[0], "classname");
  int valid_bit_count_field = mxGetFieldNumber(plhs[0], "valid_bit_count");
  int alpha_channel_index_field = mxGetFieldNumber(plhs[0], "alpha_channel_index");
  int voxelSizeUnit_field = mxGetFieldNumber(plhs[0], "voxel_size_unit");
  int voxelSizeX_field = mxGetFieldNumber(plhs[0], "voxel_size_X");
  int voxelSizeY_field = mxGetFieldNumber(plhs[0], "voxel_size_Y");
  int voxelSizeZ_field = mxGetFieldNumber(plhs[0], "voxel_size_Z");
  int channel_names_field = mxGetFieldNumber(plhs[0], "channel_names");
  int position_field = mxGetFieldNumber(plhs[0], "position");

  for (size_t i=0; i<infos.size(); ++i) {
    mxArray *width = mxCreateDoubleMatrix(1, 1, mxREAL);
    *mxGetDoubles(width) = infos[i].width;
    mxSetFieldByNumber(plhs[0], i, width_field, width);

    mxArray *height = mxCreateDoubleMatrix(1, 1, mxREAL);
    *mxGetDoubles(height) = infos[i].height;
    mxSetFieldByNumber(plhs[0], i, height_field, height);

    mxArray *depth = mxCreateDoubleMatrix(1, 1, mxREAL);
    *mxGetDoubles(depth) = infos[i].depth;
    mxSetFieldByNumber(plhs[0], i, depth_field, depth);

    mxArray *ch = mxCreateDoubleMatrix(1, 1, mxREAL);
    *mxGetDoubles(ch) = infos[i].numChannels;
    mxSetFieldByNumber(plhs[0], i, num_channels_field, ch);

    mxArray *time = mxCreateDoubleMatrix(1, 1, mxREAL);
    *mxGetDoubles(time) = infos[i].numTimes;
    mxSetFieldByNumber(plhs[0], i, num_times_field, time);

    if (infos[i].voxelFormat == VoxelFormat::Signed) {
      switch (infos[i].bytesPerVoxel) {
      case 1:
        mxSetFieldByNumber(plhs[0], i, classname_field, mxCreateString("int8"));
        break;
      case 2:
        mxSetFieldByNumber(plhs[0], i, classname_field, mxCreateString("int16"));
        break;
      case 4:
        mxSetFieldByNumber(plhs[0], i, classname_field, mxCreateString("int32"));
        break;
      case 8:
        mxSetFieldByNumber(plhs[0], i, classname_field, mxCreateString("int64"));
        break;
      default:
        break;
      }
    } else if (infos[i].voxelFormat == VoxelFormat::Float) {
      switch (infos[i].bytesPerVoxel) {
      case 4:
        mxSetFieldByNumber(plhs[0], i, classname_field, mxCreateString("single"));
        break;
      case 8:
        mxSetFieldByNumber(plhs[0], i, classname_field, mxCreateString("double"));
        break;
      default:
        break;
      }
    } else {
      switch (infos[i].bytesPerVoxel) {
      case 1:
        mxSetFieldByNumber(plhs[0], i, classname_field, mxCreateString("uint8"));
        break;
      case 2:
        mxSetFieldByNumber(plhs[0], i, classname_field, mxCreateString("uint16"));
        break;
      case 4:
        mxSetFieldByNumber(plhs[0], i, classname_field, mxCreateString("uint32"));
        break;
      case 8:
        mxSetFieldByNumber(plhs[0], i, classname_field, mxCreateString("uint64"));
        break;
      default:
        break;
      }
    }

    mxArray *vbc = mxCreateDoubleMatrix(1, 1, mxREAL);
    *mxGetDoubles(vbc) = infos[i].validBitCount;
    mxSetFieldByNumber(plhs[0], i, valid_bit_count_field, vbc);

    mxArray *aci = mxCreateDoubleMatrix(1, 1, mxREAL);
    *mxGetDoubles(aci) = infos[i].lastChannelIsAlphaChannel ? infos[i].numChannels : -1;
    mxSetFieldByNumber(plhs[0], i, alpha_channel_index_field, aci);

    mxSetFieldByNumber(plhs[0], i, voxelSizeUnit_field, mxCreateString(infos[i].voxelSizeUnit == VoxelSizeUnit::none ? "none" : "um"));

    mxArray *vsx = mxCreateDoubleMatrix(1, 1, mxREAL);
    *mxGetDoubles(vsx) = infos[i].voxelSizeXInUm();
    mxSetFieldByNumber(plhs[0], i, voxelSizeX_field, vsx);

    mxArray *vsy = mxCreateDoubleMatrix(1, 1, mxREAL);
    *mxGetDoubles(vsy) = infos[i].voxelSizeYInUm();
    mxSetFieldByNumber(plhs[0], i, voxelSizeY_field, vsy);

    mxArray *vsz = mxCreateDoubleMatrix(1, 1, mxREAL);
    *mxGetDoubles(vsz) = infos[i].voxelSizeZInUm();
    mxSetFieldByNumber(plhs[0], i, voxelSizeZ_field, vsz);

    mxArray *cns = mxCreateCellMatrix(infos[i].channelNames.size(), 1);
    for(mwIndex j=0; j<mwIndex(infos[i].channelNames.size()); ++j){
      mxSetCell(cns, j, mxCreateString(qPrintable(infos[i].channelNames[j])));
    }
    mxSetFieldByNumber(plhs[0], i, channel_names_field, cns);

    if (!infos[i].position.empty()) {
      mxArray *pos = mxCreateDoubleMatrix(1, infos[i].position.size(), mxREAL);
      std::memcpy(mxGetDoubles(pos), infos[i].position.data(), infos[i].position.size() * sizeof(double));
      mxSetFieldByNumber(plhs[0], i, position_field, pos);
    }
  }
}
