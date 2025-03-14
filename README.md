# geodesk-tiles #

Combine [libgeodesk](https://github.com/clarisma/libgeodesk) and [vtzero](https://github.com/mapbox/vtzero) to build OpenStreetMap (OSM) vector tiles on demand from a [GeoDesk](https://www.geodesk.com/) Geographic Object Library (GOL).


## Usage ##

On Linux, `git clone https://github.com/styluslabs/geodesk-tiles`, `git submodule update --init`, then `make` to create `build/Release/server` (compiler with C++20 support required), then run `server <OSM GOL file> <ocean polygons GOL file>`  to provide a standard XYZ (WMTS) tile server at `http://localhost:8080/tiles/{z}/{x}/{y}`.  The TCP port can be set with the `--port` option or `iptables` can be used to redirect port 80 to 8080 (so server needs no special permissions).

The GOL files can be created from OSM pbf files with the GeoDesk [GOL utility](https://docs.geodesk.com/gol/build).

To create a pbf for ocean polygons, simplified water polygons from https://osmdata.openstreetmap.de/data/water-polygons.html can be converted with [ogr2osm](https://github.com/roelderickx/ogr2osm).  These are used for determining whether tiles without any coastline ways are ocean or land.  A prebuilt oceans GOL file is available in [releases](https://github.com/styluslabs/geodesk-tiles/releases/tag/tag-for-assets).

An initial mbtiles file such as [this](https://github.com/styluslabs/maps/releases/download/alpha-1/basemap7.mbtiles) with low zoom tiles can be provided with the `--db` option to avoid having to generate these (which is not tested).


## Schema ##

The tile schema is defined by extending the `TileBuilder` class and implementing `processFeature()`, which is called for each feature intersecting the tile (as returned by a GeoDesk query).  The `TileBuilder` class provides an API based on the Lua API used by [tilemaker](https://github.com/systemed/tilemaker/).

Call `Find(<tag>)` to read feature tags.  The returned `geodesk::TagValue` object is convertible to `bool` (for existence testing), `double` (or `int`), and `std::string`.  `Id()` returns the OSM id as a string, and `IsClosed()`, `Length()` (meters), and `Area()` (sq meters) return additional feature information.  See `AscendTileBuilder::WriteBoundary()` for how to iterate over relation members (to be cleaned up).

`MinZoom(z)` returns `<current tile z> >= z` for use in determining if the current feature should be added to the tile.

Call `Layer(<layer name>)` or `LayerAsCentroid(<layer name>)` to add the current feature (or its centroid as a single point) to the tile, then call `Attribute(<key>, <value>)` and `AttributeNumeric(<key>, <value>)` to add attributes.

Coastline ways (`natural=coastline`) should be passed to `addCoastline()` instead of being added to tile.  After all features are processed, ocean polygons will be generated and `processFeature()` called with no feature set - in this case it should call `Layer()` and `Attribute()` as appropriate for ocean features.

[ascendtiles.cpp](ascendtiles.cpp) (a direct translation of the tilemaker script [process.lua](https://github.com/styluslabs/maps/blob/master/scripts/tilemaker/process.lua)) implements the [Ascend Maps](https://github.com/styluslabs/maps/) schema, which mostly just uses unmodified OSM tags for feature attributes.  Adapting for other schemas should be relatively straightforward.

Rebuilding (i.e., running `make`) after schema changes should only take a few seconds.


## Details ##

At the moment, it is necessary to use a forked libgeodesk with [one small change](https://github.com/clarisma/libgeodesk/pull/6) which hopefully can be merged upstream.

[server.cpp](server.cpp) uses [cpp-httplib](https://github.com/yhirose/cpp-httplib) to provide an HTTP server and sqlite to save generated tiles to an mbtiles file.  Tile builder threads (number set by `--threads` option, defaulting to number of CPU cores minus one) share a queue to prevent duplicate work.

Simple Cohen–Sutherland clipping is used in [clipper.h](clipper.h); more robust clipping for edge cases should be added in the future.

For simplicity, every tile is built independently - processed geometry and attributes are not reused for building parents or children of tile.  Also, geometry with identical attributes is not currently combined in tile.


## Performance ##

Results for dense urban area (San Francisco): time to process tile is ~1x to ~3x time to gzip tile (miniz level 5).

    Tile 2617/6332/14/14 (243209 bytes) built in 43.5 ms (22.3 ms process 8486/10451 features w/ 116260 points, 21.2 ms gzip 489316 bytes)
    Tile 1308/3166/13/13 (50387 bytes) built in 17.8 ms (14.5 ms process 2856/30380 features w/ 10158 points, 3.3 ms gzip 105501 bytes)
    Tile 654/1583/12/12 (208908 bytes) built in 76.2 ms (56.4 ms process 14587/151092 features w/ 41012 points, 19.8 ms gzip 484240 bytes)
    Tile 327/791/11/11 (203717 bytes) built in 93.0 ms (69.6 ms process 17917/306543 features w/ 58383 points, 23.5 ms gzip 494058 bytes)
    Tile 163/395/10/10 (215077 bytes) built in 91.4 ms (72.1 ms process 19622/419999 features w/ 68507 points, 19.3 ms gzip 531326 bytes)
