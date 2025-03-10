// Converted from Lua Tilemaker processing script for Ascend Maps OSM schema

#include "tilebuilder.h"

#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>


// tilebuilder.cpp/.h + ascendtiles.cpp/.h ?
class AscendTileBuilder : public TileBuilder {
public:
  AscendTileBuilder(TileID _id);
  void processFeature() override;

  void ProcessNode();
  void ProcessWay();
  void ProcessRelation();

  void WriteBoundary();
  void SetBuildingHeightAttributes();
  bool SetMinZoomByArea(double area = 0);
  void SetBrunnelAttributes();
  void SetEleAttributes();
  void SetNameAttributes(int minz = 0);
  void SetIdAttributes();
  bool NewWritePOI(double area = 0, bool force = false);
};

std::string buildTile(const Features& world, const Features& ocean, TileID id)
{
  AscendTileBuilder tileBuilder(id);
  try {
    return tileBuilder.build(world, ocean);
  }
  catch(std::exception &e) {
    int64_t fid = tileBuilder.m_feat ? tileBuilder.feature().id() : -1;
    LOG("Exception building tile %s (feature id %ld): %s", id.toString().c_str(), fid, e.what());
    return "";
  }
}

// AscendTileBuilder impl

// we could try something like github.com/serge-sans-paille/frozen or github.com/renzibei/fph-table for
//  the set/map here, but at this point, tag lookup is only a small part of CPU time
struct Set {
  std::unordered_set<std::string> m_items;
  Set(std::initializer_list<std::string> items) : m_items(items) {}

  bool operator[](const std::string& key) const { return !key.empty() && m_items.find(key) != m_items.end(); }
  bool operator[](const TagValue& key) const { return bool(key) && m_items.find(std::string(key)) != m_items.end(); }
};

struct ZMap {
  using map_t = std::unordered_map<std::string, int>;
  std::string m_tag;
  mutable CodedString m_tagCode = {{}, INT_MAX};
  map_t m_items;
  const int m_dflt = 100;
  ZMap(std::string_view _tag, int _dflt=100) : m_tag(_tag), m_dflt(_dflt) {}
  ZMap(std::initializer_list<map_t::value_type> items) : m_items(items) {}
  ZMap& add(int z, std::initializer_list<std::string> items) {
    for(auto& s : items)
      m_items.emplace(s, z);
    return *this;
  }

  const std::string& tag() const { return m_tag; }
  const CodedString& tagCode() const {
    if(m_tagCode.code == INT_MAX)
      m_tagCode = TileBuilder::getCodedString(m_tag);
    return m_tagCode;
  }

  int getValue(const std::string& key) const {
    auto it = m_items.find(key);
    return it != m_items.end() ? it->second : m_dflt;
  }

  int operator[](const std::string& key) const { return !key.empty() ? getValue(key) : m_dflt; }
  int operator[](const TagValue& key) const { return key ? getValue(key) : m_dflt; }
};


static const std::vector<std::string> ascendLayers =
    { "place", "boundary", "poi", "transportation", "transit", "building", "water", "landuse" };

AscendTileBuilder::AscendTileBuilder(TileID _id) : TileBuilder(_id, ascendLayers) {}

void AscendTileBuilder::processFeature()
{
  if(!m_feat) {  // building ocean polygon?
    Layer("water", true);
    Attribute("class", "ocean");
  }
  else if (feature().isWay()) { ProcessWay(); }
  else if (feature().isNode()) { ProcessNode(); }
  else if (Find("type") == "multipolygon") { ProcessWay(); }
  else { ProcessRelation(); }  //if (feature().isRelation())
  //else { LOG("Unknown feature type!"); }
}


static const auto aerodromeValues = Set { "international", "public", "regional", "military", "private" };

