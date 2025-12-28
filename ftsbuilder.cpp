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

static const char* searchNoDistSQL = "SELECT pois.rowid, lng, lat, bm25_once(pois_fts, 1.0, 1.0, 0.25, 0.5) AS score, pois.tags, props FROM pois_fts JOIN pois ON"
    " pois.ROWID = pois_fts.ROWID WHERE pois_fts MATCH ? ORDER BY osmSearchRank(score, pois.tags) LIMIT ? OFFSET ?;";
static const char* searchDistSQL = "SELECT pois.rowid, lng, lat, bm25_once(pois_fts, 1.0, 1.0, 0.25, 0.5) AS score, pois.tags, props FROM pois_fts JOIN pois ON"
    " pois.ROWID = pois_fts.ROWID WHERE pois_fts MATCH ? ORDER BY osmSearchRank(score, pois.tags, lng, lat, ?, ?, ?) LIMIT ? OFFSET ?;";
static const char* searchOnlyDistSQL = "SELECT pois.rowid, lng, lat, -1.0, pois.tags, props FROM pois_fts JOIN pois ON"
    " pois.ROWID = pois_fts.ROWID WHERE pois_fts MATCH ? ORDER BY osmSearchRank(-1.0, '', lng, lat, ?, ?, ?) LIMIT ? OFFSET ?;";
static const char* searchBoundedSQL = R"#(SELECT p.rowid, p.lng, p.lat, -1.0, p.tags, p.props
  FROM rtree_index r JOIN pois p ON p.rowid = r.id JOIN pois_fts f ON f.rowid = p.rowid
  WHERE r.minLng >= ? AND r.maxLng <= ? AND r.minLat >= ? AND r.maxLat <= ? AND pois_fts MATCH ?
  ORDER BY osmSearchRank(-1.0, '', p.lng, p.lat, ?, ?, ?) LIMIT ? OFFSET ?;)#";
static const char* countMatchesSQL = "SELECT count(1) FROM pois_fts WHERE pois_fts MATCH ?;";


class SearchDB : public SQLiteDB
{
public:
  SQLiteStmt searchNoDist = {NULL};
  SQLiteStmt searchDist = {NULL};
  SQLiteStmt searchOnlyDist = {NULL};
  SQLiteStmt searchBounded = {NULL};
  SQLiteStmt countMatches = {NULL};
  SQLiteStmt insertPOI = {NULL};
};

thread_local SearchDB searchDB;

static const char* POI_SCHEMA = R"#(PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;
CREATE TABLE pois(name TEXT, name_en TEXT, admin TEXT, tags TEXT, props TEXT, lng REAL, lat REAL);
CREATE VIRTUAL TABLE pois_fts USING fts5(name, name_en, admin, tags, content='pois');

CREATE VIRTUAL TABLE rtree_index USING rtree(id, minLng, maxLng, minLat, maxLat);
)#";

//CREATE TRIGGER pois_insert AFTER INSERT ON pois BEGIN
//  INSERT INTO pois_fts(rowid, name, admin, tags) VALUES (NEW.rowid, NEW.name, NEW.admin, NEW.tags);
//END;

