# Fallout 2 Item Proto (PID) Glossary

Quick-glance decoder for item **prototype IDs** — the numbers that appear in code, on the
wire, and in save files whenever the engine refers to "what an object IS". The map analog of
[MAP_GLOSSARY.md](MAP_GLOSSARY.md), but for items instead of `.map` files.

**How this was built (authoritative, not guessed):** item protos are not loose on disk — they
live inside `FO2/master.dat`. Three files were extracted from that DAT and cross-checked:

- **`proto/items/items.lst`** — the ordered proto list. A proto's **1-based line number is its
  PID index**. All 531 item protos carry object-type byte `0x00` (ITEM), so the full 32-bit
  **PID equals that line index** (e.g. line 41 → PID `41` → 10mm Pistol).
- **each `NNNNNNNN.pro` header** — read big-endian for the authoritative PID (offset `0x00`),
  item-type subcode (offset `0x20`), and text id (offset `0x04`). Verified: PID low-bits ==
  line index and text id == `index*100` for **all 531** protos, with zero mismatches.
- **`data/text/english/game/pro_item.msg`** — the English strings. For PID `N`, the **name** is
  message `{N*100}` and the **description** is message `{N*100+1}`.

Note the `.pro` filename number is *not* the PID (the list is reordered): use the **PID** column,
which is what code/wire/saves actually carry. Item **type** subcodes: `0` Armor, `1` Container,
`2` Drug, `3` Weapon, `4` Ammo, `5` Misc, `6` Key.

Total: **531 item protos** documented (110 weapons, 25 ammo, 16 armor, 28 drugs, 115 containers, 8 keys, 229 misc).

## Weapon (110)

