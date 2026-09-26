// The AUTHORITATIVE container: a real box that nobody is ever told about.
// Design: docs/specs/2026-09-24-storage-proxy-inventory-design.md, stage A.
//
// WHY A REAL CONTAINER AND NOT A TABLE OF RECORDS. Everything the engine and
// every other mod know about what may go where -- CanReceiveItemIntoCargo,
// CanBeCombined, slot rules, sizes, every script override a mod ever wrote --
// can only be asked of a live entity. A table of records would make us the
// author of those rules; a real container keeps the engine as their author.
//
// WHY NOBODY SEES IT. Two flags, both measured on 2026-09-24:
//
//   ECE_LOCAL                 the object is not registered on the network at
//                             all -- netid stays "00", so no client can be
//                             told about it even by accident
//   ECE_NOPERSISTENCY_WORLD   the engine never writes it, or anything inside
//                             it, into the world save. After a crash there is
//                             nothing left at all: no second truth to
//                             reconcile, no sweep, no ghosts
//
// (results-nopersistency.md; the control box created without the second flag
// did come back, which is what makes the measurement mean anything.)
//
// The class of an authority is THE SAME CLASS as the box it stands for, so its
// grid, its slots and its rules match by construction rather than by a table
// somebody has to keep in step.
class OZS_Authority
{
    protected static ref array<ref OZS_AuthRec> s_Live;
    // Up for exactly one CreateObjectEx call. EEInit runs inside that call, so
    // there is no other moment at which the box can be told what it is before
    // it registers itself as an ordinary box in the world.
    protected static bool s_Making;

    static bool IsMaking()
    {
        return s_Making;
    }

    // Whether any authority stands at all: the watchdog's first question,
    // asked on every item that changes hands anywhere in the world.
    static bool Any()
    {
        return s_Live && s_Live.Count() > 0;
    }

    // Mission finish. Statics survive a restart inside one process, and a
    // session whose End was still waiting for the wire at the finish never
    // discarded its row: one dead entry per such restart (review 2026-09-26,
    // D4). The entities themselves go with the world.
    static void Reset()
    {
        // Whatever was still being deleted a few a frame goes now: no frames
        // are coming, and an authority left standing is a box of items the
        // next mission would find (ECE_NOPERSISTENCY_WORLD keeps it out of
        // the save, but not out of the process).
        FinishAllNow();
        s_Live = null;
        s_Making = false;
    }

    protected static array<ref OZS_AuthRec> Live()
    {
        if (!s_Live)
            s_Live = new array<ref OZS_AuthRec>();
        return s_Live;
    }

    // ---- making one ------------------------------------------------------

    // `forId` is the id whose contents this will hold -- a box's persistent id
    // or a stash's pair. `cls` must be the class of that box, or its grid will
    // not match what is stored and the restore will park roots it cannot
    // place. `at` only has to be a valid position; nobody will ever look at it.
    static OZ_StorageBox Create(string forId, string cls, vector at)
    {
        if (!GetGame() || !GetGame().IsServer())
            return null;
        if (forId == "" || cls == "")
        {
            OZ_Log.Error("storage: an authority needs both an id and a class");
            return null;
        }
        OZ_StorageBox already = Find(forId);
        if (already)
        {
            OZ_Log.Warn("storage: an authority for " + forId + " exists already; reusing it");
            return already;
        }
        s_Making = true;
        Object made = GetGame().CreateObjectEx(cls, at, ECE_LOCAL | ECE_NOPERSISTENCY_WORLD | ECE_NOLIFETIME);
        s_Making = false;
        OZ_StorageBox box = OZ_StorageBox.Cast(made);
        if (!box)
        {
            OZ_Log.Error("storage: the authority for " + forId + " could not be created as " + cls);
            if (made)
                GetGame().ObjectDelete(made);
            return null;
        }
        // AFTER creation: EEInit has left the id empty on purpose, and an
        // authority answers with the id of the box whose contents it holds.
        box.OZS_StandForId(forId);
        // NOTHING HURTS IT. It stands in the placed box's own coordinates,
        // unannounced but solid on the server, and a charge set off beside
        // the box ruined it as well (owner, 2026-09-26). Container_Base
        // answers RUINED by dropping everything it holds on the ground
        // (container_base.c:96): 264 entities left this box in one frame,
        // and the closing write then told the record the box was empty.
        // An authority is contents in a container's shape, not a thing in
        // the world; the world may not damage it.
        box.SetAllowDamage(false);
        OZS_AuthRec rec = new OZS_AuthRec(box, forId);
        Live().Insert(rec);
        OZ_Log.Info("storage: authority for " + forId + " created as " + cls + ", netid " + box.GetNetworkIDString());
        return box;
    }

