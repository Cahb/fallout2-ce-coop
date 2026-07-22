---
name: item-pid-glossary
description: "ITEM_GLOSSARY.md = PID decoder (531 item protos); PID==items.lst line index, name=msg{PID*100}"
metadata: 
  node_type: memory
  type: reference
  originSessionId: 7b9f5982-a111-4da9-9744-2d678fb46707
---

ITEM_GLOSSARY.md (repo root) is the item analog of [[dialog-headless-plan]]'s sibling MAP_GLOSSARY.md — a PID→name/description decoder for the 531 item protos, grouped by item type (Weapon/Ammo/Armor/Drug/Container/Key/Misc). Useful when a raw PID shows up on the wire, in saves, or in code.

**Authoritative mapping (verified across all 531, zero mismatch):**
- Item protos live inside `FO2/master.dat` (loose `proto/items/` is empty). Extract via DAT2 reader: footer `<I` at [-8:-4]=treeSize, tree at `len-treeSize-8`; per entry = nameLen(4)+name+comp(1)+realSize(4)+packedSize(4)+offset(4); comp==1 → zlib. `.pro` headers are BIG-endian.
- **PID == 1-based line number in `proto/items/items.lst`** (all items have object-type byte 0x00, so full 32-bit PID == line index). The `.pro` FILENAME number is NOT the PID (list is reordered).
- **name = `pro_item.msg` `{PID*100}`, description = `{PID*100+1}`** (msg is cp1252).
- `.pro` header offsets: PID@0x00, text_id@0x04 (==PID*100), item_type@0x20 (0 armor,1 container,2 drug,3 weapon,4 ammo,5 misc,6 key).

Generator + DAT extractor scripts were one-off in scratchpad; re-derive from the rule above if needed. ~40 protos (dev/boss weapons) have a name but blank description — faithful, rendered as `—`.
