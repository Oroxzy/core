/*
 * Copyright (C) 2005-2011 MaNGOS <http://getmangos.com/>
 * Copyright (C) 2009-2011 MaNGOSZero <https://github.com/mangos/zero>
 * Copyright (C) 2011-2016 Nostalrius <https://nostalrius.org>
 * Copyright (C) 2016-2017 Elysium Project <https://github.com/elysium-project>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "WorldSocket.h"
#include "Common.h"
#include "Database/DatabaseEnv.h"
#include "Log.h"
#include "Opcodes.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "Player.h"
#include "ObjectMgr.h"
#include "Group.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "World.h"
#include "BattleGroundMgr.h"
#include "MapManager.h"
#include "SocialMgr.h"
#include "PlayerBotMgr.h"
#include "PlayerBotAI.h"
#include "Anticheat.h"
#include "Language.h"
#include "Chat.h"
#include "MasterPlayer.h"
#include "PlayerBroadcaster.h"
#include "Crypto/Hash/MD5.h"

#include <limits>

// select opcodes appropriate for processing in Map::Update context for current session state
static bool MapSessionFilterHelper(WorldSession* session, OpcodeHandler const& opHandle)
{
    // we do not process not logged in player packets
    Player* plr = session->GetPlayer();
    if (!plr)
        return false;

    // in Map::Update() we do not process packets where player is not in world!
    return plr->IsInWorld();
}


/*
 * A VISITOR'S VIEW OF THE MATCH RUNS A LITTLE BEHIND, AND IS INTERPOLATED IN THE GAP.
 *
 * This is the third and last piece of "the spectator's view stutters", because the first two
 * turned out to be necessary and not sufficient. Exempting him from movement compression and
 * draining the movement queue every sixteen milliseconds instead of once a tick both had to
 * happen - and with both in place, measured off a recording, the fighter he was watching still
 * moved in bursts three frames long with fifty to a hundred and forty milliseconds of nothing
 * between them. The two clients were on the same machine as the server, the anticheat was off,
 * and the relay was faster than the gaps. What remained was the SENDER: the 1.12 client, while
 * it is running and turning at once, hands out its position that often and no more, and the
 * viewer's client extrapolates straight ahead between one report and the next. On a curve or a
 * strafe that is the "one yard forward and pulled back again" every player of this game has
 * seen on somebody running past.
 *
 * A fighter cannot be helped. He needs the other man's position the instant it exists, and the
 * only thing that makes a sparse stream smooth is waiting for the next point before drawing the
 * line to it. A VISITOR CAN WAIT. He fights nobody, so his view can run a hundred and fifty
 * milliseconds behind the match without anything being lost, and in that window the server
 * holds two real reports of every man he watches and can put a synthetic heartbeat between
 * them every sub-tick - position and facing interpolated, the flags of the earlier report so the
 * animation is right. His client is then handed sixty points a second and has nothing left to
 * guess, which is exactly the fluidity a player has for his own character.
 *
 * The real packets still go out, in order, each at its own delayed moment - a stop, a jump, a
 * change of direction must arrive as the client expects them, not be reconstructed. The
 * synthetic ones fill the gaps between two of them and stop when there is no later report to
 * aim at: then the viewer's client extrapolates from the last real one, exactly as it does
 * today, and the worst case is what he had before rather than something new.
 *
 * Everything here runs on the map's own update thread. A packet is pushed from the relay of
 * another player on the same map, and the feed is advanced from the same sub-tick loop that
 * drains the movement queues, so there is no lock because there is no second thread.
 */
class WorldSession::SpectatorSmoother
{
    public:
        struct Sample
        {
            uint32 at;                          // the relay's stamp, taken as the packet arrived
            MovementInfo info;
            WorldPacket raw;                    // sent as it was, when its moment comes
            bool sent;
        };

        struct Track
        {
            ObjectGuid guid;
            std::deque<Sample> samples;

            /*
             * THE LAST POINT THE VIEWER WAS HANDED, and how far it stands from the line the
             * reports describe. When a report is released and the viewer's man is not where it
             * says - because the line was carried on through a gap and he went somewhere else -
             * the difference is not snapped away. It becomes an offset that every point sent
             * afterwards carries, fading to nothing over the next hundred and fifty
             * milliseconds, so he glides back onto the line instead of jumping to it.
             */
            bool     haveSent = false;
            Position sent;
            float    errX = 0.0f, errY = 0.0f, errZ = 0.0f, errO = 0.0f;
            uint32   fadedAt = 0;               // so the fade is by time, not by tick

            /*
             * How he was moving, from the last two reports that were at least forty
             * milliseconds apart - two closer than that are one moment reported twice, and a
             * speed read off them is noise. This is what carries the line on when the sender
             * goes quiet for longer than the feed runs behind.
             */
            bool     haveVel = false;
            float    velX = 0.0f, velY = 0.0f, velZ = 0.0f, velO = 0.0f;   // per millisecond
            uint32   lastReportAt = 0;
            Position lastReportPos;
        };

        // the opcodes HandleMovementOpcodes relays - a packed guid and a MovementInfo, nothing
        // else - and so the only ones that can be parsed here and re-timed
        static bool IsRelayedMovement(uint16 opcode)
        {
            switch (opcode)
            {
                case MSG_MOVE_START_FORWARD:
                case MSG_MOVE_START_BACKWARD:
                case MSG_MOVE_STOP:
                case MSG_MOVE_START_STRAFE_LEFT:
                case MSG_MOVE_START_STRAFE_RIGHT:
                case MSG_MOVE_STOP_STRAFE:
                case MSG_MOVE_JUMP:
                case MSG_MOVE_START_TURN_LEFT:
                case MSG_MOVE_START_TURN_RIGHT:
                case MSG_MOVE_STOP_TURN:
                case MSG_MOVE_START_PITCH_UP:
                case MSG_MOVE_START_PITCH_DOWN:
                case MSG_MOVE_STOP_PITCH:
                case MSG_MOVE_SET_RUN_MODE:
                case MSG_MOVE_SET_WALK_MODE:
                case MSG_MOVE_FALL_LAND:
                case MSG_MOVE_START_SWIM:
                case MSG_MOVE_STOP_SWIM:
                case MSG_MOVE_SET_FACING:
                case MSG_MOVE_SET_PITCH:
                case MSG_MOVE_HEARTBEAT:
                    return true;
                default:
                    return false;
            }
        }

        void Push(WorldSession& session, WorldPacket const& packet, uint32 now)
        {
            WorldPacket copy(packet);
            ObjectGuid guid;
            MovementInfo info;
            copy >> guid.ReadAsPacked();
            info.Read(copy);

            /*
             * Read stamps stime with the moment of THIS read and puts the number on the wire -
             * which the relay wrote from its own stime - into ctime. So ctime is the stamp the
             * network thread took as the sender's packet arrived, with the sender's real spacing
             * in it, and that is the time this sample is filed under.
             */
            // built in one go: WorldPacket can be copied into being but not assigned afterwards
            Sample sample{ info.ctime ? info.ctime : now, info, packet, false };

            Track& track = m_tracks[guid.GetRawValue()];
            track.guid = guid;

            if (!track.samples.empty())
            {
                Sample const& last = track.samples.back();

                /*
                 * EIGHT YARDS IN ONE STEP IS NOT A STEP. On foot a man covers three and a half
                 * yards between two heartbeats and there are no mounts in an arena, so anything
                 * further is a charge, a blink, a teleport - something the client was told about
                 * separately and moves him along by itself. Drawing a line from where he was
                 * would walk him across the arena at a run, and the reports still waiting here
                 * describe a road he has been lifted off. They are dropped, and the track starts
                 * again from this report. Eight and not twenty: Blink is exactly twenty.
                 */
                float const dx = info.pos.x - last.info.pos.x;
                float const dy = info.pos.y - last.info.pos.y;
                float const dz = info.pos.z - last.info.pos.z;
                if (dx * dx + dy * dy + dz * dz > 64.0f)
                {
                    track.samples.clear();
                    track.haveSent = false;     // the viewer holds a point of the client's own now
                    track.haveVel = false;
                    track.lastReportAt = 0;
                    track.errX = track.errY = track.errZ = track.errO = 0.0f;
                }
                else if (sample.at < last.at)
                    sample.at = last.at;        // never backwards; the client would not know what to do with it
            }

            track.samples.push_back(sample);
        }

        /*
         * A SPLINE, A KNOCKBACK OR A TELEPORT OVERTAKES THE REPORTS. From that packet on the
         * viewer's client moves the man by itself - along a charge, through a knockback's arc,
         * to wherever he was put - and the reports gathered before it describe a road he has
         * just been lifted off. It is delivered at its delayed moment like everything else, so
         * what is dropped here is exactly what would otherwise have been drawn on top of it;
         * the first report from after it starts the track again, exactly.
         */
        void NoticeServerMove(WorldPacket const& packet)
        {
            uint16 const opcode = packet.GetOpcode();
            if (opcode != SMSG_MONSTER_MOVE && opcode != MSG_MOVE_KNOCK_BACK && opcode != MSG_MOVE_TELEPORT)
                return;

            try
            {
                WorldPacket copy(packet);
                ObjectGuid guid;
                copy >> guid.ReadAsPacked();
                m_tracks.erase(guid.GetRawValue());
            }
            catch (ByteBufferException const&)
            {
                // not a packet of the shape this expects; then it moved nobody this feed knows
            }
        }

