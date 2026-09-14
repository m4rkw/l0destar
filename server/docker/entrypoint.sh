#!/bin/bash
# Entrypoint of the l0destar server image: prepares /data, then runs the
# database and the server side by side until either of them stops.
#
# Everything that has to outlive the container is in /data, a directory
# mounted from the host:
#
#   config.yaml   written on first start; edit it, then restart the container
#   certs/        the CA, and the certificate trackers download updates over
#   mysql/        the database
#   fw/           firmware images and manifests, published from the build machine
#   logs/         the server's log files
#
# Both processes run as the owner of /data, so its files can be edited from the
# host without root - or as uid 10001, if root owns it.

set -euo pipefail

DATA=/data
CONFIG=$DATA/config.yaml
CERTS=$DATA/certs

log() {
    echo "l0destar: $*"
}

die() {
    echo "l0destar: $*" >&2
    exit 1
}

# However the script ends, stop the server before the database, so that
# neither is killed part way through a write.
stop() {
    local status=$?
    trap - EXIT TERM INT
    for pid in ${app_pid:-} ${db_pid:-}; do
        if kill -0 "$pid" 2>/dev/null; then
            kill -TERM "$pid" 2>/dev/null || true
            wait "$pid" || true
        fi
    done
    exit "$status"
}
trap stop EXIT
trap 'exit 0' TERM INT

sql() {
    mariadb --batch --skip-column-names "$@"
}

mountpoint -q "$DATA" || die "mount a directory from the host at $DATA, for example" \
    "-v /srv/l0destar:$DATA: the database and everything else the server keeps is stored there"

# -- the account ---------------------------------------------------------------

uid=$(stat -c %u "$DATA")
gid=$(stat -c %g "$DATA")
if [ "$uid" = 0 ]; then
    uid=10001
    gid=10001
fi
if [ "$(id -u tracker)" != "$uid" ] || [ "$(id -g tracker)" != "$gid" ]; then
    groupmod --non-unique --gid "$gid" tracker
    usermod --non-unique --uid "$uid" --gid "$gid" tracker
fi

install -d -o tracker -g tracker -m 700 "$DATA/mysql"
install -d -o tracker -g tracker -m 750 "$CERTS" "$DATA/logs"
# Firmware is published into fw/ from the host, so it is only created here.
[ -d "$DATA/fw" ] || install -d -o tracker -g tracker -m 755 "$DATA/fw"

# The owner of /data may have changed since these were created.
for dir in "$DATA/mysql" "$CERTS" "$DATA/logs"; do
    if [ -n "$(find "$dir" ! -user tracker -print -quit)" ]; then
        chown -R tracker:tracker "$dir"
    fi
done
# config.yaml keeps the owner it was written with, and is mode 600, so it has to
# follow the owner of /data too or the server cannot read it.
if [ -e "$CONFIG" ] && [ "$(stat -c %u:%g "$CONFIG")" != "$uid:$gid" ]; then
    chown tracker:tracker "$CONFIG"
fi

# -- certificates --------------------------------------------------------------

