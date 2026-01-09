#include "zneuroglancerstate.h"

#include <gtest/gtest.h>

#include <boost/json.hpp>

namespace nim {
namespace json = boost::json;

TEST(ZNeuroglancerState, ParseSupportedLayersAndBindings)
{
  const std::string text = R"json(
{
  "layers": [
    {
      "type": "image",
      "name": "img_layer",
      "visible": true,
      "opacity": 0.6,
      "source": "precomputed://gs://bucket/img"
    },
    {
      "type": "segmentation",
      "name": "seg_layer",
      "visible": false,
      "source": [
        { "url": "precomputed://gs://bucket/seg", "subsources": { "default": true } },
        "precomputed://gs://bucket/seg/skeletons_32nm",
        "precomputed://gs://bucket/seg/mesh#type=mesh"
      ]
    },
    {
      "type": "annotation",
      "name": "ann_layer",
      "source": "precomputed://gs://bucket/ann",
      "linkedSegmentationLayer": { "a": "seg_layer" }
    }
  ]
}
)json";

  const json::value state = json::parse(text);
  const ZNeuroglancerState::ParseResult parsed = ZNeuroglancerState::parse(state);

  ASSERT_EQ(parsed.layers.size(), 2U);
  EXPECT_TRUE(parsed.warnings.isEmpty());

  {
    const auto& l = parsed.layers.at(0);
    EXPECT_EQ(l.type, ZNeuroglancerState::LayerType::Image);
    EXPECT_EQ(l.name, QStringLiteral("img_layer"));
    EXPECT_EQ(l.volumeUrl, QStringLiteral("precomputed://gs://bucket/img"));
    EXPECT_TRUE(l.visible);
    ASSERT_TRUE(l.opacity.has_value());
    EXPECT_DOUBLE_EQ(*l.opacity, 0.6);
  }

  {
    const auto& l = parsed.layers.at(1);
    EXPECT_EQ(l.type, ZNeuroglancerState::LayerType::Segmentation);
    EXPECT_EQ(l.name, QStringLiteral("seg_layer"));
    EXPECT_EQ(l.volumeUrl, QStringLiteral("precomputed://gs://bucket/seg"));
    EXPECT_FALSE(l.visible);
    EXPECT_FALSE(l.opacity.has_value());
    EXPECT_EQ(l.skeletonSourceOverrideUrl, QStringLiteral("precomputed://gs://bucket/seg/skeletons_32nm"));
    EXPECT_EQ(l.meshSourceOverrideUrl, QStringLiteral("precomputed://gs://bucket/seg/mesh"));
  }

  ASSERT_EQ(parsed.annotationsBindings.size(), 1U);
  {
    const auto& b = parsed.annotationsBindings.at(0);
    EXPECT_EQ(b.annotationsRootUrl, QStringLiteral("precomputed://gs://bucket/ann"));
    ASSERT_EQ(b.linkedSegmentationLayerNames.size(), 1);
    EXPECT_EQ(b.linkedSegmentationLayerNames.front(), QStringLiteral("seg_layer"));
  }
}

} // namespace nim

