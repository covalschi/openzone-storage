// Stand only, client side: a file the stand writes and the client polls once
// a second, because the bridge cannot reach the client and a headless stand
// has no mouse for the inventory screen's widgets.
//
//   $profile:OpenZone_StorageProbe/control.txt, first line:
//     search <text>   -- as if typed into the storage search bar
//     clear           -- empty the search
//     sort [n]        -- press the Sort button (change n to press again)
//     inventory [n]   -- open the inventory screen
//     take <cls> [n]  -- the nearest loose <cls> into the hands (predictive,
//                        the vanilla Take action's path)
//     into <cls> [n]  -- the nearest loose <cls> into the cargo of what the
//                        player holds (a drag onto the held item)
//     onto <cls> [n]  -- what the player holds into the cargo of the nearest
//                        loose <cls> (a drag onto an item on the ground)
//     grab <cls> [n]  -- the nearest <cls> that sits in some container's cargo
//                        into the hands (predictive)
//     stashto <cls> [n] -- what the player holds into the nearest <cls>'s cargo
//     drop [n]        -- what the player holds onto the ground (predictive)
//     stash [n]       -- move what the player holds into the nearest storage
//                        box, the predictive way the inventory screen does it
//     tree <r> [n]    -- the same tree the server's `oz_probe tree` prints,
//                        from this client's point of view, into scan.txt
//     scan <r> [n]    -- list every item THIS CLIENT has lying loose within r
//                        metres of the player: the server's own list of the
//                        same spot tells which of them are ghosts
//     stashopen [n]   -- the personal stash's screen over the nearest locker,
//                        the way F does it (the client asks through the anchor)
//     alt <handle> [n]  -- Alt+click by hand: the box item with that handle
//                          into the player's inventory (OZS_Mirrors.QuickMove)
//     altin <cls> [n]   -- Alt+click the other way: the player's own <cls>
//                          (hands first) into the open box
//     stackin <cls> [n]   -- the player's <cls> stack dropped onto the box's
//                            <cls> stack: the screen's own CombineItemsClient
//     stackout <cls> [n]  -- the box's <cls> stack onto the player's
//     gstackin <cls> [n]  -- a loose <cls> stack at the player's feet onto
//                            the box's; gstackout the other way round
//     gswap <cls> <handle> [n] -- a loose <cls> dropped onto the box item with
//                          that handle, routed as the screen would route it
//     pxsplit <handle> [here] [n] -- the right-click split of that box item,
//                          exactly the call OnRightClick makes; `here` names
//                          the item's own container as the holder instead
//     pxmove <id> <handle> <row> <col> [into] -- a move inside the box, into
//                          the box's own grid or into the container <into>
//     inv [n]         -- the player's inventory and every proxy's items with
//                        their quantities, into scan.txt
#ifndef NO_GUI
class OZ_ProbeClientControl
{
    static const string FILE = "$profile:OpenZone_StorageProbe/control.txt";
    static const string SCAN_FILE = "$profile:OpenZone_StorageProbe/scan.txt";

    protected float  m_Timer;
    protected string m_Last;

    void OnFrame(float timeslice)
    {
        m_Timer = m_Timer + timeslice;
        if (m_Timer < 1.0)
            return;
        m_Timer = 0;
        if (!FileExist(FILE))
            return;
        FileHandle fh = OpenFile(FILE, FileMode.READ);
        if (fh == 0)
            return;
        string line;
        FGets(fh, line);
        CloseFile(fh);
        line = line.Trim();
        if (line == m_Last)
            return;
        m_Last = line;
        Apply(line);
    }

