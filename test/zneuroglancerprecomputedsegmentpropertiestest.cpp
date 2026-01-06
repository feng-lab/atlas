#include "zneuroglancerprecomputedsegmentproperties.h"

#include "zexception.h"

#include <gtest/gtest.h>

#include <QUrl>

#include <string>

namespace nim {
namespace {
TEST(ZNeuroglancerPrecomputedSegmentProperties, ParseInline)
{
  const std::string info = R"({
    "@type": "neuroglancer_segment_properties",
    "inline": {
      "ids": ["1", "2"],
      "properties": [
        { "id": "label", "type": "label", "values": ["a", "b"] },
        { "id": "description", "type": "description", "values": ["da", "db"] },
        { "id": "tags", "type": "tags", "tags": ["foo", "bar"], "values": [[0, 1], null] }
      ]
    }
  })";
  auto props = ZNeuroglancerPrecomputedSegmentProperties::parseInfoJsonText(QUrl("https://example.invalid/"), info);
  ASSERT_TRUE(props);
  EXPECT_EQ(props->numIds(), 2u);
  EXPECT_TRUE(props->hasLabel());
  EXPECT_TRUE(props->hasDescription());
  EXPECT_TRUE(props->hasTags());
  EXPECT_EQ(props->labelForId(1).value_or(QString()), QStringLiteral("a"));
  EXPECT_EQ(props->labelForId(2).value_or(QString()), QStringLiteral("b"));
  EXPECT_EQ(props->descriptionForId(1).value_or(QString()), QStringLiteral("da"));
  EXPECT_EQ(props->descriptionForId(2).value_or(QString()), QStringLiteral("db"));
  const QStringList tags1 = props->tagsForId(1);
  EXPECT_EQ(tags1.size(), 2);
  EXPECT_EQ(tags1[0], QStringLiteral("foo"));
  EXPECT_EQ(tags1[1], QStringLiteral("bar"));
  const QStringList tags2 = props->tagsForId(2);
  EXPECT_TRUE(tags2.empty());
}
TEST(ZNeuroglancerPrecomputedSegmentProperties, ParseLegacyRootLevelIdsAndProperties)
{
  // Some datasets omit the `inline` wrapper and place `ids`/`properties` at the root.
  const std::string info = R"({
    "@type": "neuroglancer_segment_properties",
    "ids": ["10"],
    "properties": [
      { "id": "label", "type": "label", "values": ["ten"] }
    ]
  })";
  auto props = ZNeuroglancerPrecomputedSegmentProperties::parseInfoJsonText(QUrl("https://example.invalid/"), info);
  ASSERT_TRUE(props);
  EXPECT_EQ(props->numIds(), 1u);
  EXPECT_EQ(props->labelForId(10).value_or(QString()), QStringLiteral("ten"));
}
TEST(ZNeuroglancerPrecomputedSegmentProperties, ParseEmptyNoInline)
{
  const std::string info = R"({
    "@type": "neuroglancer_segment_properties"
  })";
  auto props = ZNeuroglancerPrecomputedSegmentProperties::parseInfoJsonText(QUrl("https://example.invalid/"), info);
  ASSERT_TRUE(props);
  EXPECT_EQ(props->numIds(), 0u);
  EXPECT_TRUE(props->properties().empty());
}
TEST(ZNeuroglancerPrecomputedSegmentProperties, ParseEmptyInlineNull)
{
  const std::string info = R"({
    "@type": "neuroglancer_segment_properties",
    "inline": null
  })";
  auto props = ZNeuroglancerPrecomputedSegmentProperties::parseInfoJsonText(QUrl("https://example.invalid/"), info);
  ASSERT_TRUE(props);
  EXPECT_EQ(props->numIds(), 0u);
  EXPECT_TRUE(props->properties().empty());
}
TEST(ZNeuroglancerPrecomputedSegmentProperties, RejectUnsupportedNonInlineRepresentation)
{
  const std::string info = R"({
    "@type": "neuroglancer_segment_properties",
    "by_id": {}
  })";
  EXPECT_THROW((void)ZNeuroglancerPrecomputedSegmentProperties::parseInfoJsonText(QUrl("https://example.invalid/"), info),
               ZException);
}
} // namespace
} // namespace nim
