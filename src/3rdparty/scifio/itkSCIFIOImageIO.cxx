/*=========================================================================
 *
 *  Copyright Insight Software Consortium
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0.txt
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *=========================================================================*/

#include "itkSCIFIOImageIO.h"
#include "itkIOCommon.h"
#include "itkMacro.h"
#include "itkMetaDataObject.h"

#include "zlog.h"
#include "zglobal.h"

#include <cstdio>
#include <cstdlib>

#include <cmath>
#include <fstream>
#include <string>
#include <sstream>

#ifdef _WIN32
#define SCIFIO_SEP ";"
#include <io.h>
#include <fcntl.h>
#include <process.h>
#include <cmath>
#else
#define SCIFIO_SEP ":"

#include <unistd.h>

#endif

namespace {
void checkLength(long length, double spacing,
                 std::vector<long>& lengthVec, std::vector<double>& spacingVec)
{
  if (length > 1 || lengthVec.size() > 0) {
    if (spacing <= 0.0) spacing = 1.0;

    lengthVec.push_back(length);
    spacingVec.push_back(spacing);
  }
}

/*
 * Splits a string into tokens using the given delimiter.
 *
 * Thanks to SO #236129 for this solution:
 * http://stackoverflow.com/a/236803
 */
[[maybe_unused]] std::vector<std::string>& split(const std::string& s, char delim, std::vector<std::string>& elems)
{
  std::stringstream ss(s);
  std::string item;
  while (std::getline(ss, item, delim)) {
    elems.push_back(item);
  }
  return elems;
}

/*
 * this signature not needed
  std::vector<std::string> split( const std::string &s, char delim )
  {
    std::vector<std::string> elems;
    return split(s, delim, elems);
  }
*/
}

