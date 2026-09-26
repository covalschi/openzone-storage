// The server half of the proxy design: who is looking into which box, what
// their proxies have been told, and the streaming of a box's contents to one
// player at a time.
// Design: docs/specs/2026-09-24-storage-proxy-inventory-design.md §2, §5, §8.
//
// A SESSION is one box being looked at. It owns the authoritative container
// (OZS_Authority) and a list of watchers. There is no lock: several players
// may be in one box at once (owner's decision), so the server is the only
// arbiter and it needs the four things §8 asks for -- stable handles, an
// optimistic version, one operation at a time, and a change sent to every
// proxy of that box.
//
// A WATCHER is one player's proxy. It is streamed the box once, in chunks
// paced over frames, and then kept up to date by changes. Changes that happen
// WHILE it is still streaming are queued and sent after the stream's end: the
// snapshot was taken before them, so applying them earlier would be applying
// them twice.
class OZS_Proxies
{
    protected static ref OZS_Proxies s_Inst;
    protected ref array<ref OZS_Session> m_Sessions;

    static OZS_Proxies Get()
    {
        if (!s_Inst)
            s_Inst = new OZS_Proxies();
        return s_Inst;
    }

    static void Reset()
    {
        s_Inst = null;
    }

    void OZS_Proxies()
    {
        m_Sessions = new array<ref OZS_Session>();
    }

    array<ref OZS_Session> Sessions()
    {
        return m_Sessions;
    }

    OZS_Session Find(string id)
    {
        for (int i = 0; i < m_Sessions.Count(); i++)
        {
            if (m_Sessions.Get(i).m_Id == id)
                return m_Sessions.Get(i);
        }
        return null;
    }

    // ---- a player asks to see a box --------------------------------------

    // `spot` is only a valid position for the authoritative container to stand
    // at -- the anchor's, so a ground-built container of the restore has real
    // ground under it. Nobody ever looks at it.
    bool Open(string id, string cls, vector spot, PlayerIdentity who, out string why)
    {
        if (id == "" || cls == "" || !who)
        {
            why = "an open needs an id, a class and a player";
            return false;
        }
        // THE TWO SCHEMES MUST NOT MEET ON ONE BOX. If the placed box is
        // materialised in the world by the old open, its items are already
        // out; filling an authority from SQL as well would put the same loot
        // in the world twice.
        OZ_StorageBox placed = OZS_Controller.Get().FindById(id);
        if (placed && placed.OZS_GetState() != OZS_Const.STATE_CLOSED)
        {
            why = "the box is open the old way (" + OZS_Const.StateName(placed.OZS_GetState()) + "); close it first";
            return false;
        }
        // WITHIN REACH OF THE ANCHOR, ASKED HERE AND NOT TRUSTED FROM THE
        // CLIENT. The message names an anchor by network id, and a box is an
        // announced entity whose id any client that ever had it in its bubble
        // still knows: without this a crafted message opened any box on the
        // map from anywhere, and `Out` put the contents in the sender's own
        // pockets (review 2026-09-26, A1). The client's reach test lives in
        // the action's condition, which a crafted message never runs. The
        // same leash is held for the whole session in OZS_Session.OnFrame.
        PlayerBase asker = OZS_Controller.FindPlayerByUid(OZS_Controller.UidOfIdentity(who));
        if (!asker || !OZS_Session.Near(asker, spot))
        {
            why = "too far from the box";
            return false;
        }
        OZS_Session s = Find(id);
        if (!s)
        {
            s = new OZS_Session(id, cls, spot);
            m_Sessions.Insert(s);
        }
        return s.Join(who, why);
    }

    void Shut(string id, PlayerIdentity who)
    {
        OZS_Session s = Find(id);
        if (s)
            s.Leave(who, "closed the screen");
    }

    // Every session this player is in: a disconnect, a death, a teleport.
    void DropPlayer(PlayerIdentity who, string cause)
    {
        if (!who)
            return;
        for (int i = m_Sessions.Count() - 1; i >= 0; i--)
            m_Sessions.Get(i).Leave(who, cause);
    }

    void OnFrame(float timeslice)
    {
        // The authorities being let go, a few deletions a frame -- before the
        // sessions, so a resort waiting for its box to empty sees it empty in
        // the same frame the last entity went.
        OZS_Authority.OnFrame();
        for (int i = m_Sessions.Count() - 1; i >= 0; i--)
        {
            OZS_Session s = m_Sessions.Get(i);
            s.OnFrame(timeslice);
            if (s.IsDone())
                s.End();
            // Listed until it HAS ended: the closing write is a job of
            // several frames (OZS_WholeJob), and End returns while it runs.
            if (s.m_Ended)
                m_Sessions.RemoveOrdered(i);
        }
    }

    // The mission is coming down: let every authority go without writing, the
    // way a crash would. Whatever reached SQL is what there is (§9).
    void EndAll()
    {
        for (int i = m_Sessions.Count() - 1; i >= 0; i--)
        {
            OZS_Session s = m_Sessions.Get(i);
            s.End();
            // No frames are coming: a closing write still running finishes
            // here, whole, the way it did before it was paced. The mission
            // is stopping, and one long frame costs nobody anything now.
            s.FinishWholeNow();
        }
        m_Sessions.Clear();
        // And the authorities they let go: deleted a few a frame while the
        // mission runs, all at once now that it stops.
        OZS_Authority.FinishAllNow();
    }

    string Status()
    {
        string s = "sessions=" + m_Sessions.Count();
        for (int i = 0; i < m_Sessions.Count(); i++)
            s = s + " | " + m_Sessions.Get(i).Status();
        return s;
    }

    // A REFUSAL TO SOMEBODY WHO HAS NO WATCHER TO BE REFUSED THROUGH. An open
    // that is turned away -- the bridge down, the box in a transition, the
    // asker too far -- used to be a line in the server log and nothing on
    // the client, whose screen went on waiting for a box that was never
    // coming (review 2026-09-26, C3). The message carries the box id, so the
    // client matches it the way it matches every other refusal.
    static void Refuse(PlayerIdentity to, string id, string why)
    {
        PlayerBase p = OZS_Controller.FindPlayerByUid(OZS_Controller.UidOfIdentity(to));
        if (!p || !p.GetIdentity())
            return;
        ScriptRPC no = new ScriptRPC();
        no.Write(id);
        no.Write(0);
        no.Write(0);
        no.Write(why);
        no.Send(p, OZS_Const.RPC_PX_NO, true, p.GetIdentity());
    }

    // ---- the anchor's RPCs -----------------------------------------------

    // Called from the player's OnRPC: a client's message rides on its own
    // player entity, and that direction keeps its target. Returns true when
    // the message was ours.
    static bool OnWire(PlayerIdentity sender, int type, ParamsReadContext ctx)
    {
        if (!GetGame() || !GetGame().IsServer())
            return false;
        if (type == OZS_Const.RPC_PX_OPEN)
        {
            int low;
            int high;
            if (!ctx.Read(low) || !ctx.Read(high))
                return true;
            // THE CLIENT NAMES THE ANCHOR, THE SERVER NAMES THE BOX. A box id
            // is the engine's persistent id or a stash's pair; the client has
            // never been told either, and should not be able to name one it
            // cannot see. Everything about the session -- the id, the class,
            // the place -- is decided here.
            string uid = OZS_Controller.UidOfIdentity(sender);
            string why;
            // A STASH ANCHOR NAMES A DIFFERENT BOX FOR EVERY PLAYER, and the
            // pairing is made HERE rather than sent. A locker is one object
            // that several people may stand at; each of them has their own
            // record under it, keyed by the anchor and their own uid. The
            // client sends the anchor it is looking at and nothing else, so
            // there is no message in which somebody else's stash could be
            // named (design 2026-09-24 section 11).
            //
            // Nothing is created in the world for it. The authority is
            // unannounced like any other, which is where the privacy comes
            // from now -- it used to come from standing a real container under
            // the player's feet, and that is what this replaces.
            OZ_StashAnchor rack = OZ_StashAnchor.Cast(GetGame().GetObjectByNetworkId(low, high));
            if (rack)
            {
                string mine = OZS_Const.StashId(rack.OZS_AnchorKey(), uid);
                if (!OZS_Proxies.Get().Open(mine, "OZ_PersonalStash", rack.GetPosition(), sender, why))
                {
                    OZ_Log.Warn("storage: proxy: " + uid + " cannot open the stash " + mine + ": " + why);
                    Refuse(sender, mine, why);
                }
                return true;
            }
            OZ_StorageBox real = OZ_StorageBox.Cast(GetGame().GetObjectByNetworkId(low, high));
            if (!real)
            {
                OZ_Log.Warn("storage: proxy: " + uid + " asked for an anchor that is neither a box nor a stash rack");
                return true;
            }
            // A STASH IS NEVER AN ANCHOR. The class exists only as a session's
            // authority and as its proxy, and neither has a network id a
            // client could name; whatever answered to this one is not a place
            // a player may ask for, and its id would be somebody's pair
            // (review 2026-09-26, A2).
            if (OZ_PersonalStash.Cast(real))
            {
                OZ_Log.Warn("storage: proxy: " + uid + " asked for a stash entity as an anchor (" + real.OZS_GetId() + ")");
                return true;
            }
            if (!OZS_Proxies.Get().Open(real.OZS_GetId(), real.GetType(), real.GetPosition(), sender, why))
            {
                OZ_Log.Warn("storage: proxy: " + uid + " cannot open " + real.OZS_GetId() + ": " + why);
                Refuse(sender, real.OZS_GetId(), why);
            }
            return true;
        }
        if (type == OZS_Const.RPC_PX_SHUT)
        {
            string shutId;
            if (!ctx.Read(shutId))
                return true;
            OZS_Proxies.Get().Shut(shutId, sender);
            return true;
        }
        if (type == OZS_Const.RPC_PX_OP)
        {
            string opId;
            int op;
            int handle;
            int other;
            int netLow;
            int netHigh;
            int lt;
            int slot;
            int row;
            int col;
            int flip;
            int version;
            if (!OZS_Wire.ReadOp(ctx, opId, op, handle, other, netLow, netHigh, lt, slot, row, col, flip, version))
                return true;
            OZS_Session s = OZS_Proxies.Get().Find(opId);
            if (!s)
                return true;
            s.Operate(sender, op, handle, other, netLow, netHigh, lt, slot, row, col, flip, version);
            return true;
        }
        return false;
    }
}