        void Update(WorldSession& session, uint32 now, uint32 delay)
        {
            if (now < delay)
                return;
            uint32 const renderAt = now - delay;

            for (auto itr = m_tracks.begin(); itr != m_tracks.end();)
            {
                Track& track = itr->second;
                std::deque<Sample>& samples = track.samples;

                /*
                 * THE OFFSET FADES BY TIME before anything else happens this tick, so a report
                 * released below goes out with what is left of it rather than with all of it.
                 */
                if (track.haveSent)
                {
                    float const keep = 1.0f - std::min(1.0f, float(WorldTimer::getMSTimeDiff(track.fadedAt, now)) / FADE_MS);
                    track.errX *= keep;
                    track.errY *= keep;
                    track.errZ *= keep;
                    track.errO *= keep;
                }
                track.fadedAt = now;

                // 1. every real report whose moment has come goes out, in order
                for (Sample& s : samples)
                {
                    if (s.sent || s.at > renderAt)
                        continue;
                    s.sent = true;

                    // how he was moving, for the gaps - see the Track
                    if (track.lastReportAt && s.at >= track.lastReportAt + 40)
                    {
                        float const dt = float(s.at - track.lastReportAt);
                        track.velX = (s.info.pos.x - track.lastReportPos.x) / dt;
                        track.velY = (s.info.pos.y - track.lastReportPos.y) / dt;
                        track.velZ = (s.info.pos.z - track.lastReportPos.z) / dt;
                        track.velO = ShortestArc(track.lastReportPos.o, s.info.pos.o) / dt;
                        track.haveVel = true;
                    }
                    track.lastReportAt = s.at;
                    track.lastReportPos = s.info.pos;

                    bool const airborne = s.info.HasMovementFlag(MOVEFLAG_JUMPING | MOVEFLAG_FALLINGFAR);
                    bool const moving = s.info.HasMovementFlag(MOVEFLAG_MASK_MOVING);
                    if (!moving)
                        track.haveVel = false;

                    /*
                     * AS IT WAS, unless the viewer's man is somewhere else. Between two reports
                     * the line ends exactly on the second one, so this is normally a copy of the
                     * original bytes. After a gap it is not: the line was carried on past the
                     * last report and this one says where he really went, and snapping to it is
                     * the step this whole thing exists to remove - so the difference becomes the
                     * offset, and this report goes out from where the viewer already has him.
                     *
                     * A stop is sent exactly. It is the one moment a small correction reads as
                     * natural, and a man left standing a hand's breadth from his true spot until
                     * he next moves would be the worse of the two. So is a jump, whose arc the
                     * client flies by itself from this very packet, and anything on a transport.
                     */
                    if (!track.haveSent || airborne || !moving || s.info.HasMovementFlag(MOVEFLAG_ONTRANSPORT))
                    {
                        session.SendPacketNow(&s.raw);
                        track.errX = track.errY = track.errZ = track.errO = 0.0f;
                        track.sent = s.info.pos;
                        track.haveSent = !airborne;
                        continue;
                    }

                    track.errX = track.sent.x - s.info.pos.x;
                    track.errY = track.sent.y - s.info.pos.y;
                    track.errZ = track.sent.z - s.info.pos.z;
                    track.errO = ShortestArc(s.info.pos.o, track.sent.o);

                    MovementInfo shifted = s.info;
                    shifted.stime = s.at;
                    Send(session, track, s.raw.GetOpcode(), shifted);
                }

                // 2. the last one that went out, and the first one still waiting
                Sample const* from = nullptr;
                Sample const* to = nullptr;
                for (Sample const& s : samples)
                {
                    if (s.sent)
                        from = &s;
                    else
                    {
                        to = &s;
                        break;
                    }
                }

                /*
                 * 3. A POINT FOR THIS TICK. Between two reports it lies on the line between
                 * them. Past the last one with nothing newer in hand - the sender was quiet for
                 * longer than the feed runs behind - the line is carried on the way he was
                 * going, for up to half a second: that is his own client's heartbeat interval,
                 * and a man quiet for longer than that has stopped and said so. That was the
                 * last of the steps: the client carries a position on by itself but never a
                 * mouse turn, so a facing used to freeze through every such gap and jump at the
                 * next report. The turn is halved unless a turn key is held - a mouse that was
                 * turning is as likely to have stopped as to have gone on, and half an overshoot
                 * fades better than a whole one; a key held turns at a fixed rate the client
                 * matches exactly, and halving that would fight it.
                 *
                 * AND NOT WHILE HE IS IN THE AIR. A jump is a parabola the viewer's client flies
                 * by itself from the one JUMP packet; a heartbeat every sixteen milliseconds with
                 * the jumping flag still set would have it restart that flight from each new
                 * point, and a player in a fight jumps all the time. The real packets still go
                 * out on time, so he leaves the ground and lands exactly when he did - only the
                 * arc is left alone.
                 */
                bool haveTarget = false;
                Position target;
                if (from && renderAt > from->at &&
                    !from->info.HasMovementFlag(MOVEFLAG_JUMPING | MOVEFLAG_FALLINGFAR) &&
                    !from->info.HasMovementFlag(MOVEFLAG_ONTRANSPORT))
                {
                    Position const& a = from->info.pos;
                    if (to && to->at > from->at)
                    {
                        ArcPoint(a, to->info.pos,
                                 float(renderAt - from->at) / float(to->at - from->at), target);
                        haveTarget = true;
                    }
                    else if (!to && track.haveVel && from->info.HasMovementFlag(MOVEFLAG_MASK_MOVING) &&
                             renderAt - from->at <= sWorld.getConfig(CONFIG_UINT32_ARENA_SPECTATOR_CARRY_ON))
                    {
                        float const hole = float(renderAt - from->at);
                        float const turn = from->info.HasMovementFlag(MOVEFLAG_TURN_LEFT | MOVEFLAG_TURN_RIGHT) ? 1.0f : 0.5f;
                        target.x = a.x + track.velX * hole;
                        target.y = a.y + track.velY * hole;
                        target.z = a.z + track.velZ * hole;
                        target.o = NormalizeO(a.o + track.velO * turn * hole);
                        haveTarget = true;
                    }
                }

                if (haveTarget)
                {
                    MovementInfo mid = from->info;
                    mid.pos = target;
                    mid.stime = renderAt;

                    /*
                     * A DIAGNOSTIC, AND IT BREAKS THE RUN ANIMATION ON PURPOSE. Never leave it on.
                     *
                     * The heartbeat above carries the flags of the report it was built from -
                     * FORWARD, STRAFE_LEFT, whatever the man was doing. The client does not treat
                     * those as decoration: it puts the unit where the packet says and then DEAD
                     * RECKONS him onward in the flag direction at his own speed until the next one
                     * arrives. Our interpolated line and that dead reckoning are not the same
                     * curve, so every heartbeat lands a small correction - and the client applies
                     * corrections by SNAPPING. Forty a second of two or three centimetres is not
                     * stutter any more, but it is not smooth either, and no packet rate fixes it:
                     * a higher rate only raises the frequency of the buzz.
                     *
                     * Stripping the flags stops the dead reckoning, so the man sits exactly where
                     * the feed puts him and nowhere else. IF THAT THEORY IS RIGHT the buzz turns
                     * into clean stepping at the send rate; if something else is wrong the picture
                     * changes in some other way, and days of client reverse engineering were about
                     * to be spent on the wrong cause. That is the whole reason this exists.
                     *
                     * The animation is the price: the client picks run, walk or stand from these
                     * same flags, so with them gone he slides along standing still.
                     */
                    if (sWorld.getConfig(CONFIG_BOOL_ARENA_SPECTATOR_NO_EXTRAPOLATION))
                        mid.RemoveMovementFlag(MOVEFLAG_MASK_MOVING_OR_TURN);

                    Send(session, track, MSG_MOVE_HEARTBEAT, mid);
                }

                /*
                 * 4. What is behind is let go of - but never the last one that went out, which
                 * is the "from" of the next interpolation - and a man nobody has heard from for
                 * five seconds has left, stopped, or died, and his track goes with him.
                 */
                while (samples.size() > 1 && samples[0].sent && samples[1].sent)
                    samples.pop_front();

                if (!samples.empty() && WorldTimer::getMSTimeDiff(samples.back().at, now) > 5000)
                    itr = m_tracks.erase(itr);
                else
                    ++itr;
            }
        }

    private:
        static constexpr float FADE_MS = 150.0f;        // an offset is gone after this long
        // how far the feed may guess past its newest report is Arena.Spectator.CarryOn, whose
        // default is the sender's own heartbeat interval - the longest a moving man can be quiet

        static float NormalizeO(float o)
        {
            float const twoPi = 6.28318530717959f;
            o = std::fmod(o, twoPi);
            if (o < 0.0f)
                o += twoPi;
            return o;
        }

        // the signed shortest way round from a to b, in (-pi, pi]
        static float ShortestArc(float a, float b)
        {
            float const twoPi = 6.28318530717959f;
            float const pi = 3.14159265358979f;
            return std::fmod(b - a + 3.0f * pi, twoPi) - pi;
        }

        // the fraction f of the shortest way round from a to b
        static float LerpOrientation(float a, float b, float f)
        {
            return NormalizeO(a + ShortestArc(a, b) * f);
        }

