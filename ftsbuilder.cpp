#include <geodesk/geodesk.h>
#include "tilebuilder.h"
#include "ulib.h"

#define SQLITEPP_LOGE LOG
#define SQLITEPP_LOGW LOG
#include "sqlitepp.h"

#define LOGT(t0, fmt, ...) do { \
  auto t1 = std::chrono::steady_clock::now(); \
  double dt = std::chrono::duration<double>(t1 - t0).count(); \
  fprintf(stderr, "+%.3f s: " fmt "\n", dt, ## __VA_ARGS__); \
} while(0);

// search index query

static const char* searchNoDistSQL = "SELECT pois.rowid, lng, lat, rank, pois.tags, props FROM pois_fts JOIN pois ON"
    " pois.ROWID = pois_fts.ROWID WHERE pois_fts MATCH ? ORDER BY osmSearchRank(rank, pois.tags) LIMIT ? OFFSET ?;";
static const char* searchDistSQL = "SELECT pois.rowid, lng, lat, rank, pois.tags, props FROM pois_fts JOIN pois ON"
    " pois.ROWID = pois_fts.ROWID WHERE pois_fts MATCH ? ORDER BY osmSearchRank(rank, pois.tags, lng, lat, ?, ?, ?) LIMIT ? OFFSET ?;";
//static const char* searchOnlyDistSQL = "SELECT pois.rowid, lng, lat, rank, tags, props FROM pois_fts JOIN pois ON"
//    " pois.ROWID = pois_fts.ROWID WHERE pois_fts MATCH ? ORDER BY osmSearchRank(-1.0, lng, lat) LIMIT 50 OFFSET ?;";

class SearchDB : public SQLiteDB
{
public:
  SQLiteStmt searchNoDist = {NULL};
  SQLiteStmt searchDist = {NULL};
  //SQLiteStmt searchOnlyDist = {NULL};
  SQLiteStmt insertPOI = {NULL};
};

thread_local SearchDB searchDB;

static const char* POI_SCHEMA = R"#(PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;
CREATE TABLE pois(name TEXT, admin TEXT, tags TEXT, props TEXT, lng REAL, lat REAL);
CREATE VIRTUAL TABLE pois_fts USING fts5(name, admin, tags, content='pois');)#";

/*
static const char* POI_FTS_SCHEMA = R"#(BEGIN;
CREATE VIRTUAL TABLE pois_fts USING fts5(name, admin, tags, content='pois');

-- triggers to keep the FTS index up-to-date
CREATE TRIGGER pois_insert AFTER INSERT ON pois BEGIN
  INSERT INTO pois_fts(rowid, name, admin, tags) VALUES (NEW.rowid, NEW.name, NEW.admin, NEW.tags);
END;
CREATE TRIGGER pois_delete AFTER DELETE ON pois BEGIN
  INSERT INTO pois_fts(pois_fts, rowid, name, admin, tags) VALUES ('delete', OLD.rowid, OLD.name, OLD.admin, OLD.tags);
END;
CREATE TRIGGER pois_update AFTER UPDATE ON pois BEGIN
  INSERT INTO pois_fts(pois_fts, rowid, name, admin, tags) VALUES ('delete', OLD.rowid, OLD.name, OLD.admin, OLD.tags);
  INSERT INTO pois_fts(rowid, name, admin, tags) VALUES (NEW.rowid, NEW.name, NEW.admin, NEW.tags);
END;
COMMIT;)#";
*/

struct PoiRow { std::string names, admin, tags, props; double lng, lat; };

class FTSBuilder : public TileBuilder {
public:
  FTSBuilder(TileID _id) : TileBuilder(_id, {}) {}
  std::vector<PoiRow> index(const Features& world);
};

// we expect FTS index creation to take longer than iterating features, so not much benefit from multithreading
// ... probably no longer true with admin boundary processing