// ---------------------------------------------------------------------------

// One operation waiting for its turn on the wire.
//
// A QUEUED OPERATION HAS NOT HAPPENED. Nothing has moved, nothing is owed to
// SQL, and a server that dies with a full queue loses exactly the moves that
// never reached the record -- which is what section 9 says the price of a
// crash is. That is what makes this different from buffering WRITES, which
// section 9 forbids: a buffered write is a move that already happened and is
// waiting to be recorded, and it is precisely the window a commit per
// operation exists to close.
class OZS_Waiting
{
    string m_Uid;
    int m_Op;
    int m_Handle;
    int m_Other;
    int m_NetLow;
    int m_NetHigh;
    int m_Lt;
    int m_Slot;
    int m_Row;
    int m_Col;
    int m_Flip;
    // Only for an operation the stand's fake ping holds back: the version
    // the client acted on, and when the operation is due to run.
    int m_Version;
    float m_Due;

    void OZS_Waiting(string uid, int op, int handle, int other, int netLow, int netHigh, int lt, int slot, int row, int col, int flip)
    {
        m_Uid = uid;
        m_Op = op;
        m_Handle = handle;
        m_Other = other;
        m_NetLow = netLow;
        m_NetHigh = netHigh;
        m_Lt = lt;
        m_Slot = slot;
        m_Row = row;
        m_Col = col;
        m_Flip = flip;
    }
}

class OZS_Session
{
    string m_Id;
    string m_Class;
    vector m_Spot;
    OZ_StorageBox m_Auth;
    ref array<ref OZS_Watcher> m_Watchers;
    // Bumped by every accepted operation. A client names the version it acted
    // on; one that has fallen behind is refused and resynchronised (§8.2).
    int m_Version;
    // Seconds with nobody looking. An authority costs memory, not secrecy, so
    // the wait can be short (§11).
    float m_Empty;
    bool m_Ended;
    // Turns posted to the bridge that have not answered yet, and what the
    // bridge last said about the record. A session with turns in flight is not
    // finished, however quiet it has gone.
    int m_Flying;
    int m_SqlVersion;
    int m_SqlRoots;
    int m_SqlEntities;
    // Operations that arrived while a turn was on the wire (§8.3). See
    // OZS_Waiting for why holding these is not the buffering §9 forbids.
    ref array<ref OZS_Waiting> m_Waiting;
    // Operations the stand's fake ping is holding back (FakePingMs); each
    // knows when it is due. Empty on a real server.
    ref array<ref OZS_Waiting> m_Delayed;
    // The closing write: `m_Closing` means End is waiting for the wire, and
    // `m_Wrote` that the record has already been told what the box holds.
    bool m_Closing;
    bool m_Wrote;
    // How many times this session has put its record straight. A repair that
    // does not end the disagreement will not end it on the tenth try either,
    // and a box that repairs itself once per turn looks to the player like
    // nothing they do takes effect (owner, 2026-09-25).
    int m_Repairs;
    // THE BOX LOST SOMETHING TO SOMEBODY WHO IS NOT THIS MOD. Raised by the
    // watchdog when an item leaves the authority without an operation -- a
    // ruined container dropping its inventory was the case that wrote it
    // (owner's charge, 2026-09-26). From then on the box is not the truth
    // about the record: nothing is written on the way out, no drift repair
    // sets the record to what the box holds, and the session ends on the
    // next frame with the record at its last committed turn.
    bool m_Compromised;

    void Compromised(string what)
    {
        if (m_Compromised || m_Ended)
            return;
        m_Compromised = true;
        // Nothing to write: the closing write states what the container
        // holds, and that stopped being the truth just now.
        m_Wrote = true;
        OZ_Log.Error("storage: proxy: box " + m_Id + " is compromised: " + what + "; the record stays at its last turn and the session ends");
    }

    void OZS_Session(string id, string cls, vector spot)
    {
        m_Id = id;
        m_Class = cls;
        m_Spot = spot;
        m_Watchers = new array<ref OZS_Watcher>();
        m_Waiting = new array<ref OZS_Waiting>();
        m_Delayed = new array<ref OZS_Waiting>();
        m_Compromised = false;
        m_Version = 0;
        m_Empty = 0;
        m_Ended = false;
    }

    // A WATCHER IS KEYED BY ITS UID, NOT BY ITS PlayerIdentity. The engine
    // hands OnRPC an identity that is not the same object as the player's own
    // GetIdentity(): the first version of this compared the two and never
    // found the player, so every message was sent to a null target -- which
    // silently becomes a global RPC the client ignores. Measured 2026-09-24.
    OZS_Watcher WatcherOf(PlayerIdentity who)
    {
        return WatcherOfUid(OZS_Controller.UidOfIdentity(who));
    }

    OZS_Watcher WatcherOfUid(string uid)
    {
        if (uid == "")
            return null;
        for (int i = 0; i < m_Watchers.Count(); i++)
        {
            if (m_Watchers.Get(i).m_Uid == uid)
                return m_Watchers.Get(i);
        }
        return null;
    }

    // ---- joining and leaving ---------------------------------------------

    bool Join(PlayerIdentity who, out string why)
    {
        // A SESSION ON ITS WAY OUT TAKES NOBODY: its closing write is running
        // (OZS_WholeJob) and the box will be gone in a moment. The player is
        // told to try again; a moment later a fresh session answers.
        if (m_Ended || m_Wrote)
        {
            why = "#STR_OZS_OPENING";
            return false;
        }
        OZS_Watcher w = WatcherOf(who);
        if (w)
        {
            // Asking twice is not an error: a reopened screen wants the box
            // again. The old proxy is replaced by a fresh stream.
            w.Restart();
        }
        else
        {
            if (!m_Auth)
            {
                m_Auth = OZS_Authority.Create(m_Id, m_Class, AuthoritySpot());
                if (!m_Auth)
                {
                    why = "the authority could not be created";
                    return false;
                }
            }
            w = new OZS_Watcher(this, who);
            m_Watchers.Insert(w);
        }
        m_Empty = 0;
        // The first watcher pays for the fill; the rest arrive to a box that
        // is already there.
        //
        // FOR A WATCHER ASKING AGAIN AS MUCH AS FOR A NEW ONE. A fill that
        // failed leaves the authority CLOSED and empty, and this used to
        // return above for anybody who already had a watcher -- so the same
        // player pressing the button again got a restream of nothing, for as
        // long as they stayed on the server (review 2026-09-26, C2).
        if (m_Auth.OZS_GetState() == OZS_Const.STATE_CLOSED)
        {
            string openWhy;
            if (!OZS_Controller.Get().RequestOpenAs(m_Auth, "proxy", OZS_Controller.UidOfIdentity(who), openWhy))
            {
                Leave(who, "the box cannot be filled: " + openWhy);
                // The reason as the controller gave it, which is a
                // stringtable key the client can show, not a sentence of
                // ours wrapped around it.
                why = openWhy;
                return false;
            }
        }
        OZ_Log.Info("storage: proxy: " + OZS_Controller.UidOfIdentity(who) + " is looking into " + m_Id + " (" + m_Watchers.Count().ToString() + " watcher(s))");
        return true;
    }

