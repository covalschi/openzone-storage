# Review of the storage mod and its bridge side: leaks, holes, desyncs, duplicates

Read on 2026-09-26, every file of `OpenZone_Storage` (35 scripts, 12 965
lines), the bridge's `storage-*.js` (1 771 lines) and the core's
`OZ_BridgeClient` where the storage leans on it. Everything below is read out
of the code as it stands; where a claim was measured it says so, and where it
needs measuring it says that too.

The yardstick is the design's own (§7, §9): **losing is acceptable,
duplicating is not** -- and, from the proxy design (§8), the server is the
only arbiter. A finding is ranked by how close it comes to breaking one of
those two.

The findings of the 2026-09-25 review that are still open are listed at the
end rather than repeated.

---

## A. Holes -- what a client can do that it should not

### A1. Any box on the map opens for anyone who names its network id -- HIGH

`OZS_Proxies.OnWire` (RPC_PX_OPEN) takes an anchor by network id, finds it
with `GetObjectByNetworkId`, and opens the session. Nothing on the server
asks how far the player is from that anchor -- not at the open, not at any
operation after it. The distance check lives only in the client's action
condition (`CCTObject(UAMaxDistances.DEFAULT)`), which a crafted RPC does not
go through.

A box is an announced entity, so its id is known to any client that has ever
had it in its bubble. A modified client therefore opens any box it has seen
from anywhere on the map, and `Out` puts the contents into the player's own
inventory (`Asked`) or at their feet (`GROUND`). Cheat menus that send
arbitrary RPCs exist for this game.

Fix: a distance rule on the server. At `Open`/`Join`, refuse when the player
is farther from the anchor than `UAMaxDistances.DEFAULT` plus slack; in
`OZS_Session.OnFrame`, drop a watcher who has walked away (`No` + `Gone`,
the same pair a session end sends). The stash's `StashMaxDistance` is the
precedent, and the proxy stands still beside the player anyway, so a rule of
a few metres costs the honest player nothing.

### A2. A legacy physical stash opens its owner's record for whoever names it -- LOW

The same handler falls through `OZ_StashAnchor.Cast` to `OZ_StorageBox.Cast`,
and an `OZ_PersonalStash` entity is a storage box: naming one opens
`real.OZS_GetId()` -- somebody else's pair -- as a session. New stashes are
never made any more, but a world updated from an older build can still have
physical stashes standing in it until the boot sweep gets them, and the
vicinity filter that hides them is client-side only.

Fix: in `OnWire`, refuse `OZ_PersonalStash.Cast(real)` unless
`OZS_OwnerUid()` is the sender's uid.

### A3. A flood of operations makes the server restream the whole box -- LOW/MEDIUM

Three paths answer a client with `Restart()`: a stale version, a full queue
(`MAX_WAITING`) and a repeated RPC_PX_OPEN. A restart is a fresh snapshot
(see D1 for what that costs on one frame) and every row of the box again over
the wire. Nothing limits how often one client can trigger it, so a client that
sends sixteen operations a frame, or opens in a loop, has the server
re-serialising a thousand-item box continuously.

Fix: a minimum interval between restarts per watcher (a second is plenty);
past it, refuse without restarting.

## B. Duplication and loss -- the record against the world

### B1. With `WaitForRecord`, the hand-over is released by the FIRST reply, not its own -- MEDIUM

`OZS_Session.OnCommitted` calls `ReleaseHandover()` before anything else,
whatever letter the reply answers. Under the one-turn-at-a-time rule those
are the same thing -- except for the operations that post more than one
letter. `OZS_Boundary.Across` posts three (the step-aside move, the put-in,
the take-out) and holds its hand-over on the third while the first two are
still in flight. The reply to the FIRST letter releases it: the item goes to
the player before the `Left` that drops it from the record has landed. If
that third letter is then refused or lost, the item is in the player's
inventory and in the record -- the exact window `WaitForRecord` exists to
close.

Fix: release only when the flight that carried the hand-over has answered --
simplest, when `m_Flying` reaches zero after the decrement; or tag the
hand-over with the serial of its letter and match it in the reply.

### B2. A sort writes every re-celled root with `flip = 0`, against a layout planned with the flipped footprint -- MEDIUM

`OZS_Sorter.Plan` measures each root with `OZS_Ops.SizeOf`, which swaps
width and height for an item lying turned (that was the fix of 2026-09-26
for the rag). `OZS_Records.WriteDescriptor` then writes the planned cell and
forces `flip = 0` (line 175). The restore creates the item unturned in a
cell the planner reserved for it turned: a rag planned as three wide and one
tall comes back one wide and three tall, over two neighbours. The engine
refuses the cell, `Create` falls back to any free cell or to a stand-in, and
a stand-in root is parked as `no_room`.

Every sort with a turned item in the box loses that item to the parked list
(recoverable by an admin, invisible to the player). The stand test that
confirmed the sort had no turned items in it.

Fix, one line: keep `loc.GetFlip()` in the descriptor instead of forcing 0.
Alternatively plan with the unturned size -- either way both sides must
measure the same rectangle.

### B3. A split creates a NETWORKED entity inside the unannounced authority -- MEDIUM, needs measuring

`OZS_Ops.Split` calls vanilla's `SplitItemToInventoryLocation` /
`SplitIntoStackMaxToInventoryLocationEx` on the authority's item. All three
vanilla bodies (itembase.c:1985, :1872; magazine.c:206) create the new stack
with `GameInventory.LocationCreateEntity(dst, ..., ECE_IN_INVENTORY,
RF_DEFAULT)` -- a registered, announced entity -- as a child of a container
the network has never heard of. The mod's own rule, written in
`OZS_Records.ReadRoot`, is that everything built into an authority must be
LOCAL, "a networked child of an unannounced parent is a contradiction the
clients resolve badly". And when that stack later goes OUT, `Handover` calls
`RemoteObjectTreeCreate` on an entity that is already registered.