struct PoiRow { std::string name, name_en, admin, tags, props; double lng, lat; };

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

  auto dbfut = dbWriter.enqueue([&](){
    if(searchDB.open(searchDBPath, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) != SQLITE_OK) {
      LOG("Error opening search DB %s", searchDBPath.c_str());
      return false;
    }
    //if(!searchDB.exec(POI_SCHEMA)) { return false; }
    if(!searchDB.exec(POI_SCHEMA)) {
      LOG("Error creating FTS tables: %s", searchDB.errMsg());
      return false;
    }
    char const* insertPOISQL = "INSERT INTO pois (name,name_en,admin,tags,props,lng,lat) VALUES (?,?,?,?,?,?,?);";
    searchDB.insertPOI = searchDB.stmt(insertPOISQL);
    return true;
  });
  if(!dbfut.get()) { return -1; }

  size_t nfeats = 0;
  auto t0 = std::chrono::steady_clock::now();
  std::function<void(TileID)> buildFn = [&](TileID id){
    if(id.z < 4 || (id.z < 10 && isHeavyTile(worldGOL, id))) {
      for(int ii = 0; ii < 4; ++ii)
        indexWorkers.enqueue(buildFn, id.getChild(ii, 10));
      return;
    }
    int idmask = (1 << (id.z - 4)) - 1;  // print for upper-leftmost tile inside each z4 tile
    if((id.x & idmask) == 0 && (id.y & idmask) == 0)
      LOGT(t0, "processing %s", id.withMaxSourceZoom(4).toString().c_str());

    std::vector<PoiRow> rows = indexTile(worldGOL, id);
    if(!rows.empty()) {
      nfeats += rows.size();
      dbWriter.enqueue([&, rows = std::move(rows), id](){
        searchDB.exec("BEGIN;");
        for(auto& r : rows) {
          if(!searchDB.insertPOI.bind(r.name, r.name_en, r.admin, r.tags, r.props, r.lng, r.lat).exec())
            LOG("Error adding row to search DB: %s", searchDB.errMsg());
        }
        searchDB.exec("COMMIT;");
      });
    }

  };
  //onSigInt = [&](){ buildWorkers.requestStop(true); };
  indexWorkers.enqueue(buildFn, toptile);
  indexWorkers.waitForIdle();
  LOGT(t0, "%zu features processed", nfeats);
  dbWriter.enqueue([&](){
    LOGT(t0, "Building FTS index...");
    searchDB.exec("INSERT INTO pois_fts(pois_fts) VALUES('rebuild');");
    LOGT(t0, "Building rtree index...");
    searchDB.exec("INSERT INTO rtree_index SELECT rowid, lng, lng, lat, lat FROM pois;");
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
  json.append("\"").append(key).append("\": \"");
  for(const char& c : val) {
    if(c == '\\' || c == '"') { json += '\\'; }
    json += c;
  }
  json += '"';
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
    int level = leveltag ? double(leveltag) : INT_MAX;
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
      if(name_en == name) { name_en.clear(); }
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

    auto leveltag = readTag(f, "admin_level");
    // don't include boundary=admin areas because they duplicate place= nodes
    if(leveltag) {
      auto bndry = readTag(f, "boundary");
      if(bndry && (bndry == "administrative" || bndry == "disputed")) { continue; }
    }
    int flevel = leveltag ? double(leveltag) : INT_MAX;

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
      if(flevel <= mp.level) { continue; }  // only include lower admin levels
      //++numPinPTests;
      for(auto& poly : mp.mpoly) {
        if(pointInPolygon(poly, pt)) {
          //++numPinPHits;
          if(!adminfts.empty()) { adminfts.push_back(' '); }
          if(!mp.name_en.empty()) { adminfts.append(mp.name_en).push_back(' '); }
          adminfts.append(mp.name);
          if(!admin.empty()) { admin.append(", "); }
          admin.append(!mp.name_en.empty() ? mp.name_en : mp.name);
          break;
        }
      }
    }

    std::string name_en = readTag(f, "name:en");
    if(name_en == name) { name_en.clear(); }

    addJson(props, "osm_id", std::to_string(f.id()));
    addJson(props, "osm_type", f.isWay() ? "way" : f.isNode() ? "node" : "relation");
    addJson(props, "name", name);
    addJson(props, "name_en", name_en);
    addJson(props, "admin", admin);
    //addJson(props, "place", readTag(f, "place"));
    //addJson(props, "population", readTag(f, "population"));
    //addJson(props, "type", maintag);
    props.append(" }");

    double lng = Mercator::lonFromX(coords.x);
    double lat = Mercator::latFromY(coords.y);

    rows.emplace_back(name, name_en, adminfts, tags, props, lng, lat);
    tags.clear(); props.clear(); admin.clear(); adminfts.clear();
  }

  m_tileFeats = nullptr;

  return rows;
}

// searching

// scoring fn - cut and paste of bm25 from sqlite fts5_aux.c

// The first time the bm25() function is called for a query, an instance
// of the following structure is allocated and populated.
typedef struct Fts5Bm25Data Fts5Bm25Data;
struct Fts5Bm25Data {
  int nPhrase;                    /* Number of phrases in query */
  double avgdl;                   /* Average number of tokens in each row */
  double *aIDF;                   /* IDF for each phrase */
  double *aFreq;                  /* Array used to calculate phrase freq. */
};

// Callback used by fts5Bm25GetData() to count the number of rows in the
// table matched by each individual phrase within the query.
static int fts5CountCb(const Fts5ExtensionApi *pApi, Fts5Context *pFts, void *pUserData)
{
  sqlite3_int64 *pn = (sqlite3_int64*)pUserData;
  //UNUSED_PARAM2(pApi, pFts);
  (*pn)++;
  return SQLITE_OK;
}

