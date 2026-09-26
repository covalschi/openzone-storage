// The `oz_storage` verb, following the bridge's contract for project verbs.
//
//   world_exec verb=oz_storage args={"op":"spawn","size":"large","pos":"x y z"}
//   world_exec verb=oz_storage args={"op":"list"}
//   world_exec verb=oz_storage args={"op":"status","id":"<box id>"}      (or pos, or nearest to the player)
//   world_exec verb=oz_storage args={"op":"files","id":"<box id>"}
//   world_exec verb=oz_storage args={"op":"tune","ping":"100","wait":"1"}                    (stand: settings at runtime)
//   world_exec verb=oz_storage args={"op":"clean","cls":"Rag","value":"1"}                   (stand: mark the player's stack disinfected)
//   world_exec verb=oz_storage args={"op":"probe","class":"OZ_PersonalStash","slot":"Body"}          (stand measurement)
//   world_exec verb=oz_storage args={"op":"auth","do":"make","id":"<box id>"}                 (stand: the authoritative box, design 2026-09-24)
//   world_exec verb=oz_storage args={"op":"proxy","do":"open","id":"<box id>"}                (stand: a proxy session, design 2026-09-24)
//
// `open`, `close`, `open_all`, `close_all`, `fill`, `slot` and `auth do=close`
// stood here until 2026-09-26: the old scheme's verbs, which materialised the
// record into the PLACED box or captured its cargo. A placed box holds
// nothing now; everything goes through `proxy do=...`.
modded class DZMCP_BridgeCore
{
    override protected string KnownVerbs()
    {
        return super.KnownVerbs() + ", oz_storage";
    }

    override protected bool IsKnownVerb(string verb)
    {
        if (verb == "oz_storage")
            return true;
        return super.IsKnownVerb(verb);
    }

    override protected void Dispatch(string verb, string raw)
    {
        if (verb != "oz_storage")
        {
            super.Dispatch(verb, raw);
            return;
        }

        DZMCP_CommandFull full = new DZMCP_CommandFull();
        string parseError;
        if (!m_Json.ReadFromString(full, raw, parseError))
        {
            FinishCommand(DZMCP_STATUS_FAILED, "oz_storage: the args block could not be parsed -- every value must be a string: " + Excerpt(parseError));
            return;
        }

        map<string, string> args = full.args;
        string op = OZS_Arg(args, "op", "list");
        string detail;
        bool ok = OZS_Run(op, args, detail);
        if (ok)
            FinishCommand(DZMCP_STATUS_DONE, detail);
        else
            FinishCommand(DZMCP_STATUS_FAILED, detail);
    }

    protected string OZS_Arg(map<string, string> args, string key, string fallback)
    {
        if (!args)
            return fallback;
        string v;
        if (args.Find(key, v))
            return v;
        return fallback;
    }

    protected bool OZS_Run(string op, map<string, string> args, out string detail)
    {
        OZS_Controller c = OZS_Controller.Get();

        if (op == "list")
        {
            detail = c.Status();
            return true;
        }

        if (op == "tune")
        {
            // Runtime overrides of the settings, for the stand only. `ping`
            // is the fake round trip in ms and `wait` is WaitForRecord, so
            // the feel of a box at a real latency can be tried without a
            // restart (owner asked for it, 2026-09-25).
            OZS_Settings st = OZS_Settings.Get();
            string v = OZS_Arg(args, "rate", "");
            if (v != "")
                st.OpenItemsPerSecond = v.ToInt();
            v = OZS_Arg(args, "px_rows", "");
            if (v != "")
                st.ProxyRowsPerMessage = v.ToInt();
            v = OZS_Arg(args, "px_msgs", "");
            if (v != "")
                st.ProxyMessagesPerFrame = v.ToInt();
            v = OZS_Arg(args, "px_idle", "");
            if (v != "")
                st.ProxyIdleSeconds = v.ToInt();
            v = OZS_Arg(args, "release", "");
            if (v != "")
                st.ReleaseDeletesPerFrame = v.ToInt();
            v = OZS_Arg(args, "ping", "");
            if (v != "")
                st.FakePingMs = v.ToInt();
            v = OZS_Arg(args, "wait", "");
            if (v != "")
            {
                bool wantWait = v.ToInt() != 0;
                st.WaitForRecord = wantWait;
            }
            detail = "rate=" + st.OpenItemsPerSecond;
            detail = detail + " proxy=" + st.ProxyRowsPerMessage + "x" + st.ProxyMessagesPerFrame + "/frame idle=" + st.ProxyIdleSeconds + "s";
            detail = detail + " release=" + st.ReleaseDeletesPerFrame + "/frame";
            detail = detail + " ping=" + st.FakePingMs + "ms wait_for_record=" + st.WaitForRecord;
            return true;
        }

        if (op == "clean")
        {
            // STAND ONLY: the player's first <cls> is marked disinfected
            // (value=1) or dirty (0), the way ActionDisinfect would, so a
            // report about a disinfected stack can be replayed without the
            // spray and the gesture (2026-09-26).
            array<Man> cleanMen = new array<Man>();
            GetGame().GetPlayers(cleanMen);
            if (cleanMen.Count() == 0)
            {
                detail = "clean needs a player";
                return false;
            }
            PlayerBase cleanMe = PlayerBase.Cast(cleanMen.Get(0));
            string ccls = OZS_Arg(args, "cls", "Rag");
            int cval = OZS_Arg(args, "value", "1").ToInt();
            array<EntityAI> cleanCarried = new array<EntityAI>();
            cleanMe.GetInventory().EnumerateInventory(InventoryTraversalType.PREORDER, cleanCarried);
            for (int cleanIdx = 0; cleanIdx < cleanCarried.Count(); cleanIdx++)
            {
                ItemBase cleanItem = ItemBase.Cast(cleanCarried.Get(cleanIdx));
                if (!cleanItem || cleanItem == cleanMe || !cleanItem.IsKindOf(ccls))
                    continue;
                cleanItem.SetCleanness(cval);
                detail = cleanItem.GetType() + " qty " + cleanItem.GetQuantity().ToString() + " cleanness " + cleanItem.GetCleanness().ToString() + " netid " + cleanItem.GetNetworkIDString();
                return true;
            }
            detail = "no " + ccls + " on the player";
            return false;
        }

        if (op == "spawn")
        {
            string size = OZS_Arg(args, "size", "large");
            string type = "OZ_StorageBox_Large";
            if (size == "small")
                type = "OZ_StorageBox_Small";
            else if (size == "medium")
                type = "OZ_StorageBox_Medium";
            vector pos;
            string posText = OZS_Arg(args, "pos", "");
            if (posText == "")
            {
                if (!OZS_PlayerPos(pos))
                {
                    detail = "spawn needs pos=\"x y z\" when nobody is connected";
                    return false;
                }
            }
            else
            {
                pos = posText.ToVector();
            }
            Object o = GetGame().CreateObjectEx(type, pos, ECE_PLACE_ON_SURFACE | ECE_NOLIFETIME);
            OZ_StorageBox box = OZ_StorageBox.Cast(o);
            if (!box)
            {
                detail = "the engine returned no " + type + " at " + pos.ToString();
                return false;
            }
            detail = "spawned " + type + " id=" + box.OZS_GetId() + " at " + box.GetPosition().ToString();
            return true;
        }

        if (op == "persist")
        {
            // STAND ONLY: does ECE_NOPERSISTENCY_WORLD really keep a container
            // AND ITS CONTENTS out of the engine's save? The flag's comment in
            // centraleconomy.c says "do not save this object in world", but a
            // comment is not a measurement -- and the engine autosaves
            // continuously (a crate with 5000 items has survived taskkill
            // before), so this is load-bearing enough to ask rather than
            // assume.
            //
            // Three chests, one metre apart, filled the same way:
            //   +0 m  plain              (control: must come back)
            //   +2 m  NOPERSISTENCY      (must be gone)
            //   +4 m  LOCAL|NOPERSISTENCY (the shape the design wants)
            //
            // Items are created into them with CreateEntityInCargo, which
            // takes no flags -- so this also answers what happens to CHILDREN
            // of a parent that is not saved.
            string pCls = OZS_Arg(args, "class", "SeaChest");
            string pItem = OZS_Arg(args, "item", "BandageDressing");
            int pCount = OZS_Arg(args, "count", "3").ToInt();
            vector pAt;
            if (!OZS_PlayerPos(pAt))
            {
                detail = "nobody is connected";
                return false;
            }
            array<int> pFlags = new array<int>();
            array<string> pNames = new array<string>();
            pFlags.Insert(ECE_PLACE_ON_SURFACE);
            pNames.Insert("plain");
            pFlags.Insert(ECE_PLACE_ON_SURFACE | ECE_NOPERSISTENCY_WORLD);
            pNames.Insert("nopersist");
            pFlags.Insert(ECE_PLACE_ON_SURFACE | ECE_LOCAL | ECE_NOPERSISTENCY_WORLD);
            pNames.Insert("local+nopersist");
            // THE ONE THE RESTORE ACTUALLY USED, and the reason this verb was
            // opened again on 2026-09-25: a ground-built container carried
            // ECE_LOCAL and ECE_NOLIFETIME but NOT ECE_NOPERSISTENCY_WORLD.
            // If "local" alone were enough to keep an object out of the save,
            // the copies beside the box could not have come from there -- so
            // this case decides whether the diagnosis is right.
            pFlags.Insert(ECE_PLACE_ON_SURFACE | ECE_LOCAL | ECE_NOLIFETIME);
            pNames.Insert("local-only (the old restore flags)");
            detail = "";
            for (int pi = 0; pi < pFlags.Count(); pi++)
            {
                vector spot = pAt;
                spot[0] = spot[0] + 2 * (pi + 1);
                Object pMade = GetGame().CreateObjectEx(pCls, spot, pFlags.Get(pi));
                EntityAI pBox = EntityAI.Cast(pMade);
                if (!pBox)
                {
                    detail = detail + pNames.Get(pi) + ": NOT CREATED | ";
                    continue;
                }
                int pIn = 0;
                for (int pj = 0; pj < pCount; pj++)
                {
                    if (pBox.GetInventory().CreateEntityInCargo(pItem))
                        pIn++;
                }
                detail = detail + pNames.Get(pi) + " at " + spot.ToString(false) + " netid " + pBox.GetNetworkIDString() + " with " + pIn.ToString() + " | ";
            }
            detail = detail + "now stop and start the server, then look for them";
            return true;
        }

        if (op == "chain")
        {
            // STAND ONLY: hang a CHAIN of attachments of ANY LENGTH off a
            // container and then put something in the last one's cargo.
            //
            // `chain` is "Class@Slot,Class@Slot,..." -- as many links as you
            // like. Nothing here counts levels, because nothing in the mod
            // counts levels either: the restore walks a parent-indexed tree of
            // whatever depth the capture found. The only depth limit in play
            // is the ENGINE's, AreChildrenAccessible()'s budget of
            // INVENTORY_MAX_REACHABLE_DEPTH_ATT = 2 attachment steps, and this
            // op exists to find where that budget actually runs out rather
            // than to assert a number.
            string cHost = OZS_Arg(args, "host", "OZ_PersonalStash");
            string cChain = OZS_Arg(args, "chain", "");
            string cInner = OZS_Arg(args, "inner", "Nail");
            vector cAt;
            if (!OZS_PlayerPos(cAt))
            {
                detail = "nobody is connected";
                return false;
            }
            array<Object> cAround = new array<Object>();
            GetGame().GetObjectsAtPosition3D(cAt, 40.0, cAround, null);
            EntityAI cNode = null;
            for (int ci = 0; ci < cAround.Count(); ci++)
            {
                EntityAI cCand = EntityAI.Cast(cAround.Get(ci));
                if (cCand && cCand.GetType() == cHost)
                {
                    cNode = cCand;
                    break;
                }
            }
            if (!cNode)
            {
                detail = "no '" + cHost + "' within 40 m";
                return false;
            }
            array<string> cLinks = new array<string>();
            cChain.Split(",", cLinks);
            string cPath = cHost;
            for (int cl = 0; cl < cLinks.Count(); cl++)
            {
                array<string> cPair = new array<string>();
                cLinks.Get(cl).Split("@", cPair);
                if (cPair.Count() != 2)
                {
                    detail = "link " + (cl + 1).ToString() + " is not Class@Slot: '" + cLinks.Get(cl) + "'";
                    return false;
                }
                EntityAI cMade = EntityAI.Cast(cNode.GetInventory().CreateAttachmentEx(cPair.Get(0), InventorySlots.GetSlotIdFromString(cPair.Get(1))));
                if (!cMade)
                {
                    detail = "REFUSED at link " + (cl + 1).ToString() + ": '" + cPair.Get(0) + "' would not go into the '" + cPair.Get(1) + "' slot of " + cNode.GetType() + " (reachable=" + cNode.GetInventory().AreChildrenAccessible().ToString() + ")";
                    return false;
                }
                cNode = cMade;
                cPath = cPath + " <- " + cPair.Get(0) + "@" + cPair.Get(1);
            }
            Object cLoose = GetGame().CreateObjectEx(cInner, cAt, ECE_PLACE_ON_SURFACE);
            EntityAI cItem = EntityAI.Cast(cLoose);
            if (!cItem)
            {
                detail = "'" + cInner + "' could not be created at all -- is that a class?";
                return false;
            }
            bool cReach = cNode.GetInventory().AreChildrenAccessible();
            if (!cNode.GetInventory().TakeEntityToCargo(InventoryMode.SERVER, cItem))
            {
                detail = cPath + ": REFUSED '" + cInner + "' into the cargo of the last link (AreChildrenAccessible=" + cReach.ToString() + ")";
                return false;
            }
            detail = cPath + " <- '" + cInner + "' in its cargo: every link accepted";
            return true;
        }

        if (op == "nest")
        {
            // STAND ONLY: does a container of class `nestHost` take an item into
            // its cargo, and does THAT item then take one into its own? The
            // owner reported that items inside the personal stash refuse to
            // hold anything, so this asks the engine the same question twice
            // and reports which of the two refused.
            string hostClass = OZS_Arg(args, "host", "OZ_PersonalStash");
            string outerClass = OZS_Arg(args, "item", "TTSKOJacket");
            string innerClass = OZS_Arg(args, "inner", "BandageDressing");
            vector nestAt;
            if (!OZS_PlayerPos(nestAt))
            {
                detail = "nobody is connected";
                return false;
            }
            array<Object> nestAround = new array<Object>();
            GetGame().GetObjectsAtPosition3D(nestAt, 40.0, nestAround, null);
            EntityAI nestHost = null;
            for (int ni = 0; ni < nestAround.Count(); ni++)
            {
                EntityAI nestCand = EntityAI.Cast(nestAround.Get(ni));
                if (nestCand && nestCand.GetType() == hostClass)
                {
                    nestHost = nestCand;
                    break;
                }
            }
            if (!nestHost)
            {
                detail = "no '" + hostClass + "' within 40 m";
                return false;
            }

            // With `slot`, the outer item is ATTACHED instead of dropped into
            // cargo. That is the whole question: the engine lets an attachment
            // keep its own cargo reachable and a cargo item does not.
            string nestSlot = OZS_Arg(args, "slot", "");
            EntityAI nestOuter = null;
            if (nestSlot != "")
                nestOuter = EntityAI.Cast(nestHost.GetInventory().CreateAttachmentEx(outerClass, InventorySlots.GetSlotIdFromString(nestSlot)));
            else
                nestOuter = EntityAI.Cast(nestHost.GetInventory().CreateEntityInCargo(outerClass));
            if (!nestOuter)
            {
                detail = hostClass + " REFUSED '" + outerClass + "' into '" + nestSlot + "' (empty means cargo)";
                return false;
            }
            // The MOVE path, not the creation path: a player dragging an item
            // is a move, and the two have different validation. Creating into
            // a nested container fails even in a plain vanilla box, so the
            // creation path says nothing about what the owner saw.
            Object nestLoose = GetGame().CreateObjectEx(innerClass, nestAt, ECE_PLACE_ON_SURFACE);
            EntityAI nestInner = EntityAI.Cast(nestLoose);
            if (!nestInner)
            {
                detail = "'" + innerClass + "' could not be created at all -- is that a class?";
                return false;
            }
            bool nestMoved = nestOuter.GetInventory().TakeEntityToCargo(InventoryMode.SERVER, nestInner);
            if (!nestMoved)
            {
                detail = hostClass + " took '" + outerClass + "', but the MOVE of '" + innerClass + "' into ITS cargo was refused";
                return false;
            }
            detail = hostClass + " took '" + outerClass + "', which took a MOVED '" + innerClass + "': both levels accepted";
            return true;
        }

        // `stash` -- a physical stash put in the world with a chosen owner --
        // stood here until 2026-09-26. Nothing of that class is placed in
        // the world any more; a stash is a proxy session (`proxy do=open`
        // through a rack).

        if (op == "probe")
        {
            // STAND ONLY, measurement (personal stash spec 2026-09-23 M1):
            // does an ORDINARY container accept an item in a VANILLA character
            // slot? Reads the nearest entity of `class` -- any entity, not only
            // a storage box -- and reports what its inventory says and whether
            // the attachment was made. Nothing else in the mod depends on it.
            string probeClass = OZS_Arg(args, "class", "OZ_StashSlotProbe");
            string probeItem = OZS_Arg(args, "item", "");
            string probeSlot = OZS_Arg(args, "slot", "Body");
            vector probeAt;
            if (!OZS_PlayerPos(probeAt))
            {
                detail = "nobody is connected";
                return false;
            }
            array<Object> found = new array<Object>();
            GetGame().GetObjectsAtPosition3D(probeAt, 30.0, found, null);
            EntityAI host = null;
            for (int fi = 0; fi < found.Count(); fi++)
            {
                EntityAI cand = EntityAI.Cast(found.Get(fi));
                if (cand && cand.GetType() == probeClass)
                {
                    host = cand;
                    break;
                }
            }
            if (!host)
            {
                detail = "no " + probeClass + " within 30 m of the player";
                return false;
            }
            int probeId = InventorySlots.GetSlotIdFromString(probeSlot);
            if (probeId == InventorySlots.INVALID)
            {
                detail = "the engine does not know a slot called " + probeSlot;
                return false;
            }
            detail = probeClass + ": slot " + probeSlot + " id=" + probeId.ToString();
            if (host.GetInventory().HasInventorySlot(probeId))
                detail += ", declared on the container YES";
            else
                detail += ", declared on the container NO";
            if (probeItem == "")
                return true;
            EntityAI made = host.GetInventory().CreateAttachmentEx(probeItem, probeId);
            if (made)
                detail += "; " + made.GetType() + " ATTACHED";
            else
                detail += "; " + probeItem + " REFUSED";
            return true;
        }

        if (op == "proxy")
        {
            // STAND ONLY (proxy design 2026-09-24, stage B): drives a proxy
            // session without a screen, so the wire can be measured before the
            // screen exists.
            //
            //   do=open   id=<box id>              show it to the first player
            //   do=shut   id=<box id>
            //   do=status
            //   do=move   id=<box id> handle=N row=R col=C [into=H] [slot=S]
            //   do=out    id=<box id> handle=N
            //   do=in     id=<box id> item=<class> [row=R col=C]
            string pxDo = OZS_Arg(args, "do", "status");
            if (pxDo == "status")
            {
                detail = OZS_Proxies.Get().Status();
                return true;
            }
            if (pxDo == "grid")
            {
                // THE BOX'S REAL GEOMETRY, cell by cell. "Visually empty" and
                // "the engine thinks it is taken" are different claims, and
                // until this existed there was no way to tell them apart from
                // outside the game (2026-09-25).
                string gridId = OZS_Arg(args, "id", "");
                string gridOnly = OZS_Arg(args, "only", "");
                OZS_Session gs = OZS_Proxies.Get().Find(gridId);
                if (!gs || !gs.m_Auth)
                {
                    detail = "no session for " + gridId;
                    return false;
                }
                CargoBase gc = gs.m_Auth.GetInventory().GetCargo();
                if (!gc)
                {
                    detail = "that box has no cargo";
                    return false;
                }
                detail = "grid " + gc.GetWidth().ToString() + "x" + gc.GetHeight().ToString();
                for (int gi = 0; gi < gc.GetItemCount(); gi++)
                {
                    EntityAI ge = gc.GetItem(gi);
                    // `only=<text>` narrows the dump to the items whose class
                    // contains it. A box of sixty items writes more detail
                    // than the bridge will carry in one answer, and the line
                    // is cut where it is cut.
                    if (gridOnly != "")
                    {
                        if (ge && ge.GetType().IndexOf(gridOnly) < 0)
                            continue;
                    }
                    if (!ge)
                        continue;
                    int gr;
                    int gcl;
                    // From the item, not from the cargo index: the index
                    // accessor writes numbers that are not the item's
                    // (measured 2026-09-25).
                    OZS_Ops.Where(ge, gr, gcl);
                    int gw;
                    int gh;
                    gc.GetItemSize(gi, gw, gh);
                    int fw;
                    int fh;
                    bool gknown = OZS_Ops.SizeOf(ge, fw, fh);
                    detail = detail + " | #" + OZS_Authority.Handle(gs.m_Auth, ge).ToString() + " " + ge.GetType();
                    detail = detail + " at " + gr.ToString() + "," + gcl.ToString() + " size " + gw.ToString() + "x" + gh.ToString();
                    // THE SAME QUESTION ASKED TWICE. The cargo's own numbers
                    // and the config's, side by side: if they disagree about
                    // which of the two is the width, every rectangle this mod
                    // measures is transposed, and a cell that looks free is
                    // refused (owner, 2026-09-26, a rag that lies across).
                    int cfgW;
                    int cfgH;
                    GetGame().GetInventoryItemSize(InventoryItem.Cast(ge), cfgW, cfgH);
                    detail = detail + " cfg " + cfgW.ToString() + "x" + cfgH.ToString();
                    // The rectangle it actually covers, which differs from its
                    // own size whenever it lies turned.
                    if (OZS_Ops.Flipped(ge))
                        detail = detail + " FLIPPED covers " + fw.ToString() + "x" + fh.ToString();
                    // BOTH NUMBERS OFF THE SAME LIVE ENTITY, because they are
                    // not the same number and the difference decides whether
                    // an emptied stack is deleted and what the panel draws.
                    ItemBase gib = ItemBase.Cast(ge);
                    if (gib)
                    {
                        detail = detail + " qty " + gib.GetQuantity().ToString();
                        Magazine gmag = Magazine.Cast(ge);
                        if (gmag)
                            detail = detail + " ammo " + gmag.GetAmmoCount().ToString() + "/" + gmag.GetAmmoMax().ToString();
                    }
                }
                int ga = gs.m_Auth.GetInventory().AttachmentCount();
                detail = detail + " | slots used " + ga.ToString();
                // WHAT THE ENGINE ACTUALLY KNOWS ABOUT THIS BOX'S SLOTS.
                //
                // Three separate claims that were all being assumed: what the
                // CONFIG declares, what the engine's slot table resolves those
                // names to, and what the CONTAINER says it has. A weapon that
                // would not hang on the rack had all three disagreeing
                // somewhere, and nothing printed any of them (2026-09-25).
                array<string> gslots = new array<string>();
                GetGame().ConfigGetTextArray("CfgVehicles " + gs.m_Auth.GetType() + " attachments", gslots);
                detail = detail + " | config declares " + gslots.Count().ToString() + ":";
                for (int gsi = 0; gsi < gslots.Count(); gsi++)
                {
                    string gsn = gslots.Get(gsi);
                    int gsid = InventorySlots.GetSlotIdFromString(gsn);
                    string gshas = "absent";
                    if (gsid != InventorySlots.INVALID && gs.m_Auth.GetInventory().HasAttachmentSlot(gsid))
                    {
                        gshas = "empty";
                        // WHICH SLOT ACTUALLY HOLDS IT. "The weapon fell into
                        // the first one" is two different bugs depending on
                        // whether the ENGINE put it there or the PANEL merely
                        // draws it first, and nothing distinguished them.
                        EntityAI gsin = gs.m_Auth.GetInventory().FindAttachment(gsid);
                        if (gsin)
                            gshas = gsin.GetType();
                    }
                    detail = detail + " " + gsn + "=" + gshas;
                }
                detail = detail + " | the box offers " + gs.m_Auth.GetInventory().GetAttachmentSlotsCount().ToString() + " slot(s)";
                // DO THE ENGINE'S OWN PRE-CHECKS STILL REFUSE THIS BOX?
                //
                // The whole server-side swap was written around the answer
                // "yes, because the authority is a container the engine was
                // never told about" (measured at e1df77e). That measurement
                // predates everything built since, so it is taken again here,
                // on two real items of a real box, rather than assumed.
                if (gc.GetItemCount() > 1)
                {
                    EntityAI one = gc.GetItem(0);
                    EntityAI two = gc.GetItem(1);
                    if (one && two)
                    {
                        detail = detail + " | natives on " + one.GetType() + "+" + two.GetType();
                        detail = detail + ": CanSwapEntities=" + GameInventory.CanSwapEntities(one, two).ToString();
                        InventoryLocation forTwo = new InventoryLocation();
                        detail = detail + " CanForceSwapEntities=" + GameInventory.CanForceSwapEntities(one, null, two, forTwo).ToString();
                    }
                }
                return true;
            }
            string pxId = OZS_Arg(args, "id", "");
            if (pxId == "")
            {
                detail = "proxy do=" + pxDo + " needs id=<box id>";
                return false;
            }
            array<Man> pxMen = new array<Man>();
            GetGame().GetPlayers(pxMen);
            if (pxMen.Count() == 0)
            {
                detail = "nobody is connected";
                return false;
            }
            PlayerIdentity pxWho = pxMen.Get(0).GetIdentity();
            if (!pxWho)
            {
                detail = "the player has no identity";
                return false;
            }
            if (pxDo == "open")
            {
                OZ_StorageBox pxAnchor = c.FindById(pxId);
                if (!pxAnchor)
                {
                    detail = "no box with id " + pxId + " is in the world to be the anchor";
                    return false;
                }
                string pxWhy;
                if (!OZS_Proxies.Get().Open(pxId, pxAnchor.GetType(), pxAnchor.GetPosition(), pxWho, pxWhy))
                {
                    detail = "open refused: " + pxWhy;
                    return false;
                }
                detail = "session opening for " + pxId + " -> " + OZS_Proxies.Get().Status();
                return true;
            }
            if (pxDo == "shut")
            {
                OZS_Proxies.Get().Shut(pxId, pxWho);
                detail = "shut -> " + OZS_Proxies.Get().Status();
                return true;
            }
            OZS_Session pxS = OZS_Proxies.Get().Find(pxId);
            if (!pxS)
            {
                detail = "no session for " + pxId;
                return false;
            }
            OZS_Watcher pxW = pxS.WatcherOf(pxWho);
            if (!pxW)
            {
                detail = "that player is not watching " + pxId;
                return false;
            }
            int pxHandle = OZS_Arg(args, "handle", "1").ToInt();
            int pxRow = OZS_Arg(args, "row", "-1").ToInt();
            int pxCol = OZS_Arg(args, "col", "-1").ToInt();
            // THE WAY ROUND, ON PURPOSE. A turned item is the one shape the
            // proxy got wrong, and until this existed there was no way to ask
            // for one from outside the game -- the only turned can in the box
            // had arrived by hand (2026-09-25).
            int pxFlip = OZS_Arg(args, "flip", "0").ToInt();
            int pxInto = OZS_Arg(args, "into", "0").ToInt();
            string pxSlot = OZS_Arg(args, "slot", "");
            int pxLt = InventoryLocationType.CARGO;
            // The same sentinel the game uses; -1 is a possible slot id, not
            // a way of saying "none".
            int pxSlotId = InventorySlots.INVALID;
            if (pxSlot != "")
            {
                pxSlotId = InventorySlots.GetSlotIdFromString(pxSlot);
                pxLt = InventoryLocationType.ATTACHMENT;
            }
            if (pxDo == "move")
            {
                pxS.Operate(pxWho, OZS_Const.OP_MOVE, pxHandle, pxInto, 0, 0, pxLt, pxSlotId, pxRow, pxCol, pxFlip, pxS.m_Version);
                detail = "move sent -> " + pxS.Status();
                return true;
            }
            if (pxDo == "out")
            {
                pxS.Operate(pxWho, OZS_Const.OP_OUT, pxHandle, 0, 0, 0, pxLt, pxSlotId, pxRow, pxCol, 0, pxS.m_Version);
                detail = "out sent -> " + pxS.Status();
                return true;
            }
            if (pxDo == "swap")
            {
                pxS.Operate(pxWho, OZS_Const.OP_SWAP, pxHandle, OZS_Arg(args, "other", "2").ToInt(), 0, 0, pxLt, pxSlotId, pxRow, pxCol, 0, pxS.m_Version);
                detail = "swap sent -> " + pxS.Status();
                return true;
            }
            if (pxDo == "seed")
            {
                // STAND ONLY: many small items into the box by the ORDINARY
                // inbound path. IN TWO STAGES: `stage=make` puts them in the
                // player's own inventory, `stage=send` sends those of the
                // named classes into the box.
                //
                // NOT VIA THE GROUND, and not both in one call. Either one
                // ends the server process inside `TakeToDst` -- a ground
                // source kills it outright (the mod refuses that now), and an
                // item created and moved in the same frame is the engine trap
                // the skill records. Both were measured here on 2026-09-25,
                // and neither leaves a crash dump.
                //
                //   do=seed stage=make [classes=A,B,C] [count=12]
                //   do=seed stage=send [classes=A,B,C] [count=12]
                string seedStage = OZS_Arg(args, "stage", "make");
                int seedCount = OZS_Arg(args, "count", "12").ToInt();
                if (seedCount < 1 || seedCount > 60)
                {
                    detail = "count must be between 1 and 60";
                    return false;
                }
                // SEVERAL CLASSES ON PURPOSE. A box of one class is a box
                // where every descriptor is the same size and every stack can
                // merge with every other; mixed types are what a real box
                // holds (owner, 2026-09-25).
                string seedList = OZS_Arg(args, "classes", "Ammo_762x39,Ammo_556x45,Ammo_9x19,Ammo_45ACP,Apple,Plum,Nail,Rag");
                array<string> seedClasses = new array<string>();
                seedList.Split(",", seedClasses);
                if (seedClasses.Count() == 0)
                {
                    detail = "no classes named";
                    return false;
                }
                for (int seedTrim = 0; seedTrim < seedClasses.Count(); seedTrim++)
                {
                    string seedOne = seedClasses.Get(seedTrim);
                    seedOne.TrimInPlace();
                    seedClasses.Set(seedTrim, seedOne);
                }
                PlayerBase seedMe = PlayerBase.Cast(pxMen.Get(0));
                int seedDone = 0;
                if (seedStage == "make")
                {
                    for (int seedStep = 0; seedStep < seedCount; seedStep++)
                    {
                        string seedCls = seedClasses.Get(seedStep % seedClasses.Count());
                        if (seedCls == "")
                            continue;
                        if (seedMe.GetInventory().CreateInInventory(seedCls))
                            seedDone++;
                    }
                    detail = "made " + seedDone.ToString() + " of " + seedCount.ToString() + " in the player; send them with stage=send";
                    return true;
                }
                if (seedStage == "send")
                {
                    array<EntityAI> seedHeld = new array<EntityAI>();
                    seedMe.GetInventory().EnumerateInventory(InventoryTraversalType.PREORDER, seedHeld);
                    // AND WHAT IS LYING AROUND. The box takes a loose item as
                    // readily as a carried one (design section 6), and after
                    // the destination check went in it does so without killing
                    // anything -- which is the case this seeder exists to
                    // exercise (2026-09-25).
                    array<Object> seedNear = new array<Object>();
                    GetGame().GetObjectsAtPosition3D(seedMe.GetPosition(), 4.0, seedNear, null);
                    for (int seedN = 0; seedN < seedNear.Count(); seedN++)
                    {
                        ItemBase seedLoose = ItemBase.Cast(seedNear.Get(seedN));
                        if (!seedLoose || seedLoose.GetHierarchyParent())
                            continue;
                        if (OZ_StorageBox.Cast(seedLoose))
                            continue;
                        seedHeld.Insert(seedLoose);
                    }
                    for (int seedI = 0; seedI < seedHeld.Count(); seedI++)
                    {
                        if (seedDone >= seedCount)
                            break;
                        EntityAI seedItem = seedHeld.Get(seedI);
                        if (!seedItem)
                            continue;
                        // ONLY WHAT THIS COMMAND MADE. The player is wearing
                        // their own gear and it is in the same enumeration;
                        // naming the classes is what keeps the seeder from
                        // posting somebody's trousers into the box.
                        if (seedClasses.Find(seedItem.GetType()) < 0)
                            continue;
                        int seedLow;
                        int seedHigh;
                        seedItem.GetNetworkID(seedLow, seedHigh);
                        pxS.Operate(pxWho, OZS_Const.OP_IN, 0, 0, seedLow, seedHigh, InventoryLocationType.CARGO, -1, -1, -1, 0, pxS.m_Version);
                        seedDone++;
                    }
                    detail = "sent " + seedDone.ToString() + " item(s) -> " + pxS.Status();
                    return true;
                }
                detail = "seed: stage must be make or send";
                return false;
            }
            if (pxDo == "burst")
            {
                // STAND ONLY: several operations in one handler call, which is
                // the only way from outside to put more than one turn in the
                // air at a time -- every bridge verb waits for its own command
                // to finish, and a local round trip is faster than the gap
                // between two of them. It measures the queue of section 8.3
                // and gives the crash test of stage E something to interrupt.
                int burstCount = OZS_Arg(args, "count", "8").ToInt();
                int burstRow2 = OZS_Arg(args, "row2", "-1").ToInt();
                int burstCol2 = OZS_Arg(args, "col2", "-1").ToInt();
                if (burstCount < 1 || burstCount > 200)
                {
                    detail = "count must be between 1 and 200";
                    return false;
                }
                if (pxRow < 0 || pxCol < 0 || burstRow2 < 0 || burstCol2 < 0)
                {
                    detail = "burst needs two free cells: row/col and row2/col2";
                    return false;
                }
                int burstUseRow;
                int burstUseCol;
                for (int burstStep = 0; burstStep < burstCount; burstStep++)
                {
                    burstUseRow = pxRow;
                    burstUseCol = pxCol;
                    if (burstStep % 2 == 1)
                    {
                        burstUseRow = burstRow2;
                        burstUseCol = burstCol2;
                    }
                    pxS.Operate(pxWho, OZS_Const.OP_MOVE, pxHandle, 0, 0, 0, InventoryLocationType.CARGO, -1, burstUseRow, burstUseCol, 0, pxS.m_Version);
                }
                detail = "burst of " + burstCount.ToString() + " sent -> " + pxS.Status();
                return true;
            }
            if (pxDo == "split")
            {
                // STAND ONLY: the split without a drag.
                //   do=split id=<box> handle=N [kind=0|1] [row=R col=C]
                // kind 0 is vanilla's half split, 1 its stack-max one.
                int splitKind = OZS_Arg(args, "kind", "0").ToInt();
                pxS.Operate(pxWho, OZS_Const.OP_SPLIT, pxHandle, 0, splitKind, 0, pxLt, pxSlotId, pxRow, pxCol, 0, pxS.m_Version);
                detail = "split sent -> " + pxS.Status();
                return true;
            }
            if (pxDo == "asother")
            {
                // STAND ONLY: an operation attributed to SOMEBODY ELSE.
                //
                // The rules about two people in one box -- whose operation is
                // refused as stale, whose change raises the other's mark --
                // all turn on the uid an operation is attributed to, and
                // nothing else. So a watcher with a made-up uid, which no
                // player is behind, is enough to measure them: the real
                // client's mark rises exactly as it would for a second
                // machine, and the made-up watcher is sent nothing because
                // `Listening` finds nobody there.
                //
                // It is NOT in the session's watcher list on purpose: a
                // watcher nobody is behind would hold the box open forever and
                // be counted among the people looking into it.
                //
                //   do=asother id=<box> what=move|swap|split handle=N
                //              [other=M] [row=R col=C] [who=<uid>]
                string asWhat = OZS_Arg(args, "what", "move");
                OZS_Watcher ghost = new OZS_Watcher(pxS, null);
                ghost.m_Uid = OZS_Arg(args, "who", "stand-second-player");
                int asOther = OZS_Arg(args, "other", "0").ToInt();
                if (asWhat == "move")
                    OZS_Ops.Run(pxS, ghost, OZS_Const.OP_MOVE, pxHandle, asOther, 0, 0, pxLt, pxSlotId, pxRow, pxCol, 0);
                else if (asWhat == "swap")
                    OZS_Ops.Run(pxS, ghost, OZS_Const.OP_SWAP, pxHandle, asOther, 0, 0, pxLt, pxSlotId, pxRow, pxCol, 0);
                else if (asWhat == "split")
                    OZS_Ops.Run(pxS, ghost, OZS_Const.OP_SPLIT, pxHandle, 0, OZS_Arg(args, "kind", "0").ToInt(), 0, pxLt, pxSlotId, pxRow, pxCol, 0);
                else
                {
                    detail = "asother: what must be move, swap or split";
                    return false;
                }
                detail = asWhat + " done as " + ghost.m_Uid + " -> " + pxS.Status();
                return true;
            }
            if (pxDo == "across")
            {
                // STAND ONLY: the cross-boundary swap, with the outside half
                // taken from the player rather than from a drag. `from=hands`
                // is the case that cannot be reasoned about from the code
                // alone -- `Asked` refuses a hands destination while the hands
                // are full, and whether they are empty by the time step three
                // asks depends on the order of the steps (2026-09-25).
                string acrossFrom = OZS_Arg(args, "from", "hands");
                PlayerBase acrossMe = PlayerBase.Cast(pxMen.Get(0));
                if (!acrossMe)
                {
                    detail = "nobody is connected";
                    return false;
                }
                EntityAI acrossItem = null;
                if (acrossFrom == "hands")
                    acrossItem = acrossMe.GetHumanInventory().GetEntityInHands();
                else
                {
                    array<EntityAI> acrossAll = new array<EntityAI>();
                    acrossMe.GetInventory().EnumerateInventory(InventoryTraversalType.PREORDER, acrossAll);
                    for (int ai = 0; ai < acrossAll.Count(); ai++)
                    {
                        EntityAI acrossCand = acrossAll.Get(ai);
                        if (acrossCand && acrossCand.GetType() == acrossFrom)
                        {
                            acrossItem = acrossCand;
                            break;
                        }
                    }
                }
                if (!acrossItem)
                {
                    detail = "the player has no " + acrossFrom;
                    return false;
                }
                int acrossLow;
                int acrossHigh;
                acrossItem.GetNetworkID(acrossLow, acrossHigh);
                pxS.Operate(pxWho, OZS_Const.OP_XSWAP, pxHandle, 0, acrossLow, acrossHigh, pxLt, pxSlotId, pxRow, pxCol, 0, pxS.m_Version);
                detail = "across sent with " + acrossItem.GetType() + " -> " + pxS.Status();
                return true;
            }
            if (pxDo == "drift")
            {
                // STAND ONLY: BREAKS THE BOOKKEEPING ON PURPOSE, so the repair
                // that exists for it can be watched doing its job instead of
                // waited for. `OZS_RootOrder` is the list a relative letter
                // names roots by position in; knock an entry out of it and
                // every later letter is written in a numbering the bridge does
                // not share, which is exactly how the record and the box
                // stopped agreeing on 2026-09-25.
                //
                // Nothing here touches the container: the ITEMS are fine and
                // the box is fine. Only this side's idea of their order is
                // wrong, which is the fault the absolute rewrite answers.
                array<EntityAI> driftOrder = pxS.m_Auth.OZS_RootOrder();
                int driftAt = OZS_Arg(args, "at", "0").ToInt();
                if (driftAt < 0 || driftAt >= driftOrder.Count())
                {
                    detail = "the order has " + driftOrder.Count().ToString() + " root(s); there is no position " + driftAt.ToString();
                    return false;
                }
                string driftWhat = "nothing";
                if (driftOrder.Get(driftAt))
                    driftWhat = driftOrder.Get(driftAt).GetType();
                driftOrder.RemoveOrdered(driftAt);
                detail = "the order lost position " + driftAt.ToString() + " (" + driftWhat + "): it now has " + driftOrder.Count().ToString() + " root(s) while the box holds " + pxS.m_Auth.OZS_CountEntities().ToString() + " -- the next turn should be caught and put straight";
                return true;
            }
            if (pxDo == "vanish")
            {
                // STAND ONLY, and the other half of `drift`: this one takes an
                // item out of the AUTHORITY without writing a letter about it,
                // so the box and the record disagree about TOTALS while every
                // name the letters use still matches. That is the fault the
                // count check exists for, and the only way to reach it now
                // that the identity check catches a bad position first.
                EntityAI vanishIt = OZS_Authority.ByHandle(pxS.m_Auth, pxHandle);
                if (!vanishIt)
                {
                    detail = "no item with handle " + pxHandle.ToString();
                    return false;
                }
                string vanishWhat = vanishIt.GetType();
                // The watchdog is told, or it reports this as an item that
                // left the box on its own -- which is exactly what it is, and
                // exactly what we are doing on purpose.
                OZS_Watchdog.Expect(vanishIt);
                GetGame().ObjectDelete(vanishIt);
                detail = vanishWhat + " was taken out of the authority with nothing written about it; the next turn should find the counts disagreeing";
                return true;
            }
            if (pxDo == "sort")
            {
                pxS.Operate(pxWho, OZS_Const.OP_SORT, 0, 0, 0, 0, InventoryLocationType.CARGO, InventorySlots.INVALID, -1, -1, 0, pxS.m_Version);
                detail = "sort sent -> " + pxS.Status();
                return true;
            }
            if (pxDo == "combine")
            {
                pxS.Operate(pxWho, OZS_Const.OP_COMBINE, pxHandle, OZS_Arg(args, "other", "2").ToInt(), 0, 0, pxLt, pxSlotId, pxRow, pxCol, 0, pxS.m_Version);
                detail = "combine sent -> " + pxS.Status();
                return true;
            }
            if (pxDo == "in")
            {
                // The item is made in the player's hands first, exactly as one
                // they picked up would be, and then named by its network id.
                string pxItem = OZS_Arg(args, "item", "BandageDressing");
                EntityAI pxMade = EntityAI.Cast(pxMen.Get(0).GetHumanInventory().CreateInHands(pxItem));
                if (!pxMade)
                    pxMade = EntityAI.Cast(pxMen.Get(0).GetInventory().CreateInInventory(pxItem));
                if (!pxMade)
                {
                    detail = "the player cannot hold a " + pxItem;
                    return false;
                }
                int pxLow;
                int pxHigh;
                pxMade.GetNetworkID(pxLow, pxHigh);
                pxS.Operate(pxWho, OZS_Const.OP_IN, 0, pxInto, pxLow, pxHigh, pxLt, pxSlotId, pxRow, pxCol, 0, pxS.m_Version);
                detail = "in sent for " + pxMade.GetType() + " netid " + pxMade.GetNetworkIDString() + " -> " + pxS.Status();
                return true;
            }
            detail = "proxy: unknown do=" + pxDo;
            return false;
        }

        if (op == "auth")
        {
            // STAND ONLY (proxy design 2026-09-24, stage A): the authoritative
            // box -- a real container nobody is told about, standing for a box
            // whose contents live in SQL.
            //
            //   do=make   id=<box id> [class=<cls>] [pos="x y z"]
            //   do=open   id=<box id>       fill it from SQL by the ordinary open
            //   do=index  id=<box id>       a handle for every entity in it
            //   do=peek   id=<box id> [handle=N]   with every gate a split asks
            //   do=tree   id=<box id>       the whole handle table with quantities
            //   do=discard id=<box id>      delete it, write nothing
            //   do=status                   every live authority
            string what = OZS_Arg(args, "do", "status");
            string aid = OZS_Arg(args, "id", "");
            if (what == "status")
            {
                detail = OZS_Authority.Status();
                return true;
            }
            if (aid == "")
            {
                detail = "auth do=" + what + " needs id=<box id>";
                return false;
            }
            if (what == "make")
            {
                string acls = OZS_Arg(args, "class", "");
                vector aat;
                bool haveAt = false;
                string aposText = OZS_Arg(args, "pos", "");
                if (aposText != "")
                {
                    aat = aposText.ToVector();
                    haveAt = true;
                }
                // The class and the place of the real box, when it is here:
                // the grid has to match what is stored, and a ground-built
                // container needs a real surface under it.
                OZ_StorageBox real = c.FindById(aid);
                if (real)
                {
                    if (acls == "")
                        acls = real.GetType();
                    if (!haveAt)
                    {
                        aat = real.GetPosition();
                        haveAt = true;
                    }
                }
                if (acls == "")
                {
                    detail = "no box with id " + aid + " is here, so auth do=make needs class=<cls>";
                    return false;
                }
                if (!haveAt && !OZS_PlayerPos(aat))
                {
                    detail = "auth do=make needs pos=\"x y z\" when neither the box nor a player is here";
                    return false;
                }
                OZ_StorageBox born = OZS_Authority.Create(aid, acls, aat);
                if (!born)
                {
                    detail = "the authority for " + aid + " could not be created as " + acls;
                    return false;
                }
                detail = "authority " + born.GetType() + " for " + born.OZS_GetId();
                detail = detail + " netid=" + born.GetNetworkIDString();
                detail = detail + " authority=" + born.OZS_IsAuthority();
                detail = detail + " at " + born.GetPosition().ToString(false);
                detail = detail + "; boxes registered " + c.BoxCount();
                return true;
            }
            OZ_StorageBox auth = OZS_Authority.Find(aid);
            if (!auth)
            {
                detail = "no authority stands for " + aid;
                return false;
            }
            if (what == "open")
            {
                string whyAuth;
                if (!c.RequestOpenAs(auth, "stand", "authority", whyAuth))
                {
                    detail = "open refused: " + whyAuth;
                    return false;
                }
                detail = "open accepted for the authority of " + aid + ", state now " + OZS_Const.StateName(auth.OZS_GetState());
                return true;
            }
            if (what == "index")
            {
                int handed = OZS_Authority.Index(auth);
                detail = "handles for " + handed.ToString() + " entity(ies) in the authority of " + aid;
                array<EntityAI> shown = new array<EntityAI>();
                auth.OZS_GetRoots(shown);
                int upto = shown.Count();
                if (upto > 6)
                    upto = 6;
                for (int si = 0; si < upto; si++)
                {
                    EntityAI se = shown.Get(si);
                    detail = detail + " | #" + OZS_Authority.Handle(auth, se) + " " + se.GetType() + " netid " + se.GetNetworkIDString();
                }
                if (shown.Count() > upto)
                    detail = detail + " | +" + (shown.Count() - upto).ToString() + " more roots";
                return true;
            }
            if (what == "peek")
            {
                int handle = OZS_Arg(args, "handle", "1").ToInt();
                EntityAI got = OZS_Authority.ByHandle(auth, handle);
                if (!got)
                {
                    detail = "the authority of " + aid + " has no handle " + handle.ToString();
                    return false;
                }
                detail = "#" + handle.ToString() + " " + got.GetType() + " netid " + got.GetNetworkIDString();
                detail = detail + " tree " + OZS_Records.CountTree(got).ToString();
                // EVERY GATE A SPLIT ASKS, named one by one (2026-09-26: a
                // stack inside a container hung in a stash's slot was refused
                // #STR_OZS_NO_SPLIT and nothing said by whom).
                ItemBase pib = ItemBase.Cast(got);
                if (pib)
                {
                    detail = detail + " qty " + pib.GetQuantity().ToString() + " splitable " + pib.IsSplitable().ToString();
                    detail = detail + " CanBeSplit " + pib.CanBeSplit().ToString();
                    detail = detail + " CanRemoveEntity " + pib.GetInventory().CanRemoveEntity().ToString();
                    InventoryLocation pil = new InventoryLocation();
                    if (pib.GetInventory().GetCurrentInventoryLocation(pil))
                    {
                        detail = detail + " lt " + pil.GetType().ToString() + " at " + pil.GetRow().ToString() + "," + pil.GetCol().ToString();
                        detail = detail + " LocationCanRemoveEntity " + GameInventory.LocationCanRemoveEntity(pil).ToString();
                        EntityAI pparent = pil.GetParent();
                        if (pparent)
                        {
                            detail = detail + " parent " + pparent.GetType() + " #" + OZS_Authority.Handle(auth, pparent).ToString();
                            detail = detail + " parentReleasesCargo " + pparent.CanReleaseCargo(got).ToString();
                            detail = detail + " parentCanRemoveInCargo " + pparent.GetInventory().CanRemoveEntityInCargo(got).ToString();
                            detail = detail + " parentRuined " + pparent.IsRuined().ToString();
                            detail = detail + " parentChildrenAccessible " + pparent.AreChildrenAccessible().ToString();
                        }
                    }
                }
                return true;
            }
            if (what == "tree")
            {
                // The whole handle table of the authority, as the server holds
                // it: what the client's own table is diffed against.
                array<EntityAI> tnodes = new array<EntityAI>();
                array<int> tparents = new array<int>();
                OZS_Records.Flatten(auth, -1, tnodes, tparents);
                detail = "authority of " + aid + ": " + (tnodes.Count() - 1).ToString() + " entity(ies)";
                for (int ti = 1; ti < tnodes.Count(); ti++)
                {
                    EntityAI tn = tnodes.Get(ti);
                    if (!tn)
                        continue;
                    int tph = 0;
                    if (tparents.Get(ti) > 0)
                        tph = OZS_Authority.Handle(auth, tnodes.Get(tparents.Get(ti)));
                    string trow = " | #" + OZS_Authority.Handle(auth, tn).ToString() + " " + tn.GetType() + " in #" + tph.ToString();
                    ItemBase tib = ItemBase.Cast(tn);
                    if (tib && tib.HasQuantity())
                        trow = trow + " qty " + tib.GetQuantity().ToString();
                    InventoryLocation til = new InventoryLocation();
                    if (tn.GetInventory().GetCurrentInventoryLocation(til))
                        trow = trow + " lt " + til.GetType().ToString() + " " + til.GetRow().ToString() + "," + til.GetCol().ToString();
                    detail = detail + trow;
                }
                return true;
            }
            if (what == "discard")
            {
                int lost = OZS_Authority.Discard(aid);
                detail = "the authority of " + aid + " is gone with " + lost.ToString() + " entity(ies); nothing was written";
                return true;
            }
            detail = "auth: unknown do=" + what;
            return false;
        }

        OZ_StorageBox target = OZS_Pick(args, detail);
        if (!target)
            return false;

        if (op == "status")
        {
            detail = target.GetType() + " id=" + target.OZS_GetId() + " state=" + OZS_Const.StateName(target.OZS_GetState());
            detail = detail + " entities=" + target.OZS_CountEntities() + " stored=" + target.OZS_GetStoredCount();
            detail = detail + " slots=[" + OZS_Slots(target) + "]";
            return true;
        }
        if (op == "lower")
        {
            // What the engine's ToLower does to non-ASCII text (the search).
            string t = OZS_Arg(args, "text", "");
            t.ToLower();
            detail = "[" + t + "] find=" + t.IndexOf(OZS_Arg(args, "find", "x"));
            return true;
        }
        if (op == "files")
        {
            // The box as the engine sees it, the bridge's reachability, and
            // what sits in the exchange directory: the cache of this box and
            // any turn file of it still waiting for the bridge.
            string bid = target.OZS_GetId();
            detail = "box " + bid + " " + OZS_Const.StateName(target.OZS_GetState()) + " stored=" + target.OZS_GetStoredCount() + " entities=" + target.OZS_CountEntities();
            detail = detail + " bridge=" + OZS_Bridge.Up() + " boot_done=" + c.BootDone();
            detail = detail + " cache=" + FileExist(OZS_Store.XchgPath(bid + ".bin"));
            string name;
            FileAttr attr;
            int waiting = 0;
            int others = 0;
            FindFileHandle h = FindFile(OZS_Const.DIR_XCHG + "\\*", name, attr, FindFileFlags.ALL);
            if (h)
            {
                bool more = true;
                while (more)
                {
                    if (name != "" && name != "." && name != ".." && name != bid + ".bin")
                    {
                        if (name.IndexOf(bid + "-") == 0)
                            waiting++;
                        else
                            others++;
                    }
                    more = FindNextFile(h, name, attr);
                }
                CloseFindFile(h);
            }
            detail = detail + " turn_files_waiting=" + waiting + " other_files=" + others;
            return true;
        }

        detail = "unknown op '" + op + "'; known: list, spawn, status, files, tune, lower, persist, chain, nest, probe, auth, proxy";
        return false;
    }

    // The box the caller means: by id, else the nearest to pos, else the
    // nearest to the connected player.
    protected OZ_StorageBox OZS_Pick(map<string, string> args, out string detail)
    {
        OZS_Controller c = OZS_Controller.Get();
        string id = OZS_Arg(args, "id", "");
        if (id != "")
        {
            OZ_StorageBox byId = c.FindById(id);
            if (!byId)
                detail = "no box with id " + id;
            return byId;
        }
        vector pos;
        string posText = OZS_Arg(args, "pos", "");
        if (posText != "")
        {
            pos = posText.ToVector();
        }
        else if (!OZS_PlayerPos(pos))
        {
            detail = "name the box with id=, or pos=, or connect a player";
            return null;
        }
        OZ_StorageBox near = c.Nearest(pos, 100);
        if (!near)
            detail = "no box within 100 m of " + pos.ToString();
        return near;
    }

    // What hangs in the weapon slots: "AKM[Mag_AKM_30Rnd:17,+1]" per slot.
    protected string OZS_Slots(OZ_StorageBox box)
    {
        string s = "";
        GameInventory inv = box.GetInventory();
        if (!inv)
            return s;
        for (int a = 0; a < inv.AttachmentCount(); a++)
        {
            EntityAI att = inv.GetAttachmentFromIndex(a);
            if (!att)
                continue;
            if (s != "")
                s = s + " ";
            s = s + att.GetType();
            Weapon_Base w = Weapon_Base.Cast(att);
            if (!w)
                continue;
            Magazine mag = w.GetMagazine(0);
            string inside = "";
            if (mag)
                inside = mag.GetType() + ":" + mag.GetAmmoCount();
            if (!w.IsChamberEmpty(0))
                inside = inside + ",+1";
            s = s + "[" + inside + "]";
        }
        return s;
    }

    protected bool OZS_PlayerPos(out vector pos)
    {
        array<Man> players = new array<Man>();
        GetGame().GetPlayers(players);
        if (players.Count() == 0)
            return false;
        pos = players.Get(0).GetPosition();
        return true;
    }
}
