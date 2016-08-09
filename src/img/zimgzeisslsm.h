#pragma once

#include "zimgtiff.h"
#include <map>

namespace nim {

#pragma pack(push, 1)
struct CZ_LsmInfo {
  uint32_t u32MagicNumber;      //0x00300494C (release 1.3) or 0x00400494C (release 1.5 to 6.0).
  int32_t s32StructureSize;     //Number of bytes in the structure.
  int32_t s32DimensionX;        //Number of intensity values in x-direction.
  int32_t s32DimensionY;        //Number of intensity values in y-direction.
  int32_t s32DimensionZ;        //Number of intensity values in z-direction or in case of scan mode "Time Series Mean-of-ROIs" the Number of ROIs.
  int32_t s32DimensionChannels; //Number of channels.
  int32_t s32DimensionTime;     //Number of intensity values in time-direction.
  int32_t s32DataType;          //Format of the intensity values:
                                //1 for 8-bit unsigned integer,
                                //2 for 12-bit unsigned integer and
                                //5 for 32-bit float (for "Time Series Mean-of-ROIs" or topography height map images)
                                //0 in case of different data types for different channels. In the latter case the field 32OffsetChannelDataTypes contains further information.
  int32_t s32ThumbnailX;        //Width in pixels of a thumbnail.
  int32_t s32ThumbnailY;        //Height in pixels of a thumbnail.
  double f64VoxelSizeX;         //Distance of the pixels in x-direction in meter.
  double f64VoxelSizeY;         //Distance of the pixels in y-direction in meter.
  double f64VoxelSizeZ;         //Distance of the pixels in z-direction in meter.
  double f64OriginX;            //The x-offset of the center of the image in meter.
                                //relative to the optical axis. For LSM images the x-direction is the direction of the x-scanner.
                                //For cameras it is the CCD horizontal direction (depending on how camera is aligned/mounted)
                                //In releases prior 4.0 the entry was not used and the value 0 was written instead.
  double f64OriginY;            //The y-offset of the center of the image in meter.
                                //relative to the optical axis. For LSM images the y-direction is the direction of the y-scanner.
                                //For cameras it is the CCD vertical direction (depending on how camera is aligned/mounted).
                                //In releases prior 4.0 the entry was not used and the value 0 was written instead.
  double f64OriginZ;            //Not used
  uint16_t u16ScanType;         //Scan type:
                                //0 - normal x-y-z-scan
                                //1 - z-Scan (x-z-plane)
                                //2 - line scan
                                //3 - time series x-y
                                //4 - time series x-z (release 2.0 or later)
                                //5 - time series "Mean of ROIs" (release 2.0 or later)
                                //6 - time series x-y-z (release 2.3 or later)
                                //7 - spline scan (release 2.5 or later)
                                //8 - spline plane x-z (release 2.5 or later)
                                //9 - time series spline plane x-z (release 2.5 or later)
                                //10 - point mode (release 3.0 or later)
  uint16_t u16SpectralScan;     //Spectral scan flag:
                                //0 - no spectral scan.
                                //1 - image has been acquired in spectral scan mode with a LSM 510 META or LSM 710 QUASAR detector (release 3.0 or later).
  uint32_t u32DataType;         //Data type:
                                //0 - Original scan data
                                //1 - Calculated data
                                //2 - 3D reconstruction
                                //3 - Topography height map
  uint32_t u32OffsetVectorOverlay; //File offset to the description of the vector overlay (can be 0, if not present).
  uint32_t u32OffsetInputLut;    //File offset to the channel input LUT with brightness and contrast properties (can be 0, if not present).
  uint32_t u32OffsetOutputLut;   //File offset to the color palette (can be 0, if not present).
  uint32_t u32OffsetChannelColors; //File offset to the list of channel colors and channel names (can be 0, if not present).
  double f64TimeInterval;       //Time interval for time series in "s" (can be 0, if it is not a time series or if there is more detailed information in u32OffsetTimeStamps).
  uint32_t u32OffsetChannelDataTypes; //File offset to an array with uint32_t-values with the format of the intensity values for the respective channels (can be 0, if not present).
                                //The contents of the array elements are
                                //  1 - for 8-bit unsigned integer,
                                //  2 - for 12-bit unsigned integer and
                                //  5 - for 32-bit float (for "Time Series Mean-of-ROIs" ).
  uint32_t u32OffsetScanInformation; //File offset to a structure with information of the device settings used to scan the image (can be 0, if not present).
  uint32_t u32OffsetKsData;     //File offset to "Zeiss Vision KS-3D" specific data (can be 0, if not present).
  uint32_t u32OffsetTimeStamps; //File offset to a structure containing the time stamps for the time indexes (can be 0, if it is not a time series).
  uint32_t u32OffsetEventList;  //File offset to a structure containing the experimental notations recorded during a time series (can be 0, if not present).
  uint32_t u32OffsetRoi;        //File offset to a structure containing a list of the ROIs used during the scan operation (can be 0, if not present).
  uint32_t u32OffsetBleachRoi;  //File offset to a structure containing a description of the bleach region used during the scan operation (can be 0, if not present).
  uint32_t u32OffsetNextRecording; //For "Time Series Mean-of-ROIs" and for "Line scans" it is possible that a second image is stored in the file (can be 0, if not present).
                                //For "Time Series Mean-of-ROIs" it is an image with the ROIs. For "Line scans" it is the image with the selected line.
                                //In these cases u32OffsetNextRecording contains a file offset to a second file header.
                                //This TIFF-header and all sub-structures are built exactly the same way as a simple LSM 5/7 file.
                                //All offsets without exception are given there relative to the start of the second TIFF-header.
  double f64DisplayAspectX;     //Zoom factor for the image display in x-direction (0.0 for release 2.3 and earlier).
  double f64DisplayAspectY;     //Zoom factor for the image display in y-direction (0.0 for release 2.3 and earlier).
  double f64DisplayAspectZ;     //Zoom factor for the image display in z-direction (0.0 for release 2.3 and earlier).
  double f64DisplayAspectTime;  //Zoom factor for the image display in time-direction (0.0 for release 2.3 and earlier).
  uint32_t u32OffsetMeanOfRoisOverlay; //File offset to the description of the vector overlay with the ROIs used during a scan in "Mean of ROIs" mode (can be 0, if not present).
  uint32_t u32OffsetTopoIsolineOverlay; //File offset to the description of the vector overlay for the topography-iso-lines and height display with the profile selection line (can be 0, if not present).
  uint32_t u32OffsetTopoProfileOverlay; //File offset to the description of the vector overlay for the topography-profile display (can be 0, if not present).
  uint32_t u32OffsetLinescanOverlay; //File offset to the description of the vector overlay for the line scan line selection with the selected line or Bezier curve (can be 0, if not present).
  uint32_t u32ToolbarFlags;     //Bit-field for disabled toolbar buttons:
                                //bit 0 - "Corp" button
                                //bit 1 - "Reuse" button.
                                //If the bit is set the corresponding button is disabled.
  uint32_t u32OffsetChannelWavelength; //Offset to memory block with the wavelength range used during acquisition for the individual channels (new for release 3.0; can be 0, if not present).
  uint32_t u32OffsetChannelFactors; // Offset to memory block with scaling factor, offset and unit for each image channel. The data are currently used by images with ion concentration data.
                                // The display value is calculated by
                                //DisplayValue = Factor * PixelIntensity / MaxPixelIntensity + Offset.
                                //where "MaxPixelIntensity" is the maximum possible pixel intensity for the data type (4095 or 255).
                                //The parameters are stored in an array of structures LSMCHANNELFACTORS (24 bytes per channel) with the members:
                                //    FLOAT64 f64Factor;
                                //    FLOAT64 f64Offset;
                                //    UINT32 u32Unit; // eUnitNone " unknown or eUnitConcentration - Ion concentration in mol
                                //    UINT32 u32Reserved [3];
  double f64ObjectiveSphereCorrection; //The inverse radius of the spherical error of the objective that was used during acquisition.
                                //This is the radius of the sphere that can be fitted on the topography reconstruction of an absolutely plane object that has been recorded with the objective.
  uint32_t u32OffsetUnmixParameters; //File offset to the parameters for linear unmixing that have been used to generate the image data from scan data of the spectral detector
                                //(new for release 3.2; can be 0, if not present).
  uint32_t u32OffsetAcquisitionParameters; //File offset to a block with acquisition parameters for support of the re-use function of the LSM 5/7 program (new for release 3.5; can be 0, if not present).
  uint32_t u32OffsetCharacteristics; //File offset to a block with user specified properties (new for release 3.5; can be 0, if not present).
  uint32_t u32OffsetPalette;         //File offset to a block with detailed color palette properties (new for release 3.5; can be 0, if not present).
  double f64TimeDifferenceX;    //The time difference for the acquisition of adjacent pixels in x-direction in seconds. The property is used by RICS analysis (new for release 5.0; can be 0, if not present).
  double f64TimeDifferenceY;    //The time difference for the acquisition of adjacent pixels in y-direction in seconds. The property is used by RICS analysis (new for release 5.0; can be 0, if not present).
  double f64TimeDifferenceZ;    //The time difference for the acquisition of adjacent pixels in z-direction in seconds. The property is used by RICS analysis (new for release 5.0; can be 0, if not present).
  uint32_t u32InternalUse1;     //Reserved for internal use. Writer should set this field to 0.
  int32_t s32DimensionP;        //Number of intensity values in position-direction. (new for release 5.5; can be 0, if not present).
  int32_t s32DimensionM;        //Number of intensity values in tile (mosaic)-direction.(new for release 5.5; can be 0, if not present).
  int32_t s32DimensionsReserved[16]; //16 reserved 32-bit words, must be 0.
  uint32_t u32OffsetTilePositions;   //File offset to a block with the positions of the tiles in (new for release 5.5; can be 0, if not present).
  uint32_t u32Reserved[9];      //9 reserved 32-bit words, must be 0.
  uint32_t u32OffsetPositions;  //File offset to a block with the positions of the acquisition regions
                                //(new in release 6.2, can be 0, if not present, release 6.2 is NOT released yet! beta and special built only).
  uint32_t u32Reserved2[21];    //21 reserved 32-bit words, must be 0.
};

struct CZ_ChannelColors {
  int32_t s32BlockSize;         // Size of the structure in bytes including the name strings and colors.
  int32_t s32NumberColors;      // Number of colors in the color array; should be the same as the number of channels.
  int32_t s32NumberNames;       // Number of character strings for the channel names; should be the same as the number of channels.
  int32_t s32ColorsOffset;      // Offset relative ti the start of the structure to the "uint32_t" array of channel colors.
                                // Each array entry contains a color with intensity values in the range 0..255 for the three color components
  int32_t s32NamesOffset;       // Offset relative ti the start of the structure to the list of channel names. The list of channel names is a series of "\0"-terminated ANSI character strings.
  int32_t s32Mono;              // If unequal zero the "Mono" button in the LSM-imagefenster window was peressed
  int32_t s32Reserved[4];       // Four 32-bit words reserved for use in the future. The values are set to "0".
};

struct CZ_TimeStamps {
  int32_t s32Size;              // Size, in bytes, of the whole block used for time stamps.
  int32_t s32NumberTimeStamps;	// Number of time stamps in the following list.
};
#pragma pack(pop)

// Data types
#define TYPE_SUBBLOCK 0
#define TYPE_ASCII    2
#define TYPE_LONG     4
#define TYPE_RATIONAL 5

//Every subblock starts with a list entry of the type TYPE_SUBBLOCK. The u32Entry field can be:

#define SUBBLOCK_RECORDING             0x10000000
#define SUBBLOCK_TRACKS				         0x20000000
#define SUBBLOCK_LASERS                0x30000000
#define SUBBLOCK_TRACK 				         0x40000000
#define SUBBLOCK_LASER				         0x50000000
#define SUBBLOCK_DETECTION_CHANNELS    0x60000000
#define SUBBLOCK_DETECTION_CHANNEL 	   0x70000000
#define SUBBLOCK_ILLUMINATION_CHANNELS 0x80000000
#define SUBBLOCK_ILLUMINATION_CHANNEL  0x90000000
#define SUBBLOCK_BEAM_SPLITTERS		     0xA0000000
#define SUBBLOCK_BEAM_SPLITTER 		     0xB0000000
#define SUBBLOCK_DATA_CHANNELS	       0xC0000000
#define SUBBLOCK_DATA_CHANNEL          0xD0000000
#define SUBBLOCK_TIMERS    		         0x11000000
#define SUBBLOCK_TIMER      		    	 0x12000000
#define SUBBLOCK_MARKERS               0x13000000
#define SUBBLOCK_MARKER                0x14000000
#define SUBBLOCK_END                   0xFFFFFFFF


struct CZ_ScanInformation {
  uint32_t u32Entry; // A value that specifies which data are stored
  uint32_t u32Type;  //	A value that specifies the type of the data stored in the "Varaibable length data" field.
                     // TYPE_SUBBLOCK	- start or end of a subblock
                     // TYPE_LONG		  - 32 bit signed integer
                     // TYPE_RATIONAL - 64 bit floatingpoint
                     // TYPE_ASCII 		- zero terminated string.
  uint32_t u32Size;	 // Size, in bytes, of the "Varaibable length data" field.
};

class ZImgZeissLsm : public ZImgTiff
{
public:
  ZImgZeissLsm();

  virtual ~ZImgZeissLsm()
  {}

  // ZImgFormat interface
public:
  virtual QString shortName() const override;

  virtual QString fullName() const override;

  virtual QStringList extensions() const override;

  virtual FileFormat format() const override
  { return FileFormat::ZeissLsm; }

  virtual bool supportRead() const override;

  virtual bool supportWrite() const override;

  // ZImgTiff interface
protected:
  virtual void readIntoInternalStructure(const QString& filename, ZTiff& tiff) override;

  virtual void clearInternalState() override;

  virtual void detectImgInfo(ZTiff& tiff) override;

protected:
  void readLsmInfo(const QString& filename, ZTiff& tiff);

  void logLsmInfo(const QString& filename);

protected:
  CZ_LsmInfo m_lsmInfo;
  CZ_TimeStamps m_lsmTimeStamps;
  CZ_ChannelColors m_lsmChannelColors;
  std::vector<uint32_t> m_channelDataTypes;
  std::vector<Location> m_positions;
  std::vector<Location> m_tilePositions;
  ZImgInfo m_lsmImgInfo;
  size_t m_numScenes;
};

} // namespace

