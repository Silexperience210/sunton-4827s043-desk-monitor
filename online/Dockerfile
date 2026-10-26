# VelohNav Ops en ligne — image du serveur, construite depuis la RACINE du dépôt (le banc a besoin de src/)
#   cd online && docker compose up -d                                   (docker-compose.yml règle le contexte)
#   docker build -f online/Dockerfile -t velohnav-ops-online .          (depuis la racine du dépôt)
#   docker run -d -p 8790:8790 -v velohnav-data:/data velohnav-ops-online
#
# Étape 1 : le banc natif sim/bench_prevision (même cœur src/core que la carte), lancé chaque nuit sur
# l'archive pour publier l'erreur des prévisions. Les moteurs WebAssembly (public/*.wasm) sont livrés
# compilés ; online/engine/build.sh les reconstruit depuis src/ si besoin.
FROM alpine:3.20 AS banc
RUN apk add --no-cache g++ make git
WORKDIR /src
COPY include ./include
COPY src ./src
COPY sim ./sim
RUN git clone -q -b v7.4.2 --depth 1 https://github.com/bblanchon/ArduinoJson.git .deps/ArduinoJson && mkdir -p .deps/lvgl/src \
 && make -C sim -j"$(nproc)" bench_prevision \
 && strip sim/bench_prevision && ./sim/bench_prevision --jours 2 --court >/dev/null

# Étape 2 : le serveur (Node, aucune dépendance npm)
FROM node:22-alpine
RUN apk add --no-cache libstdc++
WORKDIR /app
COPY online/package.json online/server.mjs ./
COPY online/lib ./lib
COPY online/public ./public
# le banc lit docs/donnees (capacités de secours) relativement à son dossier de travail : ../docs/donnees
COPY --from=banc /src/sim/bench_prevision /opt/banc/sim/bench_prevision
COPY docs/donnees /opt/banc/docs/donnees
ENV VN_DATA=/data VN_PORT=8790 NODE_ENV=production VN_BENCH_BIN=/opt/banc/sim/bench_prevision VN_BENCH_CWD=/opt/banc/sim
RUN mkdir -p /data && chown node:node /data
VOLUME ["/data"]
EXPOSE 8790
# /healthz répond 200 tant que le dernier relevé a moins de 3 périodes
HEALTHCHECK --interval=60s --timeout=5s --start-period=60s CMD wget -qO- http://127.0.0.1:8790/healthz >/dev/null || exit 1
USER node
CMD ["node", "server.mjs"]
