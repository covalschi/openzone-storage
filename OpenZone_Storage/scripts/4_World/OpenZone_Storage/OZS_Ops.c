// What a player's screen can ask of a box, and what the server does about it.
// Design: docs/specs/2026-09-24-storage-proxy-inventory-design.md §5, §6, §7.
//
// Every operation here is ATOMIC and SERVER-SIDE. The proxy has already shown
// the player its own guess; this is the word that counts, and it is told to
// every proxy of the box afterwards -- including the one that guessed, because
// a guess that happened to be right and the authority's word must not be two
// different things (§8.4).
//
// TWO RULES DECIDE THE ORDER OF EVERYTHING BELOW:
//
//   1. The proxy never decides last and never forbids (§5). A refusal here is
//      the only refusal there is.
//   2. The step that could create a duplicate goes LAST (§7). An item leaves a
//      place that is saved and arrives in one that is not; if the server dies
//      between the move and the write, the item is lost. The other order would
//      duplicate it. Losing is visible and an admin can fix it; a duplicate is
//      invisible and eats the economy.
class OZS_Ops
{
    static void Run(OZS_Session s, OZS_Watcher w, int op, int handle, int other, int netLow, int netHigh, int lt, int slot, int row, int col, int flip)
    {
        if (op == OZS_Const.OP_MOVE)
        {
            Move(s, w, handle, other, lt, slot, row, col, flip);
            return;
        }
        if (op == OZS_Const.OP_COMBINE)
        {
            Combine(s, w, handle, other);
            return;
        }
        if (op == OZS_Const.OP_SWAP)
        {
            Swap(s, w, handle, other, netLow, netHigh, lt, slot, row, col, flip);
            return;
        }
        if (op == OZS_Const.OP_SORT)
        {
            Sort(s, w);
            return;
        }
        if (op == OZS_Const.OP_SPLIT)
        {
            // `netLow` is the kind of split, not a network id: nothing in this
            // operation lives outside the box, so there is no id to carry.
            Split(s, w, handle, other, netLow, lt, slot, row, col, flip);
            return;
        }
        if (op == OZS_Const.OP_XSWAP)
        {
            // The one inside is named by handle, the one outside by network
            // id -- which is why this cannot ride on OP_SWAP's two handles.
            OZS_Boundary.Across(s, w, handle, netLow, netHigh);
            return;
        }
        if (op == OZS_Const.OP_OUT)
        {
            OZS_Boundary.Out(s, w, handle, netLow, netHigh, lt, slot, row, col, flip);
            return;
        }
        if (op == OZS_Const.OP_IN)
        {
            // Coming IN, the item is the player's own and announced, so it is
            // named by its NETWORK id; `other` is the container inside the box
            // it is going into, 0 being the box itself.
            OZS_Boundary.In(s, w, netLow, netHigh, other, lt, slot, row, col, flip);
            return;
        }
        if (op == OZS_Const.OP_STACK_IN)
        {
            // The stack in the box by handle, the player's by network id;
            // nothing else travels, both are read here.
            OZS_Boundary.StackIn(s, w, handle, netLow, netHigh);
            return;
        }
        if (op == OZS_Const.OP_STACK_OUT)
        {
            OZS_Boundary.StackOut(s, w, handle, netLow, netHigh);
            return;
        }
        w.No(handle, "unknown operation " + op.ToString(), s.m_Version);
    }

    // ---- inside the box --------------------------------------------------

    // A cell to a cell, or a cell to a slot, or into a container that is in
    // the box. `other` is the handle of the destination's parent: 0 is the box
    // itself.
    static void Move(OZS_Session s, OZS_Watcher w, int handle, int other, int lt, int slot, int row, int col, int flip)
    {
        EntityAI e = OZS_Authority.ByHandle(s.m_Auth, handle);
        if (!e)
        {
            w.No(handle, "no such item", s.m_Version);
            return;
        }
        EntityAI parent = s.m_Auth;
        if (other != 0)
        {
            parent = OZS_Authority.ByHandle(s.m_Auth, other);
            if (!parent)
            {
                w.No(handle, "no such container", s.m_Version);
                return;
            }
        }
        // "SOME SLOT, YOU CHOOSE" IS A REAL ANSWER AND HAS TO BE GIVEN ONE.
        // See FreeSlotFor: the panel's unnamed variant is what a drop on a
        // weapon rack uses, and it is the only one a player can reach by
        // dragging.
        // "NOT NAMED" IS A SENTINEL, NOT A SIGN. Slot ids are hashes and they
        // are NEGATIVE -- OZ_Weapon_4 is -692829675 -- so testing `slot < 0`
        // treated every real slot as unnamed and threw it away for the first
        // free one. That is exactly what the owner saw: dropping on the third
        // hook filled the first, the fourth filled the second (2026-09-25).
        if (lt == InventoryLocationType.ATTACHMENT && slot == InventorySlots.INVALID)
            slot = FreeSlotFor(parent, e);
        // The screen computes its own destination and so does this: a cell
        // outside the grid once made a proxy accept a move the authority
        // refused, and both calls returned true (measured 2026-09-24).
        if (!Fits(parent, lt, slot, row, col))
        {
            string asked = row.ToString() + "," + col.ToString();
            if (lt == InventoryLocationType.ATTACHMENT)
                asked = "slot " + slot.ToString();
            OZ_Log.Dbg("storage: proxy: #" + handle.ToString() + " " + e.GetType() + " was sent to " + asked + ", which is not a place in this box");
            // As a key, so the player reads it: the screen proposed a cell
            // the grid does not have, and a refusal in words reaches nobody.
            w.No(handle, "#STR_OZS_OUTSIDE", s.m_Version);
            return;
        }
        // An item cannot be put inside itself, at any depth.
        if (parent == e || Holds(e, parent))
        {
            w.No(handle, "an item cannot go inside itself", s.m_Version);
            return;
        }
        // A CELL SOMETHING ELSE IS STANDING ON IS NOT A PLACE, AND THE ENGINE
        // WILL NOT SAY SO.
        //
        // `TakeToDst` onto a taken cell does not refuse: it puts the mover
        // there and THROWS THE SITTER OUT OF THE CONTAINER. Measured against
        // the owner 2026-09-25: the authority went 9 -> 8 -> 7 entities over a
        // few drags while the record stayed at 9, and every one of those drags
        // was a drop onto an occupied cell. Nothing in this mod deleted them;
        // the move itself did it, silently, and the commit that followed only
        // rewrote the root of the item that HAD moved.
        //
        // The record is unharmed -- it still holds all nine, and the next open
        // brings them back -- but the player watches items vanish, which is
        // indistinguishable from losing them.
        // A CELL IS NOT ONE CELL. An item covers a rectangle, and asking only
        // about the cell under the cursor let a drop whose BODY lay over a
        // neighbour through as an ordinary move -- and that move pushed the
        // neighbour out of the container. The whole footprint is asked about,
        // the item's own cells excepted.
        if (lt != InventoryLocationType.ATTACHMENT)
        {
            int mw;
            int mh;
            // The shape it will have in the cell it is going to; see SizeFor.
            bool known = SizeFor(e, flip, mw, mh);
            if (!known)
            {
                mw = 1;
                mh = 1;
            }
            if (!Clear(parent, row, col, mw, mh, e, null))
            {
                // WHO IS IN THE WAY, BY NAME. A refusal reaches the player as
                // four words and reaches nobody at all when the operation came
                // from somewhere without a screen -- and then a perfectly
                // ordinary "that cell is taken" looks like the box being
                // broken. It cost an hour and a wrong theory about the
                // engine's grid height (2026-09-25).
                // The WHOLE rectangle, not the corner cell: what blocks a
                // rifle is usually not standing on the cell the rifle was
                // aimed at.
                EntityAI inTheWay = InTheWay(parent, e, row, col, flip);
                // Nothing in the way and still refused means `Clear` failed on
                // the bounds -- the rectangle runs off the grid.
                string blocker = "nothing; the rectangle runs off the grid";
                if (inTheWay)
                {
                    int br;
                    int bc;
                    Where(inTheWay, br, bc);
                    int bw;
                    int bh;
                    SizeOf(inTheWay, bw, bh);
                    blocker = inTheWay.GetType() + " standing at " + br.ToString() + "," + bc.ToString() + " over " + bw.ToString() + "x" + bh.ToString();
                }
                OZ_Log.Dbg("storage: proxy: #" + handle.ToString() + " " + e.GetType() + " cannot go to " + row.ToString() + "," + col.ToString() + " (" + mw.ToString() + "x" + mh.ToString() + "): " + blocker);
                w.No(handle, "#STR_OZS_TAKEN", s.m_Version);
                return;
            }
        }
        // WHAT THE SCREEN ASKED, AND WHAT THE BOX HELD WHEN IT ASKED. One line
        // per drag, and the count on either side of the move: an item that
        // leaves the box without anybody deleting it shows up here as a drop
        // of one, beside the drag that caused it.
        // EVERY ENTITY, THE NESTED ONES TOO. Counting roots cried wolf: a
        // move into a container standing in the box turns a root into a
        // nested item, the root count falls by one, and "pushed out" was
        // reported for a round put into a hoodie (owner, 2026-09-26). The
        // tree count does not change on any move that stays in the box.
        int held = OZS_Records.CountTree(s.m_Auth) - 1;
        string where = row.ToString() + "," + col.ToString();
        OZ_Log.Dbg("storage: proxy: move #" + handle.ToString() + " " + e.GetType() + " to " + where + " of " + parent.GetType() + "; the box holds " + held.ToString());
        // Where it stood in the record BEFORE the move: a move into or out of
        // a container inside the box changes which root it belongs to, and
        // afterwards the old one is no longer reachable from the entity.
        OZS_Was was = new OZS_Was(s, e);
        InventoryLocation src = new InventoryLocation();
        e.GetInventory().GetCurrentInventoryLocation(src);
        InventoryLocation dst = new InventoryLocation();
        if (lt == InventoryLocationType.ATTACHMENT)
            dst.SetAttachment(parent, e, slot);
        else
            dst.SetCargo(parent, e, 0, row, col, flip == 1);
        // THROUGH THE VERIFIED PLACEMENT, NOT THE RAW CALL. `Put` reads the
        // item back afterwards and says what happened; the bare `TakeToDst`
        // here answered TRUE and left a rifle sitting in the cargo when it was
        // asked for a weapon slot, so the box quietly refused every weapon put
        // on its rack while reporting success (owner, 2026-09-25).
        if (!Put(e, dst))
        {
            w.No(handle, "refused", s.m_Version);
            return;
        }
        // DID THE MOVE COST THE BOX AN ITEM? A move must not change how many
        // things are in the box. When it does, something was pushed out of the
        // container by the move itself, and the player sees it disappear.
        int after = OZS_Records.CountTree(s.m_Auth) - 1;
        if (after < held)
            OZ_Log.Error("storage: proxy: box " + s.m_Id + ": moving " + e.GetType() + " to " + where + " pushed " + (held - after).ToString() + " item(s) out of the box (" + held.ToString() + " -> " + after.ToString() + ")");
        s.Touch();
        OZS_Commit.Moved(s, e, was);
        // Where it ACTUALLY went, not where it was asked to go: the row is
        // read back off the entity.
        s.TellMoved(e, w.m_Uid);
    }

