#pragma once

#include <QString>
#include <QStringList>

#include <boost/json/value.hpp>

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
    LayerType type = LayerType::Image;
    QString name;
    QString volumeUrl; // precomputed://... (root)
    bool visible = true;
    std::optional<double> opacity; // [0,1] if present in state

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
    QStringList warnings;
  };

  // Parses a Neuroglancer "viewer state" JSON object and extracts:
  // - Precomputed image/segmentation volume URLs to open.
  // - Optional mesh/skeleton/annotations source URLs to register as dataset overrides.
  //
  // Unsupported layers are ignored (warnings are returned).
  static ParseResult parse(const boost::json::value& stateJson);
};

} // namespace nim

