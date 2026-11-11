// Where the program keeps its files. The test builds: /dev_hdd0/tmp/conkerps3 (reachable over FTP, with
// the logs). The release build (CONKER_RELEASE): /dev_hdd0/data/conker, outside the installed program's
// folder, so the saves, the settings and the copy of the ROM stay when the game is deleted or updated (as
// the Pikmin PS3 port keeps its memory card in /dev_hdd0/data/pikmin).
#pragma once

#if defined(CONKER_RELEASE)
#define CONKER_GAME_DIR "/dev_hdd0/game/" CONKER_APPID
#define CONKER_DATA_DIR "/dev_hdd0/data/conker"
#else
#define CONKER_DATA_DIR "/dev_hdd0/tmp/conkerps3"
#endif
// The test builds' folder: the release build brings the ROM and saves over from it on its first start.
#define CONKER_TEST_DIR "/dev_hdd0/tmp/conkerps3"