    void Leave(PlayerIdentity who, string cause)
    {
        string uid = OZS_Controller.UidOfIdentity(who);
        for (int i = m_Watchers.Count() - 1; i >= 0; i--)
        {
            if (m_Watchers.Get(i).m_Uid != uid)
                continue;
            m_Watchers.Get(i).Gone();
            m_Watchers.RemoveOrdered(i);
            OZ_Log.Info("storage: proxy: " + OZS_Controller.UidOfIdentity(who) + " left " + m_Id + " (" + cause + "), " + m_Watchers.Count().ToString() + " left");
        }
    }

    // IS THIS PLAYER CLOSE ENOUGH TO THE ANCHOR TO BE USING IT. Asked of the
    // server's own idea of where the player stands, against the anchor's
    // spot; see OZS_Const.SESSION_LEASH for the figure and the reason.
    static bool Near(Man player, vector spot)
    {
        if (!player)
            return false;
        return vector.Distance(player.GetPosition(), spot) <= OZS_Const.SESSION_LEASH;
    }

    // THE FILL DID NOT HAPPEN. The authority is CLOSED and empty, and every
    // watcher is waiting for a stream that will never begin: they are told
    // and sent away, the session idles out and lets the authority go, and
    // the next press of the button starts afresh (review 2026-09-26, C2).
    void OnFillFailed(string why)
    {
        OZ_Log.Warn("storage: proxy: box " + m_Id + " could not be filled (" + why + "); " + m_Watchers.Count().ToString() + " watcher(s) are sent away");
        for (int i = m_Watchers.Count() - 1; i >= 0; i--)
        {
            m_Watchers.Get(i).No(0, "#STR_OZS_OPEN_FAILED", m_Version);
            m_Watchers.Get(i).Gone();
            m_Watchers.RemoveOrdered(i);
        }
    }

    // EVERYBODY OUT, AND THE SESSION ENDS -- the admin's live `close`, and
    // the placed box leaving the world under a session that is still going.
    // The watchers are told the way a session end always tells them, and End
    // itself waits for the wire if a turn is still on it.
    void Close(string cause)
    {
        OZ_Log.Info("storage: proxy: box " + m_Id + " is being closed (" + cause + "), " + m_Watchers.Count().ToString() + " watcher(s) sent away");
        for (int i = 0; i < m_Watchers.Count(); i++)
            m_Watchers.Get(i).Gone();
        m_Watchers.Clear();
        End();
    }

    // A place for the authority nobody will ever stand in. It only has to be
    // a valid position: the anchor's own spot keeps a ground-built container
    // of the restore on real ground.
    vector AuthoritySpot()
    {
        return m_Spot;
    }

    // ---- the frame -------------------------------------------------------

    void OnFrame(float timeslice)
    {
        if (m_Ended)
            return;
        // A box that lost items outside the mod ends at once, writing
        // nothing; End waits for a turn still on the wire, and OnCommitted
        // brings it back here.
        if (m_Compromised)
        {
            End();
            return;
        }
        // The absolute letter being written, a budget's worth per frame.
        TickWhole(OZS_Settings.Get().OpenFrameBudgetMs * 0.001);
        if (m_Ended)
            return;
        // A sort whose box is being emptied, a few deletions a frame: the
        // refill starts the frame the last of them has gone.
        if (m_Emptying)
        {
            Resort();
            if (m_Ended)
                return;
        }
        // The stand's fake ping: operations whose delay has passed run now,
        // in the order they came.
        while (m_Delayed.Count() > 0 && m_Delayed.Get(0).m_Due <= GetGame().GetTickTime())
        {
            OZS_Waiting d = m_Delayed.Get(0);
            m_Delayed.RemoveOrdered(0);
            OperateAs(d.m_Uid, d.m_Op, d.m_Handle, d.m_Other, d.m_NetLow, d.m_NetHigh, d.m_Lt, d.m_Slot, d.m_Row, d.m_Col, d.m_Flip, d.m_Version, true);
            if (m_Ended)
                return;
        }
        for (int i = m_Watchers.Count() - 1; i >= 0; i--)
        {
            OZS_Watcher w = m_Watchers.Get(i);
            Man man = w.Player();
            if (!man)
            {
                OZ_Log.Info("storage: proxy: " + w.m_Uid + " is no longer on the server; " + m_Id + " lets them go");
                m_Watchers.RemoveOrdered(i);
                continue;
            }
            // THE LEASH, HELD EVERY FRAME. The open asked it once; a player
            // who walks off with the screen open, or is carried off in a
            // vehicle, is let go exactly as if they had closed it. The proxy
            // beside them has long dropped out of their panel by six metres,
            // so an honest player never meets this line (review 2026-09-26,
            // A1).
            if (!Near(man, m_Spot))
            {
                OZ_Log.Info("storage: proxy: " + w.m_Uid + " walked away from " + m_Id + "; they are let go");
                w.Gone();
                m_Watchers.RemoveOrdered(i);
                continue;
            }
            w.OnFrame(timeslice);
        }
        if (m_Watchers.Count() > 0)
        {
            m_Empty = 0;
            return;
        }
        m_Empty = m_Empty + timeslice;
    }

    bool IsDone()
    {
        if (m_Ended)
            return true;
        if (m_Watchers.Count() > 0)
            return false;
        if (m_Flying > 0 || m_Whole || m_Emptying)
            return false;
        return m_Empty >= OZS_Settings.Get().ProxyIdleSeconds;
    }

    // Nobody is looking any more. The authority is discarded -- not closed:
    // SQL was written operation by operation and is already current (§7).
    // A SESSION ENDS BY SAYING WHAT THE BOX HOLDS.
    //
    // Nothing used to be written here at all: the record was built solely by
    // the per-turn letters, and the authority was then deleted with everything
    // in it. So any drift that appeared during the session survived into SQL
    // and the next open materialised the wrong contents -- and the evidence,
    // the items themselves, was destroyed on the way out.
    //
    // `ok` false means the closing write could not even be sent (the bridge is
    // gone). The authority is released either way -- holding it would keep the
    // box open for a session nobody is in -- but the log says which of the two
    // happened, and on a failure it says what is being lost.
    void End()
    {
        if (m_Ended)
            return;
        // WAIT FOR THE WIRE, AND FOR A WRITE ALREADY RUNNING. A closing write
        // while a turn is still in flight is two writers on one record; the
        // queue exists so there is never more than one, and this is the last
        // place that has to respect it. A repair's job running now posts,
        // is answered, and the answer brings us back here (OnCommitted).
        // A box being emptied for a sort is no different: it is half gone,
        // and Resort brings us back here when the last entity has.
        if (m_Auth && (m_Flying > 0 || m_Whole || m_Emptying))
        {
            m_Closing = true;
            return;
        }
        // A SESSION WHOSE BOX NEVER OPENED HAS NOTHING TO SAY ABOUT IT.
        //
        // The closing write states what the container holds, and that is only
        // the truth when the container was filled. An open that FAILED leaves
        // the authority empty and the box CLOSED -- and writing then tells the
        // record the box is empty, over a record of a hundred roots.
        //
        // Measured 2026-09-25: a box whose roots could not be read (three
        // parked in a row, "too many unreadable roots") was closed by its
        // watcher a moment later, and the close wrote 0 roots over 117. The
        // bytes survived in the history and a rollback brought them back, but
        // nothing in the box said anything was wrong.
        if (m_Auth && !m_Wrote && m_Auth.OZS_GetState() != OZS_Const.STATE_OPEN)
        {
            m_Wrote = true;
            OZ_Log.Warn("storage: proxy: box " + m_Id + " never finished opening; the record is left exactly as it was");
        }
        // THE CLOSING WRITE IS A JOB (OZS_WholeJob). It carries the `close`
        // itself (OZS_OpLetter.close), so the box is shut in the same step as
        // its roots are written; it runs over the frames it needs, and the
        // session ends when it has been posted -- FinishEnd, from TickWhole.
        // Only a session with nothing to write ends here and says `closed`
        // on its own.
        if (m_Auth && !m_Wrote)
        {
            m_Wrote = true;
            if (OZS_Commit.Whole(this, "the session is ending", true))
                return;
            // Starting it failed the session on the way (Ready, or the file):
            // Fail has ended it already, and there is nothing left to say.
            if (m_Ended)
                return;
            OZ_Log.Error("storage: proxy: box " + m_Id + " could not be written on the way out; what it held is below");
            if (m_Auth)
                OZ_Log.Error("storage: proxy: " + OZS_Ops.Grid(m_Auth));
        }
        FinishEnd(false);
    }