    protected void Apply(string line)
    {
        // WARNING, because INFO never reaches the retail client's .RPT.
        if (line.IndexOf("search ") == 0)
        {
            OZS_Search.Set(line.Substring(7, line.Length() - 7));
            ErrorEx("[OpenZone] probe control: search [" + OZS_Search.s_Text + "] version " + OZS_Search.s_Version, ErrorExSeverity.WARNING);
        }
        else if (line == "clear")
        {
            OZS_Search.Set("");
            ErrorEx("[OpenZone] probe control: search cleared", ErrorExSeverity.WARNING);
        }
        else if (line.IndexOf("sort") == 0)
        {
            // THROUGH THE PROXY, like the button in the panel. The old path
            // went through the client's vicinity scan, which is gone with the
            // old scheme (2026-09-26): it looked for a PLACED box that was
            // open, and under the proxy there is never one.
            OZS_Mirror only = OZS_Mirrors.Get().First();
            if (only)
            {
                only.Sort();
                ErrorEx("[OpenZone] probe control: sort requested", ErrorExSeverity.WARNING);
            }
            else
            {
                ErrorEx("[OpenZone] probe control: no box is open to sort", ErrorExSeverity.WARNING);
            }
        }
        else if (line.IndexOf("inventory") == 0)
        {
            GetGame().GetMission().ShowInventory();
            ErrorEx("[OpenZone] probe control: inventory shown", ErrorExSeverity.WARNING);
        }
        else if (line.IndexOf("scan") == 0)
        {
            Scan(line);
        }
        else if (line.IndexOf("tree") == 0)
        {
            ClientTree(line);
        }
        else if (line.IndexOf("stashopen") == 0)
        {
            // The stash's screen over the nearest locker, as F does.
            StashOpen();
        }
        else if (line.IndexOf("stash") == 0)
        {
            Stash(line);
        }
        else if (line.IndexOf("take ") == 0 || line.IndexOf("into ") == 0 || line.IndexOf("onto ") == 0)
        {
            Move(line);
        }
        else if (line.IndexOf("grab ") == 0 || line.IndexOf("stashto ") == 0 || line.IndexOf("drop") == 0)
        {
            RoundTrip(line);
        }
        else if (line.IndexOf("rpc ") == 0)
        {
            DayZGame.s_OZ_Trace = line.IndexOf("rpc on") == 0;
            Note("=== rpc trace " + DayZGame.s_OZ_Trace.ToString());
        }
        else if (line.IndexOf("pxopen") == 0)
        {
            // The CLIENT asks for a box, which is what a screen does: through
            // the ANCHOR in front of the player, because a box id belongs to
            // the server and this side is never told it.
            PxAsk();
        }
        else if (line.IndexOf("screen") == 0)
        {
            // The whole thing as a player does it: the box's own screen over
            // the nearest anchor.
            PxScreen(line);
        }
        else if (line.IndexOf("pxshut ") == 0)
        {
            OZS_Mirror shutting = OZS_Mirrors.Get().Find(line.Substring(7, line.Length() - 7));
            if (shutting)
            {
                shutting.Shut();
                Note("=== shut " + shutting.m_Id);
            }
        }
        else if (line.IndexOf("alt ") == 0 || line.IndexOf("altin ") == 0)
        {
            // ALT+CLICK BY HAND (2026-09-26): the call the click handlers
            // make, without the mouse.
            QuickMove(line);
        }
        else if (line.IndexOf("stackin ") == 0 || line.IndexOf("stackout ") == 0 || line.IndexOf("gstackin ") == 0 || line.IndexOf("gstackout ") == 0)
        {
            // Two stacks either side of the boundary, through the screen's
            // own combine call (2026-09-26).
            Stack(line);
        }
        else if (line.IndexOf("gswap ") == 0)
        {
            GroundSwap(line);
        }
        else if (line.IndexOf("swap") == 0)
        {
            // The vanilla screen's own swap call, made by hand: two items of
            // the box change places.
            PxSwap(line);
        }
        else if (line.IndexOf("drag") == 0)
        {
            // THE VANILLA SCREEN'S OWN CALL, made by hand. Every drag of the
            // inventory ends in one of the player's Predictive* methods; this
            // makes the same call with the same arguments, so it goes through
            // exactly the hooks a mouse would.
            //
            //   drag <row> <col>   the first item in the box to that cell
            //   drag out           the first item in the box to the hands
            //   drag pocket        the first item in the box into a POCKET --
            //                      the case the owner reported: it must land
            //                      where it was dropped, not in the hands
            //   drag in            what is in the hands into the box
            PxDrag(line);
        }
        else if (line.IndexOf("pxmove ") == 0)
        {
            // pxmove <id> <handle> <row> <col> [into]
            PxMove(line);
        }
        else if (line.IndexOf("pxsplit ") == 0)
        {
            // pxsplit <handle> [here] [n] -- the right-click split by hand
            PxSplit(line);
        }
        else if (line == "inv" || line.IndexOf("inv ") == 0)
        {
            // inv [n] -- the player's inventory and every proxy, with counts
            Inv();
        }
        else if (line.IndexOf("px") == 0)
        {
            // What this client's PROXIES hold (design 2026-09-24): the answer
            // to "did the wire deliver the box", asked of the client itself.
            Note("=== " + line + " -> " + OZS_Mirrors.Get().Status());
            PxTree(line);
        }
        else if (line.IndexOf("mirror") == 0)
        {
            Mirror(line);
        }
        else if (line.IndexOf("shuffle") == 0)
        {
            Shuffle(line);
        }
        else if (line.IndexOf("moves") == 0)
        {
            Battery(line);
        }
    }

    // THE DIVERGENCE QUESTION (2026-09-24): does a client mirror judge a move
    // the same way the server's authoritative box would? Both are the same
    // engine with the same classes, so they should -- but "should" is not a
    // measurement, and a mirror that accepts what the server refuses is a
    // snap-back on every drag.
    //
    // The same battery runs here and in the stand bridge's `moves` op on a
    // server-built box with the same contents; the two outputs are compared
    // line by line.
    protected void Battery(string line)
    {
        PlayerBase me = PlayerBase.Cast(GetGame().GetPlayer());
        if (!me)
        {
            Say("moves: no player");
            return;
        }
        array<Object> around = new array<Object>();
        GetGame().GetObjectsAtPosition(me.GetPosition(), 6, around, null);
        EntityAI box = null;
        for (int i = 0; i < around.Count(); i++)
        {
            EntityAI cand = EntityAI.Cast(around.Get(i));
            if (cand && !cand.GetHierarchyParent() && cand.GetInventory() && cand.GetInventory().GetCargo() && cand.GetNetworkIDString() == "00")
            {
                box = cand;
                break;
            }
        }
        if (!box)
        {
            Say("moves: no client-local container within 6 m -- run mirror first");
            return;
        }
        Say("moves: battery on the MIRROR of " + box.GetType());
        array<string> report = new array<string>();
        OZ_ProbeBattery.Run(box, report);
        for (int r = 0; r < report.Count(); r++)
            Say("moves: " + report.Get(r));
    }

    // The other half of the mirror question: can anything be MOVED inside a
    // container that exists only here? The grab out of it failed, as a move to
    // the player's real inventory must -- the server has no counterpart to
    // agree with. A move that stays entirely inside the local tree has no
    // server half to fail, so it is a different question, and it decides
    // whether a screen over such a mirror can rearrange anything at all.
    //
    //   shuffle <row> <col> [n]   -- the first item in the local box to that cell
    //
    // Two modes are tried in turn, because the inventory screen uses the
    // second: LOCAL (apply here, tell nobody) and PREDICTIVE (apply here, ask
    // the server to agree).
    protected void Shuffle(string line)
    {
        array<string> parts = new array<string>();
        line.Split(" ", parts);
        int row = 2;
        int col = 3;
        if (parts.Count() > 1)
            row = parts.Get(1).ToInt();
        if (parts.Count() > 2)
            col = parts.Get(2).ToInt();

        PlayerBase me = PlayerBase.Cast(GetGame().GetPlayer());
        if (!me)
        {
            Say("shuffle: no player");
            return;
        }
        array<Object> around = new array<Object>();
        GetGame().GetObjectsAtPosition(me.GetPosition(), 6, around, null);
        EntityAI box = null;
        for (int i = 0; i < around.Count(); i++)
        {
            EntityAI cand = EntityAI.Cast(around.Get(i));
            if (!cand || cand.GetHierarchyParent())
                continue;
            // The mirror is the one with no network id of its own.
            if (cand.GetInventory() && cand.GetInventory().GetCargo() && cand.GetNetworkIDString() == "00")
            {
                box = cand;
                break;
            }
        }
        if (!box)
        {
            Say("shuffle: no client-local container within 6 m -- run mirror first");
            return;
        }
        CargoBase cargo = box.GetInventory().GetCargo();
        if (cargo.GetItemCount() == 0)
        {
            Say("shuffle: the mirror is empty");
            return;
        }
        EntityAI item = EntityAI.Cast(cargo.GetItem(0));
        InventoryLocation src = new InventoryLocation();
        item.GetInventory().GetCurrentInventoryLocation(src);
        Say("shuffle: " + item.GetType() + " from " + src.GetRow() + "," + src.GetCol() + " to " + row + "," + col);

        InventoryLocation dst = new InventoryLocation();
        dst.SetCargo(box, item, 0, row, col, false);
        bool okLocal = box.GetInventory().TakeToDst(InventoryMode.LOCAL, src, dst);
        InventoryLocation after = new InventoryLocation();
        item.GetInventory().GetCurrentInventoryLocation(after);
        Say("shuffle: LOCAL returned " + okLocal.ToString() + ", item now at " + after.GetRow() + "," + after.GetCol());

        InventoryLocation src2 = new InventoryLocation();
        item.GetInventory().GetCurrentInventoryLocation(src2);
        InventoryLocation dst2 = new InventoryLocation();
        dst2.SetCargo(box, item, 0, row + 1, col, false);
        bool okPred = box.GetInventory().TakeToDst(InventoryMode.PREDICTIVE, src2, dst2);
        InventoryLocation after2 = new InventoryLocation();
        item.GetInventory().GetCurrentInventoryLocation(after2);
        Say("shuffle: PREDICTIVE returned " + okPred.ToString() + ", item now at " + after2.GetRow() + "," + after2.GetCol());
    }

