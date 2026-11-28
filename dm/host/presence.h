// Discord presence (presence.c): the game's place, Link's age and hearts and the file's play time on the player's Discord profile, while Discord runs.
#pragma once
// after the modules are loaded; dir is the modules folder, which may hold the place names (places.tsv, dm/tools/place_names.py)
void dm_presence_start(const char *dir);
