# ogr2osm translation script for converting water polygons shapefile to OSM PBF for geodesk import

import ogr2osm

class AllAreas(ogr2osm.TranslationBase):
    def filter_tags(self, attrs):
        return {'area':'yes'}
