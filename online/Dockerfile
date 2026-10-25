# VelohNav Ops en ligne — image du serveur (aucune dépendance npm, aucun compilateur)
# Les moteurs WebAssembly (public/vn-core.wasm, public/vn-twin.wasm) sont livrés compilés ;
# online/engine/build.sh les reconstruit depuis src/ si besoin.
#   docker build -t velohnav-ops-online online/
#   docker run -d -p 8790:8790 -v velohnav-data:/data velohnav-ops-online
FROM node:22-alpine
WORKDIR /app
COPY package.json server.mjs ./
COPY lib ./lib
COPY public ./public
ENV VN_DATA=/data VN_PORT=8790 NODE_ENV=production
RUN mkdir -p /data && chown node:node /data
VOLUME ["/data"]
EXPOSE 8790
# /healthz répond 200 tant que le dernier relevé a moins de 3 périodes
HEALTHCHECK --interval=60s --timeout=5s --start-period=60s CMD wget -qO- http://127.0.0.1:8790/healthz >/dev/null || exit 1
USER node
CMD ["node", "server.mjs"]
