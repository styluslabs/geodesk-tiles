#include "tilebuilder.h"
#include "polylabel.hpp"
#include <geom/polygon/RingCoordinateIterator.h>
#include <geom/polygon/RingBuilder.h>
#include <geom/polygon/Segment.h>

#define MINIZ_GZ_IMPLEMENTATION
#include "miniz/miniz_gzip.h"

// TileBuilder
// Refs:
// - https://github.com/mapbox/vector-tile-spec
// - https://github.com/mapbox/mbtiles-spec
// - https://github.com/onthegomap/planetiler / https://github.com/openmaptiles/planetiler-openmaptiles
// - https://github.com/systemed/tilemaker - tile_worker.cpp, etc.

using namespace geodesk;

Features* TileBuilder::worldFeats = nullptr;

CodedString TileBuilder::getCodedString(std::string_view s)
{
  return worldFeats->key(s);  //CodedString{s, worldFeats->store()->strings().getCode(s.data(), s.size())};
}


TileBuilder::TileBuilder(TileID _id, const std::vector<std::string>& layers) : m_id(_id)
{
  for(auto& l : layers)
    m_layers.emplace(l, vtzero::layer_builder{m_tile, l, 2, uint32_t(tileExtent)});  // MVT v2

  double units = Mercator::MAP_WIDTH/MapProjection::EARTH_CIRCUMFERENCE_METERS;
  m_origin = units*MapProjection::tileSouthWestCorner(m_id);
  m_scale = 1/(units*MapProjection::metersPerTileAtZoom(m_id.z));

  simplifyThresh = _id.z < 14 ? 1/512.0f : 0;  // no simplification for highest zoom (which can be overzoomed)
}

static LngLat tileCoordToLngLat(const TileID& tileId, dvec2 tileCoord)
{
  double scale = MapProjection::metersPerTileAtZoom(tileId.z);
  ProjectedMeters tileOrigin = MapProjection::tileSouthWestCorner(tileId);
  ProjectedMeters meters = tileCoord * scale + tileOrigin;
  return MapProjection::projectedMetersToLngLat(meters);
}

static Box tileBox(const TileID& id, double eps = 0.0)
{
  LngLat minBBox = tileCoordToLngLat(id, {eps, eps});
  LngLat maxBBox = tileCoordToLngLat(id, {1-eps, 1-eps});
  return Box::ofWSEN(minBBox.longitude, minBBox.latitude, maxBBox.longitude, maxBBox.latitude);
}

void TileBuilder::setFeature(Feature& feat)
{
  m_feat = &feat;
  m_area = NAN;
  m_featMPoly.clear();
  m_featId = feat.id();  // save id for debugging
}

