#include "zneuroglancerurl.h"

#include "zcloudobjectstore.h"
#include "zlog.h"

#include <QUrl>

namespace nim {
namespace {

std::string toStdString(const QString& s)
{
  const auto u8 = s.toUtf8();
  return std::string(u8.data(), static_cast<size_t>(u8.size()));
}

} // namespace

std::optional<QString> decodeSupportedNeuroglancerPrecomputedSourceUrl(QString url)
{
  QString s = url.trimmed();
  if (s.isEmpty()) {
    return std::nullopt;
  }

  if (s.startsWith("precomputed://", Qt::CaseInsensitive)) {
    return s;
  }

  // Newer Neuroglancer datasource URLs may wrap a precomputed root as:
  //   s3://bucket/path|neuroglancer-precomputed:
  // Keep support narrow and explicit: strip only the known provider suffix and
  // return the underlying dataset root URL that Atlas already knows how to open.
  static const QString kNgPrecomputedSuffix = QStringLiteral("|neuroglancer-precomputed:");
  if (s.endsWith(kNgPrecomputedSuffix, Qt::CaseInsensitive)) {
    s.chop(kNgPrecomputedSuffix.size());
    s = s.trimmed();
    if (!s.isEmpty()) {
      return s;
    }
  }

  return std::nullopt;
}

QString normalizeNeuroglancerUrlDropFragment(QString url)
{
  QString s = url.trimmed();
  const int hash = s.indexOf('#');
  if (hash >= 0) {
    s = s.left(hash);
  }
  return s.trimmed();
}

QString mapCloudStorageUrlToHttps(QString url)
{
  return cloudObjectUrlToHttps(std::move(url));
}

QString normalizeNeuroglancerPrecomputedRootUrl(QString url)
{
  url = url.trimmed();
  if (url.startsWith("precomputed://", Qt::CaseInsensitive)) {
    url = url.mid(QStringLiteral("precomputed://").size());
  }

  if (url.startsWith("s3://", Qt::CaseInsensitive)) {
    const QString rest = url.mid(QStringLiteral("s3://").size());
    const int slash = rest.indexOf('/');
    const QString bucket = (slash < 0) ? rest : rest.left(slash);
    if (bucket.isEmpty()) {
      throw ZException(fmt::format("Invalid S3 URL '{}': missing bucket name", toStdString(url)));
    }
  }

  url = mapCloudStorageUrlToHttps(std::move(url));

  QUrl qurl(url);
  if (!qurl.isValid()) {
    throw ZException(fmt::format("Invalid URL '{}'", toStdString(url)));
  }

  QString path = qurl.path();
  if (path.endsWith(".json", Qt::CaseInsensitive)) {
    throw ZException(fmt::format(
      "Neuroglancer precomputed expects a dataset root URL (a directory containing an 'info' file), but got a JSON file URL '{}'. "
      "If this is a Neuroglancer viewer state (.json), open it in Neuroglancer and copy the layer source URL (e.g. precomputed://gs://.../volume) or paste the dataset root URL.",
      toStdString(url)));
  }
  if (path.endsWith("/info", Qt::CaseInsensitive)) {
    path.chop(QString("/info").size());
    qurl.setPath(path);
  } else if (path.endsWith("info", Qt::CaseInsensitive) && !path.endsWith("/info", Qt::CaseInsensitive)) {
    path.chop(QString("info").size());
    qurl.setPath(path);
  }

  path = qurl.path();
  while (path.endsWith('/') && path.size() > 1) {
    path.chop(1);
  }
  if (!path.endsWith('/')) {
    path += '/';
  }
  qurl.setPath(path);
  return qurl.toString();
}

} // namespace nim