    // EVERYTHING IN THE BOX, TIDIED. The planner says which cell each root
    // belongs in; the record is rewritten to say so, and the authority is
    // rebuilt from the record once the bridge has taken it.
    //
    // The old sort was the old scheme's last user: it closed the PLACED box
    // with new cells and opened it again into the world, so its first test was
    // "is the placed box open" -- and under the proxy it never is. The button
    // answered "opening" and did nothing at all (2026-09-26).
    static void Sort(OZS_Session s, OZS_Watcher w)
    {
        if (!s.m_Auth)
        {
            w.No(0, "the box is not ready", s.m_Version);
            return;
        }
        float now = GetGame().GetTickTime();
        if (now - s.m_Auth.OZS_GetLastSort() < OZS_Const.SORT_COOLDOWN)
        {
            w.No(0, "#STR_OZS_SORT_WAIT", s.m_Version);
            return;
        }
        array<EntityAI> roots = new array<EntityAI>();
        s.m_Auth.OZS_GetRoots(roots);
        if (roots.Count() == 0)
        {
            w.No(0, "#STR_OZS_SORT_WAIT", s.m_Version);
            return;
        }
        array<int> rows = new array<int>();
        array<int> cols = new array<int>();
        int placed = OZS_Sorter.Plan(s.m_Auth, roots, rows, cols);
        OZ_Log.Info("storage: proxy: box " + s.m_Id + " sorting " + roots.Count().ToString() + " root(s), " + placed.ToString() + " placed by the planner");
        // A SORT THAT CANNOT PLACE EVERY ROOT DOES NOT HAPPEN. The planner
        // packs by name, so a large item late in the alphabet can find no
        // block in a box that held it perfectly well before -- and the refill
        // then parks that root with the bridge: measured 2026-09-26 on a
        // Large box at 976 of 1000 cells, one AKM parked out of a box it
        // came from. Losing an item to a tidy-up is not a trade anyone made;
        // the box stays as it is and the player is told why.
        int cargoRoots = 0;
        for (int cr = 0; cr < roots.Count(); cr++)
        {
            EntityAI cargoRoot = roots.Get(cr);
            if (!cargoRoot || !cargoRoot.GetInventory())
                continue;
            InventoryLocation rootLoc = new InventoryLocation();
            if (cargoRoot.GetInventory().GetCurrentInventoryLocation(rootLoc) && rootLoc.GetType() == InventoryLocationType.CARGO)
                cargoRoots++;
        }
        if (placed < cargoRoots)
        {
            OZ_Log.Warn("storage: proxy: box " + s.m_Id + " is too full to sort: the planner placed " + placed.ToString() + " of " + cargoRoots.ToString() + " cargo root(s); nothing is moved");
            w.No(0, "#STR_OZS_SORT_FULL", s.m_Version);
            return;
        }
        if (!OZS_Commit.Sorted(s, roots, rows, cols))
        {
            w.No(0, "#STR_OZS_STORE_FAILED", s.m_Version);
            return;
        }
        s.m_Auth.OZS_SetLastSort(now);
        // The rebuild waits for the bridge: until the record has taken the new
        // cells there is nothing to rebuild FROM.
        s.SortPending();
    }

    // Two stacks into one. The engine decides, with every mod's CanBeCombined
    // in force -- which is the whole reason the authority is a real container
    // and not a table (§10, "what is NOT lost").
    // A SLOT ON THIS CONTAINER THAT WILL TAKE THIS ITEM, when the screen did
    // not say which one.
    //
    // The vanilla panel has two ways of putting something on a slot, and only
    // one of them names it: `PredictiveTakeEntityToTargetAttachmentEx` carries
    // a slot id, `PredictiveTakeEntityToTargetAttachment` does not and expects
    // the receiving side to find one. The second is the one a drop onto a
    // box's weapon rack goes through, so a rifle arrived asking for slot -1,
    // `HasInventorySlot(-1)` answered no, and the box refused every weapon put
    // on it (owner, 2026-09-25).
    //
    // Each candidate is put to `LocationCanAddEntity`, which knows the slot's
    // own rules, what is already hanging there and every mod's opinion -- the
    // config alone would only say the slot exists.
    static int FreeSlotFor(EntityAI holder, EntityAI e)
    {
        if (!holder || !holder.GetInventory() || !e)
            return InventorySlots.INVALID;
        // THE SLOTS THIS CONTAINER OFFERS. `GetSlotIdCount` is the other list
        // -- the slots the holder itself could hang in -- and walking it found
        // one entry that had nothing to do with the weapon rack.
        int count = holder.GetInventory().GetAttachmentSlotsCount();
        // EVERY SLOT AND THE ENGINE'S VERDICT ON EACH, when none of them will
        // do. "No free slot" is the same four words whether the container has
        // none, whether they are full, or whether the item is not allowed in
        // them -- and those are three different bugs (2026-09-25).
        string seen = "";
        for (int i = 0; i < count; i++)
        {
            int id = holder.GetInventory().GetAttachmentSlotId(i);
            if (id == InventorySlots.INVALID)
                continue;
            InventoryLocation at = new InventoryLocation();
            at.SetAttachment(holder, e, id);
            bool takes = holder.GetInventory().LocationCanAddEntity(at);
            string has = "empty";
            if (holder.GetInventory().FindAttachment(id))
                has = "taken";
            seen = seen + " | " + InventorySlots.GetSlotName(id) + " (" + id.ToString() + ") " + has + " takes=" + takes.ToString();
            if (takes)
                return id;
        }
        OZ_Log.Dbg("storage: proxy: no slot on " + holder.GetType() + " will take " + e.GetType() + "; it declares " + count.ToString() + " slot(s):" + seen);
        return InventorySlots.INVALID;
    }

    // A GENUINELY EMPTY RECTANGLE FOR A NEW ITEM SHAPED LIKE THIS ONE.
    //
    // NOT `FreeSpot`, AND THE DIFFERENCE IS THE WHOLE BUG IT FIXES. `FreeSpot`
    // asks `LocationCanAddEntity` about a location whose ITEM is the one being
    // placed -- which is the right question for an item that is arriving, and
    // the wrong one for a stack that is splitting: the source's own cell
    // trivially passes, because the source is already standing in it. The
    // split then asked vanilla to make the new stack where the old one was,
    // the engine put it somewhere else or nowhere, and the search at the named
    // cell found nothing. Measured 2026-09-25: the same pile refused to divide
    // at 5,2 over and over, and divided the moment the owner moved it out of
    // 5,2.
    //
    // So this asks the occupancy map instead, with NOTHING treated as leaving:
    // both stacks are going to stand there when it is over.
    static bool RoomFor(EntityAI holder, EntityAI like, out InventoryLocation spot)
    {
        if (!holder || !holder.GetInventory() || !like)
            return false;
        CargoBase cargo = holder.GetInventory().GetCargo();
        if (!cargo)
            return false;
        int w;
        int h;
        if (!SizeOf(like, w, h))
            return false;
        for (int r = 0; r + h <= cargo.GetHeight(); r++)
        {
            for (int c = 0; c + w <= cargo.GetWidth(); c++)
            {
                if (!Clear(holder, r, c, w, h, null, null))
                    continue;
                // The new stack lies the way the old one does: the rectangle
                // above was measured against that.
                spot.SetCargo(holder, like, 0, r, c, Flipped(like));
                return true;
            }
        }
        return false;
    }

