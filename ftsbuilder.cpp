#include <geodesk/geodesk.h>
#include "tilebuilder.h"
#include "ulib.h"

#define SQLITEPP_LOGE LOG
#define SQLITEPP_LOGW LOG
#include "sqlitepp.h"

// search index query

static const char* searchNoDistSQL = "SELECT pois.rowid, lng, lat, rank, tags, props FROM pois_fts JOIN pois ON"
    " pois.ROWID = pois_fts.ROWID WHERE pois_fts MATCH ? ORDER BY osmSearchRank(rank, pois.tags) LIMIT 50 OFFSET ?;";
//static const char* searchDistSQL = "SELECT pois.rowid, lng, lat, rank, tags, props FROM pois_fts JOIN pois ON"
//    " pois.ROWID = pois_fts.ROWID WHERE pois_fts MATCH ? ORDER BY osmSearchRank(rank, lng, lat) LIMIT 50 OFFSET ?;";
//static const char* searchOnlyDistSQL = "SELECT pois.rowid, lng, lat, rank, tags, props FROM pois_fts JOIN pois ON"
//    " pois.ROWID = pois_fts.ROWID WHERE pois_fts MATCH ? ORDER BY osmSearchRank(-1.0, lng, lat) LIMIT 50 OFFSET ?;";

class SearchDB : public SQLiteDB
{
public:
  SQLiteStmt searchNoDist = {NULL};
  //SQLiteStmt searchDist = {NULL};
  //SQLiteStmt searchOnlyDist = {NULL};
  SQLiteStmt insertPOI = {NULL};
};

thread_local SearchDB searchDB;