    // THE QUESTION THIS PROBE EXISTS FOR (2026-09-24): can a container that
    // exists ONLY in this client's memory hold items and be shown by the
    // vanilla inventory screen?
    //
    // If it can, a storage box can be private for real: the server keeps the
    // authoritative box unannounced (ECE_LOCAL there too), sends its contents
    // to ONE player over RPC -- the only call in game.c that takes a
    // PlayerIdentity -- and that player's client builds this copy. Nothing
    // reaches anybody else, because nothing is ever announced.
    //
    // Vanilla has no precedent: its own local entities (scriptconsoleitemstab.c
    // :695) are made for a still picture and are never asked for an inventory.
    // So this asks the engine instead of guessing:
    //
    //   mirror <container> <item> <count> [n]
    //
    // and writes every answer to scan.txt.
    protected void Mirror(string line)
    {
        array<string> parts = new array<string>();
        line.Split(" ", parts);
        string boxCls = "SeaChest";
        string itemCls = "BandageDressing";
        int want = 3;
        if (parts.Count() > 1)
            boxCls = parts.Get(1);
        if (parts.Count() > 2)
            itemCls = parts.Get(2);
        if (parts.Count() > 3)
            want = parts.Get(3).ToInt();

        PlayerBase me = PlayerBase.Cast(GetGame().GetPlayer());
        if (!me)
        {
            Say("mirror: no player");
            return;
        }
        // A step to the side, so it cannot be confused with a real box the
        // server put under the player's feet.
        vector at = me.GetPosition();
        at[0] = at[0] + 1.5;

        Say("mirror: asking for " + boxCls + " with " + want + " x " + itemCls);
        float t0 = GetGame().GetTickTime();
        Object made = GetGame().CreateObjectEx(boxCls, at, ECE_LOCAL | ECE_PLACE_ON_SURFACE | ECE_NOLIFETIME);
        if (!made)
        {
            Say("mirror: Q1 FAILED -- the client refused to create " + boxCls + " at all");
            return;
        }
        EntityAI box = EntityAI.Cast(made);
        if (!box)
        {
            Say("mirror: Q1 FAILED -- created, but not an EntityAI");
            return;
        }
        Say("mirror: Q1 ok -- created, netid " + box.GetNetworkIDString());

        GameInventory inv = box.GetInventory();
        if (!inv)
        {
            Say("mirror: Q2 FAILED -- no GameInventory on a client-local container");
            return;
        }
        CargoBase cargo = inv.GetCargo();
        if (!cargo)
        {
            Say("mirror: Q2 FAILED -- GetCargo() is null on a client-local container");
            return;
        }
        Say("mirror: Q2 ok -- cargo exists, " + cargo.GetItemCount() + " item(s) in it now");

        int made2 = 0;
        float tItems = GetGame().GetTickTime();
        for (int i = 0; i < want; i++)
        {
            EntityAI kid = EntityAI.Cast(inv.CreateEntityInCargo(itemCls));
            if (kid)
                made2++;
        }
        float tDone = GetGame().GetTickTime();
        // ALL IN ONE FRAME on purpose: this is the burst case, the one that
        // stalled a client for 39.2 s server-side in 2026-09-17. The client
        // monitor's client.log carries the frame statistics of the same
        // second; these numbers say how long the loop itself took.
        Say("mirror: Q3 " + made2 + " of " + want + " item(s) in " + ((tDone - tItems) * 1000) + " ms (box itself " + ((tItems - t0) * 1000) + " ms); cargo reports " + cargo.GetItemCount());
        Say("mirror: Q4 -- open the inventory and look in the nearby panel for " + box.GetDisplayName());
    }

    // One line to the .RPT as a WARNING (INFO never reaches a retail client's
    // log) and the same line appended to scan.txt, which is what a headless
    // stand can actually read back.
    protected void Say(string what)
    {
        ErrorEx("[OpenZone] probe " + what, ErrorExSeverity.WARNING);
        FileHandle f = OpenFile(SCAN_FILE, FileMode.APPEND);
        if (f == 0)
            return;
        FPrintln(f, what);
        CloseFile(f);
    }