    // The end itself: the screens are told, the authority is let go and --
    // when no closing letter carried the `close` -- the bridge is told the
    // box is shut.
    void FinishEnd(bool shutByLetter)
    {
        if (m_Ended)
            return;
        m_Ended = true;
        // NOBODY IS LEFT HOLDING A BOX THAT NO LONGER EXISTS. A proxy is a
        // real container in the client's own memory, and until this message
        // existed nothing ever told the client to let one go: it stayed in the
        // vicinity panel for the rest of the session, under the box's own
        // name, empty. Two of them side by side is what the owner reported as
        // "there are two boxes here" and "the box is empty" -- they had opened
        // the ghost of a box removed half an hour earlier (measured 2026-09-24:
        // the client held mirrors=2, one of them 0/0 for a deleted box).
        for (int i = 0; i < m_Watchers.Count(); i++)
            m_Watchers.Get(i).Gone();
        m_Watchers.Clear();
        if (m_Auth)
        {
            int gone = OZS_Authority.Discard(m_Id);
            OZ_Log.Info("storage: proxy: session " + m_Id + " ended, " + gone.ToString() + " entity(ies) being released");
            m_Auth = null;
        }
        // AND THE BRIDGE IS TOLD THE BOX IS SHUT.
        //
        // An open says so (OZS_OpenJob posts ROUTE_OPENED), and until now a
        // close said nothing at all -- the closing write goes through the TURN
        // route, which by design does not touch the box's status. So every box
        // this scheme ever opened stayed `open` in SQL for good, and `open` is
        // exactly what makes the bridge refuse an admin's give, unpark, edit,
        // rollback or empty. One session, and the box became unmanageable
        // (measured 2026-09-26: the bridge said open, the mod said CLOSED, and
        // the status had to be fixed by hand before a rollback would run).
        //
        // NOT WHEN THE CLOSING WRITE WENT OUT: that letter closes the box
        // itself, in one transaction with the roots. A separate `closed` was
        // a concurrent request that could land first, and an admin's write in
        // the gap was overwritten by the rewrite that followed (review
        // 2026-09-26, C4).
        if (shutByLetter)
            return;
        OZS_IdLetter shut = new OZS_IdLetter();
        shut.id = m_Id;
        shut.version = m_SqlVersion;
        string json;
        string err;
        if (JsonFileLoader<OZS_IdLetter>.MakeData(shut, json, err, false))
            OZS_Bridge.Post(OZS_Const.ROUTE_CLOSED, json, new OZS_AckReply("the proxy session ended"));
    }

    // ---- the absolute letter, as a job -----------------------------------

    // The file of an absolute letter being written, a budget's worth per
    // frame; see OZS_WholeJob. While it runs the box must not change, so
    // operations queue behind it (OperateAs) and the session does not end
    // (IsDone).
    ref OZS_WholeJob m_Whole;

    // Starts one. True when it is running -- the letter is on its way, as
    // far as every caller is concerned -- false when it could not even
    // begin, in which case the session has failed on the way.
    bool BeginWhole(OZS_Letter letter, string why, bool closing)
    {
        if (!m_Auth)
            return false;
        if (m_Whole)
        {
            OZ_Log.Warn("storage: proxy: box " + m_Id + ": a whole write is already running; another is not started (" + why + ")");
            return false;
        }
        OZS_WholeJob job = new OZS_WholeJob(this, letter, why, closing);
        if (!job.Begin())
        {
            OZ_Log.Error("storage: proxy: a turn of box " + m_Id + " cannot be written: " + job.m_Why);
            Fail("the turn could not be written");
            return false;
        }
        m_Whole = job;
        OZ_Log.Info("storage: proxy: box " + m_Id + ": the record is being set to what the box holds -- " + job.m_Blobs.Count().ToString() + " root(s), " + job.m_Entities.ToString() + " entity(ies) (" + why + "); writing over the frames it needs");
        return true;
    }

    protected void TickWhole(float budgetSec)
    {
        if (!m_Whole)
            return;
        if (!m_Whole.Step(budgetSec))
            return;
        OZS_WholeJob done = m_Whole;
        m_Whole = null;
        if (done.m_Failed)
        {
            OZ_Log.Error("storage: proxy: a turn of box " + m_Id + " was written short: " + done.m_Why);
            done.Abort();
            if (done.m_Closing && m_Auth)
            {
                OZ_Log.Error("storage: proxy: box " + m_Id + " could not be written on the way out; what it held is below");
                OZ_Log.Error("storage: proxy: " + OZS_Ops.Grid(m_Auth));
            }
            Fail("the turn could not be written");
            return;
        }
        if (!done.m_Letter.PostWritten(done.FileName(), done.m_Blobs, done.m_Entities, done.m_Closing, done.m_Note, done.m_Frames, done.m_WorkMs))
            return;
        if (done.m_Closing)
            FinishEnd(true);
    }

    // The whole write at once, for the one moment no frame is coming: the
    // mission's end.
    void FinishWholeNow()
    {
        int guard = 0;
        while (m_Whole && !m_Ended && guard < 100000)
        {
            TickWhole(3600.0);
            guard++;
        }
    }

    // ---- what the watchers are told --------------------------------------

    // The whole box as rows, parents before children. The handles come from
    // the authority and are the only name an operation may use.
    int Snapshot(array<ref OZS_Row> into)
    {
        into.Clear();
        if (!m_Auth)
            return 0;
        OZS_Authority.Index(m_Auth);
        array<EntityAI> nodes = new array<EntityAI>();
        array<int> parents = new array<int>();
        OZS_Records.Flatten(m_Auth, -1, nodes, parents);
        for (int i = 1; i < nodes.Count(); i++)
        {
            EntityAI e = nodes.Get(i);
            if (!e)
                continue;
            int parentHandle = 0;
            if (parents.Get(i) > 0)
                parentHandle = OZS_Authority.Handle(m_Auth, nodes.Get(parents.Get(i)));
            OZS_Row r = new OZS_Row();
            Describe(e, OZS_Authority.Handle(m_Auth, e), parentHandle, r);
            into.Insert(r);
        }
        return into.Count();
    }

    // One entity as a row. Everything here is asked of the entity itself, so
    // a modded item answers for its own quantity and damage.
    static void Describe(EntityAI e, int handle, int parentHandle, OZS_Row r)
    {
        InventoryLocation il = new InventoryLocation();
        e.GetInventory().GetCurrentInventoryLocation(il);
        int flip = 0;
        if (il.GetFlip())
            flip = 1;
        r.Set(handle, parentHandle, il.GetType(), il.GetSlot(), il.GetRow(), il.GetCol(), flip, e.GetType());
        r.health = Math.Round(e.GetHealth01("", "") * 100);
        ItemBase item = ItemBase.Cast(e);
        if (item && item.HasQuantity())
        {
            r.qty = Math.Round(item.GetQuantity());
            r.qtyMax = Math.Round(item.GetQuantityMax());
        }
        // AND THE ROUNDS, WHICH ARE NOT THE QUANTITY. The screen draws a
        // magazine's number from GetAmmoCount alone, so without this a pile
        // of two rounds arrives looking like a full one.
        Magazine mag = Magazine.Cast(e);
        if (mag)
            r.ammo = mag.GetAmmoCount();
    }

    // Every proxy of this box hears every change (§8.4), including the one
    // whose player caused it: the proxy shows its own guess at once and this
    // is the authority's word, which always wins.
    // `by` is the uid of the player whose operation caused this. It is what
    // lets a watcher tell ITS OWN changes from somebody else's -- see
    // OZS_Watcher.m_OthersAt and the stale test in Operate.
    void Tell(int change, OZS_Row r, string by = "")
    {
        m_Version++;
        for (int i = 0; i < m_Watchers.Count(); i++)
            m_Watchers.Get(i).Change(change, r, m_Version, by);
    }

    void TellGone(int handle, string by = "")
    {
        OZS_Row r = new OZS_Row();
        r.Set(handle, 0, 0, 0, -1, -1, 0, "");
        Tell(OZS_Const.CH_GONE, r, by);
    }

    // THE TWO NUMBERS ARE LOOKED UP INTO LOCALS FIRST, NEVER IN THE ARGUMENT
    // LIST. `Describe(e, Handle(e), ParentHandle(e), r)` reached the client
    // with the PARENT's number in the handle field for every nested item:
    // ParentHandle calls Handle again, and the engine hands the second call's
    // result back in the slot of the first (measured 2026-09-26: a rag moved
    // into a bag hung in a stash was told as "#1 Rag" -- #1 being the bag --
    // while the server's own table said #2; a quantity change on it landed
    // on the bag; a stack split off it made the client delete the bag).
    // Snapshot and TellTree never had the fault because they computed the
    // parent's number into a local before the call.
    void TellMoved(EntityAI e, string by = "")
    {
        if (!e || !m_Auth)
            return;
        OZS_Row r = new OZS_Row();
        int movedHandle = OZS_Authority.Handle(m_Auth, e);
        int movedParent = ParentHandle(e);
        Describe(e, movedHandle, movedParent, r);
        OZ_Log.Dbg("storage: proxy: telling moved #" + r.handle.ToString() + " " + r.cls + " in #" + r.parent.ToString() + " to " + r.Where());
        Tell(OZS_Const.CH_MOVED, r, by);
    }

