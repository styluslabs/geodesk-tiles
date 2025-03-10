// Serve tiles from multiple mbtiles files, generating missing tiles on demand

#include <format>
#include <map>
#include "tilebuilder.h"
#define SQLITEPP_LOGE LOG
#define SQLITEPP_LOGW LOG
#include "sqlitepp.h"
#include "ulib.h"

// httplib should be last include because it pulls in, e.g., fcntl.h with #defines that break geodesk headers
//#define CPPHTTPLIB_OPENSSL_SUPPORT
//#define CPPHTTPLIB_ZLIB_SUPPORT
#include "httplib.h"

extern std::string buildTile(const Features& world, const Features& ocean, TileID id);

// WAL allows simultaneous reading and writing
static const char* schemaSQL = R"(PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;
BEGIN;
  CREATE TABLE IF NOT EXISTS tiles (zoom_level integer, tile_column integer, tile_row integer, tile_data blob);
  CREATE UNIQUE INDEX IF NOT EXISTS tile_index on tiles (zoom_level, tile_column, tile_row);
COMMIT;)";

static const char* getTileSQL =
    "SELECT tile_data FROM tiles WHERE zoom_level = ? AND tile_column = ? AND tile_row = ?;";
static const char* putTileSQL =
    "INSERT INTO tiles (zoom_level, tile_column, tile_row, tile_data) VALUES (?,?,?,?);";

class TileDB : public SQLiteDB
{
public:
  SQLiteStmt getTile = {NULL};
  SQLiteStmt putTile = {NULL};
};

thread_local TileDB worldDB;

static void sigint_handler(int s)
{
  LOG("SIGINT received, exiting");
  // exit() destroys static objects, so sqlite WAL should be flushed and removed
  // ... but only thread_locals on current thread are destroyed - seems to work but watch out for this
  exit(1);
}

// Building ocean.gol (used to determine if empty tiles are ocean or land):
// - download simplified water polygons from https://osmdata.openstreetmap.de/data/water-polygons.html
//  - note: built with https://github.com/osmcode/osmcoastline
// - unzip and run `PYTHONPATH=$HOME/maps/ogr2osm python3 -m ogr2osm --positive-id --pbf -o ocean.osm.pbf
//  ... simplified-water-polygons-split-3857/simplified_water_polygons.shp`
// #`osmium cat -f pbf ocean.osm.pbf -o ocean_fix.osm.pbf` -- seems unnecessary - resulting GOL size identical
// - run `tool/bin/gol build ocean.gol ocean.osm.pbf`

