#include "ztiff.h"
#include <sstream>
#include <tiffio.h>
#include <tiffio.hxx>
//#include "zxtiffio.h"
#include "zimgformat.h"
#include "zlog.h"
#include <cmath>
#include <boost/endian/conversion.hpp>
#include <boost/align/aligned_allocator.hpp>
#include "zimage2dutils.h"
#include "zioutils.h"
#include <set>
#include <QFile>

namespace {

// Carl Zeiss LSM
#define TIFFTAG_CZ_LSMINFO           34412


/* tags 33550 is a private tag registered to SoftDesk, Inc */
#define TIFFTAG_GEOPIXELSCALE        33550
/* tags 33920-33921 are private tags registered to Intergraph, Inc */
#define TIFFTAG_INTERGRAPH_MATRIX    33920   /* $use TIFFTAG_GEOTRANSMATRIX ! */
#define TIFFTAG_GEOTIEPOINTS         33922
/* tags 34263-34264 are private tags registered to NASA-JPL Carto Group */
#ifdef JPL_TAG_SUPPORT
#define TIFFTAG_JPL_CARTO_IFD        34263    /* $use GeoProjectionInfo ! */
#endif
#define TIFFTAG_GEOTRANSMATRIX       34264    /* New Matrix Tag replaces 33920 */
/* tags 34735-3438 are private tags registered to SPOT Image, Inc */
#define TIFFTAG_GEOKEYDIRECTORY      34735
#define TIFFTAG_GEODOUBLEPARAMS      34736
#define TIFFTAG_GEOASCIIPARAMS       34737

/*
 *  Define Printing method flags. These
 *  flags may be passed in to TIFFPrintDirectory() to
 *  indicate that those particular field values should
 *  be printed out in full, rather than just an indicator
 *  of whether they are present or not.
 */
#define	TIFFPRINT_GEOKEYDIRECTORY	0x80000000
#define	TIFFPRINT_GEOKEYPARAMS		0x40000000

union
{
  TIFFHeaderClassic classic;
  TIFFHeaderBig big;
  TIFFHeaderCommon common;
} hdr;

bool hostIsLittleEndian()
{
  int num = 1;
  return *reinterpret_cast<char*>(&num) == 1;
}

static const struct tiftagname
{
  uint32 tag;
  const char* name;
} tiftagnames[] = {
  {TIFFTAG_SUBFILETYPE,                "SubfileType"},
  {TIFFTAG_OSUBFILETYPE,               "OldSubfileType"},
  {TIFFTAG_IMAGEWIDTH,                 "ImageWidth"},
  {TIFFTAG_IMAGELENGTH,                "ImageLength"},
  {TIFFTAG_BITSPERSAMPLE,              "BitsPerSample"},
  {TIFFTAG_COMPRESSION,                "Compression"},
  {TIFFTAG_PHOTOMETRIC,                "PhotometricInterpretation"},
  {TIFFTAG_THRESHHOLDING,              "Threshholding"},
  {TIFFTAG_CELLWIDTH,                  "CellWidth"},
  {TIFFTAG_CELLLENGTH,                 "CellLength"},
  {TIFFTAG_FILLORDER,                  "FillOrder"},
  {TIFFTAG_DOCUMENTNAME,               "DocumentName"},
  {TIFFTAG_IMAGEDESCRIPTION,           "ImageDescription"},
  {TIFFTAG_MAKE,                       "Make"},
  {TIFFTAG_MODEL,                      "Model"},
  {TIFFTAG_STRIPOFFSETS,               "StripOffsets"},
  {TIFFTAG_ORIENTATION,                "Orientation"},
  {TIFFTAG_SAMPLESPERPIXEL,            "SamplesPerPixel"},
  {TIFFTAG_ROWSPERSTRIP,               "RowsPerStrip"},
  {TIFFTAG_STRIPBYTECOUNTS,            "StripByteCounts"},
  {TIFFTAG_MINSAMPLEVALUE,             "MinSampleValue"},
  {TIFFTAG_MAXSAMPLEVALUE,             "MaxSampleValue"},
  {TIFFTAG_XRESOLUTION,                "XResolution"},
  {TIFFTAG_YRESOLUTION,                "YResolution"},
  {TIFFTAG_PLANARCONFIG,               "PlanarConfiguration"},
  {TIFFTAG_PAGENAME,                   "PageName"},
  {TIFFTAG_XPOSITION,                  "XPosition"},
  {TIFFTAG_YPOSITION,                  "YPosition"},
  {TIFFTAG_FREEOFFSETS,                "FreeOffsets"},
  {TIFFTAG_FREEBYTECOUNTS,             "FreeByteCounts"},
  {TIFFTAG_GRAYRESPONSEUNIT,           "GrayResponseUnit"},
  {TIFFTAG_GRAYRESPONSECURVE,          "GrayResponseCurve"},
  {TIFFTAG_RESOLUTIONUNIT,             "ResolutionUnit"},
  {TIFFTAG_PAGENUMBER,                 "PageNumber"},
  {TIFFTAG_COLORRESPONSEUNIT,          "ColorResponseUnit"},
  {TIFFTAG_TRANSFERFUNCTION,           "TransferFunction"},
  {TIFFTAG_SOFTWARE,                   "Software"},
  {TIFFTAG_DATETIME,                   "DateTime"},
  {TIFFTAG_ARTIST,                     "Artist"},
  {TIFFTAG_HOSTCOMPUTER,               "HostComputer"},
  {TIFFTAG_WHITEPOINT,                 "WhitePoint"},
  {TIFFTAG_PRIMARYCHROMATICITIES,      "PrimaryChromaticities"},
  {TIFFTAG_COLORMAP,                   "ColorMap"},
  {TIFFTAG_HALFTONEHINTS,              "HalftoneHints"},
  {TIFFTAG_TILEWIDTH,                  "TileWidth"},
  {TIFFTAG_TILELENGTH,                 "TileLength"},
  {TIFFTAG_TILEOFFSETS,                "TileOffsets"},
  {TIFFTAG_TILEBYTECOUNTS,             "TileByteCounts"},
  {TIFFTAG_SUBIFD,                     "SubIFD"},
  {TIFFTAG_INKSET,                     "InkSet"},
  {TIFFTAG_INKNAMES,                   "InkNames"},
  {TIFFTAG_NUMBEROFINKS,               "NumberOfInks"},
  {TIFFTAG_DOTRANGE,                   "DotRange"},
  {TIFFTAG_TARGETPRINTER,              "TargetPrinter"},
  {TIFFTAG_EXTRASAMPLES,               "ExtraSamples"},
  {TIFFTAG_SAMPLEFORMAT,               "SampleFormat"},
  {TIFFTAG_SMINSAMPLEVALUE,            "SMinSampleValue"},
  {TIFFTAG_SMAXSAMPLEVALUE,            "SMaxSampleValue"},
  {TIFFTAG_CLIPPATH,                   "ClipPath"},
  {TIFFTAG_XCLIPPATHUNITS,             "XClipPathUnits"},
  {TIFFTAG_XCLIPPATHUNITS,             "XClipPathUnits"},
  {TIFFTAG_YCLIPPATHUNITS,             "YClipPathUnits"},
  {TIFFTAG_YCBCRCOEFFICIENTS,          "YCbCrCoefficients"},
  {TIFFTAG_YCBCRSUBSAMPLING,           "YCbCrSubsampling"},
  {TIFFTAG_YCBCRPOSITIONING,           "YCbCrPositioning"},
  {TIFFTAG_REFERENCEBLACKWHITE,        "ReferenceBlackWhite"},
  {TIFFTAG_XMLPACKET,                  "XMLPacket"},
/* begin SGI tags */
  {TIFFTAG_MATTEING,                   "Matteing"},
  {TIFFTAG_DATATYPE,                   "DataType"},
  {TIFFTAG_IMAGEDEPTH,                 "ImageDepth"},
  {TIFFTAG_TILEDEPTH,                  "TileDepth"},
/* end SGI tags */
/* begin Pixar tags */
  {TIFFTAG_PIXAR_IMAGEFULLWIDTH,       "ImageFullWidth"},
  {TIFFTAG_PIXAR_IMAGEFULLLENGTH,      "ImageFullLength"},
  {TIFFTAG_PIXAR_TEXTUREFORMAT,        "TextureFormat"},
  {TIFFTAG_PIXAR_WRAPMODES,            "TextureWrapModes"},
  {TIFFTAG_PIXAR_FOVCOT,               "FieldOfViewCotangent"},
  {TIFFTAG_PIXAR_MATRIX_WORLDTOSCREEN, "MatrixWorldToScreen"},
  {TIFFTAG_PIXAR_MATRIX_WORLDTOCAMERA, "MatrixWorldToCamera"},
  {TIFFTAG_COPYRIGHT,                  "Copyright"},
/* end Pixar tags */
  {TIFFTAG_RICHTIFFIPTC,               "RichTIFFIPTC"},
  {TIFFTAG_PHOTOSHOP,                  "Photoshop"},
  {TIFFTAG_EXIFIFD,                    "ExifIFD"},
  {TIFFTAG_ICCPROFILE,                 "ICC Profile"},
  {TIFFTAG_GPSIFD,                     "GPSIFDOffset"},
  {TIFFTAG_FAXRECVPARAMS,              "FaxRecvParams"},
  {TIFFTAG_FAXSUBADDRESS,              "FaxSubAddress"},
  {TIFFTAG_FAXRECVTIME,                "FaxRecvTime"},
  {TIFFTAG_FAXDCS,                     "FaxDcs"},
  {TIFFTAG_STONITS,                    "StoNits"},
  {TIFFTAG_INTEROPERABILITYIFD,        "InteroperabilityIFDOffset"},
/* begin DNG tags */
  {TIFFTAG_DNGVERSION,                 "DNGVersion"},
  {TIFFTAG_DNGBACKWARDVERSION,         "DNGBackwardVersion"},
  {TIFFTAG_UNIQUECAMERAMODEL,          "UniqueCameraModel"},
  {TIFFTAG_LOCALIZEDCAMERAMODEL,       "LocalizedCameraModel"},
  {TIFFTAG_CFAPLANECOLOR,              "CFAPlaneColor"},
  {TIFFTAG_CFALAYOUT,                  "CFALayout"},
  {TIFFTAG_LINEARIZATIONTABLE,         "LinearizationTable"},
  {TIFFTAG_BLACKLEVELREPEATDIM,        "BlackLevelRepeatDim"},
  {TIFFTAG_BLACKLEVEL,                 "BlackLevel"},
  {TIFFTAG_BLACKLEVELDELTAH,           "BlackLevelDeltaH"},
  {TIFFTAG_BLACKLEVELDELTAV,           "BlackLevelDeltaV"},
  {TIFFTAG_WHITELEVEL,                 "WhiteLevel"},
  {TIFFTAG_DEFAULTSCALE,               "DefaultScale"},
  {TIFFTAG_BESTQUALITYSCALE,           "BestQualityScale"},
  {TIFFTAG_DEFAULTCROPORIGIN,          "DefaultCropOrigin"},
  {TIFFTAG_DEFAULTCROPSIZE,            "DefaultCropSize"},
  {TIFFTAG_COLORMATRIX1,               "ColorMatrix1"},
  {TIFFTAG_COLORMATRIX2,               "ColorMatrix2"},
  {TIFFTAG_CAMERACALIBRATION1,         "CameraCalibration1"},
  {TIFFTAG_CAMERACALIBRATION2,         "CameraCalibration2"},
  {TIFFTAG_REDUCTIONMATRIX1,           "ReductionMatrix1"},
  {TIFFTAG_REDUCTIONMATRIX2,           "ReductionMatrix2"},
  {TIFFTAG_ANALOGBALANCE,              "AnalogBalance"},
  {TIFFTAG_ASSHOTNEUTRAL,              "AsShotNeutral"},
  {TIFFTAG_ASSHOTWHITEXY,              "AsShotWhiteXY"},
  {TIFFTAG_BASELINEEXPOSURE,           "BaselineExposure"},
  {TIFFTAG_BASELINENOISE,              "BaselineNoise"},
  {TIFFTAG_BASELINESHARPNESS,          "BaselineSharpness"},
  {TIFFTAG_BAYERGREENSPLIT,            "BayerGreenSplit"},
  {TIFFTAG_LINEARRESPONSELIMIT,        "LinearResponseLimit"},
  {TIFFTAG_CAMERASERIALNUMBER,         "CameraSerialNumber"},
  {TIFFTAG_LENSINFO,                   "LensInfo"},
  {TIFFTAG_CHROMABLURRADIUS,           "ChromaBlurRadius"},
  {TIFFTAG_ANTIALIASSTRENGTH,          "AntiAliasStrength"},
  {TIFFTAG_SHADOWSCALE,                "ShadowScale"},
  {TIFFTAG_DNGPRIVATEDATA,             "DNGPrivateData"},
  {TIFFTAG_MAKERNOTESAFETY,            "MakerNoteSafety"},
  {TIFFTAG_CALIBRATIONILLUMINANT1,     "CalibrationIlluminant1"},
  {TIFFTAG_CALIBRATIONILLUMINANT2,     "CalibrationIlluminant2"},
  {TIFFTAG_RAWDATAUNIQUEID,            "RawDataUniqueID"},
  {TIFFTAG_ORIGINALRAWFILENAME,        "OriginalRawFileName"},
  {TIFFTAG_ORIGINALRAWFILEDATA,        "OriginalRawFileData"},
  {TIFFTAG_ACTIVEAREA,                 "ActiveArea"},
  {TIFFTAG_MASKEDAREAS,                "MaskedAreas"},
  {TIFFTAG_ASSHOTICCPROFILE,           "AsShotICCProfile"},
  {TIFFTAG_ASSHOTPREPROFILEMATRIX,     "AsShotPreProfileMatrix"},
  {TIFFTAG_CURRENTICCPROFILE,          "CurrentICCProfile"},
  {TIFFTAG_CURRENTPREPROFILEMATRIX,    "CurrentPreProfileMatrix"},
  {TIFFTAG_PERSAMPLE,                  "PerSample"},
/* end DNG tags */
/* begin TIFF/FX tags */
  {TIFFTAG_INDEXED,                    "Indexed"},
  {TIFFTAG_GLOBALPARAMETERSIFD,        "GlobalParametersIFD"},
  {TIFFTAG_PROFILETYPE,                "ProfileType"},
  {TIFFTAG_FAXPROFILE,                 "FaxProfile"},
  {TIFFTAG_CODINGMETHODS,              "CodingMethods"},
  {TIFFTAG_VERSIONYEAR,                "VersionYear"},
  {TIFFTAG_MODENUMBER,                 "ModeNumber"},
  {TIFFTAG_DECODE,                     "Decode"},
  {TIFFTAG_IMAGEBASECOLOR,             "ImageBaseColor"},
  {TIFFTAG_T82OPTIONS,                 "T82Options"},
  {TIFFTAG_STRIPROWCOUNTS,             "StripRowCounts"},
  {TIFFTAG_IMAGELAYER,                 "ImageLayer"},
/* end DNG tags */
/* begin pseudo tags */
// Carl Zeiss LSM
  {TIFFTAG_CZ_LSMINFO,                 "CarlZeissLSMInfo"},

// GEOTIFF
  {TIFFTAG_GEOPIXELSCALE,              "GeoPixelScale"},
  {TIFFTAG_INTERGRAPH_MATRIX,          "Intergraph TransformationMatrix"},
  {TIFFTAG_GEOTRANSMATRIX,             "GeoTransformationMatrix"},
  {TIFFTAG_GEOTIEPOINTS,               "GeoTiePoints"},
  {TIFFTAG_GEOKEYDIRECTORY,            "GeoKeyDirectory"},
  {TIFFTAG_GEODOUBLEPARAMS,            "GeoDoubleParams"},
  {TIFFTAG_GEOASCIIPARAMS,             "GeoASCIIParams"},
  {TIFFTAG_PREDICTOR,                  "Predictor"},

  {TIFFTAG_JPEGIFOFFSET,               "JpegInterchangeFormat"},
  {TIFFTAG_JPEGIFBYTECOUNT,            "JpegInterchangeFormatLength"},
  {TIFFTAG_JPEGQTABLES,                "JpegQTables"},
  {TIFFTAG_JPEGDCTABLES,               "JpegDcTables"},
  {TIFFTAG_JPEGACTABLES,               "JpegAcTables"},
  {TIFFTAG_JPEGPROC,                   "JpegProc"},
  {TIFFTAG_JPEGRESTARTINTERVAL,        "JpegRestartInterval"},
};

static const struct exiftagname
{
  uint32 tag;
  const char* name;
} exiftagnames[] = {
  {EXIFTAG_EXPOSURETIME,             "ExposureTime"},
  {EXIFTAG_FNUMBER,                  "FNumber"},
  {EXIFTAG_EXPOSUREPROGRAM,          "ExposureProgram"},
  {EXIFTAG_SPECTRALSENSITIVITY,      "SpectralSensitivity"},
  {EXIFTAG_ISOSPEEDRATINGS,          "ISOSpeedRatings"},
  {EXIFTAG_OECF,                     "OptoelectricConversionFactor"},
  {EXIFTAG_EXIFVERSION,              "ExifVersion"},
  {EXIFTAG_DATETIMEORIGINAL,         "DateTimeOriginal"},
  {EXIFTAG_DATETIMEDIGITIZED,        "DateTimeDigitized"},
  {EXIFTAG_COMPONENTSCONFIGURATION,  "ComponentsConfiguration"},
  {EXIFTAG_COMPRESSEDBITSPERPIXEL,   "CompressedBitsPerPixel"},
  {EXIFTAG_SHUTTERSPEEDVALUE,        "ShutterSpeedValue"},
  {EXIFTAG_APERTUREVALUE,            "ApertureValue"},
  {EXIFTAG_BRIGHTNESSVALUE,          "BrightnessValue"},
  {EXIFTAG_EXPOSUREBIASVALUE,        "ExposureBiasValue"},
  {EXIFTAG_MAXAPERTUREVALUE,         "MaxApertureValue"},
  {EXIFTAG_SUBJECTDISTANCE,          "SubjectDistance"},
  {EXIFTAG_METERINGMODE,             "MeteringMode"},
  {EXIFTAG_LIGHTSOURCE,              "LightSource"},
  {EXIFTAG_FLASH,                    "Flash"},
  {EXIFTAG_FOCALLENGTH,              "FocalLength"},
  {EXIFTAG_SUBJECTAREA,              "SubjectArea"},
  {EXIFTAG_MAKERNOTE,                "MakerNote"},
  {EXIFTAG_USERCOMMENT,              "UserComment"},
  {EXIFTAG_SUBSECTIME,               "SubSecTime"},
  {EXIFTAG_SUBSECTIMEORIGINAL,       "SubSecTimeOriginal"},
  {EXIFTAG_SUBSECTIMEDIGITIZED,      "SubSecTimeDigitized"},
  {EXIFTAG_FLASHPIXVERSION,          "FlashpixVersion"},
  {EXIFTAG_COLORSPACE,               "ColorSpace"},
  {EXIFTAG_PIXELXDIMENSION,          "PixelXDimension"},
  {EXIFTAG_PIXELYDIMENSION,          "PixelYDimension"},
  {EXIFTAG_RELATEDSOUNDFILE,         "RelatedSoundFile"},
  {EXIFTAG_FLASHENERGY,              "FlashEnergy"},
  {EXIFTAG_SPATIALFREQUENCYRESPONSE, "SpatialFrequencyResponse"},
  {EXIFTAG_FOCALPLANEXRESOLUTION,    "FocalPlaneXResolution"},
  {EXIFTAG_FOCALPLANEYRESOLUTION,    "FocalPlaneYResolution"},
  {EXIFTAG_FOCALPLANERESOLUTIONUNIT, "FocalPlaneResolutionUnit"},
  {EXIFTAG_SUBJECTLOCATION,          "SubjectLocation"},
  {EXIFTAG_EXPOSUREINDEX,            "ExposureIndex"},
  {EXIFTAG_SENSINGMETHOD,            "SensingMethod"},
  {EXIFTAG_FILESOURCE,               "FileSource"},
  {EXIFTAG_SCENETYPE,                "SceneType"},
  {EXIFTAG_CFAPATTERN,               "CFAPattern"},
  {EXIFTAG_CUSTOMRENDERED,           "CustomRendered"},
  {EXIFTAG_EXPOSUREMODE,             "ExposureMode"},
  {EXIFTAG_WHITEBALANCE,             "WhiteBalance"},
  {EXIFTAG_DIGITALZOOMRATIO,         "DigitalZoomRatio"},
  {EXIFTAG_FOCALLENGTHIN35MMFILM,    "FocalLengthIn35mmFilm"},
  {EXIFTAG_SCENECAPTURETYPE,         "SceneCaptureType"},
  {EXIFTAG_GAINCONTROL,              "GainControl"},
  {EXIFTAG_CONTRAST,                 "Contrast"},
  {EXIFTAG_SATURATION,               "Saturation"},
  {EXIFTAG_SHARPNESS,                "Sharpness"},
  {EXIFTAG_DEVICESETTINGDESCRIPTION, "DeviceSettingDescription"},
  {EXIFTAG_SUBJECTDISTANCERANGE,     "SubjectDistanceRange"},
  {EXIFTAG_IMAGEUNIQUEID,            "ImageUniqueID"},
  {1,                                "GPSLatitudeRef"},
  {2,                                "GPSLatitude"},
  {3,                                "GPSLongitudeRef"},
  {4,                                "GPSLongitude"},
};

#ifdef _MSC_VER
#define snprintf _snprintf
#endif

void LibtiffErrorHandler(const char* module, const char* fmt, va_list ap)
{
  char buf[2048];

  int off = 0;
  Q_UNUSED(module)
  //if (module)
  //off = snprintf(buf, 2048, "libtiff %s: ", module);
  //else
  off = snprintf(buf, 2048, "libtiff: ");
  vsnprintf(buf + off, 2048 - off, fmt, ap);
  throw nim::ZIOException(QString(buf));
}

void LibtiffErrorHandlerIgnoreColormapError(const char* module, const char* fmt, va_list ap)
{
  char buf[2048];

  int off = 0;
  Q_UNUSED(module)
  //if (module)
  //off = snprintf(buf, 2048, "libtiff %s: ", module);
  //else
  off = snprintf(buf, 2048, "libtiff: ");
  vsnprintf(buf + off, 2048 - off, fmt, ap);
  QString str(buf);
  if (str.contains("Colormap", Qt::CaseInsensitive))
    return;
  throw nim::ZIOException(str);
}

const uint8_t bitmasks1[] = {
  0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01
};

const uint8_t bitmasks2[] = {
  0xC0, 0x30, 0x0C, 0x03
};

const uint8_t bitmasks4[] = {
  0xF0, 0x0F
};

}