        /*
         * THE POINT AT FRACTION f BETWEEN TWO REPORTS, ALONG THE PATH HE ACTUALLY RAN.
         *
         * A straight line between them was the obvious thing and it is visibly wrong. A man
         * running a curve is reported at two points on it, and the line joining those two points
         * is the CHORD - it cuts the corner. Worse, the facing is turned smoothly across that
         * chord, so the angle between where he looks and where he travels drifts the whole way
         * across. That angle is fixed in the real game: strafing left is ninety degrees off the
         * facing and stays there. Break it and he stops looking steered and starts looking like
         * he is sliding sideways down a slope - which is exactly what it looked like.
         *
         * SO THE CURVE IS DRAWN FROM THE FACING INSTEAD. A cubic Hermite leaves a in the
         * direction he was travelling there and arrives at b in the direction he is travelling
         * there, and lands exactly on both, so it needs no correction afterwards.
         *
         * WHICH WAY HE TRAVELS RELATIVE TO WHERE HE LOOKS IS READ OFF THE PAIR, not off the
         * movement flags. For a circular arc the chord is parallel to the tangent at the
         * midpoint, so the difference between the chord's heading and the facing halfway along
         * IS that angle - and reading it this way costs no flag table, covers every combination
         * of forward, back and strafe at once, and stays right when a snare or a buff changes
         * his speed.
         *
         * The tangent length is the one that makes a cubic match a circular arc: 4R*tan(t/4),
         * with R from the chord and the turn. It falls out to the chord length as the turn goes
         * to zero, so a man running straight gets the straight line he had before and this costs
         * him nothing.
         *
         * Height stays linear. A curve in the vertical would be guessing at terrain, and the
         * ground under an arena is flat enough that the guess could only be wrong.
         */
        static void ArcPoint(Position const& a, Position const& b, float f, Position& out)
        {
            out.z = a.z + (b.z - a.z) * f;
            out.o = LerpOrientation(a.o, b.o, f);

            float const dx = b.x - a.x;
            float const dy = b.y - a.y;
            float const L = std::sqrt(dx * dx + dy * dy);

            // standing, or turning on the spot: there is no curve, and atan2 on this would be
            // reading a heading out of rounding noise
            if (L < 0.05f)
            {
                out.x = a.x + dx * f;
                out.y = a.y + dy * f;
                return;
            }

            float const offset = ShortestArc(LerpOrientation(a.o, b.o, 0.5f), std::atan2(dy, dx));
            float const dirA = a.o + offset;
            float const dirB = b.o + offset;

            float const turn = std::fabs(ShortestArc(dirA, dirB));
            float m = L;
            if (turn > 0.03f)
                m = 2.0f * L * std::tan(turn * 0.25f) / std::sin(turn * 0.5f);

            float const t2 = f * f;
            float const t3 = t2 * f;
            float const h00 =  2.0f * t3 - 3.0f * t2 + 1.0f;
            float const h10 =         t3 - 2.0f * t2 + f;
            float const h01 = -2.0f * t3 + 3.0f * t2;
            float const h11 =         t3 -        t2;

            out.x = h00 * a.x + h10 * (m * std::cos(dirA)) + h01 * b.x + h11 * (m * std::cos(dirB));
            out.y = h00 * a.y + h10 * (m * std::sin(dirA)) + h01 * b.y + h11 * (m * std::sin(dirB));
        }

        /*
         * Every point goes out through here, carrying what is left of the offset, and is
         * remembered as the point the viewer now holds - which is what the next report is
         * measured against.
         */
        static void Send(WorldSession& session, Track& track, uint16 opcode, MovementInfo info)
        {
            info.pos.x += track.errX;
            info.pos.y += track.errY;
            info.pos.z += track.errZ;
            info.pos.o = NormalizeO(info.pos.o + track.errO);

            WorldPacket data(opcode, 40);
            data << track.guid.WriteAsPacked();
            info.Write(data);
            session.SendPacketNow(&data);   // already on the delayed timeline: never held again

            track.sent = info.pos;
            track.haveSent = true;
        }

        std::unordered_map<uint64, Track> m_tracks;
};

void WorldSession::UpdateSpectatorSmoothing(uint32 now)
{
    uint32 const delay = sWorld.getConfig(CONFIG_UINT32_ARENA_SPECTATOR_SMOOTH_DELAY);
    Player const* pViewer = GetPlayer();
    bool const behind = delay && m_socket && pViewer && pViewer->IsArenaVisitor();

    // what has fallen due goes out FIRST, so a spell lands on the man where the feed has put him
    FlushDelayedPackets(now, !behind);

    if (!m_spectatorSmoother)
        return;

    if (!behind)
    {
        m_spectatorSmoother.reset();            // whatever was still waiting is a match he has left
        return;
    }

    m_spectatorSmoother->Update(*this, now, delay);
}

bool MapSessionFilter::Process(std::unique_ptr<ClientPacket const> const& packet)
{
    OpcodeHandler const& opHandle = LookupOpcodeHandler(packet->GetOpcode());
    // let's check if our opcode can be really processed in Map::Update()
    return MapSessionFilterHelper(m_pSession, opHandle);
}

static uint32 g_sessionCounter = 1;

// WorldSession constructor
WorldSession::WorldSession(uint32 id, std::shared_ptr<WorldSocket> sock, AccountTypes sec, time_t mute_time, LocaleConstant locale) :
    m_guid(g_sessionCounter++), m_muteTime(mute_time), m_connected(true), m_disconnectTimer(0), m_who_recvd(false), m_ah_list_recvd(false),
    m_accountFlags(0), m_idleTime(WorldTimer::getMSTime()), _player(nullptr), m_socket(sock), m_security(sec), m_accountId(id),
    m_exhaustionState(0), m_createTime(time(nullptr)), m_previousPlayTime(0), m_logoutTime(0), m_inQueue(false),
    m_playerLoading(false), m_playerLogout(false), m_playerRecentlyLogout(false), m_playerSave(false), m_sessionDbcLocale(sWorld.GetAvailableDbcLocale(locale)),
    m_sessionDbLocaleIndex(sObjectMgr.GetIndexForLocale(locale)), m_latency(0), m_tutorialState(TUTORIALDATA_UNCHANGED), m_warden(nullptr), m_cheatData(nullptr),
    m_bot(nullptr), m_clientOS(CLIENT_OS_UNKNOWN), m_clientPlatform(CLIENT_PLATFORM_UNKNOWN), m_gameBuild(0), m_verifiedEmail(true),
    m_charactersCount(std::numeric_limits<uint32>::max()), m_characterMaxLevel(0), m_lastPubChannelMsgTime(0), m_moveRejectTime(0), m_masterPlayer(nullptr), m_receivedPacketType{},
    m_floodPacketsCount{}, m_tutorials{}
{
    m_remoteIpAddress = sock ? sock->GetRemoteIpString() : "<BOT>";
}

// WorldSession destructor
WorldSession::~WorldSession()
{
    // unload player if not unloaded
    if (_player)
        LogoutPlayer(!m_bot || sPlayerBotMgr.IsSavingAllowed());

    // If have unclosed socket, close it
    if (m_socket)
    {
        m_socket->FinalizeSession();
        m_socket = nullptr; // <-- technically this is unnecessary, since we are in the destructor that will destruct all other members soon anyway
    }

    // empty incoming packet queue
    for (auto& i : m_recvQueue)
        i.clear();

    if (m_warden)
        sAnticheatMgr->RemoveWardenSession(m_warden);

    delete m_cheatData;
}

// Get the player name
char const* WorldSession::GetPlayerName() const
{
    return GetPlayer() ? GetPlayer()->GetName() : "<none>";
}

// Sends a packet to the client.
void WorldSession::SendPacket(std::unique_ptr<ServerPacket const> packet)
{
    WorldPacket buffer;
    { // TODO: This part will be offloaded to an IO thread soon. Only the IO thread will allocate a buffer.
        buffer.SetOpcode(packet->GetOpcode());
        buffer.FillPacketTime(WorldTimer::getMSTime());
        packet->AppendBodyTo(buffer);
    }

    // There is a maximum size packet.
    if (buffer.size() > 0x8000)
    {
        // Packet will be rejected by client
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[NETWORK] Packet %s size %u is too large. Not sent [Account %u Player %s]", LookupOpcodeName(buffer.GetOpcode()), buffer.size(), GetAccountId(), GetPlayerName());
        return;
    }

    if (!m_socket)
    {
        if (GetBot() && GetBot()->ai)
            GetBot()->ai->OnPacketReceived(&buffer); // TODO Direct forward `ServerPacket` to bot in next PR
        return;
    }

    SendPacketImpl(&buffer); // TODO Queue `ServerPacket` and serialize it in IO thread
}

// Send a packet to the client
void WorldSession::SendPacket(WorldPacket const* packet)
{
    // There is a maximum size packet.
    if (packet->size() > 0x8000)
    {
        // Packet will be rejected by client
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[NETWORK] Packet %s size %u is too large. Not sent [Account %u Player %s]", LookupOpcodeName(packet->GetOpcode()), packet->size(), GetAccountId(), GetPlayerName());
        return;
    }

    if (!m_socket)
    {
        if (GetBot() && GetBot()->ai)
            GetBot()->ai->OnPacketReceived(packet);
        return;
    }

    SendPacketImpl(packet);
}

void WorldSession::SendPacketImpl(WorldPacket const* packet)
{
#ifdef _DEBUG

    // Code for network use statistic
    static uint64 sendPacketCount = 0;
    static uint64 sendPacketBytes = 0;

    static time_t firstTime = time(nullptr);
    static time_t lastTime = firstTime;                     // next 60 secs start time

    static uint64 sendLastPacketCount = 0;
    static uint64 sendLastPacketBytes = 0;

    time_t cur_time = time(nullptr);

    if ((cur_time - lastTime) < 60)
    {
        sendPacketCount += 1;
        sendPacketBytes += packet->size();

        sendLastPacketCount += 1;
        sendLastPacketBytes += packet->size();
    }
    else
    {
        uint64 minTime = uint64(cur_time - lastTime);
        uint64 fullTime = uint64(lastTime - firstTime);
        sLog.Out(LOG_BASIC, LOG_LVL_DETAIL, "Send all time packets count: " UI64FMTD " bytes: " UI64FMTD " avr.count/sec: %f avr.bytes/sec: %f time: %u", sendPacketCount, sendPacketBytes, float(sendPacketCount) / fullTime, float(sendPacketBytes) / fullTime, uint32(fullTime));
        sLog.Out(LOG_BASIC, LOG_LVL_DETAIL, "Send last min packets count: " UI64FMTD " bytes: " UI64FMTD " avr.count/sec: %f avr.bytes/sec: %f", sendLastPacketCount, sendLastPacketBytes, float(sendLastPacketCount) / minTime, float(sendLastPacketBytes) / minTime);

        lastTime = cur_time;
        sendLastPacketCount = 1;
        sendLastPacketBytes = packet->wpos();               // wpos is real written size
    }

#endif // _DEBUG

    // sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[%s]Send packet : %u|0x%x (%s)", GetPlayerName(), packet->GetOpcode(), packet->GetOpcode(), LookupOpcodeName(packet->GetOpcode()));
    /*
     * A VISITOR'S WHOLE STREAM RUNS BEHIND, not only his movement.
     *
     * The movement feed holds a fighter's reports back so that it can draw the line between
     * two of them, and that is the right thing for the movement - but a spell cast, a hit, a
     * health bar and a chat line were still arriving the instant they happened, so a blow landed
     * on a man who was visibly still a quarter of a second short of where it hit him, and a
     * cast bar filled before the caster had arrived at the spot he cast from. Every packet
     * this session sends is held for the same delay instead, in the order it was sent, and the
     * match arrives whole and a little late - which is what any broadcast of a match is.
     *
     * Only a VISITOR, only while the delay is on, and nothing about the connection itself: the
     * ping is answered from the socket and never comes through here, and everything that sets a
     * session up happened before he was a visitor. The two things that must not be held again
     * - the movement feed's own points, which are already on the delayed timeline, and this
     * queue's flush - go through SendPacketNow beneath.
     */
    if (Player const* pViewer = GetPlayer())
        if (pViewer->IsArenaVisitor())
            if (uint32 const delay = sWorld.getConfig(CONFIG_UINT32_ARENA_SPECTATOR_SMOOTH_DELAY))
            {
                std::lock_guard<std::mutex> guard(m_delayedLock);
                m_delayed.emplace_back(WorldTimer::getMSTime() + delay, *packet);
                return;
            }

    SendPacketNow(packet);
}

