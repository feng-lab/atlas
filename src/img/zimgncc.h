#pragma once

#include "zimg.h"

// all functions only accept single channel 3D or 2D img as input, throw
// exception if input is not

namespace nim {

double getNCCOfOffset(const ZImg& fixedImgIn, const ZImg& movingImgIn, const ZVoxelCoordinate& offset);

// first two paras are input, last two are output
// output are double img
// will clear input imgs, if you don't want input to be cleared, create a virtual view of
// input and clear virtual img will not release img memory
// need 4 double padded extra space
// for two 200M 8bit imgs, this will need 4*200*8*8M = 51200M = 51.2G memory
// reference: matlab code normxcorr2_general.m
void normXCorr(ZImg& fixedImg, ZImg& movingImg, ZImg& nccImg, ZImg& numberOfOverlapVoxelsImg);

// slower but use less memory, need 3 double padded extra space
// for two 200M 8bit imgs, this will need 3*200*8*8M = 38400M = 38.4G memory
void normXCorr_S(ZImg& fixedImg, ZImg& movingImg, ZImg& nccImg, ZImg& numberOfOverlapVoxelsImg);

// only compute ncc of part region, same as crop a region from whole ncc image
// throw exception if region is not valid
void normXCorrPart(ZImg& fixedImg,
                   ZImg& movingImg,
                   size_t xStart,
                   size_t xEnd,
                   size_t yStart,
                   size_t yEnd,
                   size_t zStart,
                   size_t zEnd,
                   ZImg& nccImg,
                   ZImg& numberOfOverlapVoxelsImg);

ZImg xCorrFFT(const ZImg& fixedImg, ZImg& movingImg, bool reflectMovingImg);

ZImg xCorrPart(const ZImg& fixedImg,
               const ZImg& movingImg,
               size_t xStart,
               size_t xEnd,
               size_t yStart,
               size_t yEnd,
               size_t zStart,
               size_t zEnd);

// throw exception if not overlap
void cropOverlapSubImg(const ZImg& fixedImgIn,
                       const ZImg& movingImgIn,
                       const ZVoxelCoordinate& offset,
                       ZImg& subFixedImg,
                       ZImg& subMovingImg);

} // namespace nim