What the engine does with such a child is not known: the split was watched
through the proxy, which is told by rows and would not show a ghost. Measure
before deciding: log `made.GetNetworkIDString()` after a split. If it is not
`00`, do the split by hand -- `LocationCreateLocalEntity`,
`TransferItemProperties`, the quantity or the cartridge loop for a magazine
-- so the authority's rule holds.

### B4. The bridge going down mid-session (finding 1 of 2026-09-25) -- still open

`OZS_Commit.Ready` fails the session and `Fail` discards the authority with
everything in it. Items put IN since the last confirmed turn are lost; items
taken OUT are duplicated unless `WaitForRecord` is on. Two remarks beyond the
earlier review:

- `WaitForRecord` is measured at about 100 ms and the owner has felt it on
  the stand. Shipping with it ON closes the duplicate half for good; the
  default in `LoadDefaults` is still `false`.
- Discarding is the design's choice, not a necessity. The authority could be
  kept alive with its watchers dropped and its record written whole when the
  bridge answers again -- a crash costs one turn, a bridge restart would then
  cost nothing.

### B5. Two letters for one operation (finding 3 of 2026-09-25) -- still open, and now known to be concurrent

`OZ_BridgeClient.Fly` posts every call at once (`s_Ctx.POST` per call, an
`s_InFlight` list, no serialisation), so the two letters of `Combine` and of
`Swapped` are two concurrent HTTP requests and the bridge applies them in
arrival order. Each was numbered against a different picture of the record.
The `expect` check refuses a mismatch of class names; it cannot see the case
where the neighbouring root is of the same class, which is exactly the case
of two piles of the same ammunition.

### B6. Two servers on one bridge (finding 4 of 2026-09-25) -- still open.

## C. Desyncs -- what the player sees against what the authority holds

### C1. A container put into the box arrives on every screen EMPTY -- MEDIUM

`OZS_Boundary.In` moves the player's item, whole subtree and all, then tells
the watchers with `TellAdded(e)` -- which describes ONE entity
(`Describe` builds one row). The player's own client has just had the real
tree deleted by `RemoteObjectTreeDelete`, so the proxy builds a fresh, empty
backpack from the row; every other watcher does the same. The contents exist
in the authority and in the record, and appear on screen only after a
restream (a stale, a refusal, a reopen). A player who puts a full backpack
into the box and opens it there sees nothing in it. `Across` step 2 has the
same shape.

Fix: `TellAdded` sends the flattened subtree, parents before children, as one
`CH_ADDED` per node. `OZS_Mirror.Add` already resolves a row's parent by
handle, and `Change(CH_ADDED)` forgets-and-rebuilds each handle, so the
sequence is idempotent as it is.

### C2. One failed fill strands the player until they relog -- MEDIUM