if [ ! -e "$CERTS/server.crt" ] && [ ! -e "$CERTS/server.key" ] && [ ! -e "$CERTS/ca.crt" ]; then
    [ -n "${L0DESTAR_HOSTNAME:-}" ] || die "set L0DESTAR_HOSTNAME to the name trackers" \
        "will reach this server at, for example -e L0DESTAR_HOSTNAME=tracker.example.com:" \
        "the certificate they download updates over is issued for it"
    log "creating a CA and a certificate for $L0DESTAR_HOSTNAME in $CERTS"
    if ! output=$(bash /app/certs/gen_certs.sh "$L0DESTAR_HOSTNAME" "$CERTS" 2>&1); then
        echo "$output" >&2
        die "could not create the certificates"
    fi
    chown tracker:tracker "$CERTS"/*
    chmod 600 "$CERTS"/*.key
    chmod 644 "$CERTS"/*.crt "$CERTS"/ca_cert.h
elif [ ! -e "$CERTS/server.crt" ] || [ ! -e "$CERTS/server.key" ]; then
    die "$CERTS needs both server.crt and server.key"
fi

# -- configuration -------------------------------------------------------------

if [ ! -e "$CONFIG" ]; then
    log "writing $CONFIG"
    session_secret=$(python3 -c 'import secrets; print(secrets.token_urlsafe(48))')
    db_password=$(python3 -c 'import secrets; print(secrets.token_urlsafe(24))')
    install -o tracker -g tracker -m 600 /dev/null "$CONFIG"
    cat > "$CONFIG" <<EOF
# l0destar server configuration, written on the container's first start.
# Restart the container after changing it:  docker restart l0destar
#
# Anything not set here keeps its default.  Every setting is described in
# config.yaml.example:  docker exec l0destar cat config.yaml.example

# Signs login sessions.  Changing it logs everyone out.
session_secret: $session_secret

# The map needs a Google Maps JavaScript API key; without one the map page
# shows a note instead.
google_maps_api_key: ''

# Where alerts go: none, pushover or webhook.
notify:
  backend: none

# The database inside the container.  The password is set on the database
# account at every start, so it can be changed here.
database:
  host: 127.0.0.1
  port: 3306
  database: tracker
  user: tracker
  password: $db_password

# Paths inside the container: /data is the directory mounted from the host.
log_dir: /data/logs
fw_dir: /data/fw
tls_cert: /data/certs/server.crt
tls_key: /data/certs/server.key
EOF
fi

# -- database ------------------------------------------------------------------

install -d -o tracker -g tracker -m 755 /run/mysqld
new_database=0
if [ ! -d "$DATA/mysql/mysql" ]; then
    log "creating the database in $DATA/mysql"
    mariadb-install-db --user=tracker --skip-test-db > /dev/null
    new_database=1
fi

mariadbd --user=tracker &
db_pid=$!
for _ in $(seq 60); do
    mariadb-admin ping --silent > /dev/null 2>&1 && break
    kill -0 "$db_pid" 2>/dev/null || die "the database did not start; its output is above"
    sleep 1
done
mariadb-admin ping --silent > /dev/null 2>&1 || die "the database did not start within a minute"
# Brings the system tables up to date after a MariaDB upgrade; a no-op otherwise.
mariadb-upgrade --silent > /dev/null

if [ "$new_database" = 1 ]; then
    # mariadb-install-db also makes the account the database runs as into a
    # superuser over the socket, with a proxy grant naming this container's
    # hostname.  Nothing needs either: the server connects as its own account,
    # which has rights to the data and nothing else.
    sql -e "DROP USER IF EXISTS 'tracker'@'localhost';
            DELETE FROM mysql.proxies_priv WHERE User = 'tracker';
            FLUSH PRIVILEGES"
fi

# The server's account, with the password config.yaml has for it.
password=$(python3 -c 'import sys, yaml; print(yaml.safe_load(open(sys.argv[1]))["database"]["password"])' "$CONFIG")
password=${password//\\/\\\\}
password=${password//\'/\\\'}
sql <<EOF
CREATE DATABASE IF NOT EXISTS tracker CHARACTER SET utf8mb4;
CREATE USER IF NOT EXISTS 'tracker'@'127.0.0.1';
ALTER USER 'tracker'@'127.0.0.1' IDENTIFIED BY '$password';
GRANT SELECT, INSERT, UPDATE, DELETE ON tracker.* TO 'tracker'@'127.0.0.1';
EOF

# A new database gets schema.sql, which already includes every migration.  An
# existing one gets the migrations it has not had yet, recorded in
# schema_migration as they are applied.
tables=$(sql -e "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = 'tracker'")
ledger=$(sql -e "SELECT COUNT(*) FROM information_schema.tables
                 WHERE table_schema = 'tracker' AND table_name = 'schema_migration'")
if [ "$tables" = 0 ]; then
    log "loading the schema"
    sql tracker < /app/schema.sql
    apply=0
elif [ "$ledger" = 0 ]; then
    # A database from before the ledger existed - restored from a dump, say.  It
    # may have some of the migrations or none, so each is applied and errors
    # that only say its change is already there are accepted.
    log "the database has no record of its migrations; applying any it lacks"
    apply=tolerant
else
    apply=1
fi
sql tracker -e "CREATE TABLE IF NOT EXISTS schema_migration (
    name VARCHAR(191) NOT NULL PRIMARY KEY,
    applied_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP
) COMMENT 'files from migrations/ this database has had, kept by the Docker entrypoint'"
for path in /app/migrations/*.sql; do
    name=$(basename "$path")
    if [ "$(sql tracker -e "SELECT COUNT(*) FROM schema_migration WHERE name = '$name'")" != 0 ]; then
        continue
    fi
    if [ "$apply" = 1 ]; then
        log "applying migration $name"
        sql tracker < "$path"
    elif [ "$apply" = tolerant ]; then
        log "applying migration $name where it is missing"
        # --force carries on past each error.  Accepted: 1050 table exists,
        # 1060 duplicate column, 1061 duplicate key, 1091 nothing to drop.
        errors=$(sql --force tracker < "$path" 2>&1 >/dev/null) || true
        unexpected=$(printf '%s\n' "$errors" | grep '^ERROR' | grep -Ev '^ERROR (1050|1060|1061|1091) ' || true)
        [ -z "$unexpected" ] || die "migration $name failed: $unexpected"
    fi
    sql tracker -e "INSERT INTO schema_migration (name) VALUES ('$name')"
done

# -- server --------------------------------------------------------------------

cd /app
setpriv --reuid=tracker --regid=tracker --clear-groups \
    gunicorn -c gunicorn.conf.py wsgi:app &
app_pid=$!

# Whichever stops first takes the container down with it, and Docker's restart
# policy starts both again.
wait -n "$app_pid" "$db_pid" || true
if kill -0 "$db_pid" 2>/dev/null; then
    die "the server stopped"
fi
die "the database stopped"
