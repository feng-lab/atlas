#include "zrundumpanimation3dschema.h"

#include "../version/version.h"
#include "zjson.h"
#include "zlog.h"
#include "zdoc.h"
#include "z3drenderingengine.h"
#include "zmeshdoc.h"
#include "zswcdoc.h"
#include "zimgdoc.h"
#include "zpunctadoc.h"
#include "zsvgdoc.h"
#include "zregionannotationdoc.h"
#include "znumericparameter.h"
#include "zoptionparameter.h"
#include "z3dglobalparameters.h"
#include "z3dtransformparameter.h"
#include "zimg.h"
#include <QTemporaryDir>
#include <QFile>
#include <QTextStream>
#include <QCoreApplication>
#include <QFileInfo>
#include <QDir>
#include <gflags/gflags.h>

DEFINE_bool(run_dump_animation3d_schema, false, "Dump Animation3D JSON Schema + capabilities and exit");
DEFINE_string(dump_output_dir,
              "",
              "Output directory for dumping schema/capabilities (files: animation3d.schema.json, capabilities.json). "
              "Default: current directory");

#if defined(__linux__)
DECLARE_bool(__use_EGL);
#endif

namespace nim {

namespace {

// Helper to construct a permissive schema for color values: either string or [r,g,b,a]
json::value makeColorSchema()
{
  json::object colorArraySchema;
  colorArraySchema["type"] = "array";
  {
    json::object items;
    items["type"] = "number";
    colorArraySchema["items"] = items;
  }
  // Allow either RGB or RGBA vectors (3 or 4 values)
  colorArraySchema["minItems"] = 3;
  colorArraySchema["maxItems"] = 4;

  json::object asString;
  asString["type"] = "string"; // e.g., "#RRGGBBAA" or named color; engine reads QColor too

  json::object anyOf;
  json::array opts;
  opts.emplace_back(asString);
  opts.emplace_back(colorArraySchema);
  anyOf["anyOf"] = opts;
  return anyOf;
}

// Forward: build schema from supplied parameter schemas and capability index
json::object buildSchema(const json::object& paramSchemas, const json::object& capabilitiesIndex)
{
  using nim::jsonToString;

  json::object root;
  root["$schema"] = "https://json-schema.org/draft/2020-12/schema";
  root["title"] = std::string("Atlas Animation3D Schema (") + GIT_VERSION + ")";
  root["type"] = "object";

  // $defs
  json::object defs;

  // EasingType: prefer permissive string; include known names as a hint via anyOf
  {
    json::object easing;
    json::array any;
    // Known names
    json::object known;
    known["type"] = "string";
    json::array enums;
    const char* names[] = {
      "Linear",       "Switch",       "InQuad",  "OutQuad",  "InOutQuad",  "OutInQuad",  "InCubic",   "OutCubic",
      "InOutCubic",   "OutInCubic",   "InQuart", "OutQuart", "InOutQuart", "OutInQuart", "InQuint",   "OutQuint",
      "InOutQuint",   "OutInQuint",   "InSine",  "OutSine",  "InOutSine",  "OutInSine",  "InExpo",    "OutExpo",
      "InOutExpo",    "OutInExpo",    "InCirc",  "OutCirc",  "InOutCirc",  "OutInCirc",  "InElastic", "OutElastic",
      "InOutElastic", "OutInElastic", "InBack",  "OutBack",  "InOutBack",  "OutInBack",  "InBounce",  "OutBounce",
      "InOutBounce",  "OutInBounce",  "InCurve", "OutCurve", "SineCurve",  "CosineCurve"};
    for (const char* n : names) {
      enums.emplace_back(n);
    }
    known["enum"] = enums;
    // Fallback any string
    json::object anyStr;
    anyStr["type"] = "string";
    any.emplace_back(known);
    any.emplace_back(anyStr);
    easing["anyOf"] = any;
    defs["EasingType"] = easing;
  }

  // Generic key
  {
    json::object key;
    key["type"] = "object";
    json::object props;
    json::object time;
    time["type"] = "number";
    time["minimum"] = 0;
    props["time"] = time;

    json::object typeProp;
    typeProp["$ref"] = "#/defs/EasingType";
    props["type"] = typeProp;

    // value can be any JSON (actual shape depends on the parameter type)
    json::object anyVal;
    json::array anyTypes;
    anyTypes.emplace_back("object");
    anyTypes.emplace_back("array");
    anyTypes.emplace_back("number");
    anyTypes.emplace_back("integer");
    anyTypes.emplace_back("string");
    anyTypes.emplace_back("boolean");
    anyVal["type"] = anyTypes;
    props["value"] = anyVal;

    key["properties"] = props;
    json::array req;
    req.emplace_back("time");
    req.emplace_back("type");
    req.emplace_back("value");
    key["required"] = req;
    key["additionalProperties"] = false;
    defs["GenericKey"] = key;
  }

  // Camera key extends generic with TCB fields
  {
    json::object key;
    json::array allOf;
    json::object ref;
    ref["$ref"] = "#/defs/GenericKey";
    allOf.emplace_back(ref);
    key["allOf"] = allOf;
    json::object props;
    auto addNum = [&](const char* n) {
      json::object o;
      o["type"] = "number";
      props[n] = o;
    };
    addNum("posTension");
    addNum("posContinuity");
    addNum("posBias");
    addNum("rotTension");
    addNum("rotContinuity");
    addNum("rotBias");
    key["properties"] = props;
    defs["CameraKey"] = key;
  }

  // ParamAnimation (keys optional; allow extra fields permissively)
  {
    json::object pa;
    pa["type"] = "object";
    json::object props;
    props["color"] = makeColorSchema();
    json::object keys;
    keys["type"] = "array";
    keys["items"] = json::object{
      {"$ref", "#/defs/GenericKey"}
    };
    props["keys"] = keys;
    pa["properties"] = props;
    pa["additionalProperties"] = true;
    defs["ParamAnimation"] = pa;
  }

  // CameraAnimation (keys optional)
  {
    json::object ca;
    ca["type"] = "object";
    json::object props;
    json::object keys;
    keys["type"] = "array";
    keys["items"] = json::object{
      {"$ref", "#/defs/CameraKey"}
    };
    props["keys"] = keys;
    ca["properties"] = props;
    ca["additionalProperties"] = true;
    defs["CameraAnimation"] = ca;
  }

  // ParamMap: map of "Name Type" -> ParamAnimation
  {
    json::object pm;
    pm["type"] = "object";
    json::object pat;
    pat["^[^\\s].+\\s+[^\\s]+$"] = json::object{
      {"$ref", "#/defs/ParamAnimation"}
    };
    pm["patternProperties"] = pat;
    pm["additionalProperties"] = true; // permissive: unknown keys tolerated by loader
    defs["ParamMap"] = pm;
  }

  // ZImgSource (permissive)
  {
    json::object zs;
    zs["type"] = "object";
    json::object props;
    props["filenames"] = json::object{
      {"type",     "array"                         },
      {"items",    json::object{{"type", "string"}}},
      {"minItems", 1                               }
    };
    props["catDim"] = json::object{
      {"type", "string"}
    };
    props["catScenes"] = json::object{
      {"type", "boolean"}
    };
    props["region"] = json::object{
      {"type", "object"}
    }; // engine-specific shape
    props["scene"] = json::object{
      {"type", "integer"}
    };
    props["format"] = json::object{
      {"type", "string"}
    };
    props["expandXY"] = json::object{
      {"type", "boolean"}
    };
    props["expandWithMaxValue"] = json::object{
      {"type", "boolean"}
    };
    zs["properties"] = props;
    json::array req;
    req.emplace_back("filenames");
    zs["required"] = req;
    zs["additionalProperties"] = true;
    defs["ZImgSource"] = zs;
  }

  // Attach dynamic parameter schemas and capability index if provided
  defs["ParameterSchemas"] = paramSchemas;
  defs["Capabilities"] = capabilitiesIndex;

  root["defs"] = defs; // draft 2020-12 uses $defs, but keep both for compatibility
  root["$defs"] = defs;

  // Animation3D object
  json::object anim;
  anim["type"] = "object";
  json::object props;
  props["Version"] = json::object{
    {"type", "number"}
  };

  // Doc: map of "Type id" -> string (path) or ZImgSource or generic object
  {
    json::object doc;
    doc["type"] = "object";
    json::object patt;
    // Accept any "Word Number" key; images use object value; other types often use file path string
    json::object anyOf;
    json::array arr;
    arr.emplace_back(json::object{
      {"type", "string"}
    });
    arr.emplace_back(json::object{
      {"$ref", "#/defs/ZImgSource"}
    });
    arr.emplace_back(json::object{
      {"type", "object"}
    });
    anyOf["anyOf"] = arr;
    patt["^[A-Za-z]+\\s+\\d+$"] = anyOf;
    doc["patternProperties"] = patt;
    doc["additionalProperties"] = false;
    props["Doc"] = doc;
  }

  props["Duration"] = json::object{
    {"type",    "number"},
    {"minimum", 0       }
  };

  // Global camera animation
  props["Camera 3DCamera"] = json::object{
    {"$ref", "#/defs/CameraAnimation"}
  };

  // Background/Axis/Lighting param maps
  {
    json::object patProps;
    patProps["^(Background|Axis|Lighting)$"] = json::object{
      {"$ref", "#/defs/ParamMap"}
    };
    // Per-object param maps keyed by numeric string id
    patProps["^\\d+$"] = json::object{
      {"$ref", "#/defs/ParamMap"}
    };
    anim["patternProperties"] = patProps;
  }

  anim["properties"] = props;
  anim["additionalProperties"] = true; // allow future extensions

  // Root properties
  json::object rootProps;
  rootProps["Animation3D"] = anim;
  root["properties"] = rootProps;
  json::array rootReq;
  rootReq.emplace_back("Animation3D");
  root["required"] = rootReq;
  root["additionalProperties"] = false;

  return root;
}

} // namespace

// Helpers to convert common parameter types to a JSON Schema for their value field
static json::object makeNumberSchema(double minV, double maxV, bool isInteger)
{
  json::object o;
  o["type"] = isInteger ? json::value_from("integer") : json::value_from("number");
  // Hints only to remain permissive: loader clamps/ranges; do not enforce limits in schema
  o["x-minimum"] = minV;
  o["x-maximum"] = maxV;
  return o;
}

static json::object schemaForParameter(const ZParameter& p)
{
  using namespace nim;
  // 3D transform (object/world transform): expose structured subfields
  if (auto tp = dynamic_cast<const Z3DTransformParameter*>(&p)) {
    auto makeVecN = [](int n) {
      json::object o;
      o["type"] = "array";
      o["minItems"] = n;
      o["maxItems"] = n;
      json::object item;
      item["type"] = "number";
      o["items"] = item;
      return o;
    };
    json::object o;
    o["type"] = "object";
    json::object props;
    // Canonical child jsonKeys (match engine serialization)
    {
      auto s = makeVecN(3);
      s["description"] = "Uniform or non-uniform scale factors [sx, sy, sz]";
      props["Scale Vec3"] = s;
    }
    {
      auto t = makeVecN(3);
      t["description"] = "Translation in world units [tx, ty, tz]";
      props["Translation Vec3"] = t;
    }
    {
      auto r = makeVecN(4);
      r["description"] = "Rotation as [degrees, axis_x, axis_y, axis_z]";
      props["Rotation Vec4"] = r; // angle(deg), axis x,y,z
    }
    {
      auto c = makeVecN(3);
      c["description"] = "Rotation/pivot center [cx, cy, cz]";
      props["Rotation Center Vec3"] = c;
    }
    o["properties"] = props;
    o["additionalProperties"] = false;
    return o;
  }
  // Bool
  if (auto b = dynamic_cast<const ZBoolParameter*>(&p)) {
    json::object o;
    o["type"] = "boolean";
    return o;
  }
  // Scalars
  if (auto ip = dynamic_cast<const ZIntParameter*>(&p)) {
    return makeNumberSchema(ip->rangeMin(), ip->rangeMax(), true);
  }
  if (auto fp = dynamic_cast<const ZFloatParameter*>(&p)) {
    return makeNumberSchema(fp->rangeMin(), fp->rangeMax(), false);
  }
  if (auto dp = dynamic_cast<const ZDoubleParameter*>(&p)) {
    return makeNumberSchema(dp->rangeMin(), dp->rangeMax(), false);
  }
  // Spans
  if (auto isp = dynamic_cast<const ZIntSpanParameter*>(&p)) {
    json::object o;
    o["type"] = "array";
    o["minItems"] = 2;
    o["maxItems"] = 2;
    json::array items;
    items.emplace_back(makeNumberSchema(isp->minimum(), isp->maximum(), true));
    items.emplace_back(makeNumberSchema(isp->minimum(), isp->maximum(), true));
    o["items"] = items;
    return o;
  }
  if (auto fsp = dynamic_cast<const ZFloatSpanParameter*>(&p)) {
    json::object o;
    o["type"] = "array";
    o["minItems"] = 2;
    o["maxItems"] = 2;
    json::array items;
    items.emplace_back(makeNumberSchema(fsp->minimum(), fsp->maximum(), false));
    items.emplace_back(makeNumberSchema(fsp->minimum(), fsp->maximum(), false));
    o["items"] = items;
    return o;
  }
  if (auto dsp = dynamic_cast<const ZDoubleSpanParameter*>(&p)) {
    json::object o;
    o["type"] = "array";
    o["minItems"] = 2;
    o["maxItems"] = 2;
    json::array items;
    items.emplace_back(makeNumberSchema(dsp->minimum(), dsp->maximum(), false));
    items.emplace_back(makeNumberSchema(dsp->minimum(), dsp->maximum(), false));
    o["items"] = items;
    return o;
  }
  // Vector types
  if (auto v2 = dynamic_cast<const ZVec2Parameter*>(&p)) {
    json::object o;
    o["type"] = "array";
    o["minItems"] = 2;
    o["maxItems"] = 2;
    auto minV = v2->rangeMin();
    auto maxV = v2->rangeMax();
    json::array items;
    for (int i = 0; i < 2; ++i) {
      items.emplace_back(makeNumberSchema(minV[i], maxV[i], false));
    }
    o["items"] = items;
    return o;
  }
  if (auto v3 = dynamic_cast<const ZVec3Parameter*>(&p)) {
    json::object o;
    o["type"] = "array";
    o["minItems"] = 3;
    o["maxItems"] = 3;
    auto minV = v3->rangeMin();
    auto maxV = v3->rangeMax();
    json::array items;
    for (int i = 0; i < 3; ++i) {
      items.emplace_back(makeNumberSchema(minV[i], maxV[i], false));
    }
    o["items"] = items;
    return o;
  }
  if (auto v4 = dynamic_cast<const ZVec4Parameter*>(&p)) {
    json::object o;
    o["type"] = "array";
    o["minItems"] = 4;
    o["maxItems"] = 4;
    auto minV = v4->rangeMin();
    auto maxV = v4->rangeMax();
    json::array items;
    for (int i = 0; i < 4; ++i) {
      items.emplace_back(makeNumberSchema(minV[i], maxV[i], false));
    }
    o["items"] = items;
    return o;
  }
  // Options
  if (auto opt = dynamic_cast<const ZStringIntOptionParameter*>(&p)) {
    // anyOf: known enums OR any string (permissive)
    json::object o;
    json::array any;
    json::object enumObj;
    enumObj["type"] = "string";
    json::array enums;
    for (const auto& s : opt->options()) {
      enums.emplace_back(s.toStdString());
    }
    enumObj["enum"] = enums;
    any.emplace_back(enumObj);
    json::object anyStr;
    anyStr["type"] = "string";
    any.emplace_back(anyStr);
    o["anyOf"] = any;
    return o;
  }
  // Fallback: allow any JSON
  json::object any;
  json::array anyTypes;
  anyTypes.emplace_back("object");
  anyTypes.emplace_back("array");
  anyTypes.emplace_back("number");
  anyTypes.emplace_back("integer");
  anyTypes.emplace_back("string");
  anyTypes.emplace_back("boolean");
  any["type"] = anyTypes;
  return any;
}

static QString writeTextFile(const QString& path, const QString& content)
{
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
    throw nim::ZException("Can not write file", nim::ZException::Option::CheckErrno);
  }
  QTextStream ts(&f);
  ts << content;
  f.close();
  return path;
}

