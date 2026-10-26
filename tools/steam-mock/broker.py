"""Fake Steam back end for one-PC tests: users, friends (everyone is everyone's friend), lobbies, P2P relay.

    python -I tools/steam-mock/broker.py [--port 27990]

Each mock_steam_api.dll connects once. Frames are [u32 length][u8 op][payload], little endian;
length counts the op byte. Requests get one reply with the same op; op 100 is pushed (a P2P packet).
"""
import argparse
import asyncio
import struct
import sys

HELLO, USERS, CREATE_LOBBY, JOIN_LOBBY, MEMBERS, LEAVE, SEND, PACKET = 1, 2, 3, 4, 5, 6, 7, 100

users = {}      # steam id -> {"name", "writer", "lobby"}
lobbies = {}    # lobby id -> {"owner", "members": [ids], "max"}
next_lobby = [0x0186000000000001]


def frame(op, payload=b""):
    return struct.pack("<IB", len(payload) + 1, op) + payload


def pstr(s):
    b = s.encode("utf-8")
    return struct.pack("<I", len(b)) + b


async def client(reader, writer):
    me = None
    try:
        while True:
            hdr = await reader.readexactly(5)
            n, op = struct.unpack("<IB", hdr)
            body = await reader.readexactly(n - 1)
            if op == HELLO:
                me = struct.unpack_from("<Q", body)[0]
                ln = struct.unpack_from("<I", body, 8)[0]
                users[me] = {"name": body[12:12 + ln].decode("utf-8"), "writer": writer, "lobby": 0}
                writer.write(frame(HELLO))
            elif op == USERS:
                others = [(i, u) for i, u in users.items() if i != me]
                out = struct.pack("<I", len(others))
                for i, u in others:
                    out += struct.pack("<QQ", i, u["lobby"]) + pstr(u["name"])
                writer.write(frame(USERS, out))
            elif op == CREATE_LOBBY:
                lid = next_lobby[0]
                next_lobby[0] += 1
                lobbies[lid] = {"owner": me, "members": [me], "max": struct.unpack_from("<I", body)[0]}
                users[me]["lobby"] = lid
                writer.write(frame(CREATE_LOBBY, struct.pack("<Q", lid)))
            elif op == JOIN_LOBBY:
                lid = struct.unpack_from("<Q", body)[0]
                lob = lobbies.get(lid)
                ok = lob is not None and len(lob["members"]) < lob["max"]
                if ok and me not in lob["members"]:
                    lob["members"].append(me)
                    users[me]["lobby"] = lid
                writer.write(frame(JOIN_LOBBY, struct.pack("<BQ", 1 if ok else 0, lob["owner"] if lob else 0)))
            elif op == MEMBERS:
                lob = lobbies.get(struct.unpack_from("<Q", body)[0])
                mem = lob["members"] if lob else []
                writer.write(frame(MEMBERS, struct.pack("<I", len(mem)) + b"".join(struct.pack("<Q", m) for m in mem)))
            elif op == LEAVE:
                leave(me, struct.unpack_from("<Q", body)[0])
                writer.write(frame(LEAVE, b"\x01"))
            elif op == SEND:
                to = struct.unpack_from("<Q", body)[0]
                target = users.get(to)
                if target:
                    target["writer"].write(frame(PACKET, struct.pack("<Q", me) + body[8:]))
            await writer.drain()
    except (asyncio.IncompleteReadError, ConnectionError):
        pass
    finally:
        if me in users:
            if users[me]["lobby"]:
                leave(me, users[me]["lobby"])
            del users[me]
        writer.close()


def leave(me, lid):
    lob = lobbies.get(lid)
    if not lob:
        return
    if me in lob["members"]:
        lob["members"].remove(me)
    if me in users:
        users[me]["lobby"] = 0
    if not lob["members"]:
        del lobbies[lid]
    elif lob["owner"] == me:
        lob["owner"] = lob["members"][0]


async def main(port):
    server = await asyncio.start_server(client, "127.0.0.1", port)
    print(f"steam mock broker on 127.0.0.1:{port}", flush=True)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=27990)
    a = ap.parse_args()
    try:
        asyncio.run(main(a.port))
    except KeyboardInterrupt:
        sys.exit(0)
