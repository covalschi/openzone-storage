// One root of a box on the wire (design 2026-09-19, section 2): first the
// descriptor of the whole subtree -- every node's class, parent, cell and
// the list-level state the bridge indexes -- then the bodies of the nodes in
// the same order: chambers, OnStoreSave, magazine, health per zone,
// lifetime. The bridge parses the descriptor and keeps the bodies as bytes.
//
// Reading creates every node first (parents come first in the descriptor),
// then applies the bodies, then checks the marker. A root that cannot be
// read is deleted whole and the caller parks it with the bridge; nothing
// half-read is ever kept, so a degraded state never gets written back.
//
// The trees are built the way vanilla's ReplaceItemWithNewLambdaBase builds
// them (measured 2026-09-18): a container that holds cargo is created LOCAL
// on the ground, its children local under it, the whole tree moved into its
// cell a frame later in InventoryMode.LOCAL and published once with
// RemoteObjectTreeCreate. A script move into a container created the same
// frame leaves an undeletable entity behind; this order does not.

// One move to run next frame: the container into its cell.
class OZS_Move
{
    EntityAI item;
    EntityAI parent;
    int row;
    int col;
    bool flip;
    int children;
    // Where this item belongs: an ATTACHMENT SLOT or a cargo cell. It used to
    // be missing, and everything with contents was moved as cargo: a jacket
    // with full pockets came back out of its Body slot and was left standing
    // wherever the restore had built it (measured 2026-09-23 on the personal
    // stash, where every garment is a slot).
    //
    // toSlot IS A SEPARATE FLAG AND NOT `slot >= 0`: A SLOT ID CAN BE
    // NEGATIVE. PlateCarrierPouches on a PlateCarrierVest sits in slot
    // -230399667, and a sign test sent it down the cargo path and left it on
    // the ground -- while a jacket in Body (5119774) went home fine. Measured
    // 2026-09-23; do not fold this back into one field.
    bool toSlot;
    int slot;
    // true for the move into a networked parent (the box or a networked
    // item): that is the move which publishes the tree.
    bool publish;

    void OZS_Move(EntityAI i, EntityAI p, int r, int c, bool f, int n, bool pub, bool inSlot = false, int sl = -1)
    {
        item = i;
        parent = p;
        toSlot = inSlot;
        slot = sl;
        row = r;
        col = c;
        flip = f;
        children = n;
        publish = pub;
    }
}

// One node of a root's subtree as the descriptor names it, plus what the
// reader made of it.
class OZS_Node
{
    int parent;
    string cls;
    int lt;
    int slot;
    int row;
    int col;
    int flip;
    float health;
    float quantity;
    int liquid;
    int ammo;
    int hasBlob;

    EntityAI made;
    // The node's entity is local (unpublished): its children are created
    // local too.
    bool isLocal;
    // Created on the ground beside the box, moved into its cell next frame.
    bool ground;
    // Could not be created where it belongs: a throwaway that consumes the
    // body and is deleted.
    bool standIn;
    int cargoKids;
}

class OZS_Records
{
    static int s_Created;
    static int s_Missed;
    static int s_LoadFails;

    // AN ENTITY ON ITS WAY OUT IS NOT IN THE BOX. `ObjectDelete` takes effect
    // at the end of the frame, and until then the engine still lists the
    // item in its parent's cargo: a stack emptied by a combine and deleted
    // was still counted, and still written into its container's blob by the
    // letter posted in the same frame -- a stack of nothing in the record,
    // rebuilt by the next open (review 2026-09-26). Both walks skip it.
    static int CountTree(EntityAI e)
    {
        if (!e || e.IsSetForDeletion())
            return 0;
        int n = 1;
        GameInventory inv = e.GetInventory();
        if (!inv)
            return n;
        int ac = inv.AttachmentCount();
        for (int a = 0; a < ac; a++)
            n = n + CountTree(inv.GetAttachmentFromIndex(a));
        CargoBase cargo = inv.GetCargo();
        if (cargo)
        {
            int cc = cargo.GetItemCount();
            for (int c = 0; c < cc; c++)
                n = n + CountTree(cargo.GetItem(c));
        }
        return n;
    }

    // ---- writing ----

