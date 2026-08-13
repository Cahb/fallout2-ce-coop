# Dedicated server with Docker Compose

The final `fo2-dedicated-server` image contains only a statically linked server
binary. It does not contain Fallout 2 assets, a shell, SDL, or an operating-system
userspace. The licensed game installation is bind-mounted at runtime.

## Start

```sh
cp .env.example .env
```

Edit `.env` and set `F2_GAME_PATH` to the absolute path of the Fallout 2
installation. It must contain at least `master.dat`, `critter.dat`, `patch000.dat`,
and `data`. On Linux, also set `F2_UID` and `F2_GID` to the output of `id -u` and
`id -g` so the server can write its saves.

```sh
docker compose up --build -d
docker compose logs -f fo2-dedicated-server
```

Players connect to the Docker host on TCP port 9200. The plain-text admin channel
is TCP 9201 and is bound to `127.0.0.1` by default because it has no authorization
layer:

```sh
printf 'status\n' | nc -q1 127.0.0.1 9201
```

Set `F2_SERVER_CMD_BIND=0.0.0.0` only when a firewall or private network protects
the port.

### Client launch commands

Linux (run from the Fallout 2 installation directory):

```sh
env F2_CLIENT_CONNECT=SERVER_IP:9200 \
    F2_PLAYER_NAME=Player F2_PLAYER_CREATE=ask F2_WINDOWED=1 \
    /path/to/fallout2-ce
```

Windows PowerShell (with `fallout2-ce.exe` in the Fallout 2 directory):

```powershell
Set-Location 'C:\Games\Fallout2'
$env:F2_CLIENT_CONNECT = 'SERVER_IP:9200'
$env:F2_PLAYER_NAME = 'Player'
$env:F2_PLAYER_CREATE = 'ask'
$env:F2_WINDOWED = '1'
.\fallout2-ce.exe
```

`F2_PLAYER_CREATE=ask` is only acted on the first time the server sees that
player name, so the same command can be reused for later connections.

## Persistence

The game directory is mounted read-write at `/game`, which is also the process
working directory required by the engine. Server saves and transient map state
therefore survive image replacement and container recreation in the host's
`data/SAVEGAME` and `data/MAPS` directories. The image itself remains immutable.

The default autosave interval is five minutes. Before a planned stop, request a
save over the admin channel and wait for the success reply, then stop the service:

```sh
printf 'save 1 before container stop\n' | nc -q1 127.0.0.1 9201
docker compose down
```

To restore a save, replace `F2_SERVER_MAP=...` in `.env` with
`F2_SERVER_LOAD=<slot>`. To use the lobby, omit both variables and select a world
through the admin channel with `saves`, `load <slot>`, or `new <map.map>`.

Compose defaults both authoritative difficulty settings to their hardest values:
`F2_GAME_DIFFICULTY=2` (Hard) and `F2_COMBAT_DIFFICULTY=2` (Rough). Their full
range is `0` Easy/Wimpy, `1` Normal, `2` Hard/Rough. These overrides are reapplied
after loading a save; client Preferences do not change server difficulty.

## Build, export, and publish

```sh
docker compose build fo2-dedicated-server
docker image save fo2-dedicated-server:latest | gzip > fo2-dedicated-server.tar.gz
docker tag fo2-dedicated-server:latest USER/fo2-dedicated-server:latest
docker push USER/fo2-dedicated-server:latest
```

Only the small final stage is tagged or exported; the Alpine compiler stage remains
in the local build cache.