namespace nim {

bool ZTiffIFD::isReducedResolutionImage() const
{
  return subfileTypeData() & FILETYPE_REDUCEDIMAGE;
}

bool ZTiffIFD::isMultipageDocument() const
{
  return subfileTypeData() & FILETYPE_PAGE;
}

bool ZTiffIFD::isTransparencyMask() const
{
  return subfileTypeData() & FILETYPE_MASK;
}

VoxelFormat ZTiffIFD::voxelFormat(size_t sample) const
{
  int64_t i = indexOf(TIFFTAG_SAMPLEFORMAT);
  if (i != -1) {
    uint16_t vfn = m_entries[i].dataAt<uint16_t>(sample);
    if (vfn == 4)  // void
      vfn = 1; // unsigned
    if (vfn == 5 || vfn == 6) {
      throw ZIOException("complex TIFFTAG_SAMPLEFORMAT is not supported");
    }
    if (vfn < 1 || vfn > 6) {
      throw ZIOException(QString("illegal TIFFTAG_SAMPLEFORMAT %1").arg(vfn));
    }
    VoxelFormat vf = static_cast<VoxelFormat>(vfn);
    return vf;
  } else
    return VoxelFormat::Unsigned;
}

size_t ZTiffIFD::samplesPerPixel() const
{
  int64_t i = indexOf(TIFFTAG_SAMPLESPERPIXEL);
  if (i != -1)
    return m_entries[i].dataAt<uint16_t>(0);
  else
    return 1;
}

size_t ZTiffIFD::bitsPerSample(size_t sample) const
{
  int64_t i = indexOf(TIFFTAG_BITSPERSAMPLE);
  if (i != -1)
    return m_entries[i].dataAt<uint16_t>(sample);
  else
    return 1;
}

size_t ZTiffIFD::imageWidth() const
{
  int64_t i = indexOf(TIFFTAG_IMAGEWIDTH);
  if (i != -1) {
    if (m_entries[i].dataType() == DataType::Short)
      return m_entries[i].dataAt<uint16_t>(0);
    else
      return m_entries[i].dataAt<uint32_t>(0);
  } else
    throw ZIOException("TIFFTAG_IMAGEWIDTH is required field of tiff.");
  return 0;
}

size_t ZTiffIFD::imageHeight() const
{
  int64_t i = indexOf(TIFFTAG_IMAGELENGTH);
  if (i != -1) {
    if (m_entries[i].dataType() == DataType::Short)
      return m_entries[i].dataAt<uint16_t>(0);
    else
      return m_entries[i].dataAt<uint32_t>(0);
  } else
    throw ZIOException("TIFFTAG_IMAGELENGTH is required field of tiff.");
  return 0;
}

uint16_t ZTiffIFD::photometricInterpretation() const
{
  int64_t i = indexOf(TIFFTAG_PHOTOMETRIC);
  if (i != -1)
    return m_entries[i].dataAt<uint16_t>(0);
  else
    throw ZIOException("TIFFTAG_PHOTOMETRIC is required field of tiff.");
  return 0;
}

QString ZTiffIFD::imageDescriptionAsQString() const
{
  int64_t i = indexOf(TIFFTAG_IMAGEDESCRIPTION);
  if (i != -1)
    return QString(m_entries[i].dataArray<char>());
  else
    return QString();
}

uint16_t ZTiffIFD::orientation() const
{
  int64_t i = indexOf(TIFFTAG_ORIENTATION);
  if (i != -1)
    return m_entries[i].dataAt<uint16_t>(0);
  else
    return 1;
}

uint16_t ZTiffIFD::compression() const
{
  int64_t i = indexOf(TIFFTAG_COMPRESSION);
  if (i != -1)
    return m_entries[i].dataAt<uint16_t>(0);
  else
    return 1;
}

uint16_t ZTiffIFD::planarConfiguration() const
{
  int64_t i = indexOf(TIFFTAG_PLANARCONFIG);
  if (i != -1)
    return m_entries[i].dataAt<uint16_t>(0);
  else
    return 1;
}

int ZTiffIFD::extraSample() const
{
  int64_t i = indexOf(TIFFTAG_EXTRASAMPLES);
  if (i != -1) {
    if (m_entries[i].count() > 1) {
      throw ZIOException(QString("Tiff with multiple TIFFTAG_EXTRASAMPLES is not supported."));
    }
    return m_entries[i].dataAt<uint16_t>(0);
  } else {
    return -1;
  }
}

bool ZTiffIFD::isTiledImage() const
{
  return indexOf(TIFFTAG_TILEWIDTH) != -1;
}

uint64_t ZTiffIFD::stripsPerImage() const
{
  if (!isTiledImage())
    return (imageHeight() + rowsPerStrip() - 1) / rowsPerStrip();
  return 0;
}

uint64_t ZTiffIFD::tilesPerImage() const
{
  if (isTiledImage()) {
    uint64_t tileAcross = (imageWidth() + tileWidth() - 1) / tileWidth();
    uint64_t tileDown = (imageHeight() + tileHeight() - 1) / tileHeight();
    return tileAcross * tileDown;
  }
  return 0;
}

uint64_t ZTiffIFD::rowsPerStrip() const
{
  int64_t i = indexOf(TIFFTAG_ROWSPERSTRIP);
  if (i != -1) {
    if (m_entries[i].dataType() == DataType::Short)
      return m_entries[i].dataAt<uint16_t>(0);
    else
      return m_entries[i].dataAt<uint32_t>(0);
  }
  return std::numeric_limits<uint32_t>::max();
}

uint64_t ZTiffIFD::stripOffsets(size_t idx) const
{
  int64_t i = indexOf(TIFFTAG_STRIPOFFSETS);
  if (i != -1) {
    if (idx >= m_entries[i].count()) {
      throw ZIOException(QString("Wrong idx %1 for strip offsets").arg(idx));
    }
    if (m_entries[i].dataType() == DataType::Short)
      return m_entries[i].dataAt<uint16_t>(idx);
    else if (m_entries[i].dataType() == DataType::Long)
      return m_entries[i].dataAt<uint32_t>(idx);
    else
      return m_entries[i].dataAt<uint64_t>(idx);
  }
  return 0;
}

uint64_t ZTiffIFD::stripByteCounts(size_t idx) const
{
  int64_t i = indexOf(TIFFTAG_STRIPBYTECOUNTS);
  if (i != -1) {
    if (idx >= m_entries[i].count()) {
      throw ZIOException(QString("Wrong idx %1 for strip byte counts").arg(idx));
    }
    if (m_entries[i].dataType() == DataType::Short)
      return m_entries[i].dataAt<uint16_t>(idx);
    else if (m_entries[i].dataType() == DataType::Long)
      return m_entries[i].dataAt<uint32_t>(idx);
    else
      return m_entries[i].dataAt<uint64_t>(idx);
  }
  return 0;
}

uint64_t ZTiffIFD::tileWidth() const
{
  int64_t i = indexOf(TIFFTAG_TILEWIDTH);
  if (i < 0) {
    throw ZIOException(QString("Tile width required for tile image"));
  }
  if (m_entries[i].dataType() == DataType::Short)
    return m_entries[i].dataAt<uint16_t>(0);
  else
    return m_entries[i].dataAt<uint32_t>(0);
}

uint64_t ZTiffIFD::tileHeight() const
{
  int64_t i = indexOf(TIFFTAG_TILELENGTH);
  if (i < 0) {
    throw ZIOException(QString("Tile length required for tile image"));
  }
  if (m_entries[i].dataType() == DataType::Short)
    return m_entries[i].dataAt<uint16_t>(0);
  else
    return m_entries[i].dataAt<uint32_t>(0);
}

uint64_t ZTiffIFD::tileOffsets(size_t idx) const
{
  int64_t i = indexOf(TIFFTAG_TILEOFFSETS);
  if (i != -1) {
    if (idx >= m_entries[i].count()) {
      throw ZIOException(QString("Wrong idx %1 for tile offsets").arg(idx));
    }
    if (m_entries[i].dataType() == DataType::Short)
      return m_entries[i].dataAt<uint16_t>(idx);
    else if (m_entries[i].dataType() == DataType::Long)
      return m_entries[i].dataAt<uint32_t>(idx);
    else
      return m_entries[i].dataAt<uint64_t>(idx);
  }
  return 0;
}

uint64_t ZTiffIFD::tileByteCounts(size_t idx) const
{
  int64_t i = indexOf(TIFFTAG_TILEBYTECOUNTS);
  if (i != -1) {
    if (idx >= m_entries[i].count()) {
      throw ZIOException(QString("Wrong idx %1 for tile byte counts").arg(idx));
    }
    if (m_entries[i].dataType() == DataType::Short)
      return m_entries[i].dataAt<uint16_t>(idx);
    else if (m_entries[i].dataType() == DataType::Long)
      return m_entries[i].dataAt<uint32_t>(idx);
    else
      return m_entries[i].dataAt<uint64_t>(idx);
  }
  return 0;
}

QString ZTiffIFD::toQString() const
{
  QString res;
  for (size_t i = 0; i < m_entries.size(); ++i) {
    res = res % m_entries[i].toQString() % QString("\n");
  }
  if (!m_subIFDs.empty()) {
    res = res % QString("\n");
    for (size_t i = 0; i < m_subIFDs.size(); ++i) {
      res = res % QString("Sub IFD %1\n").arg(i);
      res = res % m_subIFDs[i].toQString();
    }
  }
  if (!m_exifIFD.empty()) {
    res = res % QString("\nExif IFD\n");
    res = res % m_exifIFD[0].toQString();
  }
  if (!m_gpsIFD.empty()) {
    res = res % QString("\nGPS IFD\n");
    res = res % m_gpsIFD[0].toQString();
  }
  if (!m_interoperabilityIFD.empty()) {
    res = res % QString("\nInteroperability IFD\n");
    res = res % m_interoperabilityIFD[0].toQString();
  }
  return res;
}

bool ZTiffIFD::isGrayscaleColormap() const
{
  int64_t i = indexOf(TIFFTAG_COLORMAP);
  if (i != -1) {
    size_t count = m_entries[i].count();
    if (count % 3 != 0)
      return true;
    size_t colorCount = count / 3;
    for (size_t j = 0; j < colorCount; ++j) {
      if (m_entries[i].dataAt<uint16_t>(j) != m_entries[i].dataAt<uint16_t>(j + colorCount) ||
          m_entries[i].dataAt<uint16_t>(j) != m_entries[i].dataAt<uint16_t>(j + 2 * colorCount))
        return false;
    }
  }
  return true;
}

std::vector<ZImgMetatag> ZTiffIFD::extractMetadata() const
{
  std::set<uint32_t> tags;
  tags.insert(TIFFTAG_MAKE);
  tags.insert(TIFFTAG_MODEL);
  tags.insert(TIFFTAG_ORIENTATION);
  tags.insert(TIFFTAG_IMAGEDESCRIPTION);
  tags.insert(TIFFTAG_SOFTWARE);
  tags.insert(TIFFTAG_DATETIME);
  tags.insert(TIFFTAG_ARTIST);
  tags.insert(TIFFTAG_COPYRIGHT);

  std::vector<ZImgMetatag> res;
  for (size_t i = 0; i < m_entries.size(); ++i) {
    if (tags.find(m_entries[i].tag()) != tags.end())
      res.push_back(m_entries[i]);
  }
  if (hasExifIFD()) {
    res.insert(res.end(), exifIFD()->m_entries.begin(), exifIFD()->m_entries.end());
  }
  if (hasGpsIFD()) {
    res.insert(res.end(), gpsIFD()->m_entries.begin(), gpsIFD()->m_entries.end());
  }
  return res;
}

/*
std::string ZTiffIFD::toString() const
{
  std::ostringstream res;
  for (size_t i=0; i<m_entries.size(); ++i) {
    res << m_entries[i].toString() << std::endl;
  }
  if (!m_subIFDs.empty()) {
    res << std::endl;
    for (size_t i=0; i<m_subIFDs.size(); ++i) {
      res << "Sub IFD " << i << std::endl;
      res << m_subIFDs[i].toString();
    }
  }
  if (!m_exifIFD.empty()) {
    res << std::endl;
    res << "Exif IFD" << std::endl;
    res << m_exifIFD[0].toString();
  }
  if (!m_gpsIFD.empty()) {
    res << std::endl;
    res << "GPS IFD" << std::endl;
    res << m_gpsIFD[0].toString();
  }
  if (!m_interoperabilityIFD.empty()) {
    res << std::endl;
    res << "Interoperability IFD" << std::endl;
    res << m_interoperabilityIFD[0].toString();
  }
  return res.str();
}
*/

int64_t ZTiffIFD::indexOf(uint64_t tag) const
{
  for (size_t i = 0; i < m_entries.size(); ++i) {
    if (m_entries[i].tag() == tag) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

uint32_t ZTiffIFD::subfileTypeData() const
{
  int64_t i = indexOf(TIFFTAG_SUBFILETYPE);
  if (i != -1)
    return m_entries[i].dataAt<uint32_t>(0);
  return 0;
}

ZTiff::ZTiff()
  : m_tif(nullptr, TIFFClose), m_useColormap(true)
{
}

QString ZTiff::toQString() const
{
  QString res;
  for (size_t i = 0; i < m_ifds.size(); ++i) {
    res = res % QString("Directory %1: offset %2 (%3) next %4 (%5)\n")
      .arg(i)
      .arg(m_ifds[i].offset())
      .arg(m_ifds[i].offset(), 0, 16)
      .arg(m_ifds[i].nextIFDOffset())
      .arg(m_ifds[i].nextIFDOffset(), 0, 16);
    res = res % m_ifds[i].toQString() % QString("\n");
  }
  return res;
}

/*
std::string ZTiff::toString() const
{
  std::ostringstream res;
  for (size_t i=0; i<m_ifds.size(); ++i) {
    res << "Directory " << i
        << ": offset " << m_ifds[i].offset()
        << " (" << std::hex << m_ifds[i].offset() << std::dec << ")"
        << " next " << m_ifds[i].nextIFDOffset()
        << " (" << std::hex << m_ifds[i].nextIFDOffset() << std::dec << ")"
        << std::endl;
    res << m_ifds[i].toString() << std::endl;
  }
  return res.str();
}
*/

void ZTiff::load(const QString& filename, bool tagOnly)
{
  close();

  readIFDs(filename, m_ifds);

  if (m_useColormap) {
    bool allGray = true;
    for (size_t i = 0; i < m_ifds.size(); ++i) {
      if (!m_ifds[i].isGrayscaleColormap()) {
        allGray = false;
        break;
      }
    }
    if (allGray)
      m_useColormap = false;
  }
  if (m_useColormap) {
    QString imageDes = m_ifds[0].imageDescriptionAsQString();
    if (imageDes.startsWith("ImageJ=") && imageDes.contains("images=")) {
      m_useColormap = false;
    }
  }

  if (!tagOnly) {
    TIFFSetWarningHandler(0);
    if (m_useColormap) {
      TIFFSetErrorHandler(LibtiffErrorHandler);
    } else {
      TIFFSetErrorHandler(LibtiffErrorHandlerIgnoreColormapError);
    }

    try {
#if defined(_WIN32) || defined(_WIN64)
      m_tif.reset(TIFFOpenW(filename.toStdWString().c_str(), "r"));
#else
      m_tif.reset(TIFFOpen(QFile::encodeName(filename).constData(), "r"));
#endif
    }
    catch (const ZIOException& e) {
      QString err(e.what());
      if (err.contains("Colormap", Qt::CaseInsensitive)) {
        m_tif.reset();
        LOG(WARNING) << "Disable colormap because of error " << err;
        m_useColormap = false;
        TIFFSetErrorHandler(LibtiffErrorHandlerIgnoreColormapError);
#if defined(_WIN32) || defined(_WIN64)
        m_tif.reset(TIFFOpenW(filename.toStdWString().c_str(), "r"));
#else
        m_tif.reset(TIFFOpen(QFile::encodeName(filename).constData(), "r"));
#endif
      } else {
        m_tif.reset();
        throw;
      }
    }
    if (!m_tif) {
      throw ZIOException("Libtiff can not open Tiff file");
    }
  }
}

void ZTiff::load(std::istream& fs, bool tagOnly)
{
  close();

  readIFDs(fs, m_ifds);

  if (!tagOnly) {
    if (fs.fail())
      throw ZIOException("Read tiff tags failed");
    fs.clear();
    fs.seekg(0);

    TIFFSetWarningHandler(0);
    if (m_useColormap) {
      TIFFSetErrorHandler(LibtiffErrorHandler);
    } else {
      TIFFSetErrorHandler(LibtiffErrorHandlerIgnoreColormapError);
    }

    try {
      m_tif.reset(TIFFStreamOpen("MemTiff", &fs));
    }
    catch (const ZIOException& e) {
      QString err(e.what());
      if (err.contains("Colormap", Qt::CaseInsensitive)) {
        m_tif.reset();
        LOG(WARNING) << "Disable colormap because of error " << err;
        m_useColormap = false;
        TIFFSetErrorHandler(LibtiffErrorHandlerIgnoreColormapError);
        m_tif.reset(TIFFStreamOpen("MemTiff", &fs));
      } else {
        m_tif.reset();
        throw;
      }
    }
    if (!m_tif) {
      throw ZIOException("Libtiff can not open Tiff file");
    }
  }
}

bool ZTiff::isLsmFile() const
{
  return !m_ifds.empty() && m_ifds[0].containsTag(TIFFTAG_CZ_LSMINFO);
}

const ZImgMetatag& ZTiff::lsmInfoTag() const
{
  return m_ifds[0].tag(m_ifds[0].indexOf(TIFFTAG_CZ_LSMINFO));
}

void ZTiff::readInfoFromIFD(const ZTiffIFD& ifd, ZImgInfo& info)
{
  info.width = ifd.imageWidth();
  info.height = ifd.imageHeight();
  if (ifd.orientation() > 4)
    std::swap(info.width, info.height);

  if (ifd.photometricInterpretation() == PHOTOMETRIC_MINISBLACK ||
      ifd.photometricInterpretation() == PHOTOMETRIC_MINISWHITE ||
      ifd.photometricInterpretation() == PHOTOMETRIC_RGB ||
      !m_useColormap) {

    info.numChannels = ifd.samplesPerPixel();
    size_t bps = ifd.bitsPerSample(0);
    info.validBitCount = bps;
    for (size_t j = 1; j < info.numChannels; ++j)
      if (ifd.bitsPerSample(j) != bps)
        throw ZIOException("Different bits per sample is not supported.");
    if (bps > 64 || (bps > 8 && bps % 8 != 0))
      throw ZIOException(QString("%1 bits per sample tiff is not supported.").arg(bps));
    info.bytesPerVoxel = std::ceil(bps * 1.0 / 8);
    VoxelFormat vf = ifd.voxelFormat(0);
    for (size_t j = 1; j < info.numChannels; ++j)
      if (ifd.voxelFormat(j) != vf)
        throw ZIOException("Different sample format is not supported.");
    info.voxelFormat = vf;

    if (ifd.extraSample() >= 0) {
      info.lastChannelIsAlphaChannel = true;
    }
  } else {
    info.numChannels = 4;
    info.lastChannelIsAlphaChannel = true;
    info.bytesPerVoxel = 1;
    info.voxelFormat = VoxelFormat::Unsigned;
  }

  info.depth = 1;
  info.numTimes = 1;

  info.createDefaultDescriptions();
}

void ZTiff::readImgFromIFD(size_t ifdIdx, ZImg& img)
{
  if (TIFFSetDirectory(m_tif.get(), ifdIdx) != 1)
    throw ZIOException(QString("Can not read ifd of index %1").arg(ifdIdx));
  readImg(img,
          m_ifds[ifdIdx].extraSample() == EXTRASAMPLE_ASSOCALPHA ||
          m_ifds[ifdIdx].extraSample() == EXTRASAMPLE_UNSPECIFIED);
}

void ZTiff::readImgFromIFD(const ZTiffIFD& ifd, ZImg& img)
{
  if (TIFFSetSubDirectory(m_tif.get(), ifd.offset()) != 1)
    throw ZIOException(QString("Can not read ifd at offset %1").arg(ifd.offset()));
  readImg(img,
          ifd.extraSample() == EXTRASAMPLE_ASSOCALPHA ||
          ifd.extraSample() == EXTRASAMPLE_UNSPECIFIED);
}

ZImg ZTiff::readThumbnailFromIFD(const ZTiffIFD& ifd)
{
  ZImg res;

  if (ifd.isReducedResolutionImage()) {
    try {
      ZImgInfo info;
      readInfoFromIFD(ifd, info);

      res = ZImg(info);
      readImgFromIFD(ifd, res);
    }
    catch (const ZIOException& e) {
      LOG(WARNING) << "read thumbnail from tif ifd failed: " << e.what();
      res.clear();
    }
  }

  return res;
}

#pragma pack(push, 1)
struct ZTiffHeader
{
  TIFFHeaderBig header = {TIFF_LITTLEENDIAN, 43, 8, 0, 16};
  uint64_t dircount = 8;

  uint16_t tag1 = TIFFTAG_IMAGEWIDTH;
  uint16_t type1 = enumToUnderlyingType(DataType::Long);
  uint64_t count1 = 1;
  uint32_t width = 0;
  uint32_t fill1 = 0;

  uint16_t tag2 = TIFFTAG_IMAGELENGTH;
  uint16_t type2 = enumToUnderlyingType(DataType::Long);
  uint64_t count2 = 1;
  uint32_t height = 0;
  uint32_t fill2 = 0;

  uint16_t tag3 = TIFFTAG_BITSPERSAMPLE;
  uint16_t type3 = enumToUnderlyingType(DataType::Short);
  uint64_t count3 = 1;
  uint16_t bitPerSample = 0;
  uint16_t fill31 = 0;
  uint32_t fill32 = 0;

  uint16_t tag4 = TIFFTAG_SAMPLESPERPIXEL;
  uint16_t type4 = enumToUnderlyingType(DataType::Short);
  uint64_t count4 = 1;
  uint16_t samplesPerPixel = 0;
  uint16_t fill41 = 0;
  uint32_t fill42 = 0;

  uint16_t tag5 = TIFFTAG_COMPRESSION;
  uint16_t type5 = enumToUnderlyingType(DataType::Short);
  uint64_t count5 = 1;
  uint16_t compression = 0;
  uint16_t fill51 = 0;
  uint32_t fill52 = 0;

  uint16_t tag6 = TIFFTAG_PHOTOMETRIC;
  uint16_t type6 = enumToUnderlyingType(DataType::Short);
  uint64_t count6 = 1;
  uint16_t photoMetric = 1;
  uint16_t fill61 = 0;
  uint32_t fill62 = 0;

  uint16_t tag7 = TIFFTAG_STRIPOFFSETS;
  uint16_t type7 = enumToUnderlyingType(DataType::Long8);
  uint64_t count7 = 1;
  uint64_t stripOffset = 0;

  uint16_t tag8 = TIFFTAG_STRIPBYTECOUNTS;
  uint16_t type8 = enumToUnderlyingType(DataType::Long8);
  uint64_t count8 = 1;
  uint64_t stripByteCount = 0;

  uint64_t nextdiroff = 0;
};

#pragma pack(pop)

void ZTiff::writeTiffHeader(uint8_t* mem, size_t width, size_t height, size_t bitsPerSample, size_t samplesPerPixel,
                            size_t compression,
                            uint64_t stripOffset, uint64_t stripByteCount)
{
  static_assert(sizeof(ZTiffHeader) == 192, "wrong tiff header size");
  CHECK(stripOffset >= 192);
  ZTiffHeader header;
  header.width = width;
  header.height = height;
  header.bitPerSample = bitsPerSample;
  header.samplesPerPixel = samplesPerPixel;
  header.compression = compression;
  header.stripOffset = stripOffset;
  header.stripByteCount = stripByteCount;
  memcpy(mem, &header, sizeof(header));
}

uint64_t ZTiff::readIFD(std::istream& fs, ZTiffIFD& ifd, uint64_t off, bool bigtiff, bool swabflag) const
{
  uint64_t nextdiroff = 0;

  if (off == 0)      /* no more directories */
    return 0;
  fs.seekg(off);

  uint16_t dircount;
  uint32_t direntrysize;
  if (!bigtiff) {
    readStream(fs, &dircount, sizeof(uint16_t));
    if (swabflag)
      boost::endian::endian_reverse_inplace(dircount);
    direntrysize = 12;
  } else {
    uint64_t dircount64 = 0;
    readStream(fs, &dircount64, sizeof(uint64_t));
    if (swabflag)
      boost::endian::endian_reverse_inplace(dircount64);
    if (dircount64 > 0xFFFF) {
      throw ZIOException("Sanity check on directory count failed");
    }
    dircount = static_cast<uint16_t>(dircount64);
    direntrysize = 20;
  }

  std::vector<char> dirmemvector(dircount * direntrysize);
  fs.read(dirmemvector.data(), dirmemvector.size());
  uint32_t n = fs.gcount();
  if (n != dirmemvector.size()) {
    n /= direntrysize;
    LOG(WARNING) << "Could only read " << n << " of " << dircount
                 << " entries in directory at offset " << off;
    dircount = n;
    nextdiroff = 0;
  } else {
    if (!bigtiff) {
      uint32_t nextdiroff32;
      // reinterpret_cast allowed (AliasedType is char or unsigned char: this permits
      // examination of the object representation of any object as an array of unsigned char.)
      fs.read(reinterpret_cast<char*>(&nextdiroff32), sizeof(uint32_t));
      if (static_cast<size_t>(fs.gcount()) != sizeof(uint32_t))
        nextdiroff32 = 0;
      if (swabflag)
        boost::endian::endian_reverse_inplace(nextdiroff32);
      nextdiroff = nextdiroff32;
    } else {
      fs.read(reinterpret_cast<char*>(&nextdiroff), sizeof(uint64_t));
      if (static_cast<size_t>(fs.gcount()) != sizeof(uint64_t))
        nextdiroff = 0;
      if (swabflag)
        boost::endian::endian_reverse_inplace(nextdiroff);
    }
  }
  fs.clear();

  for (char* dp = dirmemvector.data(), n = dircount; n > 0; n--) {
    ZImgMetatag field;

    uint16_t tag;
    memcpy(&tag, dp, sizeof(tag));
    dp += sizeof(uint16_t);
    if (swabflag)
      boost::endian::endian_reverse_inplace(tag);
    field.setTag(tag);
    field.setName(tagToName(tag));

    uint16_t type;
    memcpy(&type, dp, sizeof(type));
    dp += sizeof(uint16_t);
    if (swabflag)
      boost::endian::endian_reverse_inplace(type);
    if (!isValidDataType(type))
      throw ZIOException(QString("Wrong tiff tag dataType: %1").arg(type));
    field.setDataType(static_cast<DataType>(type));

    uint64_t count;
    if (!bigtiff) {
      uint32_t count32;
      memcpy(&count32, dp, sizeof(count32));
      dp += sizeof(uint32_t);
      if (swabflag)
        boost::endian::endian_reverse_inplace(count32);
      count = count32;
    } else {
      memcpy(&count, dp, sizeof(count));
      dp += sizeof(uint64_t);
      if (swabflag)
        boost::endian::endian_reverse_inplace(count);
    }
    field.setCount(count);

    bool datafits = true;
    uint64_t dataoffset = 0;
    if (!bigtiff) {
      if (field.dataByteNumber() > 4) {
        datafits = false;
        uint32_t dataoffset32;
        memcpy(&dataoffset32, dp, sizeof(dataoffset32));
        if (swabflag)
          boost::endian::endian_reverse_inplace(dataoffset32);
        dataoffset = dataoffset32;
      } else {
        memcpy(field.dataArray(), dp, field.dataByteNumber());
      }
      dp += sizeof(uint32_t);
    } else {
      if (field.dataByteNumber() > 8) {
        datafits = false;
        memcpy(&dataoffset, dp, sizeof(dataoffset));
        if (swabflag)
          boost::endian::endian_reverse_inplace(dataoffset);
      } else {
        memcpy(field.dataArray(), dp, field.dataByteNumber());
      }
      dp += sizeof(uint64_t);
    }

    if (!datafits) {
      fs.seekg(dataoffset);
      readStream(fs, field.dataArray<char>(), field.dataByteNumber());
    }
    if (swabflag) {
      switch (type) {
        case TIFF_BYTE:
        case TIFF_ASCII:
        case TIFF_SBYTE:
        case TIFF_UNDEFINED:
          break;
        case TIFF_SHORT:
        case TIFF_SSHORT:
          for (size_t sc = 0; sc < count; ++sc) {
            boost::endian::endian_reverse_inplace(field.dataAt<uint16_t>(sc));
          }
          break;
        case TIFF_LONG:
        case TIFF_SLONG:
        case TIFF_FLOAT:
        case TIFF_IFD:
          for (size_t sc = 0; sc < count; ++sc) {
            boost::endian::endian_reverse_inplace(field.dataAt<uint32_t>(sc));
          }
          break;
        case TIFF_RATIONAL:
        case TIFF_SRATIONAL:
          for (size_t sc = 0; sc < count * 2; ++sc) {
            boost::endian::endian_reverse_inplace(field.dataAt<uint32_t>(sc));
          }
          break;
        case TIFF_DOUBLE:
        case TIFF_LONG8:
        case TIFF_SLONG8:
        case TIFF_IFD8:
          for (size_t sc = 0; sc < count; ++sc) {
            boost::endian::endian_reverse_inplace(field.dataAt<uint64_t>(sc));
          }
          break;
      }
    }
    ifd.addField(field);

    if (field.tag() == TIFFTAG_SUBIFD) {
      for (size_t sc = 0; sc < field.count(); ++sc) {
        ZTiffIFD subIFD;
        if (bigtiff)
          readIFD(fs, subIFD, field.dataAt<uint64_t>(sc), bigtiff, swabflag);
        else
          readIFD(fs, subIFD, field.dataAt<uint32_t>(sc), bigtiff, swabflag);
        ifd.addSubIFD(subIFD);
      }
    }

    if (field.tag() == TIFFTAG_EXIFIFD) {
      ZTiffIFD exifIFD;
      if (bigtiff)
        readIFD(fs, exifIFD, field.dataAt<uint64_t>(0), bigtiff, swabflag);
      else
        readIFD(fs, exifIFD, field.dataAt<uint32_t>(0), bigtiff, swabflag);
      ifd.setExifIFD(exifIFD);
    }

    if (field.tag() == TIFFTAG_GPSIFD) {
      ZTiffIFD gpsIFD;
      if (bigtiff)
        readIFD(fs, gpsIFD, field.dataAt<uint64_t>(0), bigtiff, swabflag);
      else
        readIFD(fs, gpsIFD, field.dataAt<uint32_t>(0), bigtiff, swabflag);
      ifd.setGpsIFD(gpsIFD);
    }

    if (field.tag() == TIFFTAG_INTEROPERABILITYIFD) {
      ZTiffIFD interIFD;
      if (bigtiff)
        readIFD(fs, interIFD, field.dataAt<uint64_t>(0), bigtiff, swabflag);
      else
        readIFD(fs, interIFD, field.dataAt<uint32_t>(0), bigtiff, swabflag);
      ifd.setInteroperabilityIFD(interIFD);
    }
  }
  ifd.setOffset(off);
  ifd.setNextIFDOffset(nextdiroff);
  return nextdiroff;
}

QString ZTiff::tagToName(uint32_t tag) const
{
  for (size_t i = 0; i < std::extent<decltype(tiftagnames)>::value; ++i) {
    if (tiftagnames[i].tag == tag) {
      return tiftagnames[i].name;
    }
  }
  for (size_t i = 0; i < std::extent<decltype(exiftagnames)>::value; ++i) {
    if (exiftagnames[i].tag == tag) {
      return exiftagnames[i].name;
    }
  }
  return "Unknown tag";
}

void ZTiff::readIFDs(const QString& filename, std::vector<ZTiffIFD>& ifds) const
{
  std::ifstream fs;
  openFileStream(fs, filename, std::ios_base::binary | std::ios_base::in);
  readIFDs(fs, ifds);
}

void ZTiff::readIFDs(std::istream& fs, std::vector<ZTiffIFD>& ifds) const
{
  std::vector<ZTiffIFD> _ifds;
  readStream(fs, &hdr, sizeof(TIFFHeaderCommon));

  if (hdr.common.tiff_magic != TIFF_BIGENDIAN
      && hdr.common.tiff_magic != TIFF_LITTLEENDIAN &&
      hdr.common.tiff_magic != (hostIsLittleEndian() ? MDI_LITTLEENDIAN : MDI_BIGENDIAN)) {
    throw ZIOException(QString("Not a TIFF or MDI file, bad magic number %1")
                         .arg(hdr.common.tiff_magic, 0, 16));
  }

  bool swabflag;
  if (hdr.common.tiff_magic == TIFF_BIGENDIAN || hdr.common.tiff_magic == MDI_BIGENDIAN)
    swabflag = hostIsLittleEndian();
  else
    swabflag = !hostIsLittleEndian();
  if (swabflag)
    boost::endian::endian_reverse_inplace(hdr.common.tiff_version);

  bool bigtiff = false;
  uint64_t diroff = 0;
  if (hdr.common.tiff_version == 42) {
    readStream(fs, &hdr.classic.tiff_diroff, 4);
    if (swabflag)
      boost::endian::endian_reverse_inplace(hdr.classic.tiff_diroff);
    //    printf("Magic: %#x <%s-endian> Version: %#x <%s>\n",
    //           hdr.classic.tiff_magic,
    //           hdr.classic.tiff_magic == TIFF_BIGENDIAN ? "big" : "little",
    //           42,"ClassicTIFF");
    if (diroff == 0)
      diroff = hdr.classic.tiff_diroff;
  } else if (hdr.common.tiff_version == 43) {
    readStream(fs, &hdr.big.tiff_offsetsize, 12);
    if (swabflag) {
      boost::endian::endian_reverse_inplace(hdr.big.tiff_offsetsize);
      boost::endian::endian_reverse_inplace(hdr.big.tiff_unused);
      // endian_reverse_inplace has no overload for unsigned long
      hdr.big.tiff_diroff = boost::endian::endian_reverse(static_cast<uint64_t>(hdr.big.tiff_diroff));
    }
    //    printf("Magic: %#x <%s-endian> Version: %#x <%s>\n",
    //           hdr.big.tiff_magic,
    //           hdr.big.tiff_magic == TIFF_BIGENDIAN ? "big" : "little",
    //           43,"BigTIFF");
    //    printf("OffsetSize: %#x Unused: %#x\n",
    //           hdr.big.tiff_offsetsize,hdr.big.tiff_unused);
    if (diroff == 0)
      diroff = hdr.big.tiff_diroff;
    bigtiff = true;
  } else {
    throw ZIOException(QString("Not a TIFF file, bad version number %1")
                         .arg(hdr.common.tiff_version));
  }

  std::set<uint64_t> visitedDiroffs;
  while (diroff != 0) {
    if (visitedDiroffs.find(diroff) != visitedDiroffs.end()) {
      throw ZIOException(QString("Cycle detected in chaining of TIFF directories"));
    }
    visitedDiroffs.insert(diroff);
    _ifds.push_back(ZTiffIFD());
    diroff = readIFD(fs, _ifds[_ifds.size() - 1], diroff, bigtiff, swabflag);
    // workaround zeiss axio scanner exporter bug
    if (!_ifds[_ifds.size() - 1].containsTag(TIFFTAG_IMAGEWIDTH)) {
      _ifds.pop_back();
    }
  }
  _ifds.swap(ifds);
}

void ZTiff::readImg(ZImg& img, bool divideByAlpha)
{
  uint16_t planarConfig;
  TIFFGetFieldDefaulted(m_tif.get(), TIFFTAG_PLANARCONFIG, &planarConfig);
  bool separatePlane = PLANARCONFIG_SEPARATE == planarConfig;
  uint16_t photometric;
  if (TIFFGetField(m_tif.get(), TIFFTAG_PHOTOMETRIC, &photometric) != 1) {
    throw ZIOException("photometric is required field");
  }
  uint16_t sampleFormat;
  TIFFGetFieldDefaulted(m_tif.get(), TIFFTAG_SAMPLEFORMAT, &sampleFormat);
  if (sampleFormat == 5 || sampleFormat == 6) {
    throw ZIOException("tiff with complex sample is not supported");
  }
  if (sampleFormat < 1 || sampleFormat > 6) {
    throw ZIOException(QString("invalid sample format %1").arg(sampleFormat));
  }

  bool readAsRGBA = true;
  if (photometric == PHOTOMETRIC_MINISBLACK ||
      photometric == PHOTOMETRIC_MINISWHITE ||
      photometric == PHOTOMETRIC_RGB ||
      !m_useColormap) {
    readAsRGBA = false;
  }

  uint16_t orientation;
  TIFFGetFieldDefaulted(m_tif.get(), TIFFTAG_ORIENTATION, &orientation);

  // img info already consider the orientation, but to read the raw data, we need to change it back
  if (orientation > 4)
    std::swap(img.infoRef().height, img.infoRef().width);

  if (photometric == PHOTOMETRIC_MINISWHITE &&
      (sampleFormat == SAMPLEFORMAT_INT || sampleFormat == SAMPLEFORMAT_IEEEFP)) {
    throw ZIOException("Don't support PHOTOMETRIC_MINISWHITE for signed or double image.");
  }

  if (readAsRGBA) {

    ZImg bufImg(img.infoRef());
    uint32_t* raster = bufImg.channelData<uint32_t>(0);

    int ret = TIFFReadRGBAImageOriented(m_tif.get(), img.width(), img.height(), raster, orientation, 0);
    if (ret == 0)
      throw ZIOException("Read tiff as rgba failed");
    separateChannel(bufImg, img);

  } else {

    bool invertWhiteBlack = PHOTOMETRIC_MINISWHITE == photometric;

    if (TIFFIsTiled(m_tif.get())) {
      uint32_t tileWidth;
      uint32_t tileHeight;
      TIFFGetField(m_tif.get(), TIFFTAG_TILEWIDTH, &tileWidth);
      TIFFGetField(m_tif.get(), TIFFTAG_TILELENGTH, &tileHeight);
      size_t numTilePerRow = (img.width() + tileWidth - 1) / tileWidth;

      if (separatePlane || img.numChannels() == 1) {
        uint32_t tilesPerChannel = TIFFNumberOfTiles(m_tif.get()) / img.numChannels();

        std::vector<uint8_t, boost::alignment::aligned_allocator<uint8_t, 32>> tileBuf(
          tileWidth * tileHeight * img.voxelByteNumber());

        for (size_t c = 0; c < img.numChannels(); ++c) {
          for (uint32_t tile = c * tilesPerChannel; tile < (c + 1) * tilesPerChannel; tile++) {
            size_t tileRow = (tile - c * tilesPerChannel) / numTilePerRow;
            size_t tileCol = (tile - c * tilesPerChannel) % numTilePerRow;
            size_t x = tileCol * tileWidth;
            size_t y = tileRow * tileHeight;
            readTile(tile, tileBuf.data(), tileWidth, tileHeight, 1, invertWhiteBlack);
            copyOneChannelTileToImg(tileBuf.data(), tileWidth, tileHeight, img.voxelByteNumber(), img, x, y, c);
          }
        }
      } else {
        std::vector<uint8_t, boost::alignment::aligned_allocator<uint8_t, 32>> tileBuf(
          tileWidth * tileHeight * img.voxelByteNumber() * img.numChannels());

        for (uint32_t tile = 0; tile < TIFFNumberOfTiles(m_tif.get()); tile++) {
          size_t tileRow = (tile) / numTilePerRow;
          size_t tileCol = (tile) % numTilePerRow;
          size_t x = tileCol * tileWidth;
          size_t y = tileRow * tileHeight;
          readTile(tile, tileBuf.data(), tileWidth, tileHeight, img.numChannels(), invertWhiteBlack);
          copyTileToImg(tileBuf.data(), tileWidth, tileHeight, img.numChannels(), img.voxelByteNumber(), img, x, y);
        }
      }

    } else {
      if (separatePlane || img.numChannels() == 1) {
        uint32_t stripsPerChannel = TIFFNumberOfStrips(m_tif.get()) / img.numChannels();

        uint32_t numRowsPerStrip;
        // works only in little endian since TIFFTAG_ROWSPERSTRIP can be short
        TIFFGetFieldDefaulted(m_tif.get(), TIFFTAG_ROWSPERSTRIP, &numRowsPerStrip);
        numRowsPerStrip = std::min(static_cast<uint32_t>(img.height()), numRowsPerStrip);
        uint32_t numRowsOfLastStrip = img.height() % numRowsPerStrip;
        if (numRowsOfLastStrip == 0)
          numRowsOfLastStrip = numRowsPerStrip;

        for (size_t c = 0; c < img.numChannels(); ++c) {
          uint8_t* buf = img.channelData<uint8_t>(c);

          size_t off = 0;
          for (uint32_t strip = c * stripsPerChannel; strip < (c + 1) * stripsPerChannel; strip++) {
            if (strip == (c + 1) * stripsPerChannel - 1)
              off += readStrip(strip, buf + off, img.width(), numRowsOfLastStrip, 1, invertWhiteBlack);
            else
              off += readStrip(strip, buf + off, img.width(), numRowsPerStrip, 1, invertWhiteBlack);
          }
          if (off != img.channelByteNumber())
            throw ZIOException(QString("read(%1):expected(%2)").arg(off).arg(img.channelByteNumber()));
        }
      } else {
        ZImg bufImg(img.infoRef());
        uint8_t* buf = bufImg.channelData<uint8_t>(0);

        uint32_t numRowsPerStrip;
        TIFFGetFieldDefaulted(m_tif.get(), TIFFTAG_ROWSPERSTRIP, &numRowsPerStrip);
        numRowsPerStrip = std::min(static_cast<uint32_t>(img.height()), numRowsPerStrip);
        uint32_t numRowsOfLastStrip = img.height() % numRowsPerStrip;
        if (numRowsOfLastStrip == 0)
          numRowsOfLastStrip = numRowsPerStrip;

        size_t off = 0;
        for (uint32_t strip = 0; strip < TIFFNumberOfStrips(m_tif.get()); strip++) {
          if (strip == TIFFNumberOfStrips(m_tif.get()) - 1)
            off += readStrip(strip, buf + off, img.width(), numRowsOfLastStrip, img.numChannels(), invertWhiteBlack);
          else
            off += readStrip(strip, buf + off, img.width(), numRowsPerStrip, img.numChannels(), invertWhiteBlack);
        }
        if (off != img.timeByteNumber())
          throw ZIOException(QString("read(%1):expected(%2)").arg(off).arg(img.timeByteNumber()));

        separateChannel(bufImg, img);
      }
    }

    if (divideByAlpha) {
      img.correctPreMultipliedColor();
    }
  }

  // correct orientation
  if (orientation > 4)
    std::swap(img.infoRef().height, img.infoRef().width);

  switch (orientation) {
    case ORIENTATION_TOPLEFT:
      break;
    case ORIENTATION_TOPRIGHT:
      for (size_t i = 0; i < img.numChannels(); ++i) {
        image2DFlip(img.channelData<uint8_t>(i), img.width(), img.height(), Dimension::X);
      }
      break;
    case ORIENTATION_BOTRIGHT:
      for (size_t i = 0; i < img.numChannels(); ++i) {
        image2DReflect(img.channelData<uint8_t>(i), img.width(), img.height());
      }
      break;
    case ORIENTATION_BOTLEFT:
      for (size_t i = 0; i < img.numChannels(); ++i) {
        image2DFlip(img.channelData<uint8_t>(i), img.width(), img.height(), Dimension::Y);
      }
      break;
    case ORIENTATION_LEFTTOP:
      for (size_t i = 0; i < img.numChannels(); ++i) {
        image2DTranspose(img.channelData<uint8_t>(i), img.height(), img.width());
      }
      break;
    case ORIENTATION_RIGHTTOP:
      for (size_t i = 0; i < img.numChannels(); ++i) {
        image2DTranspose(img.channelData<uint8_t>(i), img.height(), img.width());
        image2DFlip(img.channelData<uint8_t>(i), img.width(), img.height(), Dimension::X);
      }
      break;
    case ORIENTATION_RIGHTBOT:
      for (size_t i = 0; i < img.numChannels(); ++i) {
        image2DTranspose(img.channelData<uint8_t>(i), img.height(), img.width());
        image2DReflect(img.channelData<uint8_t>(i), img.width(), img.height());
      }
      break;
    case ORIENTATION_LEFTBOT:
      for (size_t i = 0; i < img.numChannels(); ++i) {
        image2DTranspose(img.channelData<uint8_t>(i), img.height(), img.width());
        image2DFlip(img.channelData<uint8_t>(i), img.width(), img.height(), Dimension::Y);
      }
      break;
    default:
      break;
  }
}

size_t ZTiff::readStrip(uint32_t strip, uint8_t* buf, size_t width, size_t height, size_t nChannel, bool invert)
{
  uint16 bitspersample;
  TIFFGetFieldDefaulted(m_tif.get(), TIFFTAG_BITSPERSAMPLE, &bitspersample);
  if (bitspersample % 8 == 0) {
    size_t read = TIFFReadEncodedStrip(m_tif.get(), strip, buf, static_cast<tmsize_t>(-1));
    if (invert) {
      switch (bitspersample / 8) {
        case 1: {
          uint8_t* pt = buf;
          for (size_t i = 0; i < read; ++i)
            pt[i] = std::numeric_limits<uint8_t>::max() - pt[i];
        }
          break;
        case 2: {
          uint16_t* pt = bit_cast<uint16_t*>(buf);
          for (size_t i = 0; i < read; ++i)
            pt[i] = std::numeric_limits<uint16_t>::max() - pt[i];
        }
          break;
        case 4: {
          uint32_t* pt = bit_cast<uint32_t*>(buf);
          for (size_t i = 0; i < read; ++i)
            pt[i] = std::numeric_limits<uint32_t>::max() - pt[i];
        }
          break;
        case 8: {
          uint64_t* pt = bit_cast<uint64_t*>(buf);
          for (size_t i = 0; i < read; ++i)
            pt[i] = std::numeric_limits<uint64_t>::max() - pt[i];
        }
          break;
        default:
          throw ZIOException(QString("do not support invert %1 bytes integer").arg(bitspersample / 8));
      }
    }
    return read;
  } else if (bitspersample == 1) {
    std::vector<uint8_t> packedBuf(TIFFStripSize(m_tif.get()));
    TIFFReadEncodedStrip(m_tif.get(), strip, packedBuf.data(), static_cast<tmsize_t>(-1));
    uint8_t* buf8 = buf;
    size_t bytesPerRow = (width * nChannel + 7) / 8;
    if (packedBuf.size() < bytesPerRow * height) {
      throw ZIOException(QString("Not enought strip data, nRows:%1, nChannel:%2, bitsPerSample:%3, strip data:%4")
                           .arg(height).arg(nChannel).arg(bitspersample).arg(packedBuf.size()));
    }
    for (size_t r = 0; r < height; ++r) {
      for (size_t i = 0; i < width * nChannel; ++i) {
        *buf8 = invert ? ((packedBuf[r * bytesPerRow + i / 8] & bitmasks1[i % 8]) == 0) :
                ((packedBuf[r * bytesPerRow + i / 8] & bitmasks1[i % 8]) > 0);
        buf8++;
      }
    }
    return height * width * nChannel;
  } else if (bitspersample == 2) {
    std::vector<uint8_t> packedBuf(TIFFStripSize(m_tif.get()));
    TIFFReadEncodedStrip(m_tif.get(), strip, packedBuf.data(), static_cast<tmsize_t>(-1));
    uint8_t* buf8 = buf;
    size_t bytesPerRow = (width * nChannel + 3) / 4;
    if (packedBuf.size() < bytesPerRow * height) {
      throw ZIOException(QString("Not enought strip data, nRows:%1, nChannel:%2, bitsPerSample:%3, strip data:%4")
                           .arg(height).arg(nChannel).arg(bitspersample).arg(packedBuf.size()));
    }
    for (size_t r = 0; r < height; ++r) {
      for (size_t i = 0; i < width * nChannel; ++i) {
        *buf8 = invert ? (3 - ((packedBuf[r * bytesPerRow + i / 4] & bitmasks2[i % 4]) >> ((3 - i % 4) * 2))) :
                (packedBuf[r * bytesPerRow + i / 4] & bitmasks2[i % 4] >> ((3 - i % 4) * 2));
        buf8++;
      }
    }
    return height * width * nChannel;
  } else if (bitspersample == 4) {
    std::vector<uint8_t> packedBuf(TIFFStripSize(m_tif.get()));
    TIFFReadEncodedStrip(m_tif.get(), strip, packedBuf.data(), static_cast<tmsize_t>(-1));
    uint8_t* buf8 = buf;
    size_t bytesPerRow = (width * nChannel + 1) / 2;
    if (packedBuf.size() < bytesPerRow * height) {
      throw ZIOException(QString("Not enought strip data, nRows:%1, nChannel:%2, bitsPerSample:%3, strip data:%4")
                           .arg(height).arg(nChannel).arg(bitspersample).arg(packedBuf.size()));
    }
    for (size_t r = 0; r < height; ++r) {
      for (size_t i = 0; i < width * nChannel; ++i) {
        *buf8 = invert ? (15 - ((packedBuf[r * bytesPerRow + i / 2] & bitmasks4[i % 2]) >> ((1 - i % 2) * 4))) :
                (packedBuf[r * bytesPerRow + i / 2] & bitmasks4[i % 2] >> ((1 - i % 2) * 4));
        buf8++;
      }
    }
    return height * width * nChannel;
  } else {
    throw ZIOException("should not happen");
    return 0;
  }
}

void ZTiff::readTile(uint32_t tile, uint8_t* buf, size_t tileWidth, size_t tileHeight, size_t tileChannel, bool invert)
{
  uint16 bitspersample;
  TIFFGetFieldDefaulted(m_tif.get(), TIFFTAG_BITSPERSAMPLE, &bitspersample);
  if (bitspersample % 8 == 0) {
    size_t read = TIFFReadEncodedTile(m_tif.get(), tile, buf, static_cast<tmsize_t>(-1));
    if (invert) {
      switch (bitspersample / 8) {
        case 1: {
          uint8_t* pt = buf;
          for (size_t i = 0; i < read; ++i)
            pt[i] = std::numeric_limits<uint8_t>::max() - pt[i];
        }
          break;
        case 2: {
          uint16_t* pt = bit_cast<uint16_t*>(buf);
          for (size_t i = 0; i < read; ++i)
            pt[i] = std::numeric_limits<uint16_t>::max() - pt[i];
        }
          break;
        case 4: {
          uint32_t* pt = bit_cast<uint32_t*>(buf);
          for (size_t i = 0; i < read; ++i)
            pt[i] = std::numeric_limits<uint32_t>::max() - pt[i];
        }
          break;
        case 8: {
          uint64_t* pt = bit_cast<uint64_t*>(buf);
          for (size_t i = 0; i < read; ++i)
            pt[i] = std::numeric_limits<uint64_t>::max() - pt[i];
        }
          break;
        default:
          throw ZIOException(QString("do not support invert %1 bytes integer").arg(bitspersample / 8));
      }
    }
  } else if (bitspersample == 1) {
    std::vector<uint8_t> packedBuf(TIFFTileSize(m_tif.get()));
    TIFFReadEncodedTile(m_tif.get(), tile, packedBuf.data(), static_cast<tmsize_t>(-1));
    uint8_t* buf8 = buf;
    size_t bytesPerRow = packedBuf.size() / tileHeight;
    for (size_t r = 0; r < tileHeight; ++r) {
      for (size_t i = 0; i < tileWidth * tileChannel; ++i) {
        *buf8 = invert ? ((packedBuf[r * bytesPerRow + i / 8] & bitmasks1[i % 8]) == 0) :
                ((packedBuf[r * bytesPerRow + i / 8] & bitmasks1[i % 8]) > 0);
        buf8++;
      }
    }
  } else if (bitspersample == 2) {
    std::vector<uint8_t> packedBuf(TIFFTileSize(m_tif.get()));
    TIFFReadEncodedTile(m_tif.get(), tile, packedBuf.data(), static_cast<tmsize_t>(-1));
    uint8_t* buf8 = buf;
    size_t bytesPerRow = packedBuf.size() / tileHeight;
    for (size_t r = 0; r < tileHeight; ++r) {
      for (size_t i = 0; i < tileWidth * tileChannel; ++i) {
        *buf8 = invert ? (3 - ((packedBuf[r * bytesPerRow + i / 4] & bitmasks2[i % 4]) >> ((3 - i % 4) * 2))) :
                (packedBuf[r * bytesPerRow + i / 4] & bitmasks2[i % 4] >> ((3 - i % 4) * 2));
        buf8++;
      }
    }
  } else if (bitspersample == 4) {
    std::vector<uint8_t> packedBuf(TIFFTileSize(m_tif.get()));
    TIFFReadEncodedTile(m_tif.get(), tile, packedBuf.data(), static_cast<tmsize_t>(-1));
    uint8_t* buf8 = buf;
    size_t bytesPerRow = packedBuf.size() / tileHeight;
    for (size_t r = 0; r < tileHeight; ++r) {
      for (size_t i = 0; i < tileWidth * tileChannel; ++i) {
        *buf8 = invert ? (15 - ((packedBuf[r * bytesPerRow + i / 2] & bitmasks4[i % 2]) >> ((1 - i % 2) * 4))) :
                (packedBuf[r * bytesPerRow + i / 2] & bitmasks4[i % 2] >> ((1 - i % 2) * 4));
        buf8++;
      }
    }
  }
}

void ZTiff::separateChannel(const ZImg& bufImg, ZImg& img)
{
  for (size_t c = 0; c < img.numChannels(); ++c) {
    switch (img.voxelByteNumber()) {
      case 1: {
        uint8_t* des = img.channelData<uint8_t>(c);
        const uint8_t* src = bufImg.channelData<uint8_t>(0) + c;
        size_t numCh = img.numChannels();
        size_t i = 0;
        while (i++ < img.channelVoxelNumber()) {
          *des++ = *src;
          src += numCh;
        }
      }
        break;
      case 2: {
        uint16_t* des = img.channelData<uint16_t>(c);
        const uint16_t* src = bufImg.channelData<uint16_t>(0) + c;
        size_t numCh = img.numChannels();
        size_t i = 0;
        while (i++ < img.channelVoxelNumber()) {
          *des++ = *src;
          src += numCh;
        }
      }
        break;
      default: {
        uint8_t* des = img.channelData<uint8_t>(c);
        const uint8_t* src = bufImg.channelData<uint8_t>(0) + c * img.voxelByteNumber();
        size_t voxelByte = img.voxelByteNumber();
        size_t srcStride = img.numChannels() * voxelByte;
        size_t i = 0;
        while (i++ < img.channelVoxelNumber()) {
          memcpy(des, src, voxelByte);
          des += voxelByte;
          src += srcStride;
        }
      }
    }
  }
}

void ZTiff::copyOneChannelTileToImg(const uint8_t* tileBuf, size_t tileWidth, size_t tileHeight, size_t voxelByteNumber,
                                    ZImg& img, size_t xStart, size_t yStart, size_t c)
{
  size_t xEnd = std::min(xStart + tileWidth, img.width());
  size_t cpysize = (xEnd - xStart) * voxelByteNumber;
  size_t yEnd = std::min(yStart + tileHeight, img.height());
  for (size_t y = yStart; y < yEnd; ++y) {
    memcpy(img.data<uint8_t>(xStart, y, 0, c), tileBuf + (y - yStart) * tileWidth * voxelByteNumber, cpysize);
  }
}

void ZTiff::copyTileToImg(const uint8_t* tileBuf, size_t tileWidth, size_t tileHeight, size_t numChannels,
                          size_t voxelByteNumber,
                          ZImg& img, size_t xStart, size_t yStart)
{
  for (size_t c = 0; c < numChannels; ++c) {
    size_t xEnd = std::min(xStart + tileWidth, img.width());
    size_t yEnd = std::min(yStart + tileHeight, img.height());
    for (size_t x = xStart; x < xEnd; ++x) {
      for (size_t y = yStart; y < yEnd; ++y) {
        uint8_t* des = img.data<uint8_t>(x, y, 0, c);
        const uint8_t* src = tileBuf + (y - yStart) * tileWidth * voxelByteNumber * numChannels +
                             (x - xStart) * voxelByteNumber * numChannels + c * voxelByteNumber;
        memcpy(des, src, voxelByteNumber);
      }
    }
  }
}

ZTiffWriter::ZTiffWriter()
  : m_tif(nullptr, TIFFClose)
{
}

void ZTiffWriter::startWriting(const QString& filename, Compression comp, int extraSample, bool bigTiff)
{
  m_tif.reset();

  if (comp == Compression::AUTO) {
    comp = defaultCompression(nullptr);
  } else {
    if (!checkCompression(nullptr, comp)) {
      LOG(WARNING) << QString("Compression %1 is not supported or not applicable, switch to default compression.").arg(
        enumToUnderlyingType(comp));
      comp = defaultCompression(nullptr);
    }
  }

  m_compression = comp;
  m_extraSample = extraSample;

  TIFFSetWarningHandler(0);
  TIFFSetErrorHandler(LibtiffErrorHandler);
#if defined(_WIN32) || defined(_WIN64)
  if (bigTiff)
    m_tif.reset(TIFFOpenW(filename.toStdWString().c_str(), "w8"));
  else
    m_tif.reset(TIFFOpenW(filename.toStdWString().c_str(), "w"));
#else
  if (bigTiff)
    m_tif.reset(TIFFOpen(QFile::encodeName(filename).constData(), "w8"));
  else
    m_tif.reset(TIFFOpen(QFile::encodeName(filename).constData(), "w"));
#endif
  if (!m_tif)
    throw ZIOException(QString("Can't open ") % filename % QString(" for writing"));
}

void ZTiffWriter::writeIFD(const ZImg& img, int z, int t, int c, bool writeThumbnails,
                           const std::vector<ZImgMetatag>& additionalTags)
{
  CHECK(m_tif);
  for (size_t i = 0; i < additionalTags.size(); ++i) {
    const ZImgMetatag& atag = additionalTags[i];
    if (atag.tag() > 0 && atag.tag() < 65535)
      TIFFSetField(m_tif.get(), atag.tag(), atag.dataArray());
  }

  uint16_t photo = PHOTOMETRIC_MINISBLACK;
  TIFFSetField(m_tif.get(), TIFFTAG_IMAGEWIDTH, img.width());
  TIFFSetField(m_tif.get(), TIFFTAG_IMAGELENGTH, img.height());
  TIFFSetField(m_tif.get(), TIFFTAG_BITSPERSAMPLE, img.voxelByteNumber() * 8);
  if (c < 0) {
    TIFFSetField(m_tif.get(), TIFFTAG_SAMPLESPERPIXEL, img.numChannels());
    if (img.numChannels() > 1) {
      photo = PHOTOMETRIC_RGB;
    }
  } else {
    TIFFSetField(m_tif.get(), TIFFTAG_SAMPLESPERPIXEL, 1);
  }
  TIFFSetField(m_tif.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_SEPARATE);
  TIFFSetField(m_tif.get(), TIFFTAG_PHOTOMETRIC, photo);
  TIFFSetField(m_tif.get(), TIFFTAG_COMPRESSION, m_compression);
  if (m_compression != Compression::NONE) {
    if (img.voxelFormat() == VoxelFormat::Float) {
      TIFFSetField(m_tif.get(), TIFFTAG_PREDICTOR, PREDICTOR_FLOATINGPOINT);
    } else if (img.voxelByteNumber() <= 4 && img.voxelByteNumber() > 1) {
      TIFFSetField(m_tif.get(), TIFFTAG_PREDICTOR, PREDICTOR_HORIZONTAL);
    }
  }
  if (m_extraSample >= 0 && m_extraSample <= 2) {
    uint16_t extraSample = m_extraSample;
    TIFFSetField(m_tif.get(), TIFFTAG_EXTRASAMPLES, 1, &extraSample);
  }
  TIFFSetField(m_tif.get(), TIFFTAG_ROWSPERSTRIP, img.height());
  TIFFSetField(m_tif.get(), TIFFTAG_SAMPLEFORMAT, img.voxelFormat());
  std::vector<ZImg> emptyList;
  const std::vector<ZImg>* thumbnails = &emptyList;
  if (writeThumbnails) {
    thumbnails = &(img.thumbnail().planeAttachments(z, t));
    if (thumbnails->size() > 0) {
      std::vector<toff_t> sub_IFDs_offsets(thumbnails->size());
      TIFFSetField(m_tif.get(), TIFFTAG_SUBIFD, thumbnails->size(), sub_IFDs_offsets.data());
    }
  }

  if (c < 0) {
    for (size_t ch = 0; ch < img.numChannels(); ++ch)
      TIFFWriteEncodedStrip(m_tif.get(), ch, const_cast<uint8_t*>(img.planeData(z, ch, t)), img.planeByteNumber());
  } else {
    TIFFWriteEncodedStrip(m_tif.get(), 0, const_cast<uint8_t*>(img.planeData(z, c, t)), img.planeByteNumber());
  }

  TIFFWriteDirectory(m_tif.get());

  for (size_t thumb = 0; thumb < thumbnails->size(); ++thumb) {
    TIFFSetField(m_tif.get(), TIFFTAG_SUBFILETYPE, FILETYPE_REDUCEDIMAGE);
    TIFFSetField(m_tif.get(), TIFFTAG_IMAGEWIDTH, (*thumbnails)[thumb].width());
    TIFFSetField(m_tif.get(), TIFFTAG_IMAGELENGTH, (*thumbnails)[thumb].height());
    TIFFSetField(m_tif.get(), TIFFTAG_BITSPERSAMPLE, (*thumbnails)[thumb].voxelByteNumber() * 8);
    TIFFSetField(m_tif.get(), TIFFTAG_SAMPLESPERPIXEL, (*thumbnails)[thumb].numChannels());
    TIFFSetField(m_tif.get(), TIFFTAG_COMPRESSION, m_compression);
    TIFFSetField(m_tif.get(), TIFFTAG_PHOTOMETRIC, photo);
    TIFFSetField(m_tif.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_SEPARATE);
    TIFFSetField(m_tif.get(), TIFFTAG_ROWSPERSTRIP, (*thumbnails)[thumb].height());
    TIFFSetField(m_tif.get(), TIFFTAG_SAMPLEFORMAT, (*thumbnails)[thumb].voxelFormat());
    for (size_t ch = 0; ch < (*thumbnails)[thumb].numChannels(); ++ch)
      TIFFWriteEncodedStrip(m_tif.get(), ch, const_cast<uint8_t*>((*thumbnails)[thumb].planeData(0, ch, 0)),
                            (*thumbnails)[thumb].planeByteNumber());
    TIFFWriteDirectory(m_tif.get());
  }
}

Compression ZTiffWriter::defaultCompression(const ZImg* img)
{
  const Compression list[] = {
    Compression::LZW, Compression::ADOBE_DEFLATE, Compression::PACKBITS
  };
  for (size_t i = 0; i < std::extent<decltype(list)>::value; ++i)
    if (checkCompression(img, list[i]))
      return list[i];
  return Compression::NONE;
}

bool ZTiffWriter::checkCompression(const ZImg*, Compression comp)
{
  if (comp == Compression::NONE)
    return true;
  //check exist first
  if (TIFFIsCODECConfigured(enumToUnderlyingType(comp)) != 1)
    return false;
  //  if (comp == Compression::CCITTFAX3 ||
  //      comp == Compression::CCITTFAX4 ||
  //      comp == Compression::CCITTRLE ||
  //      comp == Compression::CCITTRLEW ||
  //      comp == Compression::CCITT_T4 ||
  //      comp == Compression::CCITT_T6)
  //    return false;
  return true;
}

} // namespace