    // The subtree in depth-first order: the node, its attachments, its cargo.
    static void Flatten(EntityAI e, int parent, array<EntityAI> nodes, array<int> parents)
    {
        if (!e || e.IsSetForDeletion())
            return;
        int me = nodes.Count();
        nodes.Insert(e);
        parents.Insert(parent);
        GameInventory inv = e.GetInventory();
        if (!inv)
            return;
        int ac = inv.AttachmentCount();
        for (int a = 0; a < ac; a++)
            Flatten(inv.GetAttachmentFromIndex(a), me, nodes, parents);
        CargoBase cargo = inv.GetCargo();
        if (cargo)
        {
            int cc = cargo.GetItemCount();
            for (int c = 0; c < cc; c++)
                Flatten(cargo.GetItem(c), me, nodes, parents);
        }
    }

    // Returns the number of entities written.
    static int WriteRoot(FileSerializer f, EntityAI root, int newRow = -1, int newCol = -1)
    {
        array<EntityAI> nodes = new array<EntityAI>();
        array<int> parents = new array<int>();
        Flatten(root, -1, nodes, parents);
        f.Write(nodes.Count());
        for (int i = 0; i < nodes.Count(); i++)
            WriteDescriptor(f, nodes.Get(i), parents.Get(i), i == 0, newRow, newCol);
        for (int b = 0; b < nodes.Count(); b++)
            WriteBody(f, nodes.Get(b));
        return nodes.Count();
    }

    protected static void WriteDescriptor(FileSerializer f, EntityAI e, int parent, bool isRoot, int newRow, int newCol)
    {
        InventoryLocation loc = new InventoryLocation();
        int lt = -1;
        int slot = -1;
        int row = 0;
        int col = 0;
        int flip = 0;
        GameInventory inv = e.GetInventory();
        if (inv && inv.GetCurrentInventoryLocation(loc))
        {
            lt = loc.GetType();
            slot = loc.GetSlot();
            row = loc.GetRow();
            col = loc.GetCol();
            if (loc.GetFlip())
                flip = 1;
        }
        // The sort hands the root a new cell. THE WAY ROUND STAYS: the planner
        // measured the item as it lies (OZS_Ops.SizeOf swaps a turned item's
        // sides), so writing it down unturned stood a rag planned three wide
        // across the cells reserved for its neighbours -- the engine then
        // refused the cell and the root was parked as no_room (review
        // 2026-09-26, B2).
        if (isRoot && newRow >= 0 && newCol >= 0 && lt == InventoryLocationType.CARGO)
        {
            row = newRow;
            col = newCol;
        }
        float quantity = 0;
        int liquid = 0;
        ItemBase item = ItemBase.Cast(e);
        if (item)
        {
            if (item.HasQuantity())
                quantity = item.GetQuantity();
            liquid = item.GetLiquidType();
        }
        int ammo = 0;
        Magazine mag = Magazine.Cast(e);
        if (mag)
            ammo = mag.GetAmmoCount();
        f.Write(parent);
        f.Write(e.GetType());
        f.Write(lt);
        f.Write(slot);
        f.Write(row);
        f.Write(col);
        f.Write(flip);
        f.Write(e.GetHealth("", "Health"));
        f.Write(quantity);
        f.Write(liquid);
        f.Write(ammo);
        f.Write(1);
    }

    static void WriteBody(FileSerializer f, EntityAI e)
    {
        Weapon_Base w = Weapon_Base.Cast(e);
        int muzzles = 0;
        if (w)
            muzzles = w.GetMuzzleCount();
        f.Write(muzzles);
        for (int m = 0; m < muzzles; m++)
        {
            bool empty = w.IsChamberEmpty(m);
            f.Write(empty);
            if (!empty)
            {
                float cd;
                string ct;
                w.GetCartridgeInfo(m, cd, ct);
                f.Write(cd);
                f.Write(ct);
            }
            int ic = w.GetInternalMagazineCartridgeCount(m);
            f.Write(ic);
            for (int k = 0; k < ic; k++)
            {
                float id;
                string it;
                w.GetInternalMagazineCartridgeInfo(m, k, id, it);
                f.Write(id);
                f.Write(it);
            }
        }
        e.OnStoreSave(f);
        Magazine mag = Magazine.Cast(e);
        bool isMag = false;
        if (mag)
            isMag = true;
        f.Write(isMag);
        if (mag)
        {
            int ammo = mag.GetAmmoCount();
            bool pile = mag.IsAmmoPile();
            f.Write(ammo);
            f.Write(pile);
            if (!pile)
            {
                for (int q = 0; q < ammo; q++)
                {
                    float qd;
                    string qt;
                    mag.GetCartridgeAtIndex(q, qd, qt);
                    f.Write(qd);
                    f.Write(qt);
                }
            }
        }
        f.Write(e.GetHealth("", "Health"));
        TStringArray zones = new TStringArray();
        e.GetDamageZones(zones);
        f.Write(zones.Count());
        for (int z = 0; z < zones.Count(); z++)
        {
            f.Write(zones.Get(z));
            f.Write(e.GetHealth(zones.Get(z), "Health"));
        }
        f.Write(e.GetLifetime());
    }