std::string TileBuilder::build(const Features& world, const Features& ocean, bool compress)
{
  auto time0 = std::chrono::steady_clock::now();

  //double eps = 0.01/MapProjection::metersPerTileAtZoom(m_id.z);
  m_tileBox = tileBox(m_id);  //, eps);
  Features tileFeats = world(m_tileBox);
  m_tileFeats = &tileFeats;
  int nfeats = 0;

  if(m_id.z < 8) {
    const char* queries[] = {
      m_id.z < 7 ? "n[place=continent,country,state,city]" : "n[place=continent,country,state,city,town]",
      m_id.z < 5 ? "w[highway=motorway]" : (m_id.z < 7 ? "w[highway=motorway,trunk]" : "w[highway=motorway,trunk,primary]"),
      "wra[boundary=administrative,disputed]",  // no index on admin_level
      "a[place=island]",
      "a[natural=water,glacier]",  //,wood,grassland,grass,scrub,fell,heath,wetland,beach,sand,bare_rock,scree]"
      "a[waterway=river]"
    };

    for(auto q : queries) {
      Features queryFeats = tileFeats(q);
      for(Feature f : queryFeats) {
        setFeature(f);
        processFeature();
        ++nfeats;
      }
    }

    // use ocean geometry from ocean.gol instead of world
    Features oceanFeats = ocean(m_tileBox);
    for(Feature f : oceanFeats) {
      setFeature(f);
      m_featId = OCEAN_ID;
      processFeature();
      ++nfeats;
    }
  }
  else {
    for(Feature f : tileFeats) {
      setFeature(f);
      processFeature();
      ++nfeats;
    }
    m_feat = nullptr;

    // ocean polygons
    m_featId = OCEAN_ID;
    if(!m_coastline.empty())
      processFeature();
    else {
      LngLat center = MapProjection::projectedMetersToLngLat(MapProjection::tileCenter(m_id));
      // create all ocean tile if center is inside an ocean polygon
      // looks like there might be a bug in FeatureUtils::isEmpty() used by bool(Features), so do this instead
      Features f = ocean.containingLonLat(center.longitude, center.latitude);
      if(f.begin() != f.end()) { processFeature(); }
    }
  }
  Layer("");  // flush final feature
  m_tileFeats = nullptr;

  std::string mvt = m_tile.serialize();  // very fast, not worth separate timing
  if(mvt.size() == 0) {
    LOG("No features for tile %s", m_id.toString().c_str());
    return "";
  }
  auto time1 = std::chrono::steady_clock::now();
  int origsize = mvt.size();
  if(compress) {
    std::stringstream in_strm(mvt);
    std::stringstream out_strm;
    gzip(in_strm, out_strm, 5);  // level = 5 gives nearly same size as 6 but significantly faster
    mvt = std::move(out_strm).str();  // C++20
  }
  auto time2 = std::chrono::steady_clock::now();

  double dt01 = std::chrono::duration<double>(time1 - time0).count()*1000;
  double dt12 = std::chrono::duration<double>(time2 - time1).count()*1000;
  double dt02 = std::chrono::duration<double>(time2 - time0).count()*1000;
  LOG("Tile %s (%d bytes) built in %.1f ms (%.1f ms process %d/%d features w/ %d points, %.1f ms gzip %d bytes)",
      m_id.toString().c_str(), int(mvt.size()), dt02, dt01, m_builtFeats, nfeats, m_builtPts, dt12, origsize);

  return mvt;
}

// simplification

static real dist2(vt_point p) { return p.x*p.x + p.y*p.y; }

// distance from point `pt` to line segment `start`-`end` to `pt`
static real distToSegment2(vt_point start, vt_point end, vt_point pt)
{
  const real l2 = dist2(end - start);
  if(l2 == 0.0) // zero length segment
    return dist2(start - pt);
  // Consider the line extending the segment, parameterized as start + t*(end - start).
  // We find projection of pt onto this line and clamp t to [0,1] to limit to segment
  const real t = std::max(real(0), std::min(real(1), dot(pt - start, end - start)/l2));
  const vt_point proj = start + t * (end - start);  // Projection falls on the segment
  return dist2(proj - pt);
}

static void simplifyRDP(const std::vector<vt_point>& pts, std::vector<int>& keep, int start, int end, real thresh)
{
  real maxdist2 = 0;
  int argmax = 0;
  auto& p0 = pts[start];
  auto& p1 = pts[end];
  for(int ii = start + 1; ii < end; ++ii) {
    real d2 = distToSegment2(p0, p1, pts[ii]);
    if(d2 > maxdist2) {
      maxdist2 = d2;
      argmax = ii;
    }
  }
  if(maxdist2 < thresh*thresh) { return; }
  keep[argmax] = 1;
  simplifyRDP(pts, keep, start, argmax, thresh);
  simplifyRDP(pts, keep, argmax, end, thresh);
}

static std::vector<int> simplify(const std::vector<vt_point>& pts, real thresh)
{
  if(thresh <= 0 || pts.size() < 3) { return {}; }
  std::vector<int> keep(pts.size(), 0);
  keep.front() = 1;  keep.back() = 1;
  simplifyRDP(pts, keep, 0, pts.size() - 1, thresh);
  return keep;
  //size_t dst = 0;
  //for(size_t src = 0; src < pts.size(); ++src) {
  //  if(keep[src]) { pts[dst++] = pts[src]; }
  //}
  //pts.resize(dst);
}

// from ulib/geom.cpp
template<class T>
real linearRingArea(const std::vector<T>& points)
{
  real area = 0;
  for(size_t ii = 0, jj = points.size() - 1; ii < points.size(); jj = ii++)
    area += (points[jj].x - points[ii].x)*(points[jj].y + points[ii].y);
  return area/2;
}

