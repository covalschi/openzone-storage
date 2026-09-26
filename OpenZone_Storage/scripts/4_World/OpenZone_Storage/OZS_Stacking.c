// Stacking, when the stacks are in a box.
// Design: docs/specs/2026-09-24-storage-proxy-inventory-design.md §5, §10.3.
//
// The vanilla inventory merges two stacks with `entity.CombineItemsClient(other)`
// (icon.c:495, :531, :750). That call serialises THE ENTITIES THEMSELVES into
// `ScriptInputUserData` -- which is to say by network id -- and an item in a
// proxy has none. So it cannot work there, and the server is asked instead.
//
// AS EVERYWHERE ELSE, THIS DECIDES ONLY FOR ITEMS IN A BOX. With no box open
// the override is one integer test and a call to super; with a box open it
// compares hierarchy roots by pointer. Two stacks of bandages in the player's
// own pockets go to vanilla untouched.
//
// The merge is not instant even when it is ours: the engine does it on the
// authoritative container, with every mod's `CanBeCombined` in force, and the
// proxy is told the result. That is the trade §10.3 records -- a round trip
// for correctness that no script of ours could reproduce.
modded class ItemBase
{
    // The authority's handle for this item while it stands in a box; see
    // OZS_Handle.c for why it is here and not on EntityAI.
    int m_OZS_Handle;

    // The engine's own word on whether a piece of this size may be cut off
    // this stack. `ShouldSplitQuantity` is protected, and the authority's
    // split (OZS_Ops.SplitLocal) has to ask it from outside; nothing is
    // decided here.
    bool OZS_ShouldSplit(float piece)
    {
        return ShouldSplitQuantity(piece);
    }

    // ---- the witness ------------------------------------------------
    //
    // Who takes an item out of an authoritative box without asking.
    //
    // `EEParentedFrom` is NOT the event: it never fired once for an item
    // leaving a box's cargo, not even for one taken out by hand (measured on
    // the stand 2026-09-25 -- and the override itself was proven to be in the
    // chain, so the silence was the event's, not the hook's). Cargo moves
    // announce themselves here instead.
    //
    // DELETION IS NOT WATCHED, and was. The question it was put there to
    // answer -- is the missing item destroyed, or does it simply leave? -- came
    // back LEAVES, and the hook then had nothing left to catch but the mod's
    // own deliberate deletions: a stack emptied by combining is deleted on
    // purpose, and every one of them was reported as an error (2026-09-25).
    override void OnItemLocationChanged(EntityAI old_owner, EntityAI new_owner)
    {
        OZS_Watchdog.Moved(this, old_owner, new_owner);
        super.OnItemLocationChanged(old_owner, new_owner);
    }

    override void CombineItemsClient(EntityAI entity2, bool use_stack_max = true)
    {
        if (!OZS_Mirrors.None())
        {
            OZS_Mirror mine = OZS_Mirrors.Of(this);
            OZS_Mirror theirs = OZS_Mirrors.Of(entity2);
            if (mine && mine == theirs)
            {
                OZS_Mirrors.s_Via = "CombineItemsClient";
                mine.DragCombine(this, entity2);
                return;
            }
            // ONE STACK IN THE BOX AND ONE OUTSIDE (owner, 2026-09-26: a
            // round in the pocket, the same round in the box). Refused until
            // then as "a move of a part of a stack". It is not a move of
            // either entity: the CONTENTS cross, the way vanilla's own
            // combine moves them, and the boundary keeps section 7's order
            // for each direction (OZS_Boundary.StackIn / StackOut). `this`
            // is the stack that receives and `entity2` the one dropped onto
            // it, as vanilla has it.
            //
            // TWO DIFFERENT BOXES IS NOT THIS EITHER (as for the swap in
            // OZS_Player.CrossBoundarySwap): the stack named by network id
            // would have none, and the server would answer "no such item".
            if (mine && theirs)
                return;
            if (mine)
            {
                OZS_Mirrors.s_Via = "CombineItemsClient";
                mine.StackIn(entity2, this);
                return;
            }
            if (theirs)
            {
                OZS_Mirrors.s_Via = "CombineItemsClient";
                theirs.StackOut(entity2, this);
                return;
            }
        }
        super.CombineItemsClient(entity2, use_stack_max);
    }

    // RIGHT-CLICK IS THE SPLIT GESTURE, AND IT NEVER TOUCHES THE SPLIT METHODS.
    //
    // `Icon.MouseClick` calls `OnRightClick` on the item (icon.c:826-846), and
    // vanilla's `ItemBase.OnRightClick` does not split on the client at all:
    // it works out where the new stack should go and sends the request to the
    // server as a `ScriptInputUserData` that names the item BY ENTITY
    // (itembase.c:2124-2186). An item in a box has no network id for that
    // message to carry, so the server resolves nothing and the click does
    // nothing -- silently, which is how the owner met it (2026-09-25).
    //
    // Vanilla's own rule for where the new stack goes is "a free place in the
    // same parent first", and here the parent is the box. So the operation
    // travels with no cell named and the authority chooses one, which is the
    // same answer by a shorter road.
    override void OnRightClick()
    {
        if (!OZS_Mirrors.None())
        {
            OZS_Mirror m = OZS_Mirrors.Of(this);
            if (m)
            {
                // The engine's own word, asked of the proxy: for an ammo pile
                // that is `canBeSplit` in its config AND more than one round,
                // which is why a weapon magazine never divides.
                if (CanBeSplit())
                {
                    // THE SAME PARENT FIRST, as vanilla's own rule has it
                    // (itembase.c:2124): a stack lying in a bag hung in the
                    // box splits into that bag while it has room, and into
                    // the box's own grid otherwise. Asked of the proxy's
                    // container, which has the authority's grid.
                    EntityAI holder = m.m_Box;
                    EntityAI parent = GetHierarchyParent();
                    if (parent && parent != m.m_Box && parent.GetInventory())
                    {
                        InventoryLocation room = new InventoryLocation();
                        if (parent.GetInventory().FindFreeLocationFor(this, FindInventoryLocationType.CARGO, room))
                            holder = parent;
                    }
                    OZS_Mirrors.s_Via = "OnRightClick";
                    m.Split(this, holder, OZS_Const.SPLIT_HALF, InventoryLocationType.CARGO, -1, -1, -1, 0);
                }
                // Vanilla's message is not sent for a box item either way:
                // there is nothing in it the server could resolve.
                return;
            }
        }
        super.OnRightClick();
    }

    // SPLITTING INSIDE THE BOX IS A SPLIT; ACROSS ITS EDGE IT IS STILL A MOVE.
    //
    // Both halves of a split that stays in the box are the authority's items,
    // so the whole thing is one operation there and the vanilla call that
    // does it -- the one `Magazine` overrides to move cartridges singly -- runs
    // on the real entities.
    //
    // A split that CROSSES the boundary would be a split and a crossing in one
    // gesture, and the crossing is the half that can lose something (design
    // section 7). It keeps what it had: the whole stack moves. That is visible
    // and recoverable, where half a stack lost on the way is neither.
    override void SplitIntoStackMaxClient(EntityAI destination_entity, int slot_id)
    {
        if (!OZS_Mirrors.None())
        {
            OZS_Mirror mine = OZS_Mirrors.Of(this);
            OZS_Mirror theirs = OZS_Mirrors.Of(destination_entity);
            if (mine && mine == theirs)
            {
                int lt = InventoryLocationType.CARGO;
                if (slot_id >= 0)
                    lt = InventoryLocationType.ATTACHMENT;
                OZS_Mirrors.s_Via = "SplitIntoStackMax";
                mine.Split(this, destination_entity, OZS_Const.SPLIT_MAX, lt, slot_id, -1, -1, 0);
                return;
            }
            if (mine || theirs)
            {
                OZS_Mirror m = mine;
                if (!m)
                    m = theirs;
                OZS_Mirrors.s_Via = "SplitIntoStackMax";
                m.DragTo(this, destination_entity, InventoryLocationType.CARGO, slot_id, -1, -1);
                return;
            }
        }
        super.SplitIntoStackMaxClient(destination_entity, slot_id);
    }

    override void SplitItemToInventoryLocation(notnull InventoryLocation dst)
    {
        if (!OZS_Mirrors.None())
        {
            OZS_Mirror mine = OZS_Mirrors.Of(this);
            OZS_Mirror there = OZS_Mirrors.At(dst);
            if (mine && mine == there)
            {
                int flip = 0;
                if (dst.GetFlip())
                    flip = 1;
                OZS_Mirrors.s_Via = "SplitToLocation";
                mine.Split(this, dst.GetParent(), OZS_Const.SPLIT_HALF, dst.GetType(), dst.GetSlot(), dst.GetRow(), dst.GetCol(), flip);
                return;
            }
            if (mine || there)
            {
                OZS_Mirror m = mine;
                if (!m)
                    m = there;
                InventoryLocation src = new InventoryLocation();
                if (!GetInventory().GetCurrentInventoryLocation(src))
                    return;
                OZS_Mirrors.s_Via = "SplitToLocation";
                m.Drag(src, dst);
                return;
            }
        }
        super.SplitItemToInventoryLocation(dst);
    }
}