    void TellQuantity(EntityAI e, string by = "")
    {
        if (!e || !m_Auth)
            return;
        OZS_Row r = new OZS_Row();
        int qtyHandle = OZS_Authority.Handle(m_Auth, e);
        int qtyParent = ParentHandle(e);
        Describe(e, qtyHandle, qtyParent, r);
        Tell(OZS_Const.CH_QTY, r, by);
    }

    void TellAdded(EntityAI e, string by = "")
    {
        if (!e || !m_Auth)
            return;
        OZS_Row r = new OZS_Row();
        int addedHandle = OZS_Authority.Handle(m_Auth, e);
        int addedParent = ParentHandle(e);
        Describe(e, addedHandle, addedParent, r);
        Tell(OZS_Const.CH_ADDED, r, by);
    }

    // A WHOLE SUBTREE, ROW BY ROW, PARENTS FIRST. An item arriving from
    // outside brings everything inside it, and one row for the top of it
    // built every proxy an EMPTY container: the real tree had just been taken
    // off the network, and the row said nothing about what was in it, so a
    // full backpack put into the box opened empty until the next restream
    // (review 2026-09-26, C1). A proxy resolves a row's parent by handle, so
    // the order Flatten gives -- each parent before its children -- is the
    // order that builds.
    void TellTree(EntityAI e, string by = "")
    {
        if (!e || !m_Auth)
            return;
        array<EntityAI> nodes = new array<EntityAI>();
        array<int> parents = new array<int>();
        OZS_Records.Flatten(e, -1, nodes, parents);
        for (int i = 0; i < nodes.Count(); i++)
        {
            EntityAI node = nodes.Get(i);
            if (!node)
                continue;
            int parentHandle;
            if (i == 0)
                parentHandle = ParentHandle(node);
            else
                parentHandle = OZS_Authority.Handle(m_Auth, nodes.Get(parents.Get(i)));
            OZS_Row r = new OZS_Row();
            Describe(node, OZS_Authority.Handle(m_Auth, node), parentHandle, r);
            Tell(OZS_Const.CH_ADDED, r, by);
        }
    }

    int ParentHandle(EntityAI e)
    {
        EntityAI parent = e.GetHierarchyParent();
        if (!parent || parent == m_Auth)
            return 0;
        return OZS_Authority.Handle(m_Auth, parent);
    }

    // ---- the turn's bookkeeping ------------------------------------------

    // Somebody did something: the idle clock of the session restarts.
    void Touch()
    {
        m_Empty = 0;
    }

    // WHAT THE WIRE COSTS, measured rather than assumed. The question it
    // answers is a real one: §7 hands the item over before SQL has confirmed
    // the turn, and the alternative -- waiting for the answer -- is only worth
    // discussing next to a number. A letter is a file written to disk, an HTTP
    // call to the bridge, a SQLite transaction, and a reply picked up on a
    // later frame; none of those are guesses anybody should make.
    float m_SentAt;
    float m_WireTotal;
    float m_WireWorst;
    int m_WireCount;

    void OnCommitSent()
    {
        m_Flying++;
        m_SentAt = GetGame().GetTickTime();
    }

    // ---- a sort waiting for the record -----------------------------------

    // Set by OZS_Ops.Sort once the letter is away: the record now says where
    // every root belongs, and the authority has to be rebuilt to match. The
    // rebuild cannot start earlier -- there would be nothing to read.
    protected bool m_Resort;

    void SortPending()
    {
        m_Resort = true;
    }

    // THE AUTHORITY IS EMPTIED AND FILLED AGAIN, by the same open job that
    // filled it the first time: item by item, on the frame budget, into a real
    // container. That is the whole reason a sort does not have to move a
    // hundred things around a live box.
    //
    // IN TWO STEPS, A FEW FRAMES APART. The emptying is a job of the
    // authority (OZS_Teardown, ReleaseDeletesPerFrame a frame), and the
    // refill cannot be asked for until it is done: an entity deleted this
    // frame holds its cells until the frame ends, and a refill over it would
    // find no room. The box is shut for the whole of it, exactly as it is
    // during the refill, so a turn arriving meanwhile is refused the same
    // way. OnFrame calls back here every frame while `m_Emptying` stands.
    protected ref OZS_Teardown m_Emptying;

    protected void Resort()
    {
        if (m_Emptying)
        {
            if (!m_Emptying.IsDone())
                return;
            m_Emptying = null;
            if (m_Ended || !m_Auth)
                return;
            // Closed while the box was being emptied: the record already
            // holds the sorted layout (the sort's letter went first), and a
            // box with nothing in it has nothing to write on the way out.
            if (m_Closing)
            {
                OZ_Log.Info("storage: proxy: box " + m_Id + " was closed while being emptied for a sort; the record holds the sorted layout");
                m_Wrote = true;
                m_Closing = false;
                End();
                return;
            }
            Refill();
            return;
        }
        if (!m_Resort)
            return;
        m_Resort = false;
        if (m_Ended || !m_Auth)
            return;
        m_Auth.OZS_ForgetRoots();
        m_Auth.OZS_SetState(OZS_Const.STATE_CLOSED);
        m_Emptying = OZS_Authority.Empty(m_Auth);
        if (!m_Emptying)
        {
            Fail("the box could not be emptied for the sort");
            return;
        }
        OZ_Log.Info("storage: proxy: box " + m_Id + " is being emptied for the sort, " + m_Emptying.Count().ToString() + " entity(ies)");
        // Every screen starts again: the rows they hold name cells that are
        // all about to change. Sent now rather than after the refill's
        // request, so no client draws a box that is half gone.
        for (int i = 0; i < m_Watchers.Count(); i++)
            m_Watchers.Get(i).Restart();
        // Nothing to delete -- an empty box sorted -- refills at once.
        if (m_Emptying.IsDone())
            Resort();
    }

    protected void Refill()
    {
        string why;
        if (!OZS_Controller.Get().RequestOpenAs(m_Auth, "sort", "", why))
        {
            // The record holds the sorted layout either way, so nothing is
            // lost -- but this session has an empty box in front of the
            // player, and it must not pretend otherwise.
            OZ_Log.Error("storage: proxy: box " + m_Id + " could not be refilled after the sort: " + why);
            Fail("the box could not be refilled after the sort");
            return;
        }
        OZ_Log.Info("storage: proxy: box " + m_Id + " is refilling in sorted order");
    }

    // ---- several steps, one letter ---------------------------------------

    // ONE LETTER FOR AN OPERATION OF SEVERAL STEPS. The cross-boundary swap
    // is three crossings by construction -- the box item steps aside, the
    // player's item comes in, the box item goes out -- and each used to post
    // its own letter. The core's client sends every call at once, so the
    // three were concurrent requests the bridge applied in ARRIVAL order,
    // and a drop of position 114 landing before the rewrite of position 114
    // was refused by the identity check: "root 114 holds Rag, the letter
    // says Canteen" (owner, 2026-09-26, a canteen for a rag, on the sixth
    // swap in a row). The refusal was answered correctly -- an absolute
    // rewrite, nothing lost -- but the swap stopped half way and the player
    // was told the record had not taken it.
    //
    // While a batch is open, every commit writes into this letter instead of
    // posting one of its own, and the operation posts it once at the end.
    // The bridge applies one letter's rewrites, then its drops, then its
    // additions, which is the order the game's own bookkeeping uses, so
    // there is no order left for the wire to get wrong.
    ref OZS_Letter m_Batch;

    // The letter an operation's commit writes into: the open batch, or one
    // of its own.
    OZS_Letter Letter()
    {
        if (m_Batch)
            return m_Batch;
        return new OZS_Letter(this);
    }

    void BeginBatch()
    {
        m_Batch = new OZS_Letter(this);
    }

    void EndBatch()
    {
        OZS_Letter batch = m_Batch;
        m_Batch = null;
        if (batch)
            batch.Post();
    }

    // ---- a hand-over waiting for the record ------------------------------

    // Set by OZS_Boundary.Out when WaitForRecord is on: the item is still in
    // the box, its letter is on the wire, and it goes to the player only if
    // that letter lands. Exactly one can be waiting, because exactly one turn
    // is ever in flight (§8.3) and a take-out IS a turn.
    ref OZS_Handover m_Handover;

