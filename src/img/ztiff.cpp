#include "ztiff.h"

#include "zimgformat.h"
#include "zlog.h"
#include "zimage2dutils.h"
#include "zioutils.h"
#include "zstructutils.h"
#include "zstringutils.h"

#include <tiff.h>
#include <tiffio.h>
#include <tiffio.hxx>
#include <QFile>
#include <cmath>
#include <limits>
#include <set>

namespace {

// Carl Zeiss LSM
#define TIFFTAG_CZ_LSMINFO 34412

/* tags 33550 is a private tag registered to SoftDesk, Inc */
#define TIFFTAG_GEOPIXELSCALE 33550
/* tags 33920-33921 are private tags registered to Intergraph, Inc */
#define TIFFTAG_INTERGRAPH_MATRIX 33920 /* $use TIFFTAG_GEOTRANSMATRIX ! */
#define TIFFTAG_GEOTIEPOINTS 33922
/* tags 34263-34264 are private tags registered to NASA-JPL Carto Group */
#ifdef JPL_TAG_SUPPORT
#define TIFFTAG_JPL_CARTO_IFD 34263 /* $use GeoProjectionInfo ! */
#endif
#define TIFFTAG_GEOTRANSMATRIX 34264 /* New Matrix Tag replaces 33920 */
/* tags 34735-3438 are private tags registered to SPOT Image, Inc */
#define TIFFTAG_GEOKEYDIRECTORY 34735
#define TIFFTAG_GEODOUBLEPARAMS 34736
#define TIFFTAG_GEOASCIIPARAMS 34737

/*
 *  Define Printing method flags. These
 *  flags may be passed in to TIFFPrintDirectory() to
 *  indicate that those particular field values should
 *  be printed out in full, rather than just an indicator
 *  of whether they are present or not.
 */
#define TIFFPRINT_GEOKEYDIRECTORY 0x80000000
#define TIFFPRINT_GEOKEYPARAMS 0x40000000

// Atlas reads TIFF tags itself and uses the original strip layout for region
// reads, so file-backed libtiff handles must not expose chopped virtual strips.
constexpr const char* kTiffReadMode = "rc";

const struct tiftagname
{
  uint32_t tag;
  const char* name;
} tiftagnames[] = {
  {TIFFTAG_SUBFILETYPE,                "SubfileType"                    },
  {TIFFTAG_OSUBFILETYPE,               "OldSubfileType"                 },
  {TIFFTAG_IMAGEWIDTH,                 "ImageWidth"                     },
  {TIFFTAG_IMAGELENGTH,                "ImageLength"                    },
  {TIFFTAG_BITSPERSAMPLE,              "BitsPerSample"                  },
  {TIFFTAG_COMPRESSION,                "Compression"                    },
  {TIFFTAG_PHOTOMETRIC,                "PhotometricInterpretation"      },
  {TIFFTAG_THRESHHOLDING,              "Threshholding"                  },
  {TIFFTAG_CELLWIDTH,                  "CellWidth"                      },
  {TIFFTAG_CELLLENGTH,                 "CellLength"                     },
  {TIFFTAG_FILLORDER,                  "FillOrder"                      },
  {TIFFTAG_DOCUMENTNAME,               "DocumentName"                   },
  {TIFFTAG_IMAGEDESCRIPTION,           "ImageDescription"               },
  {TIFFTAG_MAKE,                       "Make"                           },
  {TIFFTAG_MODEL,                      "Model"                          },
  {TIFFTAG_STRIPOFFSETS,               "StripOffsets"                   },
  {TIFFTAG_ORIENTATION,                "Orientation"                    },
  {TIFFTAG_SAMPLESPERPIXEL,            "SamplesPerPixel"                },
  {TIFFTAG_ROWSPERSTRIP,               "RowsPerStrip"                   },
  {TIFFTAG_STRIPBYTECOUNTS,            "StripByteCounts"                },
  {TIFFTAG_MINSAMPLEVALUE,             "MinSampleValue"                 },
  {TIFFTAG_MAXSAMPLEVALUE,             "MaxSampleValue"                 },
  {TIFFTAG_XRESOLUTION,                "XResolution"                    },
  {TIFFTAG_YRESOLUTION,                "YResolution"                    },
  {TIFFTAG_PLANARCONFIG,               "PlanarConfiguration"            },
  {TIFFTAG_PAGENAME,                   "PageName"                       },
  {TIFFTAG_XPOSITION,                  "XPosition"                      },
  {TIFFTAG_YPOSITION,                  "YPosition"                      },
  {TIFFTAG_FREEOFFSETS,                "FreeOffsets"                    },
  {TIFFTAG_FREEBYTECOUNTS,             "FreeByteCounts"                 },
  {TIFFTAG_GRAYRESPONSEUNIT,           "GrayResponseUnit"               },
  {TIFFTAG_GRAYRESPONSECURVE,          "GrayResponseCurve"              },
  {TIFFTAG_RESOLUTIONUNIT,             "ResolutionUnit"                 },
  {TIFFTAG_PAGENUMBER,                 "PageNumber"                     },
  {TIFFTAG_COLORRESPONSEUNIT,          "ColorResponseUnit"              },
  {TIFFTAG_TRANSFERFUNCTION,           "TransferFunction"               },
  {TIFFTAG_SOFTWARE,                   "Software"                       },
  {TIFFTAG_DATETIME,                   "DateTime"                       },
  {TIFFTAG_ARTIST,                     "Artist"                         },
  {TIFFTAG_HOSTCOMPUTER,               "HostComputer"                   },
  {TIFFTAG_WHITEPOINT,                 "WhitePoint"                     },
  {TIFFTAG_PRIMARYCHROMATICITIES,      "PrimaryChromaticities"          },
  {TIFFTAG_COLORMAP,                   "ColorMap"                       },
  {TIFFTAG_HALFTONEHINTS,              "HalftoneHints"                  },
  {TIFFTAG_TILEWIDTH,                  "TileWidth"                      },
  {TIFFTAG_TILELENGTH,                 "TileLength"                     },
  {TIFFTAG_TILEOFFSETS,                "TileOffsets"                    },
  {TIFFTAG_TILEBYTECOUNTS,             "TileByteCounts"                 },
  {TIFFTAG_SUBIFD,                     "SubIFD"                         },
  {TIFFTAG_INKSET,                     "InkSet"                         },
  {TIFFTAG_INKNAMES,                   "InkNames"                       },
  {TIFFTAG_NUMBEROFINKS,               "NumberOfInks"                   },
  {TIFFTAG_DOTRANGE,                   "DotRange"                       },
  {TIFFTAG_TARGETPRINTER,              "TargetPrinter"                  },
  {TIFFTAG_EXTRASAMPLES,               "ExtraSamples"                   },
  {TIFFTAG_SAMPLEFORMAT,               "SampleFormat"                   },
  {TIFFTAG_SMINSAMPLEVALUE,            "SMinSampleValue"                },
  {TIFFTAG_SMAXSAMPLEVALUE,            "SMaxSampleValue"                },
  {TIFFTAG_CLIPPATH,                   "ClipPath"                       },
  {TIFFTAG_XCLIPPATHUNITS,             "XClipPathUnits"                 },
  {TIFFTAG_XCLIPPATHUNITS,             "XClipPathUnits"                 },
  {TIFFTAG_YCLIPPATHUNITS,             "YClipPathUnits"                 },
  {TIFFTAG_YCBCRCOEFFICIENTS,          "YCbCrCoefficients"              },
  {TIFFTAG_YCBCRSUBSAMPLING,           "YCbCrSubsampling"               },
  {TIFFTAG_YCBCRPOSITIONING,           "YCbCrPositioning"               },
  {TIFFTAG_REFERENCEBLACKWHITE,        "ReferenceBlackWhite"            },
  {TIFFTAG_XMLPACKET,                  "XMLPacket"                      },
  /* begin SGI tags */
  {TIFFTAG_MATTEING,                   "Matteing"                       },
  {TIFFTAG_DATATYPE,                   "DataType"                       },
  {TIFFTAG_IMAGEDEPTH,                 "ImageDepth"                     },
  {TIFFTAG_TILEDEPTH,                  "TileDepth"                      },
  /* end SGI tags */
  /* begin Pixar tags */
  {TIFFTAG_PIXAR_IMAGEFULLWIDTH,       "ImageFullWidth"                 },
  {TIFFTAG_PIXAR_IMAGEFULLLENGTH,      "ImageFullLength"                },
  {TIFFTAG_PIXAR_TEXTUREFORMAT,        "TextureFormat"                  },
  {TIFFTAG_PIXAR_WRAPMODES,            "TextureWrapModes"               },
  {TIFFTAG_PIXAR_FOVCOT,               "FieldOfViewCotangent"           },
  {TIFFTAG_PIXAR_MATRIX_WORLDTOSCREEN, "MatrixWorldToScreen"            },
  {TIFFTAG_PIXAR_MATRIX_WORLDTOCAMERA, "MatrixWorldToCamera"            },
  {TIFFTAG_COPYRIGHT,                  "Copyright"                      },
  /* end Pixar tags */
  {TIFFTAG_RICHTIFFIPTC,               "RichTIFFIPTC"                   },
  {TIFFTAG_PHOTOSHOP,                  "Photoshop"                      },
  {TIFFTAG_EXIFIFD,                    "ExifIFD"                        },
  {TIFFTAG_ICCPROFILE,                 "ICC Profile"                    },
  {TIFFTAG_GPSIFD,                     "GPSIFDOffset"                   },
  {TIFFTAG_FAXRECVPARAMS,              "FaxRecvParams"                  },
  {TIFFTAG_FAXSUBADDRESS,              "FaxSubAddress"                  },
  {TIFFTAG_FAXRECVTIME,                "FaxRecvTime"                    },
  {TIFFTAG_FAXDCS,                     "FaxDcs"                         },
  {TIFFTAG_STONITS,                    "StoNits"                        },
  {TIFFTAG_INTEROPERABILITYIFD,        "InteroperabilityIFDOffset"      },
  /* begin DNG tags */
  {TIFFTAG_DNGVERSION,                 "DNGVersion"                     },
  {TIFFTAG_DNGBACKWARDVERSION,         "DNGBackwardVersion"             },
  {TIFFTAG_UNIQUECAMERAMODEL,          "UniqueCameraModel"              },
  {TIFFTAG_LOCALIZEDCAMERAMODEL,       "LocalizedCameraModel"           },
  {TIFFTAG_CFAPLANECOLOR,              "CFAPlaneColor"                  },
  {TIFFTAG_CFALAYOUT,                  "CFALayout"                      },
  {TIFFTAG_LINEARIZATIONTABLE,         "LinearizationTable"             },
  {TIFFTAG_BLACKLEVELREPEATDIM,        "BlackLevelRepeatDim"            },
  {TIFFTAG_BLACKLEVEL,                 "BlackLevel"                     },
  {TIFFTAG_BLACKLEVELDELTAH,           "BlackLevelDeltaH"               },
  {TIFFTAG_BLACKLEVELDELTAV,           "BlackLevelDeltaV"               },
  {TIFFTAG_WHITELEVEL,                 "WhiteLevel"                     },
  {TIFFTAG_DEFAULTSCALE,               "DefaultScale"                   },
  {TIFFTAG_BESTQUALITYSCALE,           "BestQualityScale"               },
  {TIFFTAG_DEFAULTCROPORIGIN,          "DefaultCropOrigin"              },
  {TIFFTAG_DEFAULTCROPSIZE,            "DefaultCropSize"                },
  {TIFFTAG_COLORMATRIX1,               "ColorMatrix1"                   },
  {TIFFTAG_COLORMATRIX2,               "ColorMatrix2"                   },
  {TIFFTAG_CAMERACALIBRATION1,         "CameraCalibration1"             },
  {TIFFTAG_CAMERACALIBRATION2,         "CameraCalibration2"             },
  {TIFFTAG_REDUCTIONMATRIX1,           "ReductionMatrix1"               },
  {TIFFTAG_REDUCTIONMATRIX2,           "ReductionMatrix2"               },
  {TIFFTAG_ANALOGBALANCE,              "AnalogBalance"                  },
  {TIFFTAG_ASSHOTNEUTRAL,              "AsShotNeutral"                  },
  {TIFFTAG_ASSHOTWHITEXY,              "AsShotWhiteXY"                  },
  {TIFFTAG_BASELINEEXPOSURE,           "BaselineExposure"               },
  {TIFFTAG_BASELINENOISE,              "BaselineNoise"                  },
  {TIFFTAG_BASELINESHARPNESS,          "BaselineSharpness"              },
  {TIFFTAG_BAYERGREENSPLIT,            "BayerGreenSplit"                },
  {TIFFTAG_LINEARRESPONSELIMIT,        "LinearResponseLimit"            },
  {TIFFTAG_CAMERASERIALNUMBER,         "CameraSerialNumber"             },
  {TIFFTAG_LENSINFO,                   "LensInfo"                       },
  {TIFFTAG_CHROMABLURRADIUS,           "ChromaBlurRadius"               },
  {TIFFTAG_ANTIALIASSTRENGTH,          "AntiAliasStrength"              },
  {TIFFTAG_SHADOWSCALE,                "ShadowScale"                    },
  {TIFFTAG_DNGPRIVATEDATA,             "DNGPrivateData"                 },
  {TIFFTAG_MAKERNOTESAFETY,            "MakerNoteSafety"                },
  {TIFFTAG_CALIBRATIONILLUMINANT1,     "CalibrationIlluminant1"         },
  {TIFFTAG_CALIBRATIONILLUMINANT2,     "CalibrationIlluminant2"         },
  {TIFFTAG_RAWDATAUNIQUEID,            "RawDataUniqueID"                },
  {TIFFTAG_ORIGINALRAWFILENAME,        "OriginalRawFileName"            },
  {TIFFTAG_ORIGINALRAWFILEDATA,        "OriginalRawFileData"            },
  {TIFFTAG_ACTIVEAREA,                 "ActiveArea"                     },
  {TIFFTAG_MASKEDAREAS,                "MaskedAreas"                    },
  {TIFFTAG_ASSHOTICCPROFILE,           "AsShotICCProfile"               },
  {TIFFTAG_ASSHOTPREPROFILEMATRIX,     "AsShotPreProfileMatrix"         },
  {TIFFTAG_CURRENTICCPROFILE,          "CurrentICCProfile"              },
  {TIFFTAG_CURRENTPREPROFILEMATRIX,    "CurrentPreProfileMatrix"        },
  {TIFFTAG_PERSAMPLE,                  "PerSample"                      },
  /* end DNG tags */
  /* begin TIFF/FX tags */
  {TIFFTAG_INDEXED,                    "Indexed"                        },
  {TIFFTAG_GLOBALPARAMETERSIFD,        "GlobalParametersIFD"            },
  {TIFFTAG_PROFILETYPE,                "ProfileType"                    },
  {TIFFTAG_FAXPROFILE,                 "FaxProfile"                     },
  {TIFFTAG_CODINGMETHODS,              "CodingMethods"                  },
  {TIFFTAG_VERSIONYEAR,                "VersionYear"                    },
  {TIFFTAG_MODENUMBER,                 "ModeNumber"                     },
  {TIFFTAG_DECODE,                     "Decode"                         },
  {TIFFTAG_IMAGEBASECOLOR,             "ImageBaseColor"                 },
  {TIFFTAG_T82OPTIONS,                 "T82Options"                     },
  {TIFFTAG_STRIPROWCOUNTS,             "StripRowCounts"                 },
  {TIFFTAG_IMAGELAYER,                 "ImageLayer"                     },
  /* end DNG tags */
  /* begin pseudo tags */
  // Carl Zeiss LSM
  {TIFFTAG_CZ_LSMINFO,                 "CarlZeissLSMInfo"               },

  // GEOTIFF
  {TIFFTAG_GEOPIXELSCALE,              "GeoPixelScale"                  },
  {TIFFTAG_INTERGRAPH_MATRIX,          "Intergraph TransformationMatrix"},
  {TIFFTAG_GEOTRANSMATRIX,             "GeoTransformationMatrix"        },
  {TIFFTAG_GEOTIEPOINTS,               "GeoTiePoints"                   },
  {TIFFTAG_GEOKEYDIRECTORY,            "GeoKeyDirectory"                },
  {TIFFTAG_GEODOUBLEPARAMS,            "GeoDoubleParams"                },
  {TIFFTAG_GEOASCIIPARAMS,             "GeoASCIIParams"                 },
  {TIFFTAG_PREDICTOR,                  "Predictor"                      },

  {TIFFTAG_JPEGIFOFFSET,               "JpegInterchangeFormat"          },
  {TIFFTAG_JPEGIFBYTECOUNT,            "JpegInterchangeFormatLength"    },
  {TIFFTAG_JPEGQTABLES,                "JpegQTables"                    },
  {TIFFTAG_JPEGDCTABLES,               "JpegDcTables"                   },
  {TIFFTAG_JPEGACTABLES,               "JpegAcTables"                   },
  {TIFFTAG_JPEGPROC,                   "JpegProc"                       },
  {TIFFTAG_JPEGRESTARTINTERVAL,        "JpegRestartInterval"            },
};

const struct exiftagname
{
  uint32_t tag;
  const char* name;
} exiftagnames[] = {
  {EXIFTAG_EXPOSURETIME,             "ExposureTime"                },
  {EXIFTAG_FNUMBER,                  "FNumber"                     },
  {EXIFTAG_EXPOSUREPROGRAM,          "ExposureProgram"             },
  {EXIFTAG_SPECTRALSENSITIVITY,      "SpectralSensitivity"         },
  {EXIFTAG_ISOSPEEDRATINGS,          "ISOSpeedRatings"             },
  {EXIFTAG_OECF,                     "OptoelectricConversionFactor"},
  {EXIFTAG_EXIFVERSION,              "ExifVersion"                 },
  {EXIFTAG_DATETIMEORIGINAL,         "DateTimeOriginal"            },
  {EXIFTAG_DATETIMEDIGITIZED,        "DateTimeDigitized"           },
  {EXIFTAG_COMPONENTSCONFIGURATION,  "ComponentsConfiguration"     },
  {EXIFTAG_COMPRESSEDBITSPERPIXEL,   "CompressedBitsPerPixel"      },
  {EXIFTAG_SHUTTERSPEEDVALUE,        "ShutterSpeedValue"           },
  {EXIFTAG_APERTUREVALUE,            "ApertureValue"               },
  {EXIFTAG_BRIGHTNESSVALUE,          "BrightnessValue"             },
  {EXIFTAG_EXPOSUREBIASVALUE,        "ExposureBiasValue"           },
  {EXIFTAG_MAXAPERTUREVALUE,         "MaxApertureValue"            },
  {EXIFTAG_SUBJECTDISTANCE,          "SubjectDistance"             },
  {EXIFTAG_METERINGMODE,             "MeteringMode"                },
  {EXIFTAG_LIGHTSOURCE,              "LightSource"                 },
  {EXIFTAG_FLASH,                    "Flash"                       },
  {EXIFTAG_FOCALLENGTH,              "FocalLength"                 },
  {EXIFTAG_SUBJECTAREA,              "SubjectArea"                 },
  {EXIFTAG_MAKERNOTE,                "MakerNote"                   },
  {EXIFTAG_USERCOMMENT,              "UserComment"                 },
  {EXIFTAG_SUBSECTIME,               "SubSecTime"                  },
  {EXIFTAG_SUBSECTIMEORIGINAL,       "SubSecTimeOriginal"          },
  {EXIFTAG_SUBSECTIMEDIGITIZED,      "SubSecTimeDigitized"         },
  {EXIFTAG_FLASHPIXVERSION,          "FlashpixVersion"             },
  {EXIFTAG_COLORSPACE,               "ColorSpace"                  },
  {EXIFTAG_PIXELXDIMENSION,          "PixelXDimension"             },
  {EXIFTAG_PIXELYDIMENSION,          "PixelYDimension"             },
  {EXIFTAG_RELATEDSOUNDFILE,         "RelatedSoundFile"            },
  {EXIFTAG_FLASHENERGY,              "FlashEnergy"                 },
  {EXIFTAG_SPATIALFREQUENCYRESPONSE, "SpatialFrequencyResponse"    },
  {EXIFTAG_FOCALPLANEXRESOLUTION,    "FocalPlaneXResolution"       },
  {EXIFTAG_FOCALPLANEYRESOLUTION,    "FocalPlaneYResolution"       },
  {EXIFTAG_FOCALPLANERESOLUTIONUNIT, "FocalPlaneResolutionUnit"    },
  {EXIFTAG_SUBJECTLOCATION,          "SubjectLocation"             },
  {EXIFTAG_EXPOSUREINDEX,            "ExposureIndex"               },
  {EXIFTAG_SENSINGMETHOD,            "SensingMethod"               },
  {EXIFTAG_FILESOURCE,               "FileSource"                  },
  {EXIFTAG_SCENETYPE,                "SceneType"                   },
  {EXIFTAG_CFAPATTERN,               "CFAPattern"                  },
  {EXIFTAG_CUSTOMRENDERED,           "CustomRendered"              },
  {EXIFTAG_EXPOSUREMODE,             "ExposureMode"                },
  {EXIFTAG_WHITEBALANCE,             "WhiteBalance"                },
  {EXIFTAG_DIGITALZOOMRATIO,         "DigitalZoomRatio"            },
  {EXIFTAG_FOCALLENGTHIN35MMFILM,    "FocalLengthIn35mmFilm"       },
  {EXIFTAG_SCENECAPTURETYPE,         "SceneCaptureType"            },
  {EXIFTAG_GAINCONTROL,              "GainControl"                 },
  {EXIFTAG_CONTRAST,                 "Contrast"                    },
  {EXIFTAG_SATURATION,               "Saturation"                  },
  {EXIFTAG_SHARPNESS,                "Sharpness"                   },
  {EXIFTAG_DEVICESETTINGDESCRIPTION, "DeviceSettingDescription"    },
  {EXIFTAG_SUBJECTDISTANCERANGE,     "SubjectDistanceRange"        },
  {EXIFTAG_IMAGEUNIQUEID,            "ImageUniqueID"               },
  {1,                                "GPSLatitudeRef"              },
  {2,                                "GPSLatitude"                 },
  {3,                                "GPSLongitudeRef"             },
  {4,                                "GPSLongitude"                },
};

void LibtiffErrorHandler(const char* /*module*/, const char* fmt, va_list ap)
{
  std::array<char, 512> buf{"libtiff: "};
  CHECK(buf[buf.size() - 1] == 0) << buf[buf.size() - 1];
  auto off = std::strlen(buf.data());
  CHECK(buf[off] == 0) << buf[off];
  std::vsnprintf(buf.data() + off, buf.size() - off, fmt, ap);
  throw nim::ZException(buf.data());
}

void LibtiffErrorHandlerIgnoreColormapError(const char* /*module*/, const char* fmt, va_list ap)
{
  std::array<char, 512> buf{"libtiff: "};
  CHECK(buf[buf.size() - 1] == 0) << buf[buf.size() - 1];
  auto off = std::strlen(buf.data());
  CHECK(buf[off] == 0) << buf[off];
  std::vsnprintf(buf.data() + off, buf.size() - off, fmt, ap);
  if (QString(buf.data()).contains("Colormap", Qt::CaseInsensitive)) {
    return;
  }
  throw nim::ZException(buf.data());
}

constexpr uint8_t bitmasks1[] = {0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01};

constexpr uint8_t bitmasks2[] = {0xC0, 0x30, 0x0C, 0x03};

constexpr uint8_t bitmasks4[] = {0xF0, 0x0F};

} // namespace