| PID | `.pro` | name | description |
|----:|--------|------|-------------|
| 4 | `00000012.pro` | Knife | A sharp-bladed cutting and stabbing weapon. Min ST: 2. |
| 5 | `00000009.pro` | Club | A military or police baton. Heavy wood. Min ST: 3. |
| 6 | `00000005.pro` | Sledgehammer | A large hammer with big handle. Very popular with the muscular crowd. Min ST: 6. |
| 7 | `00000013.pro` | Spear | A razor tipped polearm. The shaft is wooden, and the tip is worked steel. Min ST: 4. |
| 8 | `00000004.pro` | 10mm Pistol | A Colt 6520 10mm autoloading pistol. Each pull of the trigger will automatically reload the firearm until the magazine is empty. Single shot only, using the powerful 10mm round. Min ST: 3. |
| 9 | `00000002.pro` | 10mm SMG | H&K MP9 Submachinegun (10mm variant). A medium-sized SMG, capable of single shot and burst mode. Min ST: 4. |
| 10 | `00000011.pro` | Hunting Rifle | A Colt Rangemaster semi-automatic rifle, in .223 caliber. Single-shot only. Min ST: 5. |
| 11 | `00000010.pro` | Flamer | A Flambe 450 model flamethrower, varmiter variation. Fires a short spray of extremely hot, flamable liquid. Requires specialized fuel to work properly. Min ST: 6. |
| 12 | `00000006.pro` | Minigun | A Rockwell CZ53 Personal Minigun. A multi-barrelled chaingun firing 5mm ammunition at over 60,000 RPM. Min ST: 7. |
| 13 | `00000007.pro` | Rocket Launcher | A Rockwell BigBazooka rocket launcher. With the deluxe 3 lb. trigger. Fires AP or Explosive Rockets. Min ST: 6. |
| 15 | `00000015.pro` | Plasma Rifle | A Winchester Model P94 Plasma Rifle. An industrial-grade energy weapon, firing superheated bolts of plasma down a superconducting barrel. Powered by Micro Fusion Cells. Min ST: 6. |
| 16 | `00000016.pro` | Laser Pistol | A Wattz 1000 Laser Pistol. Civilian model, so the wattage is lower than military or police versions. Uses small energy cells. Min ST: 3. |
| 18 | `00000018.pro` | Desert Eagle .44 | An ancient Desert Eagle pistol, in .44 magnum. Interest in late 20th century films made this one of the most popular handguns of all times. Min ST: 4. |
| 19 | `00000019.pro` | Rock | It's a rock. The Granite-Inc. model is an upgraded version. Min ST: 1. |
| 20 | `00000020.pro` | Crowbar | A very solid and heavy piece of metal, specially designed to exert leverage. Or pound heads. Min ST: 5. |
| 21 | `00000021.pro` | Brass Knuckles | Hardened knuckle grip that is actually made out of steel. They protect your hand, and do more damage, in unarmed combat. Min ST: 1. |
| 22 | `00000022.pro` | 14mm Pistol | A Sig-Sauer 14mm Auto Pistol. Large, single shot handgun. Excellent craftmanship. Min ST: 4. |
| 23 | `00000023.pro` | Assault Rifle | An AK-112 5mm Assault Rifle. An old military model, out of use around the time of the war. Can fire single-shot or burst, using the high velocity 5mm rounds. Min ST: 5. |
| 24 | `00000024.pro` | Plasma Pistol | Glock 86 Plasma Pistol. Designed by the Gaston Glock AI. Shoots a small bolt of superheated plasma. Powered by a small energy cell. Min ST: 4. |
| 25 | `00000025.pro` | Grenade (Frag) | A generic fragmentation grenade. Contains a small amount of high explosives, the container itself forming most of the damaging fragments. Explodes on contact. Min ST: 3. |
| 26 | `00000026.pro` | Grenade (Plasma) | A magnetically sealed plasma delivery unit, with detonating explosives. Creates a blast of superheated plasma on contact. Min ST: 4. |
| 27 | `00000027.pro` | Grenade (Pulse) | An electromagnetic pulse grenade, generating an intense magnetic field on detonation. Doesn't affect biological creatures. Contact fuze. Min ST: 4. |
| 28 | `00000028.pro` | Gatling Laser | An H&K L30 Gatling Laser. Designed specifically for military use, these were in the prototype stage at the beginning of the War. Multiple barrels allow longer firing before overheating. Powered by Micro Fusion Cells. Min ST: 6. |
| 45 | `00000045.pro` | Throwing Knife | A knife, balanced specifically for throwing. Made of titanium, and laser sharpened. Min ST: 3. |
| 79 | `00000079.pro` | Flare | A flare. Creates light for a short period of time. The paper is a little worn, but otherwise it is in good condition. Twist the top to activate it. |
| 94 | `00000094.pro` | Shotgun | A Winchester Widowmaker double-barreled 12-gauge shotgun. Short barrel, with mahogany grip. Min ST: 4. |
| 115 | `00000115.pro` | Super Sledge | A Super Sledgehammer, manufactured by the Brotherhood of Steel, using the finest weapons technology available. Includes a kinetic energy storage device, to increase knockback. Min ST: 5. |
| 116 | `00000116.pro` | Ripper | A Ripper(tm) vibroblade. Powered by a small energy cell, the chainblade rips and tears into it's target. Min ST: 4. |
| 118 | `00000118.pro` | Laser Rifle | A Wattz 2000 Laser Rifle. Uses micro fusion cells for more powerful lasers, and an extended barrel for additional range. Min ST: 6. |
| 120 | `00000120.pro` | Alien Blaster | A strange gun of obviously alien origin. Looks like it can support small energy cells, however. Min ST: 2. |
| 122 | `00000122.pro` | 9mm Mauser | A Mauser M/96, in 9x19mm Parabellum. In excellent condition. Extremely accurate. Min ST: 3. |
| 143 | `00000143.pro` | Sniper Rifle | A DKS-501 Sniper Rifle. Excellent long range projectile weapon. Originally .308, this one is chambered in the more common .223 caliber. Min ST: 5. |
| 159 | `00000159.pro` | Molotov Cocktail | A home-made flammable grenade. Min ST: 3. |
| 160 | `00000160.pro` | Cattle Prod | A Farmer's Best Friend model cattle prod from Wattz Electronics. Uses small energy cells for power. Min ST: 4. |
| 161 | `00000161.pro` | Red Ryder BB Gun | A Red Ryder BB gun. The classic. Min ST: 3. |
| 162 | `00000162.pro` | Red Ryder LE BB Gun | A limited edition version of the Red Ryder BB gun. A true classic. Min ST: 3. |
| 205 | `00000205.pro` | Flare | A flare. Creates light for a short period of time. The paper is a little worn, but otherwise it is in good condition. It is lit. |
| 233 | `00000233.pro` | Turbo Plasma Rifle | A modified Winchester P94 plasma rifle. The plasma bolt chamber has been hotwired to accelerate the bolt formation process. Min ST: 6. |
| 234 | `00000234.pro` | Spiked Knuckles | An improved version of the classic Brass Knuckles. The Spiked Knuckles do more damage, tearing into the flesh of your opponent in unarmed combat. Min ST: 1. |
| 235 | `00000235.pro` | Power Fist | A "Big Frigger" Power Fist from BeatCo. Considered by many to be the ultimate weapon to use in unarmed combat. Others are just scared. Powered by small energy cells. Min ST: 1. |
| 236 | `00000236.pro` | Combat Knife | A high-quality combat knife, the Stallona is from SharpWit, Inc. The edge of this blade is guaranteed sharp for over a decade of use! Min ST: 2. |
| 241 | `00000241.pro` | .223 Pistol | A .223 rifle modified and cut down to a pistol. This is a one-of-a-kind firearm, obviously made with love and skill. Min ST: 5. |
| 242 | `00000242.pro` | Combat Shotgun | A Winchester City-Killer 12-gauge combat shotgun, bullpup variant. In excellent condition, it has the optional DesertWarfare environmental sealant modification for extra reliability. Min ST: 5. |
| 261 | `00000261.pro` | Jonny's BB Gun | A Red Ryder BB gun with the name 'Jonny' scratched on the stock. Min ST: 3. |
| 268 | `00000268.pro` | H&K CAWS | The CAWS, short for Close Assault Weapons System, shotgun is a useful tool for close-range combat. The bullpup layout gives the weapon a short, easily handleable, length while still retaining enough barrel length for its high velocity shells. Min ST: 6. |
| 270 | `00000270.pro` | Robo Rocket Launcher | Only used by rocket equipped robots. |
| 280 | `00000280.pro` | Sharpened Spear | A razor tipped polearm. The shaft is wooden, and the tip is sharpened steel. Min ST: 4. |
| 283 | `00000283.pro` | Tommy Gun | This Thompson M1928 submachine gun is a sinister looking weapon; every time you hold it, you have an urge to put on a fedora hat and crack your knuckles. The Thompson is well-fed by a large 50 round drum magazine. Min ST: 6. |
| 287 | `00000287.pro` | Scoped Hunting Rifle | Nothing's better than seeing that surprised look on your target's face. The Loophole x20 Scope on this hunting rifle makes it easier than ever before. Accurate from first shot to last, no matter what kind of game you're gunning for. Min ST: 5. |
| 290 | `00000290.pro` | Robo Melee Weapon 1 | For Floating Eye Robot. Does Electrical Damage. |
| 291 | `00000291.pro` | Robo Melee Weapon 2 | For Floating Eye Robot. Does Electrical damage. |
| 292 | `00000292.pro` | Boxing Gloves | A pair of boxing gloves that smell faintly of sweat. They look like they have seen a lot of use: many old blood stains still remain. |
| 293 | `00000293.pro` | Plated Boxing Gloves | Someone has "accidentally" slipped metal plates into these boxing gloves. It could technically be considered cheating, but you prefer to think of it as an increased opportunity to dispense bone-crunching damage. |
| 296 | `00000296.pro` | HK P90c | The Heckler & Koch P90c was just coming into use at the time of the war. The weapon's bullpup layout, and compact design, make it easy to control. The durable P90c is prized for its reliability, and high firepower in a ruggedly-compact package. Min ST: 4. |
| 299 | `00000299.pro` | Pipe Rifle | This is a hand-made single shot rifle. Min ST: 5. |
| 300 | `00000300.pro` | Zip Gun | A handmade single shot pistol. Min ST: 4. |
| 313 | `00000313.pro` | .44 Magnum Revolver | Being that this is the most powerful handgun in the world, and can blow your head clean-off, you've got to ask yourself one question. Do I feel lucky? Well, do ya punk?. Min ST: 5. |
| 319 | `00000319.pro` | Switchblade | The blade of this small knife is held by a spring. When a button on the handle is pressed, the blades shoots out with a satisfying "Sssssshk" sound. Min ST: 1. |
| 320 | `00000320.pro` | Sharpened Pole | A wood pole sharpened at one end. Min ST: 4. |
| 332 | `00000332.pro` | M3A1 "Grease Gun" SMG | This submachine gun filled National Guard arsenals after the Army replaced it with newer weapons. However, the "Grease Gun" was simple and cheap to manufacture so there are still quite a few still in use. Min ST: 4. |
| 350 | `00000350.pro` | Bozar | The ultimate refinement of the sniper's art. Although, somewhat finicky and prone to jamming if not kept scrupulously clean, the big weapon's accuracy more than makes up for its extra maintenance requirements. Min ST: 6. |
| 351 | `00000351.pro` | FN FAL | This rifle has been more widely used by armed forces than any other rifle in history. It's a reliable assault weapon for any terrain or tactical situation. Min ST: 5. |
| 352 | `00000352.pro` | H&K G11 | This gun revolutionized assault weapon design. The weapon fires a caseless cartridge consisting of a block of propellant with a bullet buried inside. The resultant weight and space savings allow this weapon to have a very high magazine capacity. Min ST: 5. |
| 353 | `00000353.pro` | XL70E3 | This was an experimental weapon at the time of the war. Manufactured, primarily, from high-strength polymers, the weapon is almost indestructible. It's light, fast firing, accurate, and can be broken down without the use of any tools. Min ST: 5. |
| 354 | `00000354.pro` | Pancor Jackhammer | The Jackhammer, despite its name, is an easy to control shotgun, even when fired on full automatic. The popular bullpup design, which places the magazine behind the trigger, makes the weapon well balanced & easy to control. Min ST: 5. |
| 355 | `00000355.pro` | Light Support Weapon | This squad-level support weapon has a bullpup design. The bullpup design makes it difficult to use while lying down. Because of this it was remanded to National Guard units. It, however, earned a reputation as a reliable weapon that packs a lot of punch for its size. Min ST: 6. |
| 365 | `00000365.pro` | Plant Spike | An organic seed-spike that the carnivorous plants spit at potential food or seed carriers. |
| 371 | `00000371.pro` | Claw | Claw |
| 372 | `00000372.pro` | Claw | Claw |
| 383 | `00000383.pro` | Shiv | This home-made knife is as dangerous as it is easily concealed. Its presence can't be detected by others. Min ST: 1. |
| 384 | `00000384.pro` | Wrench | A typical wrench used by mechanics. Min ST: 3. |
| 385 | `00000385.pro` | Sawed-Off Shotgun | Someone took the time to chop the last few inches off the barrel and stock of this shotgun. Now, the wide spread of this hand-cannon's short-barreled shots makes it perfect for short-range crowd control. Min ST: 4. |
| 386 | `00000386.pro` | Louisville Slugger | This all-American, hardwood, baseball bat will knock anything right out of the park. Min ST: 4. |
| 387 | `00000387.pro` | M60 | This relatively light machine gun was prized by militaries around for world for its high rate of fire. This reliable, battlefield-proven design, was used on vehicles and for squad level fire-support. Min ST: 7. |
| 388 | `00000388.pro` | Needler Pistol | You suspect this Bringham needler pistol was once used for scientific field studies. It uses small hard-plastic hypodermic darts as ammo. Min ST: 3. |
| 389 | `00000389.pro` | Avenger Minigun | Rockwell designed the Avenger as the replacement for their aging CZ53 Personal Minigun. The Avenger's design improvements include improved, gel-fin, cooling and chromium plated barrel-bores. This gives it a greater range and lethality. Min ST: 7. |
| 390 | `00000390.pro` | Solar Scorcher | Without the sun's rays to charge this weapon's cpacitors this gun can't light a match. However, in full daylight, the experimental photo-electric cells that power the Scorcher allow it to turn almost anything into a crispy critter. Min ST: 3. |
| 391 | `00000391.pro` | H&K G11E | This gun revolutionized squad level support weapon design. The gun fires a caseless cartridge consisting of a block of propellant with a bullet buried inside. The resultant weight and space savings allow it to have a very high magazine capacity. Min ST: 6. |
| 392 | `00000392.pro` | M72 Gauss Rifle | The M72 rifle is of German design. It uses an electromagnetic field to propel rounds at tremendous speed... and pierce almost any obstacle. Its range, accuracy and stopping power is almost unparalleled. Min ST: 6. |
| 393 | `00000393.pro` | Phazer | NEED LONG DESCRIPTION. Min ST: 3. |
| 394 | `00000394.pro` | PPK12 Gauss Pistol | Praised for its range and stopping power, the PPK12 Gauss Pistol is of German design. The pistol uses an electromagnetic field to propel rounds at tremendous speed and punch through almost any armor. Min ST: 4. |
| 395 | `00000395.pro` | Vindicator Minigun | The German Rheinmetal AG company created the ultimate minigun. The Vindicator throws over 90,000 caseless shells per minute down its six carbon-polymer barrels. As the pinnacle of Teutonic engineering skill, it is the ultimate hand-held weapon. Min ST: 7. |
| 396 | `00000396.pro` | YK32 Pulse Pistol | The YK32 is an electrical pulse weapon that was developed by the Yuma Flats Energy Consortium. Though powerful, the YK32 was never considered a practical weapon due to its inefficient energy usage and bulky design. Min ST: 3. |
| 397 | `00000397.pro` | YK42B Pulse Rifle | The YK42B is an electrical pulse weapon that was developed by the Yuma Flats Energy Consortium. It is considered a far superior weapon to the YK32 pistol, having a greater charge capacity and range. Min ST: 3. |
| 398 | `00000398.pro` | .44 Magnum (Speed Load) | A .44 Magnum Revolver with a speed loader. Min ST: 5. |
| 399 | `00000399.pro` | Super Cattle Prod | A Farmer's Best Friend model cattle prod from Wattz Electronics. This model has been upgraded to increase the electrical discharge. Min ST: 4. |
| 400 | `00000400.pro` | Improved Flamer | A Flambe 450 model flamethrower, varmiter variation. Fires a short spray of extremely hot, flamable liquid. Requires specialized fuel to work properly. This model has been modified to fire a hotter mixture which causes greater combustibility. Min ST: 6. |
| 401 | `00000401.pro` | Laser Rifle (Ext. Cap.) | This Wattz 2000 laser rifle has had its recharging system upgraded and a special recycling chip installed that reduces the drain on the micro fusion cell by 50%. Min ST: 6. |
| 402 | `00000402.pro` | Magneto-Laser Pistol | This Wattz 1000 laser pistol has been upgraded with a magnetic field targeting system that tightens the laser emission, giving this pistol extra penetrating power. Min ST: 3. |
| 403 | `00000403.pro` | FN FAL (Night Sight) | This rifle has been more widely used by armed forces than any other rifle in history. It's a reliable assault weapon for any terrain or tactical situation. This weapon has been equipped with a night sight for greater night accuracy. Min ST: 5. |
| 404 | `00000404.pro` | Desert Eagle (Exp. Mag.) | An ancient Desert Eagle pistol, in .44 magnum. Interest in late 20th century films made this one of the most popular handguns of all times. This one has been equipped with an expanded magazine for longer fun and games! Min ST: 4. |
| 405 | `00000405.pro` | Assault Rifle (Exp. Mag.) | This Assault Rifle has an extended, military sized, ammunition clip. The expanded magazine capacity makes it more fun than ever to Spray-and-Pray. Min ST: 5. |
| 406 | `00000406.pro` | Plasma Pistol (Ext. Cap.) | This Glock 86 Plasma Pistol has had its magnetic housing chamber realigned to reduce the drain on its energy cell. Its efficiency has doubled, allowing it to fire more shots before the battery is expended. Min ST: 4. |
| 407 | `00000407.pro` | Mega Power Fist | A "Big Frigger" Power Fist from BeatCo. Considered by many to be the ultimate weapon to use in unarmed combat. This one has upgraded power servos for increased strength. Powered by small energy cells. Min ST: 1. |
| 421 | `00000421.pro` | Holy Hand Grenade | A Holy relic of godly might. |
| 423 | `00000423.pro` | Gold Nugget | A nugget of gold. |
| 426 | `00000426.pro` | Uranium Ore | A chunk of Uranium Ore, unrefined. |
| 427 | `00000427.pro` | Flame Breath | — |
| 486 | `00000486.pro` | Refined Uranium Ore | A chunk of Uranium Ore. This ore seems to have been processed somehow. It seems heavier. |
| 489 | `00000489.pro` | Special Boxer Weapon | — |
| 496 | `00000496.pro` | Boxing Gloves | A pair of boxing gloves that smell faintly of sweat. They look like they have seen a lot of use: many old blood stains still remain. |
| 497 | `00000497.pro` | Plated Boxing Gloves | Someone has "accidentally" slipped metal plates into these boxing gloves. It could technically be considered cheating, but you prefer to think of it as an increased opportunity to dispense bone-crunching damage. |
| 498 | `00000498.pro` | Dual Plasma Cannon | Only use for Gun Turret critter. |
| 500 | `00000500.pro` | FN FAL HPFA | — |
| 517 | `00000517.pro` | "Little Jesus" | This wicked looking blade once belonged to Lil' Jesus Mordino. It has numerous nicks and cuts along its surface, but its edge is razor sharp. On its handle is carved "Little Jesus." |
| 518 | `00000518.pro` | Dual Minigun | Only use for Auto-Cannon critter. |
| 520 | `00000520.pro` | Heavy Dual Minigun | Only use for Auto-Cannon Critter. |
| 522 | `00000522.pro` | Wakizashi Blade | A short finely crafted knife. The tip seems to be designed to pierce armor. |
| 530 | `00000530.pro` | End Boss Knife | — |
| 531 | `00000531.pro` | End Boss Plasma Gun | — |