    // Every loose item the client knows about near the player, one line each,
    // then a count. "Loose" = no hierarchy parent: in the world, not in any
    // inventory. The radius comes after the word ("scan 15"); anything after
    // the radius only makes the line differ so the same scan can run twice.
    // The three moves a player makes with the mouse to build a nested stack,
    // each through the same predictive call the inventory screen uses.
    protected void Move(string line)
    {
        array<string> parts = new array<string>();
        line.Split(" ", parts);
        string verb = parts.Get(0);
        string cls = "";
        if (parts.Count() > 1)
            cls = parts.Get(1);
        PlayerBase me = PlayerBase.Cast(GetGame().GetPlayer());
        string what = "=== " + verb + " [" + line + "] t=" + GetGame().GetTickTime();
        if (!me)
        {
            Note(what + ": no player");
            return;
        }
        EntityAI held = me.GetHumanInventory().GetEntityInHands();
        EntityAI loose = NearestLoose(me.GetPosition(), cls);
        if (!loose)
        {
            Note(what + ": no loose " + cls + " within 4 m");
            return;
        }
        bool ok = false;
        if (verb == "take")
        {
            me.PredictiveTakeEntityToHands(loose);
            Note(what + ": " + loose.GetType() + " -> hands (requested)");
            return;
        }
        if (!held)
        {
            Note(what + ": nothing in hands");
            return;
        }
        if (verb == "into")
        {
            ok = me.PredictiveTakeEntityToTargetCargo(held, loose);
            Note(what + ": " + loose.GetType() + " -> " + held.GetType() + " (held) predictive=" + ok);
            return;
        }
        ok = me.PredictiveTakeEntityToTargetCargo(loose, held);
        Note(what + ": " + held.GetType() + " (held) -> " + loose.GetType() + " predictive=" + ok);
    }

    protected EntityAI NearestLoose(vector at, string cls)
    {
        array<Object> objs = new array<Object>();
        GetGame().GetObjectsAtPosition(at, 4, objs, null);
        EntityAI best = null;
        float bestDist = 1000;
        for (int i = 0; i < objs.Count(); i++)
        {
            EntityAI e = EntityAI.Cast(objs.Get(i));
            if (!e || !e.IsKindOf(cls) || e.GetHierarchyParent())
                continue;
            float d = vector.Distance(e.GetPosition(), at);
            if (d < bestDist)
            {
                bestDist = d;
                best = e;
            }
        }
        return best;
    }

    // The owner's round trip, step by step: out of a container into the
    // hands, back into a container, onto the ground.
    protected void RoundTrip(string line)
    {
        array<string> parts = new array<string>();
        line.Split(" ", parts);
        string verb = parts.Get(0);
        string cls = "";
        if (parts.Count() > 1)
            cls = parts.Get(1);
        PlayerBase me = PlayerBase.Cast(GetGame().GetPlayer());
        string what = "=== " + verb + " [" + line + "] t=" + GetGame().GetTickTime();
        if (!me)
        {
            Note(what + ": no player");
            return;
        }
        EntityAI held = me.GetHumanInventory().GetEntityInHands();
        if (verb == "drop")
        {
            if (!held)
            {
                Note(what + ": nothing in hands");
                return;
            }
            me.PredictiveDropEntity(held);
            Note(what + ": " + held.GetType() + " -> ground (requested)");
            return;
        }
        if (verb == "grab")
        {
            EntityAI inside = NearestInCargo(me, cls);
            if (!inside)
            {
                Note(what + ": no " + cls + " inside a container within 4 m");
                return;
            }
            me.PredictiveTakeEntityToHands(inside);
            Note(what + ": " + inside.GetType() + " (in " + inside.GetHierarchyParent().GetType() + ") -> hands (requested)");
            return;
        }
        if (!held)
        {
            Note(what + ": nothing in hands");
            return;
        }
        EntityAI target = NearestLoose(me.GetPosition(), cls);
        if (!target)
        {
            Note(what + ": no loose " + cls + " within 4 m");
            return;
        }
        bool ok = me.PredictiveTakeEntityToTargetCargo(target, held);
        Note(what + ": " + held.GetType() + " -> " + target.GetType() + " predictive=" + ok);
    }

    // The nearest <cls> that is in the cargo of a container lying in the
    // world (not the player's own inventory).
    protected EntityAI NearestInCargo(PlayerBase me, string cls)
    {
        array<Object> objs = new array<Object>();
        GetGame().GetObjectsAtPosition(me.GetPosition(), 4, objs, null);
        for (int i = 0; i < objs.Count(); i++)
        {
            EntityAI box = EntityAI.Cast(objs.Get(i));
            if (!box || box.GetHierarchyParent() || box == me)
                continue;
            CargoBase cargo = box.GetInventory().GetCargo();
            if (!cargo)
                continue;
            for (int k = 0; k < cargo.GetItemCount(); k++)
            {
                EntityAI item = cargo.GetItem(k);
                if (item && item.IsKindOf(cls))
                    return item;
            }
        }
        return null;
    }

    protected void ClientTree(string line)
    {
        float radius = 8;
        array<string> parts = new array<string>();
        line.Split(" ", parts);
        if (parts.Count() > 1)
            radius = parts.Get(1).ToFloat();
        PlayerBase me = PlayerBase.Cast(GetGame().GetPlayer());
        if (!me)
            return;
        // The server's code is shared: OZ_ProbeState lives in 4_World, which
        // the client compiles too, and it only reads.
        string done = OZ_ProbeState.Tree(me.GetPosition(), radius, SCAN_FILE);
        Note("=== " + line + " -> " + done);
    }

    // The nearest box in front of the player, which is what a screen is opened
    // on. Anything else would need a box id, and this side does not have one.
    protected OZ_StorageBox PxAnchor(string want = "")
    {
        PlayerBase me = PlayerBase.Cast(GetGame().GetPlayer());
        if (!me)
            return null;
        array<Object> around = new array<Object>();
        GetGame().GetObjectsAtPosition(me.GetPosition(), 12, around, null);
        OZ_StorageBox best = null;
        float nearest = 1000;
        for (int i = 0; i < around.Count(); i++)
        {
            OZ_StorageBox b = OZ_StorageBox.Cast(around.Get(i));
            if (!b)
                continue;
            // Several boxes stand on one spot on the stand; a class picks one.
            if (want != "" && b.GetType() != want)
                continue;
            float d = vector.Distance(b.GetPosition(), me.GetPosition());
            if (d < nearest)
            {
                nearest = d;
                best = b;
            }
        }
        return best;
    }

