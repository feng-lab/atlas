#include "zswcsubtract.h"

#include "zcancellation.h"
#include "zexception.h"
#include "zswc.h"
#include "zswcops.h"

#include <QFileInfo>

namespace nim {

namespace {

void appendSwcRoots(ZSwc& dst, const ZSwc& src)
{
  for (auto root = src.beginRoot(); root != src.endRoot(); ++root) {
    const auto dstRoot = dst.appendRoot(*root);
    dst.copy(dstRoot, src, root);
  }
}

} // namespace

void ZSwcSubtract::doWork()
{
  if (m_outputSwcFilename.trimmed().isEmpty()) {
    throw ZException("Output SWC file can not be empty.");
  }
  if (m_inputSwcFilename.trimmed().isEmpty()) {
    throw ZException("Input SWC file can not be empty.");
  }
  if (m_subtractSwcFilenames.empty()) {
    throw ZException("Subtract SWC list can not be empty.");
  }

  maybeCancel(m_cancellationToken);

  ZSwc inputTree(m_inputSwcFilename);

  maybeCancel(m_cancellationToken);

  ZSwc subtractTree(m_subtractSwcFilenames.at(0));
  for (int i = 1; i < m_subtractSwcFilenames.size(); ++i) {
    maybeCancel(m_cancellationToken);
    ZSwc tmp(m_subtractSwcFilenames.at(i));
    appendSwcRoots(subtractTree, tmp);
  }

  maybeCancel(m_cancellationToken);
  subtractSwcTrees(inputTree, subtractTree);
  maybeCancel(m_cancellationToken);

  inputTree.save(m_outputSwcFilename);
  reportProgress(1.0);
}

void ZSwcSubtract::read(const json::object& jo)
{
  m_inputSwcFilename = json::value_to<QString>(jo.at("input_swc"));
  m_subtractSwcFilenames = json::value_to<QStringList>(jo.at("subtract_swcs"));
  m_outputSwcFilename = json::value_to<QString>(jo.at("output_swc"));
}

void ZSwcSubtract::write(json::object& jo) const
{
  jo["input_swc"] = json::value_from(m_inputSwcFilename);
  jo["subtract_swcs"] = json::value_from(m_subtractSwcFilenames);
  jo["output_swc"] = json::value_from(m_outputSwcFilename);
}

} // namespace nim