## Ammo (25)

| PID | `.pro` | name | description |
|----:|--------|------|-------------|
| 14 | `00000008.pro` | Explosive Rocket | A rocket with a large explosive warhead. |
| 29 | `00000029.pro` | 10mm JHP | Ammunition. Caliber: 10mm, jacketed hollow-points |
| 30 | `00000030.pro` | 10mm AP | Ammunition. Caliber: 10mm, armor piercing. |
| 31 | `00000031.pro` | .44 Magnum JHP | A brick of ammunition, .44 magnum caliber, hollow-points. |
| 32 | `00000032.pro` | Flamethrower Fuel | A cylinder containing an extremely flammable liquid fuel for flamethrowers. |
| 33 | `00000033.pro` | 14mm AP | Large caliber ammunition. 14mm armor piercing. |
| 34 | `00000034.pro` | .223 FMJ | A case of ammunition, .223 caliber, Full Metal Jacket. |
| 35 | `00000035.pro` | 5mm JHP | A brick of small, lightweight ammunition. Caliber: 5mm, jacketed hollow-point. |
| 36 | `00000036.pro` | 5mm AP | A brick of small caliber ammunition. 5mm armor piercing. |
| 37 | `00000037.pro` | Rocket AP | A rocket shell, with a smaller explosive, but designed to pierce armor plating. |
| 38 | `00000038.pro` | Small Energy Cell | A small, self-contained energy storage unit. |
| 39 | `00000039.pro` | Micro Fusion Cell | A medium sized energy production unit. Self-contained fusion plant. |
| 95 | `00000095.pro` | 12 ga. Shotgun Shells | Shotgun ammunition. This particular ammo is marked: "12-gauge shells, not for use by children under the age of 3." |
| 111 | `00000111.pro` | .44 magnum FMJ | A brick of ammunition, .44 magnum caliber, full metal jacket. |
| 121 | `00000121.pro` | 9mm ball | A collection of ancient 9x19mm rounds. Heavy grease to preserve them from the environment. Standard bullets. |
| 163 | `00000163.pro` | BB's | A package of BB's from before the war. In excellent condition. Stainless steel. |
| 274 | `00000274.pro` | Robo Rocket Ammo | Only used by rocket equipped robots. |
| 357 | `00000357.pro` | .45 Caliber | Ammunition. .45 Caliber. |
| 358 | `00000358.pro` | 2mm EC | Ammunition. |
| 359 | `00000359.pro` | 4.7mm Caseless | Ammunition. Caliber: 4.7mm, caseless. |
| 360 | `00000360.pro` | 9mm | Ammunition. Caliber: 9mm. |
| 361 | `00000361.pro` | HN Needler Cartridge | Ammunition. This cartridge appears to be ammo for the HN Needler Pistol. Each 'bullet' is a small hypodermic designed to inject a target with its contents upon impact. |
| 362 | `00000362.pro` | HN AP Needler Cartridge | Ammunition. This cartridge appears to be armor-piercing ammo for the HN Needler Pistol. The hypodermic tips are made of a strange alloy and are incredibly sharp. |
| 363 | `00000363.pro` | 7.62mm | Ammunition. Caliber: 7.62mm. |
| 382 | `00000382.pro` | Flamethrower Fuel MKII | This flamethrower fuel uses an advanced super-burn mix. |

## Armor (16)

| PID | `.pro` | name | description |
|----:|--------|------|-------------|
| 1 | `00000003.pro` | Leather Armor | Your basic all leather apparel. Finely crafted from tanned brahmin hide. |
| 2 | `00000001.pro` | Metal Armor | Polished metal plates, crudely forming a suit of armor. |
| 3 | `00000014.pro` | Power Armor | A self-contained suit of advanced technology armor. Powered by a micro-fusion reactor, with enough fuel to last a hundred years. |
| 17 | `00000017.pro` | Combat Armor | High tech armor, made out of advanced defensive polymers. |
| 74 | `00000074.pro` | Leather Jacket | A black, heavy leather jacket. |
| 113 | `00000113.pro` | Robes | Robes from the Children of the Cathedral. |
| 232 | `00000232.pro` | Hardened Power Armor | A suit of T-51b Power Armor. The hardening process has improved the defensive capability of this high-tech armor system. |
| 239 | `00000239.pro` | Brotherhood Armor | A superior version of Combat Armor. The Brotherhood of Steel have made many improvements over the standard version. |
| 240 | `00000240.pro` | Tesla Armor | This shining armor provides superior protection against energy attacks. The three Tesla Attraction Coil Rods disperse a large percentage of directed energy attacks. |
| 265 | `00000265.pro` | Combat Leather Jacket | This heavily padded leather jacket is unusual in that it has two sleeves. You'll definitely make a fashion statement whenever, and wherever, you rumble. |
| 348 | `00000348.pro` | Advanced Power Armor | This powered armor is composed of lightweight metal alloys, reinforced with ceramic castings at key points. The motion-assist servo-motors appear to be high quality models as well. |
| 349 | `00000349.pro` | Adv. Power Armor MKII | This powered armor appears to be composed of entirely of lightweight ceramic composites rather than the usual combination of metal and ceramic plates. It seems as though it should give even more protection than the standard Advanced Power Armor. |
| 379 | `00000379.pro` | Leather Armor Mark II | An enhanced version of the basic leather armor with extra layers of protection. Finely crafted from tanned brahmin hide. |
| 380 | `00000380.pro` | Metal Armor Mark II | Polished metal plates, finely crafted into a suit of armor. |
| 381 | `00000381.pro` | Combat Armor Mark II | High tech armor, made out of advanced defensive polymers. |
| 524 | `00000524.pro` | Bridgekeeper's Robes | This smelly, filthy garment must be made out of some special fabric in order to have withstood the foulness of the bridgekeeper's body. Oddly enough, it has plasma burns and scorch marks all over it, as if these weapons were used against it... to no effect. You have NO idea why it's purple. |

