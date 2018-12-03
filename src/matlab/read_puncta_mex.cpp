#include "mex.h"
#include "zpunctaio.h"
#include "zpuncta.h"

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

  ZPuncta puncta;

  try {
    ZPunctaIO::instance().load(QString(filename), puncta);
  } catch (const ZException & e) {
    mexErrMsgTxt(qPrintable(e.what()));
  }

  /* Create a n-by-a cell array. each cell contains a struct: */
  const char *field_names[] = {"centroid", "maxIntensity", "meanIntensity", "sDevOfIntensity", "volSize", "mass", "radius",
                              "score", "data"};

  plhs[0] = mxCreateCellMatrix(puncta.size(), 1);

  size_t i = 0;
  for (const auto& pun : puncta) {
    mxArray* ps = mxCreateStructMatrix(1, 1, 9, field_names);

    mxArray* centroid = mxCreateDoubleMatrix(1, 3, mxREAL);
    mxGetDoubles(centroid)[0] = pun.x();
    mxGetDoubles(centroid)[1] = pun.y();
    mxGetDoubles(centroid)[2] = pun.z();
    mxSetField(ps, 0, "centroid", centroid);

    mxArray* maxIntensity = mxCreateDoubleMatrix(1, 1, mxREAL);
    mxGetDoubles(maxIntensity)[0] = pun.maxIntensity();
    mxSetField(ps, 0, "maxIntensity", maxIntensity);

    mxArray* meanIntensity = mxCreateDoubleMatrix(1, 1, mxREAL);
    mxGetDoubles(meanIntensity)[0] = pun.meanIntensity();
    mxSetField(ps, 0, "meanIntensity", meanIntensity);

    mxArray* sDevOfIntensity = mxCreateDoubleMatrix(1, 1, mxREAL);
    mxGetDoubles(sDevOfIntensity)[0] = pun.sDevOfIntensity();
    mxSetField(ps, 0, "sDevOfIntensity", sDevOfIntensity);

    mxArray* volSize = mxCreateDoubleMatrix(1, 1, mxREAL);
    mxGetDoubles(volSize)[0] = pun.volSize();
    mxSetField(ps, 0, "volSize", volSize);

    mxArray* mass = mxCreateDoubleMatrix(1, 1, mxREAL);
    mxGetDoubles(mass)[0] = pun.mass();
    mxSetField(ps, 0, "mass", mass);

    mxArray* radius = mxCreateDoubleMatrix(1, 1, mxREAL);
    mxGetDoubles(radius)[0] = pun.radius();
    mxSetField(ps, 0, "radius", radius);

    mxArray* score = mxCreateDoubleMatrix(1, 1, mxREAL);
    mxGetDoubles(score)[0] = pun.score();
    mxSetField(ps, 0, "score", score);

    auto vls = pun.voxelLocations();
    auto vis = pun.voxelIntensities();

    if (vls.rows() > 0) {
      mxArray* data = mxCreateDoubleMatrix(pun.volSize(), 4, mxREAL);
      for (size_t j = 0; j < pun.volSize(); ++j) {
        mxDouble* dataptr = mxGetDoubles(data);
        dataptr[j] = vls(j, 0);
        dataptr[j + pun.volSize()] = vls(j, 1);
        dataptr[j + 2 * pun.volSize()] = vls(j, 2);
        dataptr[j + 3 * pun.volSize()] = vis(j);
      }
      mxSetField(ps, 0, "data", data);
    } else {
      mxArray* data = mxCreateDoubleMatrix(0, 0, mxREAL);
      mxSetField(ps, 0, "data", data);
    }

    mxSetCell(plhs[0], i++, ps);
  }
}