static std::vector<PoiRow> indexTile(const Features& world, TileID id)
{
  FTSBuilder ftsBuilder(id);
  try {
    return ftsBuilder.index(world);
  }
  catch(std::exception &e) {
    int64_t fid = ftsBuilder.m_feat ? ftsBuilder.m_featId : -1;  //tileBuilder.feature().id() : -1;
    LOG("Exception indexing tile %s (feature id %ld): %s", id.toString().c_str(), fid, e.what());
    return {};
  }
}

//static std::atomic_int numFeats = 0;
//static std::atomic_int numMPolyTests = 0;
//static std::atomic_int numPinPTests = 0;
//static std::atomic_int numPinPHits = 0;

static bool isHeavyTile(const Features& worldGOL, TileID id, int nfeats = 16384)
{
  auto feats = worldGOL(TileBuilder::tileBox(id));
  for(auto it = feats.begin(); it != feats.end(); ++it) {
    if(--nfeats <= 0) { return true; }
  }
  return false;
}

int buildSearchIndex(const Features& worldGOL, TileID toptile, const std::string& searchDBPath)
{
  int numThreads = std::max(2U, std::thread::hardware_concurrency()) - 1;
  ThreadPool indexWorkers(numThreads);
  ThreadPool dbWriter(1);  // ThreadPool(1) is like AsyncWorker
  //searchDBPath = "fts_wip.sqlite";

  auto dbfut = dbWriter.enqueue([&](){
    if(searchDB.open(searchDBPath, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) != SQLITE_OK) {
      LOG("Error opening search DB");
      return false;
    }
    //if(!searchDB.exec(POI_SCHEMA)) { return false; }
    if(!searchDB.exec(POI_SCHEMA)) {
      LOG("Error creating FTS tables");
      return false;
    }
    char const* insertPOISQL = "INSERT INTO pois (name,admin,tags,props,lng,lat) VALUES (?,?,?,?,?,?);";
    searchDB.insertPOI = searchDB.stmt(insertPOISQL);
    return true;
  });
  if(!dbfut.get()) {
    LOG("Error opening world mbtiles %s\n", searchDBPath.c_str());
    return -1;
  }

  auto t0 = std::chrono::steady_clock::now();
  std::function<void(TileID)> buildFn = [&](TileID id){
    if(id.z == 4) { LOGT(t0, "processing %s", id.toString().c_str()); }
    if(id.z < 4 || (id.z < 10 && isHeavyTile(worldGOL, id))) {
      for(int ii = 0; ii < 4; ++ii)
        indexWorkers.enqueue(buildFn, id.getChild(ii, 10));
      return;
    }

    std::vector<PoiRow> rows = indexTile(worldGOL, id);
    if(!rows.empty()) {
      dbWriter.enqueue([&, rows = std::move(rows), id](){
        searchDB.exec("BEGIN;");
        for(auto& r : rows) {
          if(!searchDB.insertPOI.bind(r.names, r.admin, r.tags, r.props, r.lng, r.lat).exec())
            LOG("Error adding row to search DB: %s", searchDB.errMsg());
        }
        searchDB.exec("COMMIT;");
      });
    }

  };
  //onSigInt = [&](){ buildWorkers.requestStop(true); };
  indexWorkers.enqueue(buildFn, toptile);
  indexWorkers.waitForIdle();
  LOGT(t0, "Building index...");
  dbWriter.enqueue([](){
    searchDB.exec("INSERT INTO pois_fts(pois_fts) VALUES('rebuild');");
  });
  dbWriter.waitForIdle();
  //LOGT(t0, "Finished: %d / %d / %d (total/tests/hits)", numMPolyTests.load(), numPinPTests.load(), numPinPHits.load());
  LOGT(t0, "Finished");
  return 0;
}