//node_keys = { "addr:housenumber","aerialway","aeroway","amenity","barrier","highway","historic","leisure","natural","office","place","railway","shop","sport","tourism","waterway" }
void AscendTileBuilder::ProcessNode()
{
  // Write 'place'
  auto place = Find("place");
  if (place) {  // != "") {
    int mz = 13;
    auto pop_tag = Find("population");
    double pop = pop_tag ? double(pop_tag) : 0;

    if (place == "continent"   ) { mz = 0; }
    else if (place == "country") { mz = 3 - (pop > 50E6) - (pop > 20E6); }
    else if (place == "state"  ) { mz = 4; }
    else if (place == "city"   ) { mz = 5; }
    else if (place == "town"   ) { mz = pop > 8000 ? 7 : 8; }
    else if (place == "village") { mz = pop > 2000 ? 9 : 10; }
    else if (place == "suburb" ) { mz = 11; }
    else if (place == "hamlet" ) { mz = 12; }
    else if (place == "quarter") { mz = 12; }
    //else if (place == "neighbourhood") { mz = 13; }  -- 13 is the default
    //else if (place == "locality"     ) { mz = 13; }

    if (!MinZoom(mz)) { return; }

    Layer("place", false);
    Attribute("class", place);
    Attribute("place", place);
    //if (rank > 0) { AttributeNumeric("rank", rank); }
    if (pop > 0) { AttributeNumeric("population", pop); }
    auto sqkm = Find("sqkm");
    if (sqkm) { AttributeNumeric("sqkm", double(sqkm)); }
    if (place == "country") { Attribute("iso_a2", Find("ISO3166-1:alpha2")); }
    Attribute("place_CN", Find("place:CN"));
    SetNameAttributes();
    SetIdAttributes();
    return;
  }

  // many smaller airports only have aerodrome node instead of way
  auto aeroway = Find("aeroway");
  if (aeroway == "aerodrome") {
    if (!MinZoom(11)) { return; }
    Layer("transportation", false);  //"aeroway"
    Attribute("aeroway", aeroway);
    Attribute("ref", Find("ref"));
    SetNameAttributes();
    SetEleAttributes();
    Attribute("iata", Find("iata"));
    Attribute("icao", Find("icao"));
    auto aerodrome = Find("aerodrome");
    Attribute("aerodrome", aerodromeValues[aerodrome] ? std::string(aerodrome) : "other");
    return;
  }

  // Write 'poi'
  NewWritePOI();

  // Write 'mountain_peak' and 'water_name'
  auto natural = Find("natural");
  if (natural == "peak" || natural == "volcano") {
    if (!MinZoom(11)) { return; }
    Layer("poi", false);
    SetNameAttributes();
    SetIdAttributes();
    SetEleAttributes();
    Attribute("natural", natural);
    return;
  }

  if (natural == "bay") {
    if (!MinZoom(8)) { return; }
    Layer("water", false);
    SetNameAttributes();  //14);
    return;
  }
}

// default zoom for including labels is 14; use | <zoom>_z to override
constexpr unsigned long long operator""_z(unsigned long long z) { return z << 8; }
static const ZMap highwayValues = {
    {"motorway", 4|8_z}, {"trunk", 5|8_z}, {"primary", 7|12_z}, {"secondary", 9|12_z}, {"tertiary", 11|12_z},
    {"unclassified", 12}, {"residential", 12}, {"road", 12}, {"living_street", 12}, {"service", 12}, // minor roads
    {"cycleway", 10}, {"byway", 10}, {"bridleway", 10}, {"track", 10},  // tracks (was z14)
    {"footway", 10}, {"path", 10}, {"steps", 10}, {"pedestrian", 10},  // paths (was z14)
    {"motorway_link", -11}, {"trunk_link", -11}, {"primary_link", -11}, {"secondary_link", -11},
    {"tertiary_link", -11}  // link roads (on/off ramps)
};

static const auto pavedValues = Set { "paved", "asphalt", "cobblestone", "concrete", "concrete:lanes",
    "concrete:plates", "metal", "paving_stones", "sett", "unhewn_cobblestone", "wood" };
static const auto unpavedValues = Set { "unpaved", "compacted", "dirt", "earth", "fine_gravel", "grass",
    "grass_paver", "gravel", "gravel_turf", "ground", "ice", "mud", "pebblestone", "salt", "sand", "snow", "woodchips" };

static const auto boundaryValues = Set { "administrative", "disputed" };
static const auto parkValues = Set { "protected_area", "national_park" };
static const auto landuseAreas = Set { "retail", "military", "residential", "commercial", "industrial",
    "railway", "cemetery", "forest", "grass", "allotments", "meadow", "recreation_ground", "village_green",
    "landfill", "farmland", "farmyard", "orchard", "vineyard", "plant_nursery", "greenhouse_horticulture", "farm" };