    // ONE STACK BECOMES TWO, BOTH INSIDE THE BOX.
    //
    // THE SPLIT ITSELF IS VANILLA'S, called on the authority's own item. That
    // matters most for ammunition: `Magazine` overrides
    // `SplitItemToInventoryLocation` to make an empty pile and move half the
    // cartridges into it ONE AT A TIME, each with its own damage and type
    // (magazine.c:206-226). Anything written here instead would either lose
    // that detail or reimplement it badly, and the box has no business knowing
    // how a cartridge is shaped.
    //
    // WHAT IS OURS is everything around it: the place is chosen and confirmed
    // before the call, the new stack is found and given a handle afterwards,
    // and the record is written once for both halves -- the source rewritten
    // and the new stack added in a single letter, so a turn cannot half-happen.
    static void Split(OZS_Session s, OZS_Watcher w, int handle, int into, int kind, int lt, int slot, int row, int col, int flip)
    {
        ItemBase item = ItemBase.Cast(OZS_Authority.ByHandle(s.m_Auth, handle));
        if (!item)
        {
            w.No(handle, "no such item", s.m_Version);
            return;
        }
        // The engine's own word on whether this can be divided at all, and for
        // a magazine it is the rounds it counts, not the quantity.
        if (!item.CanBeSplit())
        {
            w.No(handle, "#STR_OZS_NO_SPLIT", s.m_Version);
            return;
        }
        EntityAI holder = s.m_Auth;
        if (into != 0)
        {
            holder = OZS_Authority.ByHandle(s.m_Auth, into);
            if (!holder)
            {
                w.No(handle, "no such container in the box", s.m_Version);
                return;
            }
        }

        // WHERE THE NEW STACK GOES, decided and confirmed before anything is
        // made. `dst` carries the SOURCE as its item on purpose: that is what
        // the vanilla panel hands these calls, and Magazine's own split reads
        // `dst.GetItem()` to copy the source's properties across.
        int likeW;
        int likeH;
        if (!SizeOf(item, likeW, likeH))
        {
            w.No(handle, "#STR_OZS_NO_SPLIT", s.m_Version);
            return;
        }
        // A NAMED CELL HAS TO BE EMPTY TOO, and emptiness here means empty of
        // everything -- including the stack being divided, which is staying
        // exactly where it is.
        bool named = false;
        if (lt != InventoryLocationType.ATTACHMENT)
        {
            named = row >= 0;
            if (named)
                named = Fits(holder, lt, slot, row, col);
            if (named)
                named = Clear(holder, row, col, likeW, likeH, null, null);
        }
        InventoryLocation dst = new InventoryLocation();
        if (lt == InventoryLocationType.ATTACHMENT)
            dst.SetAttachment(holder, item, slot);
        else if (named)
            dst.SetCargo(holder, item, 0, row, col, flip == 1);
        else if (!RoomFor(holder, item, dst))
        {
            w.No(handle, "#STR_OZS_FULL", s.m_Version);
            return;
        }
        // The cell has to be read back off `dst` rather than off the arguments:
        // when the screen named none, `RoomFor` chose it.
        int atRow = dst.GetRow();
        int atCol = dst.GetCol();

        // OURS, NOT VANILLA'S -- see SplitLocal for the measurement that
        // decided it. It hands the new stack back, so nothing has to be
        // looked for by its place afterwards.
        EntityAI made = SplitLocal(item, dst, kind);
        s.Touch();
        if (!made)
        {
            // Nothing was made and nothing was taken: vanilla refuses a split
            // it cannot perform without touching the source. The screen is
            // told so it does not go on drawing a stack it has already halved
            // in its own picture.
            OZ_Log.Dbg("storage: proxy: " + item.GetType() + " would not split at " + atRow.ToString() + "," + atCol.ToString());
            w.No(handle, "#STR_OZS_NO_SPLIT", s.m_Version);
            return;
        }

        int madeHandle = OZS_Authority.Handle(s.m_Auth, made);
        OZS_Commit.Split(s, item, made);
        s.TellQuantity(item, w.m_Uid);
        s.TellAdded(made, w.m_Uid);
        // THE NEW STACK'S NETWORK ID IS PRINTED ON PURPOSE. Vanilla's split
        // makes it with LocationCreateEntity -- a registered entity -- inside
        // a container the network has never heard of, which is the shape the
        // authority's own rule forbids (OZS_Records.ReadRoot). "00" here
        // means the engine kept it local after all; anything else means the
        // split has to be done by hand (review 2026-09-26, B3).
        OZ_Log.Dbg("storage: proxy: split " + item.GetType() + " into #" + madeHandle.ToString() + " at " + atRow.ToString() + "," + atCol.ToString() + ", netid " + made.GetNetworkIDString());
    }

    // VANILLA'S SPLIT WITH ONE CALL CHANGED.
    //
    // `SplitItemToInventoryLocation`, `SplitIntoStackMaxToInventoryLocationEx`
    // and Magazine's override of the first all make the new stack with
    // `LocationCreateEntity` -- a REGISTERED entity -- and measured on the
    // authority 2026-09-26 that is exactly what it got: netid 019766, inside
    // a container the network has never heard of, which is the shape the
    // authority forbids for everything it holds (OZS_Records.ReadRoot), and
    // the shape the ghosts of 2026-09-18 came from. This is those three
    // bodies (itembase.c:1985 and :1872, magazine.c:206) with
    // `LocationCreateLocalEntity` where they create and nothing else changed:
    // the halving, the stack cap, the cartridges moved one at a time with
    // their own damage, the property transfer.
    //
    // Returns the new stack, or null when the engine would not split it.
    static EntityAI SplitLocal(ItemBase item, InventoryLocation dst, int kind)
    {
        Magazine mag = Magazine.Cast(item);
        if (mag)
        {
            if (!mag.CanBeSplit())
                return null;
            Magazine pile = Magazine.Cast(GameInventory.LocationCreateLocalEntity(dst, mag.GetType(), ECE_IN_INVENTORY, RF_DEFAULT));
            if (!pile)
                return null;
            MiscGameplayFunctions.TransferItemProperties(mag, pile);
            pile.ServerSetAmmoCount(0);
            int rounds = mag.GetAmmoCount();
            for (int i = 0; i < Math.Floor(rounds * 0.5); i++)
            {
                float damage;
                string cartridge;
                mag.ServerAcquireCartridge(damage, cartridge);
                pile.ServerStoreCartridge(damage, cartridge);
            }
            pile.SetSynchDirty();
            mag.SetSynchDirty();
            return pile;
        }
        float quantity = item.GetQuantity();
        float piece;
        if (kind == OZS_Const.SPLIT_MAX)
        {
            if (!dst.IsValid())
                return null;
            float cap = item.GetTargetQuantityMax(dst.GetSlot());
            piece = quantity;
            if (quantity > cap)
                piece = cap;
        }
        else
        {
            piece = Math.Floor(quantity * 0.5);
        }
        if (!item.OZS_ShouldSplit(piece))
            return null;
        ItemBase part = ItemBase.Cast(GameInventory.LocationCreateLocalEntity(dst, item.GetType(), ECE_IN_INVENTORY, RF_DEFAULT));
        if (!part)
            return null;
        if (kind != OZS_Const.SPLIT_MAX && part.GetQuantityMax() < piece)
            piece = part.GetQuantityMax();
        part.SetResultOfSplit(true);
        MiscGameplayFunctions.TransferItemProperties(item, part);
        // A half split onto a SLOT gives the slot one, as vanilla does.
        bool oneOnSlot = false;
        if (kind != OZS_Const.SPLIT_MAX && dst.IsValid() && dst.GetType() == InventoryLocationType.ATTACHMENT && piece > 1)
            oneOnSlot = true;
        if (oneOnSlot)
        {
            item.AddQuantity(-1, false, true);
            part.SetQuantity(1, false, true);
        }
        else
        {
            item.AddQuantity(-piece, false, true);
            part.SetQuantity(piece, false, true);
        }
        return part;
    }

    // WHAT IS ACTUALLY IN A STACK, WHICHEVER KIND OF STACK IT IS.
    //
    // A magazine's contents are its ROUNDS, and they are not its quantity: an
    // ammo pile carries quantity 1 and twenty rounds. So `GetQuantity() <= 0`
    // is never true for one, an emptied pile was never deleted, and combining
    // two piles inside a box left the result next to a stack of nothing --
    // which is what the owner saw as an artefact (2026-09-25). The same
    // confusion had the screen drawing a fresh pile's round count for every
    // pile in the box, because the panel reads `GetAmmoCount` and only that
    // (quantityconversions.c:12-19).
    static float Contents(ItemBase item)
    {
        if (!item)
            return 0;
        Magazine mag = Magazine.Cast(item);
        if (mag)
            return mag.GetAmmoCount();
        return item.GetQuantity();
    }

    static void Combine(OZS_Session s, OZS_Watcher w, int handle, int other)
    {
        ItemBase into = ItemBase.Cast(OZS_Authority.ByHandle(s.m_Auth, handle));
        ItemBase from = ItemBase.Cast(OZS_Authority.ByHandle(s.m_Auth, other));
        if (!into || !from || into == from)
        {
            w.No(handle, "no such pair", s.m_Version);
            return;
        }
        if (!into.CanBeCombined(from, false))
        {
            w.No(handle, "these do not stack", s.m_Version);
            return;
        }
        int fromRoot = OZS_Commit.RootOf(s, from);
        bool fromWasRoot = OZS_Commit.TopOf(s, from) == from;
        string fromType = from.GetType();
        int goneHandle = OZS_Authority.Handle(s.m_Auth, from);
        into.CombineItems(from, true);
        s.Touch();
        // CombineItems moves the contents; it does not remove an emptied item.
        bool gone = Contents(from) <= 0;
        if (gone)
        {
            // Told to the watchdog before it happens: this departure is the
            // point of the operation, not the thing it watches for.
            OZS_Watchdog.Expect(from);
            GetGame().ObjectDelete(from);
        }
        // Both halves in ONE letter (review 2026-09-26, B5).
        OZS_Commit.Combined(s, into, fromRoot, fromWasRoot, fromType, gone);
        if (gone)
            s.TellGone(goneHandle, w.m_Uid);
        else
            s.TellQuantity(from, w.m_Uid);
        s.TellQuantity(into, w.m_Uid);
    }