// Set *ppData to point to the Fts5Bm25Data object for the current query.
// If the object has not already been allocated, allocate and populate it
// now.
static int fts5Bm25GetData(const Fts5ExtensionApi *pApi, Fts5Context *pFts, Fts5Bm25Data **ppData)
{
  int rc = SQLITE_OK;             /* Return code */
  Fts5Bm25Data *p;                /* Object to return */

  p = (Fts5Bm25Data*)pApi->xGetAuxdata(pFts, 0);
  if( p==0 ){
    int nPhrase;                  /* Number of phrases in query */
    sqlite3_int64 nRow = 0;       /* Number of rows in table */
    //sqlite3_int64 nToken = 0;     /* Number of tokens in table */
    sqlite3_int64 nByte;          /* Bytes of space to allocate */
    int i;

    /* Allocate the Fts5Bm25Data object */
    nPhrase = pApi->xPhraseCount(pFts);
    nByte = sizeof(Fts5Bm25Data) + nPhrase*2*sizeof(double);
    p = (Fts5Bm25Data*)sqlite3_malloc64(nByte);
    if( p==0 ){
      rc = SQLITE_NOMEM;
    }else{
      memset(p, 0, (size_t)nByte);
      p->nPhrase = nPhrase;
      p->aIDF = (double*)&p[1];
      p->aFreq = &p->aIDF[nPhrase];
    }

    /* Calculate the average document length for this FTS5 table */
    if( rc==SQLITE_OK ) rc = pApi->xRowCount(pFts, &nRow);
    assert( rc!=SQLITE_OK || nRow>0 );
    //~if( rc==SQLITE_OK ) rc = pApi->xColumnTotalSize(pFts, 0, &nToken);  // total name tokens
    //~if( rc==SQLITE_OK ) p->avgdl = (double)nToken  / (double)nRow;

    /* Calculate an IDF for each phrase in the query */
    for(i=0; rc==SQLITE_OK && i<nPhrase; i++){
      sqlite3_int64 nHit = 0;
      rc = pApi->xQueryPhrase(pFts, i, (void*)&nHit, fts5CountCb);
      if( rc==SQLITE_OK ){
        double idf = log( (nRow - nHit + 0.5) / (nHit + 0.5) );
        if( idf<=0.0 ) idf = 1e-6;
        p->aIDF[i] = idf;
      }
    }

    if( rc!=SQLITE_OK ){
      sqlite3_free(p);
    }else{
      rc = pApi->xSetAuxdata(pFts, p, sqlite3_free);
    }
    if( rc!=SQLITE_OK ) p = 0;
  }
  *ppData = p;
  return rc;
}

static void fts5Bm25Function(
  const Fts5ExtensionApi *pApi,   /* API offered by current FTS version */
  Fts5Context *pFts,              /* First arg to pass to pApi functions */
  sqlite3_context *pCtx,          /* Context for returning result/error */
  int nVal,                       /* Number of values in apVal[] array */
  sqlite3_value **apVal           /* Array of trailing arguments */
){
  //const double k1 = 1.2;          /* Constant "k1" from BM25 formula */
  //const double b = 0.75;          /* Constant "b" from BM25 formula */
  int rc;                         /* Error code */
  double score = 0.0;             /* SQL function return value */
  Fts5Bm25Data *pData;            /* Values allocated/calculated once only */
  int i;                          /* Iterator variable */
  int nInst = 0;                  /* Value returned by xInstCount() */
  double D[2] = {0, 0};           // adjustment for name token count
  double *aFreq = 0;              /* Array of phrase freq. for current row */

  /* Calculate the phrase frequency (symbol "f(qi,D)" in the documentation)
  ** for each phrase in the query for the current row. */
  rc = fts5Bm25GetData(pApi, pFts, &pData);
  if( rc==SQLITE_OK ){
    aFreq = pData->aFreq;
    memset(aFreq, 0, sizeof(double) * pData->nPhrase);
    rc = pApi->xInstCount(pFts, &nInst);
  }
  // token counts for name columns
  if( rc==SQLITE_OK ){
    int nTok;
    rc = pApi->xColumnSize(pFts, 0, &nTok);
    D[0] = nTok > 0 ? 0.1*log10((double)nTok) : 0;  // nTok == 0 should never happen
    rc = pApi->xColumnSize(pFts, 1, &nTok);
    D[1] = nTok > 0 ? 0.1*log10((double)nTok) : 0;
  }
  // weights for each phrase
  for(i=0; rc==SQLITE_OK && i<nInst; i++){
    int ip; int ic; int io;
    rc = pApi->xInst(pFts, i, &ip, &ic, &io);  // phrase, column, offset (within col)
    if( rc!=SQLITE_OK ){ break; }
    double w = (nVal > ic) ? sqlite3_value_double(apVal[ic]) : 1.0;
    if(ip == 0 && ic <= 1 && io == 0) { w *= 2; }  // prefix boost for first phrase for name columns
    // adjustment for name length - scaled to contribute O(0.1) to final score so tag adjustment dominates
    if(ic <= 1) { w -= D[ic]/pData->aIDF[ip]; }
    if(aFreq[ip] < w) { aFreq[ip] = w; }  //aFreq[ip] += w; -- don't count phrase more than once
  }
  if( rc==SQLITE_OK ){
    for(i=0; i<pData->nPhrase; i++){
      score += pData->aIDF[i] * aFreq[i];  // simple TF-IDF
      //pData->aIDF[i]*(aFreq[i]*(k1 + 1.0))/(aFreq[i] + k1*(1 - b + b*D/pData->avgdl));
    }
    sqlite3_result_double(pCtx, -1.0 * score);
  }else{
    sqlite3_result_error_code(pCtx, rc);
  }
}