static const auto naturalAreas = Set { "wood", "grassland", "grass", "scrub", "fell", "heath", "wetland",
    "glacier", "beach", "sand", "bare_rock", "scree" };
static const auto leisureAreas = Set { "pitch", "park", "garden", "playground", "golf_course", "stadium" };
static const auto amenityAreas = Set { "school", "university", "kindergarten", "college", "library",
    "hospital", "bus_station", "marketplace" };
static const auto tourismAreas = Set { "zoo", "theme_park", "aquarium" };

static const auto waterwayClasses = Set { "stream", "river", "canal", "drain", "ditch" };
static const auto waterwayAreas   = Set { "river", "riverbank", "stream", "canal", "drain", "ditch", "dock" };
static const auto waterLanduse    = Set { "reservoir", "basin", "salt_pond" };
static const auto noNameWater     = Set { "river", "basin", "wastewater", "canal", "stream", "ditch", "drain" };
static const auto manMadeClasses  = Set { "pier", "breakwater", "groyne" };  // "storage_tank", "water_tap", "dyke", "lighthouse"
static const auto aerowayClasses  = Set { "taxiway", "hangar", "runway", "helipad", "aerodrome", "airstrip", "tower" };
static const auto aerowayBuildings = Set { "terminal", "gate", "tower" };

static const ZMap transitRoutes =
    { {"train", 8}, {"subway", 10}, {"tram", 12}, {"share_taxi", 12}, {"light_rail", 12}, {"bus", 14}, {"trolleybus", 14} };
static const ZMap otherRoutes =
    { {"road", 8}, {"ferry", 9}, {"bicycle", 10}, {"hiking", 10}, {"foot", 12}, {"mtb", 10}, {"ski", 12} };  //piste = 12;,
//ignoredRoutes = Set { "power", "railway", "detour", "tracks", "horse", "emergency_access", "snowmobile", "historic", "running", "fitness_trail" }

void AscendTileBuilder::ProcessRelation()
{
  auto reltype = Find("type");
  if (reltype == "route") {
    auto route = Find("route");
    if (route == "ferry") {
      if(!MinZoom(9)) { return; }
      Layer("transportation", false);
      Attribute("route", "ferry");
      SetNameAttributes(12);
      return;
    }
    if (MinZoom(transitRoutes[route])) {
      Layer("transit", false);
    } else if (MinZoom(otherRoutes[route])) {
      Layer("transportation", false);
    } else {
      return;
    }
    Attribute("class", "route");
    Attribute("route", route);
    Attribute("name", Find("name"));
    Attribute("ref", Find("ref"));
    Attribute("network", Find("network"));
    Attribute("color", Find("colour"));  // note spelling
    SetIdAttributes();
    return;
  }
  if (reltype == "boundary") {
    auto boundary = Find("boundary");
    if (boundary == "administrative" || boundary == "disputed") {
      WriteBoundary();
      return;
    }
    if (!parkValues[boundary] || !MinZoom(8)) { return; }   //SetMinZoomByArea(rel, area);
    if (Find("maritime") == "yes") { return; }  // marine sanctuaries not really useful for typical use
    auto leisure = Find("leisure");
    auto protect_class = Find("protect_class");
    // tilemaker doesn't calculate area for relations
    auto area = Area();
    Layer("landuse", true);
    Attribute("class", boundary);
    Attribute("boundary", boundary);
    Attribute("leisure", leisure);
    Attribute("protect_class", protect_class);
    SetNameAttributes();
    AttributeNumeric("area", area);
    // write POI at centroid
    LayerAsCentroid("poi");
    //MinZoom(8);  //SetMinZoomByArea(rel, area);
    Attribute("class", boundary);
    Attribute("boundary", boundary);
    Attribute("leisure", leisure);
    Attribute("protect_class", protect_class);
    SetNameAttributes();
    SetIdAttributes();
    AttributeNumeric("area", area);
  }
}