    // TWO ITEMS EXCHANGE PLACES, THE WAY THE VANILLA INVENTORY DOES IT.
    //
    // The screen has TWO swaps and they are not variations of each other
    // (3_game/systems/inventory/inventory.c:618, :656):
    //
    //   ORDINARY  -- `CanSwapEntities`, whose own documentation says "the two
    //                items has EQUAL SIZES and can be swapped without
    //                additional space". Cell for cell, nothing else moves.
    //   FORCED    -- `CanForceSwapEntities(item1, item1_dst, item2, OUT
    //                item2_dst)`. The sizes differ, so the item being replaced
    //                has to go SOMEWHERE ELSE, and the native finds that place
    //                and hands it back BEFORE anything moves. No place, no
    //                swap.
    //
    // Both of those pre-checks refuse everything in a container the engine has
    // never been told about, which is why the screen never offers a swap for a
    // box at all. So the decision is made here instead -- but by the same two
    // rules, in the same order, with the same refusals.
    //
    // What was wrong before, twice over: the ordinary native was called for
    // items of DIFFERENT sizes, which is outside its contract, and it answered
    // by half-doing the move and leaving a rifle with no location at all; and
    // the forced case looked for the spare cell while the items were already
    // moving, so a failure left one of them wherever it had got to. Now the
    // sizes decide which branch runs, the spare place is found first, and
    // every step is read back off the entity -- a step that did not land puts
    // everything back where it was.
    static void Swap(OZS_Session s, OZS_Watcher w, int handle, int other, int aimRow, int aimCol, int lt, int slot, int row, int col, int flip)
    {
        // `a` is the item the player is dragging -- vanilla's item1, the
        // forced one. `b` is the one sitting where it is being dropped.
        EntityAI a = OZS_Authority.ByHandle(s.m_Auth, handle);
        EntityAI b = OZS_Authority.ByHandle(s.m_Auth, other);
        if (!a || !b || a == b)
        {
            w.No(handle, "no such pair", s.m_Version);
            return;
        }
        // Where each stood in the record, before either moves: a swap between
        // the grid and a bag standing on it changes which of the two is a
        // root (OZS_Commit.Settle).
        OZS_Was wasA = new OZS_Was(s, a);
        OZS_Was wasB = new OZS_Was(s, b);
        InventoryLocation srcA = new InventoryLocation();
        InventoryLocation srcB = new InventoryLocation();
        if (!a.GetInventory().GetCurrentInventoryLocation(srcA) || !b.GetInventory().GetCurrentInventoryLocation(srcB))
        {
            w.No(handle, "no such pair", s.m_Version);
            return;
        }
        // Each item's own say, which mods override and vanilla asks in both
        // branches (inventory.c:648, :700).
        // EXACTLY `GameInventory.MakeDstForSwap` (inventory.c:1192): the
        // destination keeps the ITEM's own record and takes only the PLACE
        // from the other one -- and the flip comes from the item that is
        // moving, never from the location it is moving into. Copying the
        // other location whole carries its flip along, and the item then
        // tries to stand turned a way it cannot: the move fails, the rollback
        // of the step before it fails too, and one of the two is left where
        // it was pushed (owner, 2026-09-25: "the rifle moved, the bandage did
        // not").
        InventoryLocation dstA = new InventoryLocation();
        dstA.Copy(srcA);
        dstA.CopyLocationFrom(srcB, false);
        dstA.SetFlip(a.GetInventory().GetFlipCargo());
        InventoryLocation dstB = new InventoryLocation();
        dstB.Copy(srcB);
        dstB.CopyLocationFrom(srcA, false);
        dstB.SetFlip(b.GetInventory().GetFlipCargo());
        if (!a.CanSwapEntities(b, dstB, dstA) || !b.CanSwapEntities(a, dstA, dstB))
        {
            w.No(handle, "#STR_OZS_NO_SWAP", s.m_Version);
            return;
        }
        // AND HOW MUCH IS IN THEM. A place has a cap on quantity of its own --
        // a slot that takes a canteen takes only so much water -- and vanilla
        // refuses a swap that would put more into a place than it allows,
        // BEFORE the move rather than clamping afterwards (inventory.c:638,
        // :645 for the ordinary swap, :686, :703 for the forced one). Cargo
        // cells answer -1 and no cap, so this bites on the weapon slots.
        if (!Holds_Quantity(a, srcB.GetSlot()) || !Holds_Quantity(b, srcA.GetSlot()))
        {
            w.No(handle, "#STR_OZS_NO_SWAP", s.m_Version);
            return;
        }

        int aw;
        int ah;
        int bw;
        int bh;
        bool sizedA = SizeOf(a, aw, ah);
        bool sized = sizedA;
        if (sized)
            sized = SizeOf(b, bw, bh);
        // The sizes are still needed -- our own search measures rectangles
        // with them -- but they no longer DECIDE which kind of swap this is.
        // That is the engine's to say, and its rule is not "equal footprints":
        // measured 2026-09-25, a 1x2 can and a 3x1 radio, which are plainly
        // not equal, got `CanSwapEntities=true`. Deciding it here by a rule of
        // my own sent such pairs down the forced path, where the radio slid a
        // cell sideways and the can did not move at all.
        // THE WHOLE DECISION IN ONE LINE. Which two items, where each of them
        // stands, how big each of them is: everything the branch below turns
        // on. A report of the shape "the rifle moved and the bandage did not"
        // is answerable from this and the lines that follow it, and from
        // nothing less.
        string say = "swap " + a.GetType() + " " + aw.ToString() + "x" + ah.ToString() + " at " + srcA.GetRow().ToString() + "," + srcA.GetCol().ToString();
        say = say + " with " + b.GetType() + " " + bw.ToString() + "x" + bh.ToString() + " at " + srcB.GetRow().ToString() + "," + srcB.GetCol().ToString();
        OZ_Log.Dbg("storage: proxy: " + say + "; flips " + srcA.GetFlip().ToString() + "/" + srcB.GetFlip().ToString());
        // WHAT THE ENGINE'S OWN PRE-CHECKS SAY ABOUT THIS VERY PAIR. The
        // server-side swap exists because they refuse everything in an
        // unannounced container; printed here so the claim is re-measured on
        // every real pair instead of resting on one measurement from e1df77e.
        InventoryLocation forcedTo = new InventoryLocation();
        bool plain = GameInventory.CanSwapEntities(a, b);
        bool forced = GameInventory.CanForceSwapEntities(a, null, b, forcedTo);
        string verdicts = "natives: CanSwapEntities=" + plain.ToString();
        verdicts = verdicts + " CanForceSwapEntities=" + forced.ToString();
        OZ_Log.Dbg("storage: proxy: " + verdicts);
        // THE CELL THE PLAYER AIMED AT WINS OVER THE OTHER ITEM'S CORNER.
        //
        // NO CELL DECIDES ANYTHING HERE, AND THAT IS VANILLA'S RULE.
        //
        // `MakeDstForSwap` gives each item the OTHER one's location and its
        // own orientation; the point the cursor was over never enters it. So
        // an exchange ignores any cell it is handed, and `dstA`/`dstB` above
        // are the whole answer.
        //
        // The cell still travels, because a DROP knows one -- `Inside` reads
        // it off the engine's own `InventoryLocation` -- and it is worth
        // seeing in the log when the two disagree. It is not worth acting on:
        // the one time this mod did act on it, the number came from the
        // panel's own formula measured against the container's ROOT widget,
        // which in our panel carries a title, a search field and buttons
        // above the grid, so a drop aimed at row 3 arrived as row 10
        // (measured 2026-09-25). That capture is gone, and so is the `Icon`
        // override that took it.
        bool aimed = aimRow >= 0;
        if (aimed)
            aimed = aimCol >= 0;
        if (aimed)
            OZ_Log.Dbg("storage: proxy: the drop named " + aimRow.ToString() + "," + aimCol.ToString() + "; an exchange ignores it and the item goes to " + Spot(dstA) + ", which is vanilla's rule");

        // THE ENGINE DECIDES, WE MOVE. THAT SPLIT IS THE LESSON OF THE NIGHT.
        //
        // `LocationSwap` is the engine's own atomic exchange, and on this
        // container it does not work even for a pair the engine itself has
        // just approved: measured twice on 2026-09-25 -- with unequal
        // footprints it half-executes and leaves an item with no location at
        // all, and with `CanSwapEntities=true` it simply does not place them
        // ("did not land; both are put back"). The verification caught it both
        // times, so nothing was lost, but a primitive that has to be undone
        // after every call is not a primitive worth calling.
        //
        // So the native is asked only WHAT this gesture is -- an ordinary
        // trade or a forced one, and whether the pair is allowed at all. The
        // placing is done here, one verified move at a time, by the same
        // sequencer both branches share.
        // Where the displaced item ends up. Filled in by whichever branch
        // runs below, and read by the sequencer that does the moving.
        InventoryLocation goesB = new InventoryLocation();
        // AN ORDINARY TRADE -- IF THE CELLS ACTUALLY ALLOW IT.
        //
        // `CanSwapEntities` being true does NOT mean each item fits in the
        // other's exact place: measured 2026-09-25, a 1x2 can and a 1x3 radio
        // got `true`, and the radio then did not fit where the can had stood
        // -- it needs a third row the can never occupied. The native answers
        // "may these two trade at all", not "will they land". So its verdict
        // chooses the KIND of swap, and the geometry is still checked here.
        bool exact = plain;
        if (exact)
            exact = Clear(s.m_Auth, srcA.GetRow(), srcA.GetCol(), bw, bh, a, b);
        if (exact)
        {
            goesB.Copy(dstB);
        }
        // ---- forced swap: the engine would not take the ordinary one ----
        //
        // This is vanilla's FSWAP, and vanilla's own rule for it is: the
        // dragged item takes the target's place, and the TARGET goes wherever
        // there is room. The screen asks for it as
        //
        //   CanForceSwapEntitiesEx(selected, null, target, OUT dst)
        //
        // -- `null` meaning "the dragged one goes to the target's place", and
        // `dst` an OUT parameter the engine fills in with somewhere the target
        // fits (icon.c:301, itemmanager.c:791). So the displaced item really
        // may end up in another cell; that is the vanilla behaviour and it is
        // what we reproduce.
        //
        // Two things are ours, and both are about not losing anything: the
        // place is decided BEFORE anything moves, and a step that does not
        // land puts everything back. The old version did neither, which is why
        // a rifle could be left standing wherever it had got to.
        if (!exact)
        {
            if (!sized)
            {
                w.No(handle, "#STR_OZS_NO_SWAP", s.m_Version);
                return;
            }
            if (!a.CanBeFSwaped())
            {
                w.No(handle, "#STR_OZS_NO_SWAP", s.m_Version);
                return;
            }
            // Where `a` is going has to be clear of everything but the two of
            // them, whatever happens to `b`.
            if (!Clear(s.m_Auth, srcB.GetRow(), srcB.GetCol(), aw, ah, a, b))
            {
                w.No(handle, "#STR_OZS_NO_SWAP", s.m_Version);
                return;
            }
            // WHERE `b` GOES, IN VANILLA'S OWN ORDER. The place `a` is leaving is
            // tried first -- that is the exchange the player meant, and when it
            // fits nothing else in the box is disturbed. Only when `b` does not
            // fit there does the search for a free spot run, which is the native's
            // fallback.
            // WHERE `b` GOES IS THE SCREEN'S TO SAY, WHEN IT SAID IT.
            //
            // A forced swap arrives carrying the place vanilla computed for the
            // displaced item; only an ordinary swap leaves it unnamed. That place
            // is honoured whenever the box will take it -- otherwise the player
            // watches their rifle land where nobody pointed, which is the whole
            // complaint. `row` below zero means nothing was named.
            //
            // One test per statement below: this parser does not take a condition
            // split over two lines, and a compound boolean assigned into a field
            // is worse still -- it corrupts the heap.
            // A NAMED PLACE IS NOT ALWAYS A CELL. The screen's answer for a rifle
            // pushed out of the way is usually one of the box's WEAPON SLOTS, and
            // a slot has no row and no column at all -- testing `row >= 0` read
            // that as "nothing was named" and threw the screen's answer away
            // (measured 2026-09-25: `lt 2 slot -692829678 at -1,-1`).
            bool named = false;
            bool trade = false;
            if (lt == InventoryLocationType.ATTACHMENT)
            {
                // A slot of THIS BOX. The screen's own answer is a slot on the
                // player, and that one is dropped before we ever get here.
                named = Fits(s.m_Auth, lt, slot, -1, -1);
                if (named)
                    goesB.SetAttachment(s.m_Auth, b, slot);
            }
            else if (row >= 0)
            {
                named = Clear(s.m_Auth, row, col, bw, bh, a, b);
                if (named)
                    named = !Overlaps(row, col, bw, bh, srcB.GetRow(), srcB.GetCol(), aw, ah);
                if (named)
                {
                    goesB.SetCargo(s.m_Auth, b, 0, row, col, flip == 1);
                    trade = Overlaps(row, col, bw, bh, srcA.GetRow(), srcA.GetCol(), aw, ah);
                }
            }
            if (!named)
            {
                // THE ENGINE'S OWN ANSWER FIRST -- IT HAS ONE, AND THIS WAS THE
                // SURPRISE OF THE NIGHT.
                //
                // `CanForceSwapEntities(a, null, b, OUT dst)` was believed to
                // refuse everything in a container the engine has never been told
                // about; that belief goes back to one measurement of the ORDINARY
                // swap and was repeated all evening without being re-checked.
                // Measured again on a real pair, 2026-09-25: `CanSwapEntities` is
                // false -- correctly, the two have different footprints -- and
                // `CanForceSwapEntities` is TRUE, and fills in a destination. So
                // the vanilla answer is available on the authority after all, and
                // it is taken before any of ours.
                //
                // It is still checked: the same call on the CLIENT answers with a
                // slot on the player, and a swap inside a box must not throw
                // anything out of it.
                // OURS FIRST, AND THE REASON IS THE SHAPE OF THIS BOX.
                //
                // The engine's own answer is real -- `CanForceSwapEntities` does
                // work on the authority, measured 2026-09-25 -- but a storage box
                // is TEN cells wide and FIFTY tall, and the panel shows about a
                // dozen rows. Asked where to put a displaced item, the engine
                // answered with row 10 and row 11: correct, and invisible. The
                // player calls that "the items disappeared", and they are right to
                // (owner, 2026-09-25).
                //
                // So the nearest free place wins, and the engine's answer is the
                // fallback. Ours is also the only one that counts the cells the
                // DRAGGED item is leaving as free, which is what makes an exchange
                // look like an exchange.
                // THE PLACE THE DRAGGED ITEM IS LEAVING, FIRST OF ALL.
            //
            // That is what an exchange means, and it is the only outcome that
            // disturbs nothing else in the box. Only when the displaced item
            // does not fit there does the search for another place run -- and
            // in a box fifty rows tall that search can legitimately answer
            // "row 13", which to the player reads as the item jumping off the
            // screen (owner, 2026-09-25).
            bool found = Clear(s.m_Auth, srcA.GetRow(), srcA.GetCol(), bw, bh, a, b);
            if (found)
                found = !Overlaps(srcA.GetRow(), srcA.GetCol(), bw, bh, dstA.GetRow(), dstA.GetCol(), aw, ah);
            if (found)
                goesB.Copy(dstB);
            // AND IF IT DOES NOT FIT THERE, NOTHING HAPPENS.
            //
            // Vanilla would send the displaced item wherever there is room,
            // and that is what this did until it was watched: a box is fifty
            // rows deep, "wherever" is always a little further down, and each
            // swap walked the rifle down by its own height -- rows 28, 30, 32,
            // 34 -- until both items were far below anything the panel shows.
            // The player calls that losing them, and is right (owner,
            // 2026-09-25).
            //
            // So an exchange is an exchange: the two take each other's places
            // or the box says no. One line to undo if the other trade is ever
            // wanted -- the search that found "somewhere it fits" is still in
            // Room() above.
            // The one exception: a box has slots of its own, and an item
            // pushed out of a cell may belong on the rack. That is a PLACE,
            // not a wandering -- it cannot creep, because there are four of
            // them and they are in sight.
            if (!found)
                found = s.m_Auth.GetInventory().FindFreeLocationFor(b, FindInventoryLocationType.ATTACHMENT, goesB);
            if (!found)
            {
                w.No(handle, "#STR_OZS_NO_SWAP", s.m_Version);
                return;
            }
            }
            if (!Holds_Quantity(a, srcB.GetSlot()) || !Holds_Quantity(b, goesB.GetSlot()))
            {
                w.No(handle, "#STR_OZS_NO_SWAP", s.m_Version);
                return;
            }
        }

        OZ_Log.Dbg("storage: proxy: swap branch: named " + named.ToString() + " trade " + trade.ToString() + ", the sitter goes to " + goesB.GetRow().ToString() + "," + goesB.GetCol().ToString());

        // WHO MOVES FIRST IS DECIDED BY WHOSE DESTINATION IS UNDER THE OTHER.
        //
        // The old test asked "are the two going into each other's places",
        // which is not the question: what matters is whether an item's
        // destination is covered by the OTHER ITEM -- the one that is about to
        // leave it (owner, 2026-09-25). Get that wrong and the first move is
        // made onto occupied cells, fails, and the exchange falls apart with
        // one item moved and one not.
        //
        // An attachment destination is never "under" anything: a slot is not
        // a rectangle.
        bool aOnB = false;
        if (dstA.GetType() == InventoryLocationType.CARGO)
            aOnB = Overlaps(dstA.GetRow(), dstA.GetCol(), aw, ah, srcB.GetRow(), srcB.GetCol(), bw, bh);
        bool bOnA = false;
        if (goesB.GetType() == InventoryLocationType.CARGO)
            bOnA = Overlaps(goesB.GetRow(), goesB.GetCol(), bw, bh, srcA.GetRow(), srcA.GetCol(), aw, ah);
        bool deadlock = aOnB;
        if (deadlock)
            deadlock = bOnA;

        if (!deadlock)
        {
            // One of them can simply go first: the one whose destination is
            // already clear. If `b` needs the cells `a` is standing on, `a`
            // goes first; otherwise `b` does, which also covers the ordinary
            // case where `a` is taking `b`'s place.
            EntityAI first = b;
            EntityAI second = a;
            InventoryLocation firstTo = goesB;
            InventoryLocation secondTo = dstA;
            InventoryLocation firstHome = srcB;
            if (bOnA)
            {
                first = a;
                second = b;
                firstTo = dstA;
                secondTo = goesB;
                firstHome = srcA;
            }
            if (!Put(first, firstTo))
            {
                w.No(handle, "#STR_OZS_NO_SWAP", s.m_Version);
                return;
            }
            if (!Put(second, secondTo))
            {
                OZ_Log.Error("storage: proxy: box " + s.m_Id + ": " + first.GetType() + " moved but " + second.GetType() + " would not follow; the first is put back");
                Sit(first, firstHome);
                s.TellMoved(a, "");
                s.TellMoved(b, "");
                w.No(handle, "#STR_OZS_NO_SWAP", s.m_Version);
                return;
            }
            Swapped(s, w, first, second, wasA, wasB);
            return;
        }

        // EACH ONE'S DESTINATION IS UNDER THE OTHER, so neither can go first.
        // The smaller of the two stands aside for ONE step, in a corner clear
        // of both destinations. Nobody ever sees it there -- either the
        // exchange completes or everything is put back.
        EntityAI mover = a;
        EntityAI sitter = b;
        InventoryLocation moverHome = srcA;
        InventoryLocation sitterHome = srcB;
        InventoryLocation moverTo = dstA;
        InventoryLocation sitterTo = goesB;
        if (bw * bh < aw * ah)
        {
            mover = b;
            sitter = a;
            moverHome = srcB;
            sitterHome = srcA;
            moverTo = goesB;
            sitterTo = dstA;
        }
        InventoryLocation aside = new InventoryLocation();
        if (!Aside(s.m_Auth, mover, srcA, aw, ah, srcB, bw, bh, aside))
        {
            w.No(handle, "#STR_OZS_FULL", s.m_Version);
            return;
        }
        if (!Put(mover, aside))
        {
            w.No(handle, "#STR_OZS_NO_SWAP", s.m_Version);
            return;
        }
        if (!Put(sitter, sitterTo) || !Put(mover, moverTo))
        {
            OZ_Log.Error("storage: proxy: box " + s.m_Id + ": " + a.GetType() + " and " + b.GetType() + " could not take each other's place after all; both are put back");
            Sit(sitter, sitterHome);
            Sit(mover, moverHome);
            s.TellMoved(a, "");
            s.TellMoved(b, "");
            w.No(handle, "#STR_OZS_NO_SWAP", s.m_Version);
            return;
        }
        Swapped(s, w, sitter, mover, wasA, wasB);
    }