    // ---- finding one -----------------------------------------------------

    static OZ_StorageBox Find(string forId)
    {
        array<ref OZS_AuthRec> live = Live();
        for (int i = 0; i < live.Count(); i++)
        {
            OZS_AuthRec rec = live.Get(i);
            if (rec.m_Box && rec.m_For == forId)
                return rec.m_Box;
        }
        return null;
    }

    static bool Is(OZ_StorageBox box)
    {
        return RecOf(box) != null;
    }

    protected static OZS_AuthRec RecOf(OZ_StorageBox box)
    {
        if (!box)
            return null;
        array<ref OZS_AuthRec> live = Live();
        for (int i = 0; i < live.Count(); i++)
        {
            if (live.Get(i).m_Box == box)
                return live.Get(i);
        }
        return null;
    }

    // ---- handles ---------------------------------------------------------

    // EVERY OPERATION NAMES AN ITEM BY A HANDLE, NEVER BY ITS CELL. Cells move
    // the moment anybody rearranges anything, and two players in one box is
    // the point (design §8); a network id is no use either, because an item in
    // an unannounced container has none. So the authority hands out its own.
    static int Handle(OZ_StorageBox box, EntityAI item)
    {
        OZS_AuthRec rec = RecOf(box);
        if (!rec || !item)
            return 0;
        // ONE READ, CHECKED. The number on the item (OZS_Handle.c) is
        // trusted only when this box's own table says it names this very
        // entity: a number left over from a box the item has since left
        // could otherwise collide with one this box has handed out.
        int has = OZS_HandleTag.Read(item);
        if (has > 0 && rec.m_ByHandle.Get(has) == item)
            return has;
        if (!OZS_HandleTag.Taggable(item))
        {
            // Nothing to write a number on: the table is walked, as it was
            // for everything before 2026-09-26.
            for (int i = 0; i < rec.m_Items.Count(); i++)
            {
                if (rec.m_Items.Get(i) == item)
                    return rec.m_Handles.Get(i);
            }
        }
        rec.m_Next++;
        rec.m_Items.Insert(item);
        rec.m_Handles.Insert(rec.m_Next);
        rec.m_ByHandle.Set(rec.m_Next, item);
        OZS_HandleTag.Write(item, rec.m_Next);
        // WHO GOT A NEW NUMBER, AND WHY THE OLD ONE DID NOT DO. A number
        // handed out twice for one entity is a client told about an item
        // under a name it does not know (2026-09-26, "told #1 Rag").
        string mapHad = "nothing";
        if (has > 0 && rec.m_ByHandle.Get(has))
            mapHad = rec.m_ByHandle.Get(has).GetType();
        OZ_Log.Dbg("storage: handle #" + rec.m_Next.ToString() + " given to " + item.GetType() + " in " + rec.m_For + " (its tag said " + has.ToString() + ", the table had " + mapHad + " there)");
        return rec.m_Next;
    }

    // A handle for everything in the box, in the order the descriptor will
    // name them. Returns how many entities are there. Handles already given
    // out keep their numbers -- that is the whole point of a handle.
    static int Index(OZ_StorageBox box)
    {
        OZS_AuthRec rec = RecOf(box);
        if (!rec)
            return 0;
        Forget(box);
        array<EntityAI> nodes = new array<EntityAI>();
        array<int> parents = new array<int>();
        OZS_Records.Flatten(box, -1, nodes, parents);
        // Node 0 is the box itself; it is not an item and gets no handle.
        for (int i = 1; i < nodes.Count(); i++)
            Handle(box, nodes.Get(i));
        return nodes.Count() - 1;
    }