void AscendTileBuilder::ProcessWay()
{
  //auto tags = feature().tags();  if(tags.begin() == tags.end()) { return; }  // skip if no tags
  auto building = Find("building");  // over 50% of ways are buildings, so process first
  if (building) {  // != ""
    if (!MinZoom(13) || !SetMinZoomByArea()) { return; }
    Layer("building", true);
    SetBuildingHeightAttributes();
    if (MinZoom(14)) {
      // housenumber is also commonly set on poi nodes, but not very useful w/o at least street name too
      Attribute("housenumber", Find("addr:housenumber"));  //, 14);
      NewWritePOI(0, true);
    }
    return;
  }

  //if (Find("disused") == "yes") { return; } -- not commonly used
  // Roads/paths/trails - 2nd most common way type
  auto highway_tag = Find("highway");
  if (highway_tag) {
    std::string highway = highway_tag;
    int minzoom = highwayValues[highway];
    bool ramp = minzoom < 0;
    if(ramp) {
      highway = highway.substr(0, highway.find("_"));
      minzoom = -minzoom;
    }
    int lblzoom = (minzoom >> 8) ? (minzoom >> 8) : 14;
    minzoom = minzoom & 0xFF;

    //if (highway == "proposed" || highway == "construction") { return; }  -- will fail MinZoom test anyway
    // Construction -- not used currently
    //auto construction = Find("construction");
    //if (highway == "construction" && constructionValues[construction]) {
    //  highway = construction;
    //  construction = "yes";
    //}

    if(!MinZoom(minzoom)) { return; }

    auto access = Find("access");
    if (access == "private" || access == "no") { return; }
    // most footways are sidewalks or crossings, which are mapped inconsistently so just add clutter and
    //  confusion the map; we could consider keeping footway == "alley"
    if (highway == "footway" && Find("footway")) { return; }

    Layer("transportation", false);
    //Attribute("class", h);
    Attribute("highway", highway);
    SetBrunnelAttributes();
    if (ramp) { AttributeNumeric("ramp", 1); }

    // Service
    if (highway == "service") { Attribute("service", Find("service")); }

    auto oneway = Find("oneway");
    if (oneway == "yes" || oneway == "1") {
      AttributeNumeric("oneway", 1);
    }
    //if (oneway == "-1") {}

    // cycling
    auto cycleway = Find("cycleway");
    if (!cycleway) { // == "") {
      cycleway = Find("cycleway:both");
    }
    if (cycleway && cycleway != "no") {  //!= ""
      Attribute("cycleway", cycleway);
    }

    auto cycleway_left = Find("cycleway:left");
    if (cycleway_left && cycleway_left != "no") {
      Attribute("cycleway_left", cycleway_left);
    }

    auto cycleway_right = Find("cycleway:right");
    if (cycleway_right && cycleway_right != "no") {
      Attribute("cycleway_right", cycleway_right);
    }

    auto bicycle = Find("bicycle");
    if (bicycle && bicycle != "no") {
      Attribute("bicycle", bicycle);
    }

    // surface
    auto surface = Find("surface");
    if (pavedValues[surface]) {
      Attribute("surface", "paved");
    } else if (unpavedValues[surface]) {
      Attribute("surface", "unpaved");
    }

    // trail/path info
    auto trailvis = Find("trail_visibility");
    if (trailvis && trailvis != "good" && trailvis != "excellent") {
      Attribute("trail_visibility", trailvis);
    }
    Attribute("mtb_scale", Find("mtb:scale"));  // mountain biking difficulty rating
    if (highway == "path") { Attribute("golf", Find("golf")); }

    // name, roadway info
    SetNameAttributes(lblzoom);
    //Attribute("network","road"); // **** could also be us-interstate, us-highway, us-state
    Attribute("maxspeed", Find("maxspeed"));
    Attribute("lanes", Find("lanes"));
    Attribute("ref", Find("ref"));  //AttributeNumeric("ref_length",ref:len());
    return;
  }

  // Railways ('transportation' and 'transportation_name', plus 'transportation_name_detail');
  auto railway = Find("railway");
  if (railway) {  //!= ""
    auto service  = Find("service");
    if (!MinZoom(service ? 12 : 9)) { return; }
    Layer("transportation", false);
    Attribute("class", "rail");
    Attribute("railway", railway);
    SetBrunnelAttributes();
    SetNameAttributes(14);
    Attribute("service", service);
    return;
  }

  bool isClosed = IsClosed();
  auto waterway = Find("waterway");
  std::string landuse = Find("landuse");

  // waterway is single way indicating course of a waterway - wide rivers, etc. have additional polygons to map area
  if (waterwayClasses[waterway] && !isClosed) {
    bool namedriver = waterway == "river" && Holds("name");
    if (!MinZoom(namedriver ? 8 : 12)) { return; }
    Layer("water", false);  //waterway , waterway_detail
    if (Find("intermittent") == "yes") { AttributeNumeric("intermittent", 1); }
    Attribute("class", waterway);
    Attribute("waterway", waterway);
    SetNameAttributes();
    SetBrunnelAttributes();
    return;
  } else if (waterway == "dam") {
    if (!MinZoom(12)) { return; }  // was 13
    Layer("building", isClosed);
    Attribute("waterway", waterway);
    return;
  } else if (waterway == "boatyard" || waterway == "fuel") {
    landuse = "industrial";
  }

  auto natural = Find("natural");
  auto leisure = Find("leisure");
  std::string waterbody;
  if (waterLanduse[landuse]) { waterbody = landuse; }
  else if (waterwayAreas[waterway]) { waterbody = std::string(waterway); }
  else if (leisure == "swimming_pool") { waterbody = std::string(leisure); }
  else if (natural == "water") { waterbody = std::string(natural); }  // || natural == "bay"  -- bay used for name, not water itself!

  if (waterbody != "") {
    if (!isClosed || !SetMinZoomByArea() || Find("covered") == "yes") { return; }
    std::string cls = waterway != "" ? "river" : "lake";
    //if (natural == "bay") { cls = "ocean"; } else if (waterway != "") { cls = "river"; }
    //if (class == "lake" and Find("wikidata") == "Q192770") { return; }  // crazy lake in Finland
    //if (cls == "ocean" && isClosed && (AreaIntersecting("ocean")/Area() > 0.98)) { return; }
    auto water = Find("water");
    Layer("water", true);
    Attribute("class", cls);
    Attribute("water", water != "" ? std::string(water) : waterbody);

    if (Find("intermittent") == "yes") { AttributeNumeric("intermittent", 1); }
    // don't include names for minor man-made basins (e.g. way 25958687) or rivers, which have name on waterway way
    if (Holds("name") && natural == "water" && !noNameWater[water]) {
      SetNameAttributes(14);
      AttributeNumeric("area", Area());
    }
    return;
  }

  if (natural) {
    if (natural == "coastline") {
      addCoastline(feature());
      // can also be boundary, so don't return
    }
    else if (natural == "valley") {
      // special case since valleys are mapped as ways
      auto len = Length();
      Layer("landuse", false);
      SetMinZoomByArea(len*len);
      Attribute("natural", natural);
      SetNameAttributes();
      return;
    }
  }

  auto boundary = Find("boundary");
  // Parks ... possible for way to be both park boundary and landuse?
  bool park_boundary = parkValues[boundary];
  if (park_boundary || leisure == "nature_reserve") {
    if (!SetMinZoomByArea()) { return; }
    if (Find("protection_title") == "National Forest"
        && Find("operator") == "United States Forest Service") { return; }  // too many
    Layer("landuse", true);
    Attribute("class", park_boundary ? boundary : leisure);
    if (park_boundary) { Attribute("boundary", boundary); }
    Attribute("leisure", leisure);
    Attribute("protect_class", Find("protect_class"));
    SetNameAttributes();
    NewWritePOI(Area(), MinZoom(14));
  }

  // Boundaries ... possible for way to be shared with park boundary or landuse?
  if (!feature().belongsToRelation() && (boundary == "administrative" || boundary == "disputed")) {
    WriteBoundary();
  }

  // landuse/landcover
  auto amenity  = Find("amenity");
  auto tourism  = Find("tourism");

  if (landuse == "field") { landuse = "farmland"; }
  else if (landuse == "meadow" && Find("meadow") == "agricultural") { landuse = "farmland"; }

  if (landuseAreas[landuse] || naturalAreas[natural] || leisureAreas[leisure] || amenityAreas[amenity] || tourismAreas[tourism]) {
    if (!SetMinZoomByArea()) { return; }
    Layer("landuse", true);
    //Attribute("class", landuseKeys[l]);
    //if (landuse == "residential" and Area()<ZRES8) { MinZoom(8); } else { SetMinZoomByArea(way); }
    Attribute("landuse", landuse);
    Attribute("natural", natural);
    Attribute("leisure", leisure);
    Attribute("amenity", amenity);
    Attribute("tourism", tourism);
    if (natural == "wetland") { Attribute("wetland", Find("wetland")); }
    NewWritePOI(Area(), MinZoom(14));
    return;
  }

  // less common ways

  // Pier, breakwater, etc.
  auto man_made = Find("man_made");
  if (manMadeClasses[man_made]) {
    if(!SetMinZoomByArea()) { return; }
    Layer("landuse", isClosed);
    //SetZOrder(way);
    Attribute("class", man_made);
    Attribute("man_made", man_made);
    return;
  }

  // 'Ferry'
  auto route = Find("route");
  if (route && route == "ferry") {
    if (!MinZoom(9)) { return; }
    // parents() not implemented! ... we'll assume a parent has route=ferry if any parents
    if (feature().belongsToRelation()) { return; }  // avoid duplication
    //for (Relation rel : feature().parents()) { if (rel["route"] == "ferry") { return; }  }
    Layer("transportation", false);
    Attribute("route", route);
    SetBrunnelAttributes();
    SetNameAttributes(12);
    return;
  }

  auto piste_diff = Find("piste:difficulty");
  if (piste_diff) {  // != "") {
    if (!MinZoom(10)) { return; }
    Layer("transportation", isClosed);
    Attribute("class", "piste");
    Attribute("route", "piste");
    Attribute("difficulty", piste_diff);
    Attribute("piste_type", Find("piste:type"));
    Attribute("piste_grooming", Find("piste:grooming"));  // so we can ignore backcountry "pistes"
    SetNameAttributes(14);
    return;
  }

  auto aerialway = Find("aerialway");
  if (aerialway) {  // != "") {
    if (!MinZoom(10)) { return; }
    Layer("transportation", false);
    Attribute("class", "aerialway");
    Attribute("aerialway", aerialway);
    SetNameAttributes(14);
    return;
  }

  auto aeroway = Find("aeroway");
  if (aerowayBuildings[aeroway]) {
    if (!SetMinZoomByArea()) { return; }
    Layer("building", true);
    Attribute("aeroway", aeroway);
    SetBuildingHeightAttributes();
    if (MinZoom(14)) { NewWritePOI(0, true); }
    return;
  }
  if (aerowayClasses[aeroway]) {
    if (!MinZoom(10)) { return; }
    Layer("transportation", isClosed);  //"aeroway"
    Attribute("aeroway", aeroway);
    Attribute("ref", Find("ref"));
    //write_name = true
    if (aeroway == "aerodrome") {
      //LayerAsCentroid("aerodrome_label");
      SetNameAttributes();
      SetEleAttributes();
      Attribute("iata", Find("iata"));
      Attribute("icao", Find("icao"));
      auto aerodrome = Find("aerodrome");
      Attribute("aerodrome", aerodromeValues[aerodrome] ? std::string(aerodrome) : "other");
      AttributeNumeric("area", Area());
    }
    return;
  }
}

