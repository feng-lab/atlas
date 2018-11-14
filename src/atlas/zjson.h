#pragma once

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

namespace nim {

void modifyJsonValue(QJsonObject& obj, const QString& path, const QJsonValue& newValue);

void modifyJsonValue(QJsonArray& array, const QString& path, const QJsonValue& newValue);

void modifyJsonValue(QJsonDocument& doc, const QString& path, const QJsonValue& newValue);

void removeJsonValue(QJsonObject& obj, const QString& path);

void removeJsonValue(QJsonDocument& doc, const QString& path);

} // namespace nim

