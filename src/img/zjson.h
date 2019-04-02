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

QJsonObject loadJsonObject(const QString& file);

void saveJsonObject(const QJsonObject& json, const QString& file);

QStringList readStringList(const QJsonObject& json, const QString& key);

QString readString(const QJsonObject& json, const QString& key);

double readNumber(const QJsonObject& json, const QString& key);

std::vector<double> readNumberArray(const QJsonObject& json, const QString& key);

bool readBool(const QJsonObject& json, const QString& key);

} // namespace nim

