#include "zneuroglancerstate.h"

#include <gtest/gtest.h>

#include <boost/json.hpp>

#include <QUrl>

#include <map>

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
  EXPECT_TRUE(parsed.selectedLayerName.isEmpty());

  {
    const auto& l = parsed.layers.at(0);
    EXPECT_EQ(l.type, ZNeuroglancerState::LayerType::Image);
    EXPECT_EQ(l.name, QStringLiteral("img_layer"));
    EXPECT_EQ(l.volumeUrl, QStringLiteral("precomputed://gs://bucket/img"));
    EXPECT_TRUE(l.visible);
    ASSERT_TRUE(l.opacity.has_value());
    EXPECT_DOUBLE_EQ(*l.opacity, 0.6);
    EXPECT_TRUE(l.segments.empty());
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
    EXPECT_TRUE(l.segments.empty());
  }

  ASSERT_EQ(parsed.annotationsBindings.size(), 1U);
  {
    const auto& b = parsed.annotationsBindings.at(0);
    EXPECT_EQ(b.annotationsRootUrl, QStringLiteral("precomputed://gs://bucket/ann"));
    ASSERT_EQ(b.linkedSegmentationLayerNames.size(), 1);
    EXPECT_EQ(b.linkedSegmentationLayerNames.front(), QStringLiteral("seg_layer"));
  }
}

TEST(ZNeuroglancerState, ParseInputTextFromShareLinkAndSegments)
{
  // Compact share-link example derived from the ExPID82_1 Neuroglancer states used for manual testing.
  const QString stateText = QStringLiteral(R"json(
{
  "dimensions": {
    "x": [1.8e-8, "m"],
    "y": [1.8e-8, "m"],
    "z": [2.4e-8, "m"]
  },
  "position": [1999.332275390625, 2451.114990234375, 274.5],
  "crossSectionScale": 8.124369815694616,
  "projectionOrientation": [0, 0, 0.7071067690849304, 0.7071067690849304],
  "projectionScale": 4541.04911654507,
  "layers": [
    {
      "type": "image",
      "source": "precomputed://gs://liconn-public/ExPID82_1/image_230130b",
      "tab": "source",
      "name": "image_230130b"
    },
    {
      "type": "segmentation",
      "source": "precomputed://gs://liconn-public/ExPID82_1/segmentation/231030_agg_240123",
      "tab": "segments",
      "segments": ["79", "1843", "22772", "!104207", "!215852"],
      "segmentQuery": "79, 1843, 22772, 104207, 215852",
      "name": "dendrites"
    }
  ],
  "showAxisLines": false,
  "showSlices": false,
  "selectedLayer": {
    "layer": "dendrites"
  },
  "layout": "3d"
}
)json");
  const QString shareUrl =
    QStringLiteral("https://neuroglancer-demo.appspot.com/#!") + QString::fromUtf8(QUrl::toPercentEncoding(stateText));

  const auto input = ZNeuroglancerState::parseInputText(shareUrl);
  ASSERT_EQ(input.status, ZNeuroglancerState::InputStatus::Parsed);

  const ZNeuroglancerState::ParseResult parsed = ZNeuroglancerState::parse(input.stateJson);
  ASSERT_EQ(parsed.layers.size(), 2U);
  EXPECT_EQ(parsed.selectedLayerName, QStringLiteral("dendrites"));

  const auto& seg = parsed.layers.at(1);
  ASSERT_EQ(seg.type, ZNeuroglancerState::LayerType::Segmentation);
  EXPECT_EQ(seg.name, QStringLiteral("dendrites"));
  EXPECT_EQ(seg.volumeUrl, QStringLiteral("precomputed://gs://liconn-public/ExPID82_1/segmentation/231030_agg_240123"));
  ASSERT_EQ(seg.segments.size(), 5U);

  std::map<uint64_t, bool> visibility;
  for (const auto& segment : seg.segments) {
    visibility[segment.id] = segment.visible;
  }
  EXPECT_EQ(visibility.at(79), true);
  EXPECT_EQ(visibility.at(1843), true);
  EXPECT_EQ(visibility.at(22772), true);
  EXPECT_EQ(visibility.at(104207), false);
  EXPECT_EQ(visibility.at(215852), false);
}

TEST(ZNeuroglancerState, ParseDatasourceUrlWrappedPrecomputedSources)
{
  const std::string text = R"json(
{
  "layers": [
    {
      "type": "image",
      "name": "lsfm",
      "source": {
        "url": "s3://bossdb-open-data/xu2024/M242_MoE_6mos_control/lsfm/|neuroglancer-precomputed:",
        "transform": {
          "matrix": [[1,0,0,0],[0,1,0,0],[0,0,-1,0]],
          "outputDimensions": {
            "x": [0.000001152, "m"],
            "y": [0.000001152, "m"],
            "z": [0.000005, "m"]
          }
        }
      }
    },
    {
      "type": "segmentation",
      "name": "M242_atlas",
      "visible": false,
      "source": {
        "url": "s3://bossdb-open-data/xu2024/M242_MoE_6mos_control/M242_atlas/|neuroglancer-precomputed:",
        "transform": {
          "matrix": [[1,0,0,0],[0,1,0,0],[0,0,-1,0]],
          "outputDimensions": {
            "x": [0.000001152, "m"],
            "y": [0.000001152, "m"],
            "z": [0.000005, "m"]
          }
        }
      },
      "segments": []
    }
  ],
  "selectedLayer": {
    "layer": "lsfm"
  }
}
)json";

  const json::value state = json::parse(text);
  const ZNeuroglancerState::ParseResult parsed = ZNeuroglancerState::parse(state);

  ASSERT_EQ(parsed.layers.size(), 2U);
  EXPECT_TRUE(parsed.warnings.isEmpty());
  EXPECT_EQ(parsed.selectedLayerName, QStringLiteral("lsfm"));

  {
    const auto& l = parsed.layers.at(0);
    EXPECT_EQ(l.type, ZNeuroglancerState::LayerType::Image);
    EXPECT_EQ(l.name, QStringLiteral("lsfm"));
    EXPECT_EQ(l.volumeUrl, QStringLiteral("s3://bossdb-open-data/xu2024/M242_MoE_6mos_control/lsfm/"));
    EXPECT_TRUE(l.visible);
  }

  {
    const auto& l = parsed.layers.at(1);
    EXPECT_EQ(l.type, ZNeuroglancerState::LayerType::Segmentation);
    EXPECT_EQ(l.name, QStringLiteral("M242_atlas"));
    EXPECT_EQ(l.volumeUrl, QStringLiteral("s3://bossdb-open-data/xu2024/M242_MoE_6mos_control/M242_atlas/"));
    EXPECT_FALSE(l.visible);
    EXPECT_TRUE(l.segments.empty());
  }
}

} // namespace nim
