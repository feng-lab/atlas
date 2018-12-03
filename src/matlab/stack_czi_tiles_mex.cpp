#include "mex.h"
#include "zimg.h"
#include "zimgzeissczi.h"

#include <utility>

using namespace nim;

void mexFunction(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[])
{
  /* Check for proper number of input and  output arguments */
  if (nrhs != 3) {
    mexErrMsgTxt("Require three input arguments.");
  }
  if (nlhs > 2) {
    mexErrMsgTxt("Too many output arguments.");
  }

  /* input 1 must be a string */
  if (!mxIsChar(prhs[0]) || mxGetM(prhs[0]) != 1)
    mexErrMsgTxt("Input 1 must be a string and row vector.");

  if (!mxIsDouble(prhs[1]) || mxGetM(prhs[1]) != 1 || mxGetN(prhs[1]) != 1)
    mexErrMsgTxt("Input 2 must be a scalar.");

  if (!mxIsDouble(prhs[2]) || mxGetM(prhs[2]) != 1 || mxGetN(prhs[2]) != 1)
    mexErrMsgTxt("Input 3 must be a scalar.");

  char *filename = mxArrayToString(prhs[0]);

  if (!filename)
    mexErrMsgTxt("Could not convert input 1 to string.");

  size_t ch = *mxGetDoubles(prhs[1]);
  size_t scene = *mxGetDoubles(prhs[2]);

  try {
    std::vector<ZVoxelCoordinate> coords;
    ZImg img = ZImgZeissCZI::instance().stackTiles(filename, ch, scene, coords);

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

    plhs[0] = mxCreateNumericArray(5, imgDims, cid, mxREAL);
    auto data = static_cast<uint8_t*>(mxGetData(plhs[0]));
    for (size_t t=0; t<img.numTimes(); ++t) {
      std::memcpy(data+t*img.timeByteNumber(), img.timeData(t), img.timeByteNumber());
    }

    mwSize coordDims[2];
    coordDims[1] = img.depth();
    coordDims[0] = 5;
    plhs[1] = mxCreateNumericArray(2, coordDims, mxINT32_CLASS, mxREAL);
    auto coordData = static_cast<int32_t*>(mxGetData(plhs[1]));
    std::memcpy(coordData, coords.data(), coords.size() * sizeof(ZVoxelCoordinate));

  } catch (const ZException & e) {
    mexErrMsgTxt(qPrintable(e.what()));
  }
}