    // THE BOX AS A MAP OF CELLS, with the two items that are leaving counted
    // as EMPTY.
    //
    // This is the whole reason the search is ours and not the engine's.
    // `FindFirstFreeLocationForNewEntity` cannot know that the item being
    // dragged is about to vacate its cells, so it never offers them -- and a
    // rifle displaced by a can went to the bottom of the box instead of
    // shifting up into the space the can had just left (owner, 2026-09-25).
    //
    // One walk over the items, then every candidate rectangle is a few reads.
    static ref array<int> Map(EntityAI holder, EntityAI leaving1, EntityAI leaving2, out int width, out int height)
    {
        width = 0;
        height = 0;
        if (!holder || !holder.GetInventory())
            return null;
        CargoBase cargo = holder.GetInventory().GetCargo();
        if (!cargo)
            return null;
        width = cargo.GetWidth();
        height = cargo.GetHeight();
        if (width <= 0 || height <= 0)
            return null;
        array<int> taken = new array<int>();
        int cells = width * height;
        for (int i = 0; i < cells; i++)
            taken.Insert(0);
        for (int n = 0; n < cargo.GetItemCount(); n++)
        {
            EntityAI sitting = cargo.GetItem(n);
            if (!sitting)
                continue;
            if (sitting == leaving1)
                continue;
            if (sitting == leaving2)
                continue;
            int r;
            int c;
            if (!Where(sitting, r, c))
                continue;
            int w;
            int h;
            if (!SizeOf(sitting, w, h))
                continue;
            for (int rr = r; rr < r + h; rr++)
            {
                if (rr < 0 || rr >= height)
                    continue;
                for (int cc = c; cc < c + w; cc++)
                {
                    if (cc < 0 || cc >= width)
                        continue;
                    taken.Set(rr * width + cc, 1);
                }
            }
        }
        return taken;
    }