    // IS THIS ENTITY STILL UNDER THIS BOX? The same question OZS_Commit.TopOf
    // asks, and the one that has always kept a take-out honest.
    static bool Under(OZ_StorageBox box, EntityAI item)
    {
        if (!box || !item)
            return false;
        EntityAI up = item;
        while (up.GetHierarchyParent())
            up = up.GetHierarchyParent();
        return up == box;
    }

    // A HANDLE NAMES A THING IN THIS BOX, NOT A THING FOR EVER.
    //
    // The table holds a direct reference, and a reference does not know where
    // its entity has got to. An item that is taken OUT is not destroyed -- it
    // is the same entity with a new owner -- so its row survived, and the
    // number went on working as a pass to an entity that had left: `Move`,
    // `Combine` and `Split` would then reach into a PLAYER'S inventory and
    // haul the item back with a LOCAL move that tells no client anything.
    // Two players in one box was enough: B keeps the number of the thing A
    // has just taken (owner, 2026-09-26: "why do our methods reach into an
    // inventory?").
    //
    // `Out` never had the bug, and not by luck of its own -- it asks TopOf,
    // which is this very question. Asking it here gives every other operation
    // the same footing, whatever future one is written.
    static EntityAI ByHandle(OZ_StorageBox box, int handle)
    {
        OZS_AuthRec rec = RecOf(box);
        if (!rec || handle <= 0)
            return null;
        EntityAI item = rec.m_ByHandle.Get(handle);
        if (!item || !Under(box, item))
            return null;
        return item;
    }

    // Entries whose item is no longer in this box -- destroyed, merged away,
    // or handed to a player. Called when the authority is asked to describe
    // itself, so a long session does not grow a table of things that left.
    //
    // IT USED TO DROP ONLY THE DESTROYED, and the comment above it already
    // claimed "taken out" while the code tested `!item`. A taken-out item is
    // very much alive, so its row stayed for the rest of the session -- one
    // per thing anybody took. ByHandle refuses such a row on its own now; this
    // keeps the table from collecting them in the first place.
    static int Forget(OZ_StorageBox box)
    {
        OZS_AuthRec rec = RecOf(box);
        if (!rec)
            return 0;
        int dropped = 0;
        for (int i = rec.m_Items.Count() - 1; i >= 0; i--)
        {
            EntityAI item = rec.m_Items.Get(i);
            if (Under(box, item))
                continue;
            // The number goes off the entity too, when it is still alive: an
            // item handed to a player must not carry a box's number into its
            // next box (OZS_Handle.c).
            if (item && OZS_HandleTag.Read(item) == rec.m_Handles.Get(i))
                OZS_HandleTag.Write(item, 0);
            rec.m_ByHandle.Remove(rec.m_Handles.Get(i));
            rec.m_Items.RemoveOrdered(i);
            rec.m_Handles.RemoveOrdered(i);
            dropped++;
        }
        return dropped;
    }

    // ---- ending one ------------------------------------------------------

    // DISCARD, and nothing else. An authority holds no truth of its own: the
    // truth is in SQL, written operation by operation (design §7), so letting
    // one go costs at most the turn that did not reach the base -- which is
    // exactly what a crash costs (§9). Nothing is written here, and nothing is
    // asked of the bridge.
    //
    // Contents are deleted deepest-first rather than left to the container's
    // own deletion. It probably takes them with it; "probably" is how this
    // project got its ghosts on 2026-09-18.
    //
    // A FEW A FRAME, NOT ALL AT ONCE (owner, 2026-09-26: "deletion has to be
    // budgeted like the write, N a frame"). The entities are listed here and
    // deleted by OnFrame, ReleaseDeletesPerFrame of them a frame; the box
    // itself goes after the last of them. The registration is dropped at
    // once, so a session opening the same box again gets a fresh authority
    // while the old one is still on its way out. Returns how many are going.
    static int Discard(string forId)
    {
        int gone = 0;
        array<ref OZS_AuthRec> live = Live();
        for (int i = live.Count() - 1; i >= 0; i--)
        {
            OZS_AuthRec rec = live.Get(i);
            if (rec.m_For != forId)
                continue;
            if (rec.m_Box)
            {
                // SILENCE BEFORE THE TEARDOWN. The engine calls EECargoOut for
                // every item as the container goes, and the box cannot tell
                // that from a player emptying it: saying so first is what
                // keeps 121 phantom `take` rows out of the admin history.
                rec.m_Box.OZS_Releasing();
                OZS_Teardown job = new OZS_Teardown(rec.m_Box, forId, true);
                Teardowns().Insert(job);
                gone = gone + job.Count();
            }
            live.RemoveOrdered(i);
        }
        return gone;
    }