void WorldSession::SendPacketNow(WorldPacket const* packet)
{
    if (!m_socket)
        return;

    if (m_sniffFile)
        m_sniffFile->WritePacket(*packet, false, time(nullptr));

    m_socket->SendPacket(*packet);
}

/*
 * The whole flush happens under the lock, sends included. A send is only a push onto the
 * socket's own queue, and holding the lock across it is what keeps two threads from each
 * taking a packet and then sending them the wrong way round: the map thread drains this every
 * sub-tick while he is a visitor, and the world thread takes over - all of it, at once - the
 * moment he is not.
 */
void WorldSession::FlushDelayedPackets(uint32 now, bool everything)
{
    std::lock_guard<std::mutex> guard(m_delayedLock);
    while (!m_delayed.empty() && (everything || int32(now - m_delayed.front().first) >= 0))
    {
        WorldPacket const& packet = m_delayed.front().second;

        // a spline, a knockback or a teleport delivered now overtakes the movement feed's
        // reports of the man it moved - only on the map thread, which is the feed's own
        if (!everything && m_spectatorSmoother)
            m_spectatorSmoother->NoticeServerMove(packet);

        SendPacketNow(&packet);
        m_delayed.pop_front();
    }
}

void WorldSession::VerifyPacketWasCorrectlyRead(WorldPacket const& recvPacket, ClientPacket const& clientPacket)
{
    if (clientPacket.GetOpcode() != recvPacket.GetOpcode())
    {
        sLog.Out(LOG_NETWORK, LOG_LVL_ERROR, "[NicePacket Conversion] Received %d (%s) but after parse it was %d", recvPacket.GetOpcode(), LookupOpcodeName(recvPacket.GetOpcode()), clientPacket.GetOpcode());
    }
    if (recvPacket.rpos() != recvPacket.size())
    {
        sLog.Out(LOG_NETWORK, LOG_LVL_ERROR, "[NicePacket Conversion] Packet is size %d but only parsed %d (opcode %d %s)", recvPacket.size(), recvPacket.rpos(), recvPacket.GetOpcode(), LookupOpcodeName(recvPacket.GetOpcode()));
    }
}

#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_7_1
void WorldSession::SendMovementPacket(WorldPacket const* packet)
{
    // There is a maximum size packet.
    if (packet->size() > 0x8000)
    {
        // Packet will be rejected by client
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[NETWORK] Packet %s size %u is too large. Not sent [Account %u Player %s]", LookupOpcodeName(packet->GetOpcode()), packet->size(), GetAccountId(), GetPlayerName());
        return;
    }

    if (!m_socket)
    {
        if (GetBot() && GetBot()->ai)
            GetBot()->ai->OnPacketReceived(packet);
        return;
    }

    /*
     * AN ARENA SPECTATOR IS NEVER COMPRESSED, and he is the one case where this protection does
     * exactly the wrong thing.
     *
     * Compression exists for mass PvP: past Compression.Movement.Count packets in a ten second
     * window - three hundred, so thirty a second - a session stops sending movement as it
     * arrives and starts buffering it into a block that goes out once per world update. In a
     * forty on forty Alterac Valley that is the difference between a playable server and a dead
     * one.
     *
     * A spectator blows through that threshold by simply existing. He receives the movement of
     * every fighter, and a 3v3 where people steer with the mouse is well over a hundred packets
     * a second on its own - so he is permanently in the buffered path, and what he watches
     * arrives in lumps.
     *
     * IT SHOWS UP AS "MOUSE TURNING STUTTERS, KEYBOARD TURNING DOES NOT", which sounds like a
     * client bug and is not. Turning with the keyboard is TWO packets, a start and a stop, and
     * the viewer's own client interpolates the whole sweep between them. Turning with the mouse
     * is a stream of absolute facings at the sender's framerate, and a stream is exactly what
     * buffering destroys. Mind Control looks the same for the same reason.
     *
     * The exemption is safe precisely because of what a spectator is: at most ten fighters and
     * their pets on one small map, which is the load compression was never meant for. It is
     * asked of IsArenaSpectator, set only for somebody who came in through the orb as a visitor.
     */
    if (Player const* pViewer = GetPlayer())
    {
        if (pViewer->IsArenaSpectator())
        {
            /*
             * AND A VISITOR'S MOVEMENT IS HELD BACK AND INTERPOLATED - see SpectatorSmoother
             * above. Only a visitor: a dead fighter is a spectator too, and he is shown the match
             * as it is, since nothing about him was built to run behind it.
             */
            if (uint32 const delay = sWorld.getConfig(CONFIG_UINT32_ARENA_SPECTATOR_SMOOTH_DELAY))
            {
                if (pViewer->IsArenaVisitor() && SpectatorSmoother::IsRelayedMovement(packet->GetOpcode()))
                {
                    if (!m_spectatorSmoother)
                        m_spectatorSmoother = std::make_unique<SpectatorSmoother>();
                    m_spectatorSmoother->Push(*this, *packet, WorldTimer::getMSTime());
                    return;
                }
            }

            SendPacketImpl(packet);
            return;
        }
    }

    if (++m_movePacketsSentThisInterval < sWorld.getConfig(CONFIG_UINT32_COMPRESSION_MOVEMENT_COUNT) &&
        m_movePacketsSentLastInterval < sWorld.getConfig(CONFIG_UINT32_COMPRESSION_MOVEMENT_COUNT))
    {
        SendPacketImpl(packet);
        return;
    }

    std::lock_guard<std::mutex> guard(m_movementPacketCompressorMutex);
    if (m_movementPacketCompressor.CanAddPacket(*packet))
        m_movementPacketCompressor.AddPacket(*packet);
    else
    {
        // send batched packets first to maintain order of packets
        SendCompressedMovementPackets();
        SendPacketImpl(packet);
    }
}

void WorldSession::SendCompressedMovementPackets()
{
    if (m_movementPacketCompressor.HasData())
    {
        WorldPacket packet;
        if (m_movementPacketCompressor.BuildPacket(packet))
            SendPacket(&packet);
        else
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "Movement packet compression failed! Packets lost!");
        m_movementPacketCompressor.ClearBuffer();
    }
}
#else
void WorldSession::SendMovementPacket(WorldPacket const* packet)
{
    SendPacket(packet);
}
#endif

PacketProcessing GetChatPacketProcessingType(uint32 chatType)
{
    switch (chatType)
    {
        // These can be handled at any time session update in world thread is not running.
        case CHAT_MSG_CHANNEL:
        case CHAT_MSG_WHISPER:
        case CHAT_MSG_PARTY:
        case CHAT_MSG_GUILD:
        case CHAT_MSG_OFFICER:
        case CHAT_MSG_RAID:
#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_10_2
        case CHAT_MSG_RAID_LEADER:
        case CHAT_MSG_RAID_WARNING:
#endif
#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_11_2
        case CHAT_MSG_BATTLEGROUND:
        case CHAT_MSG_BATTLEGROUND_LEADER:
#endif
        case CHAT_MSG_DND:
            return PACKET_PROCESS_ASYNC;
        // These can be handled on the map thread.
        case CHAT_MSG_SAY:
        case CHAT_MSG_EMOTE:
        case CHAT_MSG_YELL:
            return PACKET_PROCESS_MAP;

        default:
            return PACKET_PROCESS_WORLD;
    }
}

void WorldSession::QueuePacket(std::unique_ptr<ClientPacket const> packet)
{
    // Can't sniff non-binary packets :( So bots will not appear in the packet log

    OpcodeHandler const& opHandle = LookupOpcodeHandler(packet->GetOpcode());
    MANGOS_ASSERT(opHandle.impl.has_value()); // How can you call `QueuePacket` with incorrect opcode? You invoked QueuePacket manually with a wrong packet!

    PacketProcessing processingStrategy = opHandle.impl->packetProcessing;

    // Handle chat packets on async thread when possible
    if (packet->GetOpcode() == CMSG_MESSAGECHAT)
    {
        auto const* chatMessage = dynamic_cast<WorldPackets::Chat::ChatMessage const*>(packet.get());
        MANGOS_ASSERT(chatMessage); // Should never happen if it's coming from `QueueBinaryPacket`
        processingStrategy = GetChatPacketProcessingType(chatMessage->type);
    }

    if (processingStrategy >= PACKET_PROCESS_MAX_TYPE)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "SESSION: opcode %s (0x%.4X) will be skipped", opHandle.name, packet->GetOpcode());
        return;
    }

    // put into the correct processing queue
    m_recvQueue[processingStrategy].add(std::move(packet));
}