    protected void StashOpen()
    {
        PlayerBase me = PlayerBase.Cast(GetGame().GetPlayer());
        if (!me)
        {
            Note("=== stashopen: no player");
            return;
        }
        array<Object> around = new array<Object>();
        GetGame().GetObjectsAtPosition(me.GetPosition(), 12, around, null);
        OZ_StashAnchor best = null;
        float nearest = 1000;
        for (int i = 0; i < around.Count(); i++)
        {
            OZ_StashAnchor a = OZ_StashAnchor.Cast(around.Get(i));
            if (!a)
                continue;
            float d = vector.Distance(a.GetPosition(), me.GetPosition());
            if (d < nearest)
            {
                nearest = d;
                best = a;
            }
        }
        if (!best)
        {
            Note("=== stashopen: no locker within 12 m");
            return;
        }
        OZS_Mirror.Ask(best);
        OZS_Mirrors.Get().ShowWhenReady();
        Note("=== stashopen: asked through " + best.GetType() + " netid " + best.GetNetworkIDString() + ", the panel opens when it is whole");
    }

    protected void PxAsk()
    {
        OZ_StorageBox anchor = PxAnchor();
        if (!anchor)
        {
            Note("=== pxopen: no box within 12 m");
            return;
        }
        OZS_Mirror.Ask(anchor);
        Note("=== asked through " + anchor.GetType() + " netid " + anchor.GetNetworkIDString());
    }

    protected void PxScreen(string line)
    {
        array<string> parts = new array<string>();
        line.Split(" ", parts);
        string want = "";
        if (parts.Count() > 1 && parts.Get(1).IndexOf("OZ_") == 0)
            want = parts.Get(1);
        OZ_StorageBox anchor = PxAnchor(want);
        if (!anchor)
        {
            Note("=== screen: no box within 12 m");
            return;
        }
        // What the action does: ask for the box, then let the ordinary
        // inventory open once it has arrived.
        OZS_Mirror.Ask(anchor);
        OZS_Mirrors.Get().ShowWhenReady();
        Note("=== asked through " + anchor.GetType() + ", the panel opens when it is whole");
    }

    protected void PxSwap(string line)
    {
        PlayerBase me = PlayerBase.Cast(GetGame().GetPlayer());
        OZS_Mirror m = OZS_Mirrors.Get().Newest();
        if (!me || !m)
        {
            Note("swap: no player or no box open");
            return;
        }
        array<EntityAI> roots = new array<EntityAI>();
        m.Roots(roots);
        if (roots.Count() < 2)
        {
            Note("swap: the box needs two items");
            return;
        }
        EntityAI a = roots.Get(0);
        EntityAI b = roots.Get(1);
        // WHICH of the checks inside CanSwapEntitiesEx says no. It is three
        // tests in a row and they fail for very different reasons; the
        // difference decides whether this is ours to fix at all.
        InventoryLocation il1 = new InventoryLocation();
        InventoryLocation il2 = new InventoryLocation();
        a.GetInventory().GetCurrentInventoryLocation(il1);
        b.GetInventory().GetCurrentInventoryLocation(il2);
        ItemBase ia = ItemBase.Cast(a);
        ItemBase ib = ItemBase.Cast(b);
        string why = "";
        if (ia && ia.CanBeSplit() && ia.GetQuantity() > ia.GetTargetQuantityMax(il2.GetSlot()))
            why = why + " [a splits: " + ia.GetQuantity().ToString() + " > " + ia.GetTargetQuantityMax(il2.GetSlot()).ToString() + "]";
        if (ib && ib.CanBeSplit() && ib.GetQuantity() > ib.GetTargetQuantityMax(il1.GetSlot()))
            why = why + " [b splits: " + ib.GetQuantity().ToString() + " > " + ib.GetTargetQuantityMax(il1.GetSlot()).ToString() + "]";
        if (!a.CanSwapEntities(b, il2, il1))
            why = why + " [a.CanSwapEntities false]";
        if (!b.CanSwapEntities(a, il1, il2))
            why = why + " [b.CanSwapEntities false]";
        if (!GameInventory.CanSwapEntities(a, b))
            why = why + " [native CanSwapEntities false]";
        if (why == "")
            why = " (all three pass)";
        bool can = me.GetInventory().CanSwapEntitiesEx(a, b);
        bool force = me.GetInventory().CanForceSwapEntitiesEx(a, il2, b, il1);
        bool ok = me.PredictiveSwapEntities(a, b);
        Note("swap: " + a.GetType() + " #" + m.HandleOf(a).ToString() + " with " + b.GetType() + " #" + m.HandleOf(b).ToString() + ": CanSwapEntitiesEx " + can.ToString() + ", CanForceSwapEntitiesEx " + force.ToString() + ", Predictive " + ok.ToString() + " ->" + why);
    }

    protected void PxDrag(string line)
    {
        array<string> parts = new array<string>();
        line.Split(" ", parts);
        PlayerBase me = PlayerBase.Cast(GetGame().GetPlayer());
        OZS_Mirror m = OZS_Mirrors.Get().Newest();
        if (!me || !m || !m.m_Box)
        {
            Note("drag: no player or no box open");
            return;
        }
        string what = "";
        if (parts.Count() > 1)
            what = parts.Get(1);

        if (what == "in")
        {
            EntityAI held = me.GetHumanInventory().GetEntityInHands();
            if (!held)
            {
                Note("drag in: the hands are empty");
                return;
            }
            InventoryLocation inSrc = new InventoryLocation();
            held.GetInventory().GetCurrentInventoryLocation(inSrc);
            InventoryLocation inDst = new InventoryLocation();
            if (!m.m_Box.GetInventory().FindFreeLocationFor(held, FindInventoryLocationType.CARGO, inDst))
            {
                Note("drag in: no free cell in the box");
                return;
            }
            bool inOk = me.PredictiveTakeToDst(inSrc, inDst);
            Note("drag in: " + held.GetType() + " -> the box, PredictiveTakeToDst said " + inOk.ToString());
            return;
        }

        array<EntityAI> roots = new array<EntityAI>();
        m.Roots(roots);
        if (roots.Count() == 0)
        {
            Note("drag: the box is empty");
            return;
        }
        EntityAI first = roots.Get(0);
        InventoryLocation src = new InventoryLocation();
        first.GetInventory().GetCurrentInventoryLocation(src);

        if (what == "out")
        {
            InventoryLocation hands = new InventoryLocation();
            hands.SetHands(me, first);
            bool outOk = me.PredictiveTakeToDst(src, hands);
            Note("drag out: " + first.GetType() + " #" + m.HandleOf(first).ToString() + " -> hands, PredictiveTakeToDst said " + outOk.ToString());
            return;
        }

        if (what == "pocket")
        {
            // A real place in the player's own inventory, chosen the way the
            // screen chooses one: the engine finds a free spot in something
            // they are wearing.
            InventoryLocation mine = new InventoryLocation();
            if (!me.GetInventory().FindFreeLocationFor(first, FindInventoryLocationType.CARGO, mine))
            {
                Note("drag pocket: nowhere in the player's own inventory");
                return;
            }
            string where = "nothing";
            if (mine.GetParent())
                where = mine.GetParent().GetType();
            bool pocketOk = me.PredictiveTakeToDst(src, mine);
            Note("drag pocket: " + first.GetType() + " #" + m.HandleOf(first).ToString() + " -> " + where + " at " + mine.GetRow().ToString() + "," + mine.GetCol().ToString() + ", PredictiveTakeToDst said " + pocketOk.ToString());
            return;
        }

        int row = 4;
        int col = 6;
        if (parts.Count() > 2)
        {
            row = parts.Get(1).ToInt();
            col = parts.Get(2).ToInt();
        }
        InventoryLocation dst = new InventoryLocation();
        dst.SetCargo(m.m_Box, first, 0, row, col, false);
        bool ok = me.PredictiveTakeToDst(src, dst);
        Note("drag: " + first.GetType() + " #" + m.HandleOf(first).ToString() + " to " + row.ToString() + "," + col.ToString() + ", PredictiveTakeToDst said " + ok.ToString());
    }