// POIs: moving toward including all values for key except common unwanted values

static constexpr int EXCLUDE = 100;
static const std::vector<ZMap> poiTags = {
  // all amenity values with count > 1000 (as of Jan 2024) that we wish to exclude
  ZMap("amenity", 14).add(12, { "bus_station", "ferry_terminal" }).add(EXCLUDE, { "parking_space", "bench",
      "shelter", "waste_basket", "bicycle_parking", "recycling", "hunting_stand", "vending_machine",
      "post_box", "parking_entrance", "telephone", "bbq", "motorcycle_parking", "grit_bin", "clock",
      "letter_box", "watering_place", "loading_dock", "payment_terminal", "mobile_money_agent", "trolley_bay",
      "ticket_validator", "lounger", "feeding_place", "vacuum_cleaner", "game_feeding", "smoking_area",
      "photo_booth", "kneipp_water_cure", "table", "fixme", "office", "chair" }),
  ZMap("tourism", 14).add(12, { "attraction", "viewpoint", "museum" }).add(EXCLUDE, { "yes" }),
  ZMap("leisure", 14).add(EXCLUDE, { "fitness_station", "picnic_table",
      "slipway", "outdoor_seating", "firepit", "bleachers", "common", "yes" }),
  ZMap("shop", 14),
  ZMap("sport", 14),
  ZMap("landuse").add(14, { "basin", "brownfield", "cemetery", "reservoir", "winter_sports" }),
  ZMap("historic").add(14, { "monument", "castle", "ruins", "fort", "mine", "archaeological_site" }),
  //archaeological_site = Set { "__EXCLUDE", "tumulus", "fortification", "megalith", "mineral_extraction", "petroglyph", "cairn" },
  ZMap("highway").add(12, { "bus_stop", "trailhead" }).add(14, { "traffic_signals" }),
  ZMap("railway").add(12, { "halt", "station", "tram_stop" }).add(14, { "subway_entrance", "train_station_entrance" }),
  ZMap("natural").add(13, { "spring", "hot_spring", "fumarole", "geyser", "sinkhole", "arch", "cave_entrance", "saddle" }),
  ZMap("barrier").add(14, { "bollard", "border_control",
      "cycle_barrier", "gate", "lift_gate", "sally_port", "stile", "toll_booth" }),
  ZMap("building").add(14, { "dormitory" }),
  ZMap("aerialway").add(14, { "station" }),
  ZMap("waterway").add(14, { "dock" })
};