// Add an incoming packet to the queue
void WorldSession::QueueBinaryPacket(std::unique_ptr<WorldPacket> const& binaryPacket)
{
    if (m_sniffFile)
        m_sniffFile->WritePacket(*binaryPacket, true, time(nullptr));

    if (_player && MovementAnticheat::IsLoggedOpcode(binaryPacket->GetOpcode()))
        GetCheatData()->LogMovementPacket(true, *binaryPacket);

    OpcodeHandler const& opHandle = LookupOpcodeHandler(binaryPacket->GetOpcode());
    if (!opHandle.impl.has_value()) // check if an unhandled packet
    {
        if (m_socket)
        {
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "[%s] Received unhandled opcode %s (0x%.4X) will be skipped", m_socket->GetRemoteIpString().c_str(), opHandle.name, binaryPacket->GetOpcode());
        }
        else
        {
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "[SESSION] Received unhandled opcode %s (0x%.4X) will be skipped", opHandle.name, binaryPacket->GetOpcode());
        }
        return;
    }

    // parsing the packet
    std::unique_ptr<ClientPacket const> clientPacket = opHandle.impl->readPacket(*binaryPacket);
    VerifyPacketWasCorrectlyRead(*binaryPacket, *clientPacket);

    QueuePacket(std::move(clientPacket));
}

// Logging helper for unexpected opcodes
void WorldSession::LogUnexpectedOpcode(ClientPacket const& packet, std::string const& reason)
{
    uint16 opcode = packet.GetOpcode();
    char const* opcodeName = LookupOpcodeName(opcode);
    sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "SESSION: received unexpected opcode %s (0x%.4X) %s", opcodeName, opcode, reason.c_str());
}

bool WorldSession::HasTrialRestrictions() const
{
    return !HasVerifiedEmail() && GetSecurity() <= SEC_PLAYER && sWorld.getConfig(CONFIG_BOOL_RESTRICT_UNVERIFIED_ACCOUNTS);
}

void WorldSession::CheckPlayedTimeLimit(time_t now)
{
#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_7_1
    time_t const currentPlayed = GetConsecutivePlayTime(now);

    if (currentPlayed >= PLAY_TIME_LIMIT_FULL)
    {
        if (m_exhaustionState < PLAY_TIME_LIMIT_FULL)
        {
            SendPlayTimeWarning(PTF_UNHEALTHY_TIME, 0);
            GetPlayer()->SetFlag(PLAYER_FLAGS, PLAYER_FLAGS_NO_PLAY_TIME);
            GetPlayer()->RemoveFlag(PLAYER_FLAGS, PLAYER_FLAGS_PARTIAL_PLAY_TIME);
            m_exhaustionState = PLAY_TIME_LIMIT_FULL;
        }
    }
    else if (currentPlayed >= PLAY_TIME_LIMIT_APPROCHING_FULL)
    {
        if (m_exhaustionState < PLAY_TIME_LIMIT_APPROCHING_FULL)
        {
            SendPlayTimeWarning(PTF_APPROACHING_NO_PLAY_TIME, int32(PLAY_TIME_LIMIT_FULL - currentPlayed));
            GetPlayer()->SetFlag(PLAYER_FLAGS, PLAYER_FLAGS_PARTIAL_PLAY_TIME);
            GetPlayer()->RemoveFlag(PLAYER_FLAGS, PLAYER_FLAGS_NO_PLAY_TIME);
            m_exhaustionState = PLAY_TIME_LIMIT_APPROCHING_FULL;
        }
    }
    else if (currentPlayed >= PLAY_TIME_LIMIT_PARTIAL)
    {
        if (m_exhaustionState < PLAY_TIME_LIMIT_PARTIAL)
        {
            SendPlayTimeWarning(PTF_APPROACHING_NO_PLAY_TIME, int32(PLAY_TIME_LIMIT_FULL - currentPlayed));
            GetPlayer()->SetFlag(PLAYER_FLAGS, PLAYER_FLAGS_PARTIAL_PLAY_TIME);
            GetPlayer()->RemoveFlag(PLAYER_FLAGS, PLAYER_FLAGS_NO_PLAY_TIME);
            m_exhaustionState = PLAY_TIME_LIMIT_PARTIAL;
        }
    }
    else if (currentPlayed >= PLAY_TIME_LIMIT_APPROACHING_PARTIAL)
    {
        if (m_exhaustionState < PLAY_TIME_LIMIT_APPROACHING_PARTIAL)
        {
            SendPlayTimeWarning(PTF_APPROACHING_PARTIAL_PLAY_TIME, int32(PLAY_TIME_LIMIT_PARTIAL - currentPlayed));
            m_exhaustionState = PLAY_TIME_LIMIT_APPROACHING_PARTIAL;
        }
    }
#endif
}

void WorldSession::SendPlayTimeWarning(PlayTimeFlag flag, int32 timeLeftInSeconds)
{
#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_7_1
    auto packet = std::make_unique<WorldPackets::Misc::PlayTimeWarning>();
    packet->flag = static_cast<uint32>(flag);
    packet->timeLeftInSeconds = timeLeftInSeconds;
    SendPacket(std::move(packet));
#endif
}

bool WorldSession::ForcePlayerLogoutDelay()
{
    if (!sWorld.IsStopped() && GetPlayer() && GetPlayer()->FindMap() && GetPlayer()->IsInWorld())
    {
        if (GetBot())
        {
            GetPlayer()->RemoveFromGroup();
        }
        else if (sWorld.getConfig(CONFIG_BOOL_FORCE_LOGOUT_DELAY))
        {
            sLog.Player(this, LOG_CHAR, "LostSocket", LOG_LVL_BASIC, "");
            SetDisconnectedSession();
            m_disconnectTimer = 120000;
            GetPlayer()->OnDisconnected();
            GetPlayer()->SaveToDB();
            return true;
        }
    }
    return false;
}

// Update the WorldSession (triggered by World update)
bool WorldSession::Update(PacketFilter& updater)
{
    uint32 sessionUpdateTime = WorldTimer::getMSTime();
    for (uint32 & i : m_floodPacketsCount)
        i = 0;

    // Retrieve packets from the receive queue and call the appropriate handlers
    ProcessPackets(updater);

    /*
     * A VISITOR'S DELAYED STREAM IS DRAINED FROM HIS MAP'S SUB-TICK while he is one. The moment
     * he is not - ported home, logged out, the feature switched off - no map thread looks at
     * him any more, and whatever was still waiting would sit there for good, the port home
     * itself among it. So it is let go of here, all of it, in order.
     */
    {
        Player const* pViewer = GetPlayer();
        if (!(sWorld.getConfig(CONFIG_UINT32_ARENA_SPECTATOR_SMOOTH_DELAY) && pViewer && pViewer->IsArenaVisitor()))
            FlushDelayedPackets(WorldTimer::getMSTime(), true);
    }

    if (CharacterScreenIdleKick(sessionUpdateTime))
        return false;

    sessionUpdateTime = WorldTimer::getMSTimeDiffToNow(sessionUpdateTime);

    if (sWorld.getConfig(CONFIG_UINT32_PERFLOG_SLOW_UNIQUE_SESSION_UPDATE) && sessionUpdateTime > sWorld.getConfig(CONFIG_UINT32_PERFLOG_SLOW_UNIQUE_SESSION_UPDATE))
        sLog.Out(LOG_PERFORMANCE, LOG_LVL_MINIMAL, "Slow session update: %ums. Account %u on IP %s", sessionUpdateTime, GetAccountId(), GetRemoteAddress().c_str());

    //check if we are safe to proceed with logout
    //logout procedure should happen only in World::UpdateSessions() method!!!
    if (updater.ProcessLogout())
    {
        if (m_bot != nullptr && m_bot->state == PB_STATE_OFFLINE)
        {
            LogoutPlayer(sPlayerBotMgr.IsSavingAllowed());
            return false;
        }

        // Cleanup socket pointer if needed
        if (m_socket && m_socket->IsClosing())
        {
            m_socket->FinalizeSession();
            m_socket = nullptr;

            if (m_warden)
            {
                sAnticheatMgr->RemoveWardenSession(m_warden);
                m_warden = nullptr;
            }

            if (GetPlayer() && GetPlayer()->m_broadcaster)
                GetPlayer()->m_broadcaster->ChangeSocket(nullptr);

            // Character stays IG for 2 minutes
            return ForcePlayerLogoutDelay();
        }

        time_t const currTime = time(nullptr);

#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_7_1
        // send these out every world update
        SendCompressedMovementPackets();

        // only enable compression when there's a lot of movement around us
        if (m_movePacketTrackingIntervalStart + 10 < currTime)
        {
            m_movePacketTrackingIntervalStart = currTime;
            m_movePacketsSentLastInterval = m_movePacketsSentThisInterval;
            m_movePacketsSentThisInterval = 0;
        }
#endif

        if (sWorld.getConfig(CONFIG_BOOL_LIMIT_PLAY_TIME) &&
            GetPlayer() && GetPlayer()->IsInWorld())
            CheckPlayedTimeLimit(currTime);

        ///- If necessary, log the player out
        bool const forceConnection = !sWorld.IsStopped() && sPlayerBotMgr.ForceAccountConnection(this);

        if ((!m_socket || (ShouldLogOut(currTime) && !m_playerLoading)) && !forceConnection && m_bot == nullptr)
            LogoutPlayer(true);

        if (!m_socket && !forceConnection && this->m_bot == nullptr)
            return false;                                       //Will remove this session from the world session map
    }
    else // Async map based update
    {
        if (GetMasterPlayer() && GetPlayer())
        {
            GetMasterPlayer()->LoadPlayer(GetPlayer());
            GetMasterPlayer()->Update();
        }
        // else
        // TODO: Broadcast MasterPlayer update to Master
    }

    return true;
}

bool WorldSession::CanProcessPackets() const
{
    return ((m_socket && !m_socket->IsClosing()) || (_player && (m_bot || sPlayerBotMgr.IsChatBot(_player->GetGUIDLow()))));
}

