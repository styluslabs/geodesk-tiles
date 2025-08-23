#include <geodesk/geodesk.h>

#define LOG(fmt, ...) fprintf(stderr, fmt "\n", ## __VA_ARGS__)
#define LOGT(t0, fmt, ...) do { \
  auto t1 = std::chrono::steady_clock::now(); \
  double dt = std::chrono::duration<double>(t1 - t0).count(); \
  fprintf(stderr, "+%.3f s: " fmt "\n", dt, ## __VA_ARGS__); \
} while(0);

#define SQLITEPP_LOGE LOG
#define SQLITEPP_LOGW LOG
#include "sqlitepp.h"

using geodesk::Feature;
using geodesk::Features;
using geodesk::Mercator;
using geodesk::Coordinate;
using geodesk::TagValue;

// search index build

static const char* POI_SCHEMA = R"#(PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;
CREATE TABLE pois(name TEXT, tags TEXT, props TEXT, lng REAL, lat REAL);)#";

static const char* POI_FTS_SCHEMA = R"#(BEGIN;
CREATE VIRTUAL TABLE pois_fts USING fts5(name, tags, content='pois');

-- triggers to keep the FTS index up-to-date
CREATE TRIGGER pois_insert AFTER INSERT ON pois BEGIN
  INSERT INTO pois_fts(rowid, name, tags) VALUES (NEW.rowid, NEW.name, NEW.tags);
END;
CREATE TRIGGER pois_delete AFTER DELETE ON pois BEGIN
  INSERT INTO pois_fts(pois_fts, rowid, name, tags) VALUES ('delete', OLD.rowid, OLD.name, OLD.tags);
END;
CREATE TRIGGER pois_update AFTER UPDATE ON pois BEGIN
  INSERT INTO pois_fts(pois_fts, rowid, name, tags) VALUES ('delete', OLD.rowid, OLD.name, OLD.tags);
  INSERT INTO pois_fts(rowid, name, tags) VALUES (NEW.rowid, NEW.name, NEW.tags);
END;
COMMIT;)#";

// options: iterate over every feature in GOL
// start with only place=* nodes for testing

#define readTag(feat, s) feat[ ( [](){ static geodesk::Key cs = worldFeats->key(s); return cs; }() ) ]

static void addJson(std::string& json, const std::string& key, const std::string& val)
{
  if(val.empty()) { return; }
  json.append(json.empty() ? "{ " : ", ");
  json.append("\"").append(key).append("\": \"").append(val).append("\"");
}

// we expect FTS index creation to take longer than iterating features, so not much benefit from multithreading
int buildSearchIndex(const Features& world)
{
  static std::vector<std::string> poiTags = { "place", "natural", "amenity", "leisure", "tourism", "historic",
      "waterway", "shop", "sport", "landuse", "highway", "building", "railway", "aerialway", "memorial", "cuisine" };

  static const Features* worldFeats;
  worldFeats = &world;

  SQLiteDB searchDB;
  if(searchDB.open("fts_wip.sqlite", SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) != SQLITE_OK) {
    LOG("Error opening search DB");
    return -1;
  }

  if(!searchDB.exec(POI_SCHEMA)) { return -1; }

  if(!searchDB.exec(POI_FTS_SCHEMA)) {
    LOG("Error creating FTS index");
    return -1;
  }

  char const* insertPOISQL = "INSERT INTO pois (name,tags,props,lng,lat) VALUES (?,?,?,?,?);";
  SQLiteStmt insertPOI = searchDB.stmt(insertPOISQL);

  Features pois = world("na[name]");  //"n[place=*]"

  auto t0 = std::chrono::steady_clock::now();
  std::string props;
  int64_t nfeats = 0;
  searchDB.exec("BEGIN;");
  for(Feature f : pois) {
    std::string name = readTag(f, "name");
    if(name.empty()) { continue; }
    std::string name_en = readTag(f, "name:en");
    std::string names = !name_en.empty() ? name + " " + name_en : name;

    std::string tags;  //std::string maintag;
    for(auto& tag : poiTags) {
      auto val = f[tag.key];
      if(val) {
        //if(maintag.empty()) { maintag = val; }
        if(!tags.empty()) { tags += ' '; }
        tags.append(val);
      }
    }

    addJson(props, "osm_id", std::to_string(f.id()));
    addJson(props, "osm_type", f.isWay() ? "way" : f.isNode() ? "node" : "relation");
    addJson(props, "name", name);
    addJson(props, "name:en", name_en);
    //addJson(props, "place", readTag(f, "place"));
    //addJson(props, "population", readTag(f, "population"));
    //addJson(props, "type", maintag);
    props.append(" }");

    auto coords = f.xy();
    double lng = Mercator::lonFromX(coords.x);
    double lat = Mercator::latFromY(coords.y);

    insertPOI.bind(names, tags, props, lng, lat).exec();
    props.clear();

    ++nfeats;
    props.clear();
    if(nfeats%100000 == 0) {
      searchDB.exec("COMMIT; BEGIN;");
      LOGT(t0, "Processed %ld features...", nfeats);
    }
  }
  searchDB.exec("COMMIT;");
  LOGT(t0, "pois table built (%ld features)", nfeats);

  //LOGT(t0, "Exiting...");
  return 0;
}

void udf_osmSearchRank(sqlite3_context* context, int argc, sqlite3_value** argv)
{
  static std::unordered_map<std::string, int> tagOrder = [](){
    std::unordered_map<std::string, int> res;
    const char* tags[] = { "country", "state", "province", "city", "town", "island", "suburb", "quarter",
        "neighbourhood", "district", "borough", "municipality", "village", "hamlet", "county", "locality", "islet" };
    int ntags = sizeof(tags)/sizeof(tags[0]);
    for(int ii = 0; ii < ntags; ++ii) {
      res.insert(tags[ii], ntags - ii);
    }
    return res;
  };

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
  const char* tags = sqlite3_value_text(argv[1]);

  const char* tagend = tags;
  while(*tagend && *tagend != ' ') { ++tagend; }
  if(*tagend != tags) {
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