    // ---- reading ----

    static bool ReadDescriptor(FileSerializer f, OZS_Node n)
    {
        if (!f.Read(n.parent))
            return false;
        if (!f.Read(n.cls))
            return false;
        if (!f.Read(n.lt) || !f.Read(n.slot) || !f.Read(n.row) || !f.Read(n.col) || !f.Read(n.flip))
            return false;
        if (!f.Read(n.health) || !f.Read(n.quantity) || !f.Read(n.liquid) || !f.Read(n.ammo) || !f.Read(n.hasBlob))
            return false;
        return true;
    }

    // Reads one root out of the wire and builds it in the box. On success
    // `created` counts the real entities and the moves for the ground-built
    // containers are queued; on failure every entity this root created is
    // deleted, `why` says what happened and `type` names the root's class.
    // The entity that became node 0 of the last root read. The open job puts
    // the roots into the box in the order the file lists them, and the proxy
    // design needs that order kept: a commit names a root by its POSITION in
    // the box's record, and the bridge numbers them the same way.
    static EntityAI s_LastRoot;

    static bool ReadRoot(FileSerializer f, EntityAI box, int saveVer, int m0, int m1, int m2, int m3, array<ref OZS_Move> moves, out int created, out string why, out string type)
    {
        created = 0;
        type = "";
        s_LastRoot = null;
        int count;
        if (!f.Read(count) || count < 1 || count > 100000)
        {
            why = "the node count cannot be read";
            return false;
        }
        array<ref OZS_Node> nodes = new array<ref OZS_Node>();
        for (int i = 0; i < count; i++)
        {
            OZS_Node n = new OZS_Node();
            if (!ReadDescriptor(f, n))
            {
                why = "the descriptor of node " + i.ToString() + " cannot be read";
                return false;
            }
            if (i == 0)
            {
                if (n.parent != -1)
                {
                    why = "node 0 is not a root";
                    return false;
                }
                type = n.cls;
            }
            else if (n.parent < 0 || n.parent >= i)
            {
                why = "node " + i.ToString() + " has parent " + n.parent.ToString();
                return false;
            }
            nodes.Insert(n);
        }
        for (int k = 1; k < count; k++)
        {
            OZS_Node kid = nodes.Get(k);
            if (kid.lt == InventoryLocationType.CARGO)
            {
                OZS_Node holder = nodes.Get(kid.parent);
                holder.cargoKids = holder.cargoKids + 1;
            }
        }

        // An AUTHORITY is itself unannounced, so everything built into it must
        // be built local too -- a networked child of an invisible parent is a
        // contradiction the clients resolve badly. The same flag then keeps
        // the moves below from publishing anything.
        OZ_StorageBox asBox = OZ_StorageBox.Cast(box);
        bool boxLocal = asBox && asBox.OZS_IsAuthority();

        // Every node exists before any body is read: parents first.
        for (int c = 0; c < count; c++)
        {
            OZS_Node node = nodes.Get(c);
            EntityAI parent = box;
            bool parentLocal = boxLocal;
            if (node.parent >= 0)
            {
                parent = nodes.Get(node.parent).made;
                parentLocal = nodes.Get(node.parent).isLocal;
            }
            Create(node, parent, parentLocal, box);
            if (!node.made)
            {
                why = "cannot create " + node.cls;
                Cleanup(nodes);
                return false;
            }
            // The root itself found no place (a full box, a returned root
            // whose cell is taken and no cell free): the root is parked
            // again rather than consumed by a stand-in and lost.
            if (c == 0 && node.standIn)
            {
                why = "no room in the box for " + node.cls;
                Cleanup(nodes);
                return false;
            }
        }

        for (int b = 0; b < count; b++)
        {
            OZS_Node nb = nodes.Get(b);
            if (nb.hasBlob == 0)
            {
                ApplyDescriptorState(nb);
                continue;
            }
            if (!ReadBody(f, nb.made, saveVer))
            {
                why = nb.cls + " refused its stored state";
                Cleanup(nodes);
                return false;
            }
        }

        int r0;
        int r1;
        int r2;
        int r3;
        bool markerRead = f.Read(r0) && f.Read(r1) && f.Read(r2) && f.Read(r3);
        if (!markerRead || r0 != m0 || r1 != m1 || r2 != m2 || r3 != m3)
        {
            why = "the marker after the root does not match: the record was read out of step";
            Cleanup(nodes);
            return false;
        }

        // Stand-ins consumed their bodies; they go, with whatever was
        // created local under them.
        int real = 0;
        for (int s = 0; s < count; s++)
        {
            OZS_Node ns = nodes.Get(s);
            if (ns.standIn)
                GetGame().ObjectDelete(ns.made);
            else
                real++;
        }

        // Ground-built containers move into their cells next frame, children
        // before parents (the reverse of the descriptor's order does that).
        for (int m = count - 1; m >= 0; m--)
        {
            OZS_Node nm = nodes.Get(m);
            if (!nm.ground || nm.standIn)
                continue;
            EntityAI into = box;
            bool intoLocal = boxLocal;
            if (nm.parent >= 0)
            {
                into = nodes.Get(nm.parent).made;
                intoLocal = nodes.Get(nm.parent).isLocal;
            }
            bool intoSlot = nm.lt == InventoryLocationType.ATTACHMENT;
            moves.Insert(new OZS_Move(nm.made, into, nm.row, nm.col, nm.flip == 1, nm.cargoKids, !intoLocal, intoSlot, nm.slot));
        }
        created = real;
        s_Created = s_Created + real;
        s_LastRoot = nodes.Get(0).made;
        return true;
    }