    // A move the way the screen will make it: the proxy is moved at once with
    // LOCAL, and the server is asked to agree. Both halves, so the round trip
    // can be watched.
    protected void PxMove(string line)
    {
        array<string> parts = new array<string>();
        line.Split(" ", parts);
        if (parts.Count() < 5)
        {
            Note("pxmove <id> <handle> <row> <col>");
            return;
        }
        OZS_Mirror m = OZS_Mirrors.Get().Find(parts.Get(1));
        if (!m)
        {
            Note("pxmove: no mirror of " + parts.Get(1));
            return;
        }
        int handle = parts.Get(2).ToInt();
        int row = parts.Get(3).ToInt();
        int col = parts.Get(4).ToInt();
        int into = 0;
        if (parts.Count() > 5)
            into = parts.Get(5).ToInt();
        EntityAI e = m.ByHandle(handle);
        if (!e)
        {
            Note("pxmove: no handle " + handle.ToString());
            return;
        }
        if (into != 0 && !m.ByHandle(into))
        {
            Note("pxmove: no container with handle " + into.ToString());
            return;
        }
        OZS_Row want = new OZS_Row();
        want.Set(handle, into, InventoryLocationType.CARGO, -1, row, col, 0, e.GetType());
        bool here = m.Place(e, want);
        m.Move(handle, into, InventoryLocationType.CARGO, -1, row, col, 0);
        Note("pxmove #" + handle.ToString() + " to " + row.ToString() + "," + col.ToString() + " of #" + into.ToString() + ": the proxy said " + here.ToString() + ", the server was asked");
    }

    // The right-click split, by hand: the same call OZS_Stacking's
    // OnRightClick makes, after the same CanBeSplit question -- asked of the
    // PROXY item, which is what the screen asks too.
    protected void PxSplit(string line)
    {
        array<string> parts = new array<string>();
        line.Split(" ", parts);
        OZS_Mirror m = OZS_Mirrors.Get().Newest();
        if (!m || parts.Count() < 2)
        {
            Note("=== " + line + ": no box open, or no handle");
            return;
        }
        int handle = parts.Get(1).ToInt();
        EntityAI e = m.ByHandle(handle);
        ItemBase ib = ItemBase.Cast(e);
        if (!ib)
        {
            Note("=== " + line + ": no such handle");
            return;
        }
        EntityAI holder = m.m_Box;
        if (parts.Count() > 2 && parts.Get(2) == "here" && e.GetHierarchyParent())
            holder = e.GetHierarchyParent();
        string told = "=== " + line + ": #" + handle.ToString() + " " + e.GetType() + " qty " + ib.GetQuantity().ToString();
        told = told + " splitable " + ib.IsSplitable().ToString() + " CanBeSplit " + ib.CanBeSplit().ToString();
        told = told + " CanRemoveEntity " + ib.GetInventory().CanRemoveEntity().ToString();
        if (!ib.CanBeSplit())
            Note(told + " -> the screen would not offer the split; sending anyway");
        OZS_Mirrors.s_Via = "pxsplit";
        m.Split(e, holder, OZS_Const.SPLIT_HALF, InventoryLocationType.CARGO, -1, -1, -1, 0);
        Note(told + " -> Split sent, holder " + holder.GetType());
    }

    // Both sides' counts: what the player carries and what every proxy holds,
    // handle by handle.
    protected void Inv()
    {
        PlayerBase me = PlayerBase.Cast(GetGame().GetPlayer());
        if (!me)
        {
            Note("=== inv: no player");
            return;
        }
        Note("=== inv: the player");
        EntityAI held = me.GetHumanInventory().GetEntityInHands();
        if (held)
            Note("  hands " + Counted(held));
        array<EntityAI> all = new array<EntityAI>();
        me.GetInventory().EnumerateInventory(InventoryTraversalType.PREORDER, all);
        for (int i = 0; i < all.Count(); i++)
        {
            EntityAI e = all.Get(i);
            if (!e || e == me || e == held)
                continue;
            Note("  " + Counted(e));
        }
        array<ref OZS_Mirror> mirrors = OZS_Mirrors.Get().All();
        for (int k = 0; k < mirrors.Count(); k++)
        {
            OZS_Mirror m = mirrors.Get(k);
            Note("=== inv: proxy " + m.m_Id + " " + m.m_Items.Count().ToString() + " item(s)");
            for (int j = 0; j < m.m_Items.Count(); j++)
            {
                EntityAI p = m.m_Items.Get(j);
                if (!p)
                    continue;
                Note("  #" + m.m_Handles.Get(j).ToString() + " " + Counted(p));
            }
        }
    }