static const std::vector<ZMap> extraPoiTags =
    { ZMap("cuisine"), ZMap("station"), ZMap("religion"), ZMap("operator"), ZMap("archaeological_site") };  // atm:operator

bool AscendTileBuilder::NewWritePOI(double area, bool force)
{
  if(!MinZoom(12)) { return false; }  // no POIs below z12

  bool force12 = area > 0 || Holds("wikipedia");
  for (const ZMap& z : poiTags) {
    auto val = readTag(z.tagCode());
    if (bool(val) && (force12 || MinZoom(z[val]))) {
      LayerAsCentroid("poi");
      SetNameAttributes();
      SetIdAttributes();
      if (area > 0) { AttributeNumeric("area", area); }
      // write value for all tags in poiTags (if present)
      for(const ZMap& y : poiTags) { Attribute(y.tag(), readTag(y.tagCode())); }
      for(auto& s : extraPoiTags) { Attribute(s.tag(), readTag(s.tagCode())); }
      return true;
    }
  }
  if (force && Holds("name")) {
    LayerAsCentroid("poi");
    SetNameAttributes();
    SetIdAttributes();
    if (area > 0) { AttributeNumeric("area", area); }
  }
  return false;
}

// Common functions

void AscendTileBuilder::SetNameAttributes(int minz)
{
  if (!MinZoom(minz)) { return; }
  auto name = Find("name");
  Attribute("name", name);
  auto name_en_tag = Find("name:en");
  if (name_en_tag) {
    std::string name_en = name_en_tag;  // = not impl yet for TagValue
    if(name != name_en) { Attribute("name_en", name_en); }
  }
}