    // Where and how a node is created: the same rules the restore has used
    // since the ghost fix, now driven by the descriptor instead of the
    // stream's own recursion. A cargo cell of -1 means "any free cell": a
    // parked root coming back, or a gift from the admin web.
    protected static void Create(OZS_Node n, EntityAI parent, bool parentLocal, EntityAI box)
    {
        EntityAI e = null;
        n.isLocal = false;
        n.ground = false;
        n.standIn = false;
        if (!parent)
        {
            // The parent is a stand-in that could not be made: nothing real
            // to hang on; a stand-in of our own consumes the body below.
        }
        else if (n.cargoKids > 0)
        {
            vector pos = box.GetPosition();
            pos[0] = pos[0] + 2;
            // ECE_NOPERSISTENCY_WORLD IS NOT OPTIONAL HERE, and leaving it
            // out cost the owner a week of "kits keep appearing beside the
            // box" (measured again 2026-09-25, PlateCarrierPouches).
            //
            // This container stands on the ground for one frame before it
            // moves into the box, and for that frame it is an ordinary
            // saveable object. A world save in that frame -- an autosave, a
            // server_stop, a crash -- writes it into the world file. The next
            // boot loads it as a normal, ANNOUNCED item lying beside the box,
            // while the record still describes it inside: one item, two
            // places. The player picks it up, puts it back, and now the
            // record has two.
            //
            // ECE_LOCAL alone does not prevent that. It says "do not tell the
            // network"; persistence is a separate flag, and the authority
            // itself has always carried both (OZS_Authority.Create).
            e = EntityAI.Cast(GetGame().CreateObjectEx(n.cls, pos, ECE_LOCAL | ECE_NOPERSISTENCY_WORLD | ECE_PLACE_ON_SURFACE | ECE_NOLIFETIME));
            n.ground = true;
        }
        else if (n.lt == InventoryLocationType.ATTACHMENT)
        {
            InventoryLocation il = new InventoryLocation();
            il.SetAttachment(parent, null, n.slot);
            if (parentLocal)
                e = GameInventory.LocationCreateLocalEntity(il, n.cls, ECE_IN_INVENTORY, RF_DEFAULT);
            else
                e = GameInventory.LocationCreateEntity(il, n.cls, ECE_IN_INVENTORY, RF_DEFAULT);
        }
        else if (parentLocal)
        {
            if (n.row >= 0)
            {
                InventoryLocation cell = new InventoryLocation();
                cell.SetCargo(parent, null, 0, n.row, n.col, n.flip == 1);
                e = GameInventory.LocationCreateLocalEntity(cell, n.cls, ECE_IN_INVENTORY, RF_DEFAULT);
            }
            if (!e)
            {
                InventoryLocation any = new InventoryLocation();
                if (parent.GetInventory().FindFirstFreeLocationForNewEntity(n.cls, FindInventoryLocationType.CARGO, any))
                    e = GameInventory.LocationCreateLocalEntity(any, n.cls, ECE_IN_INVENTORY, RF_DEFAULT);
            }
        }
        else
        {
            if (n.row >= 0)
                e = parent.GetInventory().CreateEntityInCargoEx(n.cls, 0, n.row, n.col, n.flip == 1);
            if (!e)
                e = parent.GetInventory().CreateEntityInCargo(n.cls);
        }
        if (!e)
        {
            s_Missed++;
            string where = "nowhere";
            if (parent)
                where = parent.GetType();
            OZ_Log.Warn("storage: cannot create " + n.cls + " in " + where + " at " + n.row.ToString() + "," + n.col.ToString() + " (slot " + n.slot.ToString() + ")");
            vector spare = box.GetPosition();
            spare[1] = spare[1] + 50;
            // The same two flags, for the same reason: a stand-in is thrown
            // away a moment later, and a save in that moment would leave it
            // in the world for good -- fifty metres above the box.
            e = EntityAI.Cast(GetGame().CreateObjectEx(n.cls, spare, ECE_LOCAL | ECE_NOPERSISTENCY_WORLD | ECE_NOLIFETIME));
            n.standIn = true;
        }
        n.made = e;
        bool isLoc = n.ground || n.standIn || parentLocal;
        n.isLocal = isLoc;
    }