void WorldSession::ProcessPackets(PacketFilter& updater)
{
    std::unique_ptr<ClientPacket const> packet;
    m_receivedPacketType[updater.PacketProcessType()] = false;
    while (CanProcessPackets() && m_recvQueue[updater.PacketProcessType()].next(packet, updater))
    {
        m_receivedPacketType[updater.PacketProcessType()] = true;
        if (!AllowPacket(packet->GetOpcode()))
            break;

        OpcodeHandler const& opHandle = LookupOpcodeHandler(packet->GetOpcode());
        MANGOS_ASSERT(opHandle.impl.has_value()); // Only queue packets with handler!
        OpcodeHandlerPacketImplDetails const& handlerDetails = opHandle.impl.value();

        try
        {
            uint32 packetTime = WorldTimer::getMSTime();
            switch (opHandle.impl->status)
            {
                case STATUS_LOGGEDIN:

                    if (!_player)
                    {
                        // skip STATUS_LOGGEDIN opcode unexpected errors if player logout sometime ago - this can be network lag delayed packets
                        if (!m_playerRecentlyLogout)
                            LogUnexpectedOpcode(*packet, "the player has not logged in yet");
                    }
                    else if (_player->IsInWorld())
                        ExecuteOpcode(handlerDetails, *packet);

                    // lag can cause STATUS_LOGGEDIN opcodes to arrive after the player started a transfer
                    break;
                case STATUS_LOGGEDIN_OR_RECENTLY_LOGGEDOUT:
                    if (!_player && !m_playerRecentlyLogout)
                        LogUnexpectedOpcode(*packet, "the player has not logged in yet and not recently logout");
                    else
                        // not expected _player or must checked in packet hanlder
                        ExecuteOpcode(handlerDetails, *packet);
                    break;
                case STATUS_TRANSFER:
                    if (!_player)
                        LogUnexpectedOpcode(*packet, "the player has not logged in yet");
                    else if (_player->IsInWorld())
                        LogUnexpectedOpcode(*packet, "the player is still in world");
                    else
                        ExecuteOpcode(handlerDetails, *packet);
                    break;
                case STATUS_AUTHED:
                    // prevent cheating with skip queue wait
                    if (m_inQueue)
                    {
                        LogUnexpectedOpcode(*packet, "the player is still in queue");
                        break;
                    }

                    // single from authed time opcodes send in to after logout time
                    // and before other STATUS_LOGGEDIN_OR_RECENTLY_LOGGOUT opcodes.
                    m_playerRecentlyLogout = false;

                    ExecuteOpcode(handlerDetails, *packet);
                    break;
                case STATUS_NEVER:
                    sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "SESSION: received not allowed opcode %s (0x%.4X)", opHandle.name, packet->GetOpcode());
                    break;
                case STATUS_UNHANDLED:
                    sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "SESSION: received not handled opcode %s (0x%.4X)", opHandle.name, packet->GetOpcode());
                    break;
                default:
                    sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "SESSION: received wrong-status-req opcode %s (0x%.4X)", opHandle.name, packet->GetOpcode());
                    break;
            }
            packetTime = WorldTimer::getMSTimeDiffToNow(packetTime);
            if (sWorld.getConfig(CONFIG_UINT32_PERFLOG_SLOW_PACKET) && packetTime > sWorld.getConfig(CONFIG_UINT32_PERFLOG_SLOW_PACKET))
                sLog.Out(LOG_PERFORMANCE, LOG_LVL_MINIMAL, "Slow packet opcode %s: %ums. Account %u on IP %s", opHandle.name, packetTime, GetAccountId(), GetRemoteAddress().c_str());
        }
        catch (ByteBufferException &)
        {
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "WorldSession::Update ByteBufferException occured while parsing a packet (opcode:0x%x) from client %s, accountid=%i.", packet->GetOpcode(), GetRemoteAddress().c_str(), GetAccountId());
            if (sWorld.getConfig(CONFIG_BOOL_KICK_PLAYER_ON_BAD_PACKET))
            {
                sLog.Out(LOG_BASIC, LOG_LVL_DETAIL, "Disconnecting session [account id %u / address %s] for badly formatted packet.",
                           GetAccountId(), GetRemoteAddress().c_str());
                ProcessAnticheatAction("Anticrash", "ByteBufferException", CHEAT_ACTION_KICK);
            }
        }
        catch (std::runtime_error &e)
        {
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "CATCH Exception 'ASSERT' for account %u / IP %s", GetAccountId(), GetRemoteAddress().c_str());
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, e.what());
            ProcessAnticheatAction("Anticrash", "ASSERT failed", CHEAT_ACTION_KICK);
        }
        catch (...)
        {
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "CATCH Unknown exception. Account %u / IP %s", GetAccountId(), GetRemoteAddress().c_str());
            ProcessAnticheatAction("Anticrash", "Exception raised", CHEAT_ACTION_KICK);
        }
    }
}


void WorldSession::ClearIncomingPacketsByType(PacketProcessing type)
{
    ASSERT(type < PACKET_PROCESS_MAX_TYPE);
    m_recvQueue[type].clear();
}

void WorldSession::SetDisconnectedSession()
{
    m_connected = false;
    StopSniffing();
    sWorld.SetSessionDisconnected(this);
}

bool WorldSession::UpdateDisconnected(uint32 diff)
{
    ASSERT(!m_connected);
    if (!_player || !_player->IsInWorld() || !_player->FindMap())
        return false;
    if (m_disconnectTimer < diff)
        return false; // Delete this session
    m_disconnectTimer -= diff;
    return true;
}

// %Log the player out
void WorldSession::LogoutPlayer(bool Save)
{
    // finish pending transfers before starting the logout
    /* while(_player && _player->IsBeingTeleportedFar())
        HandleMoveWorldportAckOpcode(); */

    m_idleTime = WorldTimer::getMSTime();
    m_playerLogout = true;
    m_playerSave = Save;

    if (_player)
    {
        bool inWorld = _player->IsInWorld() && _player->FindMap();

        sLog.Player(this, LOG_CHAR, "Logout", LOG_LVL_DETAIL, "");

        if (ObjectGuid lootGuid = GetPlayer()->GetLootGuid())
            DoLootRelease(lootGuid);

        if (inWorld)
        {
            // If the player just died before logging out, make him appear as a ghost
            if (_player->GetDeathTimer())
            {
                _player->GetHostileRefManager().deleteReferences();
                _player->BuildPlayerRepop();
                _player->RepopAtGraveyard();
            }
            else if (_player->IsInCombat())
            {
                _player->CombatStop();
                _player->GetHostileRefManager().setOnlineOfflineState(false);
            }
            else if (_player->HasAuraType(SPELL_AURA_SPIRIT_OF_REDEMPTION))
            {
                // this will kill character by SPELL_AURA_SPIRIT_OF_REDEMPTION
                _player->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);
                //_player->SetDeathPvP(*); set at SPELL_AURA_SPIRIT_OF_REDEMPTION apply time
                _player->KillPlayer();
                _player->BuildPlayerRepop();
                _player->RepopAtGraveyard();
            }

            _player->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_LEAVE_WORLD_CANCELS);

        }


        if (_player->IsInLFG())
            sWorld.GetLFGQueue().GetMessager().AddMessage([playerGuid = _player->GetObjectGuid()](LFGQueue* queue)
        {
            queue->RemovePlayerFromQueue(playerGuid, PLAYER_SYSTEM_LEAVE);
        });

        // FG: finish pending transfers after starting the logout
        // this should fix players being able to logout and login back with full hp at death position
        while (_player->IsBeingTeleportedFar())
        {
            HandleMoveWorldportAck();
            sMapMgr.ExecuteSingleDelayedTeleport(_player); // Execute chain teleport if there are some
        }

        // drop the flag if player is carrying it
        if (BattleGround *bg = _player->GetBattleGround())
        {
            _player->LeaveBattleground(true);

            // check for teleports both before and after leaving bg
            // fixes exploit where you can be considered to be inside bg
            // while you are actually outside if you kill wow process on
            // loading screen during the teleport into bg when joining
            while (_player->IsBeingTeleportedFar())
            {
                HandleMoveWorldportAck();
                sMapMgr.ExecuteSingleDelayedTeleport(_player);
            }
        }

        // Refresh apres ca
        inWorld = _player->IsInWorld() && _player->FindMap();
        if (!inWorld)
        {
            Save = false;
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[CRASH] Player %s is not in world during logout.", _player->GetName());
        }

        sBattleGroundMgr.PlayerLoggedOut(_player);

        // Reset the online field in the account table
        // no point resetting online in character table here as Player::SaveToDB() will set it to 1 since player has not been removed from world at this stage
        // No SQL injection as AccountID is uint32
        static SqlStatementID id;

        SqlStatement stmt = LoginDatabase.CreateStatement(id, "UPDATE `account` SET `current_realm` = ?, `online` = 0 WHERE `id` = ?");
        stmt.PExecute(uint32(0), GetAccountId());

        // If the player is in a guild, update the guild roster and broadcast a logout message to other guild members
        if (Guild* guild = sGuildMgr.GetGuildById(_player->GetGuildId()))
        {
            if (MemberSlot* slot = guild->GetMemberSlot(_player->GetObjectGuid()))
            {
                slot->SetMemberStats(_player);
                slot->UpdateLogoutTime();
            }

            guild->BroadcastEvent(GE_SIGNED_OFF, _player->GetObjectGuid(), _player->GetName());
        }

        // Remove pet
        _player->RemovePet(PET_SAVE_AS_CURRENT);

        // Dungeon anti-exploit. Should be before save
        bool removedFromMap = false;
        if (Map* map = _player->FindMap())
        {
            if (map->IsNonRaidDungeon() && !_player->GetBoundInstanceSaveForSelfOrGroup(map->GetId()))
            {
                AreaTriggerTeleport const* at = sObjectMgr.GetGoBackTrigger(map->GetId());
                if (at)
                    removedFromMap = _player->TeleportTo(at->destination);
                else
                    removedFromMap = _player->TeleportToHomebind();

                sMapMgr.ExecuteSingleDelayedTeleport(_player);
            }
        }

        // empty buyback items and save the player in the database
        // some save parts only correctly work in case player present in map/player_lists (pets, etc)
        if (Save)
            _player->SaveToDB(false, removedFromMap);

        // Leave all channels before player delete...
        _player->CleanupChannels();

        // If the player is in a group (or invited), remove him. If the group if then only 1 person, disband the group.
        _player->UninviteFromGroup();

        // Send update to group
        if (Group* group = _player->GetGroup())
            group->UpdatePlayerOnlineStatus(_player, false);

        // Update cached data at logout
        sObjectMgr.UpdatePlayerCache(_player);

        // No need to create any new maps
        sMapMgr.CancelInstanceCreationForPlayer(_player);

        // Remove the player from the world
        // the player may not be in the world when logging out
        // e.g if he got disconnected during a transfer to another map
        // calls to GetMap in this case may cause crashes

        if (inWorld && !removedFromMap)
        {
            Map* _map = _player->GetMap();
            _map->Remove(_player, true);
        }
        else
        {
            _player->CleanupsBeforeDelete();
            Map::DeleteFromWorld(_player);
        }