## Drug (28)

| PID | `.pro` | name | description |
|----:|--------|------|-------------|
| 40 | `00000040.pro` | Stimpak | A healing chem. When injected, the chem provides immediate healing of minor wounds. |
| 48 | `00000048.pro` | RadAway | A chemical solution that bonds with radiation particles and passes them through your system. Takes time to work. |
| 49 | `00000049.pro` | Antidote | A bottle containing a home-brewed antidote for poison. A milky liquid with floating pieces of radscorpion flesh. |
| 53 | `00000053.pro` | Mentats | A pillbox of mind-altering chems. Increases memory related functions, and speeds other mental processes. Highly habit-forming. |
| 71 | `00000071.pro` | Fruit | A strange piece of fruit. No preservatives and no additional food coloring added. |
| 81 | `00000081.pro` | Iguana-on-a-stick | A cooked iguana, roasted in it's own skin. |
| 87 | `00000087.pro` | Buffout | Highly advanced steroids. While in effect, they increase strength and reflexes. Very habit-forming. |
| 103 | `00000103.pro` | Iguana-on-a-stick | Some charred meat and vegetables on a cooking stick. |
| 106 | `00000106.pro` | Nuka-Cola | A bottle of Nuka-Cola, the flavored softdrink of the post-nuclear world. Warm and flat. |
| 109 | `00000109.pro` | Rad-X | Anti-radiation chems to be taken before exposure. No known side effects. |
| 110 | `00000110.pro` | Psycho | An unique delivery system filled with strange and unknown chemicals of probably military origin. It is supposed to increase the combat potential of a soldier. |
| 124 | `00000124.pro` | Beer | Some type of home brewed beer. |
| 125 | `00000125.pro` | Booze | An ancient liquor, from the pre-War era. |
| 144 | `00000144.pro` | Super Stimpak | An advanced healing chem. Very powerful. Superstims will cause a small amount of damage after a period of time due to powerful nature of the chemicals used. |
| 259 | `00000259.pro` | Jet | Jet is a powerful metamphetamine that stimulates the central nervous system. The initial euphoric rush rarely lasts more than a few minutes, but during that time, the user is filled with a rush of energy & strength. |
| 260 | `00000260.pro` | Jet Antidote | This antidote cures the reliant effects of Jet. |
| 273 | `00000273.pro` | Healing Powder | A very powerful healing magic- though it will bring the feeling of sleep to your head. |
| 310 | `00000310.pro` | Gamma Gulp Beer | A bottle of Gamma Gulp beer. It glows in the dark! |
| 311 | `00000311.pro` | Roentgen Rum | A bottle of Roentgen rum. It glows in the dark! |
| 334 | `00000334.pro` | Poison | A hypodermic needle full of powerful poison. |
| 378 | `00000378.pro` | Cookie | A chocolate chip cookie. Yum! |
| 424 | `00000424.pro` | Monument Chunk | This is a piece of the disgruntled stone monument you found out in the desert. Although many of your village would no doubt regard it as a sacred relic, somehow you suspect that you have been cheated. |
| 469 | `00000469.pro` | Rot Gut | A very strong liquor or cleaning fluid, you decide. |
| 473 | `00000473.pro` | Mutated Toe | You see your sixth toe. It is a small mutated part of yourself. For some reason, you feel a terrible sense of loss as you look at the tiny amputated toe. |
| 480 | `00000480.pro` | Bonus +1 Agility | — |
| 481 | `00000481.pro` | Bonus +1 Intelligence | — |
| 482 | `00000482.pro` | Bonus +1 Strength | — |
| 525 | `00000525.pro` | Hypo | A medical injection instrument of some kind. It looks very high tech. You don't know what it's filled with but it appears to have only one dose left. |

## Container (115)

| PID | `.pro` | name | description |
|----:|--------|------|-------------|
| 42 | `00000042.pro` | Fridge | A refrigerator. Out of coolant. |
| 43 | `00000043.pro` | Ice Chest | On old-style ice chest. The hinges are in good working condition. |
| 44 | `00000044.pro` | Ice Chest | On old-style ice chest. The hinges are in good working condition. |
| 46 | `00000046.pro` | Bag | An average sized bag. Made from weaved brahmin hairs. |
| 60 | `00000060.pro` | Bookcase | A wooden bookcase. |
| 61 | `00000061.pro` | Bookcase | A wooden bookcase. |
| 62 | `00000062.pro` | Bookcase | A wooden bookcase. |
| 63 | `00000063.pro` | Bookcase | A wooden bookcase. |
| 64 | `00000064.pro` | Bookcase | A wooden bookcase. |
| 65 | `00000065.pro` | Bookcase | A wooden bookcase. |
| 66 | `00000066.pro` | Desk | A wooden desk. |
| 67 | `00000067.pro` | Desk | A wooden desk. |
| 68 | `00000068.pro` | Dresser | A wooden dresser. |
| 69 | `00000069.pro` | Dresser | A wooden dresser. |
| 70 | `00000070.pro` | Dresser | A wooden dresser. |
| 90 | `00000090.pro` | Back Pack | A basic backpack, with optional carrying straps. |
| 93 | `00000093.pro` | Bag | An average sized bag. Made from weaved Brahmin hairs. |
| 107 | `00000107.pro` | Bones | A collection of strange bones. |
| 108 | `00000108.pro` | Bones | A collection of strange bones. |
| 128 | `00000128.pro` | Footlocker | Your basic footlocker. Holds stuff, sits at foot of bed, can be locked. |
| 129 | `00000129.pro` | Footlocker | Your basic footlocker. Holds stuff, sits at foot of bed, can be locked. |
| 130 | `00000130.pro` | Footlocker | Your basic footlocker. Holds stuff, sits at foot of bed, can be locked. |
| 131 | `00000131.pro` | Footlocker | Your basic footlocker. Holds stuff, sits at foot of bed, can be locked. |
| 132 | `00000132.pro` | Locker | A storage container. |
| 133 | `00000133.pro` | Locker | A storage container. |
| 134 | `00000134.pro` | Locker | A storage container. |
| 135 | `00000135.pro` | Locker | A storage container. |
| 136 | `00000136.pro` | Locker | A storage container. |
| 137 | `00000137.pro` | Locker | A storage container. |
| 138 | `00000138.pro` | Locker | A storage container. |
| 139 | `00000139.pro` | Locker | A storage container. |
| 145 | `00000145.pro` | Bookshelf | A wooden bookshelf. |
| 146 | `00000146.pro` | Bookshelf | A wooden bookshelf. |
| 147 | `00000147.pro` | Bookshelf | A wooden bookshelf. |
| 149 | `00000149.pro` | Bookshelf | A wooden bookshelf. |
| 151 | `00000151.pro` | Shelves | A set of wooden shelves. |
| 152 | `00000152.pro` | Shelves | A set of wooden shelves. |
| 153 | `00000153.pro` | Shelves | A set of wooden shelves. |
| 155 | `00000155.pro` | Shelves | A set of wooden shelves. |
| 157 | `00000157.pro` | Workbench | A workbench. Yep, your standard old, run of the mill, workbench. Looks nice sitting there, too. |
| 158 | `00000158.pro` | Tool Board | This board holds a variety of tools above the popular workbench. |
| 165 | `00000165.pro` | Iguana Stand | The home of Bob's Iguana Bits. What's not to love about fresh-roasted iguana? |
| 166 | `00000166.pro` | Table | A generic table. Holds stuff off the ground. |
| 167 | `00000167.pro` | Table | A generic table. Holds stuff off the ground. |
| 168 | `00000168.pro` | Stuff | These are miscellaneous items, also known as "stuff". |
| 169 | `00000169.pro` | Stuff | These are miscellaneous items, also known as "stuff". |
| 170 | `00000170.pro` | Stuff | These are miscellaneous items, also known as "stuff". |
| 171 | `00000171.pro` | Stuff | These are miscellaneous items, also known as "stuff". |
| 172 | `00000172.pro` | Stuff | These are miscellaneous items, also known as "stuff". |
| 173 | `00000173.pro` | Stuff | These are miscellaneous items, also known as "stuff". |
| 174 | `00000174.pro` | Stuff | These are miscellaneous items, also known as "stuff". |
| 175 | `00000175.pro` | Stuff | These are miscellaneous items, also known as "stuff". |
| 176 | `00000176.pro` | Stuff | These are miscellaneous items, also known as "stuff". |
| 177 | `00000177.pro` | Stuff | These are miscellaneous items, also known as "stuff". |
| 178 | `00000178.pro` | Stuff | These are miscellaneous items, also known as "stuff". |
| 179 | `00000179.pro` | Stuff | These are miscellaneous items, also known as "stuff". |
| 180 | `00000180.pro` | Crate | A wooden crate filled with generic stuff. |
| 181 | `00000181.pro` | Desk | A generic desk. Seen one desk, you've seen them all. |
| 182 | `00000182.pro` | Desk | A generic desk. |
| 183 | `00000183.pro` | Desk | A generic desk. |
| 184 | `00000184.pro` | Desk | A generic desk. |
| 185 | `00000185.pro` | Desk | A generic desk. |
| 186 | `00000186.pro` | Desk | A generic desk. |
| 187 | `00000187.pro` | Desk | A generic desk. |
| 188 | `00000188.pro` | Locker | A locker. |
| 189 | `00000189.pro` | Locker | A locker. |
| 197 | `00000197.pro` | Box | — |
| 198 | `00000198.pro` | Box | — |
| 199 | `00000199.pro` | Box | — |
| 200 | `00000200.pro` | Box | — |
| 201 | `00000201.pro` | Box | — |
| 202 | `00000202.pro` | Box | — |
| 203 | `00000203.pro` | Box | — |
| 204 | `00000204.pro` | Box | — |
| 211 | `00000211.pro` | Bones | You see Ed. Ed's dead. |
| 213 | `00000213.pro` | Remains of Gizmo | Gizmo, former crime boss of Junktown, is dead. And pretty stinky, too. |
| 214 | `00000214.pro` | Desk | A desk. After further observation, you decide that it is still a desk. |
| 243 | `00000243.pro` | Pot | A finely crafted clay pot. |
| 244 | `00000244.pro` | Pot | A finely crafted clay pot. |
| 245 | `00000245.pro` | Chest | A wooden chest. |
| 246 | `00000246.pro` | Shelf | A wood shelf with miscellaneous items. |
| 247 | `00000247.pro` | Shelf | A wood shelf with miscellaneous items. |
| 248 | `00000248.pro` | Pot | A finely crafted clay pot. |
| 249 | `00000249.pro` | Pot | A finely crafted clay pot. |
| 250 | `00000250.pro` | Bones | A pile of human bones. |
| 330 | `00000330.pro` | Crashed Verti-Bird | This is wreakage from a verti-bird. Looks like it crashed here months ago. |
| 344 | `00000344.pro` | Gravesite | — |
| 345 | `00000345.pro` | Gravesite | — |
| 346 | `00000346.pro` | Gravesite | — |
| 347 | `00000347.pro` | Gravesite | — |
| 367 | `00000367.pro` | Ammo Crate | — |
| 368 | `00000368.pro` | Ammo Crate | — |
| 369 | `00000369.pro` | Ammo Crate | — |
| 370 | `00000370.pro` | Ammo Crate | — |
| 374 | `00000374.pro` | Gravesite | — |
| 375 | `00000375.pro` | Gravesite | — |
| 376 | `00000376.pro` | Gravesite | — |
| 425 | `00000425.pro` | Stone Head | A huge stone monument. |
| 434 | `00000434.pro` | Wagon | A caravan wagon converted from the wrecked remains of an ancient automobile. |
| 435 | `00000435.pro` | Wagon | A caravan wagon converted from the wrecked remains of an ancient automobile. |
| 455 | `00000455.pro` | Car Trunk | The trunk is a great place to store your stuff. |
| 467 | `00000467.pro` | Jesse's Container | — |
| 501 | `00000501.pro` | Wall Safe | — |
| 502 | `00000502.pro` | Floor Safe | — |
| 510 | `00000510.pro` | Pool Table | — |
| 511 | `00000511.pro` | Pool Table | — |
| 512 | `00000512.pro` | Pool Table | — |
| 513 | `00000513.pro` | Pool Table | — |
| 514 | `00000514.pro` | Pool Table | — |
| 515 | `00000515.pro` | Pool Table | — |
| 521 | `00000521.pro` | Poor Box | A small box used to collect money for the poor. |
| 526 | `00000526.pro` | Dead Redshirt | This guy has a weird red uniform you have never seen. It looks like he died from dehydration. |
| 527 | `00000527.pro` | Dead Redshirt | This guy has a weird red uniform you have never seen. It looks like he died from dehydration. |
| 528 | `00000528.pro` | Dead Redshirt | This guy has a weird red uniform you have never seen. It looks like he died from dehydration. |
| 529 | `00000529.pro` | Mining Machine | — |