static const char* POI_SCHEMA = R"#(PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;
CREATE TABLE pois(name TEXT, admin TEXT, tags TEXT, props TEXT, lng REAL, lat REAL);)#";

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
    if(!searchDB.exec(POI_SCHEMA)) { return false; }
    if(!searchDB.exec(POI_FTS_SCHEMA)) {
      LOG("Error creating FTS index");
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

  auto time0 = std::chrono::steady_clock::now();
  std::function<void(TileID)> buildFn = [&](TileID id){
    if(id.z == 4) {
      auto t1 = std::chrono::steady_clock::now();
      double dt = std::chrono::duration<double>(t1 - time0).count();
      LOG("+%.0fs: processing %s", dt, id.toString().c_str());
    }
    if(id.z < 4 || (id.z < 10 && worldGOL(TileBuilder::tileBox(id)).count() > 65535)) {
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
  dbWriter.waitForIdle();
  return 0;
}

//template<class T> bool pointInPolygon(const std::vector<std::vector<T>>& poly, T p)
static bool pointInPolygon(const vt_polygon& poly, vt_point p)
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
  static std::vector<geodesk::Key> poiTags = [&](){
    std::vector<std::string> tags = { "place", "natural", "amenity", "leisure", "tourism", "historic",
        "waterway", "shop", "sport", "landuse", "highway", "building", "railway", "aerialway", "memorial", "cuisine" };
    std::vector<geodesk::Key> keys;
    keys.reserve(tags.size());
    for(auto& tag : tags) { keys.push_back(worldFeats->key(tag)); }
    return keys;
  }();

  m_tileBox = tileBox(m_id);  //, eps);
  Features tileFeats = world(m_tileBox);
  m_tileFeats = &tileFeats;

  Features pois = tileFeats("na[name]");  //"n[place=*]"
  if(pois.begin() == pois.end()) { return {}; }  // skip admin area processing if nothing to index

  struct AdminMPoly { int level; int64_t id; std::string name, name_en; vt_point min, max; vt_multi_polygon mpoly; };
  std::vector<AdminMPoly> adminMPolys;

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
    std::string name_en = readTag(f, "name:en");
    std::string names = (!name_en.empty() && name_en != name) ? name + " " + name_en : name;

    for(auto& tag : poiTags) {
      auto val = f[tag];
      if(val && val != "yes") {
        //if(maintag.empty()) { maintag = val; }
        if(!tags.empty()) { tags += ' '; }
        tags.append(val);
      }
    }

    auto coords = f.xy();
    double lng = Mercator::lonFromX(coords.x);
    double lat = Mercator::latFromY(coords.y);
    vt_point pt = toTileCoord(coords);

    for(auto& mp : adminMPolys) {
      if(pt.x < mp.min.x || pt.y < mp.min.y || pt.x > mp.max.x || pt.y > mp.max.y) { continue; }
      for(auto& poly : mp.mpoly) {
        if(pointInPolygon(poly, pt)) {
          if(!adminfts.empty()) { adminfts += ' '; }
          adminfts.append(mp.name);
          if(!admin.empty()) { admin += ", "; }
          admin.append(!mp.name_en.empty() ? mp.name_en : mp.name);
          break;
        }
      }
    }

    addJson(props, "osm_id", std::to_string(f.id()));
    addJson(props, "osm_type", f.isWay() ? "way" : f.isNode() ? "node" : "relation");
    addJson(props, "name", name);
    addJson(props, "name:en", name_en);
    addJson(props, "admin", admin);
    //addJson(props, "place", readTag(f, "place"));
    //addJson(props, "population", readTag(f, "population"));
    //addJson(props, "type", maintag);
    props.append(" }");

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
  static std::unordered_map<std::string, int> tagOrder = [](){
    std::unordered_map<std::string, int> res;
    const char* tags[] = { "country", "state", "province", "city", "town", "island", "suburb", "quarter",
        "neighbourhood", "district", "borough", "municipality", "village", "hamlet", "county", "locality", "islet" };
    int ntags = sizeof(tags)/sizeof(tags[0]);
    for(int ii = 0; ii < ntags; ++ii) {
      res.emplace(tags[ii], ntags - ii);
    }
    return res;
  }();

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

  if(argc < 6 || sqlite3_value_type(argv[2]) != SQLITE_FLOAT || sqlite3_value_type(argv[3]) != SQLITE_FLOAT
      || sqlite3_value_type(argv[4]) != SQLITE_FLOAT || sqlite3_value_type(argv[5]) != SQLITE_FLOAT) {
    sqlite3_result_double(context, rank);
    return;
  }
  double lon0 = sqlite3_value_double(argv[2]);
  double lat0 = sqlite3_value_double(argv[3]);
  double lon = sqlite3_value_double(argv[4]);
  double lat = sqlite3_value_double(argv[5]);
  double dist = lngLatDist(LngLat(lon0, lat0), LngLat(lon, lat));  // in kilometers
  // obviously will want a more sophisticated ranking calculation in the future
  sqlite3_result_double(context, rank/log2(1+dist));
}

// note that this is called on a cpp-httplib thread (and that searchDB is thread local!)
std::string ftsQuery(const std::string& query, const std::string& searchDBPath)
{
  if(!searchDB.db) {
    if(searchDB.open(searchDBPath, SQLITE_OPEN_READONLY) != SQLITE_OK) {
      LOG("Error opening search DB on http worker thread!");
      return {};
    }
    searchDB.searchNoDist = searchDB.stmt(searchNoDistSQL);

    if(sqlite3_create_function(searchDB.db, "osmSearchRank", 3, SQLITE_UTF8, 0, udf_osmSearchRank, 0, 0) != SQLITE_OK) {
      LOG("sqlite3_create_function: error creating osmSearchRank for search DB");
      return {};
    }
  }

  //auto it = req.params.find("offset");
  int offset = 0;

  // words containing any special characters need to be quoted, so just quote every word (and make AND
  //  operation explicit)
  auto words = splitStr<std::vector>(query, " ", true);
  std::string searchStr = "\"" + joinStr(words, "\" AND \"") + "\"*";

  std::string json;
  json.reserve(65536);
  bool ok = searchDB.searchNoDist
      .bind(searchStr, offset)
      .exec([&](int rowid, double lng, double lat, double score, const char* tags, const char* props){
        json.append(json.empty() ? "[ " : ", ");
        json.append(fstring(R"#({"lng": %.7f, "lat": %.7f, "score": %.6f, "tags": "%s", "props": )#", lng, lat, score, tags));
        json.append(props).append("}");
      });

  if(!ok) { return {}; }
  json.append(json.empty() ? "[]" : " ]");
  return json;
}