#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_7_1
        m_movementPacketCompressor.ClearBuffer();
#endif

        SetPlayer(nullptr);                                    // deleted in Remove/DeleteFromWorld call

        // Send the 'logout complete' packet to the client
        SendPacket(std::make_unique<WorldPackets::Misc::LogoutComplete>());

        sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "SESSION: Sent SMSG_LOGOUT_COMPLETE Message");
    }

    if (m_masterPlayer)
    {
        // Broadcast a logout message to the player's friends
        if (m_masterPlayer->GetSocial())
        {
            sSocialMgr.SendFriendStatus(m_masterPlayer, FRIEND_OFFLINE, m_masterPlayer->GetObjectGuid(), true);
            sSocialMgr.RemovePlayerSocial(m_masterPlayer->GetGUIDLow());
            m_masterPlayer->SetSocial(nullptr);
        }

        m_masterPlayer->SaveToDB();
        delete m_masterPlayer;
        m_masterPlayer = nullptr;
    }

    m_clientMoverGuid.Clear();
    m_playerLogout = false;
    m_playerSave = false;
    m_playerRecentlyLogout = true;
    LogoutRequest(0);
}

// Kick a player out of the World
void WorldSession::KickPlayer()
{
    if (m_socket)
        m_socket->CloseSocket();
    else if (m_bot)
        m_bot->requestRemoval = true;
}

// Cancel channeling handler

void WorldSession::SendAreaTriggerMessage(char const* Text, ...)
{
    va_list ap;
    char szStr [1024];
    szStr[0] = '\0';

    va_start(ap, Text);
    vsnprintf(szStr, 1024, Text, ap);
    va_end(ap);

    uint32 length = strlen(szStr) + 1;
    WorldPacket data(SMSG_AREA_TRIGGER_MESSAGE, 4 + length);
    data << length;
    data << szStr;
    SendPacket(&data);
}

void WorldSession::SendNotification(char const* format, ...)
{
    if (format)
    {
        va_list ap;
        char szStr [1024];
        szStr[0] = '\0';
        va_start(ap, format);
        vsnprintf(szStr, 1024, format, ap);
        va_end(ap);

        WorldPacket data(SMSG_NOTIFICATION, (strlen(szStr) + 1));
        data << szStr;
        SendPacket(&data);
    }
}

void WorldSession::SendNotification(int32 string_id, ...)
{
    char const* format = GetMangosString(string_id);
    if (format)
    {
        va_list ap;
        char szStr [1024];
        szStr[0] = '\0';
        va_start(ap, string_id);
        vsnprintf(szStr, 1024, format, ap);
        va_end(ap);

        WorldPacket data(SMSG_NOTIFICATION, (strlen(szStr) + 1));
        data << szStr;
        SendPacket(&data);
    }
}

char const*  WorldSession::GetMangosString(int32 entry) const
{
    return sObjectMgr.GetMangosString(entry, GetSessionDbLocaleIndex());
}

void WorldSession::Handle_NULL(WorldPacket& recvPacket)
{
    sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "SESSION: received unimplemented opcode %s (0x%.4X)",
                  LookupOpcodeName(recvPacket.GetOpcode()),
                  recvPacket.GetOpcode());
}

void WorldSession::Handle_EarlyProccess(WorldPacket& recvPacket)
{
    sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "SESSION: received opcode %s (0x%.4X) that must be processed in WorldSocket::OnRead",
                  LookupOpcodeName(recvPacket.GetOpcode()),
                  recvPacket.GetOpcode());
}

void WorldSession::Handle_ServerSide(WorldPacket& recvPacket)
{
    sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "SESSION: received server-side opcode %s (0x%.4X)",
                  LookupOpcodeName(recvPacket.GetOpcode()),
                  recvPacket.GetOpcode());
}

void WorldSession::SendAuthWaitQue(uint32 position)
{
    if (position == 0)
    {
        WorldPacket packet(SMSG_AUTH_RESPONSE, 1);
        packet << uint8(AUTH_OK);
        SendPacket(&packet);
    }
    else
    {
        WorldPacket packet(SMSG_AUTH_RESPONSE, 5);
        packet << uint8(AUTH_WAIT_QUEUE);
        packet << uint32(position);
        SendPacket(&packet);
    }
}

void WorldSession::LoadGlobalAccountData()
{
    std::unique_ptr<QueryResult> result = CharacterDatabase.PQuery("SELECT `type`, `time`, `data` FROM `account_data` WHERE `account`=%u", GetAccountId());
    LoadAccountData(
        std::move(result),
        NewAccountData::GLOBAL_CACHE_MASK
    );
}

void WorldSession::LoadAccountData(std::unique_ptr<QueryResult> result, uint32 mask)
{
    for (uint32 i = 0; i < NewAccountData::NUM_ACCOUNT_DATA_TYPES; ++i)
        if (mask & (1 << i))
            m_accountData[i] = AccountData();

    if (!result)
        return;

    do
    {
        Field* fields = result->Fetch();

        uint32 type = fields[0].GetUInt32();
        if (type >= NewAccountData::NUM_ACCOUNT_DATA_TYPES)
        {
            sLog.Out(LOG_DBERROR, LOG_LVL_ERROR, "Table `%s` have invalid account data type (%u), ignore.",
                mask == NewAccountData::GLOBAL_CACHE_MASK ? "account_data" : "character_account_data", type);
            continue;
        }

        if ((mask & (1 << type)) == 0)
        {
            sLog.Out(LOG_DBERROR, LOG_LVL_ERROR, "Table `%s` have non appropriate for table  account data type (%u), ignore.",
                mask == NewAccountData::GLOBAL_CACHE_MASK ? "account_data" : "character_account_data", type);
            continue;
        }

        m_accountData[type].timestamp = time_t(fields[1].GetUInt64());
        m_accountData[type].data = fields[2].GetCppString();
    } while (result->NextRow());
}

void WorldSession::SetAccountData(NewAccountData::AccountDataType type, const std::string& data)
{
    time_t const currentTime = time(nullptr);
    if ((1 << type) & NewAccountData::GLOBAL_CACHE_MASK)
    {
        if (data.empty())
        {
            CharacterDatabase.PExecute("DELETE FROM `account_data` WHERE `account`=%u AND `type`=%u", GetAccountId(), uint32(type));
        }
        else
        {
            std::string escapedData = data;
            CharacterDatabase.escape_string(escapedData);
            CharacterDatabase.PExecute("REPLACE INTO `account_data` VALUES (%u, %u, %llu, '%s')", GetAccountId(), uint32(type), uint64(currentTime), escapedData.c_str());
        }
    }
    else
    {
        // _player can be nullptr and packet received after logout but m_currentPlayerGuid still store correct guid
        if (!m_currentPlayerGuid)
            return;

        if (data.empty())
        {
            CharacterDatabase.PExecute("DELETE FROM `character_account_data` WHERE `guid`=%u AND `type`=%u", m_currentPlayerGuid.GetCounter(), uint32(type));
        }
        else
        {
            std::string escapedData = data;
            CharacterDatabase.escape_string(escapedData);
            CharacterDatabase.PExecute("REPLACE INTO `character_account_data` VALUES (%u, %u, %llu, '%s')", m_currentPlayerGuid.GetCounter(), uint32(type), uint64(currentTime), escapedData.c_str());
        }
    }

    m_accountData[type].timestamp = currentTime;
    m_accountData[type].data = data;
}

void WorldSession::SendAccountDataTimes()
{
    using namespace Crypto::Hash;

    bool const isOldClient = GetGameBuild() <= CLIENT_BUILD_1_8_4;
    uint32 const dataCount = isOldClient
                ? static_cast<uint32>(OldAccountData::NUM_ACCOUNT_DATA_TYPES)
                : static_cast<uint32>(NewAccountData::NUM_ACCOUNT_DATA_TYPES);
    WorldPacket data(SMSG_ACCOUNT_DATA_MD5, dataCount * MD5::Digest::size());
    for (uint32 index = 0; index < NewAccountData::NUM_ACCOUNT_DATA_TYPES; ++index)
    {
        // Skip indexes that dont exist in old clients
        if (isOldClient)
        {
            OldAccountData::AccountDataType oldIndex = ConvertNewAccountDataToOld(index);
            if (oldIndex == OldAccountData::NUM_ACCOUNT_DATA_TYPES)
                continue;
        }

        std::string const& accountData = m_accountData[index].data;
        MD5::Digest hash = accountData.empty() ? MD5::CreateEmpty() : MD5::ComputeFrom(accountData);
        data.append(hash.data(), hash.size());
    }
    SendPacket(&data);
}

void WorldSession::LoadTutorialsData()
{
    for (uint32 & tutorial : m_tutorials)
        tutorial = 0;

    std::unique_ptr<QueryResult> result = CharacterDatabase.PQuery("SELECT `tut0`, `tut1`, `tut2`, `tut3`, `tut4`, `tut5`, `tut6`, `tut7` FROM `character_tutorial` WHERE `account` = '%u'", GetAccountId());

    if (!result)
    {
        m_tutorialState = TUTORIALDATA_NEW;
        return;
    }

    do
    {
        Field* fields = result->Fetch();

        for (int iI = 0; iI < 8; ++iI)
            m_tutorials[iI] = fields[iI].GetUInt32();
    }
    while (result->NextRow());

    m_tutorialState = TUTORIALDATA_UNCHANGED;
}

void WorldSession::SendTutorialsData()
{
    WorldPacket data(SMSG_TUTORIAL_FLAGS, 4 * 8);
    for (uint32 tutorial : m_tutorials)
        data << tutorial;
    SendPacket(&data);
}

