#!/usr/bin/env python3
"""Local AppRTC/collider-compatible signalling server for peerconnection_client.

This is the bundled signalling server the rest of the repo previously lacked: it
lets the WebRTC testbed run end-to-end with no external server. It speaks exactly
the protocol the extended `peerconnection_client` expects (see
examples/peerconnection/client/conductor.cc and websocket_client.cc):

  * POST /join/{room}              -> assign a client id, say whether the caller
                                      is the room initiator, hand back a ws url.
  * WebSocket /ws (subproto apprtc) -> client registers, then receives relayed
                                      peer messages wrapped as {"msg": "<json>"}.
  * POST /message/{room}/{client}  -> initiator pushes offer/candidate messages;
                                      relayed to the other peer.

Only two peers per room are expected (sender + receiver). HTTP and WebSocket are
served on a single port (default 8888) so only one port needs to be reachable.

Run:  python3 signaling_server.py --host 0.0.0.0 --port 8888
"""

import argparse
import asyncio
import itertools
import json
import logging

from aiohttp import web, WSMsgType

LOG = logging.getLogger("signaling")

_client_ids = itertools.count(1)


class Room:
    """State for a single signalling room (two peers max)."""

    def __init__(self, room_id):
        self.room_id = room_id
        # client_id -> WebSocketResponse (None until the client registers its ws)
        self.clients = {}
        # client_id -> list of inner json strings awaiting delivery over its ws
        self.queues = {}
        # messages produced before the peer existed, handed to the next joiner
        self.pending = []

    def add_client(self):
        client_id = str(next(_client_ids))
        is_initiator = len(self.clients) == 0
        self.clients[client_id] = None
        self.queues[client_id] = []
        # Second peer inherits anything the initiator sent before it arrived.
        if not is_initiator and self.pending:
            self.queues[client_id].extend(self.pending)
            self.pending.clear()
        return client_id, is_initiator

    def remove_client(self, client_id):
        self.clients.pop(client_id, None)
        self.queues.pop(client_id, None)

    def is_empty(self):
        return not self.clients


class Signaling:
    def __init__(self):
        self.rooms = {}

    def get_room(self, room_id):
        room = self.rooms.get(room_id)
        if room is None:
            room = Room(room_id)
            self.rooms[room_id] = room
        return room

    async def deliver(self, room, target_id, inner_str):
        """Send an inner message to target over its ws, or queue it."""
        ws = room.clients.get(target_id)
        if ws is not None and not ws.closed:
            await ws.send_str(json.dumps({"msg": inner_str}))
            LOG.info("relay -> %s/%s (live)", room.room_id, target_id)
        else:
            room.queues.setdefault(target_id, []).append(inner_str)
            LOG.info("relay -> %s/%s (queued)", room.room_id, target_id)

    async def relay(self, room, from_id, inner_str):
        """Forward a message from from_id to the other peer in the room."""
        others = [cid for cid in room.clients if cid != from_id]
        if others:
            for other in others:
                await self.deliver(room, other, inner_str)
        else:
            # Peer hasn't joined yet (initiator's early offer/candidates).
            room.pending.append(inner_str)
            LOG.info("relay -> %s/<pending> (no peer yet)", room.room_id)

    # ---- HTTP handlers ----------------------------------------------------

    async def handle_health(self, request):
        return web.Response(text="ok")

    async def handle_join(self, request):
        room_id = request.match_info["room"]
        try:
            await request.json()  # body carries {"room_id": ...}; we use the path
        except Exception:
            pass
        room = self.get_room(room_id)
        client_id, is_initiator = room.add_client()
        # Advertise a ws url pointing back to the address the client used to reach
        # us (request.host already includes host:port), so both the in-namespace
        # sender and the host-side receiver get a reachable url.
        ws_url = "ws://{}/ws".format(request.host)
        LOG.info("join room=%s client=%s initiator=%s -> %s",
                 room_id, client_id, is_initiator, ws_url)
        return web.json_response({
            "result": "SUCCESS",
            "params": {
                "is_initiator": "true" if is_initiator else "false",
                "wss_url": ws_url,
                "wss_post_url": ws_url,
                "client_id": client_id,
                "room_id": room_id,
                "messages": [],
            },
        })

    async def handle_message(self, request):
        room_id = request.match_info["room"]
        client_id = request.match_info["client"]
        inner_str = await request.text()
        room = self.rooms.get(room_id)
        if room is None:
            LOG.warning("message for unknown room %s", room_id)
            return web.json_response({"result": "UNKNOWN_ROOM"}, status=404)
        await self.relay(room, client_id, inner_str)
        return web.json_response({"result": "SUCCESS"})

    # ---- WebSocket handler ------------------------------------------------

    async def handle_ws(self, request):
        ws = web.WebSocketResponse(protocols=("apprtc",))
        await ws.prepare(request)
        room = None
        client_id = None
        LOG.info("ws connected")
        try:
            async for msg in ws:
                if msg.type != WSMsgType.TEXT:
                    if msg.type in (WSMsgType.ERROR, WSMsgType.CLOSE):
                        break
                    continue
                try:
                    data = json.loads(msg.data)
                except ValueError:
                    LOG.warning("ws: non-json frame: %r", msg.data[:120])
                    continue
                cmd = data.get("cmd")
                if cmd == "register":
                    room = self.get_room(data.get("roomid", ""))
                    client_id = data.get("clientid", "")
                    room.clients[client_id] = ws
                    LOG.info("ws register room=%s client=%s", room.room_id, client_id)
                    # Flush anything queued for this client.
                    queued = room.queues.get(client_id, [])
                    room.queues[client_id] = []
                    for inner_str in queued:
                        await ws.send_str(json.dumps({"msg": inner_str}))
                    if queued:
                        LOG.info("ws flushed %d queued msg(s) to %s/%s",
                                 len(queued), room.room_id, client_id)
                elif cmd == "send":
                    if room is None or client_id is None:
                        LOG.warning("ws send before register")
                        continue
                    inner_str = data.get("msg", "")
                    await self.relay(room, client_id, inner_str)
                else:
                    LOG.warning("ws: unknown cmd %r", cmd)
        finally:
            LOG.info("ws disconnected room=%s client=%s",
                     room.room_id if room else None, client_id)
            if room is not None and client_id is not None:
                room.remove_client(client_id)
                if room.is_empty():
                    self.rooms.pop(room.room_id, None)
        return ws


def build_app():
    sig = Signaling()
    app = web.Application()
    app.router.add_get("/", sig.handle_health)
    app.router.add_get("/healthz", sig.handle_health)
    app.router.add_post("/join/{room}", sig.handle_join)
    app.router.add_post("/message/{room}/{client}", sig.handle_message)
    app.router.add_get("/ws", sig.handle_ws)
    return app


def main():
    parser = argparse.ArgumentParser(description="Local WebRTC signalling server")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8888)
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s [signaling] %(message)s",
    )
    LOG.info("starting on %s:%d", args.host, args.port)
    # Short shutdown timeout: experiment clients are killed with SIGKILL, leaving
    # half-open sockets; don't wait the default 60s for them on exit.
    web.run_app(build_app(), host=args.host, port=args.port,
                shutdown_timeout=1.0, print=None)


if __name__ == "__main__":
    main()