static fts5_api* mfts5_api_from_db(sqlite3 *db)
{
  fts5_api *pRet = 0;
  sqlite3_stmt *pStmt = 0;

  if( SQLITE_OK==sqlite3_prepare(db, "SELECT fts5(?1)", -1, &pStmt, 0) ){
    sqlite3_bind_pointer(pStmt, 1, (void*)&pRet, "fts5_api_ptr", NULL);
    sqlite3_step(pStmt);
  }
  sqlite3_finalize(pStmt);
  return pRet;
}

// distance and tag score adjustments

static double lngLatDist(LngLat r1, LngLat r2)
{
  constexpr double p = 3.14159265358979323846/180;
  double a = 0.5 - cos((r2.latitude-r1.latitude)*p)/2 + cos(r1.latitude*p) * cos(r2.latitude*p) * (1-cos((r2.longitude-r1.longitude)*p))/2;
  return 12742 * asin(sqrt(a));  // kilometers
}

// ChatGPT
static LngLat lngLatOffset(LngLat r0, double x_km, double y_km)
{
  constexpr double R = 6371.0;
  constexpr double deg = 180.0 / 3.14159265358979323846;
  double lat = r0.latitude + (y_km / R) * deg;
  double lng = r0.longitude + (x_km / R / std::cos(r0.latitude / deg)) * deg;
  return LngLat(lng, lat);
}

static double applyDistScore(double rank, LngLat lngLat0, LngLat lngLat1, double rad)
{
  if(rad <= 0) { return rank; }
  double dist = lngLatDist(lngLat0, lngLat1);  // in kilometers
  return rank + 0.01*log2(0.001 + dist/20000.0);  // earth circum ~40000km; map 0 to max to -0.1 to 0
}

static double applyTagScore(double rank, const char* tags)
{
  static std::unordered_map<std::string, int> tagOrder = { {"heritage", 64}, {"wikipedia", 63},
      {"nature_reserve", 62}, {"park", 61}, {"peak", 61}, {"volcano", 61},
      {"country", 90}, {"state", 85}, {"province", 85}, {"city", 80}, {"town", 70}, {"island", 65},
      {"suburb", 60}, {"quarter", 55}, {"neighbourhood", 50}, {"district", 45}, {"borough", 40},
      {"municipality", 35}, {"village", 30}, {"hamlet", 25}, {"county", 20}, {"locality", 15}, {"islet", 10},
      {"vending_machine", -100} };
  // things that should be covered by wikipedia boost: university, college, museum

  const char* tagend = tags;
  while(*tagend && *tagend != ' ') { ++tagend; }
  if(tagend != tags) {
    auto it = tagOrder.find(std::string(tags, tagend));
    if(it != tagOrder.end()) {
      rank -= it->second/100.0;  // adjust rank to break ties
    }
  }
  else
    rank *= 0.5;  // heavily downrank results w/ no tags

  return rank;
}