    // EVERYTHING OUT, THE CONTAINER ITSELF LEFT STANDING. A sort rewrites the
    // record and then rebuilds the authority from it (OZS_Session.Resort), and
    // rebuilding means starting from an empty box -- not from a new one, which
    // would take a new id, a new registration and a new stream.
    //
    // The teardown flag goes up for the same reason Discard raises it: the
    // engine calls EECargoOut for every item on the way, and a sort is not a
    // player emptying the box.
    //
    // Paced the same way as a discard, and the caller waits for the job
    // (OZS_Session.Resort polls IsDone) before it asks for the refill: an
    // item deleted this frame still holds its cells until the frame ends,
    // and a refill started over it would find no room. The flag comes down
    // when the last entity has gone (Finish), not here.
    static OZS_Teardown Empty(OZ_StorageBox box)
    {
        if (!box)
            return null;
        box.OZS_Releasing(true);
        OZS_Teardown job = new OZS_Teardown(box, box.OZS_GetId(), false);
        Teardowns().Insert(job);
        // The handles go now: the items they named are on their way out, and
        // a rebuilt box hands out its own.
        Forget(box);
        OZS_AuthRec rec = RecOf(box);
        if (rec)
        {
            rec.m_Items.Clear();
            rec.m_Handles.Clear();
            rec.m_ByHandle.Clear();
        }
        return job;
    }

    // ---- the teardowns, a few a frame -------------------------------------

    protected static ref array<ref OZS_Teardown> s_Teardowns;

    protected static array<ref OZS_Teardown> Teardowns()
    {
        if (!s_Teardowns)
            s_Teardowns = new array<ref OZS_Teardown>();
        return s_Teardowns;
    }

    // Every frame, from OZS_Proxies.OnFrame. ONE budget for all the
    // teardowns running, not one each: two sessions ending in the same
    // second must not cost the frame twice what the setting allows.
    static void OnFrame()
    {
        if (!s_Teardowns || s_Teardowns.Count() == 0)
            return;
        Step(OZS_Settings.Get().ReleaseDeletesPerFrame);
    }

    // The mission is stopping: everything still listed goes in this frame.
    static void FinishAllNow()
    {
        if (!s_Teardowns || s_Teardowns.Count() == 0)
            return;
        int left = s_Teardowns.Count();
        Step(-1);
        OZ_Log.Info("storage: " + left.ToString() + " teardown(s) finished at once for the stop");
    }

    // `budget` is how many deletions this frame may spend; below zero, all.
    protected static void Step(int budget)
    {
        int i = 0;
        while (i < s_Teardowns.Count())
        {
            OZS_Teardown job = s_Teardowns.Get(i);
            budget = job.Step(budget);
            if (job.IsDone())
            {
                job.Finish();
                s_Teardowns.RemoveOrdered(i);
            }
            else
            {
                i++;
            }
            if (budget == 0)
                return;
        }
    }

    static int TeardownCount()
    {
        if (!s_Teardowns)
            return 0;
        return s_Teardowns.Count();
    }

    static string Status()
    {
        array<ref OZS_AuthRec> live = Live();
        string s = "authorities=" + live.Count();
        if (TeardownCount() > 0)
            s = s + " tearing_down=" + TeardownCount().ToString();
        for (int i = 0; i < live.Count(); i++)
        {
            OZS_AuthRec rec = live.Get(i);
            s = s + " | " + rec.m_For;
            if (!rec.m_Box)
            {
                s = s + " GONE";
                continue;
            }
            // Handles of items that have left are dropped here, so the count
            // below is of live items and not of history.
            Forget(rec.m_Box);
            s = s + " " + rec.m_Box.GetType();
            s = s + " " + OZS_Const.StateName(rec.m_Box.OZS_GetState());
            s = s + " netid " + rec.m_Box.GetNetworkIDString();
            s = s + " roots " + rec.m_Box.OZS_CountEntities();
            s = s + " tree " + (OZS_Records.CountTree(rec.m_Box) - 1).ToString();
            s = s + " handles " + rec.m_Handles.Count();
        }
        return s;
    }
}