namespace nim {

uint16_t getTiffCompressionTag(Compression comp)
{
  static const std::unordered_map<Compression, std::uint16_t> compressionToTiffCompressionMap = {
    {Compression::NONE,          COMPRESSION_NONE         },
    {Compression::LZW,           COMPRESSION_LZW          },
    {Compression::JPEG,          COMPRESSION_JPEG         },
    {Compression::T85,           COMPRESSION_T85          },
    {Compression::T43,           COMPRESSION_T43          },
    {Compression::PACKBITS,      COMPRESSION_PACKBITS     },
    {Compression::DEFLATE,       COMPRESSION_DEFLATE      },
    {Compression::ADOBE_DEFLATE, COMPRESSION_ADOBE_DEFLATE},
    {Compression::DCS,           COMPRESSION_DCS          },
    {Compression::JP2000,        COMPRESSION_JP2000       },
    {Compression::LZMA,          COMPRESSION_LZMA         },
    {Compression::ZSTD,          COMPRESSION_ZSTD         },
    {Compression::WEBP,          COMPRESSION_WEBP         },
  };

  auto it = compressionToTiffCompressionMap.find(comp);
  if (it != compressionToTiffCompressionMap.end()) {
    return it->second;
  }
  throw ZException(fmt::format("invalid Compression for Tiff: {}", comp));
}

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
  auto i = indexOf(TIFFTAG_SAMPLEFORMAT);
  if (i != -1) {
    auto vfn = m_entries[i].dataAt<uint16_t>(sample);
    if (vfn == 4) { // void
      vfn = 1;
    } // unsigned
    if (vfn == 5 || vfn == 6) {
      throw ZException("complex TIFFTAG_SAMPLEFORMAT is not supported");
    }
    if (vfn < 1 || vfn > 6) {
      throw ZException(fmt::format("illegal TIFFTAG_SAMPLEFORMAT {}", vfn));
    }
    auto vf = static_cast<VoxelFormat>(vfn);
    return vf;
  }
  return VoxelFormat::Unsigned;
}