static void makeSampleAssets(const QDir& dir,
                             QString& meshPath,
                             QString& swcPath,
                             QString& imgPath,
                             QString& punctaPath,
                             QString& svgPath)
{
  using namespace nim;
  // Mesh OBJ
  meshPath = dir.filePath("sample.obj");
  writeTextFile(meshPath,
                "v 0 0 0\n"
                "v 1 0 0\n"
                "v 0 1 0\n"
                "f 1 2 3\n");
  // SWC
  swcPath = dir.filePath("sample.swc");
  writeTextFile(swcPath, "# sample swc\n1 1 0 0 0 1 -1\n2 3 1 0 0 1 1\n");
  // Image PNG
  imgPath = dir.filePath("sample.tif");
  // Create a small RGBA image to avoid single-channel conversion asserts in PNG writer
  ZImgInfo iinfo(16, 16, 1, 4, 1, 1, VoxelFormat::Unsigned);
  iinfo.lastChannelIsAlphaChannel = true;
  ZImg img(iinfo);
  // Fill with a simple gradient and opaque alpha
  for (int y = 0; y < 16; ++y) {
    for (int x = 0; x < 16; ++x) {
      img.setValue(uint8_t(x * 16), x, y, 0, 0);
      img.setValue(uint8_t(y * 16), x, y, 0, 1);
      img.setValue(uint8_t(128), x, y, 0, 2);
      img.setValue(uint8_t(255), x, y, 0, 3);
    }
  }
  img.save(imgPath);
  // Puncta marker
  punctaPath = dir.filePath("sample.marker");
  writeTextFile(punctaPath, "0,0,0,2, ,name,comment,255,0,0\n");
  // SVG
  svgPath = dir.filePath("sample.svg");
  writeTextFile(svgPath,
                "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>\n"
                "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"16\" height=\"16\">\n"
                "  <rect x=\"1\" y=\"1\" width=\"14\" height=\"14\" stroke=\"black\" fill=\"none\"/>\n"
                "</svg>\n");
}

