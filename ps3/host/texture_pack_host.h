// The PS3 side of texture packs (ps3/gfx/texture_pack.h): a GLideN64 texture cache put in the data folder
// as a .htc (scripts/mac "5 - Send texture pack to PS3") is unpacked once into texture_pack.pack there,
// with a progress screen (a minute or two for a 40 MB cache: 230 MB written), and then read as textures
// are first drawn. texture_pack.txt records which .htc the pack file was made from (its name and size): a
// different one is unpacked again. Without the .htc (deleted to save room), the pack file is used as it is.
#pragma once
#include "../gfx/texture_pack.h"

namespace texpack {

// Whether there's a pack (a .htc or an unpacked pack file): before the menu, which offers it then.
bool find();
// After the menu, with the setting on: unpacks if needed and opens the pack.
void prepare();
// The pack in use, or null.
n64gfx::TexturePack* active();

}  // namespace texpack