When the open job fails after `Join` accepted the watcher ("too many
unreadable roots", the bridge refusing, a timeout), `OZS_OpenJob.Fail` leaves
the authority CLOSED and `OnOpenFailed` only notifies. The watcher stays; its
`OnFrame` waits for OPEN for ever; the client's `OZS_Mirrors.m_Waiting` is
never cleared because no BEGIN and no GONE ever come. The next press of the
button reaches `Join`, finds the watcher, calls `Restart()` and returns --
the branch that re-requests the open runs only for a NEW watcher. That box is
dead for that player until they disconnect (`DropPlayer`).

Fix: in `Join`, when the authority is CLOSED, request the open again for a
re-asking watcher as well; in `OnOpenFailed`, have the session tell its
watchers `No` and `Gone`, which also clears the client's wait.

### C3. A refused open tells the client nothing -- LOW

`OZS_Proxies.Open` returning false (the bridge down, the authority not
creatable) is logged and dropped; the client keeps `m_Waiting` and the player
sees a button that does nothing. Send RPC_PX_NO with `#STR_OZ_ERR_NO_BRIDGE`
even when there is no watcher to send it through -- the message carries the
id in its body and the client matches on that.

### C4. The closing write and `closed` race an admin -- LOW

`OZS_Session.End` posts the absolute rewrite (`/op replace`) and
`ROUTE_CLOSED` in the same frame; they are concurrent requests. If `closed`
lands first, an admin's `give`/`rollback` in the gap is accepted, and the
session's rewrite that follows makes a new `live` version over it -- the
"admin operations are overwritten by a session close" the owner reported,
narrowed to a millisecond window. Post `closed` from the reply of the closing
write, or let the replace letter carry `close: 1` and have the bridge mark
the box closed in the same transaction.

## D. Leaks and cost

### D1. Handles are a linear scan, and the snapshot asks for one per node -- MEDIUM

`OZS_Authority.Handle` walks `m_Items` to find the entity; `Index` calls it
for every node, and `Snapshot` calls it again for every node and every
parent. For a box of a thousand items that is on the order of a million
steps in Enforce, on ONE server frame, every time a watcher starts or
restarts its stream (and A3 lets a client trigger that at will). A frame
hitch of that size is felt by every player on the server. The owner's
verdict on the live server's lag was about exactly this kind of synchronous
work.

Fix: an O(1) handle on the entity itself -- an `int` field on a modded
`EntityAI`, assigned once by `Handle` -- or a map keyed by the handle for the
reverse direction. The parallel arrays were chosen because "an EntityAI is
not a hashable key"; the entity carrying its own number needs no key at all.

### D2. `OZS_Watcher.Player()` walks `GetPlayers()` on every message -- LOW

`Say` calls `Listening()`, then `Player()`, then `Who()` -- three walks with
an array allocation each -- per message, and `OnFrame` calls `Alive()` once
per watcher per frame. Resolve the `Man` once per `Say`, and once per frame
for the liveness test.

### D3. `FreeSpot` asks the engine about every cell -- LOW

Up to two orientations times the whole grid (a thousand `LocationCanAddEntity`
calls on a full large box), each a native occupancy scan. It runs for a
put-in with no cell named -- a drop on the container's header -- into a
nearly full box. Ask the occupancy map first (`Clear` over the rectangle) and
confirm only the candidate with the engine.

### D4. Statics that outlive the mission -- LOW

- `OZS_Authority.s_Live` is never reset. A session that reaches
  `EndAll` with a turn in flight returns early from `End` (`m_Closing`) and
  never discards, so its `OZS_AuthRec` survives the restart with a dangling
  box -- one dead row per such restart.
- On the client, `OZS_Mirrors.s_Inst` survives a disconnect; its mirrors then
  hold null containers. Harmless today, but nothing says so on purpose.

Add `OZS_Authority.Reset()` beside `OZS_Proxies.Reset()` in
`OnMissionFinish`, and a client-side reset in `MissionGameplay`.

### D5. A proxy keeps table rows for children deleted with their parent -- LOW

`OZS_Mirror.Forget(handle, true)` deletes the container; the engine takes its
children; their rows stay in `m_Handles`/`m_Items` reading null. `Forget`
could sweep nulls while it is there.

## E. Admin and lifecycle -- what the proxy left behind

### E1. The admin's live `close` is dead -- MEDIUM

`OZS_Controller.AdminCommand("close")` reads the PLACED box's state, which
under the proxy is always CLOSED, and answers "not open". An admin can no
longer end a session to free a box for a rollback, a give or an unpark --
and every one of those is refused while the box is `open` in SQL. Route the
command to the session: `OZS_Proxies.Find(id)`, tell the watchers `Gone`,
`End()`.

### E2. The admin's `remove` deletes a box that is in use -- MEDIUM

The same reading (placed box CLOSED) lets `remove` delete the anchor while a
session is live. The session goes on: players keep taking and putting through
a box that no longer exists, the turns keep writing into a record SQL has
marked `removed` (`applyOps` and `replaceRoots` never check the status), and
the final `closed` is refused as "unknown box". Nothing duplicates, but an
admin who removed a box would not expect it to keep trading. Refuse `remove`
while a session exists for the id, and have the bridge refuse turns on a
`removed` box.

### E3. `AutoCloseSeconds` no longer applies to anything -- MEDIUM

`AutoCloseTick` walks the controller's register, which holds placed boxes
only (an authority returns early from `EEInit`), and a placed box is never
OPEN. A session has no idle rule for its WATCHERS -- only for the moment the
last one leaves (`ProxyIdleSeconds`). A player who opens a box, leaves the
panel open and goes for dinner holds the box `open` in SQL until they
disconnect, and (with E1) no admin can do anything about it. Give the session
the idle rule the setting describes: `Touch` already stamps the time; when it
is older than `AutoCloseSeconds`, drop the watchers (`No` + `Gone`) and let
the session end.

### E4. Every crossing writes two audit rows, and a nested move writes a `take` -- LOW

`EECargoOut` on the authority fires for the take-out's own move, so each
take-out is a `take` and an `out`; each put-in a `put` and an `in`. A move
from the box's cargo into a container standing inside the box is an
`EECargoOut` of the box too, and is written as a `take` of an item that never
left. The history page reads as twice the traffic it had, with departures
that were rearrangements. Silence the cargo-event audit on authorities (the
explicit `Log("out")`/`Log("in")` already say it) and keep it for the placed
boxes of the boot reconciliation.

### E5. Consecutive sessions share one `live` version in SQL -- LOW

`applyOps` forks a version only when the current one's source is not
`live`; `markClosed` changes the box's status and nothing else. So the first
turn of the next session mutates the previous session's version in place,
and "one version per session" holds for the first session only. A rollback
to "before this session" is then impossible, which is the one thing an admin
reading the history wants. On `/v1/storage/closed`, retire the version:
`UPDATE storage_versions SET source = 'session' WHERE id = current AND
source = 'live'`.

## F. Housekeeping before this ships

- `FakePingMs` / `OZS_Late`: a held `ScriptRPC` does not survive the frame it
  was built in (measured 2026-09-25: the box stopped opening, clients
  re-requested in a loop). The setting is still there with a warning. Either
  delay the OPERATION rather than the built message, or refuse a non-zero
  value outside the diag build. A warning is not a guard.
- `DebugLog = 1` on the stand's profile; the per-drag `OZS_Say` lines in
  `OZS_Player` are behind it, so they cost nothing off.
- `OZS_Settings.Load`, line 225: `s = s;`.
- `OZS_Const`, the RPC block: "every one of these travels on the ANCHOR
  object" -- they ride on the player and on `DayZGame.Event_OnRPC` now.
- Stand-only verbs (`drift`, `vanish`, `persist`, `asother`) live in
  `OpenZone_Storage_Bridge`, which is `server_only` on the stand and absent
  from `packaging/` -- not shipped, as intended.
- Nothing is committed: 28 files in this repository, 3 in the bridge.

## G. Checked and sound

- File names on the bridge side are regex-guarded (`Xchg.closeName`,
  `opName`, `discard`); no path reaches the file system unchecked.
- A turn is one SQLite transaction; a refusal is proof nothing was written;
  the `expect` check refuses a drifted numbering before anything is applied.
- The closing write is sourced from the container, never from the order.
- `ByHandle` refuses an entity that has left the box; a handle is not a pass.
- A box cannot go into a box; an item cannot swallow its own box; `In` and
  `Across` take only what the player carries or can reach; `Out` puts only
  into the player's own hierarchy or at their feet.
- Scratch objects of the restore carry `ECE_NOPERSISTENCY_WORLD`.
- `OZS_Authority.Empty` lowers `Releasing` before the deferred deletions
  fire, but `Resort` sets the state to CLOSED in the same frame, so
  `OZS_Live()` is false when `EECargoOut` runs: no phantom takes. It holds by
  the state, not by the flag -- worth knowing if either moves.
- A session cut off by the mission finish leaves SQL `open`; the boot rule
  (`PostClosedAtBoot`) closes it, and the record is at the last confirmed
  turn.
- Changes queued during a stream are applied idempotently after it, whichever
  side of the snapshot they fell on.
- The bridge crashing AFTER committing a turn costs nothing: the record has
  the turn, an item out is with the player, an item in is rebuilt once.

## Order I would take them in

1. **A1** -- one distance check; the only finding a stranger can exploit.
2. **B2** -- one line, and it loses items to the parked list on every sort.
3. **C1**, **C2** -- small, and both are "the box does not work" to a player.
4. **B1** -- small; it is the hole `WaitForRecord` was turned on to close.
5. **E1**, **E2**, **E3** together -- the session's lifecycle from the
   admin's side, one batch.
6. **D1** -- the frame hitch, before the first big box on the live server.
7. **B3** -- measure first; the fix is a page if the measurement says so.
8. The rest, and the four still open from 2026-09-25.

---

## What was done the same day

Every fix was built, booted on the stand and exercised there through the
stand's own verbs; the bridge side has tests for its part (`test/storage-store.mjs`,
107 assertions; the whole suite 21/21).

