#ifndef ZJSON_H
#define ZJSON_H

#include <QJsonDocument>
#include <QJsonValue>
#include <QJsonObject>

namespace nim {

void modifyJsonValue(QJsonObject& obj, const QString& path, const QJsonValue& newValue);

void modifyJsonValue(QJsonDocument& doc, const QString& path, const QJsonValue& newValue);

} // namespace nim

#endif // ZJSON_H