// previously we set id attributes whenever we set name attributes, but now we only set for poi, place, and
//  transit layers; if we wanted to be able to get id for any feature in app, we'd want to set id
//  attributes for all features (m_hasGeom == true), not just features with name.
void AscendTileBuilder::SetIdAttributes()
{
  std::string osm_type = feature().isWay() ? "way" : feature().isNode() ? "node" : "relation";
  Attribute("osm_id", Id());
  Attribute("osm_type", osm_type);
}

void AscendTileBuilder::SetEleAttributes()
{
  auto ele = Find("ele");
  if (ele) { AttributeNumeric("ele", float(double(ele))); }  //"ele_ft", ele * 3.2808399
}

void AscendTileBuilder::SetBrunnelAttributes()
{
  if (Find("bridge") == "yes") { Attribute("brunnel", "bridge"); }
  else if (Find("tunnel") == "yes") { Attribute("brunnel", "tunnel"); }
  else if (Find("ford") == "yes") { Attribute("brunnel", "ford"); }
}

// Meters per pixel if tile is 256x256
constexpr double SQ(double x) { return x*x; }
static constexpr double ZRES5  = SQ(4891.97);
static constexpr double ZRES6  = SQ(2445.98);
static constexpr double ZRES7  = SQ(1222.99);
static constexpr double ZRES8  = SQ(611.5);
static constexpr double ZRES9  = SQ(305.7);
static constexpr double ZRES10 = SQ(152.9);
static constexpr double ZRES11 = SQ(76.4);
static constexpr double ZRES12 = SQ(38.2);
static constexpr double ZRES13 = SQ(19.1);