int main(int argc, char* argv[])
{
  struct Stats_t { std::atomic_uint_fast64_t reqs = 0, reqsok = 0, bytesout = 0, tilesbuilt = 0; } stats;

  std::signal(SIGINT, sigint_handler);

  // defaults
  const char* worldDBPath = "planet.mbtiles";
  int tcpPort = 8080;
  int numBuildThreads = std::max(2U, std::thread::hardware_concurrency()) - 1;
  TileID topTile(-1, -1, -1);
  int maxZ = 14;

  int argi = 1;
  for(; argi < argc-1; argi += 2) {
    if(strcmp(argv[argi], "--port") == 0)
      tcpPort = atoi(argv[argi+1]);
    else if(strcmp(argv[argi], "--threads") == 0)
      numBuildThreads = std::max(1, atoi(argv[argi+1]));
    else if(strcmp(argv[argi], "--db") == 0)
      worldDBPath = argv[argi+1];
    else if(strcmp(argv[argi], "--build") == 0) {
      sscanf(argv[argi+1], "%hhd/%d/%d", &topTile.z, &topTile.x, &topTile.y);
      if(!topTile.isValid()) {
        LOG("Tile id %s is invalid (expected WMTS z/x/y)", argv[argi+1]);
        return -1;
      }
    }
    else if(strcmp(argv[argi], "--maxz") == 0)
      maxZ = atoi(argv[argi+1]);
    else
      break;
  }

  if(argi + 2 != argc) {
    LOG(R"(Usage: server [options] <OSM gol file> <Ocean gol file>
Optional arguments:
  --db <mbtiles file>: sqlite file to store generated tiles; default is planet.mbtiles
  --port <port number>: TCP port to listen on; default is 8080
  --threads <n>: number of tile builder threads; default is CPU cores - 1
  --build <z>/<x>/<y>: build tile z/x/y and all children to maxz, then exit (no server)
  --maxz <z>: maximum tile zoom level; default is 14
)");
    return -1;
  }

  Features worldGOL(argv[argi]);
  Features oceanGOL(argv[argi+1]);
  LOG("Loaded %s and %s", argv[argi], argv[argi+1]);

  TileBuilder::worldFeats = &worldGOL;
  // ... separate queues for high zoom and low zoom (slower build)?
  std::mutex buildMutex;
  std::map< TileID, std::shared_future<std::string> > buildQueue;
  ThreadPool buildWorkers(numBuildThreads);  // ThreadPool(1) is like AsyncWorker

  sqlite3_config(SQLITE_CONFIG_MULTITHREAD);  // should be OK since our DB object is declared thread_local
  if(worldDB.open(worldDBPath, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) != SQLITE_OK) {
    LOG("Error opening world mbtiles %s\n", worldDBPath);
    return -1;
  }
  worldDB.exec(schemaSQL);

  if(topTile.isValid()) {
    auto buildFn = [&](auto&& self, TileID id){
      if(!worldDB.db) {
        if(worldDB.open(worldDBPath, SQLITE_OPEN_READWRITE) != SQLITE_OK) {
          LOG("Error opening DB on tile builder thread!");
          return;
        }
        worldDB.putTile = worldDB.stmt(putTileSQL);
      }
      LOG("Building %s", id.toString().c_str());
      std::string mvt = buildTile(worldGOL, oceanGOL, id);
      if(!mvt.empty()) {
        worldDB.putTile.bind(id.z, id.x, id.yTMS());
        sqlite3_bind_blob(worldDB.putTile.stmt, 4, mvt.data(), mvt.size(), SQLITE_STATIC);
        if(!worldDB.putTile.exec())
          LOG("Error adding tile %s to DB: %s", id.toString().c_str(), worldDB.errMsg());
      }
      if(id.z < maxZ) {
        for(int ii = 0; ii < 4; ++ii)
          self(self, id.getChild(ii, maxZ));
      }
    };
    buildFn(buildFn, topTile);
    buildWorkers.waitForIdle();
    LOG("Finished building tiles");
    return 0;
  }

  httplib::Server svr;  //httplib::SSLServer svr;
  auto time0 = std::chrono::steady_clock::now();

  svr.Get("/status", [&](const httplib::Request& req, httplib::Response& res) {
    auto t1 = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(t1 - time0).count();
    auto statstr = std::format("Uptime: {:.0f} s\nReqs: {}\n200 Reqs: {}\nBytes out: {}\n",
        dt, stats.reqs.load(), stats.reqsok.load(), stats.bytesout.load());
    res.set_content(statstr, "text/plain");
    return httplib::StatusCode::OK_200;
  });

  svr.Get("/tiles/:z/:x/:y", [&](const httplib::Request& req, httplib::Response& res) {
    LOGD("Request %s\n", req.path.c_str());
    ++stats.reqs;
    const char* zstr = req.path_params.at("z").c_str();
    const char* xstr = req.path_params.at("x").c_str();
    const char* ystr = req.path_params.at("y").c_str();
    char* zout, *xout, *yout;
    int z = std::strtol(zstr, &zout, 10);
    int x = std::strtol(xstr, &xout, 10);
    int y = std::strtol(ystr, &yout, 10);
    TileID id(x, y, z);
    if(zout == zstr || xout == xstr || ystr == yout || !id.isValid()) {
      return httplib::StatusCode::BadRequest_400;
    }
    if(z > maxZ) { return httplib::StatusCode::NotFound_404; }

    if(!worldDB.db) {
      if(worldDB.open(worldDBPath, SQLITE_OPEN_READONLY) != SQLITE_OK) {
        LOG("Error opening DB on http worker thread!");
        return httplib::StatusCode::InternalServerError_500;
      }
      worldDB.getTile = worldDB.stmt(getTileSQL);
    }

    worldDB.getTile.bind(id.z, id.x, id.yTMS()).exec([&](sqlite3_stmt* stmt){
      const char* blob = (const char*) sqlite3_column_blob(stmt, 0);
      const int length = sqlite3_column_bytes(stmt, 0);
      res.set_content(blob, length, "application/vnd.mapbox-vector-tile");
    });
    // small chance that we could repeat tile build, but don't want to keep mutex locked during DB query
    if(res.body.empty()) {
      std::shared_future<std::string> fut;
      {
        std::lock_guard<std::mutex> lock(buildMutex);
        // check for pending build
        auto it = buildQueue.find(id);
        if(it != buildQueue.end()) {
          fut = it->second;
        }
        else {
          fut = std::shared_future<std::string>(buildWorkers.enqueue([&, id](){
            if(!worldDB.db) {
              if(worldDB.open(worldDBPath, SQLITE_OPEN_READWRITE) != SQLITE_OK) {
                LOG("Error opening DB on tile builder thread!");
                return std::string();
              }
              worldDB.putTile = worldDB.stmt(putTileSQL);
            }
            std::string mvt = buildTile(worldGOL, oceanGOL, id);
            if(!mvt.empty()) {
              worldDB.putTile.bind(id.z, id.x, id.yTMS());
              sqlite3_bind_blob(worldDB.putTile.stmt, 4, mvt.data(), mvt.size(), SQLITE_STATIC);
              if(!worldDB.putTile.exec())
                LOG("Error adding tile %s to DB: %s", id.toString().c_str(), worldDB.errMsg());
            }
            { std::lock_guard<std::mutex> lock(buildMutex);  buildQueue.erase(id); }
            return mvt;
          }));
          buildQueue.emplace(id, fut);
          ++stats.tilesbuilt;
        }
      }
      if(fut.wait_for(std::chrono::seconds(30)) != std::future_status::ready) {
        return httplib::StatusCode::RequestTimeout_408;  // 504 would be more correct
      }
      std::string mvt = fut.get();
      if(mvt.empty()) { return httplib::StatusCode::NotFound_404; }
      res.set_content(mvt.data(), mvt.size(), "application/vnd.mapbox-vector-tile");
    }

    LOGD("Serving %s\n", req.path.c_str());
    ++stats.reqsok;
    stats.bytesout += res.body.size();
    // client can set X-Hide-Encoding header to suppress Content-Encoding: gzip so client's network stack
    //  doesn't unzip tile (only to have it recompressed when saving to client's mbtiles cache)
    if(!req.has_header("X-Hide-Encoding")) {
      res.set_header("Content-Encoding", "gzip");
    }
    return httplib::StatusCode::OK_200;
  });

  LOG("Server listening on %d", tcpPort);
  svr.listen("0.0.0.0", tcpPort);
  return 0;
}