    // True when there is a turn in flight for this hand-over to wait on --
    // or a batch still open that will carry it. False means no letter went
    // out, and the caller must not expect a callback.
    bool HoldHandover(OZS_Watcher w, EntityAI e, InventoryLocation dst, int handle)
    {
        if (m_Ended)
            return false;
        if (m_Flying <= 0 && !m_Batch)
            return false;
        m_Handover = new OZS_Handover(w.m_Uid, e, dst, handle);
        return true;
    }

    // The letter landed: the item may go. Called before anything else in
    // OnCommitted, because the rest of that method compares the box against
    // the record and this move is part of what the record now says.
    protected void ReleaseHandover()
    {
        if (!m_Handover)
            return;
        OZS_Handover h = m_Handover;
        m_Handover = null;
        OZS_Watcher w = WatcherOfUid(h.m_Uid);
        PlayerBase player = null;
        if (w)
            player = PlayerBase.Cast(w.Player());
        InventoryLocation dst = new InventoryLocation();
        bool place = false;
        if (player && h.m_Item)
            place = h.Where(player, dst);
        if (!place)
        {
            // The player left, or the place they named is gone, in the time
            // the wire took. The record has already been told this item left
            // the box, so the box may not simply keep it and say nothing: it
            // is written back in, which is the same repair a failed move
            // makes.
            if (h.m_Item)
            {
                OZ_Log.Warn("storage: proxy: box " + m_Id + ": there was nowhere to hand " + h.m_Item.GetType() + " after the record took the turn; it is written back into the box");
                OZS_Commit.Added(this, h.m_Item);
            }
            return;
        }
        OZS_Boundary.Handover(this, w, h.m_Item, dst, h.m_Handle);
    }

    // ---- a credit waiting for the record ---------------------------------

    // Set by OZS_Boundary.StackOut when WaitForRecord is on: what a stack in
    // the box gave is out of it and its letter is on the wire, and the
    // player's stack receives it only if that letter lands. One at a time,
    // for the same reason as the hand-over above.
    ref OZS_Credit m_Credit;

    bool HoldCredit(OZS_Credit c)
    {
        if (m_Ended)
            return false;
        if (m_Flying <= 0 && !m_Batch)
            return false;
        m_Credit = c;
        return true;
    }

    protected void ReleaseCredit()
    {
        if (!m_Credit)
            return;
        OZS_Credit c = m_Credit;
        m_Credit = null;
        OZS_Boundary.Credited(this, c);
    }

    protected void WireTook()
    {
        if (m_SentAt <= 0)
            return;
        float took = GetGame().GetTickTime() - m_SentAt;
        m_SentAt = 0;
        m_WireTotal = m_WireTotal + took;
        m_WireCount++;
        if (took > m_WireWorst)
            m_WireWorst = took;
        OZ_Log.Dbg("storage: proxy: box " + m_Id + ": the wire took " + (took * 1000).ToString() + " ms");
    }

    string Wire()
    {
        if (m_WireCount == 0)
            return "";
        float mean = m_WireTotal / m_WireCount;
        return " wire " + (mean * 1000).ToString() + " ms mean / " + (m_WireWorst * 1000).ToString() + " ms worst over " + m_WireCount.ToString();
    }

    // A LETTER THE BRIDGE REFUSED IS OFF THE WIRE TOO. Only OnCommitted used
    // to put a flight back, so every refusal left one behind for good: the
    // queue never drained, End never got its turn, and IsDone never came true
    // -- the box stayed open with nobody in it for the rest of the session.
    // Harmless while refusals were nearly impossible; not once a turn can be
    // refused for naming the wrong root (measured 2026-09-25: flying stuck at
    // 1 after the first refused turn).
    void OnCommitRefused()
    {
        WireTook();
        // THE ITEM STAYS IN THE BOX, and that is the whole point of waiting:
        // a turn that was not written is a turn in which nothing left. The
        // screen is told so it stops drawing the item in the player's hands.
        if (m_Handover)
        {
            OZS_Watcher back = WatcherOfUid(m_Handover.m_Uid);
            if (back)
                back.No(m_Handover.m_Handle, "#STR_OZS_NOT_WRITTEN", m_Version);
            m_Handover = null;
        }
        // AND WHAT A STACK GAVE GOES BACK INTO IT, for the same reason: the
        // giver is still in the box and the record still counts it whole.
        if (m_Credit)
        {
            OZS_Boundary.Uncredited(this, m_Credit);
            m_Credit = null;
        }
        m_Flying--;
        if (m_Flying < 0)
            m_Flying = 0;
    }

    void OnCommitted(int version, int roots, int entities)
    {
        WireTook();
        m_Flying--;
        if (m_Flying < 0)
            m_Flying = 0;
        // THE HAND-OVER WAITS FOR THE LAST LETTER ON THE WIRE, NOT FOR THE
        // FIRST ANSWER. One turn at a time makes those the same thing --
        // except for `Across`, which posts three letters and holds its
        // hand-over on the third. Released on the first reply, the item
        // reached the player before the `Left` that drops it from the record
        // had landed, which is the one window WaitForRecord exists to close
        // (review 2026-09-26, B1). Before the comparison below either way,
        // which judges the box against a record that has let this item go.
        if (m_Flying == 0)
        {
            ReleaseHandover();
            ReleaseCredit();
        }
        // A sort's letter has landed: the record is sorted, so the authority
        // can be rebuilt from it. Before the comparison below, which would
        // otherwise judge a box that is about to be emptied on purpose.
        if (m_Resort)
        {
            Resort();
            return;
        }
        m_SqlVersion = version;
        m_SqlRoots = roots;
        m_SqlEntities = entities;
        // THE RECORD AND THE BOX MUST AGREE, AND WHEN THEY DO NOT IT MUST BE
        // LOUD. A commit that drifts from the container is how a box quietly
        // grows or loses items across a session; with nothing else saving
        // this box, SQL being wrong is the box being wrong. Checked only when
        // nothing else is in flight -- an answer that arrives while another
        // turn is on the wire is expected to lag.
        if (m_Flying > 0 || !m_Auth)
        {
            NextWaiting();
            return;
        }
        // NOT COMPARED, AND NOT REPAIRED: a compromised box disagrees with
        // the record by definition, and setting the record to what it holds
        // is exactly the write that emptied one (2026-09-26).
        if (m_Compromised)
        {
            End();
            return;
        }
        int here = OZS_Records.CountTree(m_Auth) - 1;
        int roothere = m_Auth.OZS_CountEntities();
        if (here != entities || roothere != roots)
        {
            OZ_Log.Error("storage: proxy: box " + m_Id + " and its record disagree after a turn: the box holds " + roothere.ToString() + " root(s) and " + here.ToString() + " entities, the record says " + roots.ToString() + " and " + entities.ToString());
            m_Repairs++;
            if (m_Repairs > OZS_Const.MAX_REPAIRS)
            {
                // Something is wrong that rewriting cannot reach. Saying so
                // once and stopping is better than a box that thrashes: the
                // session goes on, and the closing write still gets its turn.
                //
                // THE BOX IS NOT TAKEN AWAY FROM THE PLAYER. Every item is in
                // the authority and the authority is fine -- what is in doubt
                // is the bookkeeping about it. Ending the session here would
                // discard the authority WITH the items and write nothing
                // (see Fail), so the player would lose real things over a
                // disagreement about counting. They are told, the admin is
                // told, and the closing write still says what the box holds.
                //
                // This cap governs the COUNT path only. The refusal path in
                // OZS_OpReply repairs whatever the count is, and must: a
                // refused letter was never written, so without the repair the
                // record falls behind and stays behind, and everything the
                // player does afterwards is lost if the server goes down.
                // A repair that is merely cosmetic can be given up; one that
                // is the only way back onto the wire cannot.
                if (m_Repairs == OZS_Const.MAX_REPAIRS + 1)
                {
                    OZ_Log.Error("storage: proxy: box " + m_Id + " has been put straight " + OZS_Const.MAX_REPAIRS.ToString() + " time(s) and still disagrees; no more will be tried this session");
                    // Not to the RPT, where nobody looks: into the box's own
                    // history, which is where an admin already goes to read
                    // what happened to a box.
                    OZS_Audit.Log("drift", m_Id, "", "", "", 0, roothere, roots,
                        "", "the box holds " + roothere.ToString() + " root(s) and " + here.ToString() + " entities, the record says " + roots.ToString() + " and " + entities.ToString() + "; repairs gave up");
                    for (int t = 0; t < m_Watchers.Count(); t++)
                        m_Watchers.Get(t).No(0, "#STR_OZS_DRIFT", m_Version);
                }
                // Falls through to the tail on purpose: giving up on the
                // rewrite posts no letter, so nothing else would ever free
                // the wire and the queue would stand still for good.
                if (m_Closing)
                {
                    End();
                    return;
                }
                NextWaiting();
                return;
            }
            // AND IT IS PUT STRAIGHT, not merely reported. This check has been
            // here since the proxy was written and it only ever shouted; a
            // session went on writing relative letters in a numbering that had
            // already come apart, and the wrong picture reached SQL and stayed
            // there, because nothing rewrites the record at close either.
            OZS_Commit.Whole(this, "the box and the record disagreed");
            return;
        }
        // The wire is free: whoever was waiting for it goes now -- or, if the
        // session was only waiting for the wire to close, it closes.
        if (m_Closing)
        {
            End();
            return;
        }
        NextWaiting();
    }