void WorldSession::SaveTutorialsData()
{
    static SqlStatementID updTutorial ;
    static SqlStatementID insTutorial ;

    switch (m_tutorialState)
    {
        case TUTORIALDATA_CHANGED:
        {
            SqlStatement stmt = CharacterDatabase.CreateStatement(updTutorial, "UPDATE `character_tutorial` SET `tut0`=?, `tut1`=?, `tut2`=?, `tut3`=?, `tut4`=?, `tut5`=?, `tut6`=?, `tut7`=? WHERE `account` = ?");
            for (uint32 tutorial : m_tutorials)
                stmt.addUInt32(tutorial);

            stmt.addUInt32(GetAccountId());
            stmt.Execute();
        }
        break;

        case TUTORIALDATA_NEW:
        {
            SqlStatement stmt = CharacterDatabase.CreateStatement(insTutorial, "INSERT INTO `character_tutorial` (`account`, `tut0`, `tut1`, `tut2`, `tut3`, `tut4`, `tut5`, `tut6`, `tut7`) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");

            stmt.addUInt32(GetAccountId());
            for (uint32 tutorial : m_tutorials)
                stmt.addUInt32(tutorial);

            stmt.Execute();
        }
        break;
        case TUTORIALDATA_UNCHANGED:
            break;
    }

    m_tutorialState = TUTORIALDATA_UNCHANGED;
}

uint32 WorldSession::GetTutorialInt(uint32 intId) const
{
    ASSERT(intId < ACCOUNT_TUTORIALS_COUNT);
    return m_tutorials[intId];
}

void WorldSession::ExecuteOpcode(OpcodeHandlerPacketImplDetails const& opHandlerImpl, ClientPacket const& packet)
{
    // need prevent do internal far teleports in handlers because some handlers do lot steps
    // or call code that can do far teleports in some conditions unexpectedly for generic way work code
    if (_player)
        _player->SetCanDelayTeleport(true);

    //sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[%s] Recvd packet : %u/0x%x (%s)", GetUsername().c_str(), packet->GetOpcode(), packet->GetOpcode(), LookupOpcodeName(packet->GetOpcode()));
    (this->*opHandlerImpl.handler)(packet);

    if (_player)
    {
        // can be not set in fact for login opcode, but this not create porblems.
        _player->SetCanDelayTeleport(false);

        //we should execute delayed teleports only for alive(!) players
        //because we don't want player's ghost teleported from graveyard
        if (_player->IsHasDelayedTeleport())
            _player->TeleportTo(_player->m_teleportDest, _player->m_teleportOptions);
    }
}

void WorldSession::InitWarden()
{
    MANGOS_ASSERT(!m_warden);
    m_warden = sAnticheatMgr->CreateWardenFor(this, &m_sessionKey);
}

void WorldSession::InitCheatData(Player* pPlayer)
{
    if (m_cheatData)
        m_cheatData->InitNewPlayer(pPlayer);
    else
        m_cheatData = sAnticheatMgr->CreateAnticheatFor(pPlayer);
}

MovementAnticheat* WorldSession::GetCheatData()
{
    return m_cheatData ? m_cheatData : (m_cheatData = sAnticheatMgr->CreateAnticheatFor(GetPlayer()));
}

void WorldSession::ProcessAnticheatAction(char const* detector, char const* reason, uint32 cheatAction, uint32 banSeconds)
{
    char const* action = "";
    if (cheatAction & CHEAT_ACTION_MUTE_PUB_CHANS)
    {
        action = "Muted from public channels.";
        if (GetSecurity() == SEC_PLAYER)
        {
            LoginDatabase.PExecute("UPDATE `account` SET `flags` = `flags` | 0x%x WHERE `id` = %u", ACCOUNT_FLAG_MUTED_FROM_PUBLIC_CHANNELS, GetAccountId());
            SetAccountFlags(GetAccountFlags() | ACCOUNT_FLAG_MUTED_FROM_PUBLIC_CHANNELS);
        }
    }
    if (cheatAction & CHEAT_ACTION_BAN_IP_ACCOUNT)
    {
        action = "Account+IP banned.";
        if (GetSecurity() == SEC_PLAYER)
        {
            std::string _reason = std::string("CHEAT") + ": " + reason;
            sWorld.BanAccount(BAN_ACCOUNT, GetUsername(), banSeconds, _reason, detector);
            std::stringstream banIpReason;
            banIpReason << "Cf account " << GetUsername();
            sWorld.BanAccount(BAN_IP, GetRemoteAddress(), banSeconds, banIpReason.str(), detector);
        }
    }
    else if (cheatAction & CHEAT_ACTION_BAN_ACCOUNT)
    {
        action = "Banned.";
        std::string _reason = std::string("CHEAT") + ": " + reason;
        if (GetSecurity() == SEC_PLAYER)
            sWorld.BanAccount(BAN_ACCOUNT, GetUsername(), banSeconds, _reason, detector);
    }
    else if (cheatAction & CHEAT_ACTION_KICK)
    {
        action = "Kicked.";
        if (GetSecurity() == SEC_PLAYER)
            KickPlayer();
    }
    else if (cheatAction & CHEAT_ACTION_REPORT_GMS)
        action = "Announced to GMs.";
    else if (!(cheatAction & CHEAT_ACTION_LOG))
        return;

    std::string playerDesc;
    if (_player)
        playerDesc = _player->GetShortDescription();
    else
    {
        std::stringstream oss;
        oss << "<None> [" << GetUsername() << ":" << GetAccountId() << "@" << GetRemoteAddress().c_str() << "]";
        playerDesc = oss.str();
    }

    if ((cheatAction & CHEAT_ACTION_GLOBAL_ANNOUNNCE) &&
        (cheatAction >= CHEAT_ACTION_KICK))
    {
        std::stringstream oss;
        oss << "|r[|c1f40af20Announce by |cffff0000" << detector << "|r]: Player " << playerDesc << ", Cheat: " << reason << ", Penalty: " << action;
        sWorld.SendGlobalText(oss.str().c_str(), this);
    }

    if (cheatAction & CHEAT_ACTION_REPORT_GMS)
    {
        std::stringstream oss;
        oss << "Player " << playerDesc << ", Cheat: " << reason;

        if (cheatAction >= CHEAT_ACTION_KICK)
            oss << ", Penalty: " << action;

        sWorld.SendGMText(LANG_GM_ANNOUNCE_COLOR, detector, oss.str().c_str());
    }

    sLog.Player(this, LOG_ANTICHEAT, detector, LOG_LVL_MINIMAL, "[%s] Player %s, Cheat %s, Penalty: %s",
        detector, playerDesc.c_str(), reason, action);
}

bool WorldSession::HasUsedClickToMove() const
{
    if (m_warden)
        return m_warden->HasUsedClickToMove();
    return false;
}

bool WorldSession::AllowPacket(uint16 opcode)
{
    // Do not count packets that are often spamed by the client when loading a zone for example.
    switch (opcode)
    {
        case CMSG_GAMEOBJECT_QUERY:
        case CMSG_CREATURE_QUERY:
        case CMSG_QUESTGIVER_STATUS_QUERY:
        case CMSG_ITEM_QUERY_SINGLE:
        case CMSG_NAME_QUERY:
        case CMSG_PET_NAME_QUERY:
        case CMSG_GUILD_QUERY:
        case CMSG_JOIN_CHANNEL:         // Can be flooded by addons upon login
        case CMSG_AUCTION_LIST_ITEMS:   // We already handle only one per session update
        case CMSG_WHO:                  // We already handle only one per session update
            return true;
        default:
            break;
    }

    m_floodPacketsCount[FLOOD_TOTAL_PACKETS]++;

    switch (opcode)
    {
        case CMSG_CHAR_CREATE:
        case CMSG_CHAR_ENUM:
        case CMSG_CHAR_DELETE:
        case CMSG_OPEN_ITEM:
        case CMSG_PETITION_BUY:
        case CMSG_PETITION_SIGN:
        case CMSG_PETITION_QUERY:
        case MSG_PETITION_RENAME:
        case CMSG_SEND_MAIL:
        case CMSG_PLAYER_LOGIN:
        case CMSG_GMTICKET_UPDATETEXT:
            m_floodPacketsCount[FLOOD_VERY_SLOW_OPCODES]++;
        // no break, since slow packets are also very slow packets.
        case CMSG_LOGOUT_REQUEST:
        case CMSG_ADD_FRIEND:
        case CMSG_DEL_FRIEND:
        case CMSG_BUY_ITEM:
        case CMSG_SELL_ITEM:
            m_floodPacketsCount[FLOOD_SLOW_OPCODES]++;
            break;
        default:
            break;
    }

    // Check if the permitted threshold has been exceeded
    std::stringstream reason;
    if (m_floodPacketsCount[FLOOD_VERY_SLOW_OPCODES] > 2)
        reason << m_floodPacketsCount[FLOOD_VERY_SLOW_OPCODES] << " very slow packets";
    if (m_floodPacketsCount[FLOOD_SLOW_OPCODES] > 8)
        reason << m_floodPacketsCount[FLOOD_SLOW_OPCODES] << " slow packets";
    if (m_floodPacketsCount[FLOOD_TOTAL_PACKETS] > 300)
        reason << m_floodPacketsCount[FLOOD_TOTAL_PACKETS] << " packets";
    if (!reason.str().empty())
    {
        reason << " (" << LookupOpcodeName(opcode) << ")";
        ProcessAnticheatAction("AntiFlood", reason.str().c_str(), sWorld.getConfig(CONFIG_UINT32_ANTIFLOOD_SANCTION));
        return false;
    }

    return true;
}

bool WorldSession::CharacterScreenIdleKick(uint32 currTime)
{
    if (GetPlayer() || m_inQueue || PlayerLoading()) // not on the character screen
        return false;

    auto maxIdle = sWorld.getConfig(CONFIG_UINT32_CHARACTER_SCREEN_MAX_IDLE_TIME);

    if (!maxIdle) // disabled
        return false;

    if (currTime > m_idleTime && (currTime - m_idleTime) >= (maxIdle * 1000))
    {
        sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "SESSION: Kicking session [%s] from character selection", GetRemoteAddress().c_str());
        return true;
    }

    return false;
}