namespace itk {
template<typename ReturnType>
ReturnType valueOfString(const std::string& s)
{
  ReturnType res;
  if (!(std::istringstream(s) >> res)) {
    itkGenericExceptionMacro(<<"SCIFIOImageIO: error while converting: " << s);
  }
  return res;
}


template<typename T>
T GetTypedMetaData(MetaDataDictionary dict, std::string key)
{
  std::string tmp;
  ExposeMetaData<std::string>(dict, key, tmp);
  return valueOfString<T>(tmp);
}


template<>
bool valueOfString<bool>(const std::string& s)
{
  std::stringstream ss;
  ss << s;
  bool res = false;
  ss >> res;
  if (ss.fail()) {
    ss.clear();
    ss >> std::boolalpha >> res;
  }
  return res;
}


template<typename T>
std::string toString(const T& Value)
{
  std::ostringstream oss;
  oss << Value;
  return oss.str();
}

// Read until we get two newlines. Returns everything read until that point
std::string SCIFIOImageIO::WaitForNewLines(int pipedatalength)
{
  char* pipedata;
  std::string readBack;
  size_t findStart = 0;
  bool keepReading = true;
  std::string errorMessage;
  while (keepReading) {
    int retcode = itksysProcess_WaitForData(m_Process, &pipedata, &pipedatalength, nullptr);
    if (retcode == itksysProcess_Pipe_STDOUT) {
      readBack += std::string(pipedata, pipedatalength);

      // Remove any \r so that we only dealing with unix-style line endings
      for (size_t backslashRIndex = readBack.find('\r', findStart);
           backslashRIndex != std::string::npos;
           findStart = backslashRIndex) {
        readBack.erase(backslashRIndex, 1);
      }

      // if the two last char are "\n\n", then we're done
      if (readBack.size() >= 2 && readBack.substr(readBack.size() - 2, 2) == "\n\n") {
        keepReading = false;
      }
    } else if (retcode == itksysProcess_Pipe_STDERR) {
      std::string message(pipedata, pipedatalength);
      VLOG(1) << "Got error message: " << message;
      CheckError(message);
      errorMessage += message;
    } else {
      DestroyJavaProcess();
      itkExceptionMacro(<<"SCIFIOImageIO exited abnormally. " << errorMessage);
    }
  }

  return readBack;
}

void SCIFIOImageIO::CheckError(std::string message)
{
  if (message.size() >= 16 && message.substr(0, 16).compare("Caught exception") == 0) {
    LOG(ERROR) << "SCIFIOITKBridge caught exception: " << message;
    DestroyJavaProcess();
    //exit(1);
    itkExceptionMacro(<<"SCIFIOImageIO exited abnormally. ");
  } else if (message.size() >= 15 && message.substr(0, 15).compare("Command failure") == 0) {
    LOG(ERROR) << "SCIFIOITKBridge command failed with message: " << message;
    DestroyJavaProcess();
    //exit(1);
    itkExceptionMacro(<<"SCIFIOImageIO exited abnormally. ");
  }
}

std::string SCIFIOImageIO::FindDimensionOrder(const ImageIORegion& region)
{
  std::string command;

  // calculate max sizes. Used to determine dimension order as well.
  std::vector<long> maxSizes;
  MetaDataDictionary& dict = this->GetMetaDataDictionary();

  long sizeX = 1, sizeY = 1, sizeZ = 1, sizeT = 1, sizeC = 1;

  if (dict.HasKey("SizeX")) sizeX = GetTypedMetaData<long>(dict, "SizeX");
  if (dict.HasKey("SizeY")) sizeY = GetTypedMetaData<long>(dict, "SizeY");
  if (dict.HasKey("SizeZ")) sizeZ = GetTypedMetaData<long>(dict, "SizeZ");
  if (dict.HasKey("SizeT")) sizeT = GetTypedMetaData<long>(dict, "SizeT");
  if (dict.HasKey("SizeC")) sizeC = GetTypedMetaData<long>(dict, "SizeC");

  maxSizes.push_back(sizeX);
  maxSizes.push_back(sizeY);
  maxSizes.push_back(sizeZ);
  maxSizes.push_back(sizeT);
  maxSizes.push_back(sizeC);

  int maxSizeIndex = 0;
  for (unsigned int regionIndex = 0; regionIndex < region.GetImageDimension() && maxSizeIndex < 5; regionIndex++) {
    int offset = region.GetIndex(regionIndex);
    int length = region.GetSize(regionIndex);

    command += "\t";
    command += toString(offset);
    command += "\t";
    command += toString(length);
    maxSizeIndex++;
  }

  for (; maxSizeIndex < 5; maxSizeIndex++) {
    command += "\t";
    command += toString(0);
    command += "\t";
    command += toString(maxSizes.at(maxSizeIndex));
  }

  return command;
}

bool SCIFIOImageIO::CheckJavaPath(std::string javaHome, std::string& javaCmd)
{
  std::vector<std::string> javaCmdPath;
#if defined(_WIN32)
  javaCmd = "javaw.exe";
#else
  javaCmd = "java";
#endif
  javaCmdPath.emplace_back(""); // NB: JoinPath skips the first one (why?).
  javaCmdPath.push_back(javaHome);
  javaCmdPath.emplace_back("bin");
  javaCmdPath.push_back(javaCmd);
  std::string path = itksys::SystemTools::JoinPath(javaCmdPath);
  if (itksys::SystemTools::FileExists(path, false)) {
    javaCmd = path;
    return true;
  }
  // Else assuming Java is on the path
  return false;
}

std::string SCIFIOImageIO::RemoveFinalSlash(std::string path) const
{
  if (!path.empty() && (path[path.size() - 1] == '/' || path[path.size() - 1] == '\\')) {
    path.resize(path.size() - 1);
  }
  return path;
}

SCIFIOImageIO::SCIFIOImageIO() : m_Argv(0)
{
  this->m_FileType = IOFileEnum::Binary;

  // determine Java classpath from SCIFIO_PATH environment variable
  std::string scifioPath = nim::ZGlobal::jarsDIR.toStdString();
  if (!itksys::SystemTools::FileExists(scifioPath.c_str(), false)) {
    itkExceptionMacro("SCIFIO_PATH is not set. " << "This environment variable must point to the "
                                                 << "directory containing the SCIFIO JAR files");
  }

  std::vector<std::string> packageCmdPath;

  std::string bioformatsPackagePath = scifioPath + "/" + "bioformats_package.jar";
  std::string scifioITKBridgePath = scifioPath + "/" + "scifio-itk-bridge.jar";
  std::string classpath = bioformatsPackagePath + SCIFIO_SEP + scifioITKBridgePath;

// determine path to java executable from JAVA_HOME, if available
#if defined(_WIN32)
  std::string javaCmd = "javaw.exe";
#else
  std::string javaCmd = "java";
#endif
  std::string javaHome = nim::ZGlobal::jdkDIR.toStdString();
  if (!CheckJavaPath(javaHome, javaCmd)) {
    javaHome = "";
  }
  if (javaHome.empty()) {
    VLOG(1) << "SCIFIO: JAVA_HOME not set; assuming Java is on the path";
  }
// use the appropriate java command
  m_Args.push_back(javaCmd);

// allocate 256MB by default (can be overridden using JAVA_FLAGS variable)
  m_Args.emplace_back("-Xmx256m");

// run headless, to avoid any problems with AWT
  m_Args.emplace_back("-Djava.awt.headless=true");

// append Java classpath
  m_Args.emplace_back("-cp");
  m_Args.push_back(classpath);

// append any user-given parameters
  // std::string javaFlags = getEnv("JAVA_FLAGS");
  // split(javaFlags, ' ', m_Args);

// append the name of the main class to execute
  m_Args.emplace_back("io.scif.itk.SCIFIOITKBridge");

// append the command to pass to the ITK bridge
  m_Args.emplace_back("waitForInput");

// output the full Java command line, for debugging
  VLOG(1) << "-- JAVA COMMAND --";
  for (unsigned int i = 0; i < m_Args.size(); ++i) {
    VLOG(1) << "\t" << m_Args.at(i);
  }

// convert to something usable by itksys
  delete[] m_Argv;
  m_Argv = toCArray(m_Args);
  m_Process = nullptr;
}


void SCIFIOImageIO::CreateJavaProcess()
{
  if (m_Process) {
    // process is still there
    if (itksysProcess_GetState(m_Process) == itksysProcess_State_Executing) {
      // already created and running - just return
      return;
    } else {
      // still there but not running.
      // destroy it cleanly and continue with the creation process
      DestroyJavaProcess();
    }
  }

#ifdef _WIN32
  SECURITY_ATTRIBUTES saAttr;
  saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
  saAttr.bInheritHandle = TRUE;
  saAttr.lpSecurityDescriptor = NULL;

 if( !CreatePipe( &(m_Pipe[0]), &(m_Pipe[1]), &saAttr, 0) )
   itkExceptionMacro(<<"createpipe() failed");
 if ( ! SetHandleInformation(m_Pipe[1], HANDLE_FLAG_INHERIT, 0) )
   itkExceptionMacro(<<"set inherited failed");
#else
  const int pipeResult = pipe(m_Pipe);
  if (pipeResult != 0) {
    itkExceptionMacro(<<"Error with SCIFIOImageIO pipe.");
  }
#endif

  m_Process = itksysProcess_New();
  itksysProcess_SetCommand(m_Process, m_Argv);
  itksysProcess_SetPipeNative(m_Process, itksysProcess_Pipe_STDIN, m_Pipe);

  itksysProcess_Execute(m_Process);

  int state = itksysProcess_GetState(m_Process);
  switch (state) {
    case itksysProcess_State_Exited: {
      int retCode = itksysProcess_GetExitValue(m_Process);
      itkExceptionMacro(<<"SCIFIOImageIO: ITKReadImageInformation exited with return value: " << retCode);
      break;
    }
    case itksysProcess_State_Error: {
      std::string msg = itksysProcess_GetErrorString(m_Process);
      itkExceptionMacro(<<"SCIFIOImageIO: ITKReadImageInformation error:" << std::endl << msg);
      break;
    }
    case itksysProcess_State_Exception: {
      std::string msg = itksysProcess_GetExceptionString(m_Process);
      itkExceptionMacro(<<"SCIFIOImageIO: ITKReadImageInformation exception:" << std::endl << msg);
      break;
    }
    case itksysProcess_State_Executing: {
      // this is the expected state
      break;
    }
    case itksysProcess_State_Expired: {
      itkExceptionMacro(<<"SCIFIOImageIO: internal error: ITKReadImageInformation expired.");
      break;
    }
    case itksysProcess_State_Killed: {
      itkExceptionMacro(<<"SCIFIOImageIO: internal error: ITKReadImageInformation killed.");
      break;
    }
    case itksysProcess_State_Disowned: {
      itkExceptionMacro(<<"SCIFIOImageIO: internal error: ITKReadImageInformation disowned.");
      break;
    }
//     case kwsysProcess_State_Starting:
//       {
//       break;
//       }
    default: {
      itkExceptionMacro(<<"SCIFIOImageIO: internal error: ITKReadImageInformation is in unknown state.");
      break;
    }
  }
}


SCIFIOImageIO::~SCIFIOImageIO()
{
  DestroyJavaProcess();
  delete[] m_Argv;
}


void SCIFIOImageIO::DestroyJavaProcess()
{
  if (m_Process == nullptr) {
    // nothing to destroy
    return;
  }

  if (itksysProcess_GetState(m_Process) == itksysProcess_State_Executing) {
    VLOG(1) << "SCIFIOImageIO::DestroyJavaProcess killing java process";
    itksysProcess_Kill(m_Process);
    itksysProcess_WaitForExit(m_Process, nullptr);
  }

  VLOG(1) << "SCIFIOImageIO::DestroyJavaProcess destroying java process";
  itksysProcess_Delete(m_Process);
  m_Process = nullptr;

#ifdef _WIN32
  CloseHandle( m_Pipe[1] );
#else
  close(m_Pipe[1]);
#endif
}

bool SCIFIOImageIO::SupportsDimension(unsigned long dim)
{
  return dim <= 5;
}

bool SCIFIOImageIO::CanReadFile(const char* FileNameToRead)
{
  VLOG(1) << "SCIFIOImageIO::CanReadFile: FileNameToRead = " << FileNameToRead;

  CreateJavaProcess();

  // send the command to the java process
  std::string command = "canRead\t";
  command += FileNameToRead;
  command += "\n";
  VLOG(1) << "SCIFIOImageIO::CanRead command: " << command;

#ifdef _WIN32
  DWORD bytesWritten;
  bool r = WriteFile( m_Pipe[1], command.c_str(), command.size(), &bytesWritten, NULL );
#else
  const ssize_t writtenBytes = write(m_Pipe[1], command.c_str(), command.size());
  if (writtenBytes < static_cast< ssize_t >( command.size())) {
    itkExceptionMacro(<< "Only wrote " << writtenBytes << " of " << command.size() << " bytes!");
  }
#endif

  // fflush( m_Pipe[1] );

  // and read its reply
  std::string imgInfo;
  int pipedatalength = 1000;

  VLOG(1) << "Checking if can read file";
  imgInfo = WaitForNewLines(pipedatalength);
  VLOG(1) << "Done checking if can read file";

  // we have one thing per line
  int p0 = 0;
  int p1 = 0;
  std::string canRead;
  // can read?
  p1 = imgInfo.find('\n', p0);
  canRead = imgInfo.substr(p0, p1);

  return valueOfString<bool>(canRead);
}

bool SCIFIOImageIO::SetSeries(int series)
{
  VLOG(1) << "SCIFIOImageIO::SetSeries: series = " << series;

  CreateJavaProcess();

  std::string command = "series";
  command += "\t";
  command += toString(series);
  command += "\n";

  VLOG(1) << "SCIFIOImageIO::SetSeries command: " << command;

#ifdef _WIN32
  DWORD bytesWritten;
  WriteFile( m_Pipe[1], command.c_str(), command.size(), &bytesWritten, NULL );
#else
  const ssize_t writtenBytes = write(m_Pipe[1], command.c_str(), command.size());
  if (writtenBytes < static_cast< ssize_t >( command.size())) {
    itkExceptionMacro(<< "Only wrote " << writtenBytes << " of " << command.size() << " bytes!");
  }
#endif

  // fflush( m_Pipe[1] );

  std::string commandOutput;
  int pipedatalength = 1000;

  VLOG(1) << "Waiting for confirmation of command.";
  commandOutput = WaitForNewLines(pipedatalength);
  VLOG(1) << "Command finished.";

  // we have one thing per line
  int p0 = 0;
  int p1 = 0;
  std::string seriesResult;
  p1 = commandOutput.find("\n", p0);
  seriesResult = commandOutput.substr(p0, p1);
  VLOG(1) << "SetSeries result: " << seriesResult;

  // Clear the previous dictionary entries, since we do not
  // allow overwriting of pre-existing entries - this will
  // allow critical metadata (such as dimension extents)
  // to be recorded properly.
  MetaDataDictionary& dict = this->GetMetaDataDictionary();
  dict.Clear();

  return true;
}

int SCIFIOImageIO::GetSeriesCount()
{
  VLOG(1) << "SCIFIOImageIO::GetSeriesCount";

  CreateJavaProcess();

  std::string command = "seriesCount";
  command += "\n";

  VLOG(1) << "SCIFIOImageIO::GetSeriesCount command: " << command;

#ifdef _WIN32
  DWORD bytesWritten;
  WriteFile( m_Pipe[1], command.c_str(), command.size(), &bytesWritten, NULL );
#else
  const ssize_t writtenBytes = write(m_Pipe[1], command.c_str(), command.size());
  if (writtenBytes < static_cast< ssize_t >( command.size())) {
    itkExceptionMacro(<< "Only wrote " << writtenBytes << " of " << command.size() << " bytes!");
  }
#endif

  // fflush( m_Pipe[1] );

  int seriesCount = -1;
  std::string commandOutput;
  int pipedatalength = 1000;

  VLOG(1) << "Waiting for confirmation of command.";
  commandOutput = WaitForNewLines(pipedatalength);
  VLOG(1) << "Command finished.";

  // we have one thing per line
  int p0 = 0;
  int p1 = 0;
  std::string seriesResult;
  p1 = commandOutput.find("\n", p0);
  seriesResult = commandOutput.substr(p0, p1);
  VLOG(1) << "GetSeriesCount result: " << seriesResult;

  seriesCount = valueOfString<int>(commandOutput);

  return seriesCount;
}

void SCIFIOImageIO::ReadImageInformation()
{
  VLOG(1) << "SCIFIOImageIO::ReadImageInformation: m_FileName = " << m_FileName;

  CreateJavaProcess();

  // send the command to the java process
  std::string command = "info\t";
  command += m_FileName;
  command += "\n";
  VLOG(1) << "SCIFIOImageIO::ReadImageInformation command: " << command;

#ifdef _WIN32
  DWORD bytesWritten;
  WriteFile( m_Pipe[1], command.c_str(), command.size(), &bytesWritten, NULL );
#else
  const ssize_t writtenBytes = write(m_Pipe[1], command.c_str(), command.size());
  if (writtenBytes < static_cast< ssize_t >( command.size())) {
    itkExceptionMacro(<< "Only wrote " << writtenBytes << " of " << command.size() << " bytes!");
  }
#endif

  // fflush( m_Pipe[1] );
  std::string imgInfo;
  int pipedatalength = 1000;

  VLOG(1) << "Reading image information";
  imgInfo = WaitForNewLines(pipedatalength);
  VLOG(1) << "Done reading image information";


  // fill the metadata dictionary
  MetaDataDictionary& dict = this->GetMetaDataDictionary();

  // we have one thing per two lines
  size_t p0 = 0;
  size_t p1 = 0;
  std::string line;

  while (p0 < imgInfo.size()) {

    // get the key line
    p1 = imgInfo.find('\n', p0);

    line = imgInfo.substr(p0, p1 - p0);

    // ignore the empty lines
    if (line.empty()) {
      // go to the next line
      p0 = p1 + 1;
      continue;
    }

    std::string key = line;
    // go to the next line
    p0 = p1 + 1;

    // get the value line
    p1 = imgInfo.find('\n', p0);

    line = imgInfo.substr(p0, p1 - p0);

    // ignore the empty lines
    if (line.empty()) {
      // go to the next line
      p0 = p1 + 1;
      continue;
    }

    std::string value = line;

    // store the values in the dictionary
    if (dict.HasKey(key)) {
      VLOG(1) << "SCIFIOImageIO::ReadImageInformation metadata " << key << " = " << value
              << " ignored because the key is already defined.";
    } else {
      std::string tmp;
      // we have to unescape \\ and \n
      size_t lp0 = 0;
      size_t lp1 = 0;

      while (lp0 < value.size()) {
        lp1 = value.find('\\', lp0);
        if (lp1 == std::string::npos) {
          tmp += value.substr(lp0, value.size() - lp0);
          lp0 = value.size();

        } else {
          tmp += value.substr(lp0, lp1 - lp0);
          if (lp1 < value.size() - 1) {
            if (value[lp1 + 1] == '\\') {
              tmp += '\\';
            } else if (value[lp1 + 1] == 'n') {
              tmp += '\n';
            }
          }
          lp0 = lp1 + 2;
        }

      }
      VLOG(1) << "Storing metadata: " << key << " ---> " << tmp;
      EncapsulateMetaData<std::string>(dict, key, tmp);
    }

    // go to the next line
    p0 = p1 + 1;

  }

  // save the dicitonary
  m_MetaDataDictionary = dict;

  // set the values needed by the reader

  // is interleaved?
  const bool isInterleaved = GetTypedMetaData<bool>(dict, "Interleaved");
  if (isInterleaved) {
    VLOG(1) << "Interleaved ---> True";
  } else {
    VLOG(1) << "Interleaved ---> False";
  }

  // is little endian?
  const bool isLittleEndian = GetTypedMetaData<bool>(dict, "LittleEndian");
  if (isLittleEndian) {
    VLOG(1) << "Setting LittleEndian ---> True";
    this->SetByteOrderToLittleEndian();
  } else {
    VLOG(1) << "Setting LittleEndian ---> False";
    this->SetByteOrderToBigEndian();
  }

  // component type
  itkAssertOrThrowMacro(dict.HasKey("PixelType"), "PixelType is not in the metadata dictionary!");
  const long pixelType = GetTypedMetaData<long>(dict, "PixelType");
  VLOG(1) << "Setting ComponentType: " << pixelType;
  this->SetComponentType(scifioToITKComponentType(pixelType));

  // Dimensions are stored in x, y, z, t, c order

  // only size > 1 dimensions are stored in the ITK data structure

  // dimension lengths & spacing
  std::vector<long> lengthVec;
  std::vector<double> spacingVec;
  long length;
  double spacing;

  length = GetTypedMetaData<long>(dict, "SizeC");
  spacing = GetTypedMetaData<double>(dict, "PixelsPhysicalSizeC");
  checkLength(length, spacing, lengthVec, spacingVec);

  length = GetTypedMetaData<long>(dict, "SizeT");
  spacing = GetTypedMetaData<double>(dict, "PixelsPhysicalSizeT");
  checkLength(length, spacing, lengthVec, spacingVec);

  length = GetTypedMetaData<long>(dict, "SizeZ");
  spacing = GetTypedMetaData<double>(dict, "PixelsPhysicalSizeZ");
  checkLength(length, spacing, lengthVec, spacingVec);

  length = GetTypedMetaData<long>(dict, "SizeY");
  spacing = GetTypedMetaData<double>(dict, "PixelsPhysicalSizeY");
  checkLength(length, spacing, lengthVec, spacingVec);

  length = GetTypedMetaData<long>(dict, "SizeX");
  spacing = GetTypedMetaData<double>(dict, "PixelsPhysicalSizeX");
  checkLength(length, spacing, lengthVec, spacingVec);

  this->SetNumberOfDimensions(lengthVec.size());
  for (size_t i = 0; i < lengthVec.size(); i++) {
    VLOG(1) << "Setting Length " << i << ": " << lengthVec.at(i);
    VLOG(1) << "Setting Spacing " << i << ": " << spacingVec.at(i);
    int index = lengthVec.size() - 1 - i;
    this->SetDimensions(i, lengthVec.at(index));
    this->SetSpacing(i, spacingVec.at(index));
  }

  // number of components
  const long rgbChannelCount = GetTypedMetaData<long>(dict, "RGBChannelCount");
  if (rgbChannelCount == 1) {
    this->SetPixelType(IOPixelEnum::SCALAR);
  } else if (rgbChannelCount == 3) {
    this->SetPixelType(IOPixelEnum::RGB);
  } else if (rgbChannelCount == 4) {
    this->SetPixelType(IOPixelEnum::RGBA);
  } else {
    this->SetPixelType(IOPixelEnum::VECTOR);
  }

  this->SetNumberOfComponents(rgbChannelCount);
}

void SCIFIOImageIO::Read(void* pData)
{
  const ImageIORegion& region = this->GetIORegion();

  CreateJavaProcess();

  // send the command to the java process
  std::string command = "read\t";
  command += m_FileName;

  command += FindDimensionOrder(region);
  command += "\n";
  LOG(INFO) << "SCIFIOImageIO::Read command: " << command;

#ifdef _WIN32
  DWORD bytesWritten;
  WriteFile( m_Pipe[1], command.c_str(), command.size(), &bytesWritten, NULL );
#else
  const ssize_t writtenBytes = write(m_Pipe[1], command.c_str(), command.size());
  if (writtenBytes < static_cast< ssize_t >( command.size())) {
    itkExceptionMacro(<< "Only wrote " << writtenBytes << " of " << command.size() << " bytes!");
  }
#endif

  // fflush( m_Pipe[1] );

  // and read the image
  char* data = (char*) pData;
  size_t pos = 0;
  std::string errorMessage;
  char* pipedata;
  int pipedatalength;

  MetaDataDictionary& dict = this->GetMetaDataDictionary();
  size_t byteCount = this->GetComponentSize() * region.GetNumberOfPixels() * GetTypedMetaData<long>(dict, "SizeC");

  while (pos < byteCount) {
    int retcode = itksysProcess_WaitForData(m_Process, &pipedata, &pipedatalength, nullptr);
    if (retcode == itksysProcess_Pipe_STDOUT) {
      memcpy(data + pos, pipedata, pipedatalength);
      pos += pipedatalength;
    } else if (retcode == itksysProcess_Pipe_STDERR) {
      std::string message(pipedata, pipedatalength);
      VLOG(1) << "Got error message: " << message;
      CheckError(message);
      errorMessage += message;
    } else {
      DestroyJavaProcess();
      itkExceptionMacro(<<"SCIFIOImageIO: 'SCIFIOITKBridge read' exited abnormally. " << errorMessage);
    }
  }
}

bool SCIFIOImageIO::CanWriteFile(const char* name)
{
  VLOG(1) << "SCIFIOImageIO::CanWriteFile: name = " << name;
  CreateJavaProcess();

  std::string command = "canWrite\t";
  command += name;
  command += "\n";

#ifdef _WIN32
  DWORD bytesWritten;
   WriteFile( m_Pipe[1], command.c_str(), command.size(), &bytesWritten, NULL );
#else
  const ssize_t writtenBytes = write(m_Pipe[1], command.c_str(), command.size());
  if (writtenBytes < static_cast< ssize_t >( command.size())) {
    itkExceptionMacro(<< "Only wrote " << writtenBytes << " of " << command.size() << " bytes!");
  }
#endif

  // fflush( m_Pipe[1] );

  std::string imgInfo;
  int pipedatalength = 1000;

  VLOG(1) << "Checking if can write file.";
  imgInfo = WaitForNewLines(pipedatalength);
  VLOG(1) << "Done checking if can write file.";

  // we have one thing per line
  int p0 = 0;
  int p1 = 0;
  std::string canWrite;
  // can write?
  p1 = imgInfo.find('\n', p0);
  canWrite = imgInfo.substr(p0, p1);
  VLOG(1) << "CanWrite result: " << canWrite;
  return valueOfString<bool>(canWrite);
}


void SCIFIOImageIO::WriteImageInformation()
{
  VLOG(1) << "SCIFIOImageIO::WriteImageInformation";
  // NB: Nothing to do.
}


void SCIFIOImageIO::Write(const void* buffer)
{
  VLOG(1) << "SCIFIOImageIO::Write";

  CreateJavaProcess();

  ImageIORegion region = GetIORegion();
  int regionDim = region.GetImageDimension();

  std::string command = "write\t";
  VLOG(1) << "File name: " << m_FileName;
  command += m_FileName;
  command += "\t";
  VLOG(1) << "Byte Order: " << this->GetByteOrderAsString(GetByteOrder());
  switch (GetByteOrder()) {
    case IOByteOrderEnum::BigEndian:
      command += toString(1);
      break;
    case IOByteOrderEnum::LittleEndian:
    default:
      command += toString(0);
  }
  command += "\t";
  VLOG(1) << "Region dimensions: " << regionDim;
  command += toString(regionDim);
  command += "\t";

  for (int i = 0; i < regionDim; ++i) {
    VLOG(1) << "Dimension " << i << ": " << region.GetSize(i);
    command += toString(region.GetSize(i));
    command += "\t";
  }

  for (int i = regionDim; i < 5; ++i) {
    VLOG(1) << "Dimension " << i << ": " << 1;
    command += toString(1);
    command += "\t";
  }

  for (int i = 0; i < regionDim; ++i) {
    VLOG(1) << "Phys Pixel size " << i << ": " << this->GetSpacing(i);
    command += toString(this->GetSpacing(i));
    command += "\t";
  }

  for (int i = regionDim; i < 5; i++) {
    VLOG(1) << "Phys Pixel size" << i << ": " << 1;
    command += toString(1);
    command += "\t";
  }

  VLOG(1) << "Pixel Type: " << itkToSCIFIOPixelType(GetComponentType());
  command += toString(itkToSCIFIOPixelType(GetComponentType()));
  command += "\t";

  int rgbChannelCount = GetNumberOfComponents();

  VLOG(1) << "RGB Channels: " << rgbChannelCount;
  command += toString(rgbChannelCount);
  command += "\t";

  // int xIndex = 0, yIndex = 1
  int zIndex = 2;
  int cIndex = 3;
  int tIndex = 4;
  int bytesPerPlane = rgbChannelCount;
  int numPlanes = 1;

  for (int dim = 0; dim < 5; dim++) {
    if (dim < regionDim) {
      int index = region.GetIndex(dim);
      int size = region.GetSize(dim);
      VLOG(1) << "dim = " << dim << " index = " << toString(index) << " size = " << toString(size);
      command += toString(index);
      command += "\t";
      command += toString(size);
      command += "\t";

      if (dim == cIndex || dim == zIndex || dim == tIndex) {
        numPlanes *= size - index;
      }
    } else {
      VLOG(1) << "dim = " << dim << " index = " << 0 << " size = " << 1;
      command += toString(0);
      command += "\t";
      command += toString(1);
      command += "\t";
    }
  }

  // build lut if necessary
  MetaDataDictionary& dict = m_MetaDataDictionary;

  const bool useLut = GetTypedMetaData<bool>(dict, "UseLUT");

  VLOG(1) << "useLUT = " << useLut;

  if (useLut) {
    command += toString(1);
    command += "\t";
    int LUTBits = GetTypedMetaData<int>(dict, "LUTBits");
    command += toString(LUTBits);
    command += "\t";
    int LUTLength = GetTypedMetaData<int>(dict, "LUTLength");
    command += toString(LUTLength);
    command += "\t";
    VLOG(1) << command;

    VLOG(1) << "Found a LUT of length: " << LUTLength;
    VLOG(1) << "Found a LUT of bits: " << LUTBits;

    for (int i = 0; i < LUTLength; ++i) {
      if (LUTBits == 8) {
        int rValue = GetTypedMetaData<int>(dict, "LUTR" + toString(i));
        command += toString(rValue);
        command += "\t";
        int gValue = GetTypedMetaData<int>(dict, "LUTG" + toString(i));
        command += toString(gValue);
        command += "\t";
        int bValue = GetTypedMetaData<int>(dict, "LUTB" + toString(i));
        command += toString(bValue);
        command += "\t";
        VLOG(1) << "Retrieval " << i << " r,g,b values = " << rValue << "," << gValue << "," << bValue;
      } else {
        auto rValue = GetTypedMetaData<short>(dict, "LUTR" + toString(i));
        command += toString(rValue);
        command += "\t";
        auto gValue = GetTypedMetaData<short>(dict, "LUTG" + toString(i));
        command += toString(gValue);
        command += "\t";
        auto bValue = GetTypedMetaData<short>(dict, "LUTB" + toString(i));
        command += toString(bValue);
        command += "\t";
        VLOG(1) << "Retrieval " << i << " r,g,b values = " << rValue << "," << gValue << "," << bValue;
        command += "\t";
      }
    }
  } // if useLut
  else {
    command += toString(0);
    command += "\t";
  }

  command += "\n";


  VLOG(1) << "SCIFIOImageIO::Write command: " << command;

#ifdef _WIN32
  DWORD bytesWritten;
  WriteFile( m_Pipe[1], command.c_str(), command.size(), &bytesWritten, NULL );
#else
  ssize_t writtenBytes = write(m_Pipe[1], command.c_str(), command.size());
  if (writtenBytes < static_cast< ssize_t >( command.size())) {
    itkExceptionMacro(<< "Only wrote " << writtenBytes << " of " << command.size() << " bytes!");
  }
#endif

  // need to read back the number of planes and bytes per plane to read from buffer
  std::string imgInfo;
  int pipedatalength = 1000;

  VLOG(1) << "Reading number of planes and bytes per plane to write";
  imgInfo = WaitForNewLines(pipedatalength);
  VLOG(1) << "Done reading number of planes and bytes per plane to write";

  // bytesPerPlane is the first line
  int p0 = 0;
  int p1 = 0;
  std::string vals;
  p1 = imgInfo.find('\n', p0);
  vals = imgInfo.substr(p0, p1);

  bytesPerPlane = valueOfString<int>(vals);
  VLOG(1) << "BPP: " << bytesPerPlane << " numPlanes: " << numPlanes;

  p0 = p1;
  p1 = imgInfo.find('\n', p0);
  vals = imgInfo.substr(p0, p1);

  using BYTE = unsigned char;
  BYTE* data = (BYTE*) buffer;
  char donemsg[] = {'O', 'K'};

  constexpr int pipelength = 10000;

  for (int i = 0; i < numPlanes; ++i) {
    int bytesRead = 0;
    while (bytesRead < bytesPerPlane) {
      VLOG(1) << "bytesPerPlane: " << bytesPerPlane << " bytesRead: " << bytesRead << " pipelength: " << pipelength;
      int bytesToRead;
      if (bytesPerPlane - bytesRead > pipelength) {
        bytesToRead = pipelength;
      } else {
        bytesToRead = bytesPerPlane - bytesRead;
      }

      VLOG(1) << "Writing " << bytesToRead << " bytes to plane " << i << ".  Bytes read: " << bytesRead;

#ifdef _WIN32
      WriteFile( m_Pipe[1], data, bytesToRead, &bytesWritten, NULL );
#else
      writtenBytes = write(m_Pipe[1], data, bytesToRead);
      if (writtenBytes < bytesToRead) {
        itkExceptionMacro(<< "Only wrote " << writtenBytes << " of " << bytesToRead << " bytes!");
      }
#endif

      data += bytesToRead;
      bytesRead += bytesToRead;

      std::string bytesDone;

      VLOG(1) << "Waiting for confirmation of bytes read";
      bytesDone = WaitForNewLines(pipedatalength);
      VLOG(1) << "Done waiting for confirmation of bytes read";
    }

    std::string planeDone;

    // Hand-shake with Java signaling it's OK to send end of plane msg.
#ifdef _WIN32
    WriteFile( m_Pipe[1], donemsg, 2, &bytesWritten, NULL );
#else
    writtenBytes = write(m_Pipe[1], donemsg, 2);
#endif

    VLOG(1) << "Waiting for confirmation of plane read";
    planeDone = WaitForNewLines(pipedatalength);
    VLOG(1) << "Done waiting for confirmation of plane read";
  }
  std::string imageDone;

  // Hand-shake with Java signaling it's OK to send end of image msg.
#ifdef _WIN32
  WriteFile( m_Pipe[1], donemsg, 2, &bytesWritten, NULL );
#else
  writtenBytes = write(m_Pipe[1], donemsg, 2);
#endif

  VLOG(1) << "Waiting for confirmation of image read";
  imageDone = WaitForNewLines(pipedatalength);
  VLOG(1) << "Done waiting for confirmation of image read";
}

} // end namespace itk
