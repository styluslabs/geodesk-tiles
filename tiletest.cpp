#include "tilebuilder.h"

extern std::string buildTile(const Features& world, const Features& ocean, TileID id);

int main(int argc, char* argv[])
{
  if(argc < 3) {
    LOG("No gol file specified!");
    return -1;
  }

  Features world(argv[1]);
  Features ocean(argv[2]);
  LOG("Loaded %s and %s", argv[1], argv[2]);

  TileBuilder::worldFeats = &world;

  // for(int x = 2616; x <= 2621; ++x) {
  //   for(int y = 6331; y <= 6336; ++y) {
  //     TileID id(x, y, 14);
  //     std::string mvt = buildTile(world, id);
  //   }
  // }

  {
    TileID id(662, 1587, 12);
    std::string mvt = buildTile(world, ocean, id);
    return 0;
  }

  {
    //TileID id(11912, 6865, 14);
    //TileID id(2617, 6332, 14);  // Alamo square!
    TileID id(2641, 6335, 14);
    while(id.z > 9) {
      std::string mvt = buildTile(world, ocean, id);
      id = id.getParent();
    }
    //std::string mvt = buildTile(world, ocean, id);
    return 0;
  }
  {
    TileID id(2615, 6329, 14);
    std::string mvt = buildTile(world, ocean, id);
  }
  {
    TileID id(2612, 6327, 14);  // missing islands
    std::string mvt = buildTile(world, ocean, id);
  }
  {
    TileID id(2609, 6334, 14);  // all ocean
    std::string mvt = buildTile(world, ocean, id);
  }

  // while(id.z > 9) {
  //   std::string mvt = buildTile(world, id);
  //   id = id.getParent();
  // }

  return 0;
}