// One authority: the container, the id it stands for, and its handle table.
// Two parallel arrays for walking the table (Forget, Status), a map from the
// number to the entity for looking one up, and the number itself on the
// entity for the other direction (OZS_Handle.c): each lookup is one read
// where it used to be a walk (review 2026-09-26, D1).
// THE TEARDOWN OF ONE AUTHORITY, AS A JOB. Its entities are listed once,
// deepest first (Flatten lists a parent before its children, so the list is
// walked backwards), and OZS_Authority.Step deletes a budget's worth a frame.
// A discard deletes the container itself after the last of them; an empty
// leaves it standing and lowers its teardown flag, so the refill that
// follows is audited as usual.
//
// WHY A JOB. Discard and Empty used to call ObjectDelete on everything in
// one frame -- 240 entities for a full large box, a thousand for a nested
// one -- and the engine pays for each deletion at the end of that frame: the
// world index, the physics, the network, the inventory tree. Once the
// closing write became a job (OZS_WholeJob) this was the last one-frame
// burst the scheme had left (owner, 2026-09-26).
class OZS_Teardown
{
    protected ref array<EntityAI> m_Nodes;
    protected OZ_StorageBox m_Box;
    protected string m_For;
    protected bool m_DiscardBox;
    // The next index to delete, walking down to 1; index 0 is the box.
    protected int m_Next;
    protected int m_Gone;
    protected int m_Frames;
    protected bool m_Finished;

    void OZS_Teardown(OZ_StorageBox box, string forId, bool discardBox)
    {
        m_Box = box;
        m_For = forId;
        m_DiscardBox = discardBox;
        m_Nodes = new array<EntityAI>();
        array<int> parents = new array<int>();
        OZS_Records.Flatten(box, -1, m_Nodes, parents);
        m_Next = m_Nodes.Count() - 1;
        m_Gone = 0;
        m_Frames = 0;
        m_Finished = false;
    }

    // How many entities this job will delete, the box itself not counted.
    int Count()
    {
        return m_Nodes.Count() - 1;
    }

    bool IsDone()
    {
        return m_Next < 1;
    }

    // Deletes up to `budget` entities (all of them below zero) and answers
    // with what is left of the budget.
    int Step(int budget)
    {
        if (IsDone())
            return budget;
        m_Frames++;
        while (m_Next >= 1)
        {
            if (budget == 0)
                return 0;
            EntityAI e = m_Nodes.Get(m_Next);
            m_Next--;
            if (!e || e.IsSetForDeletion())
                continue;
            GetGame().ObjectDelete(e);
            m_Gone++;
            if (budget > 0)
                budget--;
        }
        return budget;
    }

    // After the last entity: the box goes too, or stays and is audited again.
    void Finish()
    {
        if (m_Finished)
            return;
        m_Finished = true;
        string tail = m_Gone.ToString() + " entity(ies) over " + m_Frames.ToString() + " frame(s)";
        if (!m_Box)
        {
            OZ_Log.Warn("storage: authority for " + m_For + " was gone before its teardown finished (" + tail + ")");
            return;
        }
        if (m_DiscardBox)
        {
            GetGame().ObjectDelete(m_Box);
            OZ_Log.Info("storage: authority for " + m_For + " discarded with " + tail);
            return;
        }
        // Down again: the refill and everything the player does afterwards
        // must be audited as usual.
        m_Box.OZS_Releasing(false);
        OZ_Log.Info("storage: authority for " + m_For + " emptied, " + tail);
    }
}

class OZS_AuthRec
{
    OZ_StorageBox m_Box;
    string m_For;
    ref array<EntityAI> m_Items;
    ref array<int> m_Handles;
    ref map<int, EntityAI> m_ByHandle;
    int m_Next;

    void OZS_AuthRec(OZ_StorageBox box, string forId)
    {
        m_Box = box;
        m_For = forId;
        m_Items = new array<EntityAI>();
        m_Handles = new array<int>();
        m_ByHandle = new map<int, EntityAI>();
        m_Next = 0;
    }
}