template<class T>
bool pointInPolygon(const std::vector<T>& poly, T p)
{
  bool in = false;
  for(size_t i = 0, j = poly.size()-1; i < poly.size(); j = i++) {
    if( ((poly[i].y > p.y) != (poly[j].y > p.y)) &&
        (p.x < (poly[j].x - poly[i].x) * (p.y - poly[i].y) / (poly[j].y - poly[i].y) + poly[i].x) )
      in = !in;
  }
  return in;
}

// convert to relative tile coord (float 0..1)
vt_point TileBuilder::toTileCoord(Coordinate r) {
  return vt_point(m_scale*(dvec2(r.x, r.y) - m_origin));  // + 0.5);
}

// simplify and write to TileBuilder.tilePts as MVT coord (i32 0..tileExtent)
const std::vector<i32vec2>& TileBuilder::toTilePts(const std::vector<vt_point>& pts)
{
  auto keep = simplify(pts, simplifyThresh);
  m_tilePts.clear();
  m_tilePts.reserve(pts.size());
  for(size_t ii = 0; ii < pts.size(); ++ii) {
    if(!keep.empty() && !keep[ii]) { continue; }
    auto ip = i32vec2(pts[ii].x*tileExtent + 0.5f, (1 - pts[ii].y)*tileExtent + 0.5f);
    if(m_tilePts.empty() || ip != m_tilePts.back()) { m_tilePts.push_back(ip); }
  }
  return m_tilePts;
}

// clockwise distance along tile perimeter from 0,0 to point p
static real perimDistCW(vt_point p)
{
  if(p.x == 0) return p.y;
  if(p.y == 1) return 1 + p.x;
  if(p.x == 1) return 2 + (1 - p.y);
  if(p.y == 0) return 3 + (1 - p.x);
  assert(false && "Point not on perimeter!"); return -1;
}

void TileBuilder::buildCoastline()
{
  // if m_coastline is empty, we will just create all ocean tile
  LOGD("Processing %d coastline segments for tile %s", int(m_coastline.size()), m_id.toString().c_str());

  vt_multi_polygon outers;
  vt_polygon inners;
  auto add_ring = [&](auto&& ring) {
    // water is on right side of coastline ways, so outer rings are clockwise (area < 0)
    return linearRingArea(ring) > 0 ? inners.emplace_back(std::move(ring))
        : outers.emplace_back().emplace_back(std::move(ring));
  };

  struct vt_point_order {
    bool operator()(const vt_point& lhs, const vt_point& rhs) const {
      return lhs.x < rhs.x || (lhs.x == rhs.x && lhs.y < rhs.y);
    }
  };
  std::map<vt_point, vt_linear_ring, vt_point_order> segments;

  for(auto& way : m_coastline) {
    if(way.back() == way.front())
      add_ring(std::move(way));
    else
      segments.emplace(way.front(), std::move(way));
  }

  for(auto ii = segments.begin(); ii != segments.end();) {
    vt_linear_ring& ring = ii->second;
    auto jj = segments.find(ring.back());
    if(jj == segments.end()) { ++ii; }
    else if(jj == ii) {
      add_ring(std::move(ring));
      ii = segments.erase(ii);
    }
    else {
      ring.insert(ring.end(), jj->second.begin(), jj->second.end());
      segments.erase(jj);
      // don't advance ii to repeat w/ new ring.back()
    }
  }

  // for remaining segments, we must add path from exit (end) clockwise along tile edge to entry
  //  (beginning) of next segment
  std::map<real, vt_linear_ring> edgesegs;
  for(auto& seg : segments) {
    real d = perimDistCW(seg.second.front());
    if(d < 0) {
      LOG("Invalid coastline segment for %s", m_id.toString().c_str());
      return;
    }
    edgesegs.emplace(d, std::move(seg.second));
  }

  static vt_point corners[] = {{0,0}, {0,1}, {1,1}, {1,0}};
  for(auto ii = edgesegs.begin(); ii != edgesegs.end();) {
    vt_linear_ring& ring = ii->second;
    real dback = perimDistCW(ring.back());
    if(dback < 0) {
      LOG("Invalid coastline segment for %s", m_id.toString().c_str());
      return;
    }
    auto next = edgesegs.lower_bound(dback);
    if(next == edgesegs.end()) { next = edgesegs.begin(); }

    vt_point dest = next->second.front();
    real dfront = next->first;  //perimDistCW(dest);
    if(dfront < dback) { dfront += 4; }
    int c = std::ceil(dback);
    while(c < dfront) {
      ring.push_back(corners[(c++)%4]);
    }
    if(ii == next) {
      ring.push_back(dest);
      add_ring(std::move(ring));
      ii = edgesegs.erase(ii);
    }
    else {
      ring.insert(ring.end(), next->second.begin(), next->second.end());
      edgesegs.erase(next);
      // don't advance ii to repeat w/ new ring.back()
    }
  }
  assert(edgesegs.empty());

  // next, we have to assign inner rings to outer rings
  if(outers.empty()) {
    outers.push_back({{{0,0}, {0,1}, {1,1}, {1,0}, {0,0}}});  // island!
  }
  if(outers.size() == 1) {
    outers[0].insert(outers[0].end(),
        std::make_move_iterator(inners.begin()), std::make_move_iterator(inners.end()));
  }
  else {
    for(vt_linear_ring& inner : inners) {
      // find point not on edge to reduce chance of numerical issues (since outer likely includes edge)
      vt_point pin = inner.front();
      for(auto& p : inner) {
        if(p.x != 0 && p.y != 0 && p.x != 1 && p.y != 1) { pin = p; break; }
      }
      for(vt_polygon& outer : outers) {
        // test if first point of inner is inside outer ring
        if(pointInPolygon(outer.front(), pin)) {
          outer.emplace_back(std::move(inner));
          break;
        }
      }
    }
  }

  // MVT polygon is single CCW outer ring followed by 0 or more CW inner rings; multipolygon repeats this
  auto build = static_cast<vtzero::polygon_feature_builder*>(m_build.get());
  for(vt_polygon& outer : outers) {
    for(vt_linear_ring& ring : outer) {
      const auto& tilePts = toTilePts(ring);
      if(tilePts.size() < 4) {}
      else if(tilePts.back() != tilePts.front()) {
        LOGD("Invalid polygon for %s coastline", m_id.toString().c_str());
      }
      else {
        m_hasGeom = true;
        m_builtPts += tilePts.size();
        build->add_ring_from_container(tilePts);
      }
    }
  }
}

