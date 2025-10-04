#pragma once

#include <QImage>
#include <QString>
#include <cstdint>
#include <vector>

namespace nim {

class Z3DSDFont
{
public:
  struct CharInfo
  {
    explicit CharInfo(int id_ = 0,
                      int x_ = 0,
                      int y_ = 0,
                      int width_ = 0,
                      int height_ = 0,
                      float xoffset_ = 0.f,
                      float yoffset_ = 0.f,
                      float xadvance_ = 0.f,
                      int page_ = 0,
                      int chnl_ = 0,
                      int texWidth = 1,
                      int texHeight = 1)
      : id(id_)
      , x(x_)
      , y(y_)
      , width(width_)
      , height(height_)
      , xoffset(xoffset_)
      , yoffset(yoffset_)
      , xadvance(xadvance_)
      , page(page_)
      , chnl(chnl_)
    {
      sMin = static_cast<float>(x) / static_cast<float>(texWidth);
      tMin = static_cast<float>(y + height) / static_cast<float>(texHeight);

      sMax = static_cast<float>(x + width) / static_cast<float>(texWidth);
      tMax = static_cast<float>(y) / static_cast<float>(texHeight);
    }

    int id;
    int x;
    int y;
    int width;
    int height;
    float xoffset;
    float yoffset;
    float xadvance;
    int page;
    int chnl;

    float sMin;
    float sMax;
    float tMin;
    float tMax;
  };

  Z3DSDFont(QString imageFileName, QString txtFileName);

  [[nodiscard]] QString fontName() const
  {
    return m_fontName;
  }

  [[nodiscard]] int maxFontHeight() const
  {
    return m_maxFontHeight;
  }

  [[nodiscard]] bool isEmpty() const
  {
    return m_isEmpty;
  }

  [[nodiscard]] CharInfo charInfo(int id) const;

  // CPU atlas accessors (BGRA8 in QImage memory layout)
  [[nodiscard]] const uint8_t* atlasPixelsBGRA8() const
  {
    return reinterpret_cast<const uint8_t*>(m_GLFormattedImage.bits());
  }

  [[nodiscard]] uint32_t atlasWidth() const
  {
    return static_cast<uint32_t>(m_GLFormattedImage.width());
  }

  [[nodiscard]] uint32_t atlasHeight() const
  {
    return static_cast<uint32_t>(m_GLFormattedImage.height());
  }

protected:
  void loadImage();

  void parseFontFile();

private:
  QString m_imageFileName;
  QImage m_GLFormattedImage;
  QString m_txtFileName;

  QString m_fontName;
  bool m_isEmpty; // if load image or txt failed, the font is empty
  std::vector<CharInfo> m_charInfos;
  int m_maxFontHeight;
};

} // namespace nim