    // A node without a body (hasBlob = 0) carries only what the descriptor
    // says: health and quantity.
    protected static void ApplyDescriptorState(OZS_Node n)
    {
        if (!n.made || n.standIn)
            return;
        // Health below zero in a bodiless node means "the class default" (a
        // gift from the admin side); zero and above is the health to set.
        if (n.health >= 0)
            n.made.SetHealth("", "Health", n.health);
        ItemBase item = ItemBase.Cast(n.made);
        if (item && item.HasQuantity() && n.quantity > 0)
            item.SetQuantity(n.quantity);
        n.made.SetSynchDirty();
    }

    // Everything this root created goes: the ground-built containers (local,
    // with their children inside), the root when it sits in the box (its
    // children go with it), every stand-in.
    protected static void Cleanup(array<ref OZS_Node> nodes)
    {
        for (int i = 0; i < nodes.Count(); i++)
        {
            OZS_Node n = nodes.Get(i);
            if (!n.made)
                continue;
            bool own = n.ground || n.standIn || i == 0;
            if (!own)
                continue;
            if (!n.made.IsSetForDeletion())
                GetGame().ObjectDelete(n.made);
            n.made = null;
        }
    }

    static int ApplyMoves(array<ref OZS_Move> moves)
    {
        int failed = 0;
        for (int i = 0; i < moves.Count(); i++)
        {
            OZS_Move m = moves.Get(i);
            if (!m.item)
            {
                failed++;
                continue;
            }
            bool placed = false;
            string into = "a container that is gone";
            if (m.parent)
            {
                into = m.parent.GetType();
                InventoryLocation src = new InventoryLocation();
                m.item.GetInventory().GetCurrentInventoryLocation(src);
                InventoryLocation dst = new InventoryLocation();
                if (m.toSlot)
                {
                    dst.SetAttachment(m.parent, m.item, m.slot);
                    placed = m.parent.GetInventory().TakeToDst(InventoryMode.LOCAL, src, dst);
                    if (!placed)
                        placed = m.parent.GetInventory().TakeEntityAsAttachmentEx(InventoryMode.LOCAL, m.item, m.slot);
                }
                if (!placed && !m.toSlot && m.row >= 0)
                {
                    dst.SetCargo(m.parent, m.item, 0, m.row, m.col, m.flip);
                    placed = m.parent.GetInventory().TakeToDst(InventoryMode.LOCAL, src, dst);
                }
                if (!placed && !m.toSlot && m.row >= 0)
                    placed = m.parent.GetInventory().TakeEntityToCargoEx(InventoryMode.LOCAL, m.item, 0, m.row, m.col);
                // A slotted item must NOT fall back into cargo: it would look
                // restored and be in the wrong place, and the next close would
                // write that wrong place down as the truth.
                if (!placed && !m.toSlot)
                    placed = m.parent.GetInventory().TakeEntityToCargo(InventoryMode.LOCAL, m.item);
            }
            if (!placed)
            {
                failed++;
                s_Missed++;
                string spot = "cell " + m.row.ToString() + "," + m.col.ToString();
                if (m.toSlot)
                    spot = "slot " + m.slot.ToString();
                OZ_Log.Warn("storage: " + m.item.GetType() + " with " + m.children.ToString() + " items could not be moved into " + into + " at " + spot + "; it stays where it was built");
            }
            if (placed)
            {
                // Where it ACTUALLY landed. A move that reports success and
                // puts the item somewhere else -- another container, or
                // nowhere at all -- is how a restore loses things while
                // counting none missed.
                EntityAI now = m.item.GetHierarchyParent();
                if (now != m.parent)
                {
                    string where = "nowhere";
                    if (now)
                        where = now.GetType();
                    OZ_Log.Error("storage: " + m.item.GetType() + " said it moved into " + into + " and is in " + where);
                }
            }
            // AN AUTHORITY NEVER PUTS A FAILED ITEM ON THE GROUND.
            //
            // `m.publish` is false only for a box nobody is told about, and
            // announcing there is not a rescue but a DUPLICATE: the record
            // still holds the item, so a copy on the ground is a second one.
            // Measured against the owner 2026-09-24 -- kits kept appearing
            // beside the box, one per session, each also still in the record.
            //
            // The old scheme keeps its rescue: there the world IS the truth
            // while a box is open, and an item left unannounced would be lost
            // for good.
            if (!placed && !m.publish)
            {
                OZ_Log.Error("storage: " + m.item.GetType() + " could not be put into an unannounced box and is deleted rather than dropped beside it; the record still has it and the next open will try again");
                GetGame().ObjectDelete(m.item);
                continue;
            }
            if (m.publish || !placed)
                OZS_Ops.Announce(m.item);
        }
        moves.Clear();
        return failed;
    }