void TileBuilder::addCoastline(Feature& way)
{
  vt_multi_line_string clipPts = loadWayFeature(way);
  m_coastline.insert(m_coastline.end(),
      std::make_move_iterator(clipPts.begin()), std::make_move_iterator(clipPts.end()));
}

vt_multi_line_string TileBuilder::loadWayFeature(Feature& way)
{
  vt_multi_line_string clipPts;
  vt_line_string& tempPts = clipPts.emplace_back();
  WayCoordinateIterator iter(WayPtr(way.ptr()));
  int n = iter.coordinatesRemaining();
  tempPts.reserve(n);
  vt_point pmin(REAL_MAX, REAL_MAX), pmax(-REAL_MAX, -REAL_MAX);
  while(n-- > 0) {
    vt_point p = toTileCoord(iter.next());
    tempPts.push_back(p);
    pmin = min(p, pmin);
    pmax = max(p, pmax);
  }
  // see if we can skip clipping
  if(pmin.x > 1 || pmin.y > 1 || pmax.x < 0 || pmax.y < 0) { clipPts.clear(); }
  else if(pmin.x < 0 || pmin.y < 0 || pmax.x > 1 || pmax.y > 1) {
    clipper<0> xclip{0,1};
    clipper<1> yclip{0,1};
    clipPts = yclip(xclip(tempPts));
  }
  return clipPts;
}

void TileBuilder::buildLine(Feature& way)
{
  vt_multi_line_string clipPts = loadWayFeature(way);
  auto* build = static_cast<vtzero::linestring_feature_builder*>(m_build.get());
  for(auto& line : clipPts) {
    const auto& tilePts = toTilePts(line);
    if(tilePts.size() > 1) {
      m_hasGeom = true;
      m_builtPts += tilePts.size();
      build->add_linestring_from_container(tilePts);
    }  //else LOG("Why?");
  }
}

