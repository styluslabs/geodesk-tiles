#pragma once

#include "linalg.h"

namespace linalg
{
    typedef vec<int32_t,2> i32vec2; typedef vec<uint32_t,2> u32vec2; typedef vec<float,2> vec2; typedef vec<double,2> dvec2;
    typedef vec<int32_t,3> i32vec3; typedef vec<uint32_t,3> u32vec3; typedef vec<float,3> vec3; typedef vec<double,3> dvec3;
    typedef vec<int32_t,4> i32vec4; typedef vec<uint32_t,4> u32vec4; typedef vec<float,4> vec4; typedef vec<double,4> dvec4;
}
//namespace glm = linalg;

// cut and paste from Tangram ES tileID.h

#include <cstdint>
#include <string>

/* An immutable identifier for a map tile
 *
 * Contains the x, y, and z indices of a tile in a quad tree; TileIDs are ordered by:
 * 1. z, highest to lowest
 * 2. x, lowest to highest
 * 3. y, lowest to highest
 */

struct TileID {

    int32_t x; // Index from left edge of projection space
    int32_t y; // Index from top edge of projection space
    int8_t  z; // Data zoom
    int8_t  s; // Styling zoom

    TileID(int32_t _x, int32_t _y, int32_t _z, int32_t _s) : x(_x), y(_y), z(_z), s(_s) {}

    TileID(int32_t _x, int32_t _y, int32_t _z) : TileID(_x, _y, _z, _z) {}

    TileID(const TileID& _rhs) = default;

    bool operator< (const TileID& _rhs) const {
        return s > _rhs.s || (s == _rhs.s && (z > _rhs.z || (z == _rhs.z && (x < _rhs.x || (x == _rhs.x && (y < _rhs.y))))));
    }
    bool operator> (const TileID& _rhs) const { return _rhs < const_cast<TileID&>(*this); }
    bool operator<=(const TileID& _rhs) const { return !(*this > _rhs); }
    bool operator>=(const TileID& _rhs) const { return !(*this < _rhs); }
    bool operator==(const TileID& _rhs) const { return x == _rhs.x && y == _rhs.y && z == _rhs.z && s == _rhs.s; }
    bool operator!=(const TileID& _rhs) const { return !(*this == _rhs); }

    int32_t yTMS() const { return (1 << z) - 1 - y; }

    bool isValid() const {
        int max = 1 << z;
        return x >= 0 && x < max && y >= 0 && y < max && z >= 0;
    }

    bool isValid(int _maxZoom) const {
        return isValid() && z <= _maxZoom;
    }

    TileID withMaxSourceZoom(int32_t _maxZoom) const {
        if (z <= _maxZoom) { return *this; }
        int32_t over = z - _maxZoom;
        return TileID(x >> over, y >> over, _maxZoom, s);
    }

    TileID getParent(int32_t _zoomBias = 0) const {
        if (s > (z + _zoomBias)) {
            // Over-zoomed, keep the same data coordinates
            return TileID(x, y, z, s - 1);
        }
        return TileID(x >> 1, y >> 1, z - 1, s - 1);
    }

    TileID getChild(int32_t _index, int32_t _maxZoom) const {
        if (_index > 3 || _index < 0) {
            return TileID(-1, -1, -1, -1);
        }
        int i = _index / 2;
        int j = _index % 2;
        // _index: 0, 1, 2, 3
        // i:      0, 0, 1, 1
        // j:      0, 1, 0, 1
        auto childID = TileID((x<<1)+i, (y<<1)+j, z + 1, s + 1);
        return childID.withMaxSourceZoom(_maxZoom);
    }

    std::string toString() const {
        return std::to_string(x) + "/" + std::to_string(y) + "/" + std::to_string(z) + "/" + std::to_string(s);
    }
};

static const TileID NOT_A_TILE(-1, -1, -1, -1);

// cut and paste from Tangram ES types.h

struct LngLat {
    LngLat() {}
    LngLat(double _lng, double _lat) : longitude(_lng), latitude(_lat) {}

    LngLat(const LngLat& _other) = default;
    LngLat(LngLat&& _other) = default;
    LngLat& operator=(const LngLat& _other) = default;
    LngLat& operator=(LngLat&& _other) = default;

    bool operator==(const LngLat& _other) const {
        return longitude == _other.longitude &&
               latitude == _other.latitude;
    }
    bool operator!=(const LngLat& _other) const { return !operator==(_other); }

    // Get a LngLat with an equivalent longitude within the range (-180, 180].
    LngLat wrapped() const {
        return LngLat(wrapLongitude(longitude), latitude);
    }

    // Get an equivalent longitude within the range (-180, 180].
    static double wrapLongitude(double longitude) {
        if (longitude > 180.0) {
            return longitude - (int)((longitude + 180.0) / 360.0) * 360.0;
        } else if (longitude <= -180.0) {
            return longitude - (int)((longitude - 180.0) / 360.0) * 360.0;
        }
        return longitude;
    }

    double longitude = 0.0;
    double latitude = 0.0;
};

// cut and paste from Tangram ES MapProjection

using ProjectedMeters = linalg::dvec2;

struct MapProjection
{
    constexpr static double PI = 3.14159265358979323846;
    constexpr static double EARTH_RADIUS_METERS = 6378137.0;
    constexpr static double EARTH_HALF_CIRCUMFERENCE_METERS = PI * EARTH_RADIUS_METERS;
    constexpr static double EARTH_CIRCUMFERENCE_METERS = 2 * PI * EARTH_RADIUS_METERS;

    static double metersPerTileAtZoom(int zoom) {
        return EARTH_CIRCUMFERENCE_METERS / (1 << zoom);
    }

    static LngLat projectedMetersToLngLat(ProjectedMeters meters) {
        LngLat lngLat;
        lngLat.longitude = meters.x * 180.0 / EARTH_HALF_CIRCUMFERENCE_METERS;
        lngLat.latitude = (2.0 * atan(exp(meters.y / EARTH_RADIUS_METERS)) - PI * 0.5) * 180 / PI;
        return lngLat;
    }

    static ProjectedMeters tileCoordinatesToProjectedMeters(double x, double y, int z) {
        double metersPerTile = metersPerTileAtZoom(z);
        return { x * metersPerTile - EARTH_HALF_CIRCUMFERENCE_METERS,
                 EARTH_HALF_CIRCUMFERENCE_METERS - y * metersPerTile };
    }

    static ProjectedMeters tileSouthWestCorner(TileID tile) {
        return tileCoordinatesToProjectedMeters(tile.x, tile.y + 1, tile.z);
    }

    static ProjectedMeters tileCenter(TileID tile) {
        return tileCoordinatesToProjectedMeters(tile.x + 0.5, tile.y + 0.5, tile.z);
    }
};