int ZRunDumpAnimation3DSchema::run()
{
  try {
    // Ensure we can create a valid OpenGL context in headless/Linux console mode.
    // Match the setup used by ZRunExport3DAnimation to avoid QOffscreenSurface failures
    // and prefer an EGL-based context when running without a GUI.
#if defined(__linux__)
    FLAGS___use_EGL = true;
#endif
    // Resolve output directory
    QDir outDir(QString::fromStdString(FLAGS_dump_output_dir).trimmed());
    if (outDir.path().isEmpty()) {
      outDir = QDir::current();
    }
    if (!outDir.exists()) {
      if (!outDir.mkpath(".")) {
        throw nim::ZException("Failed to create output directory for schema dump");
      }
    }
    const QString schemaOut = outDir.filePath("animation3d.schema.json");
    const QString capsOut = outDir.filePath("capabilities.json");

    // Build capability catalog by introspecting parameters from live engine/views
    nim::ZDoc doc;
    // Create sample assets on disk
    QTemporaryDir tmpDir;
    if (!tmpDir.isValid()) {
      throw nim::ZException("Failed to create temporary directory");
    }
    QString meshPath, swcPath, imgPath, punctaPath, svgPath;
    makeSampleAssets(QDir(tmpDir.path()), meshPath, swcPath, imgPath, punctaPath, svgPath);
    // Load assets
    doc.loadFile(meshPath);
    doc.loadFile(swcPath);
    doc.loadFile(imgPath);
    doc.loadFile(punctaPath);
    doc.loadFile(svgPath);
    // Ensure one RegionAnnotation exists even without a file
    doc.regionAnnotationDoc().currentRegionAnnotationPack();

    // Create engine and init
    nim::Z3DRenderingEngine engine(doc);
    engine.init();

    // Collect capabilities and parameter schemas
    json::object paramSchemas; // jsonKey -> schema
    json::object capabilities;
    capabilities["version"] = std::string(GIT_VERSION);
    json::object globals;
    auto collect = [&](const std::vector<nim::ZParameter*>& params, json::array& keyList, json::array* fullList) {
      for (auto* p : params) {
        if (!p) {
          continue;
        }
        auto key = p->jsonKey();
        keyList.emplace_back(key.toStdString());
        // Insert schema if first time
        auto k = key.toStdString();
        if (!paramSchemas.contains(k)) {
          json::object ps = schemaForParameter(*p);
          paramSchemas[k] = ps;
        }
        if (fullList) {
          json::object meta;
          meta["jsonKey"] = key.toStdString();
          meta["name"] = p->name().toStdString();
          meta["type"] = p->type().toStdString();
          meta["supportsInterpolation"] = p->supportInterpolation();
          meta["valueSchema"] = paramSchemas[k];
          fullList->emplace_back(meta);
        }
      }
    };

    // Global groups: Background(1), Axis(2), Global(3)
    {
      json::array bgKeys, axisKeys, globalKeys;
      json::array bgFull, axisFull, globalFull;
      collect(engine.parametersOfViewSetting(1), bgKeys, &bgFull);
      collect(engine.parametersOfViewSetting(2), axisKeys, &axisFull);
      collect(engine.parametersOfViewSetting(3), globalKeys, &globalFull);
      {
        json::object o;
        o["keys"] = bgKeys;
        o["parameters"] = bgFull;
        globals["Background"] = o;
      }
      {
        json::object o;
        o["keys"] = axisKeys;
        o["parameters"] = axisFull;
        globals["Axis"] = o;
      }
      {
        json::object o;
        o["keys"] = globalKeys;
        o["parameters"] = globalFull;
        globals["Global"] = o;
      }
    }

    // Objects: gather first instance id per type
    json::object objects;
    std::map<QString, size_t> typeToId;
    for (auto id : doc.objs()) {
      auto* d = doc.idToDoc(id);
      auto tn = d->typeName();
      if (!typeToId.contains(tn)) {
        typeToId[tn] = id;
      }
    }
    for (const auto& [tn, id] : typeToId) {
      json::array keyList, fullList;
      collect(engine.parametersOfViewSetting(id), keyList, &fullList);
      {
        json::object o;
        o["keys"] = keyList;
        o["parameters"] = fullList;
        objects[tn.toStdString()] = o;
      }
    }

    capabilities["globals"] = globals;
    capabilities["objects"] = objects;

    // Save capabilities.json (full metadata)
    saveJsonObject(capabilities, capsOut);

    // Prepare a compact index to embed into schema ($defs.Capabilities): only key lists
    json::object capsIndex;
    json::object gi;
    gi["Background"] = globals.at("Background").as_object().at("keys");
    gi["Axis"] = globals.at("Axis").as_object().at("keys");
    gi["Global"] = globals.at("Global").as_object().at("keys");
    capsIndex["globals"] = gi;
    json::object oi;
    for (auto& [k, v] : objects) {
      oi[k] = v.as_object().at("keys");
    }
    capsIndex["objects"] = oi;

    // Dump the schema with precise parameter value schemas and capability index under $defs
    LOG(INFO) << "Dumping Animation3D schema to " << schemaOut;
    auto schema = buildSchema(paramSchemas, capsIndex);
    saveJsonObject(schema, schemaOut);
    LOG(INFO) << "Schema and capabilities saved.";
    return 0;
  }
  catch (const std::exception& e) {
    LOG(ERROR) << "Failed to dump Animation3D schema: " << e.what();
    return 1;
  }
}

} // namespace nim