    protected string Counted(EntityAI e)
    {
        string s = e.GetType();
        ItemBase ib = ItemBase.Cast(e);
        if (ib && ib.HasQuantity())
            s = s + " qty " + ib.GetQuantity().ToString() + "/" + ib.GetQuantityMax().ToString();
        Magazine mag = Magazine.Cast(e);
        if (mag)
            s = s + " ammo " + mag.GetAmmoCount().ToString();
        if (ib)
            s = s + " clean " + ib.GetCleanness().ToString();
        InventoryLocation il = new InventoryLocation();
        if (e.GetInventory() && e.GetInventory().GetCurrentInventoryLocation(il))
        {
            string where = "lt " + il.GetType().ToString() + " " + il.GetRow().ToString() + "," + il.GetCol().ToString();
            if (il.GetParent())
                where = where + " in " + il.GetParent().GetType();
            s = s + " (" + where + ")";
        }
        s = s + " netid " + e.GetNetworkIDString();
        return s;
    }

    // The proxy's contents as this client sees them, so the two sides can be
    // diffed item by item.
    protected void PxTree(string line)
    {
        array<ref OZS_Mirror> all = OZS_Mirrors.Get().All();
        for (int i = 0; i < all.Count(); i++)
        {
            OZS_Mirror m = all.Get(i);
            if (!m.m_Box)
                continue;
            Note("px " + m.m_Id + " tree " + (OZS_Records.CountTree(m.m_Box) - 1).ToString());
            array<EntityAI> roots = new array<EntityAI>();
            m.Roots(roots);
            for (int r = 0; r < roots.Count(); r++)
            {
                EntityAI e = roots.Get(r);
                InventoryLocation il = new InventoryLocation();
                e.GetInventory().GetCurrentInventoryLocation(il);
                string where = il.GetRow().ToString() + "," + il.GetCol().ToString();
                if (il.GetType() == InventoryLocationType.ATTACHMENT)
                    where = "slot " + il.GetSlot().ToString();
                Note("  #" + m.HandleOf(e).ToString() + " " + e.GetType() + " at " + where + " tree " + OZS_Records.CountTree(e).ToString() + " netid " + e.GetNetworkIDString());
            }
        }
    }

    // ---- the gestures of 2026-09-26, by hand ------------------------------

    // The player's own item of that class: the hands first, then anything
    // worn or carried.
    protected EntityAI Mine(PlayerBase me, string cls)
    {
        EntityAI held = me.GetHumanInventory().GetEntityInHands();
        if (held && held.IsKindOf(cls))
            return held;
        array<EntityAI> all = new array<EntityAI>();
        me.GetInventory().EnumerateInventory(InventoryTraversalType.PREORDER, all);
        for (int i = 0; i < all.Count(); i++)
        {
            EntityAI e = all.Get(i);
            if (e && e != me && e.IsKindOf(cls))
                return e;
        }
        return null;
    }

    // The first root of that class in the open box.
    protected EntityAI InBox(OZS_Mirror m, string cls)
    {
        array<EntityAI> roots = new array<EntityAI>();
        m.Roots(roots);
        for (int i = 0; i < roots.Count(); i++)
        {
            if (roots.Get(i) && roots.Get(i).IsKindOf(cls))
                return roots.Get(i);
        }
        return null;
    }

    protected void QuickMove(string line)
    {
        array<string> parts = new array<string>();
        line.Split(" ", parts);
        PlayerBase me = PlayerBase.Cast(GetGame().GetPlayer());
        OZS_Mirror m = OZS_Mirrors.Get().Newest();
        if (!me || !m || parts.Count() < 2)
        {
            Note("=== " + line + ": no player, no box open, or nothing named");
            return;
        }
        EntityAI item = null;
        if (parts.Get(0) == "alt")
            item = m.ByHandle(parts.Get(1).ToInt());
        else
            item = Mine(me, parts.Get(1));
        if (!item)
        {
            Note("=== " + line + ": no such item");
            return;
        }
        string where = "loose";
        if (item.GetHierarchyParent())
            where = "in " + item.GetHierarchyParent().GetType();
        bool ok = OZS_Mirrors.QuickMove(item);
        string moved = "=== " + line + ": " + item.GetType() + " (" + where + ")";
        moved = moved + " -> QuickMove said " + ok.ToString() + " via " + OZS_Mirrors.s_Via;
        Note(moved);
    }

    protected void Stack(string line)
    {
        array<string> parts = new array<string>();
        line.Split(" ", parts);
        PlayerBase me = PlayerBase.Cast(GetGame().GetPlayer());
        OZS_Mirror m = OZS_Mirrors.Get().Newest();
        if (!me || !m || parts.Count() < 2)
        {
            Note("=== " + line + ": no player, no box open, or nothing named");
            return;
        }
        string verb = parts.Get(0);
        string cls = parts.Get(1);
        EntityAI inside = InBox(m, cls);
        if (!inside)
        {
            Note("=== " + line + ": no " + cls + " in the box");
            return;
        }
        EntityAI outside = null;
        if (verb == "stackin" || verb == "stackout")
            outside = Mine(me, cls);
        else
            outside = NearestLoose(me.GetPosition(), cls);
        if (!outside)
        {
            Note("=== " + line + ": no " + cls + " of the player's, or loose within 4 m");
            return;
        }
        ItemBase taker;
        ItemBase giver;
        if (verb == "stackin" || verb == "gstackin")
        {
            taker = ItemBase.Cast(inside);
            giver = ItemBase.Cast(outside);
        }
        else
        {
            taker = ItemBase.Cast(outside);
            giver = ItemBase.Cast(inside);
        }
        if (!taker || !giver)
        {
            Note("=== " + line + ": not items");
            return;
        }
        bool can = taker.CanBeCombined(giver);
        string before = "taker " + OZS_Ops.Contents(taker).ToString() + " giver " + OZS_Ops.Contents(giver).ToString();
        // THE SCREEN'S OWN CALL: the receiver's CombineItemsClient with the
        // dropped stack, exactly as icon.c:531 makes it.
        taker.CombineItemsClient(giver);
        string stacked = "=== " + line + ": " + before + ", CanBeCombined " + can.ToString();
        stacked = stacked + ", CombineItemsClient sent via " + OZS_Mirrors.s_Via;
        Note(stacked);
    }

