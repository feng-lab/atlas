#include "zeigenutils.h"
#include "ztest.h"

TEST(fileread, text)
{
  using namespace nim;
  using namespace Eigen;

  try {
    QString file = getTestDataDir().filePath("img/fileread.txt");
    MatrixXd mat = ZEigenUtils::readMatrix(file, "", false, 0, "#");

    ASSERT_EQ(6, mat.rows());
    ASSERT_EQ(4, mat.cols());

    EXPECT_EQ(1., mat(0,0));
    EXPECT_EQ(2., mat(0,1));
    EXPECT_EQ(3., mat(0,2));
    EXPECT_EQ(4., mat(0,3));

    EXPECT_EQ(5., mat(1,0));
    EXPECT_EQ(6., mat(1,1));
    EXPECT_EQ(std::numeric_limits<double>::infinity(), mat(1,2));
    EXPECT_EQ(88.8, mat(1,3));

    EXPECT_EQ(2e6, mat(2,0));
    EXPECT_EQ(8., mat(2,1));
    EXPECT_EQ(90.5, mat(2,2));
    EXPECT_EQ(0., mat(2,3));

    EXPECT_EQ(2., mat(3,0));
    EXPECT_EQ(2., mat(3,1));
    EXPECT_EQ(5., mat(3,2));
    EXPECT_EQ(7., mat(3,3));

    EXPECT_EQ(2., mat(4,0));
    EXPECT_EQ(7., mat(4,1));
    EXPECT_EQ(8., mat(4,2));
    EXPECT_TRUE(mat(4,3) != mat(4,3));

    EXPECT_EQ(2., mat(5,0));
    EXPECT_EQ(-std::numeric_limits<double>::infinity(), mat(5,1));
    EXPECT_EQ(std::numeric_limits<double>::infinity(), mat(5,2));
    EXPECT_TRUE(mat(4,3) != mat(5,3));
  }
  catch (const ZIOException & e) {
    LOG(WARNING) << e.what();
  }
}