template<class T>
void TileBuilder::addRing(vt_polygon& poly, T&& iter, bool outer)
{
  int n = iter.coordinatesRemaining();
  vt_linear_ring& ring = poly.emplace_back();
  ring.reserve(n);
  vt_point pmin(REAL_MAX, REAL_MAX), pmax(-REAL_MAX, -REAL_MAX);
  while(n-- > 0) {
    vt_point p = toTileCoord(iter.next());
    ring.push_back(p);
    pmin = min(p, pmin);
    pmax = max(p, pmax);
  }
  // we want area and centroid of the whole feature, before clipping
  double area = 0;
  dvec2 centroid(0,0);
  // we assume first and last points of ring are the same
  for(size_t ii = 0; ii+1 < ring.size(); ++ii) {
    double a = ring[ii].x * ring[ii+1].y - ring[ii+1].x * ring[ii].y;
    area += a;
    centroid += a * dvec2(ring[ii].x + ring[ii+1].x, ring[ii].y + ring[ii+1].y);
  }

  if(pmin.x > 1 || pmin.y > 1 || pmax.x < 0 || pmax.y < 0) { ring.clear(); }
  else if(pmin.x < 0 || pmin.y < 0 || pmax.x > 1 || pmax.y > 1) {
    clipper<0> xclip{0,1};
    clipper<1> yclip{0,1};
    ring = yclip(xclip(ring));
  }
  m_polyMin = min(m_polyMin, pmin);
  m_polyMax = max(m_polyMax, pmax);

  // note that sign of area will be reversed by y flip of tile coords
  bool rev = (area > 0) == outer;
  if(rev) { std::reverse(ring.begin(), ring.end()); }
  m_area += rev ? area/2 : -area/2;
  m_centroid += rev ? centroid : -centroid;
  // wait until feature is accepted to simplify (which is a bit slow)
}

// Tangram mvt.cpp fixes the winding direction for outer ring from the first polygon in the tile, rather
//  than using the MVT spec of positive signed area or using the winding of the first ring of each
//  multipolygon; in any case, we should just follow the spec

void TileBuilder::loadAreaFeature()
{
  if(!std::isnan(m_area)) { return; }  // already loaded?

  m_area = 0;
  m_centroid = {0,0};
  m_polyMin = vt_point(REAL_MAX, REAL_MAX);
  m_polyMax = vt_point(-REAL_MAX, -REAL_MAX);
  if(feature().isWay()) {
    vt_polygon& poly = m_featMPoly.emplace_back();
    addRing(poly, WayCoordinateIterator(WayPtr(feature().ptr())), true);
    //if(poly.back().empty()) { m_featMPoly.pop_back(); }
  }
  else {
    Polygonizer polygonizer;
    polygonizer.createRings(feature().store(), RelationPtr(feature().ptr()));
    polygonizer.assignAndMergeHoles();
    const Polygonizer::Ring* outer = polygonizer.outerRings();
    while(outer) {
      vt_polygon& poly = m_featMPoly.emplace_back();
      addRing(poly, RingCoordinateIterator(outer), true);
      const Polygonizer::Ring* inner = outer->firstInner();
      while(inner) {
        addRing(poly, RingCoordinateIterator(inner), false);
        if(poly.back().empty()) { poly.pop_back(); }
        inner = inner->next();
      }
      //if(poly.front().empty()) { m_featMPoly.pop_back(); }  // remove whole polygon if outer empty
      outer = outer->next();
    }
  }
  // centroid in tile units
  m_centroid *= 1/(6*m_area);

  // area: convert from tile units^2 to mercator meters^2
  // geodesk area computes area in mercator meters, then scales based on latitude; tilemaker uses
  //  boost::geometry to calculate exact area on spherical surface; we just use mercator meters since
  //  this is what Tangram expects (and makes sense for determining if feature should be shown on tile)
  m_area *= squared(MapProjection::metersPerTileAtZoom(m_id.z));
  if(m_area < 0) { LOGD("Polygon for feature %ld has negative area", feature().id()); }
}

void TileBuilder::buildPolygon()
{
  loadAreaFeature();
  auto* build = static_cast<vtzero::polygon_feature_builder*>(m_build.get());
  for(vt_polygon& poly : m_featMPoly) {
    if(poly.front().size() < 4) { continue; }  // skip if outer ring is empty
    bool isouter = true;
    for(vt_linear_ring& ring : poly) {
      const auto& tilePts = toTilePts(ring);
      // tiny polygons get simplified to two points and discarded ... calculate area instead?
      if(tilePts.size() < 4) {}
      else if(tilePts.back() != tilePts.front()) {
        LOGD("Invalid polygon for feature %ld", feature().id());
      }
      else {
        m_hasGeom = true;
        m_builtPts += tilePts.size();
        build->add_ring_from_container(tilePts);
      }
      isouter = false;  // any additional rings in this polygon are inner rings
    }
  }
}