## Key (8)

| PID | `.pro` | name | description |
|----:|--------|------|-------------|
| 82 | `00000082.pro` | Key | A key. A key will open a particular lock. |
| 83 | `00000083.pro` | Key Ring | Multiple keys. |
| 96 | `00000096.pro` | Red Pass Key | A electronic security key, color coded red. |
| 97 | `00000097.pro` | Blue Pass Key | A electronic security key, color coded blue. |
| 105 | `00000105.pro` | Key | A special key of some sort. |
| 223 | `00000223.pro` | Yellow Pass Key | An electronic security key, color coded yellow. |
| 438 | `00000438.pro` | Temple Key | A key for the Arroyo Temple door. |
| 456 | `00000456.pro` | Cell Door Key | This key fits the Cell Door in Broken Hills. |

## Misc (229)

| PID | `.pro` | name | description |
|----:|--------|------|-------------|
| 41 | `00000041.pro` | Money | Legal tender for the world of the wastes. |
| 47 | `00000047.pro` | First Aid Kit | A small kit containing basic medical equipment. Bandages, wraps, antiseptic spray, and more. |
| 50 | `00000050.pro` | Reserved Item | This is a reserved item. DO NOT USE. |
| 51 | `00000051.pro` | Dynamite | A high explosive, consisting of nitroglycerin mixed with the absorbent substance kiselguhr. Includes a timer. |
| 52 | `00000052.pro` | Geiger Counter | A Wattz Electronics C-Radz model Geiger Counter. Detects the presence and strength of radiation fields. |
| 54 | `00000054.pro` | Stealth Boy | A RobCo Stealth Boy 3001 personal stealth device. Generates a modulating field that transmits the reflected light from one side of an object to the other. |
| 55 | `00000055.pro` | Water Chip | This is a Vault-Tec water chip. They are typically packaged in groups of five to a box. |
| 56 | `00000056.pro` | Dog Tags | A set of military dog tags. The name Darkwater is readable, but not much else is. |
| 57 | `00000057.pro` | Bug | A miniature microphone and transmitting device. |
| 58 | `00000058.pro` | Tape | A Wattz Electronics Holodisc tape. This particular tape looks to be into very poor condition. |
| 59 | `00000059.pro` | Motion Sensor | A Wattz Electronics C-U model motion sensor. Detects the movement of biological material over a distance of meters using a tuned radar device. Having one in your inventory will also help you avoid outdoor encounters (+20% Outdoorsman skill). |
| 72 | `00000072.pro` | Briefcase | A briefcase, with a Made-in-the-USA label. Leather. Good condition, but the combination lock is broken. |
| 73 | `00000073.pro` | Big Book of Science | A set of books, containing information about different scientific fields. |
| 75 | `00000075.pro` | Tool | A tool set, containing various useful tools, including pliers. |
| 76 | `00000076.pro` | Deans Electronics | A study book on the field of electronics. A note on the cover says that it is for the "budding young electrician in everyone!" |
| 77 | `00000077.pro` | Electronic Lockpick | A Wattz Electronics Micromanipulator FingerStuff electronic lockpick. For defeating electronic locks and security devices. |
| 78 | `00000078.pro` | Fuzzy Painting | An image of a singer. Obviously, very old. The image has a felt coating that is still in good condition. |
| 80 | `00000080.pro` | First Aid Book | A study book on the concepts and practical use of first aid skills. |
| 84 | `00000084.pro` | Lockpicks | A set of locksmith tools. Includes all the necessary picks and tension wrenches to open conventional pin and tumbler locks. |
| 85 | `00000085.pro` | Plastic Explosives | A chunk of Cordex, a military brand of plastic explosives. Highly stable, very destructive. Includes a timer. |
| 86 | `00000086.pro` | Scout Handbook | A book on the methods and ideals of Scouting. Very practical information regarding outdoor life. |
| 88 | `00000088.pro` | Watch | An expensive watch. Not really working, but it still looks nice. |
| 89 | `00000089.pro` | Motor | A 40-hp electric motor. |
| 91 | `00000091.pro` | Doctor's Bag | This brown bag contains instruments and chems used by doctors in the application of their trade. |
| 92 | `00000092.pro` | Scorpion Tail | The severed tail of a radscorpion. |
| 98 | `00000098.pro` | Junk | A pile of junk parts. A little bit of everything. |
| 99 | `00000099.pro` | Gold Locket | A valuable gold locket. |
| 100 | `00000100.pro` | Radio | A model 2043B Radio Communicator, from the fine people at Wattz Electronics. Dependable, rugged, and camouflaged. With the optional RS-121 interface. |
| 101 | `00000101.pro` | Lighter | A silver butane lighter, in good condition. |
| 102 | `00000102.pro` | Guns and Bullets | A gun rag. A magazine devoted to the practical use of firearms, and the occasional biased review. |
| 104 | `00000104.pro` | Tape Recorder | A Wattz Electronics Play-It-For-Me tape recorder. Plays and records the standard 30 minute high density Record-It-Once tapes. |
| 112 | `00000112.pro` | Urn | A beautiful golden urn, with the name "Harriet" inscribed on the front and ashes inside. |
| 114 | `00000114.pro` | Tangler's Hand | A cybernetic manipulator, in the shape and form of a hand. Damaged due to the sloppy nature of the removal process. |
| 117 | `00000117.pro` | Flower | A beautiful flower. |
| 119 | `00000119.pro` | Necklace | An expensive looking necklace, made from silver, gold and pressed diamonds. |
| 123 | `00000123.pro` | Psychic Nullifier | A strange device, constructed from an odd technology. |
| 126 | `00000126.pro` | Water Flask | A container for the holding and preservation of water or other liquids. |
| 127 | `00000127.pro` | Rope | A strong, thick line. About 45 feet in length. |
| 140 | `00000140.pro` | Access Card | A security access card. Still in working condition. |
| 141 | `00000141.pro` | COC Badge | A badge in the shape of the Children of the Cathedral symbol. On one side you notice bumps and indentations, almost reminding you of a key. |
| 142 | `00000142.pro` | COC Badge | A badge in the shape of the Children of the Cathedral symbol. On one side you notice bumps and indentations, almost reminding you of a key. |
| 148 | `00000148.pro` | Bookshelf | A wooden bookshelf. |
| 150 | `00000150.pro` | Bookshelf | A wooden bookshelf. |
| 154 | `00000154.pro` | Shelves | A set of wooden shelves. |
| 156 | `00000156.pro` | Shelves | A set of wooden shelves. |
| 164 | `00000164.pro` | Brotherhood Tape | A holodisc from regarding the Brotherhood of Steel. |
| 190 | `00000190.pro` | FEV Disk | A holotape containing medical information. Can be used to enter information into your PIPBoy. |
| 191 | `00000191.pro` | Security Disk | A holotape with the writing "Security Log" on the label. You can use it to transfer the data it contains to your PIPBoy. |
| 192 | `00000192.pro` | Alpha Experiment Disk | The label on this holotape reads: "Alpha Experiment Log" |
| 193 | `00000193.pro` | Delta Experiment Disk | The label on this holotape reads: "Delta Experiment Log" |
| 194 | `00000194.pro` | Vree's Experiment Disk | This holotape has been updated with medical evidence from Vree of the BoS. It can be used to transfer the data to your PIPBoy. |
| 195 | `00000195.pro` | Brotherhood Honor Code | A holotape, with a rough symbol of the Brotherhood of Steel inscribed on it. Use the tape to transfer the data to your PIPBoy. |
| 196 | `00000196.pro` | Mutant Transmissions | This holotape looks like it was set to record audio data from a radio. Use the tape to transfer the information to your PIPBoy. |
| 206 | `00000206.pro` | Dynamite | A high explosive, consisting of nitroglycerin mixed with the absorbent substance kiselguhr. Includes a timer, which is ticking. |
| 207 | `00000207.pro` | Geiger Counter | A Wattz Electronics C-Radz model Geiger Counter. Detects the presence and strength of radiation fields. It is on. |
| 208 | `00000208.pro` | Motion Sensor | A Wattz Electronics C-U model motion sensor. Detects the movement of biological material over a distance of meters using a tuned radar device. It is on. |
| 209 | `00000209.pro` | Plastic Explosives | A chunk of Cordex, a military brand of plastic explosives. Highly stable, very destructive. Includes a timer, which is ticking. |
| 210 | `00000210.pro` | Stealth Boy | A RobCo Stealth Boy 3001 personal stealth device. Generates a modulating field that transmits the reflected light from one side of an object to the other. |
| 212 | `00000212.pro` | Tandi | You are bartering for Tandi's release. |
| 215 | `00000215.pro` | Brotherhood History | A holotape containing information relating to the Brotherhood of Steel. Can be used to enter information into your PIPBoy 2000. |
| 216 | `00000216.pro` | Maxson's History | — |
| 217 | `00000217.pro` | Maxson's Journal | — |
| 218 | `00000218.pro` | Light Healing | Barter for this option if you want light healing. |
| 219 | `00000219.pro` | Medium Healing | Barter for this option if you want medium healing. |
| 220 | `00000220.pro` | Heavy Healing | Barter for this option if you want lots of care and heavy healing. |
| 221 | `00000221.pro` | Security Card | A keycard with a security level encoded within it's very simple electronics. |
| 222 | `00000222.pro` | Field Switch | An electronic transmission device, with a very simple, and large, toggle button. It looks like it was recently made and designed for very large hands. |
| 224 | `00000224.pro` | Small Statuette | You think this might be a carving of the "Pip Boy" but you can't be sure. |
| 225 | `00000225.pro` | Cat's Paw Magazine | An issue of Cat's Paw magazine. |
| 226 | `00000226.pro` | Box Of Noodles | You have no idea what "Instant Spaghetti" means. |
| 227 | `00000227.pro` | Small Dusty Box Of Some Sort | A television dinner. You're not sure, but it's definitely not edible. You're not quite sure if it ever was. |
| 228 | `00000228.pro` | Technical Manual | A technical repair manual on the T-51b Power Armor. |
| 229 | `00000229.pro` | Small Piece Of Machinery | You see a Systolic Motivator. This unusual piece of machinery is very small. |
| 230 | `00000230.pro` | Vault Records | A compendium of events and important recordings from a Vault computer system. Possibly damaged. |
| 231 | `00000231.pro` | Military Base Security Code | — |
| 237 | `00000237.pro` | Chemistry Journals | A random pile of literature regarding the field of Chemistry. The papers on Molecular Chemistry are particularly interesting. |
| 238 | `00000238.pro` | Regulator Transmission | A holotape recording of an audio transmission. |
| 251 | `00000251.pro` | Anna's Bones | These bones seem unsettled. |
| 252 | `00000252.pro` | Anna's Gold Locket | This well-worn golden locket opens to reveal a charming picture. |
| 253 | `00000253.pro` | Fuel Cell Controller | This chip controls the flow of power into a car's electric engines. Many drivers quickly burnt out this chip through frequent rapid acceleration. Still a valuable part to have-- if you only had a car to install it in. |
| 254 | `00000254.pro` | Fuel Cell Regulator | Some car-owners installed this regulator, that doubles your car's mileage between charges, but most drivers didn't care how much juice their cars consumed, after all, power's cheap and plentiful so why worry. |
| 255 | `00000255.pro` | Day Pass | This slightly crumpled piece of paper grants you access to all areas of Vault City, except the Vault itself, during daylight hours only. |
| 256 | `00000256.pro` | False Citizenship Papers | This is a set of false Citizenship Papers. They look authentic enough, but you doubt they could pass a serious inspection. |
| 257 | `00000257.pro` | Cornelius' Gold Watch | This is an ancient time-piece known as a pocket watch. Time ran out for this keepsake, many long years ago. |
| 258 | `00000258.pro` | Hydroelectric Part | This is a Hydroelectric Magnetosphere Regulator. |
| 262 | `00000262.pro` | Rubber Boots | An old pair of sturdy rubber work boots. They look durable enough to keep out sludge, at least for a while. |
| 263 | `00000263.pro` | Slag Message | This is a message from the leader of the Slags to the townspeople of Modoc. |
| 264 | `00000264.pro` | Smith's Cool Item | LONG DESCRIPTION HERE. |
| 266 | `00000266.pro` | Vic's Radio | This hand-held radio doesn't work but it looks as though it's still in pretty good condition. Some of its parts could probably be salvaged for use in other radios. |
| 267 | `00000267.pro` | Vic's Water Flask | This relic of the Vault was probably used to contain some sacred sacrament. The Holy number 13 is emblazoned on the side of this precious link to your people's past, and hopeful future. |
| 269 | `00000269.pro` | Robot Parts | A pile of damaged robot parts. |
| 271 | `00000271.pro` | Broc Flower | The plentiful flower that forms the base for the powder of healing. |
| 272 | `00000272.pro` | Xander Root | The rare root that gives healing properties to the powder of healing. |
| 275 | `00000275.pro` | Trophy of Recognition | A solid gold trophy with the inscription "To: Dave, From: DC's, For: Being a Special Person." |
| 276 | `00000276.pro` | Gecko Pelt | This is the dried and cured hide of a Gecko. |
| 277 | `00000277.pro` | Golden Gecko Pelt | This is the dried and cured hide of a Golden Gecko. |
| 278 | `00000278.pro` | Flint | A stone used to sharpen weapons. |
| 279 | `00000279.pro` | Neural Interface | It appears to be a set of electrodes built into a head unit with a standard computer interface plug. |
| 281 | `00000281.pro` | Dixon's Eye | A small jar containing a human eye suspended in some kind of liquid. The label says Corporal Dixon. |
| 282 | `00000282.pro` | Clifton's Eye | A small jar containing a human eye suspended in some kind of liquid. The label says General Clifton. |
| 284 | `00000284.pro` | Meat Jerky | These smoked and dried chunks of beast-flesh remain chewy-licious and even somewhat nutritious for years, and years, and years.... |
| 285 | `00000285.pro` | Radscorpion Limbs | These Radscorpion pincers are hollowed out and have a strap with a broken buckle at the end. |
| 286 | `00000286.pro` | Firewood | A pile of firewood and kindling. |
| 288 | `00000288.pro` | Car Fuel Cell | A car fuel cell. |
| 289 | `00000289.pro` | Shovel | This is a shovel for digging ditches and stuff. |
| 294 | `00000294.pro` | Vault 13 Holodisk | A holodisk which supposedly has the location of Vault 13. |
| 295 | `00000295.pro` | Cheezy Poofs | A box of cheese flavored puffs. They are extremely good. |
| 297 | `00000297.pro` | Metal Pole | It's a long metal pole. What did you expect? |
| 298 | `00000298.pro` | Trapper Town Key | A standard key. |
| 301 | `00000301.pro` | Clipboard | This is a clipboard with the coolant report. |
| 302 | `00000302.pro` | Gecko Holodisk | The label on this holotape reads: "Gecko Economic Data." It's difficult to interpret most of the technical charts, tables, and formulae here, but the majority of the information seems to highlight the advantages of a Vault City -- Gecko alliance. |
| 303 | `00000303.pro` | Reactor Holodisk | The label on this holotape reads: "Gecko Reactor Performance Data." This disk contains a string of numbers that mean nothing to you. There sure is a lot of information here and it must be important to someone, somehow. |
| 304 | `00000304.pro` | Deck of "Tragic" Cards | This is a deck of cards for a collectible card game. Looks like it could be an expensive hobby if you got hooked. |
| 305 | `00000305.pro` | Yellow Reactor Keycard | An electronic security key, color coded yellow. |
| 306 | `00000306.pro` | Red Reactor Keycard | An electronic security key, color coded red. |
| 307 | `00000307.pro` | Plasma Transformer | This is a Three-step Plasma Transformer. |
| 308 | `00000308.pro` | Super Tool Kit | An impressive tool set made by "Snap-Off". |
| 309 | `00000309.pro` | Talisman | A talisman which is worn by followers of The Brain. |
| 312 | `00000312.pro` | Part Requisition Form | This is a Part Requisition Request form. |
| 314 | `00000314.pro` | Condom (Blue Package) | This is a Jimmy Hats brand condom, a very reliable brand. This one is ribbed for her pleasure. |
| 315 | `00000315.pro` | Condom (Green package) | This is a Jimmy Hats brand condom, a very reliable brand. This one contains phosphorous green dye #2 and is slathered in spermicidal lubricant for added protection. |
| 316 | `00000316.pro` | Condom (Red Package) | This is a Jimmy Hats brand condom, a very reliable brand. This one contains phosophorous red dye #5 and has a yummy cinnamon bun flavor... or so you heard. |
| 317 | `00000317.pro` | Cosmetics Case | This is a genuine Mary May brand cosmetics case. |
| 318 | `00000318.pro` | Empty Hypodermic | This is an empty hypodermic. |
| 321 | `00000321.pro` | Cybernetic Brain | A human brain that has been enhanced by the addition of electronic and robotic attachments. |
| 322 | `00000322.pro` | Human Brain | A typical human brain. Normally they are found in human skulls. Yuck. |
| 323 | `00000323.pro` | Chimpanzee Brain | A monkey brain. It's soft and squishy. |
| 324 | `00000324.pro` | Abnormal Brain | Quite possibly a human brain. The color doesn't seem quite right and the left hemisphere of the brain has caved in on itself. |
| 325 | `00000325.pro` | Dice | A standard pair of gambling dice. It'd be cool to get a fuzzy pair for your car. |
| 326 | `00000326.pro` | Loaded Dice | A pair of loaded dice. Don't be flashing these around the casino. |
| 327 | `00000327.pro` | Easter Egg | This is a hard boiled chicken egg painted with colored dyes. |
| 328 | `00000328.pro` | Magic 8-Ball | This black sphere is some strange precognitive device... a small window on the top seems to be able to predict the future! Pre-war humanity must have been geniuses to invent such a wonder! |
| 329 | `00000329.pro` | Mutagenic Serum | A strange organic concoction that could possibly reverse the Mutation Factor in humans. |
| 331 | `00000331.pro` | Cat's Paw Issue #5 | This is the hard to find Issue 5 of Cat's Paw magazine. The pictures aside, this issue has a wonderful article on energy weapons. |
| 333 | `00000333.pro` | Heart Pills | These pills are used by people with heart problems. |
| 335 | `00000335.pro` | Moore's Briefcase | Thomas Moore asked you to take this brahmin hide briefcase to the Bishop family in New Reno. It is securely locked, and you can't seem to open it. |
| 336 | `00000336.pro` | Moore's Briefcase | Thomas Moore asked you to take this brahmin hide briefcase to the Bishop family in New Reno. It is securely locked, and you can't seem to open it. |
| 337 | `00000337.pro` | Lynette Holodisk | This is the holodisk you got from Lynette. |
| 338 | `00000338.pro` | Westin Holodisk | This is the holodisk you got from Westin. |
| 339 | `00000339.pro` | NCR Spy Holodisk | This holodisk contains reports of NCR caravan schedules and security information. It's addressed to Darion and signed "F". With this info, it's easy to see how the raiders have avoided capture. |
| 340 | `00000340.pro` | Doctor's Papers | A set of very detailed plans for a Cybernetic Canine Guard Unit. With this and the right facilities, a person just might be able to build a robo-dog. |
| 341 | `00000341.pro` | Presidential Pass | The bearer of this document is Security Clearance A-Prime as authorized by the authority of the President of the New California Republic. It's signed by President Tandi. |
| 342 | `00000342.pro` | Ranger Pin | This pin says you're an official New California Ranger. Look -- it's got a code wheel and everything! |
| 343 | `00000343.pro` | Ranger's Map | A map of the surrounding area -- it won't help you much -- but there are code names on it for the Ranger safe houses in the north. |
| 356 | `00000356.pro` | Computer Voice Module | A circuit board with several unidentifiable parts, a microphone, and an inscription that reads "Vault-Tec Voice Recognition Module." |
| 364 | `00000364.pro` | Robot Motivator | This is the drive mechanism for a robot. |
| 366 | `00000366.pro` | G.E.C.K. | The Garden of Eden Creation Kit. This unit is standard equipment for all Vault-Tec vaults. A GECK is the resource for rebuilding civilization after the bomb. Just add water and stir. |
| 373 | `00000373.pro` | Vault 15 Keycard | An electronic security key, color coded red. |
| 377 | `00000377.pro` | Vault 15 Computer Parts | Miscellaneous computer parts with various functions. A computer geek's dream. |
| 408 | `00000408.pro` | Field Medic First Aid Kit | A small kit containing basic emergency medical equipment. Bandages, wraps, antiseptic spray, and more. |
| 409 | `00000409.pro` | Paramedics Bag | This bag contains instruments and chems used by paramedics in the field. The tools contained are specifically designed for high trauma and emergency cases. |
| 410 | `00000410.pro` | Expanded Lockpick Set | A set of locksmith tools. Includes all the necessary picks and tension wrenches to open conventional pin and tumbler locks. This set also includes some special tools for more difficult mechanical locking mechanisms. |
| 411 | `00000411.pro` | Electronic Lockpick MKII | This is the second generation Wattz Electronics Micromanipulator FingerStuff electronic lockpick. For defeating electronic locks and security devices. This Mark II version includes updated software and interface tools. |
| 412 | `00000412.pro` | Oil Can | You see a can of Armor-Go. A space-age, polymer, lubricant for powered armor. |
| 413 | `00000413.pro` | Stables ID Badge | This is a temporary ID badge. It has a red stripe through it, which seems to indicate that you are a "Stables Researcher." |
| 414 | `00000414.pro` | Vault 15 Shack Key | A standard key. |
| 415 | `00000415.pro` | Spectacles | A set of spectacles for eye correction. |
| 416 | `00000416.pro` | Empty Jet Canister | This empty jet canister was found in Richard Wright's room. It still has traces of Jet inside. |
| 417 | `00000417.pro` | Oxygen Tank | This is the oxygen tank for Salvatore's breathing apparatus. |
| 418 | `00000418.pro` | Poison Tank | This tank looks suspiciously like an oxygen tank, but there is a small skull and crossbones symbol etched in the bottom. If you hadn't examined it closely, you wouldn't have seen the symbol. |
| 419 | `00000419.pro` | Mine Parts | You see a bundle of parts that are clearly labeled "GX-9 Air Purifier". Using your incredible deductive skills, you decide that they are probably parts for a broken air purifier. |
| 420 | `00000420.pro` | Morningstar Mine Scrip | This is a small chit probably used to pay Morningstar Mine Workers. |
| 422 | `00000422.pro` | Excavator Chip | This chip appears to be for some sort of large industrial machine. Beneath the dirt and dust, it appears to be in surprisingly good shape. |
| 428 | `00000428.pro` | Medical Supplies | A box of assorted medical supplies. Nothing I can make use of though. |
| 429 | `00000429.pro` | Gold Tooth | This gold tooth used to belong to Jules, but it's yours now. |
| 430 | `00000430.pro` | Howitzer Shell | A 75mm Howitzer shell. The casing has mostly corroded away, but you can make out these letters. EXP. 9-25-98. |
| 431 | `00000431.pro` | Ramirez Box, Closed | This is the box you received from Big Jesus Mordino to give to Ramirez at the Stables. |
| 432 | `00000432.pro` | Ramirez Box, Open | This is the box you received from Big Jesus Mordino to give to Ramirez at the Stables. It is open, and you have removed the Jet that was inside. |
| 433 | `00000433.pro` | Mirrored Shades | This is a pair of fashionable and deadly-looking mirrored shades. Just having them in your inventory makes you feel cool. |
| 436 | `00000436.pro` | Deck of Cards | A standard set of playing cards. |
| 437 | `00000437.pro` | Pack of Marked Cards | Every card in this set is a red queen. Must make for some boring games. |
| 439 | `00000439.pro` | Pocket Lint | Some fuzz you found in a pocket. |
| 440 | `00000440.pro` | Bio Med Gel | A jar of Bio Gel used in the biomedical field. |
| 441 | `00000441.pro` | Blondie's Dog Tags | These dog tags have the owner's name scratched off and the name "Blondie" scrawled on the back. Beneath the name is the number "11." |
| 442 | `00000442.pro` | Angel-Eyes' Dog Tags | These dog tags list the owner as "Angel-Eyes." Beneath the name is the number "16." |
| 443 | `00000443.pro` | Tuco's Dog Tags | These foul-smelling dog tags list the owner as "Tuco Benedicto Pacifico Juan Maria Ramirez," followed by the number "27." |
| 444 | `00000444.pro` | Raiders Map | This is a crumpled map of pre-war Northern California. Reno and the surrounding caravan trails are outlined in red, and there is a red "X" far to the east of Reno with "Raiders" scrawled beneath it. |
| 445 | `00000445.pro` | Sheriff's Badge | A Badge worn by the Sheriff of a town. |
| 446 | `00000446.pro` | Vertibird Plans | A set of plans for building Vertibirds. |
| 447 | `00000447.pro` | Bishop's Holodisk | This holodisk contains incriminating information on Bishop's secret deal with NCR. Apparently, Bishop hired mercenaries to attack Vault City in the hopes that Vault City would turn to NCR for military aid. This holodisk is audio only and has no text data. Vault City might be interested in this. |
| 448 | `00000448.pro` | Account Book | This account book lists a series of monthly payments made to the mercenary band from the Bishop Family in New Reno. The payments depend heavily on how much "pressure" the mercenaries put on Vault City. Vault City might be interested in this. |
| 449 | `00000449.pro` | Unused | Unused |
| 450 | `00000450.pro` | Torn Paper 1 | You see a piece of paper with writing on it. It looks like it's part of a code of some sort. Unfortunately, the code is incomplete. If only you could find all three pieces. You can make out the following: 1. Physics Password KSLJ, 2. Chemistry Password TIU, 3. Biology Password INTL |
| 451 | `00000451.pro` | Torn Paper 2 | You see a piece of paper with writing on it. It looks like it's part of a code of some sort. Unfortunately, the code is incomplete. If only you could find all three pieces. You can make out the following: KJ: Ken-Lee-9, ASPO- Lo-S, VR- Dnky-Pnch- |
| 452 | `00000452.pro` | Torn Paper 3 | You see a piece of paper with writing on it. It looks like it's part of a code of some sort. Unfortunately, the code is incomplete. If only you could find all three pieces. You can make out the following: 7313, hi-S12908, 98790 |
| 453 | `00000453.pro` | Password Paper | This is three pieces of a single page that, when assembled, reveal three passwords. The text reads: 1. Physics Password KSLJKJ: Ken-Lee-97313, 2. Chemistry Password TIUASPO- Lo-Shi-S12908, 3. Biology Password INTLVR- Dnky-Pnch-98790 |
| 454 | `00000454.pro` | Explosive Switch | This toggle switch is the final conponent of an explosive device. |
| 457 | `00000457.pro` | Hubologist Field Report | A field office report for the membership drive in the True Faith. |
| 458 | `00000458.pro` | M.B. Holodisk 5 | The label reads: Message to CHQ- Confidential. |
| 459 | `00000459.pro` | M.B. Holodisk 1 | The label reads: Radio Transmission log checkpoint 1. |
| 460 | `00000460.pro` | M.B. Holodisk 2 | The label reads: Radio transmission log checkpoint 2. |
| 461 | `00000461.pro` | M.B. Holodisk 3 | The label reads: Radio transmission log checkpoint 3. |
| 462 | `00000462.pro` | M.B. Holodisk 4 | The label reads: Radio transmission log research team. |
| 463 | `00000463.pro` | Evacuation Holodisk | The label reads: Base evacuation notice. |
| 464 | `00000464.pro` | Experiment Holodisk | The label reads: Research log. |
| 465 | `00000465.pro` | Medical Holodisk | The label reads: Medical log. |
| 466 | `00000466.pro` | Password Paper | A sheet of folded paper with the word "TCHAIKOVSKY" written on it. |
| 468 | `00000468.pro` | Smitty's Meal | It's some kind of salad and a sandwich made out of what you think is Brahmin meat covered in a thick dark yellow sauce. There is also a strange pickled green vegetable on the side. |
| 470 | `00000470.pro` | Ball Gag | A questionable sexual device. If you need to ask, you don't want to know. |
| 471 | `00000471.pro` | "The Lavender Flower" | It appears to be some kind of romance novel written by Dorothy Rixon. The cover has a woman laying on a bed surrounded by a hundred flowers. |
| 472 | `00000472.pro` | Hubologist Holodisk | This holodisk is labeled "Hubology: The Truth Behind the Lies". It looks like it works with your PIPBoy. |
| 474 | `00000474.pro` | Daisies | A flower pot of daisies. Aren't they nice? |
| 475 | `00000475.pro` | Unused | Unused |
| 476 | `00000476.pro` | Enlightened One's Letter | A report addressed to AHS-9 in San Francisco. It's really dry reading- tables of oppressive adjustments, expense reports, and other twaddle not worth wasting your time on. |
| 477 | `00000477.pro` | Broadcast Holodisk | The label reads: "Galaxy News Network." |
| 478 | `00000478.pro` | Sierra Mission Holodisk | The label reads: "Mission Statement." |
| 479 | `00000479.pro` | NavCom Parts | These are computer parts that look like they slot into the interior of a machine. The fact that they say "Poseidon Oil" and "Navigational Computer" on them lead you to believe they fit into a Poseidon Oil Navigational Computer. |
| 483 | `00000483.pro` | Fallout 2 Hintbook | Well, THIS would have been good to have at the beginning of the goddamn game. |
| 484 | `00000484.pro` | Player's Ear | This is your ear. The Masticator bit it off during the fight and spit it on your unconscious body. If you are reading this, it probably means you will be reloading soon. Of course, it is possible this item has some special value. |
| 485 | `00000485.pro` | Masticator's Ear | This is the Masticator's ear. You bit it off after pummeling him senseless. Congratulations on beating him. He's one of the toughest NPC's in the game, especially when you don't have any weapons or armor. |
| 487 | `00000487.pro` | Note from Francis | "Zaius, Marcus isn't doing anything about that mutant-hater Jacob and his damned conspirators. First they disable the air purifier... and then what? I found their secret meeting tunnels. I figure a body or two down there could implicate Jacob. If you want in, let me know. And burn the damn note this time!" -Signed, Francis. |
| 488 | `00000488.pro` | K-9 Motivator | This is the drive mechanism for a K-9 series robot. |
| 490 | `00000490.pro` | NCR History Holodisk | The label reads: "History of NCR." |
| 491 | `00000491.pro` | Mr. Nixon Doll | You see a small doll with a big red nose. For some reason, you don't trust this seemingly-innocent child's toy. |
| 492 | `00000492.pro` | Tanker Fob | An encoded passkey that provides access to high security areas. |
| 493 | `00000493.pro` | Teachings Holodisk | The label reads: "Hubologist Teachings." |
| 494 | `00000494.pro` | Kokoweef Mine Scrip | This is a small chit probably used to pay Kokoweef Mine Workers. |
| 495 | `00000495.pro` | Presidential Access Key | This access key has what looks like the presidential seal on it. It appears as though it's used to gain Presidential level access to computers. |
| 499 | `00000499.pro` | Pip Boy Lingual Enhancer | This Pip Boy lingual enhancer consists of a storage holodisk, a microfilament cord, headgear, and an optical sensor that is placed over the user's right eye. When used, an optical flash transmits an entire dictionary into the user's memory, permanently improving the user's speech skills. |
| 503 | `00000503.pro` | Blue Memory Module | A read only computer memory module containing medical information. This module details charisma enhancements. |
| 504 | `00000504.pro` | Green Memory Module | A read only computer memory module containing medical information. This module details perception enhancements. |
| 505 | `00000505.pro` | Red Memory Module | A read only computer memory module containing medical information. This module details strength enhancements. |
| 506 | `00000506.pro` | Yellow Memory Module | A read only computer memory module containing medical information. This module details intelligence enhancements. |
| 507 | `00000507.pro` | Decomposing Body | This is a partially decomposed body of a humanoid creature. |
| 508 | `00000508.pro` | Rubber Doll | An inflatable rubber sex doll. This model is called "Tandi". |
| 509 | `00000509.pro` | Damaged Rubber Doll | This inflatable rubber sex doll has a torn seam. It's obviously been well used. |
| 516 | `00000516.pro` | Pip Boy Medical Enhancer | This Pip Boy medical enhancer consists of a storage holodisk, microfilament cord, headgear, and an optical sensor that is placed over the user's left eye. When used, an optical flash transmits a dictionary of physician skills and know-how into the user's memory, permanently improving the user's doctor skill. |
| 519 | `00000519.pro` | Bottle Caps | These are worthless bottle caps. You've heard that at one time they were used as money, though you suspect its only a story. |
| 523 | `00000523.pro` | Survey Map | It looks like a geological survey map of the Bay area. |