//template<class T> bool pointInPolygon(const std::vector<std::vector<T>>& poly, T p)
/*__attribute__((noinline))*/ bool pointInPolygon(const vt_polygon& poly, vt_point p)
{
  bool in = false;
  for(auto& ring : poly) {
    for(size_t i = 0, j = ring.size()-1; i < ring.size(); j = i++) {
      if(((ring[i].y > p.y) != (ring[j].y > p.y)) &&
          (p.x < (ring[j].x - ring[i].x) * (p.y - ring[i].y) / (ring[j].y - ring[i].y) + ring[i].x) )
        in = !in;
    }
  }
  return in;
}

static void addJson(std::string& json, const std::string& key, const std::string& val)
{
  if(val.empty()) { return; }
  json.append(json.empty() ? "{ " : ", ");
  json.append("\"").append(key).append("\": \"").append(val).append("\"");
}

#define readTag(feat, s) feat[ ( [](){ static geodesk::Key cs = worldFeats->key(s); return cs; }() ) ]

std::vector<PoiRow> FTSBuilder::index(const Features& world)  //, const Features& ocean, bool compress)
{
  // strings must have same lifetime as Keys so we can get string from Key
  static std::vector<std::string> poiTagStrs = { "place", "natural", "amenity", "leisure", "tourism", "historic",
      "waterway", "shop", "sport", "landuse", "building", "railway", "aerialway", "memorial",
      "office", "cuisine", "water" };  //"highway" ... we only want highway=trailhead (and not bus_stop)
  static std::vector<geodesk::Key> poiTags = [&](){
    std::vector<geodesk::Key> keys;
    keys.reserve(poiTagStrs.size());
    for(auto& tag : poiTagStrs) { keys.push_back(worldFeats->key(tag)); }
    return keys;
  }();

  // features that have none of the above tags but any of these tags will be excluded
  // public_transport is dominated by bus stops
  static std::unordered_set<std::string> badTags = {"traffic_sign", "public_transport"};
  auto hasBadTag = [](const Feature& f){
    for(auto kv : f.tags()) {
      if(badTags.find(std::string(kv.key())) != badTags.end()) { return true; }
    }
    return false;
  };

  m_tileBox = tileBox(m_id);  //, eps);
  Features tileFeats = world(m_tileBox);
  m_tileFeats = &tileFeats;

  Features pois = tileFeats("na[name]");  //"n[place=*]"
  if(pois.begin() == pois.end()) { return {}; }  // skip admin area processing if nothing to index

  struct AdminMPoly { int level; int64_t id; std::string name, name_en; vt_point min, max; vt_multi_polygon mpoly; };
  std::vector<AdminMPoly> adminMPolys;

  // admin_level = 3,5,7 are mostly undesired in US, Europe, but China cities are 5, Japan cities 7
  const char* adminquery = "wra[boundary=administrative,disputed]";
  for(Feature f : tileFeats(adminquery)) {
    auto leveltag = readTag(f, "admin_level");
    if(!leveltag) { continue; }
    int level = double(leveltag);
    if(level < 2 || level > 8) { continue; }
    setFeature(f);
    loadAreaFeature();

    m_featMPoly.erase(std::remove_if(m_featMPoly.begin(), m_featMPoly.end(),
        [](const vt_polygon& poly) { return poly.front().size() < 4; }), m_featMPoly.end());
    //vt_multi_polygon res;
    //res.reserve(m_featMPoly.size());
    //for(const vt_polygon& poly : m_featMPoly) {
    //  if(poly.front().size() < 4) { continue; }  // skip if outer ring is empty
    //  bool coverstile = false;  //poly.size() == 1 && covers(poly.front());
    //  res.push_back(std::move(poly));
    //}
    if(!m_featMPoly.empty()) {
      std::string name = readTag(f, "name");
      if(name.empty()) { continue; }
      std::string name_en = readTag(f, "name:en");
      std::string names = (!name_en.empty() && name_en != name) ? name + " " + name_en : name;
      adminMPolys.push_back({level, f.id(), name, name_en, m_polyMin, m_polyMax, m_featMPoly});
    }
  }

  // sort from higher to lower admin level
  std::sort(adminMPolys.begin(), adminMPolys.end(),
      [](const AdminMPoly& a, const AdminMPoly& b){ return a.level > b.level; });

  std::vector<PoiRow> rows;
  rows.reserve(8192);  //pois.count());
  std::string tags, props, admin, adminfts;

  for(Feature f : pois) {
    std::string name = readTag(f, "name");
    if(name.empty()) { continue; }

    auto coords = f.xy();
    vt_point pt = toTileCoord(coords);
    if(pt.x < 0 || pt.y < 0 || pt.x > 1 || pt.y > 1) { continue; }  // area belongs to another tile

    // if not a "place", give priority to heritage and wikipedia tags
    if(readTag(f, "place")) {}
    else if(readTag(f, "heritage")) { tags.append("heritage"); }
    else if(readTag(f, "wikipedia")) { tags.append("wikipedia"); }
    //else if(readTag(f, "wikidata")) { tags.append("wikidata"); }

    for(auto& key : poiTags) {
      auto val = f[key];
      if(val && val != "yes") {
        //if(maintag.empty()) { maintag = val; }
        if(!tags.empty()) { tags += ' '; }
        tags.append(val);
        addJson(props, std::string(key), val);
      }
    }

    if(tags.empty() && hasBadTag(f)) { continue; }

    //++numFeats;
    for(auto& mp : adminMPolys) {
      //++numMPolyTests;
      if(pt.x < mp.min.x || pt.y < mp.min.y || pt.x > mp.max.x || pt.y > mp.max.y) { continue; }
      //++numPinPTests;
      for(auto& poly : mp.mpoly) {
        if(pointInPolygon(poly, pt)) {
          //++numPinPHits;
          if(!adminfts.empty()) { adminfts += ' '; }
          adminfts.append(mp.name);
          if(!admin.empty()) { admin += ", "; }
          admin.append(!mp.name_en.empty() ? mp.name_en : mp.name);
          break;
        }
      }
    }

    std::string name_en = readTag(f, "name:en");
    std::string names = (!name_en.empty() && name_en != name) ? name + " " + name_en : name;

    addJson(props, "osm_id", std::to_string(f.id()));
    addJson(props, "osm_type", f.isWay() ? "way" : f.isNode() ? "node" : "relation");
    addJson(props, "name", name);
    addJson(props, "name:en", name_en);
    addJson(props, "admin", admin);
    //addJson(props, "place", readTag(f, "place"));
    //addJson(props, "population", readTag(f, "population"));
    //addJson(props, "type", maintag);
    props.append(" }");

    double lng = Mercator::lonFromX(coords.x);
    double lat = Mercator::latFromY(coords.y);

    rows.emplace_back(names, adminfts, tags, props, lng, lat);
    tags.clear(); props.clear(); admin.clear(); adminfts.clear();
  }

  m_tileFeats = nullptr;

  return rows;
}