    // The session cannot go on: the bridge is gone, or a turn could not be
    // written. Everything the players did that reached SQL stands; the rest is
    // the one turn §9 is willing to lose. The screens are told and the
    // authority is let go.
    void Fail(string why)
    {
        if (m_Ended)
            return;
        OZ_Log.Error("storage: proxy: session " + m_Id + " ends: " + why);
        for (int i = 0; i < m_Watchers.Count(); i++)
            m_Watchers.Get(i).No(0, "#STR_OZ_ERR_NO_BRIDGE", m_Version);
        // Nothing waiting ever happened, so there is nothing to undo and
        // nothing owed to the record -- they are simply dropped.
        m_Waiting.Clear();
        m_Delayed.Clear();
        // A batch half-written for an operation that will not finish.
        m_Batch = null;
        // A hand-over that was waiting for a turn nobody will ever answer.
        // The item is in the authority and the record still describes it, so
        // letting it go with the authority loses nothing: the next open builds
        // it again, exactly once.
        m_Handover = null;
        // Likewise a credit: what it holds is still in the record, and goes
        // with the authority.
        m_Credit = null;
        // And a whole write half done: its file is removed, the record stays
        // at the last turn the bridge took.
        if (m_Whole)
        {
            m_Whole.Abort();
            m_Whole = null;
        }
        // An emptying still running finishes by itself under the authority;
        // the discard below only adds what is left of the box to the queue.
        m_Emptying = null;
        // NO CLOSING WRITE ON THE WAY OUT OF A FAILURE. Whoever called Fail
        // has already decided the record cannot be put straight -- the repair
        // was tried and refused, or the bridge is gone. Trying again from here
        // would call Fail from inside Fail, through the bridge check.
        m_Wrote = true;
        m_Closing = false;
        End();
    }

    // ---- operations ------------------------------------------------------

    void Operate(PlayerIdentity who, int op, int handle, int other, int netLow, int netHigh, int lt, int slot, int row, int col, int flip, int version)
    {
        OperateAs(OZS_Controller.UidOfIdentity(who), op, handle, other, netLow, netHigh, lt, slot, row, col, flip, version, false);
    }

    // `held` is true for an operation the stand's fake ping held back and is
    // now letting through; it must not be held a second time.
    protected void OperateAs(string uid, int op, int handle, int other, int netLow, int netHigh, int lt, int slot, int row, int col, int flip, int version, bool held)
    {
        OZS_Watcher w = WatcherOfUid(uid);
        if (!w)
            return;
        // THE STAND'S PING, ADDED ON THE WAY IN. The whole round trip in one
        // place, because the player only ever sees the moment the answer
        // arrives; the arguments are a handful of numbers and keep for as
        // long as they must. On a real server FakePingMs is 0 and this is a
        // comparison.
        int late = OZS_Settings.Get().FakePingMs;
        if (late > 0 && !held)
        {
            OZS_Waiting d = new OZS_Waiting(uid, op, handle, other, netLow, netHigh, lt, slot, row, col, flip);
            d.m_Version = version;
            d.m_Due = GetGame().GetTickTime() + late * 0.001;
            m_Delayed.Insert(d);
            return;
        }
        if (!m_Auth || m_Auth.OZS_GetState() != OZS_Const.STATE_OPEN)
        {
            w.No(handle, "the box is not ready", m_Version);
            return;
        }
        // STALE MEANS "SOMEBODY ELSE CHANGED IT", NOT "I AM BEHIND MY OWN
        // OPERATIONS" (measured against the owner on 2026-09-24: comparing
        // against the session version refused every drag after the first,
        // because a client's next drag leaves before the answer to the
        // previous one arrives, and each refusal restreamed the whole box --
        // which is exactly what "the box lags and items do not drag" looks
        // like). A watcher only has to be up to date with OTHER players.
        if (version < w.m_OthersAt)
        {
            w.No(handle, "stale", m_Version);
            w.Restart();
            return;
        }
        // ONE TURN AT A TIME, PER BOX (§8.3).
        //
        // A letter names the record's roots BY POSITION, and both sides
        // renumber as drops and additions land. Two letters in flight are two
        // sets of positions computed against different pictures: the bridge
        // applies them in arrival order, which is not guaranteed to be the
        // order they were sent in, and an out-of-order pair either throws on
        // an index that is not there -- loud, the session ends -- or, when
        // both indexes happen to be valid, writes the wrong roots quietly.
        //
        // It is also what §9's promise rests on. "The price of a crash is one
        // turn" is only true while at most one turn is unconfirmed; with N in
        // flight the price is N.
        //
        // So an operation that arrives while a turn is on the wire waits, and
        // runs when the bridge has answered. Waiting is not buffering: see
        // OZS_Waiting.
        if (m_Flying > 0 || m_Whole)
        {
            // A QUEUE WITH A CEILING. Past it the client is told to start
            // again rather than the server growing a backlog it will have to
            // apply against a box the player stopped looking at -- and a
            // client that far ahead of the record is drawing a picture it
            // cannot justify anyway.
            if (m_Waiting.Count() >= OZS_Const.MAX_WAITING)
            {
                w.No(handle, "stale", m_Version);
                w.Restart();
                return;
            }
            m_Waiting.Insert(new OZS_Waiting(w.m_Uid, op, handle, other, netLow, netHigh, lt, slot, row, col, flip));
            return;
        }
        OZS_Ops.Run(this, w, op, handle, other, netLow, netHigh, lt, slot, row, col, flip);
    }

    // The next operation that was waiting for the wire, if the wire is free.
    // One at a time on purpose: running the whole queue here would put every
    // one of its letters in flight at once, which is the thing being avoided.
    void NextWaiting()
    {
        if (m_Ended || m_Flying > 0 || m_Whole)
            return;
        while (m_Waiting.Count() > 0)
        {
            OZS_Waiting next = m_Waiting.Get(0);
            m_Waiting.RemoveOrdered(0);
            OZS_Watcher w = WatcherOfUid(next.m_Uid);
            // The player stopped looking while their turn waited. Nothing has
            // happened yet, so there is nothing to undo -- the next one goes.
            if (!w)
                continue;
            OZS_Ops.Run(this, w, next.m_Op, next.m_Handle, next.m_Other, next.m_NetLow, next.m_NetHigh, next.m_Lt, next.m_Slot, next.m_Row, next.m_Col, next.m_Flip);
            // A REFUSED OPERATION POSTS NO LETTER, so the wire is still free
            // and nothing will call back to drain the rest. Whoever is behind
            // it would wait for an answer that is never coming, which is a box
            // that stops responding after one refusal.
            if (m_Flying > 0)
                return;
            if (m_Ended)
                return;
        }
    }

    string Status()
    {
        string s = m_Id + " v" + m_Version.ToString() + " watchers " + m_Watchers.Count().ToString() + " waiting " + m_Waiting.Count().ToString();
        s = s + " sql v" + m_SqlVersion.ToString() + "/" + m_SqlRoots.ToString() + "r/" + m_SqlEntities.ToString() + "e";
        if (m_Flying > 0)
            s = s + " flying " + m_Flying.ToString();
        s = s + Wire();
        if (!m_Auth)
            return s + " NO AUTHORITY";
        s = s + " " + OZS_Const.StateName(m_Auth.OZS_GetState());
        s = s + " tree " + (OZS_Records.CountTree(m_Auth) - 1).ToString();
        for (int i = 0; i < m_Watchers.Count(); i++)
            s = s + " | " + m_Watchers.Get(i).Status();
        return s;
    }
}

// ---------------------------------------------------------------------------

class OZS_Watcher
{
    OZS_Session m_Session;
    string m_Uid;
    // The stream: a snapshot taken when the box became ready, sent in chunks.
    ref array<ref OZS_Row> m_Rows;
    int m_Sent;
    bool m_Begun;
    bool m_Whole;
    float m_Started;
    float m_Spent;
    // Changes that happened while the stream was still running. The snapshot
    // predates them, so they go after the end and never twice.
    ref array<ref OZS_Change> m_Queued;
    // The session version at the last change caused by SOMEBODY ELSE. An
    // operation older than this acted on a box another player has moved on,
    // and that is the only kind of stale there is.
    int m_OthersAt;
    // When the last restream began, and whether another was asked for
    // sooner than OZS_Const.RESTART_GAP allows -- see Restart().
    protected float m_RestartedAt;
    protected bool m_RestartWanted;