| # | Status | How it was checked |
|---|---|---|
| A1 | fixed | `proxy do=open` from 66 m: "too far from the box"; from 1.4 m: opens; teleport 20 m away with the session open: "walked away … let go", watchers 0 |
| A2 | gone with the legacy stash | `OnWire` refuses any stash entity as an anchor; the physical stash and its lifecycle are deleted |
| A3 | fixed | a burst of 40 moves (24 over the queue) produced ONE restream, not 24 |
| B1 | fixed | cross-boundary swap: `flying 3`, the item reached the hands after the third reply |
| B2 | fixed | a bag moved turned, then a sort: "parked 0", the bag came back FLIPPED at its planned cell |
| B3 | measured, then fixed | vanilla's split made netid `019766` inside the authority; `SplitLocal` makes `00` (ammo 13→7+6, rag 6→3+3) |
| B4 | open (owner's decision) | `WaitForRecord` default; the authority discarded when the bridge dies mid-session |
| B5 | fixed | combine: one letter "rewrite 1 drop 1"; swap: "rewrite 2 drop 0" |
| B6 | fixed | `open_by` on the box: the holder re-opens, another server is refused and told `held_by` at boot |
| C1 | fixed | a bag with contents put in: the client's mirror tree equals the authority's (157/157), the bag shows `tree 2` |
| C2 | fixed | a failed fill tells the watchers `No`+`Gone`, a re-asking watcher requests the fill again |
| C3 | fixed | a refused open reaches the client as `RPC_PX_NO` with the box id; the wait ends |
| C4 | fixed | the closing letter carries `close: 1`; SQL closed in the same transaction |
| D1 | fixed | the handle lives on `ItemBase`; a 158-row snapshot takes 0.99 ms instead of 5.98 ms |
| D2 | fixed | `Player()` keeps and checks its last answer |
| D3 | fixed | `FreeSpot` walks our map first, the engine confirms; a rag with no cell landed at 19,9 |
| D4 | fixed | `OZS_Authority.Reset`, client `OZS_Mirrors.Reset` at mission finish |
| D5 | fixed | `Forget` drops null rows |
| E1 | fixed | admin `close` ends the session: "ok the session is ending; 1 watcher(s) were sent away" |
| E2 | fixed | admin `remove` refuses a box in use; `EEDelete` of a placed box ends its session |
| E3 | not a regression | the owner's rule was "the close waits while somebody is looking"; a session ends when its last watcher leaves |
| E4 | fixed | the session's history holds `in`/`out`/`open` only, no `take`/`put` |
| E5 | fixed | `closed` retires the `live` version; three sessions in a row made three versions (3977, 3983, 3984) |
| F | done | `OZS_Late` gone, the fake ping holds the operation (`tune ping=3000`: the move ran after the delay); settings v6 without the five dead knobs; `s = s;` gone; the RPC comment fixed |

Also found and fixed on the way: an item taken off a weapon slot into the
cargo was measured as 1x1 (`SizeOf` knows nothing outside a cargo), so an AKM
of 8x3 was accepted on row 48 of a fifty-row box; `SizeFor` now falls back
to the config size.

And the whole old scheme is gone, as the owner asked: the close job, the
auto-close, the boot close of a box saved with cargo, the physical stash
and its sweep, the stand verbs that opened or filled a placed box. A placed
box is always CLOSED and empty.

Two engine facts were measured and written into the `dayz-modding` skill:
`modded class EntityAI` is refused ("Engine class cannot be modded"), and
`Weapon_Base` descends from `ItemBase` (declaring the same field on both is
"Multiple declaration").

Not committed, not pushed.

### Found by the owner an hour later: the cross-boundary swap's three letters

Six canteen-for-rag swaps in a row through a hoodie, and the sixth answered
"the record did not take the turn". The log: `root 114 holds Rag, the letter
says Canteen`. `Across` posted its three steps as three letters, the core's
client sends every call at once, and the bridge applied the drop of position
114 before the rewrite of position 114 -- the identity check refused it, the
session repaired with an absolute rewrite (116 roots, 163 entities, nothing
lost or doubled), and the swap stopped half way. Before B1 the same race was
repaired silently, with the item already handed over.

Fix: `OZS_Session.m_Batch`. `Across` opens a batch, its three steps write
into one letter, and the letter is posted once at the end; `HoldHandover`
accepts a hold while a batch is open. The safety argument of §7 holds
unchanged: the letter goes out after the put-in's move (record last) and
before the take-out's hand-over (record first), so at every instant exactly
one item is unsaved and none is doubled. Verified: four swaps in a row, each
one letter `rewrite 1 drop 1 add 1`, no refusal, the item in the hands after
each.

### The evening of 2026-09-26: three gestures the owner asked for

"What else does not work: swapping between the ground and the box; stacking
from the inventory or the ground into the box and back (one round on the
ground or in the inventory, the same round in the box); and Alt+click to move
an item from the box into the inventory and back."

| gesture | what was wrong | what was done | verified on the stand |
|---|---|---|---|
| ground <-> box swap | the screen DOES offer the swap for a loose item (`GameInventory.CanSwapEntitiesEx` answers true for a ground item and an item in the proxy -- measured); it was `Across` that refused: a ground location has no parent, and step 3 named the parent by network id | `Across` takes a loose item: step 3 goes out with the GROUND type alone and the box's item lands at the player's feet. On the client, a drop vanilla finds nothing for (`Icon.PerformCombination` with NONE) is routed as an `Across` too, as a fallback | an apple off the ground onto ammo pile #4: `move #4 to 0,2`, `taking in Apple, loose on the ground -> 0,3`, the pile out at the feet; one letter `rewrite 1 drop 1 add 1` |
| stacking across the boundary | `CombineItemsClient` was refused on purpose ("a move of a part of a stack is a split") | two operations, one per direction, because the safe order of the record is opposite for each (section 7), as for IN and OUT. No entity crosses: only the contents move, as vanilla's own combine moves them. `OP_STACK_IN` -- the engine's `CombineItems` on the two real entities (the taker is the authority's, the giver the player's, the engine syncs what it loses), SQL last, an emptied giver deleted. `OP_STACK_OUT` -- a two-phase credit (`OZS_Credit`), the hand-over of a take-out for contents: they leave the box's stack, the letter goes (rewrite or drop), the player's stack is credited in the same frame or -- with `WaitForRecord` -- when the bridge has answered; a refused turn restores the giver, which stands in the box until then, at nothing if it gave everything | rags 2 -> box (3 -> 5, the player's stack gone); ammo 20 into a 10/20 pile (10 moved, 10 stayed); out: rag 5 -> the player's 2 (4 moved), ammo 20 -> 10/20 (10 moved), nails 70 onto 10 (the box's stack gone, letter `drop 1`); the same with a pile lying on the ground, both ways; a full receiver refused with `#STR_OZS_NO_STACK` reaching the screen; with `WaitForRecord` and 300 ms of ping: `drop 1` posted while the giver still stood at nothing, the credit given after the reply, no disagreement between box and record, `errors_total 0`; the bridge's history shows `stack_in`/`stack_out` with the amounts, and the closing write (4 roots: apple, rag 3, rag 3, nails 80) equals the box |
| Alt+click | there is no such gesture on a keyboard: `UAUIFastTransferItem`/`ToVicinity` are bound on consoles only (bin.pbo: ps4X, x1X), and the PC's one modifier click, Ctrl+LMB = drop, is written into the five click handlers | the same way: `OZS_QuickMove` at the top of `Icon.MouseClick`, `PlayerContainer.MouseClick`, `ContainerWithCargoAndAttachments.MouseClick2`, `AttachmentCategoriesRow.MouseClick`, `HandsContainer.MouseClick2`; Alt read with `KeyState(KeyCode.KC_LMENU/RMENU)` (`DayZGame` keeps Alt in a private field). A box item -> `TakeInto` (the client proposes a CARGO place, then ANY, else the server decides); the player's own, hands included -> `PutInto` (a cell or a weapon slot the proxy sees, else the authority's choice; never the ground). `PredictiveTakeEntityToInventory`/`ToTargetInventory` hooked as well | by the probe's `alt 1` / `altin Nail` / `altin Rag` (from the hands): op 2 to the hoodie's 1,2, op 3 to 0,0, the rag from the hands to 0,2, all in the box's history. THE MOUSE CLICK ITSELF CANNOT BE PRESSED FROM THE STAND (no mouse tool): Alt+LMB live is the owner's to try |

Also on the way: `OZ_StorageBox.OZS_CountEntities` now skips entities set for
deletion, as `OZS_Records.CountTree` already did -- the emptied giver of a
held credit is deleted in the very frame the bridge's answer is judged. And
the compiler's "Formula too complex": one `Note(...)` with fourteen `+`
terms in the probe; split into statements.

Stand commands added to the client probe for these: `alt <handle>`,
`altin <cls>`, `stackin/stackout <cls>`, `gstackin/gstackout <cls>`,
`gswap <cls> <handle>` (the last one also prints what vanilla's own tests
say about the pair).

Not committed, not pushed. Not re-checked on screen in Ukrainian: the one
new string, `STR_OZS_NO_STACK`.

Later the same evening, after the owner confirmed Alt+LMB works in the game:
a combine between two different open boxes is refused explicitly in
`OZS_Stacking` (it would have named a stack by a network id it does not
have); four dead stringtable keys removed (`STR_OZS_CLOSE`, `STR_OZS_SORTING`,
`STR_OZS_EMPTY`, `STR_OZS_WAITING` -- the old scheme's close and sort, a
duplicate of `COUNT_EMPTY`, a waiting line nothing showed), and the fifth
unused one, `STR_OZS_OUTSIDE`, now carries the out-of-grid refusal of
`OZS_Ops.Move` instead of plain words nobody saw. 44 keys, all referenced.

### Found by the owner's matryoshka in the stash: roots and nesting

The owner hung a hoodie in the stash's Body slot and put rounds into its
pockets. Five ERRORs in the log, all one defect in the record's bookkeeping
of ROOTS -- what stands directly in the box -- when a move crosses into or
out of a container that is itself in the box:

- `In` with a host (`into` != 0) committed `Added -> Add(top)`: the hoodie,
  already a root, was written AGAIN with the round inside -- the record held
  two hoodies for one turn (`2 roots / 3 entities` against the box's `1 / 2`).
- `Move` of a root into the hoodie's cargo committed `Rewrite(oldPosition)`:
  the round, by then inside the hoodie, was serialised as a root once more.
  The "pushed 1 item out of the box" alarm beside it compared ROOT counts,
  which a nesting move lowers by one -- a false alarm.
- `Move` of a nested item out to the grid added it (correctly) while logging
  it as an anomaly.

Every time the count check caught the disagreement and repaired it with an
absolute rewrite, so nothing was lost or doubled in the end -- but between
the bad letter and the repair the record held a duplicate root, and a server
dying in that window would have materialised it on the next open.

Fix: one rule, `OZS_Commit.Settle`, fed with a snapshot taken before the
move (`OZS_Was`: root position, was-it-the-root-itself, class):
root->root rewrite; root->nested DROP the old root and rewrite the host;
nested->root rewrite the old host and ADD; nested->nested rewrite both.
`Added` rewrites the host when the item arrived inside a root the record has.
`Move` counts the whole tree for the pushed-out check. `Swap` takes both
snapshots before anything moves and `MovedPair` settles each with its own.

Verified on the stand (17:00, after the owner's word): in the owner's own
stash, a round `In` into the hoodie hung in the Body slot -- `rewrite 1 drop
0 add 0, the order now has 1 root(s), the box 1`; nails in as a root -- `add
1`; nails moved into the hoodie -- `move #5 Nail to 2,4 of Hoodie_Blue`,
`rewrite 1 drop 1 add 0`, no "pushed out"; back to the grid -- `rewrite 1
drop 0 add 1`; a root-for-root swap -- `rewrite 2`; the record 3 roots / 6
entities equal to the box; `errors_total 0` for the whole run. A container
lying in the box's CARGO cannot be filled at all, and that is vanilla:
`Container_Base.CanReceiveItemIntoCargo` refuses while
`AreChildrenAccessible()` is false, so the bandage aimed at a bag on the
grid was refused with `#STR_OZS_FULL` -- only a container hung in a slot
takes things, which is the stash's whole point.

One blink remains on the client: its LOCAL move of the nails into the
hoodie's pockets "said true" and left the item nowhere, so the mirror
asked for the stash again (5 rows). Correct, one restream; the follow-up is
to recreate the item locally at its new place instead of asking for the
whole box.

### The lag question: the absolute write is a job again

Asked for other sources of server lag, the honest list was: (1) the absolute
letter -- every session's closing write, every repair, the sort's letter --
serialised every root of the box in ONE frame: ~0.2 ms an entity (measured
2026-09-16 on the old, paced close), so ~200 ms for a full Large box, the
same shape of stall as the obfuscated storage mod's synchronous loads on the
live server; (2) the sort planner, one frame, unmeasured on a big box;
(3) the snapshot at open/restream, all rows built at once (~6 ms per 1000)
and then streamed 160 rows a frame; (4) `TellTree` of a big container, one
RPC per node per watcher; (5) the per-turn file written synchronously on the
main thread -- small, but an antivirus on the profile folder would stall
every turn. Not sources: the fill (5 ms a frame, shared by every box),
events (once a second), restreams (once a second at most), the leash, placed
boxes, the RPC and item hooks; `DebugLog` ships off.

Done, at the owner's word ("of course bring it back"): `OZS_WholeJob`. The
absolute letter's file is written root by root on the fill's own budget
(`OpenFrameBudgetMs`), the writer keeping its file open between frames; the
session takes no turn while it runs (operations queue behind it as behind a
letter in flight), does not end (`IsDone`), and refuses a new watcher with
`#STR_OZS_OPENING` while its closing write runs; the letter is posted when
the file is whole (`OZS_Letter.PostWritten`), a closing one ends the session
then (`FinishEnd`). Sessions stay listed until they have ended; the mission's
end finishes a running write at once (`FinishWholeNow`). A job that fails
removes its file and fails the session as a synchronous write did. Verified on the stand: a 104-root box closing -- `written in 5 frame(s),
23.0 ms of work` (the budget's 5 ms a frame; ~0.22 ms a root, as
estimated); its sort -- `written in 5 frame(s), 24.0 ms`, then the refill of
104 in 656 frames; the 3-root stash -- 1 frame, 2 ms; at the server's stop
with a session still open, `EndAll` finished the write at once -- `written
in 1 frame(s), 25.0 ms`. Every letter landed (versions `session` 104/104,
status closed), no `disagree`.

### The anchor as an item, verified

Spawned at the runway: stands upright as the grey locker, has a network id
(the client asked through it, `netid 019452`), health 1 000 000, and the
owner's stash opened under it by the position key with the hoodie and its
rounds. It survived a server stop and start: `OZ_StashAnchor` at the same
spot after the boot, the box beside it counted by the boot exchange.

Found on the way, a stand-sharing hazard rather than a mod defect: while
this stand was down the owner ran the radio stand on the same mission
without the storage mod loaded, and its world save dropped every entity of
a class it did not know -- all three placed boxes were gone at the next
boot. Their records are intact in SQL (the bridge lists them closed); a box
placed again gets a new persistent id, so an admin `restore`/`move` is the
way back to a lost box's roots. A stash is keyed by position and needs
nothing: the locker placed again on the same metre found the kit.

---

Everything above was committed as `f970c34` (the Workshop description within
Steam's 8000-byte limit as `a23cb33`) and pushed on 2026-09-26; the bridge's
`eb86ab5` pushed with it; Workshop item 3803455084 updated with this build
at 14:14Z. "Not committed, not pushed" lines above describe the moment they
were written.

### Evening, live with two players: a sort on a nearly full box parked a rifle

The owner and a second player used a Large box together (both watching,
in and out constantly, items raced -- `no such item` refusals for the
loser, no errors). At 976 of 1000 cells the owner pressed Sort: the
planner packs by name, an AKM late in the alphabet found no 8x3 block in
the new layout, the refill had nowhere to put it and parked the root with
the bridge -- an item lost to a tidy-up, out of a box that held it a
second earlier (`root 239 of 240 (AKM) cannot be read: no room in the
box`). Fixed in source: `OZS_Ops.Sort` counts the cargo roots and refuses
with `#STR_OZS_SORT_FULL` ("The box is too full to sort") when the planner
placed fewer than all of them, moving nothing. Lint clean, not yet built
-- the stand is in use. The parked AKM stays in `storage_parked` for an
`unpark` once a block frees up.

The seeder's `send` stage queues one turn per item and the session's queue
holds sixteen: send in batches of 16 or the rest is refused as stale. The
bridge's `give` needs the box closed in SQL; between two players' sessions
it is, for the twenty idle seconds, which is enough for a retrying loop.

### Evening, second pass: the warm cache, the teardown as a job, the magazine

Three things from the same live session, all in source and lint clean, NOT
BUILT: the stand is still in use by two players and the running server
holds the pbo.

**The warm cache is five minutes.** `ProxyIdleSeconds` is how long an
authority is kept after the last watcher left, and a player who opens the
box again inside that time gets it at once -- no read of the record, no
refill. It was 20 s. The owner's word: five minutes. Applied to the running
stand without a restart (`tune px_idle=300`, answered `idle=300s`), written
into the stand's settings file, and made the shipped default: settings v7,
whose migration gives a file still at the old default the new one and leaves
an admin's own number alone. The price, stated: the memory of idle
authorities, and a box that stays `open` in SQL for those five minutes,
which is where the bridge refuses an admin's give, edit or rollback.

**The teardown is a job.** `OZS_Authority.Discard` (a session's end) and
`Empty` (a sort's refill) called `ObjectDelete` on every entity of an
authority in one frame -- 240 for a full large box -- and the engine pays
for each at the end of that frame. That was the last one-frame burst the
scheme had left once the closing write was paced. Now `OZS_Teardown` lists
the entities once, deepest first, and `OZS_Authority.OnFrame` (from
`OZS_Proxies.OnFrame`) deletes `ReleaseDeletesPerFrame` of them a frame --
50 by default, one budget shared by every teardown running. A discard
deletes the box after the last of them; an empty leaves it standing and
lowers its teardown flag then, so the refill is audited as usual. The sort
therefore runs in two steps: `Resort` shuts the box, starts the emptying
and restarts the screens; the session's `OnFrame` polls the job and asks
for the refill the frame it is done (an entity deleted this frame holds its
cells until the frame ends, and a refill over it would find no room). A
close arriving meanwhile waits for the emptying and then ends with nothing
to write: the sort's letter went first and the record holds the layout. At
the mission's stop `EndAll` finishes every teardown at once, as it does the
closing write. The stand's `tune` verb takes `release=N`.

**The magazine on a weapon in the proxy.** The owner's screenshot: the AKM
with the 75-round drum was drawn without it, while the panel listed the drum
and the proxy held it (`tree 2`). Read from source: a weapon draws its
magazine only when told to -- `Weapon_Base.ShowMagazine`/`HideMagazine`
switch the selection (`SelectionMagazineShow`, or the simple hidden
selection on the weapons that have one), and the game calls them from the
weapon's state machine and from `ForceSyncSelectionState`; `EEItemAttached`
on a weapon only refreshes its property modifiers (weapon_base.c:1116). A
magazine created straight into the slot of a client-local weapon runs
neither. `OZS_Mirror` now calls `ForceSyncSelectionState` on the weapon a
row hangs on (Add), on both weapons a magazine moves between (Place), and
hides it by name when the magazine's row is forgotten (Forget: the deletion
is deferred to the frame's end, so a resync would still see it). The
chamber the same call reads is empty on a proxy and stays hidden, which is
right. Unverified on the stand until the rebuild. The other rifles in the
box were seeded without magazines; nothing is missing from them.

Still open from the same evening: the parked AKM (an `unpark` once a block
frees up), and the second player's VPP super-admin entry, which this
session was not allowed to write -- it is one line in
`profiles/VPPAdminTools/Permissions/SuperAdmins/SuperAdmins.txt`, or the
Permissions manager in VPP's own menu.

### The charge beside the box: a ruined authority emptied the record

The owner set off explosives beside the anchor, next to the first filled box
(a large box of 272 roots). The authority of that box -- the real container
that holds a session's contents -- stands in the placed box's own
coordinates: unannounced, but solid on the server. The blast ruined it, and
`Container_Base.EEHealthLevelChanged` answers RUINED by dropping the whole
inventory on the ground (container_base.c:96). The watchdog shouted 264
times -- 103 nails, 33 ammunition piles, 32 bandages, 30 apples, 20
canteens, 20 bags, 13 rags, 8 rifles, a plum, a pear, a PDA -- `LEFT the
authority and is now in nowhere`, each with the same stack through
`DropAllItemsInInventoryInBounds`. Five minutes later the session's idle
clock ended it, and the closing write did exactly what it is for: it set
the record to what the box held. Nothing. The 264 entities lay in a pile at
the box's coordinates as server-local ghosts, invisible to every client and
bound for the world save; 239 of them were deleted by class before the
wipe, the rest went with the world.

Four guards, all in source, built and booted on the wiped stand:

- **The authority takes no damage.** `SetAllowDamage(false)` right after
  its creation (`OZS_Authority.Create`). It is contents in a container's
  shape, not a thing in the world.
- **A ruined authority keeps what it holds.** `OZ_StorageBox.
  EEHealthLevelChanged` does nothing for an authority but log an error, so
  whatever else changes its health level, vanilla's dump never runs on one.
- **The watchdog acts, on every server.** It used to run behind `DebugLog`
  and only say what it saw. Now an item that leaves an authority without an
  operation is deleted (the record still describes it; the next open builds
  it again exactly once) and the session is told it is compromised. The
  walk it costs is a few pointer hops per item that changes hands, skipped
  outright while no authority stands (`OZS_Authority.Any`).
- **A compromised session writes nothing on the way out.** `OZS_Session.
  Compromised` marks the record as already written, `OnFrame` ends the
  session on the next frame, and the drift check neither compares nor
  repairs: setting the record to what a compromised box holds is the write
  that emptied one. The record stays at its last committed turn.

The stand was wiped on the owner's word -- the world save and the storage
half of the bridge's database (backup `state/bridge.sqlite.before-wipe2-
2026-09-26`) -- and brought up again on this build.

Read out of the same session, not yet explained and to be reproduced by
the probe: a split of a stack lying inside a container hung in a stash's
slot is refused by the engine itself (`CanBeSplit` asks
`CanRemoveEntity`, and it answers no there); a move inside such a container
comes back to the client under the container's own handle and forces a
restream every time; and the owner's "six rags again" -- a stack split in
a box that shows its whole pre-split count once taken back into the
inventory -- did not leave a trace in either log.

### The three open items, reproduced by the probe and fixed

The stand was wiped and rebuilt; the probe client got `pxsplit`, `pxmove
... [into]` and `inv`, the stand verb got `auth do=peek` with every gate a
split asks, `auth do=tree` with the whole handle table, and `clean` (the
player's stack marked disinfected, which in vanilla is cleanness 1,
disinfectitem.c:89). Each item was replayed, the cause read off both logs,
fixed, rebuilt and replayed again.

**A nested item was told to the client under its container's handle.** A
rag moved into a bag hung in the stash was logged by the server as `move #2
Rag to 0,0 of MountainBag_Red` and by the client as `told #1 Rag to 0,0`,
#1 being the bag; the client then moved the bag into itself, read it at
`-1,-1` and asked for the whole box again. A quantity change on the nested
rag landed on the bag, and a stack split off it made the client forget the
bag, with everything in it. The server's own table was right the whole
time (`auth do=tree`: `#2 Rag in #1`). The fault was one line, three
times: `Describe(e, OZS_Authority.Handle(m_Auth, e), ParentHandle(e), r)`.
ParentHandle calls Handle again, and THE ENGINE HANDS THE SECOND CALL'S
RESULT BACK IN THE FIRST ARGUMENT'S SLOT. For a root item ParentHandle
returns 0 before calling anything, which is why root items were always
right, and the full stream (Snapshot, TellTree, both with the parent's
number in a local) never showed it. TellMoved, TellQuantity and TellAdded
now look both numbers up into locals before the call. Replayed: `telling
moved #2 Rag in #1 to 1,0` on the server, `told #2 Rag to 1,0` on the
client, no warning, no restream; a split of a stack inside the bag leaves
the bag standing with the source at its new count and the new stack drawn
where the server put it. Recorded in the skill as an engine rule.

**Splits inside a slot-hung container.** Every gate the split asks is open
there (`peek`: `CanBeSplit true CanRemoveEntity true
LocationCanRemoveEntity true parentReleasesCargo true`), and the server
split the nested stack every time. What the owner saw as refusals was the
same handle fault: the client asked to split the number it had been given,
which was the container's or a stack of one. The right-click split now
also follows vanilla's own rule for WHERE the new stack goes
(itembase.c:2124): the item's own container while it has room, the box's
grid otherwise.

**"Six rags again".** Replayed exactly: a disinfected stack of six into a
box, split, a dirty rag stacked onto the secondary, the secondary split,
the primary taken out. The primary appeared in the shirt as `qty 6/6
clean 1` while the server held `qty 3 cleanness 1`. No duplicate: the
client's COPY was wrong. `RemoteObjectTreeCreate` announces an entity the
network had forgotten, and the copy a client builds starts from the config;
the quantity arrived only with the next synchronised change (a cleanness
change on the server redrew it as three at once). A stack that had never
left the box before showed the right number, its change was still unsent,
which is why the plain split-and-take-out never reproduced it.
`OZS_Ops.Announce` now marks every entity of the announced tree dirty
right after the announcement, and the next frame's sync carries quantity,
wet and cleanness to the copies just made; the three places that announced
(the take-out, the refused put-in, the restore of a returned root) go
through it.