// searching

static double lngLatDist(LngLat r1, LngLat r2)
{
  constexpr double p = 3.14159265358979323846/180;
  double a = 0.5 - cos((r2.latitude-r1.latitude)*p)/2 + cos(r1.latitude*p) * cos(r2.latitude*p) * (1-cos((r2.longitude-r1.longitude)*p))/2;
  return 12742 * asin(sqrt(a));  // kilometers
}

static void udf_osmSearchRank(sqlite3_context* context, int argc, sqlite3_value** argv)
{
  static std::unordered_map<std::string, int> tagOrder = { {"heritage", 64}, {"wikipedia", 63},
      {"nature_reserve", 62}, {"park", 61},
      {"country", 90}, {"state", 85}, {"province", 80}, {"city", 75}, {"town", 70}, {"island", 65},
      {"suburb", 60}, {"quarter", 55}, {"neighbourhood", 50}, {"district", 45}, {"borough", 40},
      {"municipality", 35}, {"village", 30}, {"hamlet", 25}, {"county", 20}, {"locality", 15}, {"islet", 10},
      {"vending_machine", -100} };

  if(argc < 2) {
    sqlite3_result_error(context, "osmSearchRank - Invalid number of arguments (2 or 6 required).", -1);
    return;
  }
  if(sqlite3_value_type(argv[0]) != SQLITE_FLOAT || sqlite3_value_type(argv[1]) != SQLITE_TEXT) {
    sqlite3_result_double(context, -1.0);
    return;
  }
  // sqlite FTS5 rank is roughly -1*number_of_words_in_query; ordered from -\inf to 0
  double rank = sqlite3_value_double(argv[0]);

  // get the first tag
  const char* tags = (const char*)sqlite3_value_text(argv[1]);
  const char* tagend = tags;
  while(*tagend && *tagend != ' ') { ++tagend; }
  if(tagend != tags) {
    auto it = tagOrder.find(std::string(tags, tagend));
    if(it != tagOrder.end()) {
      rank -= it->second/100;  // adjust rank to break ties
    }
  }
  else
    rank *= 0.5;  // heavily downrank results w/ no tags

  if(argc < 7 || sqlite3_value_type(argv[2]) != SQLITE_FLOAT || sqlite3_value_type(argv[3]) != SQLITE_FLOAT
      || sqlite3_value_type(argv[4]) != SQLITE_FLOAT || sqlite3_value_type(argv[5]) != SQLITE_FLOAT
      || sqlite3_value_type(argv[6]) != SQLITE_FLOAT || sqlite3_value_double(argv[6]) <= 0) {
    sqlite3_result_double(context, rank);
    return;
  }
  double lon = sqlite3_value_double(argv[2]);
  double lat = sqlite3_value_double(argv[3]);
  double lon0 = sqlite3_value_double(argv[4]);
  double lat0 = sqlite3_value_double(argv[5]);
  double rad0 = sqlite3_value_double(argv[6]);
  double dist = lngLatDist(LngLat(lon0, lat0), LngLat(lon, lat));  // in kilometers
  // obviously will want a more sophisticated ranking calculation in the future
  sqlite3_result_double(context, rank/(1 + log2(std::max(1.0, dist/rad0))));
}