// Set minimum zoom level by area
bool AscendTileBuilder::SetMinZoomByArea(double area)
{
  if (MinZoom(14)) { return true; }  // skip area calc for highest zoom
  if (area <= 0) { area = Area(); }
  if      (area > ZRES5 ) { return MinZoom(6);  }
  else if (area > ZRES6 ) { return MinZoom(7);  }
  else if (area > ZRES7 ) { return MinZoom(8);  }
  else if (area > ZRES8 ) { return MinZoom(9);  }
  else if (area > ZRES9 ) { return MinZoom(10); }
  else if (area > ZRES10) { return MinZoom(11); }
  else if (area > ZRES11) { return MinZoom(12); }
  else if (area > ZRES12) { return MinZoom(13); }
  else                    { return MinZoom(14); }
}

void AscendTileBuilder::SetBuildingHeightAttributes()
{
  static constexpr double BUILDING_FLOOR_HEIGHT = 3.66;  // meters

  float height = 0, minHeight = 0;

  auto height_tag = Find("height");
  if(height_tag) {
    height = double(height_tag);
    auto minHeight_tag = Find("min_height");
    if(minHeight_tag)
      minHeight = double(minHeight_tag);
  }
  else {
    auto levels_tag = Find("building:levels");  //.c_str());
    if(levels_tag) {
      height = double(levels_tag) * BUILDING_FLOOR_HEIGHT;
      auto minLevel_tag = Find("building:min_level");
      if(minLevel_tag)
        minHeight = double(minLevel_tag) * BUILDING_FLOOR_HEIGHT;
    }
  }

  if (height < minHeight) { height += minHeight; }

  if(height > 0) { AttributeNumeric("height", height); }
  if(minHeight > 0) { AttributeNumeric("min_height", minHeight); }
}

void AscendTileBuilder::WriteBoundary()
{
  auto admin_level_tag = Find("admin_level");  //.c_str());
  float admin_level = admin_level_tag ? double(admin_level_tag) : 11;
  if (admin_level < 1) { admin_level = 11; }
  int mz = 0;
  if (admin_level >= 3 && admin_level < 5) { mz=4; }
  else if (admin_level >= 5 && admin_level < 7) { mz=8; }
  else if (admin_level == 7) { mz=10; }
  else if (admin_level >= 8) { mz=12; }

  if (!MinZoom(mz)) { return; }

  bool maritime = Find("maritime") == "yes";
  bool disputed = Find("boundary") == "disputed" || Find("disputed") == "yes";
  if (feature().isWay()) {
    Layer("boundary", false);
    AttributeNumeric("admin_level", admin_level);
    SetNameAttributes();
    // to allow hiding coastal boundaries (natural=coastline)
    Attribute("natural", Find("natural"));
    if (maritime) { Attribute("maritime", "yes"); }
    if (disputed) { Attribute("disputed", "yes"); }
  }
  else {
    // get name from relation
    std::string name = Find("name");
    std::string name_en = Find("name:en");
    if (name_en == name) { name_en = ""; }

    auto members = GetMembers();
    for (Feature f : members) {
      if (!f.isWay()) { continue; }
      // combining members view and bounded view is currently a "TODO" in libgeodesk, so check manually
      if (!m_tileBox.intersects(f.bounds())) { continue; }
      m_feat = &f;  //SetFeature(f);  -- temporary (ha!) hack until we verify this works
      Layer("boundary", false);
      AttributeNumeric("admin_level", admin_level);
      Attribute("name", name);
      Attribute("name_en", name_en);  // not written if empty
      Attribute("natural", Find("natural"));
      if (maritime || Find("maritime") == "yes") { Attribute("maritime", "yes"); }
      if (disputed || Find("boundary") == "disputed" || Find("disputed") == "yes") {
        Attribute("disputed", "yes");
      }
    }
  }
}