size_t ZTiffIFD::samplesPerPixel() const
{
  auto i = indexOf(TIFFTAG_SAMPLESPERPIXEL);
  if (i != -1) {
    return m_entries[i].dataAt<uint16_t>(0);
  }
  return 1;
}

size_t ZTiffIFD::bitsPerSample(size_t sample) const
{
  auto i = indexOf(TIFFTAG_BITSPERSAMPLE);
  if (i != -1) {
    return m_entries[i].dataAt<uint16_t>(sample);
  }
  return 1;
}

size_t ZTiffIFD::bitsPerSampleFromMaxSampleValue(size_t sample) const
{
  auto i = indexOf(TIFFTAG_MAXSAMPLEVALUE);
  if (i != -1) {
    return std::ceil(std::log2(m_entries[i].dataAt<uint16_t>(sample)));
  }
  return bitsPerSample(sample);
}

size_t ZTiffIFD::imageWidth() const
{
  auto i = indexOf(TIFFTAG_IMAGEWIDTH);
  if (i != -1) {
    if (m_entries[i].dataType() == DataType::Short) {
      return m_entries[i].dataAt<uint16_t>(0);
    } else {
      return m_entries[i].dataAt<uint32_t>(0);
    }
  } else {
    throw ZException("TIFFTAG_IMAGEWIDTH is required field of tiff.");
  }
  return 0;
}

size_t ZTiffIFD::imageHeight() const
{
  auto i = indexOf(TIFFTAG_IMAGELENGTH);
  if (i != -1) {
    if (m_entries[i].dataType() == DataType::Short) {
      return m_entries[i].dataAt<uint16_t>(0);
    } else {
      return m_entries[i].dataAt<uint32_t>(0);
    }
  } else {
    throw ZException("TIFFTAG_IMAGELENGTH is required field of tiff.");
  }
  return 0;
}

uint16_t ZTiffIFD::photometricInterpretation() const
{
  auto i = indexOf(TIFFTAG_PHOTOMETRIC);
  if (i != -1) {
    return m_entries[i].dataAt<uint16_t>(0);
  }

  throw ZException("TIFFTAG_PHOTOMETRIC is required field of tiff.");
  return 0;
}

std::string ZTiffIFD::imageDescription() const
{
  auto i = indexOf(TIFFTAG_IMAGEDESCRIPTION);
  if (i != -1) {
    return std::string(m_entries[i].dataArray<char>(), m_entries[i].count() - 1);
  }

  return {};
}

uint16_t ZTiffIFD::orientation() const
{
  auto i = indexOf(TIFFTAG_ORIENTATION);
  if (i != -1) {
    return m_entries[i].dataAt<uint16_t>(0);
  }

  return 1;
}

uint16_t ZTiffIFD::compression() const
{
  auto i = indexOf(TIFFTAG_COMPRESSION);
  if (i != -1) {
    return m_entries[i].dataAt<uint16_t>(0);
  }

  return 1;
}

uint16_t ZTiffIFD::planarConfiguration() const
{
  auto i = indexOf(TIFFTAG_PLANARCONFIG);
  if (i != -1) {
    return m_entries[i].dataAt<uint16_t>(0);
  }

  return 1;
}

