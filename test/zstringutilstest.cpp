#include "zstringutils.h"
#include "ztest.h"

namespace nim {

TEST(RemoveCommentTest, NoComment)
{
  // No comment in the line
  EXPECT_EQ(removeComment("This is a line without comment", "#", false), "This is a line without comment");
  EXPECT_EQ(removeComment("Another line without comment", "//", false), "Another line without comment");
  EXPECT_EQ(removeComment("Hello, world!", "#"sv, true), "Hello, world!");
}

TEST(RemoveCommentTest, SimpleComment)
{
  // Simple comment cases
  EXPECT_EQ(removeComment("This is a line with comment # here", "#", false), "This is a line with comment ");
  EXPECT_EQ(removeComment("Code with comment // test comment", "//", false), "Code with comment ");
  EXPECT_EQ(removeComment("Hello, world! # This is a comment", "#"sv, true), "Hello, world! ");
}

TEST(RemoveCommentTest, SpecialNumberComment)
{
  // Comment starting after special number should be preserved
  EXPECT_EQ(removeComment("Value = 1.#INF # comment", "#", true), "Value = 1.#INF ");
  EXPECT_EQ(removeComment("Value = 1.#SNAN # another comment", "#", true), "Value = 1.#SNAN ");
  EXPECT_EQ(removeComment("Value = 1.#QNAN // not a comment", "//", true), "Value = 1.#QNAN ");
  EXPECT_EQ(removeComment("Value = 1.#IND # yet another comment", "#", true), "Value = 1.#IND ");
}

TEST(RemoveCommentTest, SpecialNumberWithoutCheck)
{
  // Special numbers should not be preserved when checkSpecialNumber is false
  EXPECT_EQ(removeComment("Value = 1.#INF # comment", "#", false), "Value = 1.");
  EXPECT_EQ(removeComment("Value = 1.#SNAN # another comment", "#", false), "Value = 1.");
  EXPECT_EQ(removeComment("Value = 1.#QNAN // not a comment", "//", false), "Value = 1.#QNAN ");
  EXPECT_EQ(removeComment("Value = 1.#IND # yet another comment", "#", false), "Value = 1.");
}

TEST(RemoveCommentTest, MultipleComments)
{
  // Line with multiple comments
  EXPECT_EQ(removeComment("Code with # comment // another comment", "#", false), "Code with ");
  EXPECT_EQ(removeComment("Code with #INF special number # another comment", "#", true), "Code with ");
}

TEST(RemoveCommentTest, DifferentCommentStart)
{
  // Different comment start strings
  EXPECT_EQ(removeComment("This is a line // with a different comment start", "//", false), "This is a line ");
  EXPECT_EQ(removeComment("A comment with no special numbers; here", ";", false), "A comment with no special numbers");
  EXPECT_EQ(removeComment("Hello, world! // This is a comment", "//"sv, false), "Hello, world! ");
}

TEST(RemoveCommentTest, EmptyString)
{
  EXPECT_EQ(removeComment("", "#"sv, true), "");
}

TEST(RemoveCommentTest, OnlyComment)
{
  EXPECT_EQ(removeComment("# This is a comment", "#"sv, true), "");
}

TEST(RemoveCommentTest, CommentAtStart)
{
  EXPECT_EQ(removeComment("# This is a comment\nHello, world!", "#"sv, true), "");
}

TEST(RemoveCommentTest, SpecialNumberINF)
{
  EXPECT_EQ(removeComment("Value: 1.#INF # This is a comment", "#"sv, true), "Value: 1.#INF ");
}

TEST(RemoveCommentTest, SpecialNumberSNAN)
{
  EXPECT_EQ(removeComment("Value: 1.#SNAN # This is a comment", "#"sv, true), "Value: 1.#SNAN ");
}

TEST(RemoveCommentTest, SpecialNumberQNAN)
{
  EXPECT_EQ(removeComment("Value: 1.#QNAN # This is a comment", "#"sv, true), "Value: 1.#QNAN ");
}

TEST(RemoveCommentTest, SpecialNumberIND)
{
  EXPECT_EQ(removeComment("Value: 1.#IND # This is a comment", "#"sv, true), "Value: 1.#IND ");
}

TEST(RemoveCommentTest, MultipleSpecialNumbers)
{
  EXPECT_EQ(removeComment("Value1: 1.#INF Value2: 1.#SNAN # This is a comment", "#"sv, true),
            "Value1: 1.#INF Value2: 1.#SNAN ");
}

TEST(RemoveCommentTest, SpecialNumbersWithCheckSpecialNumberFalse)
{
  EXPECT_EQ(removeComment("Value: 1.#INF # This is a comment", "#"sv, false), "Value: 1.");
}

TEST(RemoveCommentTest, SpecialNumbersWithDifferentCommentStart)
{
  EXPECT_EQ(removeComment("Value: 1.#INF // This is a comment", "//"sv, true), "Value: 1.#INF ");
}

TEST(RemoveCommentTest, CaseInsensitiveSpecialNumbers)
{
  EXPECT_EQ(removeComment("Value1: 1.#inf Value2: 1.#SNAN # This is a comment", "#"sv, true),
            "Value1: 1.#inf Value2: 1.#SNAN ");
}

} // namespace nim
