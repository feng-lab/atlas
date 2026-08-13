#pragma once

#include "zglmutils.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace nim {

// Pure spatial tile geometry shared by mono or both eyes of a stereo attempt.
// The stored output rectangle is the single source of truth. Output
// coordinates are bottom-left-origin and half-open; attachment, expanded, and
// normalized geometry is derived from that checked rectangle and guard width.
class Z3DTileDescriptor
{
public:
  Z3DTileDescriptor(glm::uvec2 fullOutputExtent,
                    glm::uvec2 validOutputOrigin,
                    glm::uvec2 validOutputExtent,
                    uint32_t guardPixels);

  [[nodiscard]] bool operator==(const Z3DTileDescriptor&) const noexcept = default;

  [[nodiscard]] const glm::uvec2& fullOutputExtent() const noexcept
  {
    return m_fullOutputExtent;
  }

  [[nodiscard]] const glm::uvec2& validOutputOrigin() const noexcept
  {
    return m_validOutputOrigin;
  }

  [[nodiscard]] const glm::uvec2& validOutputExtent() const noexcept
  {
    return m_validOutputExtent;
  }

  [[nodiscard]] uint32_t guardPixels() const noexcept
  {
    return m_guardPixels;
  }

  [[nodiscard]] glm::uvec2 attachmentExtent() const noexcept
  {
    return glm::uvec2(
      static_cast<uint32_t>(static_cast<uint64_t>(m_validOutputExtent.x) + 2u * static_cast<uint64_t>(m_guardPixels)),
      static_cast<uint32_t>(static_cast<uint64_t>(m_validOutputExtent.y) + 2u * static_cast<uint64_t>(m_guardPixels)));
  }

  [[nodiscard]] glm::uvec2 validAttachmentOrigin() const noexcept
  {
    return glm::uvec2(m_guardPixels);
  }

  [[nodiscard]] glm::uvec2 validAttachmentEnd() const noexcept
  {
    return validAttachmentOrigin() + m_validOutputExtent;
  }

  [[nodiscard]] double normalizedLeft() const noexcept
  {
    return static_cast<double>(expandedRenderOrigin().x) / m_fullOutputExtent.x;
  }

  [[nodiscard]] double normalizedRight() const noexcept
  {
    return static_cast<double>(expandedRenderOrigin().x + attachmentExtent().x) / m_fullOutputExtent.x;
  }

  [[nodiscard]] double normalizedBottom() const noexcept
  {
    return static_cast<double>(expandedRenderOrigin().y) / m_fullOutputExtent.y;
  }

  [[nodiscard]] double normalizedTop() const noexcept
  {
    return static_cast<double>(expandedRenderOrigin().y + attachmentExtent().y) / m_fullOutputExtent.y;
  }

  // Converts the descriptor's bottom-left output rectangle to the origin at
  // which a top-left-oriented tile image must be pasted into a top-left frame.
  [[nodiscard]] glm::uvec2 topLeftAssemblyOrigin() const noexcept
  {
    return glm::uvec2(m_validOutputOrigin.x, m_fullOutputExtent.y - validOutputEnd().y);
  }

  [[nodiscard]] bool containsTopLeftOutputPixel(glm::uvec2 pixel) const noexcept
  {
    const glm::uvec2 origin = topLeftAssemblyOrigin();
    return pixel.x >= origin.x && pixel.x < origin.x + m_validOutputExtent.x && pixel.y >= origin.y &&
           pixel.y < origin.y + m_validOutputExtent.y;
  }

  // Maps an owned top-left-oriented output pixel to this tile's guarded,
  // top-left-oriented attachment. The first valid output pixel starts after
  // the leading guard in both dimensions.
  [[nodiscard]] glm::uvec2 topLeftAttachmentPixel(glm::uvec2 pixel) const
  {
    CHECK(containsTopLeftOutputPixel(pixel)) << "Top-left output pixel must belong to this tile";
    return validAttachmentOrigin() + pixel - topLeftAssemblyOrigin();
  }

private:
  [[nodiscard]] glm::uvec2 validOutputEnd() const noexcept
  {
    return m_validOutputOrigin + m_validOutputExtent;
  }

  [[nodiscard]] glm::i64vec2 expandedRenderOrigin() const noexcept
  {
    return glm::i64vec2(static_cast<int64_t>(m_validOutputOrigin.x) - static_cast<int64_t>(m_guardPixels),
                        static_cast<int64_t>(m_validOutputOrigin.y) - static_cast<int64_t>(m_guardPixels));
  }

  glm::uvec2 m_fullOutputExtent;
  glm::uvec2 m_validOutputOrigin;
  glm::uvec2 m_validOutputExtent;
  uint32_t m_guardPixels;
};

// Returns every tile exactly once in bottom-row-first serpentine order.
[[nodiscard]] std::vector<Z3DTileDescriptor>
makeZ3DTileDescriptors(glm::uvec2 fullOutputExtent, glm::uvec2 tileExtent, uint32_t guardPixels);

// Splits the output into stable, left-to-right vertical regions. Every region
// spans the full output height and retains the descriptor's normal guard.
[[nodiscard]] std::vector<Z3DTileDescriptor>
makeZ3DFixedRegionDescriptors(glm::uvec2 fullOutputExtent, size_t regionCount, uint32_t guardPixels);

} // namespace nim
