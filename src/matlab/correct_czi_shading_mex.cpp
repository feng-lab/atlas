#include "mex.h"
#include "zimg.h"
#include "zimgzeissczi.h"

#include <utility>

using namespace nim;

void mexFunction(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[])
{
  /* Check for proper number of input and  output arguments */
  if (nrhs != 6) {
    mexErrMsgTxt("Require three input arguments.");
  }
  if (nlhs > 1) {
    mexErrMsgTxt("Too many output arguments.");
  }

  /* input 1 must be a string */
  if (!mxIsChar(prhs[0]) || mxGetM(prhs[0]) != 1)
    mexErrMsgTxt("Input 1 must be a string and row vector.");

  if (!mxIsDouble(prhs[1]) || mxGetM(prhs[1]) != 1 || mxGetN(prhs[1]) != 1)
    mexErrMsgTxt("Input 2 must be a scalar.");

  if (!mxIsDouble(prhs[2]) || mxGetM(prhs[2]) != 1 || mxGetN(prhs[2]) != 1)
    mexErrMsgTxt("Input 3 must be a scalar.");

  if (!mxIsDouble(prhs[3]))
    mexErrMsgTxt("Input 4 must be a double matrix.");

  if (!mxIsDouble(prhs[4]))
    mexErrMsgTxt("Input 5 must be a double matrix.");

  if (!mxIsDouble(prhs[5]) || mxGetM(prhs[5]) != 1 || mxGetN(prhs[5]) != 1)
    mexErrMsgTxt("Input 6 must be a scalar.");

  char *filename = mxArrayToString(prhs[0]);

  if (!filename)
    mexErrMsgTxt("Could not convert input 1 to string.");

  size_t ch = *mxGetDoubles(prhs[1]);
  size_t scene = *mxGetDoubles(prhs[2]);
  ZImg modelZ;
  modelZ.wrapData(mxGetDoubles(prhs[3]), ZImgInfo(mxGetM(prhs[3]), mxGetN(prhs[3]), 1, 1, 1, 8, VoxelFormat::Float));  // input is transposed matrix
  ZImg modelV;
  modelV.wrapData(mxGetDoubles(prhs[4]), ZImgInfo(mxGetM(prhs[4]), mxGetN(prhs[4]), 1, 1, 1, 8, VoxelFormat::Float));  // input is transposed matrix
  size_t cmn = *mxGetDoubles(prhs[5]);
  ZImgZeissCZI::CorrectionMode cm = ZImgZeissCZI::CorrectionMode::ZeroLightPreserved;
  switch (cmn) {
  case 0:
    cm = ZImgZeissCZI::CorrectionMode::ZeroLightPreserved;
    break;
  case 1:
    cm = ZImgZeissCZI::CorrectionMode::IntensityRangeCorrected;
    break;
  case 2:
    cm = ZImgZeissCZI::CorrectionMode::Direct;
    break;
  default:
    cm = ZImgZeissCZI::CorrectionMode::ZeroLightPreserved;
    break;
  }

  try {
    ZImg img = ZImgZeissCZI::instance().correctShading(filename, ch, scene, modelZ, modelV, cm);

    mwSize imgDims[5];
    imgDims[0] = img.width();
    imgDims[1] = img.height();
    imgDims[2] = img.depth();
    imgDims[3] = img.numChannels();
    imgDims[4] = img.numTimes();

    uint8_t* data = nullptr;

    if (img.voxelFormat() == VoxelFormat::Signed) {
      switch (img.bytesPerVoxel()) {
      case 1:
        plhs[0] = mxCreateNumericArray(5, imgDims, mxINT8_CLASS, mxREAL);
        data = reinterpret_cast<uint8_t*>(mxGetInt8s(plhs[0]));
        break;
      case 2:
        plhs[0] = mxCreateNumericArray(5, imgDims, mxINT16_CLASS, mxREAL);
        data = reinterpret_cast<uint8_t*>(mxGetInt16s(plhs[0]));
        break;
      case 4:
        plhs[0] = mxCreateNumericArray(5, imgDims, mxINT32_CLASS, mxREAL);
        data = reinterpret_cast<uint8_t*>(mxGetInt32s(plhs[0]));
        break;
      case 8:
        plhs[0] = mxCreateNumericArray(5, imgDims, mxINT64_CLASS, mxREAL);
        data = reinterpret_cast<uint8_t*>(mxGetInt64s(plhs[0]));
        break;
      default:
        break;
      }
    } else if (img.voxelFormat() == VoxelFormat::Float) {
      switch (img.bytesPerVoxel()) {
      case 4:
        plhs[0] = mxCreateNumericArray(5, imgDims, mxSINGLE_CLASS, mxREAL);
        data = reinterpret_cast<uint8_t*>(mxGetSingles(plhs[0]));
        break;
      case 8:
        plhs[0] = mxCreateNumericArray(5, imgDims, mxDOUBLE_CLASS, mxREAL);
        data = reinterpret_cast<uint8_t*>(mxGetDoubles(plhs[0]));
        break;
      default:
        break;
      }
    } else {
      switch (img.bytesPerVoxel()) {
      case 1:
        plhs[0] = mxCreateNumericArray(5, imgDims, mxUINT8_CLASS, mxREAL);
        data = reinterpret_cast<uint8_t*>(mxGetUint8s(plhs[0]));
        break;
      case 2:
        plhs[0] = mxCreateNumericArray(5, imgDims, mxUINT16_CLASS, mxREAL);
        data = reinterpret_cast<uint8_t*>(mxGetUint16s(plhs[0]));
        break;
      case 4:
        plhs[0] = mxCreateNumericArray(5, imgDims, mxUINT32_CLASS, mxREAL);
        data = reinterpret_cast<uint8_t*>(mxGetUint32s(plhs[0]));
        break;
      case 8:
        plhs[0] = mxCreateNumericArray(5, imgDims, mxUINT64_CLASS, mxREAL);
        data = reinterpret_cast<uint8_t*>(mxGetUint64s(plhs[0]));
        break;
      default:
        break;
      }
    }

    for (size_t t=0; t<img.numTimes(); ++t) {
      std::memcpy(data+t*img.timeByteNumber(), img.timeData(t), img.timeByteNumber());
    }
  } catch (const ZException & e) {
    mexErrMsgTxt(qPrintable(e.what()));
  }
}
