#include "mex.h"
#include "zimgio.h"

#include <utility>

using namespace nim;

void mexFunction(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[])
{
  /* Check for proper number of input and  output arguments */
  if (nrhs != 4) {
    mexErrMsgTxt("Require four input arguments.");
  }
  if (nlhs > 1) {
    mexErrMsgTxt("Too many output arguments.");
  }

  /* input 1 must be a string */
  if (!mxIsChar(prhs[0]) || mxGetM(prhs[0]) != 1)
    mexErrMsgTxt("Input 1 must be a string and row vector.");

  if (!mxIsDouble(prhs[1]) || mxGetM(prhs[1]) != 1 || mxGetN(prhs[1]) != 10)
    mexErrMsgTxt("Input 2 must be a double 1x10 row vector.");

  if (!mxIsDouble(prhs[2]) || mxGetM(prhs[2]) != 1 || mxGetN(prhs[2]) != 1)
    mexErrMsgTxt("Input 3 must be a scalar.");

  if (!mxIsDouble(prhs[3]) || mxGetM(prhs[3]) != 1 || mxGetN(prhs[3]) != 1)
    mexErrMsgTxt("Input 4 must be a scalar.");

  char *filename = mxArrayToString(prhs[0]);

  if (!filename)
    mexErrMsgTxt("Could not convert input 1 to string.");

  double *pt = mxGetDoubles(prhs[1]);
  ZImgRegion region(pt[0], pt[1], pt[2], pt[3], pt[4], pt[5], pt[6], pt[7], pt[8], pt[9]);
  size_t scene = *mxGetDoubles(prhs[2]);
  size_t pyramidalLevel = *mxGetDoubles(prhs[3]);

  ZImg img;

  try {
    ZImgIO::instance().readImg(QString(filename), img, region, scene, std::pow(2,pyramidalLevel));
  } catch (const ZException & e) {
    mexErrMsgTxt(qPrintable(e.what()));
  }

  /* Create a 1-by-1 array of structs. */
  mwSize dims[1] = {1};
  const char *field_names[] = {"img", "voxel_size_unit", "voxel_size_X", "voxel_size_Y", "voxel_size_Z"};
  plhs[0] = mxCreateStructArray(1, dims, 5, field_names);

  int img_field = mxGetFieldNumber(plhs[0], "img");
  int voxelSizeUnit_field = mxGetFieldNumber(plhs[0], "voxel_size_unit");
  int voxelSizeX_field = mxGetFieldNumber(plhs[0], "voxel_size_X");
  int voxelSizeY_field = mxGetFieldNumber(plhs[0], "voxel_size_Y");
  int voxelSizeZ_field = mxGetFieldNumber(plhs[0], "voxel_size_Z");

  mwSize imgDims[5];
  imgDims[0] = img.width();
  imgDims[1] = img.height();
  imgDims[2] = img.depth();
  imgDims[3] = img.numChannels();
  imgDims[4] = img.numTimes();

  mxClassID cid = mxINT8_CLASS;

  if (img.voxelFormat() == VoxelFormat::Signed) {
    switch (img.bytesPerVoxel()) {
    case 1:
      cid = mxINT8_CLASS;
      break;
    case 2:
      cid = mxINT16_CLASS;
      break;
    case 4:
      cid = mxINT32_CLASS;
      break;
    case 8:
      cid = mxINT64_CLASS;
      break;
    default:
      break;
    }
  } else if (img.voxelFormat() == VoxelFormat::Float) {
    switch (img.bytesPerVoxel()) {
    case 4:
      cid = mxSINGLE_CLASS;
      break;
    case 8:
      cid = mxDOUBLE_CLASS;
      break;
    default:
      break;
    }
  } else {
    switch (img.bytesPerVoxel()) {
    case 1:
      cid = mxUINT8_CLASS;
      break;
    case 2:
      cid = mxUINT16_CLASS;
      break;
    case 4:
      cid = mxUINT32_CLASS;
      break;
    case 8:
      cid = mxUINT64_CLASS;
      break;
    default:
      break;
    }
  }

  mxArray *imgArray = mxCreateNumericArray(5, imgDims, cid, mxREAL);
  uint8_t *data = static_cast<uint8_t*>(mxGetData(imgArray));
  for (size_t t=0; t<img.numTimes(); ++t) {
    std::memcpy(data+t*img.timeByteNumber(), img.timeData(t), img.timeByteNumber());
  }
  mxSetFieldByNumber(plhs[0], 0, img_field, imgArray);

  mxSetFieldByNumber(plhs[0], 0, voxelSizeUnit_field, mxCreateString(img.voxelSizeUnit() == VoxelSizeUnit::none ? "none" : "um"));
  mxArray *vsx = mxCreateDoubleMatrix(1, 1, mxREAL);
  *mxGetDoubles(vsx) = img.voxelSizeUnit() == VoxelSizeUnit::none ? 1.0 : img.info().voxelSizeXInUm();
  mxSetFieldByNumber(plhs[0], 0, voxelSizeX_field, vsx);
  mxArray *vsy = mxCreateDoubleMatrix(1, 1, mxREAL);
  *mxGetDoubles(vsy) = img.voxelSizeUnit() == VoxelSizeUnit::none ? 1.0 : img.info().voxelSizeYInUm();
  mxSetFieldByNumber(plhs[0], 0, voxelSizeY_field, vsy);
  mxArray *vsz = mxCreateDoubleMatrix(1, 1, mxREAL);
  *mxGetDoubles(vsz) = img.voxelSizeUnit() == VoxelSizeUnit::none ? 1.0 : img.info().voxelSizeZInUm();
  mxSetFieldByNumber(plhs[0], 0, voxelSizeZ_field, vsz);
}
