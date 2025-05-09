#pragma once

#include <geodesk/geodesk.h>
#include <vtzero/builder.hpp>
#include "tileId.h"
#include "clipper.h"

using geodesk::Feature;
using geodesk::Features;
using geodesk::Mercator;
using geodesk::Coordinate;
using geodesk::TagValue;

using dvec2 = linalg::dvec2;
using i32vec2 = linalg::i32vec2;

template<typename T> constexpr T squared(T x) { return x*x; }

#define LOG(fmt, ...) fprintf(stderr, fmt "\n", ## __VA_ARGS__)
#ifdef NDEBUG
#define LOGD(...) do {} while(0)
#else
#define LOGD LOG
#endif

// partial GOL file produced from tile repository by `load` will cause crash when iterating relation members
//#define DISABLE_RELATIONS

using CodedString = geodesk::Key;

// can't think of a way to do this (w/o separate list of tag strings) w/o using macro
#define Find(s) readTag( [](){ static CodedString cs = TileBuilder::getCodedString(s); return cs; }() )
#define Holds(s) bool(Find(s))

class TileBuilder
{
public:
  static Features* worldFeats;
  static CodedString getCodedString(std::string_view s);

  geodesk::Box m_tileBox;
  Features* m_tileFeats = nullptr;
  std::unique_ptr<vtzero::feature_builder> m_build;

  // current feature
  Feature* m_feat = nullptr;  //std::reference_wrapper<Feature> m_feat;
  int64_t m_featId = -1;
  vt_multi_polygon m_featMPoly;
  double m_area = NAN;
  dvec2 m_centroid;

  // coord mapping
  dvec2 m_origin;
  double m_scale = 0;
  const float tileExtent = 4096;  // default vtzero extent
  float simplifyThresh = 1/512.0f;

  // stats
  int m_builtPts = 0;
  int m_builtFeats = 0;
  bool m_hasGeom = false;  // doesn't seem we can get this from vtzero

  // temp containers
  std::vector<i32vec2> m_tilePts;

  // coastline
  vt_multi_line_string m_coastline;

  TileID m_id;
  vtzero::tile_builder m_tile;
  std::map<std::string, vtzero::layer_builder> m_layers;

  TileBuilder(TileID _id, const std::vector<std::string>& layers);
  Feature& feature() { return *m_feat; }
  vt_point toTileCoord(Coordinate r);
  virtual void processFeature() = 0;
  std::string build(const Features& world, const Features& ocean, bool compress = true);
  void setFeature(Feature& feat);

  // reading geodesk feature
  TagValue readTag(CodedString cs) { return feature()[cs]; }
  std::string Id() { return std::to_string(feature().id()); }
  //bool Holds(const std::string& key) { return Find(key) != ""; }
  bool IsClosed() { return feature().isArea(); }
  double Length() { return feature().length(); }
  double Area();
  //double AreaIntersecting();
  Features GetMembers();

  // writing tile feature
  bool MinZoom(int z) { return m_id.z >= z; }
  void Attribute(const std::string& key, const TagValue& val) {
    if(val) { m_build->add_property(key, std::string(val)); }
  }
  void Attribute(const std::string& key, const std::string& val) {
    if(!val.empty()) { m_build->add_property(key, val); }  //&& m_id.z >= z
  }
  template<class T>
  void AttributeNumeric(const std::string& key, T val) { m_build->add_property(key, val); }
  void Layer(const std::string& layer, bool isClosed = false, bool _centroid = false);
  void LayerAsCentroid(const std::string& layer) { Layer(layer, false, true); }

//private:
  void buildLine(Feature& way);
  vt_multi_line_string loadWayFeature(Feature& way);
  void buildPolygon();
  template<class T> void addRing(vt_polygon& poly, T&& iter, bool outer);
  void loadAreaFeature();
  const std::vector<i32vec2>& toTilePts(std::vector<vt_point>& pts);

  void addCoastline(Feature& way);
  void buildCoastline();
};
