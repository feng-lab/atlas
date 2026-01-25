#include "zcutspanparameter.h"

#include "zparameteranimation.h"
#include "zparameterkey.h"
#include "ztest.h"

using namespace nim;

TEST(ZCutSpanParameterTest, SetValueSameAsCopiesBindingFields)
{
  ZCutSpanParameter src("Global X Cut", glm::vec2(0.0f, 0.0f), 0.0f, 0.0f);
  src.setMode(ZCutSpanParameter::Mode::Absolute);
  src.setPins(false, false);
  src.applyBounds(0.0, 200.0);
  src.set(glm::vec2(10.0f, 190.0f));
  src.setNormalized(glm::dvec2(0.25, 0.75));

  ZCutSpanParameter dst("Global X Cut", glm::vec2(0.0f, 0.0f), 0.0f, 200.0f);
  EXPECT_EQ(dst.mode(), ZCutSpanParameter::Mode::TrackEdges);
  EXPECT_TRUE(dst.pinLower());
  EXPECT_TRUE(dst.pinUpper());

  dst.setValueSameAs(src);

  EXPECT_EQ(dst.mode(), ZCutSpanParameter::Mode::Absolute);
  EXPECT_FALSE(dst.pinLower());
  EXPECT_FALSE(dst.pinUpper());
  EXPECT_DOUBLE_EQ(dst.normalized()[0], src.normalized()[0]);
  EXPECT_DOUBLE_EQ(dst.normalized()[1], src.normalized()[1]);
  EXPECT_FLOAT_EQ(dst.minimum(), 0.0f);
  EXPECT_FLOAT_EQ(dst.maximum(), 200.0f);
  EXPECT_FLOAT_EQ(dst.get()[0], 10.0f);
  EXPECT_FLOAT_EQ(dst.get()[1], 190.0f);
}

TEST(ZCutSpanParameterTest, ParameterKeyConstructorCopiesRangeAndValue)
{
  ZCutSpanParameter src("Global X Cut", glm::vec2(0.0f, 0.0f), 0.0f, 0.0f);
  src.setMode(ZCutSpanParameter::Mode::Absolute);
  src.setPins(false, false);
  src.applyBounds(0.0, 200.0);
  src.set(glm::vec2(10.0f, 190.0f));

  ZParameterKey key(1.0, src);
  auto* asCut = dynamic_cast<ZCutSpanParameter*>(&key.value());
  ASSERT_TRUE(asCut);

  EXPECT_EQ(asCut->mode(), ZCutSpanParameter::Mode::Absolute);
  EXPECT_FALSE(asCut->pinLower());
  EXPECT_FALSE(asCut->pinUpper());
  EXPECT_FLOAT_EQ(asCut->minimum(), 0.0f);
  EXPECT_FLOAT_EQ(asCut->maximum(), 200.0f);
  EXPECT_FLOAT_EQ(asCut->get()[0], 10.0f);
  EXPECT_FLOAT_EQ(asCut->get()[1], 190.0f);
}

TEST(ZCutSpanParameterTest, ReadValueSeedsRangeWhenUnbound)
{
  ZCutSpanParameter p("Global X Cut");
  EXPECT_FLOAT_EQ(p.minimum(), 0.0f);
  EXPECT_FLOAT_EQ(p.maximum(), 0.0f);

  json::object obj;
  obj["Mode StringIntOption"] = "Absolute";
  obj["Pin Lower Bool"] = false;
  obj["Pin Upper Bool"] = false;
  obj["Normalized [0..1] DoubleSpan"] = json::array{0.0, 1.0};
  obj["Range FloatSpan"] = json::array{1767.0, 13173.0};

  p.readValue(obj);

  EXPECT_FLOAT_EQ(p.minimum(), 1767.0f);
  EXPECT_FLOAT_EQ(p.maximum(), 13173.0f);
  EXPECT_FLOAT_EQ(p.get()[0], 1767.0f);
  EXPECT_FLOAT_EQ(p.get()[1], 13173.0f);
  EXPECT_EQ(p.mode(), ZCutSpanParameter::Mode::Absolute);
  EXPECT_FALSE(p.pinLower());
  EXPECT_FALSE(p.pinUpper());
}

TEST(ZParameterAnimationTest, RemoveRedundantKeysAlwaysKeepsLastKey)
{
  // Use a trivial float track: four identical keys should compress to just the
  // explicit endpoints (first + last).
  ZFloatParameter p("P", 5.0f, 0.0f, 10.0f);
  ZParameterAnimation anim("P", p.type());
  anim.addKey(std::make_unique<ZParameterKey>(0.0, p), /*keepRedundant=*/true);
  anim.addKey(std::make_unique<ZParameterKey>(1.0, p), /*keepRedundant=*/true);
  anim.addKey(std::make_unique<ZParameterKey>(2.0, p), /*keepRedundant=*/true);
  anim.addKey(std::make_unique<ZParameterKey>(3.0, p), /*keepRedundant=*/true);

  anim.removeRedundantKeys();

  ASSERT_EQ(anim.numKeys(), 2);
  EXPECT_DOUBLE_EQ(anim.keys().front()->time(), 0.0);
  EXPECT_DOUBLE_EQ(anim.keys().back()->time(), 3.0);
  EXPECT_TRUE(anim.keys().front()->value().jsonValue() == anim.keys().back()->value().jsonValue());
}
