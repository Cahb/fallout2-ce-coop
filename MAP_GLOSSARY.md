# Fallout 2 `.map` Filename Glossary

Quick-glance decoder for the cryptic 8.3 `.map` names, e.g. `klatoxcv.map` = **Kla**math **Tox**ic **C**a**v**es.

**How this was built (authoritative, not guessed):** the 155 `.map` files are not loose on disk — they live inside `FO2/master.dat`. The filename list was extracted from that DAT, and every human-readable name comes from the game's own **`data/maps.txt`** config (also inside the DAT), which maps each internal `map_name` to a `lookup_name` (the string the engine/automap shows). Only a handful of files have no `maps.txt` entry (alt/dev/return-state variants); those are marked and described from Fallout 2 knowledge.

**Naming convention:** most names are `<location-prefix><purpose/area-suffix>`. The location prefix ties the map to a worldmap city/area (see the abbreviation key at the bottom); the suffix names the specific area, level, or purpose (entrance, tunnels, downtown, vault, cave level, etc.). "Random"/"Special" rows are worldmap random-encounter tiles, not fixed towns.

Total: **155 maps** documented.

| file | location | description |
|------|----------|-------------|
| `arbridge.map` | Arroyo | Arroyo Bridge |
| `arcaves.map` | Arroyo | Arroyo Caves |
| `ardead.map` | Arroyo | Destroyed Arroyo Bridge |
| `argarden.map` | Arroyo | Arroyo Wilderness |
| `artemple.map` | Arroyo | Arroyo Temple |
| `arvill2.map` | Arroyo | Arroyo Village - alternate/return version |
| `arvillag.map` | Arroyo | Arroyo Village |
| `bhrnddst.map` | Broken Hills | Broken Hills Desert 1 |
| `bhrndmtn.map` | Broken Hills | Broken Hills Mountain 1 |
| `broken1.map` | Broken Hills | Broken Hills 1 |
| `broken2.map` | Broken Hills | Broken Hills 2 |
| `cardesrt.map` | Car | Car: Desert |
| `cave0.map` | Cave (random) | Cavern Encounter 0 |
| `cave1.map` | Cave (random) | Cavern Encounter 1 |
| `cave2.map` | Cave (random) | Cavern Encounter 2 |
| `cave3.map` | Cave (random) | Cavern Encounter 3 |
| `cave4.map` | Cave (random) | Cavern Encounter 4 |
| `cave5.map` | Cave (random) | Cavern Encounter 5 |
| `cave6.map` | Cave (random) | Cavern Encounter 6 |
| `cave7.map` | Cave (random) | Cavern Encounter 7 |
| `city1.map` | City (random) | City Encounter 1 |
| `city2.map` | City (random) | City Encounter 2 |
| `city3.map` | City (random) | City Encounter 3 |
| `city4.map` | City (random) | City Encounter 4 |
| `city5.map` | City (random) | City Encounter 5 |
| `city6.map` | City (random) | City Encounter 6 |
| `city7.map` | City (random) | City Encounter 7 |
| `city8.map` | City (random) | City Encounter 8 |
| `coast1.map` | Coast (random) | Coast Encounter 1 |
| `coast10.map` | Coast (random) | Coast Encounter 10 |
| `coast11.map` | Coast (random) | Coast Encounter 11 |
| `coast12.map` | Coast (random) | Coast Encounter 12 (maps.txt map_name typo "07desert") |
| `coast2.map` | Coast (random) | Coast Encounter 2 |
| `coast3.map` | Coast (random) | Coast Encounter 3 |
| `coast4.map` | Coast (random) | Coast Encounter 4 |
| `coast5.map` | Coast (random) | Coast Encounter 5 |
| `coast6.map` | Coast (random) | Coast Encounter 6 |
| `coast7.map` | Coast (random) | Coast Encounter 7 |
| `coast8.map` | Coast (random) | Coast Encounter 8 |
| `coast9.map` | Coast (random) | Coast Encounter 9 |
| `cowbomb.map` | Special enc. | "Cow bomb" catapult special encounter (Monty Python gag) |
| `denbus1.map` | The Den | Den Business 1 |
| `denbus2.map` | The Den | Den Business 2 |
| `denres1.map` | The Den | Den Residential 1 |
| `depolv1.map` | Sierra Army Depot | Sierra Army Depot: The Battlefield |
| `depolva.map` | Sierra Army Depot | Sierra Army Depot: Levels 1-3 |
| `depolvb.map` | Sierra Army Depot | Sierra Army Depot: Level 4 |
| `desert1.map` | Desert (random) | Desert Encounter 1 |
| `desert2.map` | Desert (random) | Desert Encounter 2 |
| `desert3.map` | Desert (random) | Desert Encounter 3 |
| `desert4.map` | Desert (random) | Desert Encounter 4 |
| `desert5.map` | Desert (random) | Desert Encounter 5 |
| `desert6.map` | Desert (random) | Desert Encounter 6 |
| `desert7.map` | Desert (random) | Desert Encounter 7 |
| `desert8.map` | Desert (random) | Desert Encounter 8 |
| `desert9.map` | Desert (random) | Desert Encounter 9 |
| `desrt10.map` | Desert (random) | Desert Encounter 10 |
| `desrt11.map` | Desert (random) | Desert Encounter 11 |
| `desrt12.map` | Desert (random) | Desert Encounter 12 |
| `desrt13.map` | Desert (random) | Desert Encounter 13 |
| `dnslvrun.map` | The Den | Den Slave Run |
| `encdet.map` | Enclave | Enclave Detention |
| `encdock.map` | Enclave | Enclave Dock |
| `encfite.map` | Enclave | Enclave End Fight |
| `encgd.map` | Enclave | Enclave Guard Barracks |
| `encpres.map` | Enclave | Enclave Presidential |
| `encrctr.map` | Enclave | Enclave Reactor |
| `enctrp.map` | Enclave | Enclave Trap Room |
| `gammovie.map` | (cutscene) | In Game Movie Map 1 |
| `geckjunk.map` | Gecko | Gecko Junkyard |
| `geckpwpl.map` | Gecko | Gecko Power Plant |
| `gecksetl.map` | Gecko | Gecko Settlement |
| `gecktunl.map` | Gecko | Gecko Access Tunnels |
| `gstcav1.map` | Ghost Farm (Slags) | Ghost Town: Main Cavern |
| `gstcav2.map` | Ghost Farm (Slags) | Ghost Town: Underground Lake |
| `gstfarm.map` | Ghost Farm (Slags) | Ghost Town: The Ghost Farm |
| `klacanyn.map` | Klamath | Klamath Canyon |
| `kladwtwn.map` | Klamath | Klamath Downtown |
| `klagraz.map` | Klamath | Klamath Grazing Area |
| `klamall.map` | Klamath | Klamath Mall |
| `klaratcv.map` | Klamath | Klamath Rat Caves |
| `klatoxcv.map` | Klamath | Klamath Toxic Caves |
| `klatrap.map` | Klamath | Klamath Trapping Caves |
| `mbase12.map` | Military Base | Military Base Levels 1-2 |
| `mbase34.map` | Military Base | Military Base Levels 3-4 |
| `mbclose.map` | Military Base | Military Base Entrance |
| `modbrah.map` | Modoc | Modoc: Grisham's Brahmin Pastures |
| `modgard.map` | Modoc | Modoc: Farrel's Garden |
| `modinn.map` | Modoc | Modoc Bed And Breakfast |
| `modmain.map` | Modoc | Modoc Main Street |
| `modshit.map` | Modoc | Modoc: Down the Shitter |
| `modwell.map` | Modoc | Modoc: Town Hall |
| `mountn1.map` | Mountain (random) | Mountain Encounter 1 |
| `mountn2.map` | Mountain (random) | Mountain Encounter 2 |
| `mountn3.map` | Mountain (random) | Mountain Encounter 3 |
| `mountn4.map` | Mountain (random) | Mountain Encounter 4 |
| `mountn5.map` | Mountain (random) | Mountain Encounter 5 |
| `mountn6.map` | Mountain (random) | Mountain Encounter 6 |
| `navarro.map` | Navarro | Navarro Entrance |
| `ncr1.map` | NCR | NCR: Downtown |
| `ncr2.map` | NCR | NCR: Council 1 |
| `ncr3.map` | NCR | NCR: Westin Ranch |
| `ncr4.map` | NCR | NCR: Grazing Lands |
| `ncrent.map` | NCR | NCR: Bazaar |
| `newr1.map` | New Reno | New Reno 1 |
| `newr1a.map` | New Reno | New Reno 1 - upper floors / alt section |
| `newr2.map` | New Reno | New Reno 2 |
| `newr2a.map` | New Reno | New Reno 2 - upper floors / alt section |
| `newr3.map` | New Reno | New Reno 3 |
| `newr4.map` | New Reno | New Reno 4 |
| `newrba.map` | New Reno | New Reno Boxing Arena |
| `newrcs.map` | New Reno | New Reno Chop Shop |
| `newrgo.map` | New Reno | New Reno Golgatha |
| `newrst.map` | New Reno | New Reno Stables |
| `newrvb.map` | New Reno | New Reno VB |
| `raiders1.map` | Raiders Camp | Raiders Camp 1 |
| `raiders2.map` | Raiders Camp | Raiders Camp 2 |
| `reddown.map` | Redding | Redding Downtown |
| `reddtun.map` | Redding | Redding Downtown Tunnels |
| `redment.map` | Redding | Redding Mine Entrance |
| `redmtun.map` | Redding | Redding Mine Tunnels |
| `redwame.map` | Redding | Wanamingo Mine Entrance |
| `redwan1.map` | Redding | Wanamingo Mine Level 12 |
| `rndbess.map` | Special enc. | Bess Dead |
| `rndbhead.map` | Special enc. | Special Head Encounter |
| `rndbridg.map` | Special enc. | Special Bridge Encounter |
| `rndcafe.map` | Special enc. | Special Cafe Encounter |
| `rndexcow.map` | Special enc. | Special Mad Brahmin Encounter |
| `rndforvr.map` | Special enc. | Special Guardian Encounter |
| `rndholy1.map` | Special enc. | Special Holy Encounter 1 |
| `rndholy2.map` | Special enc. | Special Holy Encounter 2 |
| `rndparih.map` | Special enc. | Special Pariahs Encounter |
| `rndshutl.map` | Special enc. | Special Shuttle Encounter |
| `rndtinwd.map` | Special enc. | Special Woodsman Encounter |
| `rndtoxic.map` | Special enc. | Special Toxic Encounter |
| `rnduvilg.map` | Special enc. | Special Unwashed Encounter |
| `rndwhale.map` | Special enc. | Special Whale Encounter |
| `sfchina.map` | San Francisco | San Fran China |
| `sfchina2.map` | San Francisco | Shi Temple |
| `sfdock.map` | San Francisco | San Fran Dock |
| `sfelronb.map` | San Francisco | Elronologist Base |
| `sfshutl1.map` | San Francisco | Shuttle Outside |
| `sfshutl2.map` | San Francisco | Shuttle Interior |
| `sftanker.map` | San Francisco | San Fran Tanker |
| `v13_orig.map` | Vault 13 | Vault 13 - original/pristine state |
| `v13ent.map` | Vault 13 | Vault 13 Entrance |
| `v15_orig.map` | Vault 15 | Vault 15 - original/pristine state |
| `v15ent.map` | Vault 15 | The Squat A |
| `v15sent.map` | Vault 15 | Vault 15 East Entrance |
| `vault13.map` | Vault 13 | Vault 13 |
| `vault15.map` | Vault 15 | Vault 15 |
| `vctycocl.map` | Vault City | Vault City Council |
| `vctyctyd.map` | Vault City | Vault City Courtyard |
| `vctydwtn.map` | Vault City | Vault City Downtown |
| `vctyvlt.map` | Vault City | Vault City Vault |
## Notes on families