    // Is this rectangle free on the map?
    static bool Free(array<int> taken, int width, int height, int row, int col, int w, int h)
    {
        if (row < 0 || col < 0 || w <= 0 || h <= 0)
            return false;
        if (row + h > height || col + w > width)
            return false;
        for (int r = row; r < row + h; r++)
        {
            for (int c = col; c < col + w; c++)
            {
                if (taken.Get(r * width + c) != 0)
                    return false;
            }
        }
        return true;
    }

    // WHERE THE DISPLACED ITEM GOES: THE NEAREST PLACE THAT FITS.
    //
    // Nearest to where it stands now, because a swap should move the other
    // item as little as it can -- ideally into the very cells the dragged one
    // is leaving, which is what the player sees as an exchange. The engine's
    // own search offers neither of those things: it cannot see the vacated
    // cells at all, and it answers with the first free corner it meets.
    // `goes` is where the DRAGGED item is heading, and nothing may be offered
    // there. Without that rule the nearest place for the displaced item was
    // its OWN -- distance zero, the best candidate there can be -- so it did
    // not move at all, the move said "already there", and the dragged item was
    // then sent onto it (owner, 2026-09-25: "the rifle did not move although
    // there was plenty of free space").
    static bool Room(EntityAI holder, EntityAI b, int bw, int bh, EntityAI a, InventoryLocation home, InventoryLocation leaves, int lw, int lh, InventoryLocation goes, int gw, int gh, out InventoryLocation spot)
    {
        int width;
        int height;
        array<int> taken = Map(holder, a, b, width, height);
        if (!taken)
            return false;
        int bestRow = -1;
        int bestCol = -1;
        int bestAway = 0;
        for (int r = 0; r + bh <= height; r++)
        {
            for (int c = 0; c + bw <= width; c++)
            {
                if (!Free(taken, width, height, r, c, bw, bh))
                    continue;
                // Never where the dragged item is going.
                if (goes.GetType() == InventoryLocationType.CARGO)
                {
                    if (Overlaps(r, c, bw, bh, goes.GetRow(), goes.GetCol(), gw, gh))
                        continue;
                }
                // AND THE ENGINE'S OWN WORD ON THE PLACE.
                //
                // Our map knows something the engine does not -- that `a` and
                // `b` are leaving -- so a candidate covering THEIR cells is
                // ours to judge. Every other candidate is confirmed with the
                // engine, because the engine knows things WE do not: measured
                // 2026-09-25, `GetHeight()` answers 50 for a box that refuses
                // row 10, and the search happily offered cells nothing could
                // ever be put into.
                bool mine = Overlaps(r, c, bw, bh, home.GetRow(), home.GetCol(), bw, bh);
                if (!mine)
                    mine = Overlaps(r, c, bw, bh, leaves.GetRow(), leaves.GetCol(), lw, lh);
                if (!mine)
                {
                    InventoryLocation probe = new InventoryLocation();
                    probe.SetCargo(holder, b, 0, r, c, b.GetInventory().GetFlipCargo());
                    if (!holder.GetInventory().LocationCanAddEntity(probe))
                        continue;
                }
                int away = Math.AbsInt(r - home.GetRow()) + Math.AbsInt(c - home.GetCol());
                if (bestRow >= 0 && away >= bestAway)
                    continue;
                bestRow = r;
                bestCol = c;
                bestAway = away;
            }
        }
        if (bestRow < 0)
            return false;
        spot.SetCargo(holder, b, 0, bestRow, bestCol, b.GetInventory().GetFlipCargo());
        return true;
    }

    // A corner to stand in for one step, when the two are going into each
    // other's places: inside the grid, free, and clear of BOTH destinations,
    // because one of them is about to be filled by the other item.
    static bool Aside(EntityAI holder, EntityAI e, InventoryLocation srcA, int aw, int ah, InventoryLocation srcB, int bw, int bh, out InventoryLocation spot)
    {
        if (!holder || !holder.GetInventory() || !e)
            return false;
        CargoBase cargo = holder.GetInventory().GetCargo();
        if (!cargo)
            return false;
        int ew;
        int eh;
        if (!SizeOf(e, ew, eh))
            return false;
        for (int r = 0; r + eh <= cargo.GetHeight(); r++)
        {
            for (int c = 0; c + ew <= cargo.GetWidth(); c++)
            {
                if (Overlaps(r, c, ew, eh, srcA.GetRow(), srcA.GetCol(), bw, bh))
                    continue;
                if (Overlaps(r, c, ew, eh, srcB.GetRow(), srcB.GetCol(), aw, ah))
                    continue;
                if (!Clear(holder, r, c, ew, eh, e, null))
                    continue;
                // The item keeps the orientation it already has: the corner
                // was measured against its flipped footprint, so putting it
                // down unturned would be a different rectangle.
                spot.SetCargo(holder, e, 0, r, c, e.GetInventory().GetFlipCargo());
                return true;
            }
        }
        return false;
    }

    // A CELL FOR AN ITEM THAT IS NOT IN THE BOX YET, asked of the engine one
    // cell at a time.
    //
    // NOT `FindFirstFreeLocationForNewEntity`. On an unannounced authority it
    // answers TRUE and fills in cell 0,0 whatever is standing there -- and
    // `LocationCanAddEntity` then says 0,0 is not a place, which is the engine
    // contradicting itself. Handing that cell to `TakeToDst` ends the server
    // process: no crash dump, no Windows event, the script log stops mid-line.
    // Measured five times on 2026-09-25 with different items and different
    // sources -- a box and an ammo pile off the ground, an ammo pile out of a
    // shirt -- and every inbound move that worked all night had gone to a cell
    // the screen had NAMED. The source was a red herring: a ground item was
    // refused for a while on that theory, and dropping the refusal once this
    // walk was in place cost nothing, because the source never mattered.
    //
    // The size of an item that is not in a cargo cannot be read from the
    // cargo, so nothing here measures it: each candidate cell is put to
    // `LocationCanAddEntity`, which knows the footprint, the flip and every
    // mod's own rules. Both orientations are tried, unturned first, the way
    // the vanilla panel does. The walk stops at the first yes, so a box with
    // room costs a handful of calls; only a full one pays for the whole grid.
    static bool FreeSpot(EntityAI holder, EntityAI e, out InventoryLocation spot)
    {
        if (!holder || !holder.GetInventory() || !e)
            return false;
        CargoBase cargo = holder.GetInventory().GetCargo();
        if (!cargo)
            return false;
        int height = cargo.GetHeight();
        int width = cargo.GetWidth();
        // OUR OWN MAP FIRST, THE ENGINE'S WORD LAST. A nearly full box put a
        // thousand cells to `LocationCanAddEntity` one by one, each a walk of
        // the cargo inside the engine (review 2026-09-26, D3). The occupancy
        // map is one walk, and a cell it knows to be taken is not asked
        // about; every cell it calls free is still confirmed by the engine,
        // which knows the rules the map does not.
        int mapW;
        int mapH;
        array<int> taken = Map(holder, null, null, mapW, mapH);
        int cfgW = 0;
        int cfgH = 0;
        GetGame().GetInventoryItemSize(InventoryItem.Cast(e), cfgW, cfgH);
        bool sized = false;
        if (taken && cfgW > 0 && cfgH > 0)
            sized = true;
        for (int turn = 0; turn < 2; turn++)
        {
            bool flipped = turn == 1;
            int w = cfgW;
            int h = cfgH;
            if (flipped)
            {
                w = cfgH;
                h = cfgW;
            }
            for (int r = 0; r < height; r++)
            {
                for (int c = 0; c < width; c++)
                {
                    if (sized && !Free(taken, mapW, mapH, r, c, w, h))
                        continue;
                    spot.SetCargo(holder, e, 0, r, c, flipped);
                    if (holder.GetInventory().LocationCanAddEntity(spot))
                        return true;
                }
            }
        }
        return false;
    }

    // A FREE RECTANGLE FOR AN ITEM THAT IS IN THE WAY, anywhere except the
    // place that is about to be filled.
    //
    // WHY ANYTHING NEEDS THIS. The engine will not exchange two items whose
    // footprints differ: asked to put one onto the other's cell it answers
    // TRUE and does nothing, or slides the item into the first free cell it
    // finds, which is at the top of the box. Measured on the client
    // 2026-09-25 -- a plain move to an EMPTY cell in the same log reported
    // itself correctly, so the reading is not the unreliable part; the move
    // onto a taken cell is. The owner saw it as "the horizontal one flies to
    // the top of the box" and "as if it did not fit".
    //
    // So the place is emptied first and the engine is only ever asked for a
    // cell that is already free. The item parked here does not stay: it has a
    // change of its own coming in the same burst.
    static bool Somewhere(EntityAI holder, EntityAI e, int keepRow, int keepCol, int keepW, int keepH, out InventoryLocation spot)
    {
        if (!holder || !holder.GetInventory() || !e)
            return false;
        CargoBase cargo = holder.GetInventory().GetCargo();
        if (!cargo)
            return false;
        int ew;
        int eh;
        if (!SizeOf(e, ew, eh))
            return false;
        for (int r = 0; r + eh <= cargo.GetHeight(); r++)
        {
            for (int c = 0; c + ew <= cargo.GetWidth(); c++)
            {
                if (Overlaps(r, c, ew, eh, keepRow, keepCol, keepW, keepH))
                    continue;
                if (!Clear(holder, r, c, ew, eh, e, null))
                    continue;
                // It keeps the way round it already lies: the rectangle was
                // measured against that, so putting it down turned the other
                // way would be a different rectangle.
                spot.SetCargo(holder, e, 0, r, c, e.GetInventory().GetFlipCargo());
                return true;
            }
        }
        return false;
    }

