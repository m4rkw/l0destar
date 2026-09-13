# The l0destar server and the MariaDB it stores everything in, in one image.
#
#   docker run -d --name l0destar --restart unless-stopped \
#       -e L0DESTAR_HOSTNAME=tracker.example.com \
#       -v /srv/l0destar:/data \
#       -p 65480:65480/udp -p 65481:65481/tcp -p 127.0.0.1:5000:5000 \
#       m4rkw/l0destar
#
# Configuration, certificates, the database, firmware and logs all live in the
# host directory mounted at /data; docker/entrypoint.sh prepares it on start.

FROM python:3.12-slim-bookworm

RUN apt-get update \
    && apt-get install -y --no-install-recommends mariadb-server openssl tini \
    && rm -rf /var/lib/apt/lists/* /var/lib/mysql

COPY --from=ghcr.io/astral-sh/uv:0.9.26 /uv /usr/local/bin/uv

WORKDIR /app
COPY requirements.txt .
RUN uv pip install --system --no-cache -r requirements.txt

COPY . .
COPY docker/mariadb.cnf /etc/mysql/mariadb.conf.d/99-l0destar.cnf

# The account the server and the database run as.  The entrypoint gives it the
# uid and gid of whoever owns /data on the host.
RUN groupadd --gid 10001 tracker \
    && useradd --uid 10001 --gid tracker --no-create-home --home-dir /nonexistent \
        --shell /usr/sbin/nologin tracker

ENV TRACKER_CONFIG=/data/config.yaml \
    TRACKER_BIND=0.0.0.0:5000 \
    PYTHONUNBUFFERED=1

EXPOSE 5000/tcp 65480/udp 65481/tcp

ENTRYPOINT ["tini", "--", "/app/docker/entrypoint.sh"]