    protected void GroundSwap(string line)
    {
        array<string> parts = new array<string>();
        line.Split(" ", parts);
        PlayerBase me = PlayerBase.Cast(GetGame().GetPlayer());
        OZS_Mirror m = OZS_Mirrors.Get().Newest();
        if (!me || !m || parts.Count() < 3)
        {
            Note("=== " + line + ": gswap <cls> <handle>");
            return;
        }
        EntityAI loose = NearestLoose(me.GetPosition(), parts.Get(1));
        EntityAI target = m.ByHandle(parts.Get(2).ToInt());
        if (!loose || !target)
        {
            Note("=== " + line + ": no loose " + parts.Get(1) + " within 4 m, or no such handle");
            return;
        }
        // WHAT THE SCREEN'S OWN TESTS SAY ABOUT THE PAIR, for the record:
        // whether vanilla would have offered the swap at all, and what its
        // drop handler would choose. Then the same routing the handler makes.
        InventoryLocation dst = new InventoryLocation();
        bool can = GameInventory.CanSwapEntitiesEx(loose, target);
        bool force = GameInventory.CanForceSwapEntitiesEx(loose, null, target, dst);
        int tested = InventoryCombinationFlags.COMBINE_QUANTITY2 | InventoryCombinationFlags.ADD_AS_CARGO | InventoryCombinationFlags.ADD_AS_ATTACHMENT | InventoryCombinationFlags.SWAP | InventoryCombinationFlags.FSWAP | InventoryCombinationFlags.SWAP_MAGAZINE;
        int flag = ItemManager.GetChosenCombinationFlag(loose, target, tested, dst);
        bool sent = false;
        if (flag == InventoryCombinationFlags.NONE)
            sent = OZS_Mirrors.GroundSwap(loose, target);
        else if (flag == InventoryCombinationFlags.SWAP)
            sent = me.PredictiveSwapEntities(target, loose);
        else if (flag == InventoryCombinationFlags.FSWAP)
            sent = me.PredictiveForceSwapEntities(loose, target, dst);
        // In pieces: one expression of this length is "Formula too complex"
        // to the compiler (measured 2026-09-26).
        string told = "=== " + line + ": " + loose.GetType() + " (loose) onto #" + parts.Get(2) + " " + target.GetType();
        told = told + ": CanSwapEntitiesEx " + can.ToString() + ", CanForceSwapEntitiesEx " + force.ToString();
        told = told + ", chosen flag " + flag.ToString() + " -> sent " + sent.ToString() + " via " + OZS_Mirrors.s_Via;
        Note(told);
    }

    protected void Note(string text)
    {
        FileHandle nf = OpenFile(SCAN_FILE, FileMode.APPEND);
        if (nf == 0)
            return;
        FPrintln(nf, text);
        CloseFile(nf);
    }

    // What the player holds, into the nearest storage box's cargo, through
    // PredictiveTakeEntityToTargetCargo: the same call the inventory screen
    // makes when a player drags an item from the hands onto a container.
    protected void Stash(string line)
    {
        FileHandle stashFile = OpenFile(SCAN_FILE, FileMode.APPEND);
        PlayerBase me = PlayerBase.Cast(GetGame().GetPlayer());
        EntityAI held = null;
        if (me)
            held = me.GetHumanInventory().GetEntityInHands();
        OZ_StorageBox box = null;
        if (me)
        {
            array<Object> objs = new array<Object>();
            GetGame().GetObjectsAtPosition(me.GetPosition(), 6, objs, null);
            for (int i = 0; i < objs.Count(); i++)
            {
                OZ_StorageBox b = OZ_StorageBox.Cast(objs.Get(i));
                if (b)
                {
                    box = b;
                    break;
                }
            }
        }
        string what = "=== stash [" + line + "] t=" + GetGame().GetTickTime();
        if (!held)
            what = what + ": nothing in hands";
        else if (!box)
            what = what + ": no storage box within 6 m";
        else
            what = what + ": " + held.GetType() + " -> " + box.GetType() + " predictive=" + me.PredictiveTakeEntityToTargetCargo(box, held);
        if (stashFile != 0)
        {
            FPrintln(stashFile, what);
            CloseFile(stashFile);
        }
    }

    protected void Scan(string line)
    {
        float radius = 15;
        array<string> parts = new array<string>();
        line.Split(" ", parts);
        if (parts.Count() > 1)
            radius = parts.Get(1).ToFloat();
        PlayerBase me = PlayerBase.Cast(GetGame().GetPlayer());
        if (!me)
        {
            ErrorEx("[OpenZone] probe scan: no player", ErrorExSeverity.WARNING);
            return;
        }
        array<Object> objs = new array<Object>();
        GetGame().GetObjectsAtPosition(me.GetPosition(), radius, objs, null);
        // The .RPT of the retail client is flushed minutes late, so the scan
        // also goes to its own file, appended, one block per scan, the way the
        // client monitor writes client.log.
        FileHandle scanFile = OpenFile(SCAN_FILE, FileMode.APPEND);
        if (scanFile != 0)
            FPrintln(scanFile, "=== scan [" + line + "] t=" + GetGame().GetTickTime() + " at " + me.GetPosition().ToString(false));
        int loose = 0;
        for (int i = 0; i < objs.Count(); i++)
        {
            ItemBase it = ItemBase.Cast(objs.Get(i));
            if (!it)
                continue;
            if (it.GetHierarchyParent())
                continue;
            loose++;
            string row = "[OpenZone] probe scan: loose " + it.GetType() + " at " + it.GetPosition().ToString(false);
            row = row + " netid " + it.GetNetworkIDString();
            ErrorEx(row, ErrorExSeverity.WARNING);
            if (scanFile != 0)
                FPrintln(scanFile, "loose " + it.GetType() + " at " + it.GetPosition().ToString(false) + " netid " + it.GetNetworkIDString());
        }
        ErrorEx("[OpenZone] probe scan: " + loose + " loose item(s) within " + radius + " m of " + me.GetPosition().ToString(false), ErrorExSeverity.WARNING);
        if (scanFile != 0)
        {
            FPrintln(scanFile, "total " + loose + " loose within " + radius + " m");
            CloseFile(scanFile);
        }
    }
}
#endif