    // The first item standing under the rectangle `e` would occupy if it were
    // dropped at this cell -- its own body excepted. Both halves ask this
    // before a drop, so that a drag whose tail lies over a neighbour is
    // recognised as the swap it is instead of a move that would push the
    // neighbour out.
    // THE SHAPE IT WILL HAVE WHERE IT IS GOING, not the one it has where it
    // stands.
    //
    // `SizeOf` reads the turn off the item's CURRENT location, which is right
    // for asking "what does it cover now" and wrong for asking "what will it
    // cover there". A rag lying upright at 23,3 and dropped TURNED onto 17,0
    // covers three columns of row 17 -- but measured by its current stance it
    // covers three rows of column 0, and the third of those is somebody else's
    // cell. The drop was then sent as an exchange with an item the player
    // never aimed at, and they read "these two cannot trade places" while
    // putting something on an empty cell (owner, 2026-09-26, measured from the
    // client: vanilla asked for a MOVE to 17,0 flip true and we turned it into
    // a swap).
    //
    // `wantFlip` is 1 or 0 for a known destination, -1 for "however it lies
    // now" -- which is what every caller that is not placing it wants.
    static bool SizeFor(EntityAI e, int wantFlip, out int w, out int h)
    {
        if (!SizeOf(e, w, h))
        {
            // NOT IN A CARGO -- ON A SLOT, OR IN THE PLAYER'S HANDS -- so the
            // cargo cannot say what it covers, and the callers assumed one
            // cell: an AKM taken off the weapon rack was measured 1x1, passed
            // the geometry for row 48 of a fifty-row box, and the engine's
            // LOCAL move put its three rows there without a word (measured
            // 2026-09-26). The config knows the unturned shape.
            if (!e)
                return false;
            GetGame().GetInventoryItemSize(InventoryItem.Cast(e), w, h);
            if (w <= 0 || h <= 0)
                return false;
            if (wantFlip == 1)
            {
                int cfgTurned = w;
                w = h;
                h = cfgTurned;
            }
            return true;
        }
        if (wantFlip < 0)
            return true;
        bool now = Flipped(e);
        bool want = wantFlip == 1;
        if (now == want)
            return true;
        int turned = w;
        w = h;
        h = turned;
        return true;
    }

    static EntityAI InTheWay(EntityAI holder, EntityAI e, int row, int col, int wantFlip = -1)
    {
        if (!holder || !e)
            return null;
        int w;
        int h;
        bool known = SizeFor(e, wantFlip, w, h);
        if (!known)
        {
            w = 1;
            h = 1;
        }
        for (int r = row; r < row + h; r++)
        {
            for (int c = col; c < col + w; c++)
            {
                EntityAI sitting = Occupant(holder, r, c, e);
                if (sitting)
                    return sitting;
            }
        }
        return null;
    }

    // WHERE AN ITEM STANDS, ASKED OF THE ITEM.
    //
    // NOT of `CargoBase.GetItemRowCol`. That one was recorded early on as
    // "answers false but writes correct values", on the strength of a single
    // look; measured properly on 2026-09-25 it answers false and writes
    // NUMBERS THAT ARE NOT THE ITEM'S -- three cans reported in row 0 while
    // their own inventory locations put one of them at 2,3. The occupancy map
    // was built on it, so the map disagreed with the engine, and every check
    // that leant on the map -- "is this cell taken", "where can the displaced
    // item go" -- was answering about a box that did not exist.
    //
    // `GetCurrentInventoryLocation` is what the rest of this file has always
    // used, and it agrees with the engine.
    static bool Where(EntityAI e, out int row, out int col)
    {
        row = -1;
        col = -1;
        if (!e || !e.GetInventory())
            return false;
        InventoryLocation il = new InventoryLocation();
        if (!e.GetInventory().GetCurrentInventoryLocation(il))
            return false;
        if (il.GetType() != InventoryLocationType.CARGO)
            return false;
        row = il.GetRow();
        col = il.GetCol();
        return row >= 0 && col >= 0;
    }

    // Is this item lying turned in its cargo? The flip belongs to the
    // LOCATION, not to the item, so it is read from where the item stands.
    static bool Flipped(EntityAI e)
    {
        if (!e || !e.GetInventory())
            return false;
        InventoryLocation il = new InventoryLocation();
        if (!e.GetInventory().GetCurrentInventoryLocation(il))
            return false;
        if (il.GetType() != InventoryLocationType.CARGO)
            return false;
        return il.GetFlip();
    }

    // Is this rectangle of cells inside the grid and free of everything except
    // the one or two items that are leaving it?
    static bool Clear(EntityAI holder, int row, int col, int w, int h, EntityAI leaving1, EntityAI leaving2)
    {
        if (!holder || !holder.GetInventory() || row < 0 || col < 0 || w <= 0 || h <= 0)
            return false;
        CargoBase cargo = holder.GetInventory().GetCargo();
        if (!cargo)
            return false;
        if (row + h > cargo.GetHeight() || col + w > cargo.GetWidth())
            return false;
        for (int r = row; r < row + h; r++)
        {
            for (int c = col; c < col + w; c++)
            {
                EntityAI sitting = Occupant(holder, r, c, null);
                if (!sitting)
                    continue;
                if (sitting == leaving1 || sitting == leaving2)
                    continue;
                return false;
            }
        }
        return true;
    }

    // Do two rectangles of cells share a cell?
    static bool Overlaps(int r1, int c1, int w1, int h1, int r2, int c2, int w2, int h2)
    {
        if (r1 + h1 <= r2 || r2 + h2 <= r1)
            return false;
        if (c1 + w1 <= c2 || c2 + w2 <= c1)
            return false;
        return true;
    }

    // Both halves of a swap that happened: the record, then every proxy.
    //
    // TOLD IN THE ORDER THEY MOVED, AND THE ORDER IS THE WHOLE POINT. A proxy applies what it is told one move at a time, in the order
    // it arrives. Told about the mover first, it tries to put that item where
    // the other one is STILL standing on its side of the wire -- the move
    // fails, the proxy decides it has parted from the authority and asks for
    // the whole box again. On the screen that is "one of the two did not move,
    // and then the box refreshed" (owner, 2026-09-25), with the server having
    // done everything right.
    //
    // `first` is whichever of the two moved first on the authority, so a
    // proxy replaying the pair sees the cells freed before anything is asked
    // to stand on them. When the two went into each other's places there IS
    // no order that works, and the proxy rebuilds itself -- one blink, and
    // correct.
    protected static void Swapped(OZS_Session s, OZS_Watcher w, EntityAI first, EntityAI second, OZS_Was wasA, OZS_Was wasB)
    {
        s.Touch();
        // Each mover with its own snapshot, whichever of the two went first.
        OZS_Was wasFirst = wasA;
        OZS_Was wasSecond = wasB;
        if (wasB.m_E == first)
        {
            wasFirst = wasB;
            wasSecond = wasA;
        }
        // One letter for the pair (review 2026-09-26, B5).
        OZS_Commit.MovedPair(s, first, wasFirst, second, wasSecond);
        s.TellMoved(first, w.m_Uid);
        s.TellMoved(second, w.m_Uid);
    }

    // Would this item's quantity be too much for that place? Asked exactly as
    // vanilla asks it: only of an item that can be split, and answered by the
    // item itself, because the cap belongs to the pair (item, slot).
    static bool Holds_Quantity(EntityAI e, int slot)
    {
        ItemBase item = ItemBase.Cast(e);
        if (!item || !item.CanBeSplit())
            return true;
        return item.GetQuantity() <= item.GetTargetQuantityMax(slot);
    }

    // A move, and then the question "did it actually move". Nothing here
    // trusts TakeToDst's return value; the item is asked where it is.
    // TOLD TO THE CLIENTS, AND THEN TOLD WHAT IT HOLDS. `RemoteObjectTreeCreate`
    // announces an entity the network had forgotten, and the copy a client
    // builds for it starts from the CONFIG: a stack split from six to three
    // inside the box came back into the player's shirt drawn as six, while
    // the server held three, and stayed six until the next change of any
    // synchronised variable brought the number along (measured 2026-09-26:
    // the owner's "six rags again"; a change of cleanness on the server
    // redrew it as three). So every entity of the announced tree is marked
    // dirty right after, and the next frame's sync carries quantity, wet,
    // cleanness and the rest to the copies just made.
    static void Announce(EntityAI e)
    {
        if (!e)
            return;
        GetGame().RemoteObjectTreeCreate(e);
        array<EntityAI> nodes = new array<EntityAI>();
        array<int> parents = new array<int>();
        OZS_Records.Flatten(e, -1, nodes, parents);
        for (int i = 0; i < nodes.Count(); i++)
        {
            if (nodes.Get(i))
                nodes.Get(i).SetSynchDirty();
        }
    }