// note that this is called on a cpp-httplib thread (and that searchDB is thread local!)
std::string ftsQuery(const std::string& query,
    LngLat lngLat00, LngLat lngLat11, int limit, int offset, const std::string& searchDBPath)
{
  if(!searchDB.db) {
    if(searchDB.open(searchDBPath, SQLITE_OPEN_READONLY) != SQLITE_OK) {
      LOG("Error opening search database %s on http worker thread!", searchDBPath.c_str());
      return {};
    }
    if(sqlite3_create_function(searchDB.db, "osmSearchRank", -1, SQLITE_UTF8, 0, udf_osmSearchRank, 0, 0) != SQLITE_OK) {
      LOG("sqlite3_create_function: error creating osmSearchRank for search DB");
      return {};
    }
    searchDB.searchNoDist = searchDB.stmt(searchNoDistSQL);
    searchDB.searchDist = searchDB.stmt(searchDistSQL);
    //LOG("Loaded FTS database %s", searchDBPath.c_str());
  }

  // words containing any special characters need to be quoted, so just quote every word (and make AND
  //  operation explicit)
  auto words = splitStr<std::vector>(query, " ", true);
  std::string searchStr = "\"" + joinStr(words, "\" AND \"") + "\"*";

  if(limit < 1 || limit > 50) { limit = 50; }
  if(offset < 0 || offset > 1000) { offset = 0; }

  // get center and radius for bounds
  LngLat center((lngLat00.longitude + lngLat11.longitude)/2, (lngLat00.latitude + lngLat11.latitude)/2);
  double heightkm = lngLatDist(lngLat00, LngLat(lngLat00.longitude, lngLat11.latitude));
  double widthkm = lngLatDist(lngLat11, LngLat(lngLat00.longitude, lngLat11.latitude));
  double radius = std::max(heightkm, widthkm);

  std::string json;
  json.reserve(65536);
  bool ok = searchDB.searchDist
      .bind(searchStr, center.longitude, center.latitude, radius, limit, offset)
      .exec([&](int rowid, double lng, double lat, double score, const char* tags, const char* props){
        json.append(json.empty() ? "[ " : ", ");
        json.append(fstring(R"#({"lng": %.7f, "lat": %.7f, "score": %.6f, "tags": "%s", "props": )#", lng, lat, score, tags));
        json.append(props).append("}");
      });

  if(!ok) { return {}; }
  json.append(json.empty() ? "[]" : " ]");
  return json;
}


