#pragma once

#include "z3dcompositor.h"
#include "zlog.h"
#include "zvulkanreadbackretirement_p.h"

#include <QPointer>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

namespace nim {

// Internal, move-only value carried from Vulkan submission through compositor
// publication. Until consumer ownership is transferred to a local color
// buffer, destruction releases that ownership exactly once.
class ZVulkanFinalReadbackCompletion final
{
public:
  enum class PublicationDecision : uint8_t
  {
    Accept,
    OwnerUnavailable,
    OwnerRevisionMismatch,
    ExtentMismatch,
    BackendMismatch,
    RenderFrameStale,
  };

  ZVulkanFinalReadbackCompletion(QPointer<Z3DCompositor> owner,
                                 uint64_t ownerRevision,
                                 uint64_t renderFrameToken,
                                 const glm::uvec2& outputSize,
                                 const glm::uvec2& readbackSize,
                                 const void* mapped,
                                 Z3DEye eye,
                                 Z3DLocalColorBuffer* localBuffer,
                                 Z3DScratchResourcePool::RenderTargetLease* target,
                                 bool noCopy,
                                 std::shared_ptr<ZVulkanReadbackRetirement> retirement)
    : owner(std::move(owner))
    , ownerRevision(ownerRevision)
    , renderFrameToken(renderFrameToken)
    , outputSize(outputSize)
    , readbackSize(readbackSize)
    , mapped(mapped)
    , eye(eye)
    , localBuffer(localBuffer)
    , target(target)
    , noCopy(noCopy)
    , m_retirement(std::move(retirement))
  {
    CHECK(mapped != nullptr) << "Vulkan final readback completion requires mapped staging data";
    CHECK(localBuffer != nullptr) << "Vulkan final readback completion requires a destination local buffer";
    CHECK(target != nullptr) << "Vulkan final readback completion requires a destination target";
    CHECK(m_retirement != nullptr) << "Vulkan final readback completion requires staging-slot retirement";
  }

  ZVulkanFinalReadbackCompletion(const ZVulkanFinalReadbackCompletion&) = delete;
  ZVulkanFinalReadbackCompletion& operator=(const ZVulkanFinalReadbackCompletion&) = delete;

  ZVulkanFinalReadbackCompletion(ZVulkanFinalReadbackCompletion&& other) noexcept
    : owner(other.owner)
    , ownerRevision(other.ownerRevision)
    , renderFrameToken(other.renderFrameToken)
    , outputSize(other.outputSize)
    , readbackSize(other.readbackSize)
    , mapped(other.mapped)
    , eye(other.eye)
    , localBuffer(other.localBuffer)
    , target(other.target)
    , noCopy(other.noCopy)
    , m_retirement(std::move(other.m_retirement))
  {}

  ZVulkanFinalReadbackCompletion& operator=(ZVulkanFinalReadbackCompletion&&) = delete;

  ~ZVulkanFinalReadbackCompletion()
  {
    retire();
  }

  [[nodiscard]] static PublicationDecision publicationDecision(bool ownerAvailable,
                                                               uint64_t expectedOwnerRevision,
                                                               uint64_t currentOwnerRevision,
                                                               const glm::uvec2& expectedExtent,
                                                               const glm::uvec2& readbackExtent,
                                                               const glm::uvec2& currentExtent,
                                                               RenderBackend currentBackend,
                                                               uint64_t renderFrameToken,
                                                               uint64_t lastPublishedRenderFrameToken)
  {
    if (!ownerAvailable) {
      return PublicationDecision::OwnerUnavailable;
    }
    if (expectedOwnerRevision != currentOwnerRevision) {
      return PublicationDecision::OwnerRevisionMismatch;
    }
    if (expectedExtent != readbackExtent || expectedExtent != currentExtent) {
      return PublicationDecision::ExtentMismatch;
    }
    if (currentBackend != RenderBackend::Vulkan) {
      return PublicationDecision::BackendMismatch;
    }
    if (renderFrameToken != 0u && lastPublishedRenderFrameToken != 0u &&
        renderFrameToken < lastPublishedRenderFrameToken) {
      return PublicationDecision::RenderFrameStale;
    }
    return PublicationDecision::Accept;
  }

  void transferRetirementTo(std::function<void()>& destination)
  {
    CHECK(!destination) << "Vulkan readback retirement destination must be empty";
    CHECK(m_retirement != nullptr) << "Vulkan readback retirement was already transferred";
    // Build the potentially allocating callback before relinquishing this
    // object's ownership. If allocation throws, this object still retires the
    // consumer side during destruction.
    auto retirement = m_retirement;
    std::function<void()> transfer = [retirement = std::move(retirement)]() noexcept {
      retirement->notifyConsumerFinished();
    };
    destination = std::move(transfer);
    m_retirement.reset();
  }

  const QPointer<Z3DCompositor> owner;
  const uint64_t ownerRevision;
  const uint64_t renderFrameToken;
  const glm::uvec2 outputSize;
  const glm::uvec2 readbackSize;
  const void* const mapped;
  const Z3DEye eye;
  Z3DLocalColorBuffer* const localBuffer;
  Z3DScratchResourcePool::RenderTargetLease* const target;
  const bool noCopy;

private:
  void retire()
  {
    if (!m_retirement) {
      return;
    }
    auto retirement = std::move(m_retirement);
    retirement->notifyConsumerFinished();
  }

  std::shared_ptr<ZVulkanReadbackRetirement> m_retirement;
};

} // namespace nim