    static bool Put(EntityAI e, InventoryLocation dst)
    {
        if (!e || !e.GetInventory())
            return false;
        if (Sits(e, dst))
            return true;
        InventoryLocation now = new InventoryLocation();
        if (!e.GetInventory().GetCurrentInventoryLocation(now))
            return false;
        // A SLOT HAS TO BE NAMED WITH THE CALL THAT TAKES A SLOT.
        //
        // `TakeToDst` carries an attachment location with a slot id in it and
        // then ignores it: it hangs the item on the FIRST slot that will take
        // it. Measured against the owner 2026-09-25 -- dropping on the third
        // slot put the weapon in the first, the fourth put it in the second,
        // which is that rule exactly as the earlier slots filled up.
        // `TakeEntityAsAttachmentEx` is the one that is told which slot.
        bool took = false;
        if (dst.GetType() == InventoryLocationType.ATTACHMENT && dst.GetParent())
            took = dst.GetParent().GetInventory().TakeEntityAsAttachmentEx(InventoryMode.LOCAL, e, dst.GetSlot());
        else
            took = e.GetInventory().TakeToDst(InventoryMode.LOCAL, now, dst);
        if (took)
            took = Sits(e, dst);
        // THREE ANSWERS TO ONE QUESTION, SIDE BY SIDE.
        //
        // For an attachment there are two ways to ask where an item is, and
        // they need not agree: the ITEM's own `GetCurrentInventoryLocation`,
        // and the CONTAINER's slots walked until one of them holds it. The
        // second cannot be wrong -- it is the container's own table -- so a
        // disagreement names the liar. Without this the slot the weapon asked
        // for, the slot it reports, and the slot it is actually on were three
        // unknowns and every fix was a guess (2026-09-25).
        if (dst.GetType() == InventoryLocationType.ATTACHMENT && dst.GetParent())
        {
            InventoryLocation says = new InventoryLocation();
            string reported = "nothing";
            if (e.GetInventory().GetCurrentInventoryLocation(says))
                reported = "type " + says.GetType().ToString() + " slot " + says.GetSlot().ToString();
            int truly = InventorySlots.INVALID;
            int slots = dst.GetParent().GetInventory().GetAttachmentSlotsCount();
            for (int q = 0; q < slots; q++)
            {
                int qid = dst.GetParent().GetInventory().GetAttachmentSlotId(q);
                if (dst.GetParent().GetInventory().FindAttachment(qid) == e)
                {
                    truly = qid;
                    break;
                }
            }
            OZ_Log.Dbg("storage: proxy: attach " + e.GetType() + ": asked " + dst.GetSlot().ToString() + ", the item says " + reported + ", the container has it on " + truly.ToString() + "; the call said " + took.ToString());
        }
        // THE LAST STEP WITHOUT A WITNESS. Every layer above this one says
        // what it decided and why; the move itself said nothing, so a refusal
        // that came from here looked identical to one that came from the
        // geometry. What it wanted, what it was told, and where the item
        // actually is afterwards -- all three, or the next failure is another
        // evening of guessing (2026-09-25).
        if (!took)
        {
            InventoryLocation after = new InventoryLocation();
            string where = "nowhere";
            if (e.GetInventory().GetCurrentInventoryLocation(after))
                where = Spot(after);
            OZ_Log.Error("storage: proxy: " + e.GetType() + " would not go from " + Spot(now) + " to " + Spot(dst) + " (the move said " + took.ToString() + "); it is at " + where);
            // AND THE TWO OPINIONS THAT DISAGREE, SIDE BY SIDE.
            //
            // A refused move means our map of the box and the engine's differ
            // about that place. `LocationCanAddEntity` is the engine's own
            // verdict on this very location -- so a `false` there says the
            // engine thinks the cells are taken, and a `true` says the refusal
            // came from somewhere else entirely. The listing under it is what
            // WE believe is standing where, and a cell the two disagree about
            // names the item whose position we read wrongly (2026-09-25).
            EntityAI holder = dst.GetParent();
            if (holder && holder.GetInventory())
            {
                string engine = holder.GetInventory().LocationCanAddEntity(dst).ToString();
                OZ_Log.Error("storage: proxy: the engine says LocationCanAddEntity=" + engine + " for that place; we see " + Grid(holder));
            }
        }
        return took;
    }

    // The box as we read it: every cargo item, where we think it stands and
    // what rectangle we think it covers.
    static string Grid(EntityAI holder)
    {
        if (!holder || !holder.GetInventory())
            return "no box";
        CargoBase cargo = holder.GetInventory().GetCargo();
        if (!cargo)
            return "no cargo";
        string text = cargo.GetWidth().ToString() + "x" + cargo.GetHeight().ToString();
        for (int i = 0; i < cargo.GetItemCount(); i++)
        {
            EntityAI e = cargo.GetItem(i);
            if (!e)
                continue;
            int r;
            int c;
            bool told = Where(e, r, c);
            int w;
            int h;
            bool sized = SizeOf(e, w, h);
            text = text + " | " + e.GetType() + " " + r.ToString() + "," + c.ToString();
            text = text + " " + w.ToString() + "x" + h.ToString();
            // WHETHER THE ENGINE EVEN ANSWERED. Its `false` with correct
            // numbers is known and harmless; a `false` with numbers it never
            // wrote would put this item at 0,0 on our map and leave its real
            // cells looking free, which is exactly the failure being hunted.
            if (!told)
                text = text + " (no place)";
            if (!sized)
                text = text + " (no size)";
            if (Flipped(e))
                text = text + " flipped";
        }
        return text;
    }

    // One location, in words, for a log line.
    static string Spot(InventoryLocation il)
    {
        if (!il)
            return "nothing";
        if (il.GetType() == InventoryLocationType.ATTACHMENT)
            return "slot " + il.GetSlot().ToString();
        if (il.GetType() != InventoryLocationType.CARGO)
            return "type " + il.GetType().ToString();
        string flip = "";
        if (il.GetFlip())
            flip = " flipped";
        return il.GetRow().ToString() + "," + il.GetCol().ToString() + flip;
    }

    // The footprint an item occupies in its parent's cargo, AS IT LIES.
    //
    // `GetItemSize` answers the item's own width and height, which is not the
    // rectangle it covers: an item lying FLIPPED covers the transpose of it.
    // Every occupancy test in this file runs through here, so the flip is
    // applied once, in one place -- without it a turned rifle was measured
    // against the wrong cells and the box reported "taken" over cells that
    // were visibly empty (owner, 2026-09-25).
    //
    // False when the item has no cargo footprint at all -- an attachment, or
    // an item the cargo does not list.
    static bool SizeOf(EntityAI e, out int w, out int h)
    {
        w = 0;
        h = 0;
        if (!e)
            return false;
        EntityAI holder = e.GetHierarchyParent();
        if (!holder || !holder.GetInventory())
            return false;
        CargoBase cargo = holder.GetInventory().GetCargo();
        if (!cargo)
            return false;
        for (int i = 0; i < cargo.GetItemCount(); i++)
        {
            if (cargo.GetItem(i) != e)
                continue;
            // The return value of GetItemSize is not tested on purpose: it
            // answers false while writing correct numbers, exactly as
            // GetItemRowCol does (measured 2026-09-25).
            cargo.GetItemSize(i, w, h);
            if (w <= 0 || h <= 0)
                return false;
            if (Flipped(e))
            {
                int turned = w;
                w = h;
                h = turned;
            }
            return true;
        }
        return false;
    }

    // Is the item exactly at this location?
    static bool Sits(EntityAI e, InventoryLocation want)
    {
        InventoryLocation now = new InventoryLocation();
        if (!e || !e.GetInventory() || !e.GetInventory().GetCurrentInventoryLocation(now))
            return false;
        if (now.GetParent() != want.GetParent() || now.GetType() != want.GetType())
            return false;
        if (want.GetType() == InventoryLocationType.ATTACHMENT)
            return now.GetSlot() == want.GetSlot();
        if (want.GetType() == InventoryLocationType.CARGO)
            return now.GetRow() == want.GetRow() && now.GetCol() == want.GetCol();
        return true;
    }

    // Put it back. Used only to undo a half-done exchange.
    static void Sit(EntityAI e, InventoryLocation home)
    {
        if (!e || !e.GetInventory() || Sits(e, home))
            return;
        InventoryLocation now = new InventoryLocation();
        if (!e.GetInventory().GetCurrentInventoryLocation(now))
            return;
        e.GetInventory().TakeToDst(InventoryMode.LOCAL, now, home);
    }

    // ---- the questions both halves ask -----------------------------------

    // The item whose footprint covers this cell of `holder`, or null. Asked of
    // the engine's own cargo, item by item: a cargo item is named by its
    // TOP-LEFT cell plus a width and a height, so "is this cell taken" is not
    // a lookup, it is a walk. Both halves ask it -- the proxy so a drag that
    // cannot work is never drawn, the authority because its answer is the one
    // that counts.
    //
    // Cells come from each item's own inventory location, never from
    // `CargoBase.GetItemRowCol` -- see Where().
    static EntityAI Occupant(EntityAI holder, int row, int col, EntityAI except)
    {
        if (!holder || !holder.GetInventory())
            return null;
        CargoBase cargo = holder.GetInventory().GetCargo();
        if (!cargo)
            return null;
        for (int i = 0; i < cargo.GetItemCount(); i++)
        {
            EntityAI sitting = cargo.GetItem(i);
            if (!sitting || sitting == except)
                continue;
            int r;
            int c;
            if (!Where(sitting, r, c))
                continue;
            int w;
            int h;
            // `GetItemSize`'s return value is not tested on purpose: it
            // answers false while writing correct numbers, and testing it
            // made this whole walk skip every item, so the occupancy check
            // passed everything and a rifle vanished (2026-09-25). The VALUES
            // are checked instead: a size of zero is the only unusable
            // answer. The item's PLACE, on the other hand, is never read from
            // the cargo index -- see Where().
            //
            // AND THE FLIP: an item lying turned covers the transpose of its
            // own size, so the rectangle compared here is the one it really
            // stands on, not the one it would stand on unturned.
            cargo.GetItemSize(i, w, h);
            if (w <= 0 || h <= 0)
                continue;
            if (Flipped(sitting))
            {
                int turned = w;
                w = h;
                h = turned;
            }
            if (row < r || row >= r + h)
                continue;
            if (col < c || col >= c + w)
                continue;
            return sitting;
        }
        return null;
    }

    // Is this a place at all? Asked of the engine's own cargo and slot
    // declarations, never guessed from the config.
    static bool Fits(EntityAI parent, int lt, int slot, int row, int col)
    {
        if (!parent || !parent.GetInventory())
            return false;
        if (lt == InventoryLocationType.ATTACHMENT)
            // THE CONTAINER'S OWN SLOTS, NOT THE SLOTS IT COULD HANG IN.
            //
            // The engine keeps two lists and names them almost identically.
            // `HasInventorySlot` answers "can THIS entity be attached to a
            // slot with that id" -- an item-side question, and for a box the
            // answer is no whatever you ask about. `HasAttachmentSlot` is the
            // container-side one. Asking the wrong one made the box refuse
            // every weapon put on its rack, while its config declared four
            // slots and the engine resolved all four names (measured
            // 2026-09-25: config declares 4, HasInventorySlot said no to each,
            // GetSlotIdCount answered 1 -- that 1 being where the BOX belongs).
            return parent.GetInventory().HasAttachmentSlot(slot);
        CargoBase cargo = parent.GetInventory().GetCargo();
        if (!cargo)
            return false;
        if (row < 0 || col < 0)
            return false;
        if (row >= cargo.GetHeight() || col >= cargo.GetWidth())
            return false;
        return true;
    }

    // Does `holder` already contain `maybe`, at any depth? The engine refuses
    // a cycle too, but not before the move has been half made.
    static bool Holds(EntityAI holder, EntityAI maybe)
    {
        EntityAI up = maybe;
        while (up)
        {
            if (up == holder)
                return true;
            up = up.GetHierarchyParent();
        }
        return false;
    }
}