    static int DropMoves(array<ref OZS_Move> moves)
    {
        return DropMovesFrom(moves, 0);
    }

    static int DropMovesFrom(array<ref OZS_Move> moves, int from)
    {
        int removed = 0;
        for (int i = from; i < moves.Count(); i++)
        {
            OZS_Move m = moves.Get(i);
            if (m.item && !m.item.GetHierarchyParent())
            {
                GetGame().ObjectDelete(m.item);
                removed++;
            }
        }
        while (moves.Count() > from)
            moves.Remove(moves.Count() - 1);
        return removed;
    }

    static bool ReadBody(FileSerializer f, EntityAI e, int saveVer)
    {
        int muzzles;
        if (!f.Read(muzzles))
            return false;
        Weapon_Base w = Weapon_Base.Cast(e);
        for (int m = 0; m < muzzles; m++)
        {
            bool empty;
            f.Read(empty);
            if (!empty)
            {
                float cd;
                string ct;
                f.Read(cd);
                f.Read(ct);
                if (w)
                    w.PushCartridgeToChamber(m, cd, ct);
            }
            int ic;
            f.Read(ic);
            for (int k = 0; k < ic; k++)
            {
                float id;
                string it;
                f.Read(id);
                f.Read(it);
                if (w)
                    w.PushCartridgeToInternalMagazine(m, id, it);
            }
        }
        if (!e.OnStoreLoad(f, saveVer))
        {
            s_LoadFails++;
            OZ_Log.Warn("storage: " + e.GetType() + " refused its stored state (OnStoreLoad false, game save version " + saveVer.ToString() + ")");
            return false;
        }
        bool isMag;
        f.Read(isMag);
        if (isMag)
        {
            int ammo;
            bool pile;
            f.Read(ammo);
            f.Read(pile);
            Magazine mag = Magazine.Cast(e);
            if (pile)
            {
                if (mag)
                    mag.ServerSetAmmoCount(ammo);
            }
            else
            {
                if (mag)
                    mag.ServerSetAmmoCount(0);
                for (int q = 0; q < ammo; q++)
                {
                    float qd;
                    string qt;
                    f.Read(qd);
                    f.Read(qt);
                    if (mag)
                        mag.ServerStoreCartridge(qd, qt);
                }
            }
        }
        float hp;
        f.Read(hp);
        e.SetHealth("", "Health", hp);
        int zc;
        f.Read(zc);
        for (int z = 0; z < zc; z++)
        {
            string zn;
            float zh;
            f.Read(zn);
            f.Read(zh);
            e.SetHealth(zn, "Health", zh);
        }
        float life;
        f.Read(life);
        e.SetLifetime(life);
        e.AfterStoreLoad();
        e.SetSynchDirty();
        if (w)
            w.Synchronize();
        GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).Call(e.EEOnAfterLoad);
        return true;
    }
}