double TileBuilder::Area()
{
  if(std::isnan(m_area)) {
    if(!feature().isArea()) { m_area = 0; }
    else { loadAreaFeature(); }
  }
  return m_area;
}

//GetParents() return m_tileFeats->relations().parentsOf(feature()); -- crashes since parent iteration not implemented
Features TileBuilder::GetMembers()
{
  return m_tileFeats->membersOf(feature());
}

void TileBuilder::Layer(const std::string& layer, bool isClosed, bool _centroid)
{
  if(m_build && m_hasGeom) {
    ++m_builtFeats;
    m_build->commit();
  }
  m_build.reset();  // have to commit/rollback before creating next builder
  m_hasGeom = false;

  if(layer.empty()) { return; }  // layer == "" to flush last feature
  auto it = m_layers.find(layer);
  if(it == m_layers.end()) {
    LOG("Layer not found: %s", layer.c_str());
    return;
  }
  vtzero::layer_builder& layerBuild = it->second;

  // ocean
  if(!m_feat) {
    m_build = std::make_unique<vtzero::polygon_feature_builder>(layerBuild);
    buildCoastline();
  }
  else if(feature().isNode() || _centroid) {
    vt_point p(-1, -1);
    if(!feature().isArea()) { p = toTileCoord(feature().centroid()); }
    else {
      loadAreaFeature();
      p = vt_point(m_centroid);
      // if centroid lies in this tile and only one polygon, use polylabel to get better label pos
      if(p.x >= 0 && p.y >= 0 && p.x <= 1 && p.y <= 1 && m_featMPoly.size() == 1 && m_featMPoly[0].front().size() > 3) {
        vt_point pl(-1, -1);
        if(m_id.z >= 14) { pl = mapbox::polylabel(m_featMPoly[0], 1/256.0f); }
        else {
          // clip feature to z14 tile containing centroid
          real zq = std::exp2(14 - m_id.z);
          vt_point p14 = floor(p*zq);
          vt_point min14 = p14/zq, max14 = (p14 + 1)/zq;
          clipper<0> xclip{min14.x, max14.x};
          clipper<1> yclip{min14.y, max14.y};
          vt_polygon clipped;
          clipped.reserve(m_featMPoly[0].size());
          for(const vt_linear_ring& ring : m_featMPoly[0]) { clipped.push_back(yclip(xclip(ring))); }
          // polygon already in tile coords; 1/256 precision for geometry in normalized tile coords
          // ... need to adjust precision to ensure same pos as z14 (but limit to 16x)
          real prec = 1/256.0f/std::min(zq, real(16));
          if(clipped.front().size() > 3) { pl = mapbox::polylabel(clipped, prec); }
        }
        if(pl.x >= 0 && pl.y >= 0 && pl.x <= 1 && pl.y <= 1) { p = pl; }
        else {
          LOGD("rejecting polylabel %f,%f for %ld (centroid %f,%f)", pl.x, pl.y, feature().id(), p.x, p.y);
        }
      }
    }
    auto build = std::make_unique<vtzero::point_feature_builder>(layerBuild);
    m_hasGeom = p.x >= 0 && p.y >= 0 && p.x <= 1 && p.y <= 1;
    if(m_hasGeom) {
      auto ip = i32vec2(p.x*tileExtent + 0.5f, (1 - p.y)*tileExtent + 0.5f);
      build->add_point(ip.x, ip.y);
      ++m_builtPts;
    }
    m_build = std::move(build);
  }
  else if(feature().isArea()) {
    //if(!isClosed) { LOG("isArea() but not isClosed!"); }
    m_build = std::make_unique<vtzero::polygon_feature_builder>(layerBuild);
    buildPolygon();
  }
  else {
    //if(isClosed) { LOG("isClosed but not isArea()!"); }
    m_build = std::make_unique<vtzero::linestring_feature_builder>(layerBuild);
    if(feature().isWay())
      buildLine(feature());
    else {  //if(feature().isRelation()) {
      // multi-linestring(?)
      for(Feature child : feature().members()) {
        if(child.isWay() && m_tileBox.intersects(child.bounds())) {
          buildLine(child);
        }
      }
    }
  }
}
