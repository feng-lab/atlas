#pragma once

#include <QString>
#include <QStringList>

#include <boost/json/value.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace nim {

class ZNeuroglancerState
{
public:
  enum class LayerType
  {
    Image,
    Segmentation,
  };

  struct Layer
  {
    struct Segment
    {
      uint64_t id = 0;
      bool visible = true;
    };

    LayerType type = LayerType::Image;
    QString name;
    QString volumeUrl; // precomputed://... (root)
    bool visible = true;
    std::optional<double> opacity; // [0,1] if present in state
    std::vector<Segment> segments; // segmentation-only; mirrors Neuroglancer's "segments" field

    // Optional per-dataset external sources discovered from the state.
    // These are stored as override URLs on the segmentation dataset.
    QString meshSourceOverrideUrl;
    QString skeletonSourceOverrideUrl;
  };

  // Annotation layers are not opened as objects in Atlas yet, but we can still
  // use them to register a per-dataset annotations source override on the linked
  // segmentation layer(s).
  struct AnnotationsBinding
  {
    QString annotationsRootUrl; // precomputed://... (root)
    QStringList linkedSegmentationLayerNames;
  };

  struct ParseResult
  {
    std::vector<Layer> layers; // only image+segmentation layers that can be opened as volumes
    std::vector<AnnotationsBinding> annotationsBindings;
    QString selectedLayerName; // layer name from top-level selectedLayer.layer, if present
    QStringList warnings;
  };

  enum class InputStatus
  {
    NotRecognized,
    Parsed,
    Error,
  };

  struct InputParseResult
  {
    InputStatus status = InputStatus::NotRecognized;
    boost::json::value stateJson;
    QString error;
  };

  // Parses a Neuroglancer "viewer state" JSON object and extracts:
  // - Precomputed image/segmentation volume URLs to open.
  // - Segmentation visible/hidden segment IDs from the Neuroglancer "segments" field.
  // - Optional mesh/skeleton/annotations source URLs to register as dataset overrides.
  //
  // Unsupported layers are ignored (warnings are returned).
  static ParseResult parse(const boost::json::value& stateJson);

  // Parses Neuroglancer input text into state JSON. Supported inputs:
  // - Raw state JSON
  // - Neuroglancer share links with '#!{...}' fragments
  // - HTTP/HTTPS/gs/s3 URLs to JSON state files
  //
  // Returns:
  // - Parsed on success
  // - NotRecognized if the input does not look like a Neuroglancer state
  // - Error if it looks like a state input but fetch/decode/parse fails
  static InputParseResult parseInputText(const QString& text,
                                         std::chrono::milliseconds timeout = std::chrono::milliseconds{30000});
};

} // namespace nim