    void OZS_Watcher(OZS_Session session, PlayerIdentity who)
    {
        m_Session = session;
        m_Uid = OZS_Controller.UidOfIdentity(who);
        m_Rows = new array<ref OZS_Row>();
        m_Queued = new array<ref OZS_Change>();
        Restart();
    }

    // THE BOX FROM THE BEGINNING -- BUT NOT TWICE IN ONE SECOND. Three
    // things ask for it: a stale operation, a queue that overflowed and a
    // player pressing the button again, and every one of them is something
    // a client can do as fast as it likes. Asked too soon, the restart is
    // remembered and done by OnFrame once the gap has passed, so the client
    // is never left with a stale picture and the server is never made to
    // serialise a thousand rows per message (review 2026-09-26, A3).
    void Restart()
    {
        float now = GetGame().GetTickTime();
        if (m_Begun && now - m_RestartedAt < OZS_Const.RESTART_GAP)
        {
            m_RestartWanted = true;
            return;
        }
        m_RestartedAt = now;
        m_RestartWanted = false;
        m_Rows.Clear();
        m_Queued.Clear();
        m_Sent = 0;
        m_Begun = false;
        m_Whole = false;
        m_Started = 0;
        m_Spent = 0;
    }

    // The player, found by uid rather than kept as a reference: a player who
    // dies and respawns is a new entity, and a PlayerIdentity kept across a
    // disconnect is a dangling pointer.
    //
    // THE LAST ANSWER IS KEPT AND CHECKED, NOT TRUSTED. `GetPlayers` walks
    // every player and allocates, and this was asked three times per message
    // and once per watcher per frame (review 2026-09-26, D2). The kept
    // reference is weak, so a player who left reads as null; one who died
    // and came back is a different entity and fails the identity test. Either
    // way the walk runs again, so the answer is only ever right or empty.
    protected Man m_Man;

    Man Player()
    {
        if (m_Man && m_Man.GetIdentity() && m_Man.GetIdentity().GetPlainId() == m_Uid)
            return m_Man;
        m_Man = null;
        array<Man> men = new array<Man>();
        GetGame().GetPlayers(men);
        for (int i = 0; i < men.Count(); i++)
        {
            Man man = men.Get(i);
            if (man && man.GetIdentity() && man.GetIdentity().GetPlainId() == m_Uid)
            {
                m_Man = man;
                return man;
            }
        }
        return null;
    }

    // EVERY MESSAGE TO THIS WATCHER GOES THROUGH HERE, and asks for the
    // player first: the stand can make a watcher with a made-up uid to act
    // as a second player would (OZS_Verb, do=asother), and nobody is behind
    // that one, so nothing is sent to it. (The stand's fake
    // ping used to hold the built message back here; a ScriptRPC does not
    // survive the frame it was written in, so the ping now holds the
    // OPERATION on its way in instead -- see OZS_Session.OperateAs.)
    void Say(ScriptRPC rpc, int id)
    {
        Man man = Player();
        if (!rpc || !man)
            return;
        rpc.Send(man, id, true, man.GetIdentity());
    }

    // The identity to address a message to: THE PLAYER'S OWN, not the one the
    // RPC came in with. They are different objects for the same person.
    PlayerIdentity Who()
    {
        Man man = Player();
        if (!man)
            return null;
        return man.GetIdentity();
    }

    string Name()
    {
        PlayerIdentity id = Who();
        if (!id)
            return "";
        return id.GetName();
    }

    bool Alive()
    {
        return Player() != null;
    }

    void OnFrame(float timeslice)
    {
        // A restart asked for too soon after the last one: its turn now.
        if (m_RestartWanted && GetGame().GetTickTime() - m_RestartedAt >= OZS_Const.RESTART_GAP)
            Restart();
        if (m_Whole)
            return;
        if (!m_Session.m_Auth)
            return;
        if (m_Session.m_Auth.OZS_GetState() != OZS_Const.STATE_OPEN)
            return;
        float t0 = GetGame().GetTickTime();
        if (!m_Begun)
        {
            m_Started = t0;
            m_Session.Snapshot(m_Rows);
            Man target = Player();
            PlayerIdentity to = Who();
            string aim = "target=none";
            if (target)
                aim = "target=" + target.GetType() + "/" + target.GetNetworkIDString();
            if (to)
                aim = aim + " recipient=" + to.GetPlainId();
            else
                aim = aim + " recipient=none";
            OZ_Log.Dbg("storage: proxy: BEGIN of " + m_Session.m_Id + " to " + m_Uid + " " + aim);
            ScriptRPC begin = new ScriptRPC();
            begin.Write(m_Session.m_Id);
            begin.Write(m_Session.m_Class);
            begin.Write(m_Rows.Count());
            begin.Write(m_Session.m_Version);
            Say(begin, OZS_Const.RPC_PX_BEGIN);
            m_Begun = true;
            m_Sent = 0;
        }
        int chunk = OZS_Settings.Get().ProxyRowsPerMessage;
        int perFrame = OZS_Settings.Get().ProxyMessagesPerFrame;
        for (int m = 0; m < perFrame && m_Sent < m_Rows.Count(); m++)
        {
            int n = m_Rows.Count() - m_Sent;
            if (n > chunk)
                n = chunk;
            ScriptRPC rows = new ScriptRPC();
            rows.Write(m_Session.m_Id);
            rows.Write(n);
            for (int i = 0; i < n; i++)
                OZS_Wire.WriteRow(rows, m_Rows.Get(m_Sent + i));
            Say(rows, OZS_Const.RPC_PX_ROWS);
            m_Sent = m_Sent + n;
        }
        if (m_Sent < m_Rows.Count())
        {
            m_Spent = m_Spent + (GetGame().GetTickTime() - t0);
            return;
        }
        ScriptRPC end = new ScriptRPC();
        end.Write(m_Session.m_Id);
        end.Write(m_Rows.Count());
        end.Write(m_Session.m_Version);
        Say(end, OZS_Const.RPC_PX_END);
        m_Whole = true;
        m_Spent = m_Spent + (GetGame().GetTickTime() - t0);
        OZ_Log.Info("storage: proxy: " + m_Uid + " has " + m_Rows.Count().ToString() + " row(s) of " + m_Session.m_Id + " in " + (m_Spent * 1000).ToString() + " ms of server time over " + ((GetGame().GetTickTime() - m_Started) * 1000).ToString() + " ms");
        Flush();
    }

    // "Let this box go." Sent when the session ends for any reason: the screen
    // was closed, the player left or died, the box was removed, the bridge
    // refused a turn. The client drops the proxy and the vicinity panel loses
    // it with the same frame.
    void Gone()
    {
        if (!Player() || !Who())
            return;
        ScriptRPC bye = new ScriptRPC();
        bye.Write(m_Session.m_Id);
        Say(bye, OZS_Const.RPC_PX_GONE);
    }

    void Change(int change, OZS_Row r, int version, string by = "")
    {
        // The version this watcher must be caught up with before its own
        // operations count: only somebody else's changes raise it.
        if (by != "" && by != m_Uid)
            m_OthersAt = version;
        if (!m_Whole)
        {
            m_Queued.Insert(new OZS_Change(change, r, version));
            return;
        }
        Post(change, r, version);
    }

    protected void Flush()
    {
        for (int i = 0; i < m_Queued.Count(); i++)
            Post(m_Queued.Get(i).m_What, m_Queued.Get(i).m_Row, m_Queued.Get(i).m_Version);
        m_Queued.Clear();
    }

    protected void Post(int change, OZS_Row r, int version)
    {
        ScriptRPC c = new ScriptRPC();
        c.Write(m_Session.m_Id);
        c.Write(change);
        c.Write(version);
        OZS_Wire.WriteRow(c, r);
        Say(c, OZS_Const.RPC_PX_CHANGE);
    }

    // A refusal always carries the version the server is at, so the proxy can
    // decide whether a full resend is coming.
    void No(int handle, string why, int version)
    {
        ScriptRPC no = new ScriptRPC();
        no.Write(m_Session.m_Id);
        no.Write(handle);
        no.Write(version);
        no.Write(why);
        Say(no, OZS_Const.RPC_PX_NO);
    }

    string Status()
    {
        string s = m_Uid + " " + m_Sent.ToString() + "/" + m_Rows.Count().ToString();
        if (m_Whole)
            return s + " whole";
        return s + " streaming";
    }
}

class OZS_Change
{
    int m_What;
    ref OZS_Row m_Row;
    int m_Version;

    void OZS_Change(int what, OZS_Row r, int version)
    {
        m_What = what;
        m_Row = r;
        m_Version = version;
    }
}