static void udf_osmSearchRank(sqlite3_context* context, int argc, sqlite3_value** argv)
{
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

  const char* tags = (const char*)sqlite3_value_text(argv[1]);
  rank = applyTagScore(rank, tags);

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
  rank = applyDistScore(rank, LngLat(lon0, lat0), LngLat(lon, lat), rad0);
  sqlite3_result_double(context, rank);
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
    {"supermarket", {"greengrocer"}},
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
    {"mt", "(mt OR mount)"},
    {"bike", "(bike OR bicycle)"},
    {"restaurant", "(restaurant OR food)"},
    {"restaurants", "(restaurant OR food)"},
    {"food", "(restaurant OR food)"},
    // US -> UK for common tags
    {"center", "(center OR centre)"},
    {"neighborhood", "(neighborhood OR neighbourhood)"}
};

static const std::vector<std::string> extrawords = {
    " me", " near", " nearby", " store", " shop"
};

// note that this is called on a cpp-httplib thread (and that searchDB is thread local!)
std::string ftsQuery(const std::multimap<std::string, std::string>& params, const std::string& searchDBPath)
{
  //static int maxWorldHits = [](){ const char* s = getenv("ASCEND_MAX_WORLD_HITS"); return s ? atoi(s) : 0; }();
  if(!searchDB.db) {
    if(searchDB.open(searchDBPath, SQLITE_OPEN_READONLY) != SQLITE_OK) {
      LOG("Error opening search database %s on http worker thread!", searchDBPath.c_str());
      return {};
    }
    if(sqlite3_create_function(searchDB.db, "osmSearchRank", -1, SQLITE_UTF8, 0, udf_osmSearchRank, 0, 0) != SQLITE_OK) {
      LOG("sqlite3_create_function: error creating osmSearchRank for search DB");
      return {};
    }
    //SQLITE_EXTENSION_INIT2(pApi);
    fts5_api* api = mfts5_api_from_db(searchDB.db);
    if(!api || api->xCreateFunction(api, "bm25_once", NULL, fts5Bm25Function, NULL) != SQLITE_OK) {
      LOG("error adding custom FTS5 ranking function for search DB");
      return {};
    }
    searchDB.searchNoDist = searchDB.stmt(searchNoDistSQL);
    searchDB.searchDist = searchDB.stmt(searchDistSQL);
    searchDB.searchOnlyDist = searchDB.stmt(searchOnlyDistSQL);
    searchDB.searchBounded = searchDB.stmt(searchBoundedSQL);
    searchDB.countMatches = searchDB.stmt(countMatchesSQL);
    //LOG("Loaded FTS database %s", searchDBPath.c_str());
  }

  static auto isTrue = [](const std::string& s) { return s == "true" || s == "1"; };
  auto getParam = [&](const char* key){
    auto it = params.find(key);
    return it == params.end() ? std::string() : it->second;
  };

  std::string q = getParam("q");
  if(q.empty()) { return "[]"; }
  int offset = atoi(getParam("offset").c_str());
  int limit = atoi(getParam("limit").c_str());
  std::string sortBy = getParam("sort");
  bool debug = isTrue(getParam("debug"));
  bool bounded = isTrue(getParam("bounded"));
  bool autocomplete = isTrue(getParam("autocomplete"));
  if(!debug) {
    if(offset < 0 || offset > 1000) { offset = 0; }
    if(limit < 1 || limit > 50) { limit = 50; }
  }
  LngLat lngLat00, lngLat11;
  auto parts = splitStr<std::vector>(getParam("bounds"), ",");
  if(parts.size() == 4) {
    lngLat00 = LngLat(atof(parts[0].c_str()), atof(parts[1].c_str()));
    lngLat11 = LngLat(atof(parts[2].c_str()), atof(parts[3].c_str()));
  }

  // cut and paste from transform_query.js
  std::string searchStr;
  bool isCategorical = false;
  if(q.front() == '!') { q = q.substr(1); isCategorical = true; }
  std::transform(q.begin(), q.end(), q.begin(), [](char c){ return std::tolower(c); });

  // remove extraneous trailing words
  std::string catq = q;
  for(const auto &ew : extrawords) {
    if(catq.ends_with(ew)) {
      catq = catq.substr(0, catq.size() - ew.size());
    }
  }

  // find in categories_map
  auto catIt = categories_map.find(catq);
  if(catIt == categories_map.end()) {
    catIt = categories_map.find(catq.substr(0, catq.size()-1));
  }

  if(isCategorical) {}  // use "!" prefix for unmodified categorical search
  else if(catIt != categories_map.end()) {
    const auto& catVec = catIt->second;
    if(catVec.size() > 1 && catVec[0].empty()) { searchStr = catVec[1]; }
    else { searchStr = catq + " OR " + joinStr(catVec, " OR "); }
    isCategorical = true;
  }
  else {
    auto qwords = splitStr<std::vector>(q, " ", true);
    for(auto& w : qwords) {
      auto it2 = replacements_map.find(w);
      w = it2 != replacements_map.end() ? it2->second : '"' + w + '"';
    }
    // words containing any special characters need to be quoted, so just quote every word (and make AND
    //  operation explicit)
    searchStr = joinStr(qwords, " AND ");
    if(searchStr.back() == '"') { searchStr += "*"; }
    // restrict single word autocomplete search to name
    if(autocomplete && qwords.size() == 1) {
      searchStr = "{name name_en} : " + searchStr;
    }
  }

  // get center and radius for bounds
  LngLat center((lngLat00.longitude + lngLat11.longitude)/2, (lngLat00.latitude + lngLat11.latitude)/2);
  double heightkm = lngLatDist(lngLat00, LngLat(lngLat00.longitude, lngLat11.latitude));
  double widthkm = lngLatDist(lngLat11, LngLat(lngLat00.longitude, lngLat11.latitude));
  double radius = std::max(heightkm, widthkm)/2;
  if(radius > 5000) { radius = 0; }  // disable distance ranking at very low zoom

  int64_t nhits = 0;  //namehits = 0, taghits = 0;
  //searchDB.countMatches.bind("{name name_en tags} : " + searchStr).onerow(nhits);
  // if many hits, we could try to detect categorical search by taghits >> namehits
  //searchDB.countMatches.bind("{name name_en}:" + searchStr).onerow(namehits);
  //searchDB.countMatches.bind("tags:" + searchStr).onerow(taghits);

  bool ok = false;
  std::string json = R"({ "results": [ )";  //fstring(R"({"total": %d, "results": [ )", int(nhits));
  json.reserve(65536);
  auto rowcb = [&](int rowid, double lng, double lat, double score, const char* tags, const char* props){
    //if(debug) {
    //  double tagscore = applyTagScore(score, tags);
    //  double distscore = applyDistScore(tagscore, center, LngLat(lng, lat), radius);
    //  json.append(fstring(
    //      R"#({"lng": %.7f, "lat": %.7f, "score": %.6f, "tag_score": %.6f, "dist_score": %.6f, "tags": "%s", "props": )#",
    //      lng, lat, score, tagscore, distscore, tags));
    //}
    //else
      json.append(fstring(R"#({"lng": %.7f, "lat": %.7f, "score": %.6f, "tags": "%s", "props": )#", lng, lat, score, tags));
    json.append(props).append("},");
  };

  // if too many hits, use bounded search ... this doesn't really help because FTS MATCH still uses full index
  //double hitratio = double(maxWorldHits)/nhits;
  if(bounded) { // || (!debug && && hitratio > 0 && hitratio < 1)) {
    //double arearatio = heightkm*widthkm/150E6;  // total land surface 150E6 km^2
    //if(arearatio > hitratio) {
    //  double r = std::max(1.0, std::sqrt(hitratio*150E6)/2);
    //  lngLat00 = lngLatOffset(center, -r, -r);
    //  lngLat11 = lngLatOffset(center, r, r);
    //}
    //LOG("%s", sqlite3_expanded_sql(ps.stmt));  ...   sqlite3_free()
    ok = searchDB.searchBounded.bind(lngLat00.longitude, lngLat11.longitude, lngLat00.latitude, lngLat11.latitude,
        searchStr, center.longitude, center.latitude, radius, limit, offset).exec(rowcb);
  }
  else {
    ok = (isCategorical || sortBy == "dist" ? searchDB.searchOnlyDist : searchDB.searchDist)
        .bind(searchStr, center.longitude, center.latitude, radius, limit, offset).exec(rowcb);
  }

  if(!ok) { return {}; }
  json.pop_back();
  if(debug) {
    searchDB.countMatches.bind(searchStr).onerow(nhits);
    json.append(fstring(R"( ], "total": %d })", int(nhits)));
  }
  else
    json.append(" ] }");
  return json;
}