int32_t ZTiffIFD::extraSample() const
{
  auto i = indexOf(TIFFTAG_EXTRASAMPLES);
  if (i != -1) {
    if (m_entries[i].count() > 1) {
      throw ZException("Tiff with multiple TIFFTAG_EXTRASAMPLES is not supported.");
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
  if (!isTiledImage()) {
    return (imageHeight() + rowsPerStrip() - 1) / rowsPerStrip();
  }
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
  auto i = indexOf(TIFFTAG_ROWSPERSTRIP);
  if (i != -1) {
    if (m_entries[i].dataType() == DataType::Short) {
      return m_entries[i].dataAt<uint16_t>(0);
    } else {
      return m_entries[i].dataAt<uint32_t>(0);
    }
  }
  return std::numeric_limits<uint32_t>::max();
}

uint64_t ZTiffIFD::stripOffsets(size_t idx) const
{
  auto i = indexOf(TIFFTAG_STRIPOFFSETS);
  if (i != -1) {
    if (idx >= m_entries[i].count()) {
      throw ZException(fmt::format("Wrong idx {} for strip offsets", idx));
    }
    if (m_entries[i].dataType() == DataType::Short) {
      return m_entries[i].dataAt<uint16_t>(idx);
    } else if (m_entries[i].dataType() == DataType::Long) {
      return m_entries[i].dataAt<uint32_t>(idx);
    } else {
      return m_entries[i].dataAt<uint64_t>(idx);
    }
  }
  return 0;
}

uint64_t ZTiffIFD::stripByteCounts(size_t idx) const
{
  auto i = indexOf(TIFFTAG_STRIPBYTECOUNTS);
  if (i != -1) {
    if (idx >= m_entries[i].count()) {
      throw ZException(fmt::format("Wrong idx {} for strip byte counts", idx));
    }
    if (m_entries[i].dataType() == DataType::Short) {
      return m_entries[i].dataAt<uint16_t>(idx);
    } else if (m_entries[i].dataType() == DataType::Long) {
      return m_entries[i].dataAt<uint32_t>(idx);
    } else {
      return m_entries[i].dataAt<uint64_t>(idx);
    }
  }
  return 0;
}

uint64_t ZTiffIFD::tileWidth() const
{
  auto i = indexOf(TIFFTAG_TILEWIDTH);
  if (i < 0) {
    throw ZException("Tile width required for tile image");
  }
  if (m_entries[i].dataType() == DataType::Short) {
    return m_entries[i].dataAt<uint16_t>(0);
  } else {
    return m_entries[i].dataAt<uint32_t>(0);
  }
}

uint64_t ZTiffIFD::tileHeight() const
{
  auto i = indexOf(TIFFTAG_TILELENGTH);
  if (i < 0) {
    throw ZException("Tile length required for tile image");
  }
  if (m_entries[i].dataType() == DataType::Short) {
    return m_entries[i].dataAt<uint16_t>(0);
  } else {
    return m_entries[i].dataAt<uint32_t>(0);
  }
}

uint64_t ZTiffIFD::tileOffsets(size_t idx) const
{
  auto i = indexOf(TIFFTAG_TILEOFFSETS);
  if (i != -1) {
    if (idx >= m_entries[i].count()) {
      throw ZException(fmt::format("Wrong idx {} for tile offsets", idx));
    }
    if (m_entries[i].dataType() == DataType::Short) {
      return m_entries[i].dataAt<uint16_t>(idx);
    } else if (m_entries[i].dataType() == DataType::Long) {
      return m_entries[i].dataAt<uint32_t>(idx);
    } else {
      return m_entries[i].dataAt<uint64_t>(idx);
    }
  }
  return 0;
}

uint64_t ZTiffIFD::tileByteCounts(size_t idx) const
{
  auto i = indexOf(TIFFTAG_TILEBYTECOUNTS);
  if (i != -1) {
    if (idx >= m_entries[i].count()) {
      throw ZException(fmt::format("Wrong idx {} for tile byte counts", idx));
    }
    if (m_entries[i].dataType() == DataType::Short) {
      return m_entries[i].dataAt<uint16_t>(idx);
    } else if (m_entries[i].dataType() == DataType::Long) {
      return m_entries[i].dataAt<uint32_t>(idx);
    } else {
      return m_entries[i].dataAt<uint64_t>(idx);
    }
  }
  return 0;
}

std::string ZTiffIFD::toString() const
{
  std::string res;
  for (const auto& entry : m_entries) {
    fmt::format_to(std::back_inserter(res), "{}\n", entry);
  }
  if (!m_subIFDs.empty()) {
    res.append("\n");
    for (size_t i = 0; i < m_subIFDs.size(); ++i) {
      fmt::format_to(std::back_inserter(res), "Sub IFD {}\n{}", i, m_subIFDs[i]);
    }
  }
  if (!m_exifIFD.empty()) {
    fmt::format_to(std::back_inserter(res), "\nExif IFD\n{}", m_exifIFD[0]);
  }
  if (!m_gpsIFD.empty()) {
    fmt::format_to(std::back_inserter(res), "\nGPS IFD\n{}", m_gpsIFD[0]);
  }
  if (!m_interoperabilityIFD.empty()) {
    fmt::format_to(std::back_inserter(res), "\nInteroperability IFD\n{}", m_interoperabilityIFD[0]);
  }
  return res;
}

bool ZTiffIFD::isGrayscaleColormap() const
{
  auto i = indexOf(TIFFTAG_COLORMAP);
  if (i != -1) {
    size_t count = m_entries[i].count();
    if (count % 3 != 0) {
      return true;
    }
    size_t colorCount = count / 3;
    for (size_t j = 0; j < colorCount; ++j) {
      if (m_entries[i].dataAt<uint16_t>(j) != m_entries[i].dataAt<uint16_t>(j + colorCount) ||
          m_entries[i].dataAt<uint16_t>(j) != m_entries[i].dataAt<uint16_t>(j + 2 * colorCount)) {
        return false;
      }
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
  for (const auto& entry : m_entries) {
    if (tags.contains(entry.tag())) {
      res.push_back(entry);
    }
  }
  if (hasExifIFD()) {
    res.insert(res.end(), exifIFD()->m_entries.begin(), exifIFD()->m_entries.end());
  }
  if (hasGpsIFD()) {
    res.insert(res.end(), gpsIFD()->m_entries.begin(), gpsIFD()->m_entries.end());
  }
  return res;
}

index_t ZTiffIFD::indexOf(uint64_t tag) const
{
  for (size_t i = 0; i < m_entries.size(); ++i) {
    if (m_entries[i].tag() == tag) {
      return static_cast<index_t>(i);
    }
  }
  return -1;
}

uint32_t ZTiffIFD::subfileTypeData() const
{
  auto i = indexOf(TIFFTAG_SUBFILETYPE);
  if (i != -1) {
    return m_entries[i].dataAt<uint32_t>(0);
  }
  return 0;
}

ZTiff::ZTiff()
  : m_tif(nullptr, TIFFClose)
{}

std::string ZTiff::toString() const
{
  std::string res;
  for (size_t i = 0; i < m_ifds.size(); ++i) {
    fmt::format_to(std::back_inserter(res),
                   "Directory {0}: offset {1} ({1:#x}) next {2} ({2:#x})\n{3}\n",
                   i,
                   m_ifds[i].offset(),
                   m_ifds[i].nextIFDOffset(),
                   m_ifds[i]);
  }
  return res;
}

void ZTiff::load(const QString& filename, bool tagOnly)
{
  close();

  readIFDs(filename, m_ifds, m_isNativeEndianness);
  // VLOG(1) << toString();

  if (m_useColormap) {
    bool allGray = true;
    for (auto& ifd : m_ifds) {
      if (!ifd.isGrayscaleColormap()) {
        allGray = false;
        break;
      }
    }
    if (allGray) {
      m_useColormap = false;
    }
  }
  if (m_useColormap) {
    std::string imageDes = m_ifds[0].imageDescription();
    if (imageDes.starts_with("ImageJ="sv) && absl::StrContains(imageDes, "images="sv)) {
      m_useColormap = false;
    }
  }

  if (!tagOnly) {
    TIFFSetWarningHandler(nullptr);
    if (m_useColormap) {
      TIFFSetErrorHandler(LibtiffErrorHandler);
    } else {
      TIFFSetErrorHandler(LibtiffErrorHandlerIgnoreColormapError);
    }

    try {
#if defined(_WIN32) || defined(_WIN64)
      m_tif.reset(TIFFOpenW(filename.toStdWString().c_str(), kTiffReadMode));
#else
      m_tif.reset(TIFFOpen(QFile::encodeName(filename).constData(), kTiffReadMode));
#endif
    }
    catch (const ZException& e) {
      const QString& err(e.what());
      if (err.contains("Colormap", Qt::CaseInsensitive)) {
        m_tif.reset();
        LOG(WARNING) << "Disable colormap because of error " << err;
        m_useColormap = false;
        TIFFSetErrorHandler(LibtiffErrorHandlerIgnoreColormapError);
#if defined(_WIN32) || defined(_WIN64)
        m_tif.reset(TIFFOpenW(filename.toStdWString().c_str(), kTiffReadMode));
#else
        m_tif.reset(TIFFOpen(QFile::encodeName(filename).constData(), kTiffReadMode));
#endif
      } else {
        m_tif.reset();
        throw;
      }
    }
    if (!m_tif) {
      throw ZException("Libtiff can not open Tiff file", ZException::Option::CheckErrno);
    }
  }
}

void ZTiff::load(std::istream& fs, bool tagOnly)
{
  close();

  readIFDs(fs, m_ifds, m_isNativeEndianness);

  if (!tagOnly) {
    if (fs.fail()) {
      throw ZException("Read tiff tags failed", ZException::Option::CheckErrno);
    }
    fs.clear();
    fs.seekg(0);

    TIFFSetWarningHandler(nullptr);
    if (m_useColormap) {
      TIFFSetErrorHandler(LibtiffErrorHandler);
    } else {
      TIFFSetErrorHandler(LibtiffErrorHandlerIgnoreColormapError);
    }

    try {
      m_tif.reset(TIFFStreamOpen("MemTiff", &fs));
    }
    catch (const ZException& e) {
      const QString& err(e.what());
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
      throw ZException("Libtiff can not open Tiff file", ZException::Option::CheckErrno);
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

void ZTiff::readInfoFromIFD(const ZTiffIFD& ifd, ZImgInfo& info) const
{
  info.width = ifd.imageWidth();
  info.height = ifd.imageHeight();
  if (ifd.orientation() > 4) {
    std::swap(info.width, info.height);
  }

  if (ifd.photometricInterpretation() == PHOTOMETRIC_MINISBLACK ||
      ifd.photometricInterpretation() == PHOTOMETRIC_MINISWHITE || ifd.photometricInterpretation() == PHOTOMETRIC_RGB ||
      !m_useColormap) {
    info.numChannels = ifd.samplesPerPixel();
    size_t bps = ifd.bitsPerSample(0);
    info.validBitCount = bps;
    for (size_t j = 1; j < info.numChannels; ++j) {
      if (ifd.bitsPerSample(j) != bps) {
        throw ZException("Different bits per sample is not supported.");
      }
    }

    bool bps1valid = true;
    size_t bps1 = ifd.bitsPerSampleFromMaxSampleValue(0);
    for (size_t j = 1; j < info.numChannels; ++j) {
      if (ifd.bitsPerSampleFromMaxSampleValue(j) != bps1) {
        bps1valid = false;
        LOG(WARNING) << "Different bits per sample from MaxSampleValue is not supported.";
      }
    }
    if (bps1valid) {
      info.validBitCount = bps1;
    }

    if (bps > 64 || (bps > 8 && bps % 8 != 0)) {
      throw ZException(fmt::format("{} bits per sample tiff is not supported.", bps));
    }

    info.bytesPerVoxel = std::ceil(bps * 1.0 / 8);
    VoxelFormat vf = ifd.voxelFormat(0);
    for (size_t j = 1; j < info.numChannels; ++j) {
      if (ifd.voxelFormat(j) != vf) {
        throw ZException("Different sample format is not supported.");
      }
    }
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
  if (TIFFSetDirectory(m_tif.get(), ifdIdx) != 1) {
    throw ZException(fmt::format("Can not read ifd of index {}", ifdIdx), ZException::Option::CheckErrno);
  }
  readImg(img,
          m_ifds[ifdIdx].extraSample() == EXTRASAMPLE_ASSOCALPHA ||
            m_ifds[ifdIdx].extraSample() == EXTRASAMPLE_UNSPECIFIED);
}

void ZTiff::readImgFromIFD(const ZTiffIFD& ifd, ZImg& img)
{
  if (TIFFSetSubDirectory(m_tif.get(), ifd.offset()) != 1) {
    throw ZException(fmt::format("Can not read ifd at offset {}", ifd.offset()), ZException::Option::CheckErrno);
  }
  readImg(img, ifd.extraSample() == EXTRASAMPLE_ASSOCALPHA || ifd.extraSample() == EXTRASAMPLE_UNSPECIFIED);
}

namespace {

void copyOneChannelTileRegionToImg(const uint8_t* tileBuf,
                                   size_t tileWidth,
                                   size_t tileHeight,
                                   size_t voxelByteNumber,
                                   ZImg& img,
                                   const ZImgRegion& region,
                                   size_t tileX,
                                   size_t tileY,
                                   size_t dstChannel)
{
  const size_t x0 = std::max<size_t>(tileX, static_cast<size_t>(region.start.x));
  const size_t y0 = std::max<size_t>(tileY, static_cast<size_t>(region.start.y));
  const size_t x1 = std::min<size_t>(tileX + tileWidth, static_cast<size_t>(region.end.x));
  const size_t y1 = std::min<size_t>(tileY + tileHeight, static_cast<size_t>(region.end.y));
  if (x0 >= x1 || y0 >= y1) {
    return;
  }

  const size_t copyBytes = (x1 - x0) * voxelByteNumber;
  for (size_t y = y0; y < y1; ++y) {
    const uint8_t* src = tileBuf + ((y - tileY) * tileWidth + (x0 - tileX)) * voxelByteNumber;
    uint8_t* dst = img.data<uint8_t>(x0 - region.start.x, y - region.start.y, 0, dstChannel);
    std::copy_n(src, copyBytes, dst);
  }
}

void copyContiguousTileRegionToImg(const uint8_t* tileBuf,
                                   size_t tileWidth,
                                   size_t tileHeight,
                                   size_t numChannels,
                                   size_t voxelByteNumber,
                                   ZImg& img,
                                   const ZImgRegion& region,
                                   size_t tileX,
                                   size_t tileY)
{
  const size_t x0 = std::max<size_t>(tileX, static_cast<size_t>(region.start.x));
  const size_t y0 = std::max<size_t>(tileY, static_cast<size_t>(region.start.y));
  const size_t x1 = std::min<size_t>(tileX + tileWidth, static_cast<size_t>(region.end.x));
  const size_t y1 = std::min<size_t>(tileY + tileHeight, static_cast<size_t>(region.end.y));
  if (x0 >= x1 || y0 >= y1) {
    return;
  }

  for (size_t c = static_cast<size_t>(region.start.c); c < static_cast<size_t>(region.end.c); ++c) {
    for (size_t y = y0; y < y1; ++y) {
      for (size_t x = x0; x < x1; ++x) {
        const uint8_t* src =
          tileBuf + ((y - tileY) * tileWidth * numChannels + (x - tileX) * numChannels + c) * voxelByteNumber;
        uint8_t* dst = img.data<uint8_t>(x - region.start.x, y - region.start.y, 0, c - region.start.c);
        std::copy_n(src, voxelByteNumber, dst);
      }
    }
  }
}

void copyOneChannelStripRegionToImg(const uint8_t* stripBuf,
                                    size_t sourceWidth,
                                    size_t stripY,
                                    size_t voxelByteNumber,
                                    ZImg& img,
                                    const ZImgRegion& region,
                                    size_t rowCount,
                                    size_t dstChannel)
{
  const size_t y0 = std::max<size_t>(stripY, static_cast<size_t>(region.start.y));
  const size_t y1 = std::min<size_t>(stripY + rowCount, static_cast<size_t>(region.end.y));
  if (y0 >= y1) {
    return;
  }

  const size_t x0 = static_cast<size_t>(region.start.x);
  const size_t x1 = static_cast<size_t>(region.end.x);
  const size_t copyBytes = (x1 - x0) * voxelByteNumber;
  for (size_t y = y0; y < y1; ++y) {
    const uint8_t* src = stripBuf + ((y - stripY) * sourceWidth + x0) * voxelByteNumber;
    uint8_t* dst = img.data<uint8_t>(0, y - region.start.y, 0, dstChannel);
    std::copy_n(src, copyBytes, dst);
  }
}

void copyContiguousStripRegionToImg(const uint8_t* stripBuf,
                                    size_t sourceWidth,
                                    size_t stripY,
                                    size_t numChannels,
                                    size_t voxelByteNumber,
                                    ZImg& img,
                                    const ZImgRegion& region,
                                    size_t rowCount)
{
  const size_t y0 = std::max<size_t>(stripY, static_cast<size_t>(region.start.y));
  const size_t y1 = std::min<size_t>(stripY + rowCount, static_cast<size_t>(region.end.y));
  if (y0 >= y1) {
    return;
  }

  for (size_t c = static_cast<size_t>(region.start.c); c < static_cast<size_t>(region.end.c); ++c) {
    for (size_t y = y0; y < y1; ++y) {
      for (size_t x = static_cast<size_t>(region.start.x); x < static_cast<size_t>(region.end.x); ++x) {
        const uint8_t* src =
          stripBuf + ((y - stripY) * sourceWidth * numChannels + x * numChannels + c) * voxelByteNumber;
        uint8_t* dst = img.data<uint8_t>(x - region.start.x, y - region.start.y, 0, c - region.start.c);
        std::copy_n(src, voxelByteNumber, dst);
      }
    }
  }
}

} // namespace

void ZTiff::readRegionFromIFD(const ZTiffIFD& ifd, ZImg& img, const ZImgRegion& region)
{
  ZImgInfo ifdInfo;
  readInfoFromIFD(ifd, ifdInfo);
  if (region.isEmpty() || !region.isValid(ifdInfo)) {
    throw ZException(fmt::format("Invalid TIFF IFD region. IFD info: '{}', region: '{}'", ifdInfo, region));
  }

  ZImgRegion resolvedRegion = region;
  resolvedRegion.resolveRegionEnd(ifdInfo);
  if (resolvedRegion.start.z != 0 || resolvedRegion.end.z != 1 || resolvedRegion.start.t != 0 ||
      resolvedRegion.end.t != 1) {
    throw ZException(fmt::format("TIFF IFD region must address one 2D plane: {}", resolvedRegion));
  }

  if (resolvedRegion.containsWholeImg(ifdInfo)) {
    img = ZImg(ifdInfo);
    readImgFromIFD(ifd, img);
    return;
  }

  const uint16_t orientation = ifd.orientation();
  const bool unsupportedOrientation = orientation != ORIENTATION_TOPLEFT;
  const bool readAsRgba = ifd.photometricInterpretation() != PHOTOMETRIC_MINISBLACK &&
                          ifd.photometricInterpretation() != PHOTOMETRIC_MINISWHITE &&
                          ifd.photometricInterpretation() != PHOTOMETRIC_RGB && m_useColormap;
  if (unsupportedOrientation || readAsRgba) {
    ZImg full(ifdInfo);
    readImgFromIFD(ifd, full);
    img = full.crop(resolvedRegion);
    return;
  }

  if (TIFFSetSubDirectory(m_tif.get(), ifd.offset()) != 1) {
    throw ZException(fmt::format("Can not read ifd at offset {}", ifd.offset()), ZException::Option::CheckErrno);
  }

  uint16_t planarConfig;
  TIFFGetFieldDefaulted(m_tif.get(), TIFFTAG_PLANARCONFIG, &planarConfig);
  const bool separatePlane = PLANARCONFIG_SEPARATE == planarConfig;
  const bool invertWhiteBlack = ifd.photometricInterpretation() == PHOTOMETRIC_MINISWHITE;
  if (invertWhiteBlack && (ifd.voxelFormat(0) == VoxelFormat::Signed || ifd.voxelFormat(0) == VoxelFormat::Float)) {
    throw ZException("Don't support PHOTOMETRIC_MINISWHITE for signed or double image.");
  }

  img = ZImg(resolvedRegion.clip(ifdInfo));
  const size_t channelBegin = static_cast<size_t>(resolvedRegion.start.c);
  const size_t channelEnd = static_cast<size_t>(resolvedRegion.end.c);

  if (TIFFIsTiled(m_tif.get())) {
    uint32_t tileWidth;
    uint32_t tileHeight;
    TIFFGetField(m_tif.get(), TIFFTAG_TILEWIDTH, &tileWidth);
    TIFFGetField(m_tif.get(), TIFFTAG_TILELENGTH, &tileHeight);
    const size_t tilesPerRow = (ifdInfo.width + tileWidth - 1) / tileWidth;
    const size_t firstTileCol = static_cast<size_t>(resolvedRegion.start.x) / tileWidth;
    const size_t lastTileCol = (static_cast<size_t>(resolvedRegion.end.x) - 1) / tileWidth;
    const size_t firstTileRow = static_cast<size_t>(resolvedRegion.start.y) / tileHeight;
    const size_t lastTileRow = (static_cast<size_t>(resolvedRegion.end.y) - 1) / tileHeight;

    if (separatePlane || ifdInfo.numChannels == 1) {
      const uint32_t tilesPerChannel = TIFFNumberOfTiles(m_tif.get()) / ifdInfo.numChannels;
      std::vector<uint8_t, boost::alignment::aligned_allocator<uint8_t, 64>> tileBuf(tileWidth * tileHeight *
                                                                                     ifdInfo.voxelByteNumber());
      for (size_t c = channelBegin; c < channelEnd; ++c) {
        for (size_t tileRow = firstTileRow; tileRow <= lastTileRow; ++tileRow) {
          for (size_t tileCol = firstTileCol; tileCol <= lastTileCol; ++tileCol) {
            const uint32_t tile = static_cast<uint32_t>(c * tilesPerChannel + tileRow * tilesPerRow + tileCol);
            readTile(tile, tileBuf.data(), tileWidth, tileHeight, 1, invertWhiteBlack);
            copyOneChannelTileRegionToImg(tileBuf.data(),
                                          tileWidth,
                                          tileHeight,
                                          ifdInfo.voxelByteNumber(),
                                          img,
                                          resolvedRegion,
                                          tileCol * tileWidth,
                                          tileRow * tileHeight,
                                          c - channelBegin);
          }
        }
      }
    } else {
      std::vector<uint8_t, boost::alignment::aligned_allocator<uint8_t, 64>> tileBuf(
        tileWidth * tileHeight * ifdInfo.voxelByteNumber() * ifdInfo.numChannels);
      for (size_t tileRow = firstTileRow; tileRow <= lastTileRow; ++tileRow) {
        for (size_t tileCol = firstTileCol; tileCol <= lastTileCol; ++tileCol) {
          const uint32_t tile = static_cast<uint32_t>(tileRow * tilesPerRow + tileCol);
          readTile(tile, tileBuf.data(), tileWidth, tileHeight, ifdInfo.numChannels, invertWhiteBlack);
          copyContiguousTileRegionToImg(tileBuf.data(),
                                        tileWidth,
                                        tileHeight,
                                        ifdInfo.numChannels,
                                        ifdInfo.voxelByteNumber(),
                                        img,
                                        resolvedRegion,
                                        tileCol * tileWidth,
                                        tileRow * tileHeight);
        }
      }
    }
  } else {
    const size_t rowsPerStrip = std::min<size_t>(ifd.rowsPerStrip(), ifdInfo.height);
    const size_t firstStrip = static_cast<size_t>(resolvedRegion.start.y) / rowsPerStrip;
    const size_t lastStrip = (static_cast<size_t>(resolvedRegion.end.y) - 1) / rowsPerStrip;

    if (separatePlane || ifdInfo.numChannels == 1) {
      const uint32_t stripsPerChannel = TIFFNumberOfStrips(m_tif.get()) / ifdInfo.numChannels;
      std::vector<uint8_t, boost::alignment::aligned_allocator<uint8_t, 64>> stripBuf(rowsPerStrip * ifdInfo.width *
                                                                                      ifdInfo.voxelByteNumber());
      for (size_t c = channelBegin; c < channelEnd; ++c) {
        for (size_t strip = firstStrip; strip <= lastStrip; ++strip) {
          const size_t stripY = strip * rowsPerStrip;
          const size_t rowCount = std::min(rowsPerStrip, ifdInfo.height - stripY);
          readStrip(static_cast<uint32_t>(c * stripsPerChannel + strip),
                    stripBuf.data(),
                    ifdInfo.width,
                    rowCount,
                    1,
                    invertWhiteBlack);
          copyOneChannelStripRegionToImg(stripBuf.data(),
                                         ifdInfo.width,
                                         stripY,
                                         ifdInfo.voxelByteNumber(),
                                         img,
                                         resolvedRegion,
                                         rowCount,
                                         c - channelBegin);
        }
      }
    } else {
      std::vector<uint8_t, boost::alignment::aligned_allocator<uint8_t, 64>> stripBuf(
        rowsPerStrip * ifdInfo.width * ifdInfo.voxelByteNumber() * ifdInfo.numChannels);
      for (size_t strip = firstStrip; strip <= lastStrip; ++strip) {
        const size_t stripY = strip * rowsPerStrip;
        const size_t rowCount = std::min(rowsPerStrip, ifdInfo.height - stripY);
        readStrip(static_cast<uint32_t>(strip),
                  stripBuf.data(),
                  ifdInfo.width,
                  rowCount,
                  ifdInfo.numChannels,
                  invertWhiteBlack);
        copyContiguousStripRegionToImg(stripBuf.data(),
                                       ifdInfo.width,
                                       stripY,
                                       ifdInfo.numChannels,
                                       ifdInfo.voxelByteNumber(),
                                       img,
                                       resolvedRegion,
                                       rowCount);
      }
    }
  }

  if (ifd.extraSample() == EXTRASAMPLE_ASSOCALPHA || ifd.extraSample() == EXTRASAMPLE_UNSPECIFIED) {
    img.correctPreMultipliedColor();
  }
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
    catch (const ZException& e) {
      LOG(WARNING) << "read thumbnail from tif ifd failed: " << e.what();
      res.clear();
    }
  }

  return res;
}

struct ZTiffHeader
{
  TIFFHeaderBig header = {TIFF_LITTLEENDIAN, 43, 8, 0, 16};
  uint64_t dircount = 8;

  uint16_t tag1 = TIFFTAG_IMAGEWIDTH;
  uint16_t type1 = std::to_underlying(DataType::Long);
  uint64_t count1 = 1;
  uint32_t width = 0;
  uint32_t fill1 = 0;

  uint16_t tag2 = TIFFTAG_IMAGELENGTH;
  uint16_t type2 = std::to_underlying(DataType::Long);
  uint64_t count2 = 1;
  uint32_t height = 0;
  uint32_t fill2 = 0;

  uint16_t tag3 = TIFFTAG_BITSPERSAMPLE;
  uint16_t type3 = std::to_underlying(DataType::Short);
  uint64_t count3 = 1;
  uint16_t bitPerSample = 0;
  uint16_t fill31 = 0;
  uint32_t fill32 = 0;

  uint16_t tag4 = TIFFTAG_SAMPLESPERPIXEL;
  uint16_t type4 = std::to_underlying(DataType::Short);
  uint64_t count4 = 1;
  uint16_t samplesPerPixel = 0;
  uint16_t fill41 = 0;
  uint32_t fill42 = 0;

  uint16_t tag5 = TIFFTAG_COMPRESSION;
  uint16_t type5 = std::to_underlying(DataType::Short);
  uint64_t count5 = 1;
  uint16_t compression = 0;
  uint16_t fill51 = 0;
  uint32_t fill52 = 0;

  uint16_t tag6 = TIFFTAG_PHOTOMETRIC;
  uint16_t type6 = std::to_underlying(DataType::Short);
  uint64_t count6 = 1;
  uint16_t photoMetric = 1;
  uint16_t fill61 = 0;
  uint32_t fill62 = 0;

  uint16_t tag7 = TIFFTAG_STRIPOFFSETS;
  uint16_t type7 = std::to_underlying(DataType::Long8);
  uint64_t count7 = 1;
  uint64_t stripOffset = 0;

  uint16_t tag8 = TIFFTAG_STRIPBYTECOUNTS;
  uint16_t type8 = std::to_underlying(DataType::Long8);
  uint64_t count8 = 1;
  uint64_t stripByteCount = 0;

  uint64_t nextdiroff = 0;
};

void ZTiff::writeTiffHeader(uint8_t* mem,
                            size_t memSize,
                            size_t width,
                            size_t height,
                            size_t bitsPerSample,
                            size_t samplesPerPixel,
                            Compression compression,
                            uint64_t stripOffset,
                            uint64_t stripByteCount)
{
  CHECK(stripOffset >= 192);
  ZTiffHeader header;
  header.width = width;
  header.height = height;
  header.bitPerSample = bitsPerSample;
  header.samplesPerPixel = samplesPerPixel;
  header.compression = getTiffCompressionTag(compression);
  header.stripOffset = stripOffset;
  header.stripByteCount = stripByteCount;
  compactStructToMemory(mem, memSize, header);
}

uint64_t ZTiff::readIFD(std::istream& fs, ZTiffIFD& ifd, uint64_t off, bool bigtiff, bool swabflag) const
{
  uint64_t nextdiroff = 0;

  if (off == 0) { /* no more directories */
    return 0;
  }
  fs.seekg(off);

  uint16_t dircount;
  uint32_t direntrysize;
  if (!bigtiff) {
    readStream(fs, &dircount, sizeof(uint16_t));
    if (swabflag) {
      byteswap_inplace(dircount);
    }
    direntrysize = 12;
  } else {
    uint64_t dircount64 = 0;
    readStream(fs, &dircount64, sizeof(uint64_t));
    if (swabflag) {
      byteswap_inplace(dircount64);
    }
    if (dircount64 > 0xFFFF) {
      throw ZException("Sanity check on directory count failed");
    }
    dircount = static_cast<uint16_t>(dircount64);
    direntrysize = 20;
  }

  std::vector<char> dirmemvector(dircount * direntrysize);
  fs.read(dirmemvector.data(), dirmemvector.size());
  uint32_t n = fs.gcount();
  if (n != dirmemvector.size()) {
    n /= direntrysize;
    LOG(WARNING) << "Could only read " << n << " of " << dircount << " entries in directory at offset " << off;
    dircount = n;
    nextdiroff = 0;
  } else {
    if (!bigtiff) {
      uint32_t nextdiroff32;
      // reinterpret_cast allowed (AliasedType is char or unsigned char: this permits
      // examination of the object representation of any object as an array of unsigned char.)
      fs.read(reinterpret_cast<char*>(&nextdiroff32), sizeof(uint32_t));
      if (static_cast<size_t>(fs.gcount()) != sizeof(uint32_t)) {
        nextdiroff32 = 0;
      }
      if (swabflag) {
        byteswap_inplace(nextdiroff32);
      }
      nextdiroff = nextdiroff32;
    } else {
      fs.read(reinterpret_cast<char*>(&nextdiroff), sizeof(uint64_t));
      if (static_cast<size_t>(fs.gcount()) != sizeof(uint64_t)) {
        nextdiroff = 0;
      }
      if (swabflag) {
        byteswap_inplace(nextdiroff);
      }
    }
  }
  fs.clear();

  char* dp = dirmemvector.data();
  for (n = dircount; n > 0; n--) {
    ZImgMetatag field;

    uint16_t tag;
    std::memcpy(&tag, dp, sizeof(tag));
    dp += sizeof(uint16_t);
    if (swabflag) {
      byteswap_inplace(tag);
    }
    field.setTag(tag);
    field.setName(tagToName(tag));

    uint16_t type;
    std::memcpy(&type, dp, sizeof(type));
    dp += sizeof(uint16_t);
    if (swabflag) {
      byteswap_inplace(type);
    }
    if (!isValidDataType(type)) {
      throw ZException(fmt::format("Wrong tiff tag dataType: {}", type));
    }
    field.setDataType(static_cast<DataType>(type));

    uint64_t count;
    if (!bigtiff) {
      uint32_t count32;
      std::memcpy(&count32, dp, sizeof(count32));
      dp += sizeof(uint32_t);
      if (swabflag) {
        byteswap_inplace(count32);
      }
      count = count32;
    } else {
      std::memcpy(&count, dp, sizeof(count));
      dp += sizeof(uint64_t);
      if (swabflag) {
        byteswap_inplace(count);
      }
    }
    field.setCount(count);

    bool datafits = true;
    uint64_t dataoffset = 0;
    if (!bigtiff) {
      if (field.dataByteNumber() > 4) {
        datafits = false;
        uint32_t dataoffset32;
        std::memcpy(&dataoffset32, dp, sizeof(dataoffset32));
        if (swabflag) {
          byteswap_inplace(dataoffset32);
        }
        dataoffset = dataoffset32;
      } else {
        std::memcpy(field.dataArray(), dp, field.dataByteNumber());
      }
      dp += sizeof(uint32_t);
    } else {
      if (field.dataByteNumber() > 8) {
        datafits = false;
        std::memcpy(&dataoffset, dp, sizeof(dataoffset));
        if (swabflag) {
          byteswap_inplace(dataoffset);
        }
      } else {
        std::memcpy(field.dataArray(), dp, field.dataByteNumber());
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
            byteswap_inplace(field.dataAt<uint16_t>(sc));
          }
          break;
        case TIFF_LONG:
        case TIFF_SLONG:
        case TIFF_FLOAT:
        case TIFF_IFD:
          for (size_t sc = 0; sc < count; ++sc) {
            byteswap_inplace(field.dataAt<uint32_t>(sc));
          }
          break;
        case TIFF_RATIONAL:
        case TIFF_SRATIONAL:
          for (size_t sc = 0; sc < count * 2; ++sc) {
            byteswap_inplace(field.dataAt<uint32_t>(sc));
          }
          break;
        case TIFF_DOUBLE:
        case TIFF_LONG8:
        case TIFF_SLONG8:
        case TIFF_IFD8:
          for (size_t sc = 0; sc < count; ++sc) {
            byteswap_inplace(field.dataAt<uint64_t>(sc));
          }
          break;
        default:
          break;
      }
    }
    ifd.addField(field);

    if (field.tag() == TIFFTAG_SUBIFD) {
      for (size_t sc = 0; sc < field.count(); ++sc) {
        ZTiffIFD subIFD;
        if (bigtiff) {
          readIFD(fs, subIFD, field.dataAt<uint64_t>(sc), bigtiff, swabflag);
        } else {
          readIFD(fs, subIFD, field.dataAt<uint32_t>(sc), bigtiff, swabflag);
        }
        ifd.addSubIFD(subIFD);
      }
    }

    if (field.tag() == TIFFTAG_EXIFIFD) {
      ZTiffIFD exifIFD;
      if (bigtiff) {
        readIFD(fs, exifIFD, field.dataAt<uint64_t>(0), bigtiff, swabflag);
      } else {
        readIFD(fs, exifIFD, field.dataAt<uint32_t>(0), bigtiff, swabflag);
      }
      ifd.setExifIFD(exifIFD);
    }

    if (field.tag() == TIFFTAG_GPSIFD) {
      ZTiffIFD gpsIFD;
      if (bigtiff) {
        readIFD(fs, gpsIFD, field.dataAt<uint64_t>(0), bigtiff, swabflag);
      } else {
        readIFD(fs, gpsIFD, field.dataAt<uint32_t>(0), bigtiff, swabflag);
      }
      ifd.setGpsIFD(gpsIFD);
    }

    if (field.tag() == TIFFTAG_INTEROPERABILITYIFD) {
      ZTiffIFD interIFD;
      if (bigtiff) {
        readIFD(fs, interIFD, field.dataAt<uint64_t>(0), bigtiff, swabflag);
      } else {
        readIFD(fs, interIFD, field.dataAt<uint32_t>(0), bigtiff, swabflag);
      }
      ifd.setInteroperabilityIFD(interIFD);
    }
  }
  ifd.setOffset(off);
  ifd.setNextIFDOffset(nextdiroff);
  return nextdiroff;
}

std::string ZTiff::tagToName(uint32_t tag)
{
  for (const auto& tagName : tiftagnames) {
    if (tagName.tag == tag) {
      return tagName.name;
    }
  }
  for (const auto& tagName : exiftagnames) {
    if (tagName.tag == tag) {
      return tagName.name;
    }
  }
  return "Unknown tag";
}

void ZTiff::readIFDs(const QString& filename, std::vector<ZTiffIFD>& ifds, bool& isNativeEndianness) const
{
  std::ifstream fs = openIFStream(filename, std::ios_base::binary | std::ios_base::in);
  readIFDs(fs, ifds, isNativeEndianness);
}

void ZTiff::readIFDs(std::istream& fs, std::vector<ZTiffIFD>& ifds, bool& isNativeEndianness) const
{
  union
  {
    TIFFHeaderClassic classic;
    TIFFHeaderBig big;
    TIFFHeaderCommon common;
  } hdr;

  std::vector<ZTiffIFD> _ifds;
  readStream(fs, &hdr, sizeof(TIFFHeaderCommon));

  if (hdr.common.tiff_magic != TIFF_BIGENDIAN && hdr.common.tiff_magic != TIFF_LITTLEENDIAN &&
      hdr.common.tiff_magic !=
        (std::endian::native == std::endian::little ? MDI_LITTLEENDIAN : MDI_BIGENDIAN)) {
    throw ZException(fmt::format("Not a TIFF or MDI file, bad magic number {:#x}", hdr.common.tiff_magic));
  }

  bool swabflag;
  if (hdr.common.tiff_magic == TIFF_BIGENDIAN || hdr.common.tiff_magic == MDI_BIGENDIAN) {
    swabflag = std::endian::native == std::endian::little;
  } else {
    swabflag = std::endian::native == std::endian::big;
  }
  isNativeEndianness = !swabflag;
  // VLOG(1) << swabflag << " " << hostIsLittleEndian() << " " << (hdr.common.tiff_magic == TIFF_LITTLEENDIAN);

  if (swabflag) {
    byteswap_inplace(hdr.common.tiff_version);
  }

  // VLOG(1) << swabflag << " " << hostIsLittleEndian() << " " << (hdr.common.tiff_magic == TIFF_LITTLEENDIAN) << " "
  // << hdr.common.tiff_version;

  bool bigtiff = false;
  uint64_t diroff = 0;
  if (hdr.common.tiff_version == 42) {
    readStream(fs, &hdr.classic.tiff_diroff, 4);
    if (swabflag) {
      byteswap_inplace(hdr.classic.tiff_diroff);
    }
    //    printf("Magic: %#x <%s-endian> Version: %#x <%s>\n",
    //           hdr.classic.tiff_magic,
    //           hdr.classic.tiff_magic == TIFF_BIGENDIAN ? "big" : "little",
    //           42,"ClassicTIFF");
    if (diroff == 0) {
      diroff = hdr.classic.tiff_diroff;
    }
  } else if (hdr.common.tiff_version == 43) {
    readStream(fs, &hdr.big.tiff_offsetsize, 12);
    if (swabflag) {
      byteswap_inplace(hdr.big.tiff_offsetsize);
      byteswap_inplace(hdr.big.tiff_unused);
      byteswap_inplace(hdr.big.tiff_diroff);
    }
    //    printf("Magic: %#x <%s-endian> Version: %#x <%s>\n",
    //           hdr.big.tiff_magic,
    //           hdr.big.tiff_magic == TIFF_BIGENDIAN ? "big" : "little",
    //           43,"BigTIFF");
    //    printf("OffsetSize: %#x Unused: %#x\n",
    //           hdr.big.tiff_offsetsize,hdr.big.tiff_unused);
    if (diroff == 0) {
      diroff = hdr.big.tiff_diroff;
    }
    bigtiff = true;
  } else {
    throw ZException(fmt::format("Not a TIFF file, bad version number {}", hdr.common.tiff_version));
  }

  std::set<uint64_t> visitedDiroffs;
  while (diroff != 0) {
    if (visitedDiroffs.contains(diroff)) {
      throw ZException("Cycle detected in chaining of TIFF directories");
    }
    visitedDiroffs.insert(diroff);
    _ifds.emplace_back();
    diroff = readIFD(fs, _ifds.back(), diroff, bigtiff, swabflag);
    // workaround zeiss axio scanner exporter bug
    if (!_ifds.back().containsTag(TIFFTAG_IMAGEWIDTH)) {
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
    throw ZException("photometric is required field");
  }
  uint16_t sampleFormat;
  TIFFGetFieldDefaulted(m_tif.get(), TIFFTAG_SAMPLEFORMAT, &sampleFormat);
  if (sampleFormat == 5 || sampleFormat == 6) {
    throw ZException("tiff with complex sample is not supported");
  }
  if (sampleFormat < 1 || sampleFormat > 6) {
    throw ZException(fmt::format("invalid sample format {}", sampleFormat));
  }

  bool readAsRGBA = true;
  if (photometric == PHOTOMETRIC_MINISBLACK || photometric == PHOTOMETRIC_MINISWHITE ||
      photometric == PHOTOMETRIC_RGB || !m_useColormap) {
    readAsRGBA = false;
  }

  uint16_t orientation;
  TIFFGetFieldDefaulted(m_tif.get(), TIFFTAG_ORIENTATION, &orientation);

  // img info already consider the orientation, but to read the raw data, we need to change it back
  if (orientation > 4) {
    std::swap(img.infoRef().height, img.infoRef().width);
  }

  if (photometric == PHOTOMETRIC_MINISWHITE &&
      (sampleFormat == SAMPLEFORMAT_INT || sampleFormat == SAMPLEFORMAT_IEEEFP)) {
    throw ZException("Don't support PHOTOMETRIC_MINISWHITE for signed or double image.");
  }

  if (readAsRGBA) {
    ZImg bufImg(img.infoRef());
    auto raster = bufImg.channelData<uint32_t>(0);

    auto ret = TIFFReadRGBAImageOriented(m_tif.get(), img.width(), img.height(), raster, orientation, 0);
    if (ret == 0) {
      throw ZException("Read tiff as rgba failed", ZException::Option::CheckErrno);
    }
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

        std::vector<uint8_t, boost::alignment::aligned_allocator<uint8_t, 64>> tileBuf(tileWidth * tileHeight *
                                                                                       img.voxelByteNumber());

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
        std::vector<uint8_t, boost::alignment::aligned_allocator<uint8_t, 64>> tileBuf(
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
        if (numRowsOfLastStrip == 0) {
          numRowsOfLastStrip = numRowsPerStrip;
        }

        for (size_t c = 0; c < img.numChannels(); ++c) {
          auto buf = img.channelData<uint8_t>(c);

          size_t off = 0;
          for (uint32_t strip = c * stripsPerChannel; strip < (c + 1) * stripsPerChannel; strip++) {
            if (strip == (c + 1) * stripsPerChannel - 1) {
              off += readStrip(strip, buf + off, img.width(), numRowsOfLastStrip, 1, invertWhiteBlack);
            } else {
              off += readStrip(strip, buf + off, img.width(), numRowsPerStrip, 1, invertWhiteBlack);
            }
          }
          if (off != img.channelByteNumber()) {
            throw ZException(fmt::format("read({}):expected({})", off, img.channelByteNumber()),
                             ZException::Option::CheckErrno);
          }
        }
      } else {
        ZImg bufImg(img.infoRef());
        auto buf = bufImg.channelData<uint8_t>(0);

        uint32_t numRowsPerStrip;
        TIFFGetFieldDefaulted(m_tif.get(), TIFFTAG_ROWSPERSTRIP, &numRowsPerStrip);
        numRowsPerStrip = std::min(static_cast<uint32_t>(img.height()), numRowsPerStrip);
        uint32_t numRowsOfLastStrip = img.height() % numRowsPerStrip;
        if (numRowsOfLastStrip == 0) {
          numRowsOfLastStrip = numRowsPerStrip;
        }

        size_t off = 0;
        for (uint32_t strip = 0; strip < TIFFNumberOfStrips(m_tif.get()); strip++) {
          if (strip == TIFFNumberOfStrips(m_tif.get()) - 1) {
            off += readStrip(strip, buf + off, img.width(), numRowsOfLastStrip, img.numChannels(), invertWhiteBlack);
          } else {
            off += readStrip(strip, buf + off, img.width(), numRowsPerStrip, img.numChannels(), invertWhiteBlack);
          }
        }
        if (off != img.timeByteNumber()) {
          throw ZException(fmt::format("read({}):expected({})", off, img.timeByteNumber()),
                           ZException::Option::CheckErrno);
        }

        separateChannel(bufImg, img);
      }
    }

    if (divideByAlpha) {
      img.correctPreMultipliedColor();
    }
  }

  // correct orientation
  if (orientation > 4) {
    std::swap(img.infoRef().height, img.infoRef().width);
  }

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
  uint16_t bitspersample;
  TIFFGetFieldDefaulted(m_tif.get(), TIFFTAG_BITSPERSAMPLE, &bitspersample);
  if (bitspersample % 8 == 0) {
    size_t read = TIFFReadEncodedStrip(m_tif.get(), strip, buf, static_cast<tmsize_t>(-1));
    if (invert) {
      switch (bitspersample / 8) {
        case 1: {
          uint8_t* pt = buf;
          for (size_t i = 0; i < read; ++i) {
            pt[i] = std::numeric_limits<uint8_t>::max() - pt[i];
          }
        } break;
        case 2: {
          auto pt = reinterpret_cast<uint16_t*>(buf);
          for (size_t i = 0; i < read; ++i) {
            pt[i] = std::numeric_limits<uint16_t>::max() - pt[i];
          }
        } break;
        case 4: {
          auto pt = reinterpret_cast<uint32_t*>(buf);
          for (size_t i = 0; i < read; ++i) {
            pt[i] = std::numeric_limits<uint32_t>::max() - pt[i];
          }
        } break;
        case 8: {
          auto pt = reinterpret_cast<uint64_t*>(buf);
          for (size_t i = 0; i < read; ++i) {
            pt[i] = std::numeric_limits<uint64_t>::max() - pt[i];
          }
        } break;
        default:
          throw ZException(fmt::format("do not support invert {} bytes integer", bitspersample / 8));
      }
    }
    return read;
  }

  if (bitspersample == 1) {
    std::vector<uint8_t> packedBuf(TIFFStripSize(m_tif.get()));
    TIFFReadEncodedStrip(m_tif.get(), strip, packedBuf.data(), static_cast<tmsize_t>(-1));
    uint8_t* buf8 = buf;
    size_t bytesPerRow = (width * nChannel + 7) / 8;
    if (packedBuf.size() < bytesPerRow * height) {
      throw ZException(fmt::format("Not enought strip data, nRows:{}, nChannel:{}, bitsPerSample:{}, strip data:{}",
                                   height,
                                   nChannel,
                                   bitspersample,
                                   packedBuf.size()),
                       ZException::Option::CheckErrno);
    }
    for (size_t r = 0; r < height; ++r) {
      for (size_t i = 0; i < width * nChannel; ++i) {
        *buf8 = invert ? ((packedBuf[r * bytesPerRow + i / 8] & bitmasks1[i % 8]) == 0)
                       : ((packedBuf[r * bytesPerRow + i / 8] & bitmasks1[i % 8]) > 0);
        buf8++;
      }
    }
    return height * width * nChannel;
  }
  if (bitspersample == 2) {
    std::vector<uint8_t> packedBuf(TIFFStripSize(m_tif.get()));
    TIFFReadEncodedStrip(m_tif.get(), strip, packedBuf.data(), static_cast<tmsize_t>(-1));
    uint8_t* buf8 = buf;
    size_t bytesPerRow = (width * nChannel + 3) / 4;
    if (packedBuf.size() < bytesPerRow * height) {
      throw ZException(fmt::format("Not enought strip data, nRows:{}, nChannel:{}, bitsPerSample:{}, strip data:{}",
                                   height,
                                   nChannel,
                                   bitspersample,
                                   packedBuf.size()),
                       ZException::Option::CheckErrno);
    }
    for (size_t r = 0; r < height; ++r) {
      for (size_t i = 0; i < width * nChannel; ++i) {
        *buf8 = invert ? (3 - ((packedBuf[r * bytesPerRow + i / 4] & bitmasks2[i % 4]) >> ((3 - i % 4) * 2)))
                       : (packedBuf[r * bytesPerRow + i / 4] & bitmasks2[i % 4] >> ((3 - i % 4) * 2));
        buf8++;
      }
    }
    return height * width * nChannel;
  }
  if (bitspersample == 4) {
    std::vector<uint8_t> packedBuf(TIFFStripSize(m_tif.get()));
    TIFFReadEncodedStrip(m_tif.get(), strip, packedBuf.data(), static_cast<tmsize_t>(-1));
    uint8_t* buf8 = buf;
    size_t bytesPerRow = (width * nChannel + 1) / 2;
    if (packedBuf.size() < bytesPerRow * height) {
      throw ZException(fmt::format("Not enought strip data, nRows:{}, nChannel:{}, bitsPerSample:{}, strip data:{}",
                                   height,
                                   nChannel,
                                   bitspersample,
                                   packedBuf.size()),
                       ZException::Option::CheckErrno);
    }
    for (size_t r = 0; r < height; ++r) {
      for (size_t i = 0; i < width * nChannel; ++i) {
        *buf8 = invert ? (15 - ((packedBuf[r * bytesPerRow + i / 2] & bitmasks4[i % 2]) >> ((1 - i % 2) * 4)))
                       : (packedBuf[r * bytesPerRow + i / 2] & bitmasks4[i % 2] >> ((1 - i % 2) * 4));
        buf8++;
      }
    }
    return height * width * nChannel;
  }
  throw ZException("should not happen", ZException::Option::CheckErrno);
}

void ZTiff::readTile(uint32_t tile, uint8_t* buf, size_t tileWidth, size_t tileHeight, size_t tileChannel, bool invert)
{
  uint16_t bitspersample;
  TIFFGetFieldDefaulted(m_tif.get(), TIFFTAG_BITSPERSAMPLE, &bitspersample);
  if (bitspersample % 8 == 0) {
    size_t read = TIFFReadEncodedTile(m_tif.get(), tile, buf, static_cast<tmsize_t>(-1));
    if (invert) {
      switch (bitspersample / 8) {
        case 1: {
          uint8_t* pt = buf;
          for (size_t i = 0; i < read; ++i) {
            pt[i] = std::numeric_limits<uint8_t>::max() - pt[i];
          }
        } break;
        case 2: {
          auto pt = reinterpret_cast<uint16_t*>(buf);
          for (size_t i = 0; i < read; ++i) {
            pt[i] = std::numeric_limits<uint16_t>::max() - pt[i];
          }
        } break;
        case 4: {
          auto pt = reinterpret_cast<uint32_t*>(buf);
          for (size_t i = 0; i < read; ++i) {
            pt[i] = std::numeric_limits<uint32_t>::max() - pt[i];
          }
        } break;
        case 8: {
          auto pt = reinterpret_cast<uint64_t*>(buf);
          for (size_t i = 0; i < read; ++i) {
            pt[i] = std::numeric_limits<uint64_t>::max() - pt[i];
          }
        } break;
        default:
          throw ZException(fmt::format("do not support invert {} bytes integer", bitspersample / 8));
      }
    }
  } else if (bitspersample == 1) {
    std::vector<uint8_t> packedBuf(TIFFTileSize(m_tif.get()));
    TIFFReadEncodedTile(m_tif.get(), tile, packedBuf.data(), static_cast<tmsize_t>(-1));
    uint8_t* buf8 = buf;
    size_t bytesPerRow = packedBuf.size() / tileHeight;
    for (size_t r = 0; r < tileHeight; ++r) {
      for (size_t i = 0; i < tileWidth * tileChannel; ++i) {
        *buf8 = invert ? ((packedBuf[r * bytesPerRow + i / 8] & bitmasks1[i % 8]) == 0)
                       : ((packedBuf[r * bytesPerRow + i / 8] & bitmasks1[i % 8]) > 0);
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
        *buf8 = invert ? (3 - ((packedBuf[r * bytesPerRow + i / 4] & bitmasks2[i % 4]) >> ((3 - i % 4) * 2)))
                       : (packedBuf[r * bytesPerRow + i / 4] & bitmasks2[i % 4] >> ((3 - i % 4) * 2));
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
        *buf8 = invert ? (15 - ((packedBuf[r * bytesPerRow + i / 2] & bitmasks4[i % 2]) >> ((1 - i % 2) * 4)))
                       : (packedBuf[r * bytesPerRow + i / 2] & bitmasks4[i % 2] >> ((1 - i % 2) * 4));
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
        auto* des = img.channelData<uint8_t>(c);
        const uint8_t* src = bufImg.channelData<uint8_t>(0) + c;
        size_t numCh = img.numChannels();
        size_t i = 0;
        while (i++ < img.channelVoxelNumber()) {
          *des++ = *src;
          src += numCh;
        }
      } break;
      case 2: {
        auto* des = img.channelData<uint16_t>(c);
        const uint16_t* src = bufImg.channelData<uint16_t>(0) + c;
        size_t numCh = img.numChannels();
        size_t i = 0;
        while (i++ < img.channelVoxelNumber()) {
          *des++ = *src;
          src += numCh;
        }
      } break;
      default: {
        auto* des = img.channelData<uint8_t>(c);
        const uint8_t* src = bufImg.channelData<uint8_t>(0) + c * img.voxelByteNumber();
        size_t voxelByte = img.voxelByteNumber();
        size_t srcStride = img.numChannels() * voxelByte;
        size_t i = 0;
        while (i++ < img.channelVoxelNumber()) {
          std::copy_n(src, voxelByte, des);
          des += voxelByte;
          src += srcStride;
        }
      }
    }
  }
}

void ZTiff::copyOneChannelTileToImg(const uint8_t* tileBuf,
                                    size_t tileWidth,
                                    size_t tileHeight,
                                    size_t voxelByteNumber,
                                    ZImg& img,
                                    size_t xStart,
                                    size_t yStart,
                                    size_t c)
{
  size_t xEnd = std::min(xStart + tileWidth, img.width());
  size_t cpysize = (xEnd - xStart) * voxelByteNumber;
  size_t yEnd = std::min(yStart + tileHeight, img.height());
  for (size_t y = yStart; y < yEnd; ++y) {
    std::copy_n(tileBuf + (y - yStart) * tileWidth * voxelByteNumber, cpysize, img.data<uint8_t>(xStart, y, 0, c));
  }
}

void ZTiff::copyTileToImg(const uint8_t* tileBuf,
                          size_t tileWidth,
                          size_t tileHeight,
                          size_t numChannels,
                          size_t voxelByteNumber,
                          ZImg& img,
                          size_t xStart,
                          size_t yStart)
{
  for (size_t c = 0; c < numChannels; ++c) {
    size_t xEnd = std::min(xStart + tileWidth, img.width());
    size_t yEnd = std::min(yStart + tileHeight, img.height());
    for (size_t x = xStart; x < xEnd; ++x) {
      for (size_t y = yStart; y < yEnd; ++y) {
        auto* des = img.data<uint8_t>(x, y, 0, c);
        const uint8_t* src = tileBuf + (y - yStart) * tileWidth * voxelByteNumber * numChannels +
                             (x - xStart) * voxelByteNumber * numChannels + c * voxelByteNumber;
        std::copy_n(src, voxelByteNumber, des);
      }
    }
  }
}

namespace {

void ensureTiffWriteTileBufferSize(size_t bytes)
{
  if (bytes > static_cast<size_t>(std::numeric_limits<tmsize_t>::max())) {
    throw ZException(fmt::format("TIFF tile buffer is too large to write: {} bytes", bytes));
  }
}

void copyImgChannelToTileBuffer(const ZImg& img,
                                size_t z,
                                size_t t,
                                size_t c,
                                size_t tileX,
                                size_t tileY,
                                size_t tileWidth,
                                size_t tileHeight,
                                std::vector<uint8_t>& tileBuf)
{
  CHECK(!tileBuf.empty());
  std::fill(tileBuf.begin(), tileBuf.end(), uint8_t{0});
  const size_t copyWidth = tileX >= img.width() ? 0 : std::min(tileWidth, img.width() - tileX);
  const size_t copyHeight = tileY >= img.height() ? 0 : std::min(tileHeight, img.height() - tileY);
  const size_t copyBytes = copyWidth * img.voxelByteNumber();
  for (size_t y = 0; y < copyHeight; ++y) {
    const uint8_t* src = img.data<uint8_t>(tileX, tileY + y, z, c, t);
    uint8_t* dst = tileBuf.data() + y * tileWidth * img.voxelByteNumber();
    std::copy_n(src, copyBytes, dst);
  }
}

void copyImgContiguousToTileBuffer(const ZImg& img,
                                   size_t z,
                                   size_t t,
                                   size_t tileX,
                                   size_t tileY,
                                   size_t tileWidth,
                                   size_t tileHeight,
                                   std::vector<uint8_t>& tileBuf)
{
  CHECK(!tileBuf.empty());
  std::fill(tileBuf.begin(), tileBuf.end(), uint8_t{0});
  const size_t copyWidth = tileX >= img.width() ? 0 : std::min(tileWidth, img.width() - tileX);
  const size_t copyHeight = tileY >= img.height() ? 0 : std::min(tileHeight, img.height() - tileY);
  const size_t voxelByteNumber = img.voxelByteNumber();
  const size_t numChannels = img.numChannels();
  for (size_t y = 0; y < copyHeight; ++y) {
    for (size_t x = 0; x < copyWidth; ++x) {
      for (size_t c = 0; c < numChannels; ++c) {
        const uint8_t* src = img.data<uint8_t>(tileX + x, tileY + y, z, c, t);
        uint8_t* dst = tileBuf.data() + ((y * tileWidth + x) * numChannels + c) * voxelByteNumber;
        std::copy_n(src, voxelByteNumber, dst);
      }
    }
  }
}

void writeTileOrThrow(TIFF* tif, uint32_t tile, std::vector<uint8_t>& tileBuf)
{
  ensureTiffWriteTileBufferSize(tileBuf.size());
  if (TIFFWriteEncodedTile(tif, tile, tileBuf.data(), static_cast<tmsize_t>(tileBuf.size())) < 0) {
    throw ZException(fmt::format("Can not write TIFF tile {}", tile), ZException::Option::CheckErrno);
  }
}

void writeTiledIFDData(TIFF* tif,
                       const ZImg& img,
                       size_t z,
                       size_t t,
                       index_t c,
                       bool planarconfigSeparate,
                       size_t tileWidth,
                       size_t tileHeight)
{
  CHECK(tileWidth > 0);
  CHECK(tileHeight > 0);
  const size_t tilesPerRow = (img.width() + tileWidth - 1) / tileWidth;
  const size_t tilesPerCol = (img.height() + tileHeight - 1) / tileHeight;

  if (c < 0) {
    if (planarconfigSeparate || img.numChannels() == 1) {
      std::vector<uint8_t> tileBuf(tileWidth * tileHeight * img.voxelByteNumber());
      for (size_t ch = 0; ch < img.numChannels(); ++ch) {
        for (size_t tileRow = 0; tileRow < tilesPerCol; ++tileRow) {
          for (size_t tileCol = 0; tileCol < tilesPerRow; ++tileCol) {
            const size_t tileX = tileCol * tileWidth;
            const size_t tileY = tileRow * tileHeight;
            copyImgChannelToTileBuffer(img, z, t, ch, tileX, tileY, tileWidth, tileHeight, tileBuf);
            writeTileOrThrow(tif, TIFFComputeTile(tif, tileX, tileY, 0, static_cast<uint16_t>(ch)), tileBuf);
          }
        }
      }
    } else {
      std::vector<uint8_t> tileBuf(tileWidth * tileHeight * img.voxelByteNumber() * img.numChannels());
      for (size_t tileRow = 0; tileRow < tilesPerCol; ++tileRow) {
        for (size_t tileCol = 0; tileCol < tilesPerRow; ++tileCol) {
          const size_t tileX = tileCol * tileWidth;
          const size_t tileY = tileRow * tileHeight;
          copyImgContiguousToTileBuffer(img, z, t, tileX, tileY, tileWidth, tileHeight, tileBuf);
          writeTileOrThrow(tif, TIFFComputeTile(tif, tileX, tileY, 0, 0), tileBuf);
        }
      }
    }
    return;
  }

  std::vector<uint8_t> tileBuf(tileWidth * tileHeight * img.voxelByteNumber());
  for (size_t tileRow = 0; tileRow < tilesPerCol; ++tileRow) {
    for (size_t tileCol = 0; tileCol < tilesPerRow; ++tileCol) {
      const size_t tileX = tileCol * tileWidth;
      const size_t tileY = tileRow * tileHeight;
      copyImgChannelToTileBuffer(img, z, t, static_cast<size_t>(c), tileX, tileY, tileWidth, tileHeight, tileBuf);
      writeTileOrThrow(tif, TIFFComputeTile(tif, tileX, tileY, 0, 0), tileBuf);
    }
  }
}

void writeStripIFDData(TIFF* tif, const ZImg& img, size_t z, size_t t, index_t c, bool planarconfigSeparate)
{
  if (c < 0) {
    if (planarconfigSeparate || img.numChannels() == 1) {
      for (size_t ch = 0; ch < img.numChannels(); ++ch) {
        TIFFWriteEncodedStrip(tif, ch, const_cast<uint8_t*>(img.planeData(z, ch, t)), img.planeByteNumber());
      }
    } else {
      CHECK(z == 0 && t == 0 && c < 0 && img.numTimes() == 1 && img.depth() == 1 &&
            (img.numChannels() == 4 || img.numChannels() == 3));
      ZImg tmp(img.info());
      CHECK(tmp.channelData<uint8_t>(0) != img.channelData<uint8_t>(0)) << img.info();
      ZImgFormat::XYZCtoCXYZ(img, tmp);
      TIFFWriteEncodedStrip(tif, 0, tmp.planeData(0, 0, 0), tmp.byteNumber());
    }
  } else {
    TIFFWriteEncodedStrip(tif, 0, const_cast<uint8_t*>(img.planeData(z, c, t)), img.planeByteNumber());
  }
}

} // namespace

ZTiffWriter::ZTiffWriter()
  : m_tif(nullptr, TIFFClose)
{}

void ZTiffWriter::startWriting(const QString& filename, Compression comp, int32_t extraSample, bool bigTiff)
{
  m_tif.reset();

  if (comp == Compression::AUTO) {
    comp = defaultCompression(nullptr);
  } else {
    if (!checkCompression(nullptr, comp)) {
      LOG(WARNING) << fmt::format(
        "Compression {} is not supported or not applicable, switching to default compression.",
        comp);
      comp = defaultCompression(nullptr);
    }
  }

  m_compression = comp;
  m_extraSample = extraSample;

  TIFFSetWarningHandler(nullptr);
  TIFFSetErrorHandler(LibtiffErrorHandler);
#if defined(_WIN32) || defined(_WIN64)
  m_tif.reset(TIFFOpenW(filename.toStdWString().c_str(), bigTiff ? "w8" : "w"));
#else
  m_tif.reset(TIFFOpen(QFile::encodeName(filename).constData(), bigTiff ? "w8" : "w"));
#endif
  if (!m_tif) {
    throw ZException(fmt::format("Can't open {} for writing", filename), ZException::Option::CheckErrno);
  }
}

void ZTiffWriter::writeIFD(const ZImg& img,
                           size_t z,
                           size_t t,
                           index_t c,
                           bool writeThumbnails,
                           const std::vector<ZImgMetatag>& additionalTags,
                           const std::vector<ZImg>* explicitSubIFDs,
                           size_t tileWidth,
                           size_t tileHeight)
{
  CHECK(m_tif);
  if ((tileWidth == 0) != (tileHeight == 0)) {
    throw ZException("TIFF tiled writing requires both tile width and tile height");
  }
  if (tileWidth > std::numeric_limits<uint32_t>::max() || tileHeight > std::numeric_limits<uint32_t>::max()) {
    throw ZException(fmt::format("TIFF tile dimensions are too large: {}x{}", tileWidth, tileHeight));
  }
  const bool writeTiled = tileWidth > 0 && tileHeight > 0;
  for (const auto& atag : additionalTags) {
    if (atag.tag() > 0 && atag.tag() < 65535) {
      TIFFSetField(m_tif.get(), atag.tag(), atag.dataArray());
    }
  }

  bool planarconfigSeparate = true;
  if (c >= 0 || img.numChannels() == 1) {
    planarconfigSeparate = false; // only one channel
  }

  // for 2D image with 3 or 4 channel, save it without planarconfigSeparate so Pillow can read it correctly
  if (z == 0 && t == 0 && c < 0 && img.numTimes() == 1 && img.depth() == 1 &&
      (img.numChannels() == 4 || img.numChannels() == 3)) {
    planarconfigSeparate = false;
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
  TIFFSetField(m_tif.get(), TIFFTAG_PLANARCONFIG, planarconfigSeparate ? PLANARCONFIG_SEPARATE : PLANARCONFIG_CONTIG);
  TIFFSetField(m_tif.get(), TIFFTAG_PHOTOMETRIC, photo);
  TIFFSetField(m_tif.get(), TIFFTAG_COMPRESSION, getTiffCompressionTag(m_compression));
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
  if (writeTiled) {
    TIFFSetField(m_tif.get(), TIFFTAG_TILEWIDTH, static_cast<uint32_t>(tileWidth));
    TIFFSetField(m_tif.get(), TIFFTAG_TILELENGTH, static_cast<uint32_t>(tileHeight));
  } else {
    TIFFSetField(m_tif.get(), TIFFTAG_ROWSPERSTRIP, img.height());
  }
  TIFFSetField(m_tif.get(), TIFFTAG_SAMPLEFORMAT, img.voxelFormat());
  std::vector<ZImg> emptyList;
  const std::vector<ZImg>* reducedIFDs = explicitSubIFDs ? explicitSubIFDs : &emptyList;
  if (!explicitSubIFDs && writeThumbnails) {
    reducedIFDs = &(img.thumbnail().planeAttachments(z, t));
  }
  if (!reducedIFDs->empty()) {
    for (const ZImg& reducedIFD : *reducedIFDs) {
      if (reducedIFD.depth() != 1 || reducedIFD.numTimes() != 1) {
        throw ZException(fmt::format("TIFF SubIFD must be one 2D plane, got {}", reducedIFD.info()));
      }
      const size_t parentSamples = c < 0 ? img.numChannels() : 1;
      if (reducedIFD.numChannels() != parentSamples) {
        throw ZException(fmt::format("TIFF SubIFD channel count {} does not match parent IFD sample count {}",
                                     reducedIFD.numChannels(),
                                     parentSamples));
      }
      if (reducedIFD.voxelByteNumber() != img.voxelByteNumber() || reducedIFD.voxelFormat() != img.voxelFormat()) {
        throw ZException(
          fmt::format("TIFF SubIFD pixel type <{}> does not match parent image <{}>", reducedIFD.info(), img.info()));
      }
    }
    std::vector<toff_t> subIFDOffsets(reducedIFDs->size());
    TIFFSetField(m_tif.get(), TIFFTAG_SUBIFD, reducedIFDs->size(), subIFDOffsets.data());
  }

  if (writeTiled) {
    writeTiledIFDData(m_tif.get(), img, z, t, c, planarconfigSeparate, tileWidth, tileHeight);
  } else {
    writeStripIFDData(m_tif.get(), img, z, t, c, planarconfigSeparate);
  }

  TIFFWriteDirectory(m_tif.get());

  for (const auto& thumbnail : *reducedIFDs) {
    TIFFSetField(m_tif.get(), TIFFTAG_SUBFILETYPE, FILETYPE_REDUCEDIMAGE);
    TIFFSetField(m_tif.get(), TIFFTAG_IMAGEWIDTH, thumbnail.width());
    TIFFSetField(m_tif.get(), TIFFTAG_IMAGELENGTH, thumbnail.height());
    TIFFSetField(m_tif.get(), TIFFTAG_BITSPERSAMPLE, thumbnail.voxelByteNumber() * 8);
    TIFFSetField(m_tif.get(), TIFFTAG_SAMPLESPERPIXEL, thumbnail.numChannels());
    TIFFSetField(m_tif.get(), TIFFTAG_COMPRESSION, getTiffCompressionTag(m_compression));
    TIFFSetField(m_tif.get(), TIFFTAG_PHOTOMETRIC, photo);
    TIFFSetField(m_tif.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_SEPARATE);
    if (writeTiled) {
      TIFFSetField(m_tif.get(), TIFFTAG_TILEWIDTH, static_cast<uint32_t>(tileWidth));
      TIFFSetField(m_tif.get(), TIFFTAG_TILELENGTH, static_cast<uint32_t>(tileHeight));
    } else {
      TIFFSetField(m_tif.get(), TIFFTAG_ROWSPERSTRIP, thumbnail.height());
    }
    TIFFSetField(m_tif.get(), TIFFTAG_SAMPLEFORMAT, thumbnail.voxelFormat());
    if (writeTiled) {
      writeTiledIFDData(m_tif.get(), thumbnail, 0, 0, -1, true, tileWidth, tileHeight);
    } else {
      writeStripIFDData(m_tif.get(), thumbnail, 0, 0, -1, true);
    }
    TIFFWriteDirectory(m_tif.get());
  }
}

Compression ZTiffWriter::defaultCompression(const ZImg* img)
{
  constexpr Compression list[] = {Compression::LZW, Compression::ADOBE_DEFLATE, Compression::PACKBITS};
  for (auto comp : list) {
    if (checkCompression(img, comp)) {
      return comp;
    }
  }
  return Compression::NONE;
}

bool ZTiffWriter::checkCompression(const ZImg*, Compression comp)
{
  if (comp == Compression::NONE) {
    return true;
  }
  // check exist first
  if (TIFFIsCODECConfigured(getTiffCompressionTag(comp)) != 1) {
    return false;
  }
  //  if (comp == Compression::CCITTFAX3 ||
  //      comp == Compression::CCITTFAX4 ||
  //      comp == Compression::CCITTRLE ||
  //      comp == Compression::CCITTRLEW ||
  //      comp == Compression::CCITT_T4 ||
  //      comp == Compression::CCITT_T6)
  //    return false;
  return true;
}

} // namespace nim