- **Random-encounter tiles** (not real towns): `cave0-7`, `city1-8`, `coast1-12`, `desert1-9` + `desrt10-13`, `mountn1-6`. These are generic terrain maps the worldmap drops you into; they still "belong" to a nearby region for music/ambient purposes.
- **Special encounters** (`rnd*` + `cowbomb`): scripted one-off gag/lore encounters (whale, talking head, Guardian of Forever, Monty Python cow catapult, etc.).
- **`*_orig` / `arvill2` / `newr?a`:** alternate copies of a town map for a different game state (pristine vaults before you enter, Arroyo on return, New Reno upper floors). Script/engine driven — they do not appear as worldmap entries in `worldmap.txt`.
- **Data quirk:** `coast12.map`'s `maps.txt` block has a wrong `map_name` field (`07desert`) but its `lookup_name` is "Coast Encounter 12". The file is still `coast12.map`.

## Abbreviation key (location prefixes)

| prefix | location |
|--------|----------|
| `ar` | Arroyo (starting village) |
| `bhrnd`, `broken` | Broken Hills |
| `car` | Car (special "your vehicle" map) |
| `cave` | Cave random encounter |
| `city` | City random encounter |
| `coast` | Coast random encounter |
| `den`, `dnslv` | The Den |
| `depol` | Sierra Army Depot (Sierra **Depo**t, "lv"=levels) |
| `desert`, `desrt` | Desert random encounter |
| `enc` | Enclave (oil rig complex) |
| `gam` | In-game cutscene/movie map |
| `geck` | Gecko |
| `gst` | Ghost Farm / Ghost Town (the Slags, near Modoc) |
| `kla` | Klamath |
| `mbase`, `mbclose` | Mariposa Military Base |
| `mod` | Modoc |
| `mountn` | Mountain random encounter |
| `navarro` | Navarro (Enclave base) |
| `ncr` | New California Republic |
| `newr` | New Reno |
| `raiders` | Raiders (New Reno) camp |
| `red` | Redding (incl. `redwa*` = Wanamingo Mine) |
| `rnd` | Random/special scripted encounter |
| `sf` | San Francisco |
| `v13`, `vault13` | Vault 13 |
| `v15`, `vault15` | Vault 15 |
| `vcty` | Vault City |

### Suffix hints

`ent`/`close` = entrance · `dwtn`/`down` = downtown · `tun`/`tunl`/`caves`/`cav` = tunnels/caves · `res` = residential · `bus` = business district · `vlt` = vault interior · `12`/`34`/`va`/`vb` = which level(s) · `_orig` = pristine/original-state copy.