// categorical search

static const std::unordered_map<std::string, std::vector<std::string>> categories_map = {
    {"restaurant", {"fast + food", "food + court"}},
    {"food", {"restaurant"}},
    {"coffee", {"cafe"}},
    {"bar", {"pub", "biergarten"}},
    {"pub", {"bar"}},
    {"college", {"university"}},
    {"school", {"college", "university"}},
    {"gas", {"fuel"}},
    {"gas station", {"fuel"}},
    {"movie", {"cinema"}},
    {"theater", {"cinema"}},
    {"liquor", {"alcohol"}},
    {"grocery", {"supermarket", "greengrocer"}},
    {"groceries", {"supermarket", "greengrocer"}},
    {"barber", {"hairdresser"}},
    {"diy", {"doityourself", "hardware"}},
    {"hardware", {"doityourself"}},
    {"electronics", {"computer", "hifi"}},
    {"charity", {"second + hand"}},
    {"second hand", {"charity"}},
    {"auto", {"car"}},
    {"bike", {"", "(bike OR bicycle) NOT (rental OR parking)"}},  // empty first string to indicate replacement
    {"bicycle", {"", "bicycle NOT (rental OR parking)"}},
    {"hotel", {"motel", "hostel", "guest + house"}},
    {"motel", {"hotel", "hostel", "guest + house"}},
    {"accomodation", {"hotel", "motel", "hostel", "guest + house", "apartment", "chalet"}},
    {"lodging", {"hotel", "motel", "hostel", "guest + house", "apartment", "chalet"}},
    {"park", {"", "park NOT parking"}}
};

static const std::unordered_map<std::string, std::string> replacements_map = {
    {"bike", "(bike OR bicycle)"},
    {"restaurant", "(restaurant OR food)"},
    {"restaurants", "(restaurant OR food)"},
    {"food", "(restaurant OR food)"}
};

static const std::vector<std::string> extrawords = {
    " me", " near", " nearby", " store", " shop"
};

std::string transformQuery(std::string q)
{
  bool replaced = false;
  std::transform(q.begin(), q.end(), q.begin(), [](char c){ return std::tolower(c); });

  // remove extraneous trailing words
  for(const auto &ew : extrawords) {
    if(q.ends_with(ew)) {
      replaced = true;
      q = q.substr(0, q.size() - ew.size());
    }
  }

  // find in categories_map
  auto it = categories_map.find(q);
  if(it == categories_map.end()) {
    it = categories_map.find(q.substr(0, q.size()-1));
  }

  if(it != categories_map.end()) {
    const auto& catVec = it->second;
    if(catVec.size() > 1 && catVec[0].empty()) { return catVec[1]; }
    return q + " OR " + joinStr(catVec, " OR ");
  }

  auto qwords = splitStr<std::vector>(q, " ");
  for(auto& w : qwords) {
    auto it2 = replacements_map.find(w);
    if(it2 != replacements_map.end()) {
      replaced = true;
      w = it2->second;
    }
    else
      w = '"' + w + '"';
  }

  return replaced ? joinStr(qwords, " AND ") : "";
}
